/* hde-idle — how long has nobody touched the computer, and what happens then.
 *
 * This is the part of HDE that used to be left to a power manager of another desktop: after a while without
 * anybody using the computer the screen goes off, then the session locks, then the computer sleeps. It runs in
 * both of HDE's sessions — on "HDE" (X11) it asks the XScreenSaver extension how long the keyboard and mouse have
 * been quiet and turns the screen off through DPMS; on "HDE (Wayland)" it asks the compositor through
 * ext-idle-notify-v1 (labwc and NexWM both offer it).
 *
 *   hde-idle               watch the session and do what Settings says (this is what hde-session runs)
 *   hde-idle --check       0 when the idle time of this session can be measured at all
 *   hde-idle --status      what it is doing and what is due when
 *   hde-idle --lock | --suspend | --blank | --unblank | --activate
 *                          do one of them now (the same keys the panel and the hotkeys use)
 *
 * Programs that are showing something can hold it back over D-Bus — the two interfaces every desktop speaks, so
 * that the programs of GNOME, of KDE and of the web all work:
 *   org.freedesktop.ScreenSaver.Inhibit          keeps the screen on (no blanking, no locking)
 *   org.freedesktop.PowerManagement.Inhibit      keeps the computer awake (no sleeping)
 * A film being watched asks for the first, a download for the second, and a film that is still downloading for
 * both. An inhibitor whose program crashes is dropped when it disappears from the bus, so a dead program cannot
 * keep a laptop awake all night.
 *
 * Which deadline has passed and what to do about it is decided in src/hde-idle-core.c (plain C, tested without a
 * display). This file is the halves that need a screen and a bus: measuring the idle time, carrying out what was
 * decided, and answering the D-Bus interfaces.
 */
#include "hde-build.h"
#include "hde-commands.h"
#include "hde-idle-core.h"
#include "hde-power.h"

#include <gio/gio.h>
#include <glib.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_XSS
#include <X11/Xlib.h>
#include <X11/extensions/dpms.h>
#include <X11/extensions/scrnsaver.h>
#endif

#ifdef HAVE_WAYLAND_IDLE
#include <glib-unix.h>
#include <wayland-client.h>
#include "ext-idle-notify-v1-client-protocol.h"
#endif

/* Below this the user is taken to be at the computer: X11 and Wayland both say "no input for a moment" for all
 * sorts of reasons (a pointer that was not moved while a menu was open) and this is not a screensaver. */
#define HDE_IDLE_ACTIVE_MS 1500
/* How often the clock is looked at. A second: the screen goes off within a second of the time Settings says. */
#define HDE_IDLE_TICK_MS   1000
/* How often the battery is asked whether the adapter is still plugged in. */
#define HDE_IDLE_POWER_MS  15000

static HdeIdleConfig   cfg;
static HdeIdleState    state;
static HdeIdleInhibitors inhibitors;
static GHashTable     *inhibitor_senders;   /* inhibitor cookie -> the D-Bus name that asked for it */
static gint64          activity_override;   /* monotonic us: SimulateUserActivity() counts as touching the mouse */
static gint64          power_checked_at;
static int             on_battery_cached;
/* FALSE after the computer has been sent to sleep: the session does not sleep a second time until somebody has
 * touched the computer again, so that a machine that wakes up for a moment cannot fall asleep in a loop. */
static gboolean        armed = TRUE;
static gboolean        verbose;
static GMainLoop      *loop;

static void log_line(const char *fmt, ...) G_GNUC_PRINTF(1, 2);

/* Used before it is defined: the settings watcher, the wayland re-arming and the "the sleep did not take"
 * timer all point at things further down. */
static void      settings_monitor_changed(GFileMonitor *monitor, GFile *file, GFile *other,
                                          GFileMonitorEvent event, gpointer data);
static gboolean  settings_changed_timeout(gpointer data);
static gboolean  re_arm(gpointer data);
#ifdef HAVE_WAYLAND_IDLE
static void      wl_arm_next_deadline(void);
#endif

static void log_line(const char *fmt, ...)
{
    if (!verbose) return;
    va_list ap;
    va_start(ap, fmt);
    char *msg = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    g_printerr("hde-idle: %s\n", msg);
    g_free(msg);
}

/* ---------------------------------------------------------------- the settings */

static int cfg_int(GKeyFile *kf, const char *key, int fallback)
{
    GError *err = NULL;
    int v = g_key_file_get_integer(kf, "settings", key, &err);
    if (err) { g_clear_error(&err); return fallback; }
    return v;
}

static void read_settings(void)
{
    HdeIdleConfig old = cfg;
    hde_idle_config_defaults(&cfg);
    char *path = g_build_filename(g_get_user_config_dir(), "hde", "settings.ini", NULL);
    GKeyFile *kf = g_key_file_new();
    if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
        cfg.blank_minutes = cfg_int(kf, "idle_blank", cfg.blank_minutes);
        cfg.lock_minutes = cfg_int(kf, "idle_lock", cfg.lock_minutes);
        cfg.suspend_ac_minutes = cfg_int(kf, "idle_suspend_ac", cfg.suspend_ac_minutes);
        cfg.suspend_bat_minutes = cfg_int(kf, "idle_suspend_battery", cfg.suspend_bat_minutes);
        cfg.lock_before_suspend = g_key_file_get_boolean(kf, "settings", "idle_lock_before_suspend", NULL) != FALSE;
    }
    g_key_file_free(kf);
    g_free(path);
    hde_idle_config_clamp(&cfg);
    if (hde_idle_config_equal(&old, &cfg)) return;

    char text[256];
    hde_idle_config_text(&cfg, text, sizeof text);
    log_line("settings: %s", text);
#ifdef HAVE_WAYLAND_IDLE
    wl_arm_next_deadline();
#endif
}

static gboolean settings_changed_timeout(gpointer data)
{
    (void)data;
    read_settings();
    return G_SOURCE_REMOVE;
}

static void settings_monitor_changed(GFileMonitor *monitor, GFile *file, GFile *other,
                                     GFileMonitorEvent event, gpointer data)
{
    (void)monitor; (void)file; (void)other; (void)data;
    if (event == G_FILE_MONITOR_EVENT_CHANGED || event == G_FILE_MONITOR_EVENT_CREATED)
        g_timeout_add(300, settings_changed_timeout, NULL);
}

/* ---------------------------------------------------------------- how idle is the session? */

#ifdef HAVE_XSS
static Display *x_display;
static int      xss_available, dpms_available;

static void x11_open(void)
{
    x_display = XOpenDisplay(NULL);
    if (!x_display) { log_line("no X display: the idle time cannot be measured"); return; }
    int ev = 0, er = 0;
    xss_available = XScreenSaverQueryExtension(x_display, &ev, &er) ? 1 : 0;
    dpms_available = DPMSQueryExtension(x_display, &ev, &er) ? 1 : 0;
    if (!xss_available) log_line("the X server has no XScreenSaver extension: the idle time cannot be measured");
}

static gint64 x11_idle_ms(void)
{
    if (!x_display || !xss_available) return -1;
    XScreenSaverInfo *info = XScreenSaverAllocInfo();
    if (!info) return -1;
    gint64 idle = -1;
    if (XScreenSaverQueryInfo(x_display, DefaultRootWindow(x_display), info)) idle = (gint64)info->idle;
    XFree(info);
    return idle;
}
#endif

#ifdef HAVE_WAYLAND_IDLE
static struct wl_display             *wl_display;
static struct wl_registry            *wl_registry;
static struct wl_seat                *wl_seat;
static struct ext_idle_notifier_v1     *idle_notifier;
static struct ext_idle_notification_v1 *idle_notification;
static gint64                         wl_idle_since;
static gboolean                       wl_is_idle;
static guint                          wl_fd_tag;

static void on_idled(void *data, struct ext_idle_notification_v1 *n)
{
    (void)data; (void)n;
    if (!wl_is_idle) log_line("the compositor says: nobody has used the computer");
    wl_is_idle = TRUE;
    wl_idle_since = g_get_monotonic_time();
}

static void on_resumed(void *data, struct ext_idle_notification_v1 *n)
{
    (void)data; (void)n;
    if (wl_is_idle) log_line("the compositor says: the user is back");
    wl_is_idle = FALSE;
}

static const struct ext_idle_notification_v1_listener idle_notification_listener = { on_idled, on_resumed };

static void registry_global(void *data, struct wl_registry *reg, uint32_t name, const char *iface, uint32_t version)
{
    (void)data; (void)version;
    if (!strcmp(iface, wl_seat_interface.name) && !wl_seat)
        wl_seat = wl_registry_bind(reg, name, &wl_seat_interface, 1);
    else if (!strcmp(iface, ext_idle_notifier_v1_interface.name) && !idle_notifier)
        idle_notifier = wl_registry_bind(reg, name, &ext_idle_notifier_v1_interface, 1);
}

static void registry_global_remove(void *data, struct wl_registry *reg, uint32_t name)
{
    (void)data; (void)reg; (void)name;
}

static const struct wl_registry_listener registry_listener = { registry_global, registry_global_remove };

static gboolean wl_readable(gint fd, GIOCondition cond, gpointer data)
{
    (void)fd; (void)cond; (void)data;
    if (!wl_display) return G_SOURCE_REMOVE;
    if (wl_display_dispatch(wl_display) < 0) {
        log_line("lost the connection to the compositor");
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void wl_open(void)
{
    wl_display = wl_display_connect(NULL);
    if (!wl_display) return;
    wl_registry = wl_display_get_registry(wl_display);
    wl_registry_add_listener(wl_registry, &registry_listener, NULL);
    wl_display_roundtrip(wl_display);
    if (!idle_notifier) log_line("the compositor has no ext-idle-notify-v1: the idle time cannot be measured");
    if (idle_notifier)
        wl_fd_tag = g_unix_fd_add(wl_display_get_fd(wl_display), G_IO_IN | G_IO_HUP | G_IO_ERR, wl_readable, NULL);
}

/* Ask the compositor to tell us when the user has been away for deadline_ms. Zero (or nothing to do ever): no
 * notification is created, and the idle time is not measured at all. */
static void wl_arm(long deadline_ms)
{
    if (!idle_notifier || !wl_seat) return;
    if (idle_notification) {
        ext_idle_notification_v1_destroy(idle_notification);
        idle_notification = NULL;
    }
    wl_is_idle = FALSE;
    if (deadline_ms <= 0) return;
    idle_notification = ext_idle_notifier_v1_get_idle_notification(idle_notifier, (uint32_t)deadline_ms, wl_seat);
    ext_idle_notification_v1_add_listener(idle_notification, &idle_notification_listener, NULL);
    wl_display_flush(wl_display);
}

static void wl_arm_next_deadline(void)
{
    /* The first thing that is due: everything after it is worked out from that moment on. */
    long first = 0;
    long mins[3] = { cfg.blank_minutes, cfg.lock_minutes,
                     state.on_battery ? cfg.suspend_bat_minutes : cfg.suspend_ac_minutes };
    for (int i = 0; i < 3; ++i) {
        if (mins[i] <= 0) continue;
        long ms = mins[i] * 60000L;
        if (first == 0 || ms < first) first = ms;
    }
    wl_arm(first);
}

static gint64 wl_idle_ms(void)
{
    if (!idle_notifier) return -1;
    if (!wl_is_idle) return 0;
    return (g_get_monotonic_time() - wl_idle_since) / 1000;
}
#endif

static gint64 idle_ms(void)
{
#ifdef HAVE_WAYLAND_IDLE
    if (wl_display) {
        gint64 ms = wl_idle_ms();
        if (ms >= 0) return ms;
    }
#endif
#ifdef HAVE_XSS
    return x11_idle_ms();
#endif
#if !defined(HAVE_XSS) && !defined(HAVE_WAYLAND_IDLE)
    return -1;
#endif
}

/* Can the idle time of this session be measured at all? `hde-idle --check` says so out loud, and Settings uses
 * it to decide whether the X server has to blank its own screen instead. */
static gboolean can_measure_idle(void)
{
#ifdef HAVE_WAYLAND_IDLE
    if (wl_display && idle_notifier) return TRUE;
#endif
#ifdef HAVE_XSS
    if (x_display && xss_available) return TRUE;
#endif
    return FALSE;
}

static int on_battery(void)
{
    gint64 now = g_get_monotonic_time();
    if (power_checked_at && now - power_checked_at < (gint64)HDE_IDLE_POWER_MS * 1000) return on_battery_cached;
    power_checked_at = now;
    HdePower p;
    hde_power_read(&p);
    on_battery_cached = hde_power_on_battery(&p) ? 1 : 0;
    return on_battery_cached;
}

/* ---------------------------------------------------------------- carrying out what was decided */

static void run_sh(const char *shell)
{
    char *argv[] = { (char *)"/bin/sh", (char *)"-c", (char *)shell, NULL };
    g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL, NULL, NULL, NULL, NULL);
}

static void lock_screen(void)
{
    log_line("locking the screen");
    /* HDE's own lock screen first, then the lockers of the other desktops: the same list as the hotkeys and the
     * Start menu use (src/hde-commands.h), so that locking from here is not a different thing. */
    run_sh(HDE_SH_LOCK);
}

static void suspend_now(void)
{
    log_line("putting the computer to sleep");
    /* systemctl is what logind gives us; the rest is for a machine without systemd. */
    run_sh("systemctl suspend 2>/dev/null || loginctl suspend 2>/dev/null || pm-suspend 2>/dev/null || "
           "dbus-send --system --print-reply --dest=org.freedesktop.login1 /org/freedesktop/login1 "
           "org.freedesktop.login1.Manager.Suspend boolean:true >/dev/null 2>&1");
    /* Do not sleep a second time until somebody has used the computer again (see `armed`). */
    armed = FALSE;
    /* ... but if it did not work (no logind, no permission), start watching again or the session would never
     * sleep again. */
    g_timeout_add_seconds(20, re_arm, NULL);
}

static gboolean re_arm(gpointer data)
{
    (void)data;
    if (!armed) { armed = TRUE; log_line("the computer is still awake: watching again"); }
    return G_SOURCE_REMOVE;
}

static void screen_off(void)
{
    log_line("turning the screen off");
#ifdef HAVE_XSS
    if (x_display && dpms_available) {
        DPMSEnable(x_display);
        DPMSForceLevel(x_display, DPMSModeOff);
        XFlush(x_display);
        return;
    }
#endif
    /* No DPMS here (Wayland, or an X server without it): the compositor turns its own screen off, which is why
     * it is told when the user has been away. */
    run_sh("xset dpms force off 2>/dev/null || true");
}

static void screen_on(void)
{
    log_line("turning the screen on");
#ifdef HAVE_XSS
    if (x_display && dpms_available) {
        DPMSForceLevel(x_display, DPMSModeOn);
        XFlush(x_display);
        return;
    }
#endif
    run_sh("xset dpms force on 2>/dev/null || true");
}

static void carry_out(HdeIdleAction a)
{
    switch (a) {
    case HDE_IDLE_BLANK:    screen_off(); break;
    case HDE_IDLE_UNBLANK:  screen_on(); break;
    case HDE_IDLE_LOCK:     lock_screen(); break;
    case HDE_IDLE_SUSPEND:  suspend_now(); break;
    case HDE_IDLE_NONE:     break;
    }
}

/* ---------------------------------------------------------------- the clock */

static gboolean tick(gpointer data)
{
    (void)data;
    gint64 idle = idle_ms();
    if (idle < 0) return G_SOURCE_CONTINUE;             /* nothing to measure: do nothing at all */

    /* SimulateUserActivity() counts as the user being there, whatever the screen says. */
    if (activity_override) {
        gint64 since_override = (g_get_monotonic_time() - activity_override) / 1000;
        if (since_override < idle) idle = since_override;
        else activity_override = 0;
    }

    state.idle_ms = idle;
    state.on_battery = on_battery();
    state.inhibit_screen = hde_idle_inhibit_screen(&inhibitors);
    state.inhibit_power = hde_idle_inhibit_power(&inhibitors);

    if (idle < HDE_IDLE_ACTIVE_MS) {
        gboolean was_blanked = state.blanked != 0;
        hde_idle_reset(&state);
        if (!armed) { armed = TRUE; log_line("the user is back: watching again"); }
        if (was_blanked) carry_out(HDE_IDLE_UNBLANK);
        return G_SOURCE_CONTINUE;
    }
    if (!armed) return G_SOURCE_CONTINUE;               /* waiting for the user to come back after a sleep */

    HdeIdleAction action = hde_idle_decide(&cfg, &state);
    if (action == HDE_IDLE_NONE) return G_SOURCE_CONTINUE;
    hde_idle_mark(&state, action);
    carry_out(action);
    return G_SOURCE_CONTINUE;
}

/* ---------------------------------------------------------------- the D-Bus interfaces */

static const char *SCREENSAVER_XML =
    "<node>"
    "  <interface name='org.freedesktop.ScreenSaver'>"
    "    <method name='Inhibit'>"
    "      <arg type='s' name='application_name' direction='in'/>"
    "      <arg type='s' name='reason_for_inhibit' direction='in'/>"
    "      <arg type='u' name='cookie' direction='out'/>"
    "    </method>"
    "    <method name='UnInhibit'>"
    "      <arg type='u' name='cookie' direction='in'/>"
    "    </method>"
    "    <method name='SimulateUserActivity'/>"
    "    <method name='GetActive'>"
    "      <arg type='b' name='blanked' direction='out'/>"
    "    </method>"
    "    <method name='GetActiveTime'>"
    "      <arg type='u' name='seconds' direction='out'/>"
    "    </method>"
    "    <method name='GetSessionIdleTime'>"
    "      <arg type='u' name='seconds' direction='out'/>"
    "    </method>"
    "    <method name='SetActive'>"
    "      <arg type='b' name='active' direction='in'/>"
    "    </method>"
    "  </interface>"
    "</node>";

static const char *POWER_XML =
    "<node>"
    "  <interface name='org.freedesktop.PowerManagement'>"
    "    <method name='CanSuspend'><arg type='b' name='can_suspend' direction='out'/></method>"
    "    <method name='Suspend'/>"
    "    <method name='CanHibernate'><arg type='b' name='can_hibernate' direction='out'/></method>"
    "    <method name='Hibernate'/>"
    "  </interface>"
    "</node>";

static const char *POWER_INHIBIT_XML =
    "<node>"
    "  <interface name='org.freedesktop.PowerManagement.Inhibit'>"
    "    <method name='Inhibit'>"
    "      <arg type='s' name='application_name' direction='in'/>"
    "      <arg type='s' name='reason_for_inhibit' direction='in'/>"
    "      <arg type='u' name='cookie' direction='out'/>"
    "    </method>"
    "    <method name='UnInhibit'>"
    "      <arg type='u' name='cookie' direction='in'/>"
    "    </method>"
    "    <method name='HasInhibit'>"
    "      <arg type='b' name='has_inhibit' direction='out'/>"
    "    </method>"
    "  </interface>"
    "</node>";

/* An inhibitor is taken back when the program that asked for it disappears from the bus: a program that crashed
 * never calls UnInhibit, and a laptop that stays awake all night because of it is the worst kind of bug. */
static void forget_sender(uint32_t cookie)
{
    if (inhibitor_senders) g_hash_table_remove(inhibitor_senders, GUINT_TO_POINTER(cookie));
}

static void remember_sender(uint32_t cookie, const char *sender)
{
    if (inhibitor_senders && sender && *sender)
        g_hash_table_replace(inhibitor_senders, GUINT_TO_POINTER(cookie), g_strdup(sender));
}

static void drop_inhibitors_of(const char *sender)
{
    if (!sender || !*sender) return;
    for (int i = hde_idle_inhibit_count(&inhibitors) - 1; i >= 0; --i) {
        uint32_t cookie = hde_idle_inhibit_cookie_at(&inhibitors, i);
        const char *owner = g_hash_table_lookup(inhibitor_senders, GUINT_TO_POINTER(cookie));
        if (!owner || strcmp(owner, sender)) continue;
        hde_idle_inhibit_remove(&inhibitors, cookie);
        forget_sender(cookie);
        log_line("the inhibitor of %s went away with it", sender);
    }
}

static GDBusConnection *session_bus(void)
{
    static GDBusConnection *bus = NULL;
    static gboolean tried = FALSE;
    if (!tried) {
        tried = TRUE;
        bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    }
    return bus;
}

static void watch_sender(const char *sender);

static void sender_vanished(GDBusConnection *conn, const gchar *name, gpointer user_data)
{
    (void)conn; (void)user_data;
    drop_inhibitors_of(name);
}

static void sender_appeared(GDBusConnection *conn, const gchar *name, const gchar *owner, gpointer user_data)
{
    (void)conn; (void)name; (void)owner; (void)user_data;
}

static void watch_sender(const char *sender)
{
    if (!sender || !*sender) return;
    /* A unique name (:1.42) is what a program that is not watching names is called; nothing to watch for a
     * well-known name, but there is nothing to lose either. */
    g_bus_watch_name(G_BUS_TYPE_SESSION, sender, G_BUS_NAME_WATCHER_FLAGS_NONE,
                     sender_appeared, sender_vanished, NULL, NULL);
}

static void method_call(GDBusConnection *conn, const gchar *sender, const gchar *path, const gchar *iface,
                        const gchar *method, GVariant *params, GDBusMethodInvocation *invocation, gpointer user_data);

static const GDBusInterfaceVTable vtable = { method_call, NULL, NULL, { NULL } };

static void reply_u(GDBusMethodInvocation *inv, guint32 v) { g_dbus_method_invocation_return_value(inv, g_variant_new("(u)", v)); }
static void reply_b(GDBusMethodInvocation *inv, gboolean v) { g_dbus_method_invocation_return_value(inv, g_variant_new("(b)", v)); }
static void reply_void(GDBusMethodInvocation *inv) { g_dbus_method_invocation_return_value(inv, NULL); }

/* Both Inhibit methods: the two differ only in what is being held back. */
static void do_inhibit(GDBusMethodInvocation *inv, GVariant *params, int screen, int power)
{
    const char *app = NULL, *reason = NULL;
    g_variant_get(params, "(&s&s)", &app, &reason);
    const char *sender = g_dbus_method_invocation_get_sender(inv);
    uint32_t cookie = hde_idle_inhibit_add(&inhibitors, app, reason, screen, power);
    if (!cookie) {
        g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR, G_DBUS_ERROR_LIMITS_EXCEEDED,
                                              "Too many programs are holding the computer back at once.");
        return;
    }
    remember_sender(cookie, sender);
    watch_sender(sender);
    char why[256];
    hde_idle_inhibit_why(&inhibitors, power && !screen, why, sizeof why);
    log_line("%s asked to keep %s (%s)", app && *app ? app : sender,
             screen && power ? "the screen on and the computer awake" : (screen ? "the screen on" : "the computer awake"),
             why);
    reply_u(inv, cookie);
}

static void do_uninhibit(GDBusMethodInvocation *inv, GVariant *params)
{
    guint32 cookie = 0;
    g_variant_get(params, "(u)", &cookie);
    if (!hde_idle_inhibit_remove(&inhibitors, cookie)) {
        g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                              "No such inhibitor (%u).", cookie);
        return;
    }
    forget_sender(cookie);
    reply_void(inv);
}

static void method_call(GDBusConnection *conn, const gchar *sender, const gchar *path, const gchar *iface,
                        const gchar *method, GVariant *params, GDBusMethodInvocation *invocation, gpointer user_data)
{
    (void)conn; (void)sender; (void)path; (void)user_data;
    gboolean is_screensaver = !strcmp(iface, "org.freedesktop.ScreenSaver");
    gboolean is_power = !strcmp(iface, "org.freedesktop.PowerManagement");
    gboolean is_inhibit = !strcmp(iface, "org.freedesktop.PowerManagement.Inhibit");

    if (is_screensaver && !strcmp(method, "Inhibit")) { do_inhibit(invocation, params, 1, 0); return; }
    if (is_inhibit && !strcmp(method, "Inhibit")) { do_inhibit(invocation, params, 0, 1); return; }
    if (is_screensaver && !strcmp(method, "UnInhibit")) { do_uninhibit(invocation, params); return; }
    if (is_inhibit && !strcmp(method, "UnInhibit")) { do_uninhibit(invocation, params); return; }
    if (is_inhibit && !strcmp(method, "HasInhibit")) { reply_b(invocation, hde_idle_inhibit_power(&inhibitors) > 0); return; }

    if (is_screensaver && !strcmp(method, "SimulateUserActivity")) {
        activity_override = g_get_monotonic_time();
        hde_idle_reset(&state);
        if (state.blanked) carry_out(HDE_IDLE_UNBLANK);
        reply_void(invocation);
        return;
    }
    if (is_screensaver && !strcmp(method, "GetActive")) { reply_b(invocation, state.blanked != 0); return; }
    if (is_screensaver && !strcmp(method, "GetActiveTime")) {
        reply_u(invocation, (guint32)(state.idle_ms / 1000));
        return;
    }
    if (is_screensaver && !strcmp(method, "GetSessionIdleTime")) {
        reply_u(invocation, (guint32)(state.idle_ms / 1000));
        return;
    }
    if (is_screensaver && !strcmp(method, "SetActive")) {
        /* Nothing: this desktop is not a screensaver that can be asked to start. */
        reply_void(invocation);
        return;
    }
    if (is_power && !strcmp(method, "CanSuspend")) { reply_b(invocation, TRUE); return; }
    if (is_power && !strcmp(method, "Suspend")) { hde_idle_mark(&state, HDE_IDLE_SUSPEND); suspend_now(); reply_void(invocation); return; }
    if (is_power && !strcmp(method, "CanHibernate")) { reply_b(invocation, FALSE); return; }
    if (is_power && !strcmp(method, "Hibernate")) {
        g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_NOT_SUPPORTED,
                                              "Hibernation is not offered by this desktop.");
        return;
    }
    g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD,
                                          "No such method: %s.%s", iface, method);
}

static void export_interfaces(GDBusConnection *bus)
{
    static GDBusNodeInfo *ss_node, *pw_node, *inh_node;
    GError *err = NULL;
    if (!ss_node) ss_node = g_dbus_node_info_new_for_xml(SCREENSAVER_XML, &err);
    if (!ss_node) { g_clear_error(&err); return; }
    if (!pw_node) pw_node = g_dbus_node_info_new_for_xml(POWER_XML, &err);
    if (!pw_node) { g_clear_error(&err); return; }
    if (!inh_node) inh_node = g_dbus_node_info_new_for_xml(POWER_INHIBIT_XML, &err);
    if (!inh_node) { g_clear_error(&err); return; }

    g_dbus_connection_register_object(bus, "/org/freedesktop/ScreenSaver", ss_node->interfaces[0], &vtable,
                                      NULL, NULL, &err);
    if (err) { log_line("could not offer org.freedesktop.ScreenSaver: %s", err->message); g_clear_error(&err); }
    g_dbus_connection_register_object(bus, "/org/freedesktop/PowerManagement", pw_node->interfaces[0], &vtable,
                                      NULL, NULL, &err);
    if (err) { log_line("could not offer org.freedesktop.PowerManagement: %s", err->message); g_clear_error(&err); }
    g_dbus_connection_register_object(bus, "/org/freedesktop/PowerManagement/Inhibit", inh_node->interfaces[0],
                                      &vtable, NULL, NULL, &err);
    if (err) { log_line("could not offer org.freedesktop.PowerManagement.Inhibit: %s", err->message); g_clear_error(&err); }
}

static void name_acquired(GDBusConnection *conn, const gchar *name, gpointer user_data)
{
    (void)user_data;
    log_line("offering %s on the session bus", name);
    export_interfaces(conn);
}

static void name_lost(GDBusConnection *conn, const gchar *name, gpointer user_data)
{
    (void)conn; (void)user_data;
    /* Somebody else is the screensaver of this session (a power manager of another desktop, usually): it is
     * their business then, and programs that ask will be answered by them. */
    log_line("%s is taken by another program: programs will ask that one instead", name);
}

/* logind tells us when the computer is about to sleep and when it wakes up. */
static void on_prepare_for_sleep(GDBusConnection *conn, const gchar *sender, const gchar *path, const gchar *iface,
                                 const gchar *signal, GVariant *params, gpointer user_data)
{
    (void)conn; (void)sender; (void)path; (void)iface; (void)signal; (void)user_data;
    gboolean going_to_sleep = FALSE;
    g_variant_get(params, "(b)", &going_to_sleep);
    if (going_to_sleep) {
        log_line("the computer is going to sleep");
        return;
    }
    log_line("the computer woke up: waiting for the user before anything happens again");
    hde_idle_reset(&state);
    armed = FALSE;
}

/* ---------------------------------------------------------------- the command line */

static int cmd_status(void)
{
    char text[256];
    hde_idle_config_text(&cfg, text, sizeof text);
    printf("Settings: %s\n", text);
    printf("Idle time can be measured: %s\n", can_measure_idle() ? "yes" : "no");
    if (can_measure_idle()) printf("Nobody has used the computer for: %.1f s\n", (double)idle_ms() / 1000.0);
    printf("Running on the battery: %s\n", on_battery() ? "yes" : "no");
    int screen = hde_idle_inhibit_screen(&inhibitors), power = hde_idle_inhibit_power(&inhibitors);
    printf("Held back: %s\n", (screen || power) ? "yes" : "no");
    if (screen) { char why[256]; hde_idle_inhibit_why(&inhibitors, 0, why, sizeof why); printf("  screen: %s\n", why); }
    if (power) { char why[256]; hde_idle_inhibit_why(&inhibitors, 1, why, sizeof why); printf("  sleep: %s\n", why); }
    printf("Screen off: %s   Locked: %s   Asleep: %s\n", state.blanked ? "yes" : "no",
           state.locked ? "yes" : "no", state.suspended ? "yes" : "no");
    return 0;
}

static void usage(void)
{
    printf("Usage: hde-idle [OPTION]\n"
           "Turn the screen off, lock the session and put the computer to sleep when the user is away.\n\n"
           "  (no option)        watch the session and do what Settings says (what hde-session runs)\n"
           "  --check            0 when the idle time of this session can be measured at all\n"
           "  --status           what it is doing, and when the next thing is due\n"
           "  --lock             lock the screen now\n"
           "  --suspend          put the computer to sleep now\n"
           "  --blank            turn the screen off now\n"
           "  --unblank          turn the screen on now\n"
           "  --activate         pretend the user just did something (what a program asks for over D-Bus)\n"
           "  --help             this text\n"
           "  --version          the version of HDE\n\n"
           "Settings (~/.config/hde/settings.ini): idle_blank, idle_lock, idle_suspend_ac,\n"
           "idle_suspend_battery (minutes, 0 = never) and idle_lock_before_suspend.\n");
}

int main(int argc, char **argv)
{
    gboolean check = FALSE, status = FALSE, help = FALSE, version = FALSE;
    HdeIdleAction once = HDE_IDLE_NONE;
    gboolean activate = FALSE;

    verbose = g_getenv("HDE_DEBUG") != NULL;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) help = TRUE;
        else if (!strcmp(argv[i], "--version")) version = TRUE;
        else if (!strcmp(argv[i], "--check")) check = TRUE;
        else if (!strcmp(argv[i], "--status")) status = TRUE;
        else if (!strcmp(argv[i], "--lock")) once = HDE_IDLE_LOCK;
        else if (!strcmp(argv[i], "--suspend")) once = HDE_IDLE_SUSPEND;
        else if (!strcmp(argv[i], "--blank")) once = HDE_IDLE_BLANK;
        else if (!strcmp(argv[i], "--unblank")) once = HDE_IDLE_UNBLANK;
        else if (!strcmp(argv[i], "--activate")) activate = TRUE;
        else { fprintf(stderr, "hde-idle: unknown option '%s' (try --help)\n", argv[i]); return 2; }
    }
    if (help) { usage(); return 0; }
    if (version) { printf("hde-idle (HDE) %s\n", HDE_VERSION); return 0; }

    hde_idle_state_init(&state);
    hde_idle_inhibitors_init(&inhibitors);
    inhibitor_senders = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
    read_settings();

    /* Which session is this? WAYLAND_DISPLAY first, so that a session nested in another one is read right. */
#ifdef HAVE_WAYLAND_IDLE
    if (g_getenv("WAYLAND_DISPLAY")) wl_open();
#endif
#ifdef HAVE_XSS
    /* Also tried when the compositor could not be reached or has no idle protocol: some sessions are X11 after
     * all, and there is nothing to lose by asking. */
    if (!can_measure_idle()) x11_open();
#endif

    if (once != HDE_IDLE_NONE) { carry_out(once); return 0; }
    if (activate) { activity_override = g_get_monotonic_time(); hde_idle_reset(&state); return 0; }
    if (check) {
        if (can_measure_idle()) {
            printf("ok: the idle time of this session can be measured\n");
            return 0;
        }
        printf("no: this session cannot measure how long the user has been away\n");
        return 1;
    }
    if (status) return cmd_status();

    if (!can_measure_idle()) {
        /* Nothing to watch: say so and go away, so that hde-session does not keep restarting us. */
        fprintf(stderr, "hde-idle: this session cannot measure how long the user has been away; "
                        "nothing will be turned off, locked or put to sleep\n");
        return 1;
    }

    GDBusConnection *bus = session_bus();
    if (bus) {
        /* Three names, one of them an interface of another: programs that ask for an inhibitor find it here. */
        g_bus_own_name(G_BUS_TYPE_SESSION, "org.freedesktop.ScreenSaver", G_BUS_NAME_OWNER_FLAGS_NONE,
                       NULL, name_acquired, name_lost, NULL, NULL);
        g_bus_own_name(G_BUS_TYPE_SESSION, "org.freedesktop.PowerManagement", G_BUS_NAME_OWNER_FLAGS_NONE,
                       NULL, name_acquired, name_lost, NULL, NULL);
        g_dbus_connection_signal_subscribe(bus, "org.freedesktop.login1", "org.freedesktop.login1.Manager",
                                           "PrepareForSleep", "/org/freedesktop/login1", NULL,
                                           G_DBUS_SIGNAL_FLAGS_NONE, on_prepare_for_sleep, NULL, NULL);
    }
    char *ini = g_build_filename(g_get_user_config_dir(), "hde", "settings.ini", NULL);
    GFile *f = g_file_new_for_path(ini);
    GFileMonitor *mon = g_file_monitor_file(f, G_FILE_MONITOR_NONE, NULL, NULL);
    if (mon) g_signal_connect(mon, "changed", G_CALLBACK(settings_monitor_changed), NULL);
    g_object_unref(f);
    g_free(ini);

#ifdef HAVE_WAYLAND_IDLE
    wl_arm_next_deadline();
#endif

    log_line("watching the session");
    g_timeout_add(HDE_IDLE_TICK_MS, tick, NULL);
    loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(loop);

    g_main_loop_unref(loop);
    if (mon) g_object_unref(mon);
    g_hash_table_destroy(inhibitor_senders);
#ifdef HAVE_WAYLAND_IDLE
    (void)wl_fd_tag;
#endif
    return 0;
}
