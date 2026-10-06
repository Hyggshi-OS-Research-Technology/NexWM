/* hde-panel: the panel — Start menu, pinned apps, taskbar, workspaces, extensions, tray, status, notifications, clock.
 *
 * Everything is set in Settings > Panel and Settings > Start Menu (src/hde-panel-config.h) and follows changes live:
 * bottom or top of the screen, height, transparency, which items are shown, pinned apps, extensions, the clock, the
 * Start button and the Start menu layout (modern = Linux Mint style, kickoff = KDE style, classic drop-down).
 *
 * Controlled from other processes:
 *   hde-panel --menu | --search | --run | --power | --osd-volume | --refresh
 * through an X ClientMessage on X11 (hde-ipc.h, used by hde-hotkeys for the Super key and the OSDs) and on D-Bus
 * everywhere (org.hyggshi.HDE.Panel; the Wayland session's key bindings use it).
 * On Wayland (the "HDE (Wayland)" session) the panel is a layer-shell surface and the taskbar uses
 * wlr-foreign-toplevel-management (src/hde-wltaskbar.c) instead of libwnck.
 */
#define WNCK_I_KNOW_THIS_IS_UNSTABLE 1
#include <gtk/gtk.h>
#include <gio/gdesktopappinfo.h>
#include <glib-unix.h>
#include <gdk/gdkx.h>
#include <libwnck/libwnck.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "hde-tray.h"
#include "hde-status.h"
#include "hde-notify.h"
#include "hde-osd.h"
#include "hde-search.h"
#include "hde-theme.h"
#include "hde-ipc.h"
#include "hde-panel-config.h"
#include "hde-startmenu.h"
#include "hde-applets.h"
#include "hde-osinfo.h"
#include "hde-wl.h"
#ifdef HAVE_WAYLAND_TASKBAR
#include "hde-wltaskbar.h"
#endif

#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

#define PANEL_DBUS_NAME  "org.hyggshi.HDE.Panel"
#define PANEL_DBUS_PATH  "/org/hyggshi/HDE/Panel"
#define PANEL_DBUS_IFACE "org.hyggshi.HDE.Panel"

typedef struct {
    const char *title;
    const char *icon;
    const char *cats[5];
} Category;

static const Category categories[] = {
    /* icon: alternative names separated by '|' (newer Adwaita themes dropped many applications-* names) */
    { "Internet",       "applications-internet|web-browser|emblem-web|network-workgroup|web-browser-symbolic",
                        { "Network", "WebBrowser", "Email", NULL } },
    { "Office",         "applications-office|x-office-document|x-office-document-symbolic", { "Office", NULL } },
    { "Graphics",       "applications-graphics|image-x-generic|applications-graphics-symbolic", { "Graphics", NULL } },
    { "Audio & Video",  "applications-multimedia|audio-x-generic|applications-multimedia-symbolic",
                        { "AudioVideo", "Audio", "Video", NULL } },
    { "Programming",    "applications-development|applications-engineering|text-x-script|utilities-terminal|applications-engineering-symbolic",
                        { "Development", NULL } },
    { "Games",          "applications-games|input-gaming|applications-games-symbolic", { "Game", NULL } },
    { "Utilities",      "applications-utilities|applications-accessories|applications-utilities-symbolic", { "Utility", NULL } },
    { "System",         "applications-system|preferences-system|applications-system-symbolic", { "System", "Settings", NULL } },
};
#define N_CATS G_N_ELEMENTS(categories)

static HdePanelConfig pcfg;
static GtkWidget *panel_win, *panel_box;
static GtkWidget *menu_btn, *menu_btn_img, *menu_btn_lbl, *desk_btn, *run_btn, *launchers, *tasks, *pager, *tray,
                 *status_area, *notify_btn, *applets, *clock_btn, *clock_label, *date_label;
static GtkWidget *app_menu;
static GtkCssProvider *panel_css;
static Atom cmd_atom;
static char *applets_sig;
static gboolean debug_on;          /* HDE_DEBUG=1: write diagnostic logs to stderr (~/.cache/hde/session.log) */
#define DBG(...) do { if (debug_on) { g_printerr("hde-panel: " __VA_ARGS__); g_printerr("\n"); } } while (0)

static void run_cmd(GtkMenuItem *item, gpointer cmd);
static void show_power_dialog(GtkMenuItem *item, gpointer data);
static void on_run_command(GtkButton *btn, gpointer data);
static gboolean panel_place(gpointer d);

static int icon_px(void) { return CLAMP(pcfg.size * 10 / 17, 14, 36); }   /* 20 px icons in a 34 px panel */

/* ---------- clock + calendar ---------- */
static GtkWidget *cal_win, *cal_title, *cal;

static gboolean update_clock(gpointer data)
{
    (void)data;
    GDateTime *now = g_date_time_new_now_local();
    const char *tf = pcfg.clock_24h ? (pcfg.clock_seconds ? "%H:%M:%S" : "%H:%M")
                                    : (pcfg.clock_seconds ? "%l:%M:%S %p" : "%l:%M %p");
    gboolean two_lines = pcfg.clock_date && pcfg.size >= 30;
    char *time_s = g_date_time_format(now, tf);
    char *date_s = g_date_time_format(now, "%a  %d/%m/%Y");
    char *tip = g_date_time_format(now, "%A, %d %B %Y");
    g_strstrip(time_s);
    if (pcfg.clock_date && !two_lines) {          /* a thin panel: everything on one line */
        char *d = g_date_time_format(now, "%d/%m");
        char *one = g_strdup_printf("%s  %s", time_s, d);
        gtk_label_set_text(GTK_LABEL(clock_label), one);
        g_free(one);
        g_free(d);
    } else gtk_label_set_text(GTK_LABEL(clock_label), time_s);
    gtk_label_set_text(GTK_LABEL(date_label), date_s);
    gtk_widget_set_visible(date_label, two_lines);
    gtk_widget_set_tooltip_text(clock_btn, tip);
    g_free(time_s); g_free(date_s); g_free(tip);
    g_date_time_unref(now);
    return G_SOURCE_CONTINUE;
}

static gboolean cal_focus_out(GtkWidget *w, GdkEventFocus *e, gpointer d)
{
    (void)e; (void)d;
    gtk_widget_hide(w);
    return FALSE;
}

static gboolean cal_key(GtkWidget *w, GdkEventKey *e, gpointer d)
{
    (void)d;
    if (e->keyval == GDK_KEY_Escape) { gtk_widget_hide(w); return TRUE; }
    return FALSE;
}

static gboolean cal_map(GtkWidget *w, GdkEvent *e, gpointer d)
{
    (void)e; (void)d;
    hde_window_force_activate(w, gtk_get_current_event_time());
    return FALSE;
}

static void toggle_calendar(GtkButton *b, gpointer d)
{
    (void)d;
    if (cal_win && gtk_widget_get_visible(cal_win)) { gtk_widget_hide(cal_win); return; }
    if (!cal_win) {
        cal_win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        gtk_window_set_title(GTK_WINDOW(cal_win), "Calendar");
        gtk_window_set_decorated(GTK_WINDOW(cal_win), FALSE);
        gtk_window_set_skip_taskbar_hint(GTK_WINDOW(cal_win), TRUE);
        gtk_window_set_skip_pager_hint(GTK_WINDOW(cal_win), TRUE);
        gtk_window_set_keep_above(GTK_WINDOW(cal_win), TRUE);
        gtk_window_set_resizable(GTK_WINDOW(cal_win), FALSE);
        gtk_window_set_type_hint(GTK_WINDOW(cal_win), GDK_WINDOW_TYPE_HINT_DIALOG);
        gtk_style_context_add_class(gtk_widget_get_style_context(cal_win), "hde-calendar");
        hde_wl_layer_init(GTK_WINDOW(cal_win), "hde-calendar", HDE_LAYER_TOP, HDE_EDGE_RIGHT, HDE_KB_ON_DEMAND);
        g_signal_connect(cal_win, "focus-out-event", G_CALLBACK(cal_focus_out), NULL);
        g_signal_connect(cal_win, "key-press-event", G_CALLBACK(cal_key), NULL);
        g_signal_connect(cal_win, "map-event", G_CALLBACK(cal_map), NULL);
        g_signal_connect(cal_win, "delete-event", G_CALLBACK(gtk_widget_hide_on_delete), NULL);
        hde_popup_setup_alpha(cal_win);
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        gtk_container_set_border_width(GTK_CONTAINER(box), 12);
        cal_title = gtk_label_new("");
        gtk_style_context_add_class(gtk_widget_get_style_context(cal_title), "cal-title");
        gtk_label_set_xalign(GTK_LABEL(cal_title), 0);
        cal = gtk_calendar_new();
        gtk_box_pack_start(GTK_BOX(box), cal_title, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(box), cal, FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(cal_win), box);
        gtk_widget_show_all(box);
    }
    GDateTime *now = g_date_time_new_now_local();
    char *title = g_date_time_format(now, "%A, %d %B %Y");
    gtk_label_set_text(GTK_LABEL(cal_title), title);
    gtk_calendar_select_month(GTK_CALENDAR(cal), g_date_time_get_month(now) - 1, g_date_time_get_year(now));
    gtk_calendar_select_day(GTK_CALENDAR(cal), g_date_time_get_day_of_month(now));
    gtk_calendar_clear_marks(GTK_CALENDAR(cal));
    gtk_calendar_mark_day(GTK_CALENDAR(cal), g_date_time_get_day_of_month(now));
    g_free(title);
    g_date_time_unref(now);

    GtkRequisition nat;
    gtk_widget_get_preferred_size(cal_win, NULL, &nat);
    if (hde_wl_is_layer(GTK_WINDOW(cal_win))) {
        /* next to the clock, on the panel's side of the screen (the panel's exclusive zone keeps it clear) */
        hde_wl_layer_edges(GTK_WINDOW(cal_win), HDE_EDGE_RIGHT | (pcfg.top ? HDE_EDGE_TOP : HDE_EDGE_BOTTOM));
        hde_wl_layer_margins(GTK_WINDOW(cal_win), 0, 6, 6, 6);
    } else {
        GdkWindow *pw = gtk_widget_get_window(panel_win);
        int ox = 0, oy = 0;
        gdk_window_get_origin(pw, &ox, &oy);
        GtkAllocation a;
        gtk_widget_get_allocation(GTK_WIDGET(b), &a);
        int x = ox + a.x + a.width - nat.width;
        if (x < ox) x = ox;
        gtk_window_move(GTK_WINDOW(cal_win), x, pcfg.top ? oy + gdk_window_get_height(pw) + 6 : oy - nat.height - 6);
    }
    gtk_widget_show(cal_win);
    gtk_window_present_with_time(GTK_WINDOW(cal_win), gtk_get_current_event_time());
}

/* ---------- classic Start menu (menu_style=classic): the drop-down GtkMenu ---------- */
static char *pick_icon(const char *spec)
{
    char **names = g_strsplit(spec ? spec : "application-x-executable", "|", -1);
    GtkIconTheme *t = gtk_icon_theme_get_default();
    char *res = NULL;
    for (int i = 0; names[i] && !res; i++)
        if (gtk_icon_theme_has_icon(t, names[i])) res = g_strdup(names[i]);
    if (!res) res = g_strdup(names[0]);
    g_strfreev(names);
    return res;
}

static GtkWidget *make_item(const char *label, GIcon *gicon, const char *icon_name)
{
    GtkWidget *item = gtk_menu_item_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    char *picked = gicon ? NULL : pick_icon(icon_name ? icon_name : "application-x-executable");
    GtkWidget *img = gicon ? gtk_image_new_from_gicon(gicon, GTK_ICON_SIZE_LARGE_TOOLBAR)
                           : gtk_image_new_from_icon_name(picked, GTK_ICON_SIZE_LARGE_TOOLBAR);
    g_free(picked);
    gtk_image_set_pixel_size(GTK_IMAGE(img), 22);
    GtkWidget *lbl = gtk_label_new(label);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0);
    gtk_box_pack_start(GTK_BOX(box), img, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), lbl, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(item), box);
    return item;
}

static void on_app_activate(GtkMenuItem *item, gpointer data)
{
    (void)data;
    GAppInfo *app = g_object_get_data(G_OBJECT(item), "app");
    GdkAppLaunchContext *ctx = gdk_display_get_app_launch_context(gdk_display_get_default());
    gdk_app_launch_context_set_timestamp(ctx, gtk_get_current_event_time());
    GError *err = NULL;
    if (!g_app_info_launch(app, NULL, G_APP_LAUNCH_CONTEXT(ctx), &err)) {
        g_printerr("hde-panel: %s\n", err->message);
        g_clear_error(&err);
    }
    g_object_unref(ctx);
}

static gboolean app_in_category(GAppInfo *app, const Category *c)
{
    const char *cats = g_desktop_app_info_get_categories(G_DESKTOP_APP_INFO(app));
    if (!cats) return FALSE;
    char *wrapped = g_strdup_printf(";%s;", cats);
    gboolean found = FALSE;
    for (int i = 0; c->cats[i] && !found; i++) {
        char *needle = g_strdup_printf(";%s;", c->cats[i]);
        found = strstr(wrapped, needle) != NULL;
        g_free(needle);
    }
    g_free(wrapped);
    return found;
}

static gint cmp_app(gconstpointer a, gconstpointer b)
{
    return g_utf8_collate(g_app_info_get_display_name((GAppInfo *)a),
                          g_app_info_get_display_name((GAppInfo *)b));
}

static void add_app(GtkWidget *menu, GAppInfo *app)
{
    GtkWidget *it = make_item(g_app_info_get_display_name(app), g_app_info_get_icon(app), NULL);
    const char *desc = g_app_info_get_description(app);
    if (desc && *desc) gtk_widget_set_tooltip_text(it, desc);
    g_object_set_data_full(G_OBJECT(it), "app", g_object_ref(app), g_object_unref);
    g_signal_connect(it, "activate", G_CALLBACK(on_app_activate), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), it);
}

/* Typing while the menu is open -> switch to the app search box (like the Windows Start menu). */
static gboolean on_menu_key(GtkWidget *w, GdkEventKey *e, gpointer d)
{
    (void)w; (void)d;
    if (e->state & (GDK_CONTROL_MASK | GDK_MOD1_MASK | GDK_SUPER_MASK)) return FALSE;
    gunichar c = gdk_keyval_to_unicode(e->keyval);
    if (!c || !g_unichar_isprint(c) || g_unichar_isspace(c)) return FALSE;
    char buf[8] = { 0 };
    g_unichar_to_utf8(c, buf);
    guint32 t = e->time;
    if (app_menu) gtk_menu_shell_deactivate(GTK_MENU_SHELL(app_menu));
    hde_search_show(buf, t);
    return TRUE;
}

static GtkWidget *new_menu(void)
{
    GtkWidget *m = gtk_menu_new();
    g_signal_connect(m, "key-press-event", G_CALLBACK(on_menu_key), NULL);
    return m;
}

static void on_search_item(GtkMenuItem *item, gpointer data)
{
    (void)item; (void)data;
    hde_search_show(NULL, gtk_get_current_event_time());
}

static void on_settings_item(GtkMenuItem *item, gpointer data)
{
    (void)item; (void)data;
    hde_open_settings(NULL);
}

/* Custom launchers in ~/.config/hde/start-apps/ appear in the Custom submenu. */
static void add_custom_start_apps(GtkWidget *menu)
{
    char *dir = g_build_filename(g_get_user_config_dir(), "hde", "start-apps", NULL);
    GDir *d = g_dir_open(dir, 0, NULL);
    if (!d) {
        g_free(dir);
        return;
    }
    GtkWidget *sub = new_menu();
    int count = 0;
    const char *name;
    while ((name = g_dir_read_name(d))) {
        if (!g_str_has_suffix(name, ".desktop")) continue;
        char *path = g_build_filename(dir, name, NULL);
        GDesktopAppInfo *app = g_desktop_app_info_new_from_filename(path);
        if (app && g_app_info_should_show(G_APP_INFO(app))) {
            add_app(sub, G_APP_INFO(app));
            count++;
        }
        g_clear_object(&app);
        g_free(path);
    }
    g_dir_close(d);
    if (count > 0) {
        GtkWidget *it = make_item("Custom", NULL, "applications-other|application-x-executable");
        gtk_menu_item_set_submenu(GTK_MENU_ITEM(it), sub);
        gtk_menu_shell_insert(GTK_MENU_SHELL(menu), it, 2);     /* right below the Search item */
    } else {
        gtk_widget_destroy(sub);
    }
    g_free(dir);
}

static GtkWidget *build_menu(void)
{
    GtkWidget *menu = new_menu();
    gtk_style_context_add_class(gtk_widget_get_style_context(menu), "hde-start-menu");

    GtkWidget *search = make_item("Search applications…", NULL, "system-search|edit-find|system-search-symbolic");
    gtk_widget_set_tooltip_text(search, "Or just start typing while the menu is open");
    g_signal_connect(search, "activate", G_CALLBACK(on_search_item), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), search);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

    GList *all = g_app_info_get_all();
    GList *apps = NULL;
    for (GList *l = all; l; l = l->next)
        if (G_IS_DESKTOP_APP_INFO(l->data) && g_app_info_should_show(l->data))
            apps = g_list_prepend(apps, l->data);
    apps = g_list_sort(apps, cmp_app);

    GHashTable *placed = g_hash_table_new(g_direct_hash, g_direct_equal);
    for (guint i = 0; i < N_CATS; i++) {
        GtkWidget *sub = new_menu();
        int count = 0;
        for (GList *l = apps; l; l = l->next) {
            if (!g_hash_table_contains(placed, l->data) && app_in_category(l->data, &categories[i])) {
                add_app(sub, l->data);
                g_hash_table_add(placed, l->data);
                count++;
            }
        }
        if (count == 0) { gtk_widget_destroy(sub); continue; }
        GtkWidget *it = make_item(categories[i].title, NULL, categories[i].icon);
        gtk_menu_item_set_submenu(GTK_MENU_ITEM(it), sub);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), it);
    }

    GtkWidget *other = new_menu();
    int n_other = 0;
    for (GList *l = apps; l; l = l->next)
        if (!g_hash_table_contains(placed, l->data)) { add_app(other, l->data); n_other++; }
    if (n_other) {
        GtkWidget *it = make_item("Other", NULL, "applications-other|application-x-executable");
        gtk_menu_item_set_submenu(GTK_MENU_ITEM(it), other);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), it);
    } else {
        gtk_widget_destroy(other);
    }

    add_custom_start_apps(menu);

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    GtkWidget *settings = make_item("Settings", NULL, "preferences-system|preferences-system-symbolic|emblem-system-symbolic");
    g_signal_connect(settings, "activate", G_CALLBACK(on_settings_item), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), settings);
    GtkWidget *power = make_item("Power / Session…", NULL, "system-shutdown|system-shutdown-symbolic");
    g_signal_connect(power, "activate", G_CALLBACK(show_power_dialog), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), power);

    g_hash_table_destroy(placed);
    g_list_free(apps);
    g_list_free_full(all, g_object_unref);
    gtk_widget_show_all(menu);
    return menu;
}

/* A "fake" event so that GTK grabs keyboard/pointer properly when the menu is opened with the Super key
 * (the command arrives as a ClientMessage, so there is no current GdkEvent). */
static GdkEvent *make_key_trigger(guint32 time)
{
    GdkWindow *gw = gtk_widget_get_window(panel_win);
    GdkSeat *seat = gdk_display_get_default_seat(gdk_window_get_display(gw));
    GdkDevice *kbd = seat ? gdk_seat_get_keyboard(seat) : NULL;
    if (!kbd) return NULL;
    GdkEvent *e = gdk_event_new(GDK_KEY_PRESS);
    e->key.window = g_object_ref(gw);
    e->key.send_event = TRUE;
    e->key.time = time ? time : (GDK_IS_X11_WINDOW(gw) ? gdk_x11_get_server_time(gw) : GDK_CURRENT_TIME);
    e->key.keyval = GDK_KEY_Super_L;
    gdk_event_set_device(e, kbd);
    return e;
}

static int menu_retry;
static void open_menu(guint32 time, gboolean keyboard);

static gboolean retry_menu(gpointer d)
{
    (void)d;
    open_menu(0, TRUE);
    return G_SOURCE_REMOVE;
}

static void open_menu(guint32 time, gboolean keyboard)
{
    if (app_menu) {
        gtk_widget_destroy(app_menu);                /* rebuilt every time so newly installed apps always show up */
        g_object_unref(app_menu);
    }
    app_menu = build_menu();
    g_object_ref_sink(app_menu);
    GdkEvent *trigger = keyboard ? make_key_trigger(time) : NULL;
    GtkWidget *at = gtk_widget_get_visible(menu_btn) ? menu_btn : panel_box;
    gtk_menu_popup_at_widget(GTK_MENU(app_menu), at, pcfg.top ? GDK_GRAVITY_SOUTH_WEST : GDK_GRAVITY_NORTH_WEST,
                             pcfg.top ? GDK_GRAVITY_NORTH_WEST : GDK_GRAVITY_SOUTH_WEST, trigger);
    if (trigger) gdk_event_free(trigger);
    if (keyboard) {
        DBG("menu popup by keyboard (time %u): mapped=%d retry=%d", time, gtk_widget_get_mapped(app_menu), menu_retry);
        if (!gtk_widget_get_mapped(app_menu) && debug_on && hde_is_x11()) {
            /* find the cause: who is holding the keyboard / pointer grab? */
            GdkSeat *seat = gdk_display_get_default_seat(gdk_display_get_default());
            GdkWindow *pw = gtk_widget_get_window(panel_win);
            GdkGrabStatus k = gdk_seat_grab(seat, pw, GDK_SEAT_CAPABILITY_KEYBOARD, TRUE, NULL, NULL, NULL, NULL);
            if (k == GDK_GRAB_SUCCESS) gdk_seat_ungrab(seat);
            GdkGrabStatus p = gdk_seat_grab(seat, pw, GDK_SEAT_CAPABILITY_ALL_POINTING, TRUE, NULL, NULL, NULL, NULL);
            if (p == GDK_GRAB_SUCCESS) gdk_seat_ungrab(seat);
            DBG("grab probe: keyboard=%d pointer=%d (0 ok, 1 already grabbed, 2 invalid time, 3 not viewable, 4 frozen)", k, p);
        }
        if (!gtk_widget_get_mapped(app_menu) && menu_retry < 3) {
            menu_retry++;                            /* the WM still holds the grab of the Super key: retry */
            g_timeout_add(120, retry_menu, NULL);
            return;
        }
        gtk_menu_shell_select_first(GTK_MENU_SHELL(app_menu), TRUE);
    }
    menu_retry = 0;
}

static gboolean classic_menu(void) { return pcfg.menu_style == HDE_MENU_CLASSIC; }

static void toggle_menu(guint32 time)
{
    DBG("toggle menu: search_visible=%d menu_mapped=%d", hde_search_visible(),
        classic_menu() ? app_menu && gtk_widget_get_mapped(app_menu) : hde_startmenu_visible());
    if (hde_search_visible()) { hde_search_hide(); return; }
    if (!classic_menu()) {
        hde_startmenu_toggle(menu_btn, panel_win, time);
        return;
    }
    if (app_menu && gtk_widget_get_mapped(app_menu)) {
        gtk_menu_shell_deactivate(GTK_MENU_SHELL(app_menu));
        return;
    }
    menu_retry = 0;
    open_menu(time, TRUE);
}

static void on_menu_clicked(GtkButton *btn, gpointer data)
{
    (void)btn; (void)data;
    if (!classic_menu()) {
        hde_startmenu_toggle(menu_btn, panel_win, gtk_get_current_event_time());
        return;
    }
    if (app_menu && gtk_widget_get_mapped(app_menu)) return;
    open_menu(gtk_get_current_event_time(), FALSE);
}

/* ---------- Session / Power ---------- */
static void run_cmd(GtkMenuItem *item, gpointer cmd)
{
    (void)item;
    if (!cmd) return;
    gchar *argv[] = { (gchar *)"/bin/sh", (gchar *)"-c", (gchar *)cmd, NULL };
    GError *err = NULL;
    if (!g_spawn_async(NULL, argv, NULL, G_SPAWN_DEFAULT, NULL, NULL, NULL, &err)) {
        g_printerr("hde-panel: %s\n", err->message);
        g_clear_error(&err);
    }
}

static void do_logout(void)
{
    const char *pid = g_getenv("HDE_SESSION_PID");
    if (pid && atoi(pid) > 1) kill((pid_t)atoi(pid), SIGTERM);
    else gtk_main_quit();
}

static void do_power_action(int response)
{
    switch (response) {
    case 1: do_logout(); break;
    case 2: run_cmd(NULL, "systemctl reboot"); break;
    case 3: run_cmd(NULL, "systemctl poweroff"); break;
    case 4: run_cmd(NULL, "(" HDE_SH_LOCK ") & sleep 1; systemctl suspend"); break;
    case 5: run_cmd(NULL, HDE_SH_LOCK); break;
    default: break;
    }
}

static gboolean power_dialog_open;

static void show_power_dialog(GtkMenuItem *item, gpointer data)
{
    (void)item; (void)data;
    if (power_dialog_open) return;
    power_dialog_open = TRUE;
    GtkWidget *dlg = gtk_dialog_new_with_buttons(
        "Session / Power", NULL, GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "Cancel", GTK_RESPONSE_CANCEL,
        "Lock Screen", 5,
        "Suspend", 4,
        "Log Out", 1,
        "Reboot", 2,
        "Shut Down", 3,
        NULL);
    gtk_window_set_default_size(GTK_WINDOW(dlg), 460, 160);
    gtk_window_set_position(GTK_WINDOW(dlg), GTK_WIN_POS_CENTER);
    gtk_window_set_keep_above(GTK_WINDOW(dlg), TRUE);
    gtk_window_set_icon_name(GTK_WINDOW(dlg), "system-shutdown");
    GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    gtk_container_set_border_width(GTK_CONTAINER(area), 18);
    GtkWidget *title = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(title), "<b>What do you want to do?</b>");
    gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
    gtk_box_pack_start(GTK_BOX(area), title, FALSE, FALSE, 4);
    GtkWidget *info = gtk_label_new("Choose an action. Unsaved work in open applications may be lost.");
    gtk_label_set_xalign(GTK_LABEL(info), 0.0f);
    gtk_box_pack_start(GTK_BOX(area), info, FALSE, FALSE, 4);
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_CANCEL);
    gtk_widget_show_all(dlg);
    hde_window_force_activate(dlg, gtk_get_current_event_time());
    int response = gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    power_dialog_open = FALSE;
    if (response != GTK_RESPONSE_CANCEL && response != GTK_RESPONSE_DELETE_EVENT)
        do_power_action(response);
}

/* Start menu buttons: 0 the dialog, 2 restart / 3 shut down (asked first), else at once */
static void menu_power(int action)
{
    if (action == 0) { show_power_dialog(NULL, NULL); return; }
    if (action == 2 || action == 3) {
        GtkWidget *d = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE,
                                              action == 2 ? "Restart the computer now?" : "Shut down the computer now?");
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), "Unsaved work in open applications may be lost.");
        gtk_dialog_add_buttons(GTK_DIALOG(d), "Cancel", GTK_RESPONSE_CANCEL, action == 2 ? "Restart" : "Shut Down",
                               GTK_RESPONSE_ACCEPT, NULL);
        gtk_window_set_keep_above(GTK_WINDOW(d), TRUE);
        gtk_window_set_position(GTK_WINDOW(d), GTK_WIN_POS_CENTER);
        gtk_window_set_title(GTK_WINDOW(d), action == 2 ? "Restart" : "Shut Down");
        gtk_widget_show_all(d);
        hde_window_force_activate(d, gtk_get_current_event_time());
        int r = gtk_dialog_run(GTK_DIALOG(d));
        gtk_widget_destroy(d);
        if (r != GTK_RESPONSE_ACCEPT) return;
    }
    do_power_action(action);
}

/* ---------- Run ---------- */
static gboolean run_dialog_open;

static void on_run_command(GtkButton *btn, gpointer data)
{
    (void)btn; (void)data;
    if (run_dialog_open) return;
    run_dialog_open = TRUE;
    GtkWidget *dialog = gtk_dialog_new_with_buttons(
        "Run Command", NULL, GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "Cancel", GTK_RESPONSE_CANCEL,
        "Run", GTK_RESPONSE_ACCEPT,
        NULL);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 440, 120);
    gtk_window_set_keep_above(GTK_WINDOW(dialog), TRUE);
    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);
    gtk_container_add(GTK_CONTAINER(content), box);
    GtkWidget *label = gtk_label_new("Enter a command to run:");
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);
    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "e.g. xterm, thunar, hde-settings");
    gtk_box_pack_start(GTK_BOX(box), entry, FALSE, FALSE, 0);
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
    gtk_window_set_position(GTK_WINDOW(dialog), GTK_WIN_POS_CENTER);
    gtk_widget_show_all(dialog);
    hde_window_force_activate(dialog, gtk_get_current_event_time());
    gtk_widget_grab_focus(entry);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        const char *cmd = gtk_entry_get_text(GTK_ENTRY(entry));
        if (cmd && *cmd) {
            GError *err = NULL;
            if (!g_spawn_command_line_async(cmd, &err) && err) {
                GtkWidget *m = gtk_message_dialog_new(GTK_WINDOW(dialog), GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR,
                                                      GTK_BUTTONS_CLOSE, "Could not run command: %s", err->message);
                gtk_dialog_run(GTK_DIALOG(m));
                gtk_widget_destroy(m);
                g_clear_error(&err);
            }
        }
    }
    gtk_widget_destroy(dialog);
    run_dialog_open = FALSE;
}

static void on_show_desktop(GtkButton *btn, gpointer data)
{
    (void)btn; (void)data;
#ifdef HAVE_WAYLAND_TASKBAR
    if (hde_is_wayland()) { hde_wl_taskbar_show_desktop(); return; }
#endif
    if (!hde_is_x11()) return;
    WnckScreen *scr = wnck_screen_get_default();
    if (!scr) return;
    wnck_screen_force_update(scr);
    wnck_screen_toggle_showing_desktop(scr, !wnck_screen_get_showing_desktop(scr));
}

/* ---------- command channel: X ClientMessage (hde-ipc.h) and D-Bus ---------- */
typedef struct { long cmd, arg; guint32 time; } PanelCmd;

static gboolean run_panel_cmd(gpointer p)
{
    PanelCmd *c = p;
    DBG("command %ld (time %u, arg %ld)", c->cmd, c->time, c->arg);
    switch (c->cmd) {
    case HDE_CMD_MENU:   toggle_menu(c->time); break;
    case HDE_CMD_SEARCH:
        if (app_menu && gtk_widget_get_mapped(app_menu)) gtk_menu_shell_deactivate(GTK_MENU_SHELL(app_menu));
        if (classic_menu()) hde_search_show(NULL, c->time);
        else if (!hde_startmenu_visible()) hde_startmenu_show(menu_btn, panel_win, c->time, NULL);
        break;
    case HDE_CMD_RUN:    on_run_command(NULL, NULL); break;
    case HDE_CMD_POWER:  show_power_dialog(NULL, NULL); break;
    case HDE_CMD_OSD_VOLUME: hde_status_osd_volume(); break;
    case HDE_CMD_OSD_MIC:    hde_status_osd_mic(); break;
    case HDE_CMD_OSD_BRIGHTNESS:
        hde_osd_show(c->arg <= 0 ? "display-brightness-off-symbolic" : "display-brightness-symbolic",
                     (int)CLAMP(c->arg, 0, 100), NULL);
        break;
    case HDE_CMD_REFRESH: hde_status_refresh(); break;
    case HDE_CMD_SHOW_DESKTOP: on_show_desktop(NULL, NULL); break;
    default: break;
    }
    g_free(c);
    return G_SOURCE_REMOVE;
}

static void queue_cmd(long cmd, guint32 time, long arg)
{
    PanelCmd *c = g_new0(PanelCmd, 1);
    c->cmd = cmd;
    c->time = time;
    c->arg = arg;
    g_idle_add(run_panel_cmd, c);       /* do not run dialogs/menus from inside a filter or a D-Bus call */
}

static GdkFilterReturn cmd_filter(GdkXEvent *xev, GdkEvent *ev, gpointer data)
{
    (void)ev; (void)data;
    XEvent *x = (XEvent *)xev;
    if (x->type == ClientMessage && cmd_atom && x->xclient.message_type == cmd_atom) {
        queue_cmd(x->xclient.data.l[0], (guint32)x->xclient.data.l[1], x->xclient.data.l[2]);
        return GDK_FILTER_REMOVE;
    }
    return GDK_FILTER_CONTINUE;
}

static void publish_panel_window(void)
{
    GdkWindow *gw = gtk_widget_get_window(panel_win);
    if (!GDK_IS_X11_WINDOW(gw)) return;
    Display *d = GDK_WINDOW_XDISPLAY(gw);
    Window xid = GDK_WINDOW_XID(gw);
    Atom prop = XInternAtom(d, HDE_PANEL_WINDOW_ATOM, False);
    cmd_atom = XInternAtom(d, HDE_PANEL_COMMAND_ATOM, False);
    XChangeProperty(d, xid, prop, XA_WINDOW, 32, PropModeReplace, (unsigned char *)&xid, 1);
    XChangeProperty(d, DefaultRootWindow(d), prop, XA_WINDOW, 32, PropModeReplace, (unsigned char *)&xid, 1);
    XFlush(d);
    gdk_window_add_filter(gw, cmd_filter, NULL);
}

static void unpublish_panel_window(void)
{
    if (!panel_win || !gtk_widget_get_window(panel_win) || !GDK_IS_X11_WINDOW(gtk_widget_get_window(panel_win))) return;
    GdkWindow *gw = gtk_widget_get_window(panel_win);
    Display *d = GDK_WINDOW_XDISPLAY(gw);
    Atom prop = XInternAtom(d, HDE_PANEL_WINDOW_ATOM, False);
    if (hde_ipc_window_prop(d, DefaultRootWindow(d), prop) == GDK_WINDOW_XID(gw))
        XDeleteProperty(d, DefaultRootWindow(d), prop);
    XFlush(d);
}

static const char panel_introspection[] =
    "<node><interface name='" PANEL_DBUS_IFACE "'>"
    "<method name='Command'><arg type='i' name='command' direction='in'/><arg type='u' name='time' direction='in'/>"
    "<arg type='i' name='argument' direction='in'/></method>"
    "</interface></node>";

static void on_dbus_call(GDBusConnection *c, const gchar *sender, const gchar *path, const gchar *iface,
                         const gchar *method, GVariant *params, GDBusMethodInvocation *inv, gpointer d)
{
    (void)c; (void)sender; (void)path; (void)iface; (void)d;
    if (!g_strcmp0(method, "Command")) {
        gint32 cmd = 0, arg = 0;
        guint32 t = 0;
        g_variant_get(params, "(iui)", &cmd, &t, &arg);
        queue_cmd(cmd, t, arg);
        g_dbus_method_invocation_return_value(inv, NULL);
        return;
    }
    g_dbus_method_invocation_return_dbus_error(inv, "org.freedesktop.DBus.Error.UnknownMethod", "unknown method");
}

static const GDBusInterfaceVTable panel_vtable = { on_dbus_call, NULL, NULL, { 0 } };

static void on_bus_acquired(GDBusConnection *c, const gchar *name, gpointer d)
{
    (void)name; (void)d;
    GDBusNodeInfo *info = g_dbus_node_info_new_for_xml(panel_introspection, NULL);
    if (info) {
        g_dbus_connection_register_object(c, PANEL_DBUS_PATH, info->interfaces[0], &panel_vtable, NULL, NULL, NULL);
        g_dbus_node_info_unref(info);
    }
}

static void on_name_acquired(GDBusConnection *c, const gchar *name, gpointer d)
{
    (void)c; (void)d;
    DBG("D-Bus name %s acquired", name);
}

/* hde-panel --menu & co.: X11 first (the panel window), then D-Bus (Wayland, or no X display) */
static int send_cli_command(long cmd, long arg)
{
    Display *d = g_getenv("DISPLAY") && *g_getenv("DISPLAY") ? XOpenDisplay(NULL) : NULL;
    if (d) {
        int rc = hde_ipc_send(d, cmd, arg, CurrentTime);
        XCloseDisplay(d);
        if (rc == 0) return 0;
    }
    GError *e = NULL;
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &e);
    if (bus) {
        GVariant *r = g_dbus_connection_call_sync(bus, PANEL_DBUS_NAME, PANEL_DBUS_PATH, PANEL_DBUS_IFACE, "Command",
                                                  g_variant_new("(iui)", (gint32)cmd, 0u, (gint32)arg), NULL,
                                                  G_DBUS_CALL_FLAGS_NO_AUTO_START, 3000, NULL, &e);
        g_object_unref(bus);
        if (r) { g_variant_unref(r); return 0; }
    }
    fprintf(stderr, "hde-panel: no running hde-panel found%s%s\n", e ? ": " : "", e ? e->message : "");
    g_clear_error(&e);
    return 1;
}

static gboolean panel_running(void)
{
    Display *d = g_getenv("DISPLAY") && *g_getenv("DISPLAY") && !g_getenv("WAYLAND_DISPLAY") ? XOpenDisplay(NULL) : NULL;
    if (d) {
        Window w = hde_ipc_find_panel(d);
        XCloseDisplay(d);
        return w != 0;
    }
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    if (!bus) return FALSE;
    GVariant *r = g_dbus_connection_call_sync(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                                              "NameHasOwner", g_variant_new("(s)", PANEL_DBUS_NAME), G_VARIANT_TYPE("(b)"),
                                              G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL);
    gboolean owned = FALSE;
    if (r) { g_variant_get(r, "(b)", &owned); g_variant_unref(r); }
    g_object_unref(bus);
    return owned;
}

/* ---------- strut: reserve space so windows do not cover the panel (X11) ---------- */
static void set_strut(GtkWidget *win, GdkRectangle *mon)
{
    GdkWindow *gw = gtk_widget_get_window(win);
    if (!gw || !GDK_IS_X11_WINDOW(gw)) return;
    GdkScreen *scr = gtk_widget_get_screen(win);
    gulong st[12] = { 0 };
    if (pcfg.top) {
        st[2] = mon->y + pcfg.size;
        st[8] = mon->x;
        st[9] = mon->x + mon->width - 1;
    } else {
        st[3] = pcfg.size + (gdk_screen_get_height(scr) - (mon->y + mon->height));
        st[10] = mon->x;
        st[11] = mon->x + mon->width - 1;
    }
    gdk_property_change(gw, gdk_atom_intern("_NET_WM_STRUT_PARTIAL", FALSE),
                        gdk_atom_intern("CARDINAL", FALSE), 32, GDK_PROP_MODE_REPLACE,
                        (const guchar *)st, 12);
    gulong s4[4] = { 0, 0, st[2], st[3] };
    gdk_property_change(gw, gdk_atom_intern("_NET_WM_STRUT", FALSE),
                        gdk_atom_intern("CARDINAL", FALSE), 32, GDK_PROP_MODE_REPLACE,
                        (const guchar *)s4, 4);
}

/* ---------- where the panel goes: top or bottom of the primary screen (F8, a monitor plugged in, Settings) ---------- */
static guint place_id;

/* d: why ("screens changed", "settings changed", "started"), written in the log */
static gboolean panel_place(gpointer d)
{
    const char *why = d ? d : "started";
    place_id = 0;
    if (!panel_win) return G_SOURCE_REMOVE;
    GdkDisplay *dpy = gdk_display_get_default();
    GdkMonitor *m = hde_main_monitor();
    if (!m) return G_SOURCE_REMOVE;
    GdkRectangle geo;
    gdk_monitor_get_geometry(m, &geo);
    if (hde_wl_is_layer(GTK_WINDOW(panel_win))) {
        hde_wl_layer_edges(GTK_WINDOW(panel_win), HDE_EDGE_LEFT | HDE_EDGE_RIGHT | (pcfg.top ? HDE_EDGE_TOP : HDE_EDGE_BOTTOM));
        hde_wl_layer_exclusive(GTK_WINDOW(panel_win), pcfg.size);
        gtk_widget_set_size_request(panel_win, -1, pcfg.size);
        fprintf(stderr, "hde-panel: %s: panel at the %s, %dpx high (Wayland layer shell, %d screen(s), %dx%d)\n",
                why, pcfg.top ? "top" : "bottom", pcfg.size, gdk_display_get_n_monitors(dpy), geo.width, geo.height);
        return G_SOURCE_REMOVE;
    }
    if (!gtk_widget_get_window(panel_win)) return G_SOURCE_REMOVE;
    int y = pcfg.top ? geo.y : geo.y + geo.height - pcfg.size;
    gtk_widget_set_size_request(panel_win, geo.width, pcfg.size);
    gtk_window_resize(GTK_WINDOW(panel_win), geo.width, pcfg.size);
    gtk_window_move(GTK_WINDOW(panel_win), geo.x, y);
    set_strut(panel_win, &geo);
    fprintf(stderr, "hde-panel: %s: panel at %d,%d %dx%d (primary screen %dx%d+%d+%d, %d screen(s))\n",
            why, geo.x, y, geo.width, pcfg.size, geo.width, geo.height, geo.x, geo.y, gdk_display_get_n_monitors(dpy));
    return G_SOURCE_REMOVE;
}

static void on_screens_changed(GdkScreen *s, gpointer d)
{
    (void)s; (void)d;
    if (place_id) g_source_remove(place_id);
    place_id = g_timeout_add(250, panel_place, (gpointer)"screens changed");
}

/* clicking the panel closes the Start menu (on Wayland the menu does not hold the pointer; on X11 it does, and
 * such clicks reach the menu instead) — except the Start button itself, which toggles it */
static void on_panel_pressed(GtkGestureMultiPress *g, int n, double x, double y, gpointer d)
{
    (void)g; (void)n; (void)d;
    if (!hde_startmenu_visible()) return;
    int bx = -1, by = -1;
    GtkAllocation a;
    gtk_widget_get_allocation(menu_btn, &a);
    if (gtk_widget_get_visible(menu_btn))
        gtk_widget_translate_coordinates(panel_win, menu_btn, (int)x, (int)y, &bx, &by);
    if (bx < 0 || by < 0 || bx >= a.width || by >= a.height) hde_startmenu_hide();
}

/* ---------- CSS: light/dark according to Settings > Appearance, opacity and size of Settings > Panel ---------- */
static void load_css(void)
{
    HdeThemeInfo ti;
    hde_theme_info_load(&ti);
    gboolean dark = ti.style != HDE_STYLE_LIGHT;
    const char *bg     = dark ? "30, 34, 42" : "243, 244, 246";
    const char *fg     = dark ? "#e6e9ef" : "#1f2329";
    const char *hover  = dark ? "#343b48" : "#dfe3e8";
    const char *sub    = dark ? "#aeb7c6" : "#5f6672";
    const char *runbg  = dark ? "#303744" : "#e4e7eb";
    const char *border = dark ? "#2c323d" : "#cfd4da";
    const char *popbg  = dark ? "#252a33" : "#ffffff";
    const char *popbd  = dark ? "#3a4150" : "#d0d4da";
    const char *entry  = dark ? "#1b1f26" : "#f4f5f7";
    double alpha = pcfg.opacity / 100.0;
    if (panel_win && hde_is_x11() && !gdk_screen_is_composited(gtk_widget_get_screen(panel_win))) alpha = 1.0;
    int big = pcfg.size >= 44, small = pcfg.size < 30;
    GString *s = g_string_new(NULL);
    g_string_append_printf(s, ".hde-panel { background: rgba(%s, %.2f); color: %s; %s: 1px solid %s; }", bg, alpha, fg,
                           pcfg.top ? "border-bottom" : "border-top", border);
    g_string_append_printf(s, ".hde-panel button { background: transparent; background-image: none; border: none; border-radius: 4px;"
                              "  padding: %dpx %dpx; color: %s; box-shadow: none; text-shadow: none; -gtk-icon-shadow: none; }",
                           small ? 0 : 2, small ? 5 : 8, fg);
    g_string_append_printf(s, ".hde-panel button:hover { background: %s; }", hover);
    g_string_append_printf(s, ".hde-panel button:checked { background: %s; color: white; }", ti.accent);
    g_string_append_printf(s, ".hde-panel .menu-btn, .hde-panel .menu-btn label { font-weight: bold; background: %s; color: white; }", ti.accent);
    g_string_append_printf(s, ".hde-panel .menu-btn:hover { background: shade(%s, 1.15); }", ti.accent);
    g_string_append_printf(s, ".hde-panel .run-btn { background: %s; }", runbg);
    g_string_append_printf(s, ".hde-panel .run-btn:hover { background: %s; }", hover);
    g_string_append(s, ".hde-panel .launcher { padding: 2px 5px; }");
    g_string_append(s, ".hde-panel .applet { padding: 2px 6px; }");
    g_string_append(s, ".hde-panel .icons-only button { padding: 2px 6px; }");
    g_string_append(s, ".hde-panel .clock-btn { padding: 0 8px; }");
    g_string_append_printf(s, ".hde-panel .clock-box { min-width: %dpx; }", pcfg.clock_seconds || !pcfg.clock_24h ? 120 : 100);
    g_string_append_printf(s, ".hde-panel .clock-time { font-weight: 700; font-size: %dpx; }", big ? 14 : 12);
    g_string_append_printf(s, ".hde-panel .clock-date { font-size: %dpx; color: %s; }", big ? 10 : 8, sub);
    g_string_append(s, ".hde-panel .status-btn { padding: 2px 5px; }");
    g_string_append_printf(s, ".hde-panel .notif-count { background: %s; color: white; border-radius: 8px; padding: 0 5px;"
                              "  font-size: 9px; font-weight: bold; }", ti.accent);
    g_string_append_printf(s, ".hde-panel label { color: %s; }", fg);
    g_string_append_printf(s, ".hde-osd, .hde-notification, .hde-search, .hde-calendar { background: %s; color: %s;"
                              "  border: 1px solid %s; border-radius: 0; }", popbg, fg, popbd);
    /* rounded corners only when the WM composites (hde_popup_setup_alpha adds the .rounded class), avoiding black corners */
    g_string_append(s, ".hde-osd.rounded, .hde-notification.rounded, .hde-search.rounded, .hde-calendar.rounded { border-radius: 10px; }");
    g_string_append_printf(s, ".hde-osd label, .hde-notification label, .hde-search label, .hde-calendar label { color: %s; }", fg);
    g_string_append_printf(s, ".hde-osd levelbar trough { min-height: 6px; border-radius: 3px; background: %s; border: none; }", hover);
    g_string_append_printf(s, ".hde-osd levelbar block.filled { background: %s; border-radius: 3px; border: none; min-height: 6px; }", ti.accent);
    g_string_append(s, ".hde-osd .osd-text { font-weight: bold; }");
    g_string_append(s, ".hde-notification.critical { border-left: 4px solid #e01b24; }");
    g_string_append(s, ".hde-notification .notif-summary { font-weight: bold; }");
    g_string_append_printf(s, ".hde-notification .notif-app { font-size: 9px; color: %s; }", sub);
    g_string_append_printf(s, ".hde-notification .notif-body { color: %s; }", fg);
    g_string_append_printf(s, ".hde-notification button { padding: 3px 8px; background: %s; background-image: none; border: 1px solid %s;"
                              "  border-radius: 6px; box-shadow: none; text-shadow: none; }", runbg, popbd);
    g_string_append_printf(s, ".hde-notification button:hover { background: shade(%s, 1.2); }", runbg);
    g_string_append_printf(s, ".hde-notification button label { color: %s; }", fg);
    g_string_append(s, ".hde-notification button.notif-close { background: transparent; border: none; padding: 2px; }");
    g_string_append_printf(s, ".hde-search entry { background: %s; color: %s; border-radius: 8px; padding: 6px 8px; min-height: 26px; }", entry, fg);
    g_string_append(s, ".hde-search list, .hde-search scrolledwindow, .hde-search viewport { background: transparent; }");
    g_string_append_printf(s, ".hde-search row { border-radius: 8px; color: %s; }", fg);
    g_string_append_printf(s, ".hde-search row:selected { background: %s; color: white; }", ti.accent);
    g_string_append(s, ".hde-search row:selected label { color: white; }");
    g_string_append(s, ".hde-search .search-title { font-weight: 600; }");
    g_string_append_printf(s, ".hde-search .search-desc { font-size: 9px; color: %s; }", sub);
    g_string_append(s, ".hde-calendar .cal-title { font-weight: bold; font-size: 12px; }");
    g_string_append_printf(s, ".hde-calendar calendar { background: transparent; color: %s; border: none; }", fg);
    g_string_append_printf(s, ".hde-calendar calendar:selected { background: %s; color: white; border-radius: 4px; }", ti.accent);
    char *menu_css = hde_startmenu_css(dark, ti.accent);
    g_string_append(s, menu_css);
    g_free(menu_css);
    if (!panel_css) {
        panel_css = gtk_css_provider_new();
        gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(panel_css),
                                                  GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
    GError *err = NULL;
    if (!gtk_css_provider_load_from_data(panel_css, s->str, -1, &err)) {
        g_printerr("hde-panel: CSS: %s\n", err->message);
        g_clear_error(&err);
    }
    g_string_free(s, TRUE);
    hde_theme_info_clear(&ti);
}

/* ---------- the Start button: ☰ / logo of the system / HDE logo / an icon, and a label ---------- */
static void update_menu_button(void)
{
    const char *icon = pcfg.menu_icon;
    const char *label = pcfg.menu_label ? pcfg.menu_label : "";
    int px = icon_px();
    gtk_widget_hide(menu_btn_img);
    if (!g_strcmp0(icon, "menu")) {
        char *t = *label ? g_strdup_printf("  ☰  %s  ", label) : g_strdup("  ☰  ");
        gtk_label_set_text(GTK_LABEL(menu_btn_lbl), t);
        g_free(t);
        DBG("start button: ☰ %s", label);
        return;
    }
    if (*label) {
        char *t = g_strdup_printf(" %s ", label);
        gtk_label_set_text(GTK_LABEL(menu_btn_lbl), t);
        g_free(t);
    } else gtk_label_set_text(GTK_LABEL(menu_btn_lbl), "");
    gtk_widget_set_visible(menu_btn_lbl, *label != '\0');
    if (!g_strcmp0(icon, "none")) { DBG("start button: %s (no icon)", label); return; }
    int scale = gtk_widget_get_scale_factor(menu_btn);
    cairo_surface_t *s = NULL;
    char *how = NULL;
    if (!g_strcmp0(icon, "os")) {
        HdeOsInfo os;
        hde_os_info_load(&os);
        s = hde_os_logo_surface(&os, px, scale, TRUE, &how);
        hde_os_info_clear(&os);
    } else if (!g_strcmp0(icon, "hde")) {
        s = hde_hde_logo_surface(px, scale, "#ffffff");
        how = g_strdup("HDE logo");
    }
    if (s) {
        gtk_image_set_from_surface(GTK_IMAGE(menu_btn_img), s);
        cairo_surface_destroy(s);
    } else {
        gtk_image_set_from_icon_name(GTK_IMAGE(menu_btn_img), icon, GTK_ICON_SIZE_LARGE_TOOLBAR);
        gtk_image_set_pixel_size(GTK_IMAGE(menu_btn_img), px);
        how = g_strdup_printf("icon %s", icon);
    }
    gtk_widget_show(menu_btn_img);
    DBG("start button: %s, %s", label, how ? how : icon);
    g_free(how);
}

/* ---------- taskbar ---------- */
static WnckScreen *wnck_scr;

static int wnck_buttons(void)
{
    if (!wnck_scr) return 0;
    WnckWorkspace *ws = wnck_screen_get_active_workspace(wnck_scr);
    int n = 0;
    for (GList *l = wnck_screen_get_windows(wnck_scr); l; l = l->next) {
        WnckWindow *w = l->data;
        if (wnck_window_is_skip_tasklist(w)) continue;
        if (ws && !wnck_window_is_on_workspace(w, ws)) continue;
        n++;
    }
    return n;
}

/* icons only: give libwnck just enough room for the icons (it then hides the titles itself) */
static void update_taskbar_width(void)
{
    if (!tasks || !hde_is_x11()) return;
    gboolean labels = pcfg.taskbar_labels;
    gtk_box_set_child_packing(GTK_BOX(panel_box), tasks, labels, labels, 0, GTK_PACK_START);
    gtk_widget_set_size_request(tasks, labels ? -1 : MAX(1, wnck_buttons()) * 44, -1);
}

static void on_wnck_changed(WnckScreen *s, gpointer a, gpointer d) { (void)s; (void)a; (void)d; update_taskbar_width(); }

static GtkWidget *make_taskbar(void)
{
#ifdef HAVE_WAYLAND_TASKBAR
    if (hde_is_wayland()) {
        GtkWidget *t = hde_wl_taskbar_new();
        if (t) return t;
    }
#endif
    if (!hde_is_x11()) {
        GtkWidget *l = gtk_label_new("");
        gtk_widget_set_tooltip_text(l, "No taskbar: this HDE was built without Wayland support (libgtk-layer-shell-dev)");
        return l;
    }
    GtkWidget *t = wnck_tasklist_new();
    wnck_tasklist_set_grouping(WNCK_TASKLIST(t), WNCK_TASKLIST_AUTO_GROUP);
    wnck_tasklist_set_include_all_workspaces(WNCK_TASKLIST(t), FALSE);
    wnck_tasklist_set_button_relief(WNCK_TASKLIST(t), GTK_RELIEF_NONE);
    wnck_scr = wnck_screen_get_default();
    if (wnck_scr) {
        g_signal_connect(wnck_scr, "window-opened", G_CALLBACK(on_wnck_changed), NULL);
        g_signal_connect(wnck_scr, "window-closed", G_CALLBACK(on_wnck_changed), NULL);
        g_signal_connect(wnck_scr, "active-workspace-changed", G_CALLBACK(on_wnck_changed), NULL);
    }
    return t;
}

static void apply_taskbar(void)
{
#ifdef HAVE_WAYLAND_TASKBAR
    if (hde_is_wayland()) { hde_wl_taskbar_set_labels(tasks, pcfg.taskbar_labels); return; }
#endif
    if (!hde_is_x11() || !WNCK_IS_TASKLIST(tasks)) return;
    static const WnckTasklistGroupingType g[3] = { WNCK_TASKLIST_NEVER_GROUP, WNCK_TASKLIST_AUTO_GROUP, WNCK_TASKLIST_ALWAYS_GROUP };
    wnck_tasklist_set_grouping(WNCK_TASKLIST(tasks), g[CLAMP(pcfg.taskbar_group, 0, 2)]);
    if (pcfg.taskbar_labels) gtk_style_context_remove_class(gtk_widget_get_style_context(tasks), "icons-only");
    else gtk_style_context_add_class(gtk_widget_get_style_context(tasks), "icons-only");
    update_taskbar_width();
}

/* ---------- Settings > Panel / Start Menu changed: apply everything live ---------- */
static char *applets_signature(void)
{
    HdeApplet *a = NULL;
    int n = hde_applets_load(&a);
    GString *s = g_string_new(NULL);
    for (int i = 0; i < n; i++)
        g_string_append_printf(s, "%s|%s|%s|%s|%s|%d\n", a[i].id, a[i].type, a[i].label ? a[i].label : "",
                               a[i].command ? a[i].command : "", a[i].click ? a[i].click : "", a[i].interval);
    hde_applets_free(a, n);
    return g_string_free(s, FALSE);
}

static void apply_config(gboolean first)
{
    HdePanelConfig old = pcfg;
    hde_panel_config_load(&pcfg);
    gtk_widget_set_visible(menu_btn, pcfg.show_menu);
    gtk_widget_set_visible(desk_btn, pcfg.show_desktop);
    gtk_widget_set_visible(run_btn, pcfg.show_run);
    gtk_widget_set_visible(tasks, pcfg.show_taskbar);
    if (pager) gtk_widget_set_visible(pager, pcfg.show_workspaces && hde_is_x11());
    gtk_widget_set_visible(tray, pcfg.show_tray);
    gtk_widget_set_visible(status_area, pcfg.show_status);
    gtk_widget_set_visible(notify_btn, pcfg.show_notifications);
    gtk_widget_set_visible(clock_btn, pcfg.show_clock);
    gtk_image_set_pixel_size(GTK_IMAGE(gtk_button_get_image(GTK_BUTTON(desk_btn))), icon_px() - 4);
    if (first || old.size != pcfg.size || !g_strv_equal((const char *const *)old.launchers, (const char *const *)pcfg.launchers))
        hde_launchers_update(launchers, pcfg.launchers, icon_px());
    gtk_widget_set_visible(launchers, pcfg.show_launchers && pcfg.launchers[0]);
    char *sig = applets_signature();
    if (first || g_strcmp0(sig, applets_sig)) {
        hde_applets_update(applets);
        g_free(applets_sig);
        applets_sig = sig;
    } else g_free(sig);
    if (first || old.size != pcfg.size || old.menu_icon_size != pcfg.menu_icon_size || g_strcmp0(old.menu_label, pcfg.menu_label) ||
        g_strcmp0(old.menu_icon, pcfg.menu_icon))
        update_menu_button();
    apply_taskbar();
    update_clock(NULL);
    hde_startmenu_set_config(&pcfg);
    if (!first && (old.top != pcfg.top || old.size != pcfg.size)) {
        if (place_id) g_source_remove(place_id);
        place_id = g_idle_add(panel_place, (gpointer)"settings changed");
    }
    if (first || old.top != pcfg.top || old.size != pcfg.size || old.opacity != pcfg.opacity ||
        old.clock_24h != pcfg.clock_24h || old.clock_seconds != pcfg.clock_seconds)
        load_css();
    if (!first)
        DBG("settings applied: %s, %dpx, opacity %d%%, menu %s, items:%s%s%s%s%s%s%s%s%s%s", pcfg.top ? "top" : "bottom",
            pcfg.size, pcfg.opacity, hde_menu_style_id(pcfg.menu_style), pcfg.show_menu ? " menu" : "",
            pcfg.show_desktop ? " desktop" : "", pcfg.show_run ? " run" : "", pcfg.show_launchers ? " launchers" : "",
            pcfg.show_taskbar ? " taskbar" : "", pcfg.show_workspaces ? " workspaces" : "", pcfg.show_tray ? " tray" : "",
            pcfg.show_status ? " status" : "", pcfg.show_notifications ? " notifications" : "", pcfg.show_clock ? " clock" : "");
    hde_panel_config_clear(&old);
}

static void on_theme_changed(gpointer d)
{
    (void)d;
    load_css();
    apply_config(FALSE);
}

static gboolean on_unix_signal(gpointer d)
{
    (void)d;
    gtk_main_quit();
    return G_SOURCE_REMOVE;
}

static void usage(void)
{
    printf("Usage: hde-panel                 start the panel\n"
           "       hde-panel --menu          toggle the Start menu of the running panel\n"
           "       hde-panel --search        open application search\n"
           "       hde-panel --run           open the Run dialog\n"
           "       hde-panel --power         open the Session / Power dialog\n"
           "       hde-panel --show-desktop  show the desktop / bring the windows back\n"
           "       hde-panel --osd-volume    show the volume OSD\n"
           "       hde-panel --osd-brightness N   show the brightness OSD at N %%\n"
           "       hde-panel --refresh       refresh the status area\n");
}

int main(int argc, char **argv)
{
    static const struct { const char *opt; long cmd; } cli[] = {
        { "--menu", HDE_CMD_MENU }, { "--search", HDE_CMD_SEARCH }, { "--run", HDE_CMD_RUN },
        { "--power", HDE_CMD_POWER }, { "--osd-volume", HDE_CMD_OSD_VOLUME }, { "--osd-mic", HDE_CMD_OSD_MIC },
        { "--refresh", HDE_CMD_REFRESH }, { "--show-desktop", HDE_CMD_SHOW_DESKTOP },
    };
    for (int i = 1; i < argc; i++) {
        for (guint k = 0; k < G_N_ELEMENTS(cli); k++)
            if (!strcmp(argv[i], cli[k].opt)) return send_cli_command(cli[k].cmd, 0);
        if (!strcmp(argv[i], "--osd-brightness")) return send_cli_command(HDE_CMD_OSD_BRIGHTNESS, i + 1 < argc ? atol(argv[i + 1]) : 0);
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(); return 0; }
    }
    if (panel_running()) {
        fprintf(stderr, "hde-panel: another hde-panel is already running\n");
        return 0;
    }

    gtk_init(&argc, &argv);
    debug_on = g_getenv("HDE_DEBUG") != NULL;
    hde_applets_set_debug(debug_on);
    if (hde_is_x11()) wnck_set_client_type(WNCK_CLIENT_TYPE_PAGER);
    hde_panel_config_load(&pcfg);
    hde_theme_apply_process();
    load_css();
    hde_theme_watch(on_theme_changed, NULL);
    hde_notify_init();
    const HdeMenuActions acts = { menu_power, hde_open_settings };
    hde_startmenu_init(&acts, debug_on);
    g_bus_own_name(G_BUS_TYPE_SESSION, PANEL_DBUS_NAME, G_BUS_NAME_OWNER_FLAGS_NONE, on_bus_acquired, on_name_acquired,
                   NULL, NULL, NULL);

    GdkMonitor *m = hde_main_monitor();
    GdkRectangle geo = { 0, 0, 1024, 768 };
    if (m) gdk_monitor_get_geometry(m, &geo);

    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    panel_win = win;
    gtk_window_set_title(GTK_WINDOW(win), "hde-panel");
    gtk_style_context_add_class(gtk_widget_get_style_context(win), "hde-panel");
    g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    GdkVisual *rgba = gdk_screen_get_rgba_visual(gtk_widget_get_screen(win));
    if (rgba && (hde_is_wayland() || gdk_screen_is_composited(gtk_widget_get_screen(win))))
        gtk_widget_set_visual(win, rgba);                /* the panel can be see-through (Settings > Panel) */
    if (hde_wl_layer_init(GTK_WINDOW(win), "hde-panel", HDE_LAYER_TOP,
                          HDE_EDGE_LEFT | HDE_EDGE_RIGHT | (pcfg.top ? HDE_EDGE_TOP : HDE_EDGE_BOTTOM), HDE_KB_NONE)) {
        hde_wl_layer_exclusive(GTK_WINDOW(win), pcfg.size);
        gtk_widget_set_size_request(win, -1, pcfg.size);
    } else {
        gtk_window_set_type_hint(GTK_WINDOW(win), GDK_WINDOW_TYPE_HINT_DOCK);
        gtk_window_set_decorated(GTK_WINDOW(win), FALSE);
        gtk_window_set_skip_taskbar_hint(GTK_WINDOW(win), TRUE);
        gtk_window_set_skip_pager_hint(GTK_WINDOW(win), TRUE);
        gtk_window_stick(GTK_WINDOW(win));
        gtk_window_set_keep_above(GTK_WINDOW(win), TRUE);   /* always above the desktop, even if the WM ignores the DOCK hint */
        gtk_widget_set_size_request(win, geo.width, pcfg.size);
        gtk_window_set_default_size(GTK_WINDOW(win), geo.width, pcfg.size);
        gtk_window_move(GTK_WINDOW(win), geo.x, pcfg.top ? geo.y : geo.y + geo.height - pcfg.size);
    }
    GtkGesture *press = gtk_gesture_multi_press_new(win);
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(press), 0);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(press), GTK_PHASE_CAPTURE);
    g_signal_connect(press, "pressed", G_CALLBACK(on_panel_pressed), NULL);
    g_object_set_data_full(G_OBJECT(win), "hde-press", press, g_object_unref);

    GtkWidget *box = panel_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(box), 3);
    gtk_container_add(GTK_CONTAINER(win), box);

    menu_btn = gtk_button_new();
    GtkWidget *mb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    menu_btn_img = gtk_image_new();
    menu_btn_lbl = gtk_label_new("");
    gtk_box_pack_start(GTK_BOX(mb), menu_btn_img, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(mb), menu_btn_lbl, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(menu_btn), mb);
    gtk_style_context_add_class(gtk_widget_get_style_context(menu_btn), "menu-btn");
    gtk_widget_set_tooltip_text(menu_btn, "Start menu (Super)");
    g_signal_connect(menu_btn, "clicked", G_CALLBACK(on_menu_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(box), menu_btn, FALSE, FALSE, 0);

    desk_btn = gtk_button_new_from_icon_name("user-desktop-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(desk_btn, "Show Desktop (Super+D)");
    g_signal_connect(desk_btn, "clicked", G_CALLBACK(on_show_desktop), NULL);
    gtk_box_pack_start(GTK_BOX(box), desk_btn, FALSE, FALSE, 0);

    run_btn = gtk_button_new_with_label("⌘ Run");
    gtk_style_context_add_class(gtk_widget_get_style_context(run_btn), "run-btn");
    gtk_widget_set_tooltip_text(run_btn, "Run a command (Super+R)");
    g_signal_connect(run_btn, "clicked", G_CALLBACK(on_run_command), NULL);
    gtk_box_pack_start(GTK_BOX(box), run_btn, FALSE, FALSE, 0);

    launchers = hde_launchers_new();
    gtk_box_pack_start(GTK_BOX(box), launchers, FALSE, FALSE, 0);

    tasks = make_taskbar();
    gtk_box_pack_start(GTK_BOX(box), tasks, TRUE, TRUE, 0);

    if (hde_is_x11()) {
        pager = wnck_pager_new();
        wnck_pager_set_display_mode(WNCK_PAGER(pager), WNCK_PAGER_DISPLAY_CONTENT);
        wnck_pager_set_n_rows(WNCK_PAGER(pager), 1);
        gtk_box_pack_start(GTK_BOX(box), pager, FALSE, FALSE, 4);
    }

    /* right side, from right to left: clock | notification bell | status | system tray | extensions */
    clock_btn = gtk_button_new();
    gtk_button_set_relief(GTK_BUTTON(clock_btn), GTK_RELIEF_NONE);
    gtk_style_context_add_class(gtk_widget_get_style_context(clock_btn), "clock-btn");
    GtkWidget *clock_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(clock_box), "clock-box");
    gtk_widget_set_valign(clock_box, GTK_ALIGN_CENTER);
    clock_label = gtk_label_new("");
    gtk_style_context_add_class(gtk_widget_get_style_context(clock_label), "clock-time");
    gtk_box_pack_start(GTK_BOX(clock_box), clock_label, FALSE, FALSE, 0);
    date_label = gtk_label_new("");
    gtk_style_context_add_class(gtk_widget_get_style_context(date_label), "clock-date");
    gtk_box_pack_start(GTK_BOX(clock_box), date_label, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(clock_btn), clock_box);
    g_signal_connect(clock_btn, "clicked", G_CALLBACK(toggle_calendar), NULL);
    gtk_box_pack_end(GTK_BOX(box), clock_btn, FALSE, FALSE, 2);
    notify_btn = hde_notify_button_new();
    gtk_box_pack_end(GTK_BOX(box), notify_btn, FALSE, FALSE, 0);
    status_area = hde_status_new();
    gtk_box_pack_end(GTK_BOX(box), status_area, FALSE, FALSE, 2);
    tray = hde_tray_new();
    gtk_box_pack_end(GTK_BOX(box), tray, FALSE, FALSE, 4);
    applets = hde_applets_new();
    gtk_box_pack_end(GTK_BOX(box), applets, FALSE, FALSE, 2);

    gtk_widget_show_all(win);
    apply_config(TRUE);
    g_timeout_add_seconds(1, update_clock, NULL);

    if (!hde_wl_is_layer(GTK_WINDOW(win))) {
        set_strut(win, &geo);
        publish_panel_window();
        gdk_window_raise(gtk_widget_get_window(win));   /* never covered by hde-desktop if they start in the wrong order */
    }
    panel_place((gpointer)"started");
    g_signal_connect(gdk_screen_get_default(), "monitors-changed", G_CALLBACK(on_screens_changed), NULL);
    g_signal_connect(gdk_screen_get_default(), "size-changed", G_CALLBACK(on_screens_changed), NULL);

    g_unix_signal_add(SIGTERM, on_unix_signal, NULL);
    g_unix_signal_add(SIGINT, on_unix_signal, NULL);
    gtk_main();
    unpublish_panel_window();
    return 0;
}
