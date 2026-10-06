/* hde-powersave.c — battery saver and low-battery warnings (see hde-powersave.h) */
#include "hde-powersave.h"
#include "hde-applets.h"
#include "hde-panel-config.h"
#include "hde-profiles.h"
#include "hde-run.h"
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>

#define DIM_PERCENT 70          /* the battery saver dims the screen to 70 % of its brightness */

static gboolean debug_on, inited;
#define DBG(...) do { if (debug_on) { g_printerr("hde-panel: " __VA_ARGS__); g_printerr("\n"); } } while (0)

static struct {
    gboolean enabled, dim, warnings;
    int level;
    gboolean active;
    char prev_profile[32];      /* the power mode before the battery saver ("" = it did not change it) */
    int prev_bright, our_bright;    /* the brightness before / the one it set (-1 = it did not change it) */
    int warned;                 /* the last low-battery warning given (hde_power_warning_due) */
    guint32 warn_id, saver_id;  /* notification ids, to replace / withdraw them */
    gboolean have_reading;
    HdePower last;
} ps = { .level = 20, .prev_bright = -1, .our_bright = -1 };

/* ---------------------------------------------------------------- the state, kept for a restarted panel */
static char *state_path(void)
{
    return g_build_filename(g_get_user_runtime_dir(), "hde", "battery-saver.ini", NULL);
}

static void state_save(void)
{
    char *path = state_path();
    if (!ps.active) {
        g_unlink(path);
        g_free(path);
        return;
    }
    char *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0700);
    GKeyFile *kf = g_key_file_new();
    g_key_file_set_string(kf, "battery-saver", "prev_profile", ps.prev_profile);
    g_key_file_set_integer(kf, "battery-saver", "prev_brightness", ps.prev_bright);
    g_key_file_set_integer(kf, "battery-saver", "our_brightness", ps.our_bright);
    g_key_file_save_to_file(kf, path, NULL);
    g_key_file_free(kf);
    g_free(dir);
    g_free(path);
}

static void state_load(void)
{
    char *path = state_path();
    GKeyFile *kf = g_key_file_new();
    if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
        ps.active = TRUE;
        char *prof = g_key_file_get_string(kf, "battery-saver", "prev_profile", NULL);
        g_strlcpy(ps.prev_profile, prof ? prof : "", sizeof ps.prev_profile);
        g_free(prof);
        ps.prev_bright = g_key_file_get_integer(kf, "battery-saver", "prev_brightness", NULL);
        ps.our_bright = g_key_file_get_integer(kf, "battery-saver", "our_brightness", NULL);
        if (ps.prev_bright <= 0 || ps.our_bright <= 0) ps.prev_bright = ps.our_bright = -1;
        DBG("battery saver: still on from before the panel restarted (power mode before: %s, brightness before: %d%%)",
            ps.prev_profile[0] ? ps.prev_profile : "unchanged", ps.prev_bright);
    }
    g_key_file_free(kf);
    g_free(path);
}

/* ---------------------------------------------------------------- notifications */
static GDBusConnection *session_bus(void)
{
    static GDBusConnection *bus;
    static gboolean tried;
    if (!tried) {
        tried = TRUE;
        bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    }
    return bus;
}

static void on_notified(GObject *src, GAsyncResult *res, gpointer d)
{
    guint32 *id = d;
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, NULL);
    if (r) {
        g_variant_get(r, "(u)", id);
        g_variant_unref(r);
    }
}

/* urgency: 1 normal, 2 critical (stays until closed) */
static void notify(guint32 *id, const char *icon, const char *summary, const char *body, guchar urgency)
{
    GDBusConnection *bus = session_bus();
    DBG("power: notification: %s — %s", summary, body);
    if (!bus) return;
    GVariantBuilder actions, hints;
    g_variant_builder_init(&actions, G_VARIANT_TYPE("as"));
    g_variant_builder_init(&hints, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&hints, "{sv}", "urgency", g_variant_new_byte(urgency));
    g_variant_builder_add(&hints, "{sv}", "category", g_variant_new_string("device"));
    g_variant_builder_add(&hints, "{sv}", "desktop-entry", g_variant_new_string("hyggshi-settings"));
    g_dbus_connection_call(bus, "org.freedesktop.Notifications", "/org/freedesktop/Notifications",
                           "org.freedesktop.Notifications", "Notify",
                           g_variant_new("(susssasa{sv}i)", "Power", *id, icon, summary, body, &actions, &hints,
                                         urgency >= 2 ? 0 : -1),
                           G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 5000, NULL, on_notified, id);
}

static void notify_close(guint32 *id)
{
    GDBusConnection *bus = session_bus();
    if (!*id || !bus) return;
    g_dbus_connection_call(bus, "org.freedesktop.Notifications", "/org/freedesktop/Notifications",
                           "org.freedesktop.Notifications", "CloseNotification", g_variant_new("(u)", *id), NULL,
                           G_DBUS_CALL_FLAGS_NONE, 5000, NULL, NULL, NULL);
    *id = 0;
}

static char *left_text(const HdePower *p)
{
    if (p->minutes < 0) return g_strdup_printf("%d%% left", p->percent);
    char *t = hde_power_time_text(p->minutes);
    char *s = g_strdup_printf("%d%% left (about %s)", p->percent, t);
    g_free(t);
    return s;
}

static void warn(const HdePower *p, int level)
{
    char *left = left_text(p);
    char *body;
    if (level >= 2) {
        body = g_strdup_printf("%s. Plug in the computer now, or save your work: it turns off when the battery is empty.",
                               left);
        notify(&ps.warn_id, "battery-empty-symbolic", "Battery critically low", body, 2);
    } else {
        body = g_strdup_printf("%s. Plug in the computer soon.", left);
        notify(&ps.warn_id, "battery-caution-symbolic", "Battery low", body, 1);
    }
    g_printerr("hde-panel: power: %s, %s\n", level >= 2 ? "battery critically low" : "battery low", left);
    g_free(body);
    g_free(left);
}

/* ---------------------------------------------------------------- brightness through hde-settings --brightness */
static int first_percent(const char *s)
{
    for (const char *p = s ? strchr(s, '%') : NULL; p; p = strchr(p + 1, '%')) {
        const char *q = p;
        while (q > s && g_ascii_isdigit(q[-1])) q--;
        if (q < p) return atoi(q);
    }
    return -1;
}

static void set_brightness(int v)
{
    char num[16];
    g_snprintf(num, sizeof num, "%d", v);
    const char *args[] = { "--brightness", num, NULL };
    hde_run_settings(args, 10, NULL, NULL);
}

static void on_bright_for_dim(gboolean ok, const char *out, const char *err, gpointer d)
{
    (void)ok; (void)err; (void)d;
    int pct = first_percent(out);
    if (!ps.active || !ps.dim || ps.our_bright >= 0) return;
    if (!out || g_str_has_prefix(out, "none") || pct < 0) {
        DBG("battery saver: the brightness cannot be changed here");
        return;
    }
    int min = g_str_has_prefix(out, "software") ? 10 : 5;
    int target = MAX(pct * DIM_PERCENT / 100, min);
    if (target >= pct) {
        DBG("battery saver: the screen is already dim (%d%%)", pct);
        return;
    }
    ps.prev_bright = pct;
    ps.our_bright = target;
    state_save();
    set_brightness(target);
    DBG("battery saver: brightness %d%% -> %d%%", pct, target);
}

static void on_bright_for_restore(gboolean ok, const char *out, const char *err, gpointer d)
{
    (void)ok; (void)err;
    int packed = GPOINTER_TO_INT(d), prev = packed / 1000, ours = packed % 1000;
    int pct = first_percent(out);
    if (pct >= 0 && abs(pct - ours) <= 2) {
        set_brightness(prev);
        DBG("battery saver: brightness back to %d%%", prev);
    } else {
        DBG("battery saver: brightness left at %d%% (changed meanwhile; %d%% before the battery saver)", pct, prev);
    }
}

static void restore_brightness(void)
{
    if (ps.our_bright < 0 || ps.prev_bright < 0) return;
    int packed = ps.prev_bright * 1000 + ps.our_bright;
    ps.prev_bright = ps.our_bright = -1;
    const char *args[] = { "--brightness", NULL };
    hde_run_settings(args, 8, on_bright_for_restore, GINT_TO_POINTER(packed));
}

static void dim_now(void)
{
    const char *args[] = { "--brightness", NULL };
    hde_run_settings(args, 8, on_bright_for_dim, NULL);
}

/* ---------------------------------------------------------------- the power mode */
static void on_profiles_for_saver(const char *active, const char *const *list, gpointer d)
{
    (void)d;
    if (!ps.active || ps.prev_profile[0]) return;
    if (!active) {
        DBG("battery saver: no power-profiles-daemon: the power mode stays");
        return;
    }
    if (!strcmp(active, "power-saver")) {
        DBG("battery saver: already in the Power Saver mode");
        return;
    }
    if (!list || !g_strv_contains(list, "power-saver")) return;
    g_strlcpy(ps.prev_profile, active, sizeof ps.prev_profile);
    state_save();
    hde_power_profiles_set("power-saver", NULL, NULL);
    DBG("battery saver: power mode %s -> power-saver", active);
}

static void on_profiles_for_restore(const char *active, const char *const *list, gpointer d)
{
    (void)list;
    char *prev = d;
    if (active && !strcmp(active, "power-saver")) {
        hde_power_profiles_set(prev, NULL, NULL);
        DBG("battery saver: power mode back to %s", prev);
    } else if (active) {
        DBG("battery saver: power mode left at %s (changed meanwhile)", active);
    }
    g_free(prev);
}

/* ---------------------------------------------------------------- on / off */
static void saver_on(const HdePower *p)
{
    ps.active = TRUE;
    ps.prev_profile[0] = '\0';
    ps.prev_bright = ps.our_bright = -1;
    state_save();
    g_printerr("hde-panel: battery saver: on (%d%% on battery, %s)\n", p->percent,
               ps.level >= 100 ? "always on battery" : "turns on at the level chosen in Settings");
    DBG("battery saver: level %d%%, dim %s", ps.level, ps.dim ? "yes" : "no");
    hde_power_profiles_get(on_profiles_for_saver, NULL);
    if (ps.dim) dim_now();
    hde_applets_set_slow(TRUE);
    char *left = left_text(p);
    char *body = g_strdup_printf("%s. The Power Saver mode%s make the battery last longer. It turns off when you plug in "
                                 "the computer.", left, ps.dim ? " and a dimmer screen" : "");
    char *icon = hde_power_icon_name(p->percent, p->state);
    notify(&ps.saver_id, icon, "Battery saver is on", body, 1);
    g_free(icon);
    g_free(body);
    g_free(left);
}

static void saver_off(const char *why)
{
    ps.active = FALSE;
    g_printerr("hde-panel: battery saver: off (%s)\n", why);
    if (ps.prev_profile[0]) {
        hde_power_profiles_get(on_profiles_for_restore, g_strdup(ps.prev_profile));
        ps.prev_profile[0] = '\0';
    }
    restore_brightness();
    hde_applets_set_slow(FALSE);
    notify_close(&ps.saver_id);
    state_save();
}

static void decide(void)
{
    if (!ps.have_reading) return;
    const HdePower *p = &ps.last;
    gboolean want = hde_power_saver_wanted(p, ps.enabled, ps.level, ps.active);
    if (want && !ps.active) saver_on(p);
    else if (!want && ps.active)
        saver_off(!ps.enabled ? "turned off in Settings" : !hde_power_on_battery(p) ? "plugged in"
                  : "charged above the level chosen in Settings");
}

/* ---------------------------------------------------------------- public */
void hde_powersave_reload(void)
{
    if (!inited) return;
    gboolean was_dim = ps.dim;
    ps.enabled = hde_cfg_get_bool("battery_saver", FALSE);
    ps.dim = hde_cfg_get_bool("battery_saver_dim", TRUE);
    ps.warnings = hde_cfg_get_bool("battery_warnings", TRUE);
    ps.level = CLAMP(hde_cfg_get_int("battery_saver_level", 20), 5, 100);
    if (ps.active && was_dim != ps.dim) {
        if (!ps.dim) restore_brightness();
        else if (ps.our_bright < 0) dim_now();
    }
    decide();
}

void hde_powersave_init(gboolean debug)
{
    debug_on = debug;
    inited = TRUE;
    state_load();
    if (ps.active) hde_applets_set_slow(TRUE);
    ps.dim = hde_cfg_get_bool("battery_saver_dim", TRUE);
    hde_powersave_reload();
}

void hde_powersave_update(const HdePower *p)
{
    if (!inited) return;
    ps.last = *p;
    ps.have_reading = TRUE;
    if (p->percent < 0) {
        if (ps.active) saver_off("no battery");
        return;
    }
    int due = hde_power_warning_due(p, &ps.warned);
    if (due && ps.warnings) warn(p, due);
    if (!hde_power_on_battery(p) && ps.warn_id) notify_close(&ps.warn_id);     /* plugged in: the warning is over */
    decide();
}

gboolean hde_powersave_active(void) { return ps.active; }
