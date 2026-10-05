/* Hyggshi Settings — the GTK3 settings center of the Hyggshi Desktop Environment.
 *
 *   hde-settings              open the settings window
 *   hde-settings <page>       open a page directly: display appearance input sound network bluetooth
 *                             windows notifications power keyboard users about
 *   hde-settings --apply      re-apply the settings needed at every login, then exit (called by hde-session)
 *
 * The big pages live in separate files: hde-settings-{network,bluetooth,appearance,windows,keyboard,sound}.c
 */
#include "hde-settings.h"
#include "hde-theme.h"
#include "hde-input.h"
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <sys/utsname.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define APP_NAME "Hyggshi Settings"

static GtkWidget *window;
static GtkWidget *content_stack;
static GtkWidget *page_title;
static GtkWidget *page_subtitle;
static GtkWidget *sidebar;
static GtkWidget *status_label;
static GtkCssProvider *app_css;

typedef struct { const char *id; const char *icon; const char *title; const char *subtitle; } SettingItem;
static const SettingItem items[] = {
    /* icon: several alternative names separated by '|'; the first one present in the icon theme is used */
    { "display", "video-display-symbolic|preferences-desktop-display-symbolic", "Display", "Resolution, scale and monitors" },
    { "appearance", "preferences-desktop-appearance-symbolic|preferences-desktop-theme-symbolic|applications-graphics-symbolic|weather-clear-night-symbolic",
      "Appearance", "Dark mode, theme, accent, icons and fonts" },
    { "input", "input-mouse-symbolic", "Input", "Mouse, touchpad and pointer" },
    { "sound", "audio-volume-high-symbolic", "Sound", "Output, input and volume" },
    { "network", "network-wireless-symbolic", "Network", "Wi-Fi networks, Ethernet and VPN" },
    { "bluetooth", "bluetooth-active-symbolic|bluetooth-symbolic", "Bluetooth", "Pair and connect Bluetooth devices" },
    { "windows", "preferences-system-windows-symbolic|focus-windows-symbolic|view-dual-symbolic|window-maximize-symbolic",
      "Window Management", "Window manager used by the desktop" },
    { "notifications", "preferences-system-notifications-symbolic|notification-symbolic", "Notifications", "Alerts and Do Not Disturb" },
    { "power", "battery-good-symbolic|battery-full-symbolic", "Power", "Sleep, screen timeout and battery" },
    { "keyboard", "input-keyboard-symbolic", "Keyboard & Shortcuts", "Layouts, repeat, Super key and sound keys" },
    { "users", "system-users-symbolic|avatar-default-symbolic", "Users", "Accounts and administrator" },
    { "about", "help-about-symbolic", "About", "Hyggshi Desktop Environment" },
};

/* ================= configuration ================= */
static char *cfg_path(void) { return hde_settings_ini_path(); }

static GKeyFile *cfg_load(void)
{
    GKeyFile *kf = g_key_file_new();
    char *p = cfg_path();
    g_key_file_load_from_file(kf, p, G_KEY_FILE_KEEP_COMMENTS, NULL);
    g_free(p);
    return kf;
}

static void cfg_save(GKeyFile *kf)
{
    char *p = cfg_path();
    char *dir = g_path_get_dirname(p);
    g_mkdir_with_parents(dir, 0755);
    GError *e = NULL;
    if (!g_key_file_save_to_file(kf, p, &e)) {
        settings_status("Could not save settings: %s", e->message);
        g_clear_error(&e);
    } else {
        settings_status("Changes saved");
    }
    g_free(dir);
    g_free(p);
}

GKeyFile *cfg_begin(void)
{
    return cfg_load();
}

void cfg_commit(GKeyFile *kf)
{
    cfg_save(kf);
    g_key_file_free(kf);
}

gboolean cfg_has_key(const char *key)
{
    GKeyFile *kf = cfg_load();
    gboolean r = g_key_file_has_key(kf, CONFIG_GROUP, key, NULL);
    g_key_file_free(kf);
    return r;
}

gboolean cfg_get_bool(const char *key, gboolean fallback)
{
    GKeyFile *kf = cfg_load();
    GError *e = NULL;
    gboolean v = g_key_file_get_boolean(kf, CONFIG_GROUP, key, &e);
    if (e) { v = fallback; g_clear_error(&e); }
    g_key_file_free(kf);
    return v;
}

int cfg_get_int(const char *key, int fallback)
{
    GKeyFile *kf = cfg_load();
    GError *e = NULL;
    int v = g_key_file_get_integer(kf, CONFIG_GROUP, key, &e);
    if (e) { v = fallback; g_clear_error(&e); }
    g_key_file_free(kf);
    return v;
}

double cfg_get_double(const char *key, double fallback)
{
    GKeyFile *kf = cfg_load();
    GError *e = NULL;
    double v = g_key_file_get_double(kf, CONFIG_GROUP, key, &e);
    if (e) { v = fallback; g_clear_error(&e); }
    g_key_file_free(kf);
    return v;
}

char *cfg_get_string(const char *key, const char *fallback)
{
    GKeyFile *kf = cfg_load();
    char *v = g_key_file_get_string(kf, CONFIG_GROUP, key, NULL);
    g_key_file_free(kf);
    return v ? v : g_strdup(fallback);
}

void cfg_set_bool(const char *key, gboolean value)
{
    GKeyFile *kf = cfg_load();
    g_key_file_set_boolean(kf, CONFIG_GROUP, key, value);
    cfg_save(kf);
    g_key_file_free(kf);
}

void cfg_set_int(const char *key, int value)
{
    GKeyFile *kf = cfg_load();
    g_key_file_set_integer(kf, CONFIG_GROUP, key, value);
    cfg_save(kf);
    g_key_file_free(kf);
}

void cfg_set_double(const char *key, double value)
{
    GKeyFile *kf = cfg_load();
    g_key_file_set_double(kf, CONFIG_GROUP, key, value);
    cfg_save(kf);
    g_key_file_free(kf);
}

void cfg_set_string(const char *key, const char *value)
{
    GKeyFile *kf = cfg_load();
    g_key_file_set_string(kf, CONFIG_GROUP, key, value ? value : "");
    cfg_save(kf);
    g_key_file_free(kf);
}

/* ================= UI helpers ================= */
GtkWidget *settings_window(void) { return window; }

void settings_status(const char *fmt, ...)
{
    if (!status_label) return;
    va_list ap;
    va_start(ap, fmt);
    char *s = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    gtk_label_set_text(GTK_LABEL(status_label), s);
    g_free(s);
}

GtkWidget *row_box(const char *title, const char *description, GtkWidget *control)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 18);
    gtk_widget_set_margin_top(row, 8); gtk_widget_set_margin_bottom(row, 8);
    gtk_widget_set_margin_start(row, 6); gtk_widget_set_margin_end(row, 6);
    GtkWidget *labels = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    gtk_widget_set_hexpand(labels, TRUE);
    GtkWidget *t = gtk_label_new(title);
    gtk_widget_set_halign(t, GTK_ALIGN_START);
    gtk_label_set_xalign(GTK_LABEL(t), 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(t), "row-title");
    gtk_box_pack_start(GTK_BOX(labels), t, FALSE, FALSE, 0);
    if (description && *description) {
        GtkWidget *d = gtk_label_new(description);
        gtk_label_set_line_wrap(GTK_LABEL(d), TRUE);
        gtk_label_set_xalign(GTK_LABEL(d), 0);
        gtk_widget_set_halign(d, GTK_ALIGN_START);
        gtk_style_context_add_class(gtk_widget_get_style_context(d), "row-description");
        gtk_box_pack_start(GTK_BOX(labels), d, FALSE, FALSE, 0);
        g_object_set_data(G_OBJECT(row), "hde-description", d);
    }
    gtk_box_pack_start(GTK_BOX(row), labels, TRUE, TRUE, 0);
    if (control) {
        gtk_widget_set_valign(control, GTK_ALIGN_CENTER);
        gtk_box_pack_start(GTK_BOX(row), control, FALSE, FALSE, 0);
    }
    return row;
}

GtkWidget *section(const char *title)
{
    GtkWidget *l = gtk_label_new(title);
    gtk_widget_set_halign(l, GTK_ALIGN_START);
    gtk_widget_set_margin_top(l, 18);
    gtk_widget_set_margin_bottom(l, 5);
    gtk_style_context_add_class(gtk_widget_get_style_context(l), "section-title");
    return l;
}

GtkWidget *page_base(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_margin_start(box, 28); gtk_widget_set_margin_end(box, 28);
    gtk_widget_set_margin_top(box, 4); gtk_widget_set_margin_bottom(box, 26);
    return box;
}

GtkWidget *info_label(const char *text)
{
    GtkWidget *l = gtk_label_new(text);
    gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_widget_set_halign(l, GTK_ALIGN_FILL);
    gtk_style_context_add_class(gtk_widget_get_style_context(l), "row-description");
    gtk_widget_set_margin_top(l, 4);
    gtk_widget_set_margin_bottom(l, 4);
    return l;
}

GtkWidget *card_new(void)
{
    GtkWidget *lb = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(lb), GTK_SELECTION_NONE);
    gtk_style_context_add_class(gtk_widget_get_style_context(lb), "card");
    return lb;
}

static void destroy_child(GtkWidget *w, gpointer d) { (void)d; gtk_widget_destroy(w); }

void card_clear(GtkWidget *card)
{
    gtk_container_foreach(GTK_CONTAINER(card), destroy_child, NULL);
}

GtkWidget *card_placeholder(const char *text)
{
    GtkWidget *l = gtk_label_new(text);
    gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
    gtk_widget_set_margin_top(l, 14); gtk_widget_set_margin_bottom(l, 14);
    gtk_widget_set_margin_start(l, 12); gtk_widget_set_margin_end(l, 12);
    gtk_style_context_add_class(gtk_widget_get_style_context(l), "row-description");
    gtk_widget_show(l);
    return l;
}

GtkWidget *icon_button(const char *icon_name, const char *tooltip)
{
    GtkWidget *b = gtk_button_new_from_icon_name(icon_name, GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(b, tooltip);
    gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
    return b;
}

void message_dialog(GtkMessageType type, const char *title, const char *detail)
{
    GtkWidget *m = gtk_message_dialog_new(GTK_WINDOW(window), GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                          type, GTK_BUTTONS_CLOSE, "%s", title);
    if (detail && *detail) gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(m), "%s", detail);
    gtk_dialog_run(GTK_DIALOG(m));
    gtk_widget_destroy(m);
}

gboolean have_program(const char *name)
{
    char *p = g_find_program_in_path(name);
    g_free(p);
    return p != NULL;
}

void launch_candidates(const char *const *commands)
{
    for (int i = 0; commands[i]; i++) {
        char **w = g_strsplit(commands[i], " ", 2);
        gboolean ok = w[0] && have_program(w[0]);
        g_strfreev(w);
        if (!ok) continue;
        GError *e = NULL;
        if (g_spawn_command_line_async(commands[i], &e)) return;
        g_clear_error(&e);
    }
    settings_status("No suitable tool found for this action");
}

/* ================= asynchronous commands ================= */
typedef struct {
    SettingsRunCb cb;
    gpointer data;
    GSubprocess *proc;
    guint timer;
    gboolean timed_out;
} RunCtx;

static gboolean run_timeout_cb(gpointer p)
{
    RunCtx *r = p;
    r->timer = 0;
    r->timed_out = TRUE;
    g_subprocess_force_exit(r->proc);
    return G_SOURCE_REMOVE;
}

static char *bytes_to_utf8(GBytes *b)
{
    if (!b) return g_strdup("");
    gsize n = 0;
    const char *d = g_bytes_get_data(b, &n);
    return d ? g_utf8_make_valid(d, (gssize)n) : g_strdup("");
}

static void run_done_cb(GObject *src, GAsyncResult *res, gpointer p)
{
    RunCtx *r = p;
    GBytes *ob = NULL, *eb = NULL;
    GError *e = NULL;
    gboolean comm_ok = g_subprocess_communicate_finish(G_SUBPROCESS(src), res, &ob, &eb, &e);
    if (r->timer) g_source_remove(r->timer);
    char *out = bytes_to_utf8(ob), *err = bytes_to_utf8(eb);
    int status = -1;
    gboolean ok = FALSE;
    if (comm_ok && g_subprocess_get_if_exited(r->proc)) {
        status = g_subprocess_get_exit_status(r->proc);
        ok = status == 0;
    }
    if (r->timed_out) { g_free(err); err = g_strdup("The operation timed out"); ok = FALSE; }
    else if (!comm_ok && e && !*err) { g_free(err); err = g_strdup(e->message); }
    if (r->cb) r->cb(ok, status, out, err, r->data);
    g_clear_error(&e);
    if (ob) g_bytes_unref(ob);
    if (eb) g_bytes_unref(eb);
    g_free(out); g_free(err);
    g_object_unref(r->proc);
    g_free(r);
}

void run_argv_async(const char *const *argv, const char *stdin_text, int timeout_sec, SettingsRunCb cb, gpointer data)
{
    GSubprocessFlags fl = G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE |
                          (stdin_text ? G_SUBPROCESS_FLAGS_STDIN_PIPE : G_SUBPROCESS_FLAGS_STDIN_INHERIT);
    GSubprocessLauncher *l = g_subprocess_launcher_new(fl);
    /* English error messages so they can be parsed; keep LC_CTYPE (UTF-8) for network/device names */
    g_subprocess_launcher_unsetenv(l, "LC_ALL");
    g_subprocess_launcher_unsetenv(l, "LANGUAGE");
    g_subprocess_launcher_setenv(l, "LC_MESSAGES", "C", TRUE);
    GError *e = NULL;
    GSubprocess *p = g_subprocess_launcher_spawnv(l, argv, &e);
    g_object_unref(l);
    if (!p) {
        if (cb) cb(FALSE, -1, "", e ? e->message : "Could not start process", data);
        g_clear_error(&e);
        return;
    }
    RunCtx *r = g_new0(RunCtx, 1);
    r->cb = cb;
    r->data = data;
    r->proc = p;
    r->timer = timeout_sec > 0 ? g_timeout_add_seconds(timeout_sec, run_timeout_cb, r) : 0;
    GBytes *in = stdin_text ? g_bytes_new(stdin_text, strlen(stdin_text)) : NULL;
    g_subprocess_communicate_async(p, in, NULL, run_done_cb, r);
    if (in) g_bytes_unref(in);
}

void run_shell_async(const char *script, SettingsRunCb cb, gpointer data)
{
    const char *argv[] = { "/bin/sh", "-c", script, NULL };
    run_argv_async(argv, NULL, 30, cb, data);
}

/* ================= apply settings at login ================= */
static void run_quiet(const char *script)
{
    gchar *argv[] = { (gchar *)"/bin/sh", (gchar *)"-c", (gchar *)script, NULL };
    GError *e = NULL;
    if (!g_spawn_sync(NULL, argv, NULL, G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                      NULL, NULL, NULL, NULL, NULL, &e))
        g_clear_error(&e);
}

void apply_keyboard_settings(void)
{
    static const char *const layouts[] = { "us", "vn", "gb", "jp" };
    if (cfg_has_key("keyboard_layout") && have_program("setxkbmap")) {
        int i = cfg_get_int("keyboard_layout", 0);
        if (i >= 0 && i < (int)G_N_ELEMENTS(layouts)) {
            char *cmd = g_strdup_printf("setxkbmap -layout %s", layouts[i]);
            run_quiet(cmd);
            g_free(cmd);
        }
    }
    if ((cfg_has_key("repeat_rate") || cfg_has_key("repeat_delay")) && have_program("xset")) {
        static const int delays[] = { 250, 500, 800 };
        int rate = 10 + CLAMP(cfg_get_int("repeat_rate", 50), 0, 100) / 2;      /* 10..60 per second */
        int delay = delays[CLAMP(cfg_get_int("repeat_delay", 1), 0, 2)];
        char *cmd = g_strdup_printf("xset r on; xset r rate %d %d", delay, rate);
        run_quiet(cmd);
        g_free(cmd);
    }
}

void apply_power_settings(void)
{
    if (!cfg_has_key("screen_timeout") || !have_program("xset")) return;
    static const int mins[] = { 0, 5, 10, 15, 30, 60 };
    int i = CLAMP(cfg_get_int("screen_timeout", 2), 0, 5);
    int s = mins[i] * 60;
    char *cmd = s == 0 ? g_strdup("xset s off; xset -dpms")
                       : g_strdup_printf("xset s %d %d; xset +dpms; xset dpms %d %d %d", s, s, s, s, s);
    run_quiet(cmd);
    g_free(cmd);
}

/* Touchpad / mouse: talks to the X server directly (src/hde-input.c, no `xinput` program needed). The touchpad values
 * (natural scrolling, tap to click) are always applied, so the touchpad really does what this page shows. */
void apply_input_settings(void)
{
    HdeInputPrefs p;
    hde_input_prefs_load(&p);
    Display *dpy = hde_input_open();
    if (!dpy) return;
    hde_input_apply(dpy, -1, &p, "hde-settings: input");
    XCloseDisplay(dpy);
}

/* ================= simple pages ================= */
static void display_dialog(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    const char *cmds[] = { "arandr", "lxrandr", "xfce4-display-settings", "lxqt-config-monitor", "gnome-control-center display", NULL };
    launch_candidates(cmds);
}

static void cb_scale(GtkComboBox *c, gpointer x)
{
    (void)x;
    cfg_set_int("scale", gtk_combo_box_get_active(c));
    settings_status("Text scale applied to running GTK applications");
}

static void cb_int_combo(GtkComboBox *c, gpointer key) { cfg_set_int(key, gtk_combo_box_get_active(c)); }

static gboolean cb_bool(GtkSwitch *s, gboolean v, gpointer key)
{
    (void)s;
    cfg_set_bool(key, v);
    return FALSE;
}

static GtkWidget *combo_with(const char *const *labels, int n, int active)
{
    GtkWidget *c = gtk_combo_box_text_new();
    for (int i = 0; i < n; i++) gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(c), labels[i]);
    gtk_combo_box_set_active(GTK_COMBO_BOX(c), CLAMP(active, 0, n - 1));
    return c;
}

static GtkWidget *switch_with(const char *key, gboolean def)
{
    GtkWidget *s = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(s), cfg_get_bool(key, def));
    g_signal_connect(s, "state-set", G_CALLBACK(cb_bool), (gpointer)key);
    return s;
}

static GtkWidget *make_display_page(void)
{
    GtkWidget *box = page_base();
    GdkDisplay *d = gdk_display_get_default();
    int n = d ? gdk_display_get_n_monitors(d) : 0;
    gtk_box_pack_start(GTK_BOX(box), section("Screens"), FALSE, FALSE, 0);
    GtkWidget *card = card_new();
    for (int i = 0; d && i < n; i++) {
        GdkMonitor *m = gdk_display_get_monitor(d, i);
        GdkRectangle r;
        gdk_monitor_get_geometry(m, &r);
        const char *model = gdk_monitor_get_model(m);
        char *title = g_strdup_printf("%s%s", model ? model : "Display", gdk_monitor_is_primary(m) ? " (primary)" : "");
        char *desc = g_strdup_printf("%d × %d at %d,%d · %d Hz", r.width, r.height, r.x, r.y,
                                     (gdk_monitor_get_refresh_rate(m) + 500) / 1000);
        gtk_container_add(GTK_CONTAINER(card), row_box(title, desc, NULL));
        g_free(title); g_free(desc);
    }
    if (!n) gtk_container_add(GTK_CONTAINER(card), card_placeholder("No monitors detected"));
    gtk_box_pack_start(GTK_BOX(box), card, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(box), section("Display settings"), FALSE, FALSE, 0);
    const char *scales[] = { "100%", "125%", "150%", "200%" };
    GtkWidget *scale = combo_with(scales, 4, cfg_get_int("scale", 0));
    g_signal_connect(scale, "changed", G_CALLBACK(cb_scale), NULL);
    gtk_box_pack_start(GTK_BOX(box), row_box("Text scale", "Scales text in GTK applications (applies immediately).", scale), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), row_box("Night Light", "Reduce blue light during evening hours.", switch_with("night_light", FALSE)), FALSE, FALSE, 0);
    GtkWidget *btn = gtk_button_new_with_label("Open advanced display settings (resolution, rotation, arrangement)");
    gtk_widget_set_halign(btn, GTK_ALIGN_START);
    g_signal_connect(btn, "clicked", G_CALLBACK(display_dialog), NULL);
    gtk_box_pack_start(GTK_BOX(box), btn, FALSE, FALSE, 10);
    return box;
}

static guint input_apply_id;
static GtkWidget *input_devices_card;

/* The touchpads and mice HDE configures, with their state right now (refreshed after every change and hotplug). */
static void input_devices_refresh(void)
{
    if (!input_devices_card) return;
    card_clear(input_devices_card);
    HdeInputDevice devs[16];
    int n = 0;
    Display *dpy = hde_input_open();
    if (dpy) {
        n = hde_input_list(dpy, devs, (int)G_N_ELEMENTS(devs));
        XCloseDisplay(dpy);
    }
    for (int i = 0; i < n; i++) {
        const HdeInputDevice *d = &devs[i];
        GString *desc = g_string_new(d->kind == HDE_INPUT_TOUCHPAD ? "Touchpad" : "Mouse");
        g_string_append_printf(desc, " · %s driver", d->driver);
        if (d->natural >= 0) g_string_append_printf(desc, " · natural scrolling %s", d->natural ? "on" : "off");
        if (d->tapping >= 0) g_string_append_printf(desc, " · tap to click %s", d->tapping ? "on" : "off");
        if (getenv("HDE_DEBUG")) fprintf(stderr, "hde-settings: input device: %s: %s\n", d->name, desc->str);
        gtk_container_add(GTK_CONTAINER(input_devices_card), row_box(d->name, desc->str, NULL));
        g_string_free(desc, TRUE);
    }
    if (n == 0)
        gtk_container_add(GTK_CONTAINER(input_devices_card), card_placeholder(
            !hde_input_supported() ? "HDE was built without libxi-dev, so these settings cannot be applied. "
                                     "Install it (sudo apt install libxi-dev) and rebuild HDE."
                                   : "No touchpad or mouse found that uses the libinput or synaptics X driver "
                                     "(package xserver-xorg-input-libinput)."));
    gtk_widget_show_all(input_devices_card);
}

static gboolean input_apply_idle(gpointer d)
{
    (void)d;
    input_apply_id = 0;
    apply_input_settings();
    input_devices_refresh();
    return G_SOURCE_REMOVE;
}

static void input_apply_later(void)
{
    if (input_apply_id) g_source_remove(input_apply_id);
    input_apply_id = g_timeout_add(150, input_apply_idle, NULL);
}

/* a device was plugged in / unplugged while the page is open (hde-xsettings configures it; show it a bit later) */
static void on_seat_device_changed(GdkSeat *seat, GdkDevice *device, gpointer data)
{
    (void)seat; (void)device; (void)data;
    input_apply_later();
}

/* every time the page is shown: make sure the devices match the page (also without hde-xsettings, or if something
 * else changed them), then list them */
static void on_input_page_map(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    input_apply_later();
}

static void on_input_page_destroy(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    input_devices_card = NULL;
    GdkDisplay *dsp = gdk_display_get_default();
    GdkSeat *seat = dsp ? gdk_display_get_default_seat(dsp) : NULL;
    if (seat) g_signal_handlers_disconnect_by_func(seat, G_CALLBACK(on_seat_device_changed), NULL);
}

static gboolean cb_input_bool(GtkSwitch *s, gboolean v, gpointer key)
{
    (void)s;
    cfg_set_bool(key, v);
    input_apply_later();
    return FALSE;
}

static void cb_pointer_speed(GtkRange *r, gpointer x)
{
    (void)x;
    cfg_set_double("pointer_speed", gtk_range_get_value(r));
    input_apply_later();
}

static GtkWidget *input_switch(const char *key, gboolean def)
{
    GtkWidget *s = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(s), cfg_get_bool(key, def));
    g_signal_connect(s, "state-set", G_CALLBACK(cb_input_bool), (gpointer)key);
    return s;
}

static GtkWidget *make_input_page(void)
{
    GtkWidget *box = page_base();
    /* defaults here = defaults in hde-input.c */
    gtk_box_pack_start(GTK_BOX(box), section("Touchpad"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), row_box("Natural scrolling",
        "On: the content follows your fingers (swipe up and the page moves up, as on a phone). "
        "Off: the classic direction, where the content moves against your fingers.",
        input_switch("natural_scroll", TRUE)), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), row_box("Tap to click",
        "Tap the touchpad to click, tap with two fingers to right-click.", input_switch("tap_to_click", TRUE)),
        FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(box), section("Mouse"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), row_box("Natural scrolling", "Reverse the direction of the mouse wheel.",
                                             input_switch("mouse_natural_scroll", FALSE)), FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(box), section("Pointer"), FALSE, FALSE, 0);
    GtkWidget *speed = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 1, 0.05);
    gtk_scale_set_draw_value(GTK_SCALE(speed), FALSE);
    gtk_widget_set_size_request(speed, 200, -1);
    gtk_range_set_value(GTK_RANGE(speed), cfg_get_double("pointer_speed", 0.5));
    g_signal_connect(speed, "value-changed", G_CALLBACK(cb_pointer_speed), NULL);
    gtk_box_pack_start(GTK_BOX(box), row_box("Pointer speed", "Adjust cursor movement speed (touchpad and mouse).", speed),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), row_box("Pointer acceleration", "Accelerate the pointer for faster movement.",
                                             input_switch("pointer_acceleration", TRUE)), FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(box), section("Devices"), FALSE, FALSE, 0);
    input_devices_card = card_new();
    gtk_box_pack_start(GTK_BOX(box), input_devices_card, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), info_label("Changes apply immediately, to devices plugged in later too "
                                                "(and again after suspend)."), FALSE, FALSE, 0);
    g_signal_connect(box, "map", G_CALLBACK(on_input_page_map), NULL);
    g_signal_connect(box, "destroy", G_CALLBACK(on_input_page_destroy), NULL);
    GdkDisplay *dsp = gdk_display_get_default();
    GdkSeat *seat = dsp ? gdk_display_get_default_seat(dsp) : NULL;
    if (seat) {
        g_signal_connect(seat, "device-added", G_CALLBACK(on_seat_device_changed), NULL);
        g_signal_connect(seat, "device-removed", G_CALLBACK(on_seat_device_changed), NULL);
    }
    return box;
}

static void send_test_notification(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    GError *e = NULL;
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &e);
    if (!bus) {
        settings_status("No session bus: %s", e->message);
        g_clear_error(&e);
        return;
    }
    const char *actions[] = { NULL };
    GVariantBuilder hints;
    g_variant_builder_init(&hints, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&hints, "{sv}", "desktop-entry", g_variant_new_string("hyggshi-settings"));
    GVariant *r = g_dbus_connection_call_sync(bus, "org.freedesktop.Notifications", "/org/freedesktop/Notifications",
        "org.freedesktop.Notifications", "Notify",
        g_variant_new("(susss^asa{sv}i)", "Hyggshi Settings", 0u, "preferences-system-notifications",
                      "Test notification", "Notifications are working. <b>Do Not Disturb</b> hides popups like this one.",
                      actions, &hints, -1),
        G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 3000, NULL, &e);
    if (r) { settings_status("Test notification sent"); g_variant_unref(r); }
    else { settings_status("No notification daemon answered: %s", e->message); g_clear_error(&e); }
    g_object_unref(bus);
}

static GtkWidget *make_notifications_page(void)
{
    GtkWidget *box = page_base();
    gtk_box_pack_start(GTK_BOX(box), section("Notifications"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), row_box("Do Not Disturb", "Hide notification popups (critical alerts still appear). Also available from the bell on the panel.", switch_with("dnd", FALSE)), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), row_box("Notification popups", "Show notification banners above the panel.", switch_with("notification_popups", TRUE)), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), row_box("Notification sounds", "Play a sound for incoming notifications.", switch_with("notification_sounds", TRUE)), FALSE, FALSE, 0);
    GtkWidget *test = gtk_button_new_with_label("Send a test notification");
    gtk_widget_set_halign(test, GTK_ALIGN_START);
    g_signal_connect(test, "clicked", G_CALLBACK(send_test_notification), NULL);
    gtk_box_pack_start(GTK_BOX(box), test, FALSE, FALSE, 10);
    return box;
}

static void cb_screen_timeout(GtkComboBox *c, gpointer x)
{
    (void)x;
    cfg_set_int("screen_timeout", gtk_combo_box_get_active(c));
    apply_power_settings();
}

static GtkWidget *make_power_page(void)
{
    GtkWidget *box = page_base();
    const char *times[] = { "Never", "5 minutes", "10 minutes", "15 minutes", "30 minutes", "1 hour" };
    gtk_box_pack_start(GTK_BOX(box), section("Power saving"), FALSE, FALSE, 0);
    GtkWidget *screen = combo_with(times, 6, cfg_get_int("screen_timeout", 2));
    g_signal_connect(screen, "changed", G_CALLBACK(cb_screen_timeout), NULL);
    gtk_box_pack_start(GTK_BOX(box), row_box("Screen timeout", "Turn off the display after inactivity.", screen), FALSE, FALSE, 0);
    GtkWidget *sleep = combo_with(times, 6, cfg_get_int("sleep_timeout", 3));
    g_signal_connect(sleep, "changed", G_CALLBACK(cb_int_combo), "sleep_timeout");
    gtk_box_pack_start(GTK_BOX(box), row_box("Automatic suspend", "Suspend the computer after inactivity (needs a power manager such as xfce4-power-manager).", sleep), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), row_box("Battery saver", "Reduce background activity when enabled.", switch_with("battery_saver", FALSE)), FALSE, FALSE, 0);
    return box;
}

static void cb_user_settings(GtkButton *b, gpointer x)
{
    (void)b; (void)x;
    const char *cmds[] = { "mugshot", "users-admin", "gnome-control-center user-accounts", "system-config-users", NULL };
    launch_candidates(cmds);
}

static GtkWidget *make_users_page(void)
{
    GtkWidget *box = page_base();
    gtk_box_pack_start(GTK_BOX(box), section("Current account"), FALSE, FALSE, 0);
    GtkWidget *card = card_new();
    const char *labels[] = { "Username", "Name", "Home directory", "Administrator" };
    gboolean admin = geteuid() == 0;
    char *idout = NULL;
    if (g_spawn_command_line_sync("id -nG", &idout, NULL, NULL, NULL) && idout)
        admin = admin || strstr(idout, "sudo") || strstr(idout, "wheel") || strstr(idout, "admin");
    g_free(idout);
    const char *vals[] = { g_get_user_name(), g_get_real_name(), g_get_home_dir(), admin ? "Yes" : "No" };
    for (int i = 0; i < 4; i++) gtk_container_add(GTK_CONTAINER(card), row_box(labels[i], vals[i], NULL));
    gtk_box_pack_start(GTK_BOX(box), card, FALSE, FALSE, 0);
    GtkWidget *btn = gtk_button_new_with_label("Open system user management");
    gtk_widget_set_halign(btn, GTK_ALIGN_START);
    g_signal_connect(btn, "clicked", G_CALLBACK(cb_user_settings), NULL);
    gtk_box_pack_start(GTK_BOX(box), btn, FALSE, FALSE, 12);
    return box;
}

static GtkWidget *make_about_page(void)
{
    GtkWidget *box = page_base();
    GtkWidget *logo = gtk_image_new_from_icon_name("preferences-system", GTK_ICON_SIZE_DIALOG);
    gtk_widget_set_halign(logo, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(box), logo, FALSE, FALSE, 5);
    GtkWidget *v = gtk_label_new("Hyggshi Desktop Environment 1.0");
    gtk_widget_set_halign(v, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(v), "about-title");
    gtk_box_pack_start(GTK_BOX(box), v, FALSE, FALSE, 0);
    GtkWidget *card = card_new();
    struct utsname u;
    if (uname(&u) == 0) {
        char *k = g_strdup_printf("%s %s", u.sysname, u.release);
        gtk_container_add(GTK_CONTAINER(card), row_box("Kernel", k, NULL));
        gtk_container_add(GTK_CONTAINER(card), row_box("Architecture", u.machine, NULL));
        g_free(k);
    }
    char *os = g_get_os_info(G_OS_INFO_KEY_PRETTY_NAME);
    if (os) { gtk_container_add(GTK_CONTAINER(card), row_box("Operating system", os, NULL)); g_free(os); }
    const char *wm = g_getenv("HDE_WM");
    gtk_container_add(GTK_CONTAINER(card), row_box("Session", g_getenv("HDE_SESSION_PID") ? "HDE (X11)" : "Not running inside an HDE session", NULL));
    if (wm && *wm) gtk_container_add(GTK_CONTAINER(card), row_box("Window manager setting", wm, NULL));
    gtk_container_add(GTK_CONTAINER(card), row_box("Configuration", "~/.config/hde/settings.ini", NULL));
    gtk_box_pack_start(GTK_BOX(box), card, FALSE, FALSE, 10);
    return box;
}

static GtkWidget *make_page(const char *id)
{
    if (!strcmp(id, "display")) return make_display_page();
    if (!strcmp(id, "appearance")) return page_appearance_new();
    if (!strcmp(id, "input")) return make_input_page();
    if (!strcmp(id, "sound")) return page_sound_new();
    if (!strcmp(id, "network")) return page_network_new();
    if (!strcmp(id, "bluetooth")) return page_bluetooth_new();
    if (!strcmp(id, "windows")) return page_windows_new();
    if (!strcmp(id, "notifications")) return make_notifications_page();
    if (!strcmp(id, "power")) return make_power_page();
    if (!strcmp(id, "keyboard")) return page_keyboard_new();
    if (!strcmp(id, "users")) return make_users_page();
    return make_about_page();
}

/* ================= window frame ================= */
static void select_page(const char *id, GtkWidget *button)
{
    if (!content_stack || !sidebar) return;
    gtk_stack_set_visible_child_name(GTK_STACK(content_stack), id);
    GList *children = gtk_container_get_children(GTK_CONTAINER(sidebar));
    for (GList *l = children; l; l = l->next)
        if (GTK_IS_TOGGLE_BUTTON(l->data) && GTK_WIDGET(l->data) != button)
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(l->data), FALSE);
    g_list_free(children);
    for (guint i = 0; i < G_N_ELEMENTS(items); i++)
        if (!strcmp(items[i].id, id)) {
            gtk_label_set_text(GTK_LABEL(page_title), items[i].title);
            gtk_label_set_text(GTK_LABEL(page_subtitle), items[i].subtitle);
            break;
        }
}

static void cb_sidebar(GtkToggleButton *t, gpointer id)
{
    if (gtk_toggle_button_get_active(t)) select_page(id, GTK_WIDGET(t));
    else {
        /* do not allow deselecting the open page */
        const char *cur = content_stack ? gtk_stack_get_visible_child_name(GTK_STACK(content_stack)) : NULL;
        if (cur && !g_strcmp0(cur, id)) gtk_toggle_button_set_active(t, TRUE);
    }
}

/* "a|b|c": the first icon name present in the theme (newer Adwaita themes dropped many old names) */
static char *pick_icon(const char *spec)
{
    char **names = g_strsplit(spec, "|", -1);
    GtkIconTheme *t = gtk_icon_theme_get_default();
    char *res = NULL;
    for (int i = 0; names[i] && !res; i++)
        if (gtk_icon_theme_has_icon(t, names[i])) res = g_strdup(names[i]);
    if (!res) res = g_strdup(names[0]);
    g_strfreev(names);
    return res;
}

static GtkWidget *make_sidebar(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_size_request(box, 250, -1);
    gtk_container_set_border_width(GTK_CONTAINER(box), 14);
    gtk_style_context_add_class(gtk_widget_get_style_context(box), "sidebar");
    GtkWidget *brand = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(brand), "<b>Hyggshi Settings</b>");
    gtk_widget_set_halign(brand, GTK_ALIGN_START);
    gtk_widget_set_margin_start(brand, 10);
    gtk_widget_set_margin_bottom(brand, 15);
    gtk_box_pack_start(GTK_BOX(box), brand, FALSE, FALSE, 0);
    for (guint i = 0; i < G_N_ELEMENTS(items); i++) {
        GtkWidget *b = gtk_toggle_button_new();
        gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
        GtkWidget *r = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        char *icon = pick_icon(items[i].icon);
        GtkWidget *im = gtk_image_new_from_icon_name(icon, GTK_ICON_SIZE_BUTTON);
        g_free(icon);
        GtkWidget *l = gtk_label_new(items[i].title);
        gtk_widget_set_halign(l, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(r), im, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(r), l, TRUE, TRUE, 0);
        gtk_container_add(GTK_CONTAINER(b), r);
        g_object_set_data(G_OBJECT(b), "hde-id", (gpointer)items[i].id);
        g_signal_connect(b, "toggled", G_CALLBACK(cb_sidebar), (gpointer)items[i].id);
        gtk_box_pack_start(GTK_BOX(box), b, FALSE, FALSE, 0);
    }
    return box;
}

/* CSS based on the GTK theme colors (@theme_*), so it is right in both light and dark mode. */
static void load_css(void)
{
    HdeThemeInfo ti;
    hde_theme_info_load(&ti);
    const char *a = ti.accent;
    char *data = g_strdup_printf(
        ".sidebar { background-color: shade(@theme_bg_color, 0.96); border-right: 1px solid alpha(@theme_fg_color, 0.10); }"
        ".sidebar button { color: @theme_fg_color; padding: 9px 10px; border-radius: 10px; }"
        ".sidebar button:checked { background: %s; color: white; }"
        ".sidebar button:checked label, .sidebar button:checked image { color: white; }"
        ".page-heading { font-size: 24px; font-weight: 700; }"
        ".page-description { opacity: 0.7; font-size: 13px; }"
        ".section-title { color: %s; font-weight: 700; font-size: 12px; }"
        ".row-title { font-weight: 600; }"
        ".row-description { opacity: 0.68; font-size: 11px; }"
        ".about-title { font-size: 20px; font-weight: 700; }"
        ".status { opacity: 0.7; font-size: 11px; }"
        ".card { background-color: @theme_base_color; border: 1px solid alpha(@theme_fg_color, 0.12); border-radius: 12px; }"
        ".card > row { padding: 2px 8px; border-bottom: 1px solid alpha(@theme_fg_color, 0.07); }"
        ".card > row:last-child { border-bottom: none; }"
        ".badge { font-size: 10px; padding: 1px 8px; border-radius: 9px; background-color: alpha(%s, 0.16); color: %s; }"
        ".badge-ok { background-color: alpha(#2ec27e, 0.18); color: #26a269; }"
        ".error-text { color: #e01b24; }"
        ".style-card { padding: 8px; border-radius: 12px; background-image: none; }"
        ".style-card:checked { box-shadow: inset 0 0 0 2px %s; background-color: alpha(%s, 0.08); }"
        ".preview { border-radius: 8px; min-width: 150px; min-height: 92px; }"
        ".preview-light { background-color: #f6f7f9; border: 1px solid #d0d4da; }"
        ".preview-dark { background-color: #1e222a; border: 1px solid #3a4150; }"
        ".preview-light .pv-win { background-color: #ffffff; border: 1px solid #d8dbe0; border-radius: 5px; }"
        ".preview-dark .pv-win { background-color: #2b303a; border: 1px solid #3a4150; border-radius: 5px; }"
        ".preview-light .pv-bar { background-color: #e2e5ea; } .preview-dark .pv-bar { background-color: #14171c; }"
        ".pv-accent { background-color: %s; border-radius: 3px; }"
        "button { border-radius: 8px; }",
        a, a, a, a, a, a, a);
    if (!app_css) {
        app_css = gtk_css_provider_new();
        gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(app_css),
                                                  GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
    gtk_css_provider_load_from_data(app_css, data, -1, NULL);
    g_free(data);
    hde_theme_info_clear(&ti);
}

static void on_theme_changed(gpointer d)
{
    (void)d;
    load_css();
}

static void open_page_arg(const char *id)
{
    GList *ch = gtk_container_get_children(GTK_CONTAINER(sidebar));
    gboolean found = FALSE;
    for (GList *l = ch; l; l = l->next) {
        const char *pid = g_object_get_data(G_OBJECT(l->data), "hde-id");
        if (pid && !strcmp(pid, id)) {
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(l->data), TRUE);
            found = TRUE;
            break;
        }
    }
    g_list_free(ch);
    if (!found) settings_status("Unknown page '%s'", id);
}

static void on_activate(GtkApplication *app, gpointer d)
{
    (void)d;
    if (window) { gtk_window_present(GTK_WINDOW(window)); return; }
    window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), APP_NAME);
    gtk_window_set_default_size(GTK_WINDOW(window), 1020, 700);
    gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER);
    gtk_window_set_icon_name(GTK_WINDOW(window), "preferences-system");

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_container_add(GTK_CONTAINER(window), root);
    GtkWidget *sidebar_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_widget_set_size_request(sidebar_scroll, 250, -1);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sidebar_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    sidebar = make_sidebar();
    gtk_container_add(GTK_CONTAINER(sidebar_scroll), sidebar);
    gtk_box_pack_start(GTK_BOX(root), sidebar_scroll, FALSE, TRUE, 0);

    GtkWidget *main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(root), main_box, TRUE, TRUE, 0);
    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_container_set_border_width(GTK_CONTAINER(header), 24);
    page_title = gtk_label_new("");
    gtk_widget_set_halign(page_title, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(page_title), "page-heading");
    page_subtitle = gtk_label_new("");
    gtk_widget_set_halign(page_subtitle, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(page_subtitle), "page-description");
    gtk_box_pack_start(GTK_BOX(header), page_title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(header), page_subtitle, FALSE, FALSE, 2);
    gtk_box_pack_start(GTK_BOX(main_box), header, FALSE, FALSE, 0);

    status_label = gtk_label_new("Ready");
    content_stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(content_stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_stack_set_transition_duration(GTK_STACK(content_stack), 160);
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), content_stack);
    gtk_box_pack_start(GTK_BOX(main_box), scroll, TRUE, TRUE, 0);
    for (guint i = 0; i < G_N_ELEMENTS(items); i++)
        gtk_stack_add_named(GTK_STACK(content_stack), make_page(items[i].id), items[i].id);

    gtk_widget_set_halign(status_label, GTK_ALIGN_START);
    gtk_widget_set_margin_start(status_label, 28);
    gtk_widget_set_margin_bottom(status_label, 8);
    gtk_label_set_ellipsize(GTK_LABEL(status_label), PANGO_ELLIPSIZE_END);
    gtk_style_context_add_class(gtk_widget_get_style_context(status_label), "status");
    gtk_box_pack_start(GTK_BOX(main_box), status_label, FALSE, FALSE, 0);

    gtk_widget_show_all(window);
    open_page_arg("display");
    settings_status("Ready");
}

static int on_command_line(GApplication *app, GApplicationCommandLine *cl, gpointer d)
{
    (void)d;
    int argc = 0;
    char **argv = g_application_command_line_get_arguments(cl, &argc);
    g_application_activate(app);
    if (argc > 1 && argv[1][0] != '-') open_page_arg(argv[1]);
    g_strfreev(argv);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--apply")) {
        /* headless mode: called by hde-session at login */
        apply_keyboard_settings();
        apply_power_settings();
        apply_input_settings();
        return 0;
    }
    if (argc > 2 && !strcmp(argv[1], "--style")) {
        /* hde-settings --style dark|light|toggle : switch Dark mode without opening the window */
        gboolean dark;
        if (!strcmp(argv[2], "dark")) dark = TRUE;
        else if (!strcmp(argv[2], "light")) dark = FALSE;
        else if (!strcmp(argv[2], "toggle")) dark = cfg_get_int("theme_index", 0) != 2;
        else { fprintf(stderr, "hde-settings: --style expects dark, light or toggle\n"); return 2; }
        appearance_apply_style(dark);
        char *eff = cfg_get_string("gtk_theme_effective", "?");
        printf("%s mode applied (GTK theme %s)\n", dark ? "Dark" : "Light", eff);
        g_free(eff);
        return 0;
    }
    if (argc > 1 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) {
        printf("Usage: hde-settings [PAGE]   (display appearance input sound network bluetooth windows\n"
               "                             notifications power keyboard users about)\n"
               "       hde-settings --apply  re-apply login-time settings and exit\n"
               "       hde-settings --style dark|light|toggle   switch Dark mode without opening the window\n");
        return 0;
    }
    signal(SIGPIPE, SIG_IGN);
    gtk_init(&argc, &argv);
    hde_theme_apply_process();
    load_css();
    hde_theme_watch(on_theme_changed, NULL);

    /* A single window: `hde-settings bluetooth` while it is already open switches the page instead of opening a second window. */
    GtkApplication *app = gtk_application_new("org.hyggshi.Settings", G_APPLICATION_HANDLES_COMMAND_LINE);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    g_signal_connect(app, "command-line", G_CALLBACK(on_command_line), NULL);
    int rc = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return rc;
}
