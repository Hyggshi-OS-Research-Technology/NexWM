/* Hyggshi Settings — the GTK3 settings center of the Hyggshi Desktop Environment.
 *
 *   hde-settings              open the settings window
 *   hde-settings <page>       open a page directly: display appearance panel startmenu input sound network
 *                             bluetooth windows notifications power keyboard users about
 *   hde-settings --apply      re-apply the settings needed at every login, then exit (called by hde-session)
 *   hde-settings --touchpad-setup[=auto]   the "Touchpad scrolling" window (auto: only at the first login with a
 *                             touchpad, until a direction is chosen; run by hde-session)
 *   hde-settings --project    the Project window of F8 / Super+P (PC screen only, Duplicate, Extend, Second screen only)
 *   hde-settings --display-mode pc|duplicate|extend|second    --displays    --brightness [+N|-N|N]
 *   hde-settings --display-set NAME WxH[@HZ]|auto [normal|left|right|inverted]
 *   hde-settings --night-light [on|off|toggle]
 *   hde-settings --power      the battery, the battery saver and the low-battery warnings, as text
 *   hde-settings --about      this computer, the system and the memory HDE uses, as text
 *   hde-settings --about-window   the "About HDE" window (logo of the system, HDE version, credits)
 *   hde-settings --wayland-config [DIR] [--reload]   the labwc configuration of the "HDE (Wayland)" session
 *   hde-settings --version
 *
 * The big pages live in separate files: hde-settings-{network,bluetooth,appearance,windows,keyboard,sound,touchpad,
 * display,about,panel,power}.c
 */
#include "hde-settings.h"
#include "hde-theme.h"
#include "hde-input.h"
#include "hde-build.h"
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define APP_NAME "Hyggshi Settings"

static GtkWidget *window;
static GtkWidget *content_stack;
static GtkWidget *content_scroll;
static GtkWidget *page_title;
static GtkWidget *page_subtitle;
static GtkWidget *sidebar;
static GtkWidget *status_label;
static GtkCssProvider *app_css;

typedef struct { const char *id; const char *icon; const char *title; const char *subtitle; } SettingItem;
static const SettingItem items[] = {
    /* icon: several alternative names separated by '|'; the first one present in the icon theme is used */
    { "display", "video-display-symbolic|preferences-desktop-display-symbolic", "Display",
      "Brightness, screens, projector (F8) and Night Light" },
    { "appearance", "preferences-desktop-appearance-symbolic|preferences-desktop-theme-symbolic|applications-graphics-symbolic|weather-clear-night-symbolic",
      "Appearance", "Dark mode, theme, accent, icons and fonts" },
    { "panel", "view-continuous-symbolic|preferences-desktop-display-symbolic|view-dual-symbolic|window-maximize-symbolic",
      "Panel", "Position, size, items, pinned apps and extensions" },
    { "startmenu", "view-app-grid-symbolic|start-here-symbolic|view-grid-symbolic|applications-other-symbolic",
      "Start Menu", "Layout (modern, Kickoff, classic), favorites and the Start button" },
    { "input", "input-mouse-symbolic", "Input", "Mouse, touchpad and pointer" },
    { "sound", "audio-volume-high-symbolic", "Sound", "Output, input and volume" },
    { "network", "network-wireless-symbolic", "Network", "Wi-Fi networks, Ethernet and VPN" },
    { "bluetooth", "bluetooth-active-symbolic|bluetooth-symbolic", "Bluetooth", "Pair and connect Bluetooth devices" },
    { "windows", "preferences-system-windows-symbolic|focus-windows-symbolic|view-dual-symbolic|window-maximize-symbolic",
      "Window Management", "Window manager used by the desktop" },
    { "notifications", "preferences-system-notifications-symbolic|notification-symbolic", "Notifications", "Alerts and Do Not Disturb" },
    { "power", "battery-good-symbolic|battery-full-symbolic", "Power", "Battery, power mode, battery saver and sleep" },
    { "keyboard", "input-keyboard-symbolic", "Keyboard & Shortcuts", "Layouts, repeat, Super key, sound and display keys" },
    { "users", "system-users-symbolic|avatar-default-symbolic", "Users", "Accounts and administrator" },
    { "about", "help-about-symbolic", "About", "Hyggshi Desktop Environment and this computer" },
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

/* ---- HDE_DEBUG: screen positions of widgets, for the GUI tests ---- */
static GSList *geom_widgets;
static guint geom_timer;

static gboolean geom_log_all(gpointer d)
{
    (void)d;
    geom_timer = 0;
    for (GSList *l = geom_widgets; l; l = l->next) {
        GtkWidget *w = l->data;
        GtkWidget *top = gtk_widget_get_toplevel(w);
        GdkWindow *gw = gtk_widget_get_window(top);
        int ox = 0, oy = 0, x = 0, y = 0;
        if (!gtk_widget_get_mapped(w) || !gw || !gtk_widget_translate_coordinates(w, top, 0, 0, &x, &y)) continue;
        gdk_window_get_origin(gw, &ox, &oy);
        GtkAllocation a;
        gtk_widget_get_allocation(w, &a);
        fprintf(stderr, "hde-settings: widget %s at %d,%d %dx%d\n", (const char *)g_object_get_data(G_OBJECT(w), "hde-geom"),
                ox + x, oy + y, a.width, a.height);
    }
    return G_SOURCE_REMOVE;
}

static void geom_schedule(void)
{
    if (geom_timer) g_source_remove(geom_timer);
    geom_timer = g_timeout_add(400, geom_log_all, NULL);
}

static gboolean on_geom_configure(GtkWidget *w, GdkEvent *e, gpointer d)
{
    (void)w; (void)e; (void)d;
    geom_schedule();
    return FALSE;
}

static void on_geom_scrolled(GtkAdjustment *a, gpointer d)
{
    (void)a; (void)d;
    geom_schedule();
}

static void on_geom_map(GtkWidget *w, gpointer d)
{
    (void)d;
    GtkWidget *top = gtk_widget_get_toplevel(w);
    if (gtk_widget_is_toplevel(top) && !g_object_get_data(G_OBJECT(top), "hde-geom-top")) {
        g_object_set_data(G_OBJECT(top), "hde-geom-top", GINT_TO_POINTER(1));
        g_signal_connect(top, "configure-event", G_CALLBACK(on_geom_configure), NULL);
    }
    /* the position also changes when the page around the widget is scrolled */
    GtkWidget *parent = gtk_widget_get_parent(w);
    GtkWidget *sw = parent ? gtk_widget_get_ancestor(parent, GTK_TYPE_SCROLLED_WINDOW) : NULL;
    GtkAdjustment *adj = sw ? gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(sw)) : NULL;
    if (adj && !g_object_get_data(G_OBJECT(adj), "hde-geom-adj")) {
        g_object_set_data(G_OBJECT(adj), "hde-geom-adj", GINT_TO_POINTER(1));
        g_signal_connect(adj, "value-changed", G_CALLBACK(on_geom_scrolled), NULL);
    }
    geom_schedule();
}

static void on_geom_destroy(GtkWidget *w, gpointer d)
{
    (void)d;
    geom_widgets = g_slist_remove(geom_widgets, w);
}

void debug_geometry_watch(GtkWidget *w, const char *name)
{
    if (!getenv("HDE_DEBUG") || !w || !name) return;
    g_object_set_data_full(G_OBJECT(w), "hde-geom", g_strdup(name), g_free);
    geom_widgets = g_slist_prepend(geom_widgets, w);
    g_signal_connect(w, "map", G_CALLBACK(on_geom_map), NULL);
    g_signal_connect(w, "destroy", G_CALLBACK(on_geom_destroy), NULL);
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
    gsettings_sync_input();                 /* Mutter / Muffin apply GNOME's values themselves: the same ones */
    Display *dpy = hde_input_open();
    if (!dpy) return;
    hde_input_apply(dpy, -1, &p, "hde-settings: input");
    XCloseDisplay(dpy);
}

/* ================= simple pages ================= */
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

static guint input_apply_id;
static GtkWidget *input_devices_card;
static GtkWidget *input_service_note;
static GtkWidget *input_no_touchpad_note;
static GSList *input_switches;          /* GtkSwitch* of the Input page, data "hde-key" / "hde-default" */

static void on_as_touchpad_toggled(GtkToggleButton *b, gpointer d)
{
    (void)d;
    touchpad_use_device(g_object_get_data(G_OBJECT(b), "hde-device"),
                        GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "hde-device-id")), gtk_toggle_button_get_active(b));
}

/* The touchpads and mice HDE configures, with their state right now (refreshed after every change and hotplug).
 * A mouse can be marked "It is a touchpad": a touchpad that X sees as a mouse (inside a virtual machine, or in mouse
 * mode) then follows the touchpad direction instead of the mouse wheel's. */
static void input_devices_refresh(void)
{
    if (!input_devices_card) return;
    card_clear(input_devices_card);
    HdeInputDevice devs[16];
    HdeInputPrefs p;
    hde_input_prefs_load(&p);
    int n = 0, touchpads = 0;
    gboolean have_x = FALSE, service = TRUE;
    Display *dpy = hde_input_open();
    if (dpy) {
        have_x = TRUE;
        n = hde_input_list(dpy, devs, (int)G_N_ELEMENTS(devs));
        service = hde_input_service_running(dpy);
        XCloseDisplay(dpy);
    }
    for (int i = 0; i < n; i++) {
        const HdeInputDevice *d = &devs[i];
        gboolean follows = hde_input_follows_touchpad(d, &p);
        if (follows) touchpads++;
        GString *desc = g_string_new(d->kind == HDE_INPUT_TOUCHPAD ? "Touchpad" : follows ? "Mouse used as the touchpad"
                                                                                        : "Mouse");
        g_string_append_printf(desc, " · %s driver", d->driver);
        if (d->natural >= 0 && follows)
            g_string_append_printf(desc, " · scrolls %s", d->natural ? "like a phone" : "like a mouse wheel");
        else if (d->natural >= 0)
            g_string_append_printf(desc, " · natural scrolling %s", d->natural ? "on" : "off");
        if (d->tapping >= 0) g_string_append_printf(desc, " · tap to click %s", d->tapping ? "on" : "off");
        if (getenv("HDE_DEBUG")) fprintf(stderr, "hde-settings: input device: %s: %s\n", d->name, desc->str);
        GtkWidget *control = NULL;
        if (d->kind != HDE_INPUT_TOUCHPAD) {
            control = gtk_check_button_new_with_label("It is a touchpad");
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(control), follows);
            gtk_widget_set_valign(control, GTK_ALIGN_CENTER);
            gtk_widget_set_tooltip_text(control, "For a touchpad that HDE sees as a mouse: the touchpad of your computer "
                                                 "inside a virtual machine, or a touchpad working in mouse mode. It then "
                                                 "scrolls the way chosen under Touchpad, not like the mouse wheel.");
            g_object_set_data_full(G_OBJECT(control), "hde-device", g_strdup(d->name), g_free);
            g_object_set_data(G_OBJECT(control), "hde-device-id", GINT_TO_POINTER(d->id));
            g_signal_connect(control, "toggled", G_CALLBACK(on_as_touchpad_toggled), NULL);
            char *gname = g_strdup_printf("as-touchpad:%s", d->name);
            debug_geometry_watch(control, gname);
            g_free(gname);
        }
        gtk_container_add(GTK_CONTAINER(input_devices_card), row_box(d->name, desc->str, control));
        g_string_free(desc, TRUE);
    }
    if (n == 0)
        gtk_container_add(GTK_CONTAINER(input_devices_card), card_placeholder(
            !hde_input_supported() ? "HDE was built without libxi-dev, so these settings cannot be applied. "
                                     "Install it (sudo apt install libxi-dev) and rebuild HDE."
                                   : "No touchpad or mouse found that uses the libinput, synaptics or evdev X driver "
                                     "(package xserver-xorg-input-libinput)."));
    gtk_widget_show_all(input_devices_card);
    if (input_service_note) gtk_widget_set_visible(input_service_note, have_x && !service && n > 0);
    if (input_no_touchpad_note) gtk_widget_set_visible(input_no_touchpad_note, n > 0 && touchpads == 0);
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

void input_page_refresh(void)
{
    if (input_devices_card) input_apply_later();
}

/* a device was plugged in / unplugged while the page is open (hde-xsettings configures it; show it a bit later) */
static void on_seat_device_changed(GdkSeat *seat, GdkDevice *device, gpointer data)
{
    (void)seat; (void)device; (void)data;
    input_apply_later();
}

static gboolean cb_input_bool(GtkSwitch *s, gboolean v, gpointer key)
{
    (void)s;
    cfg_set_bool(key, v);
    input_apply_later();
    return FALSE;
}

/* every time the page is shown: show what settings.ini says now (it may have been changed elsewhere, e.g. by the
 * Touchpad scrolling window at login), make sure the devices match the page (also without hde-xsettings, or if
 * something else changed them), then list them */
static void on_input_page_map(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    touchpad_direction_sync();
    for (GSList *l = input_switches; l; l = l->next) {
        GtkSwitch *sw = l->data;
        const char *key = g_object_get_data(G_OBJECT(sw), "hde-key");
        gboolean v = cfg_get_bool(key, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(sw), "hde-default")));
        if (gtk_switch_get_active(sw) == v) continue;
        g_signal_handlers_block_by_func(sw, G_CALLBACK(cb_input_bool), (gpointer)key);
        gtk_switch_set_active(sw, v);
        g_signal_handlers_unblock_by_func(sw, G_CALLBACK(cb_input_bool), (gpointer)key);
    }
    input_apply_later();
}

static void on_input_page_destroy(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    input_devices_card = NULL;
    input_service_note = NULL;
    input_no_touchpad_note = NULL;
    g_slist_free(input_switches);
    input_switches = NULL;
    GdkDisplay *dsp = gdk_display_get_default();
    GdkSeat *seat = dsp ? gdk_display_get_default_seat(dsp) : NULL;
    if (seat) g_signal_handlers_disconnect_by_func(seat, G_CALLBACK(on_seat_device_changed), NULL);
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
    g_object_set_data(G_OBJECT(s), "hde-key", (gpointer)key);
    g_object_set_data(G_OBJECT(s), "hde-default", GINT_TO_POINTER(def));
    g_signal_connect(s, "state-set", G_CALLBACK(cb_input_bool), (gpointer)key);
    input_switches = g_slist_prepend(input_switches, s);
    return s;
}

static void cb_touchpad_try(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    touchpad_setup_show(GTK_WINDOW(window));
}

static GtkWidget *make_input_page(void)
{
    GtkWidget *box = page_base();
    /* defaults here = defaults in hde-input.c */
    gtk_box_pack_start(GTK_BOX(box), section("Touchpad"), FALSE, FALSE, 0);
    GtkWidget *try_btn = gtk_button_new_with_mnemonic("_Try both…");
    g_signal_connect(try_btn, "clicked", G_CALLBACK(cb_touchpad_try), NULL);
    debug_geometry_watch(try_btn, "input-try");
    gtk_box_pack_start(GTK_BOX(box), row_box("Scroll direction",
        "Which way the page moves when you scroll with two fingers. Not sure? Try both on a test page.", try_btn),
        FALSE, FALSE, 0);
    GtkWidget *cards = touchpad_direction_cards("input");
    gtk_widget_set_margin_start(cards, 6);
    gtk_widget_set_margin_bottom(cards, 6);
    gtk_box_pack_start(GTK_BOX(box), cards, FALSE, FALSE, 0);
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
    input_no_touchpad_note = info_label("No touchpad found, only a mouse. If your touchpad scrolls the wrong way, HDE "
                                        "sees it as a mouse — usual inside a virtual machine, or for a touchpad in "
                                        "mouse mode: tick “It is a touchpad” next to it, then pick the direction above.");
    gtk_widget_set_no_show_all(input_no_touchpad_note, TRUE);
    gtk_box_pack_start(GTK_BOX(box), input_no_touchpad_note, FALSE, FALSE, 0);
    input_service_note = info_label("HDE's input service (hde-xsettings) is not running in this session: the choices "
                                    "above apply now, but a touchpad or mouse plugged in later, or back after "
                                    "suspend, keeps its old settings. Log out and log in again.");
    gtk_style_context_add_class(gtk_widget_get_style_context(input_service_note), "error-text");
    gtk_widget_set_no_show_all(input_service_note, TRUE);
    gtk_box_pack_start(GTK_BOX(box), input_service_note, FALSE, FALSE, 0);
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

static GtkWidget *make_page(const char *id)
{
    if (!strcmp(id, "display")) return page_display_new();
    if (!strcmp(id, "appearance")) return page_appearance_new();
    if (!strcmp(id, "panel")) return page_panel_new();
    if (!strcmp(id, "startmenu")) return page_startmenu_new();
    if (!strcmp(id, "input")) return make_input_page();
    if (!strcmp(id, "sound")) return page_sound_new();
    if (!strcmp(id, "network")) return page_network_new();
    if (!strcmp(id, "bluetooth")) return page_bluetooth_new();
    if (!strcmp(id, "windows")) return page_windows_new();
    if (!strcmp(id, "notifications")) return make_notifications_page();
    if (!strcmp(id, "power")) return page_power_new();
    if (!strcmp(id, "keyboard")) return page_keyboard_new();
    if (!strcmp(id, "users")) return make_users_page();
    return page_about_new();
}

/* ================= window frame ================= */
static void select_page(const char *id, GtkWidget *button)
{
    if (!content_stack || !sidebar) return;
    gtk_stack_set_visible_child_name(GTK_STACK(content_stack), id);
    /* All pages share one scrolled area: start every page at its top (it used to keep the scroll position of the
     * previous page, so e.g. Input or Appearance opened scrolled down, with their first sections out of sight). */
    if (content_scroll)
        gtk_adjustment_set_value(gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(content_scroll)), 0);
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
    /* no background of its own: the scrolled window around it has the sidebar's (see on_activate) */
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_size_request(box, 250, -1);
    gtk_container_set_border_width(GTK_CONTAINER(box), 14);
    gtk_style_context_add_class(gtk_widget_get_style_context(box), "sidebar-items");
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
    /* a chosen accent also goes on the switches, sliders, selections the GTK theme draws (Automatic: they have it) */
    char *accent_css = hde_theme_accent_css(&ti);
    if (getenv("HDE_DEBUG")) {
        char *gt = NULL;
        g_object_get(gtk_settings_get_default(), "gtk-theme-name", &gt, NULL);
        fprintf(stderr, "hde-settings: accent: %s %s (GTK theme %s)\n", ti.accent_auto ? "automatic" : "chosen", a,
                gt ? gt : "?");
        g_free(gt);
    }
    char *data = g_strdup_printf(
        ".sidebar { background-color: shade(@theme_bg_color, 0.96); border-right: 1px solid alpha(@theme_fg_color, 0.10); }"
        ".sidebar viewport, .sidebar-items { background: none; border: none; }"
        ".sidebar button { color: @theme_fg_color; padding: 9px 10px; border-radius: 10px; }"
        ".sidebar button:checked { background: %s; color: white; }"
        ".sidebar button:checked label, .sidebar button:checked image { color: white; }"
        ".page-heading { font-size: 24px; font-weight: 700; }"
        ".page-description { opacity: 0.7; font-size: 13px; }"
        ".section-title { color: %s; font-weight: 700; font-size: 12px; }"
        ".row-title { font-weight: 600; }"
        ".row-description { opacity: 0.68; font-size: 11px; }"
        ".about-title { font-size: 24px; font-weight: 700; }"
        ".about-os-small { font-size: 18px; font-weight: 700; }"
        ".about-hde { font-size: 15px; font-weight: 700; }"
        ".about-base { font-weight: 600; }"
        ".about-link label { color: %s; }"
        ".mem-big { font-size: 16px; font-weight: 700; }"
        ".project-tile { padding: 8px 6px; border-radius: 12px; background-image: none; }"
        ".project-tile.selected { box-shadow: inset 0 0 0 2px %s; background-color: alpha(%s, 0.10); }"
        ".project-tile:focus { outline-color: alpha(%s, 0.6); }"
        ".project-window { border: 1px solid alpha(@theme_fg_color, 0.28); }"
        ".tp-heading { font-size: 20px; font-weight: 700; }"
        ".tp-hint { padding: 8px 10px; border-radius: 10px; background-color: alpha(%s, 0.10); border: 1px solid alpha(%s, 0.35); }"
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
        "button { border-radius: 8px; }"
        "%s",
        a, a, a, a, a, a, a, a, a, a, a, a, a, accent_css);
    if (!app_css) {
        app_css = gtk_css_provider_new();
        gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(app_css),
                                                  GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
    gtk_css_provider_load_from_data(app_css, data, -1, NULL);
    g_free(data);
    g_free(accent_css);
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
    /* The sidebar's background is on the scrolled window, not on the box of buttons inside it. GtkViewport draws its
     * child through a pixel cache that it makes opaque (no alpha channel) as soon as the child has an opaque
     * background, and the child's border (border_width 14) is never painted in it: with the background on the box,
     * that border showed as a black frame around the sidebar, in light and dark mode alike. */
    GtkWidget *sidebar_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_style_context_add_class(gtk_widget_get_style_context(sidebar_scroll), "sidebar");
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
    gtk_stack_set_vhomogeneous(GTK_STACK(content_stack), FALSE);     /* each page as tall as its content */
    GtkWidget *scroll = content_scroll = gtk_scrolled_window_new(NULL, NULL);
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
    if (argc > 1 && !strcmp(argv[1], "--version")) {
        printf("hde-settings (HDE) %s %s\n", HDE_RELEASE, HDE_VERSION);
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "--about")) return about_cli();
    if (argc > 1 && !strcmp(argv[1], "--wayland-config")) return wayland_config_cli(argc, argv);
    if (argc > 1 && !strcmp(argv[1], "--about-window")) {
        signal(SIGPIPE, SIG_IGN);
        gtk_init(&argc, &argv);
        hde_theme_apply_process();
        load_css();
        hde_theme_watch(on_theme_changed, NULL);
        return about_window_main();
    }
    if (argc > 1 && !strcmp(argv[1], "--displays")) return display_cli_displays();
    if (argc > 1 && !strcmp(argv[1], "--power")) return power_cli();
    if (argc > 1 && (!strcmp(argv[1], "--display-mode") || g_str_has_prefix(argv[1], "--display-mode="))) {
        const char *m = argv[1][14] == '=' ? argv[1] + 15 : argc > 2 ? argv[2] : NULL;
        if (!m) { fprintf(stderr, "hde-settings: --display-mode expects pc, duplicate, extend or second\n"); return 2; }
        return display_cli_mode(m);
    }
    if (argc > 1 && !strcmp(argv[1], "--display-set"))
        return display_cli_set(argc > 2 ? argv[2] : NULL, argc > 3 ? argv[3] : NULL, argc > 4 ? argv[4] : NULL);
    if (argc > 1 && (!strcmp(argv[1], "--brightness") || g_str_has_prefix(argv[1], "--brightness="))) {
        const char *v = argv[1][12] == '=' ? argv[1] + 13 : argc > 2 ? argv[2] : NULL;
        return display_cli_brightness(v);
    }
    if (argc > 1 && (!strcmp(argv[1], "--night-light") || g_str_has_prefix(argv[1], "--night-light="))) {
        const char *v = argv[1][13] == '=' ? argv[1] + 14 : argc > 2 ? argv[2] : NULL;
        return display_cli_night_light(v);
    }
    if (argc > 1 && !strcmp(argv[1], "--project")) {
        /* F8 / Super+P / the display key (hde-hotkeys), or a screen just plugged in (hde-xsettings) */
        signal(SIGPIPE, SIG_IGN);
        gdk_set_allowed_backends("x11");
        if (!gtk_init_check(&argc, &argv)) {
            fprintf(stderr, "hde-settings: cannot open the X display\n");
            return 2;
        }
        hde_theme_apply_process();
        load_css();
        return display_project_main(argc, argv);
    }
    if (argc > 1 && g_str_has_prefix(argv[1], "--touchpad-setup")) {
        /* the "Touchpad scrolling" window on its own; =auto (hde-session at login): only if it is needed */
        if (!strcmp(argv[1], "--touchpad-setup=auto") && !touchpad_setup_needed()) return 0;
        signal(SIGPIPE, SIG_IGN);
        gtk_init(&argc, &argv);
        hde_theme_apply_process();
        load_css();
        hde_theme_watch(on_theme_changed, NULL);
        touchpad_setup_show(NULL);
        gtk_main();
        return 0;
    }
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
        printf("Usage: hde-settings [PAGE]   (display appearance panel startmenu input sound network bluetooth\n"
               "                             windows notifications power keyboard users about)\n"
               "       hde-settings --apply  re-apply login-time settings and exit\n"
               "       hde-settings --touchpad-setup   choose which way the touchpad scrolls, with a test page\n"
               "       hde-settings --project          the Project window (F8): PC screen only, Duplicate, Extend,\n"
               "                                       Second screen only\n"
               "       hde-settings --display-mode pc|duplicate|extend|second   the same without a window\n"
               "       hde-settings --displays         the screens, the layout in use and the brightness method\n"
               "       hde-settings --display-set NAME WxH[@HZ]|auto [normal|left|right|inverted]\n"
               "                                       the resolution / rotation of a screen (kept for next time)\n"
               "       hde-settings --brightness [+N|-N|N]   show or change the screen brightness\n"
               "       hde-settings --night-light [on|off|toggle]   show or switch Night Light (X11)\n"
               "       hde-settings --power            the battery, the battery saver and the low-battery warnings\n"
               "       hde-settings --about            this computer, the system and the memory HDE uses\n"
               "       hde-settings --about-window     the About HDE window\n"
               "       hde-settings --wayland-config [DIR] [--reload]   write labwc's configuration (Wayland session)\n"
               "       hde-settings --version\n"
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
