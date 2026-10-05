/* hde-panel: the bottom panel — Start menu, taskbar, workspaces, status, notifications and clock.
 *
 * Controlled from other processes (see hde-ipc.h):
 *   hde-panel --menu | --search | --run | --power | --osd-volume | --refresh
 * hde-hotkeys uses this channel for the Super key (open/close the Start menu) and the volume/brightness OSD.
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

#define PANEL_HEIGHT 34
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

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

static GtkWidget *panel_win;
static GtkWidget *clock_label;
static GtkWidget *date_label;
static GtkWidget *clock_btn;
static GtkWidget *menu_btn;
static GtkWidget *app_menu;
static GtkCssProvider *panel_css;
static Atom cmd_atom;
static gboolean debug_on;          /* HDE_DEBUG=1: write diagnostic logs to stderr (~/.cache/hde/session.log) */
#define DBG(...) do { if (debug_on) { g_printerr("hde-panel: " __VA_ARGS__); g_printerr("\n"); } } while (0)

static void run_cmd(GtkMenuItem *item, gpointer cmd);
static void show_power_dialog(GtkMenuItem *item, gpointer data);
static void on_run_command(GtkButton *btn, gpointer data);

/* ---------- clock + calendar ---------- */
static GtkWidget *cal_win, *cal_title, *cal;

static gboolean update_clock(gpointer data)
{
    (void)data;
    GDateTime *now = g_date_time_new_now_local();
    char *time_s = g_date_time_format(now, "%H:%M");
    char *date_s = g_date_time_format(now, "%a  %d/%m/%Y");
    char *tip = g_date_time_format(now, "%A, %d %B %Y");
    gtk_label_set_text(GTK_LABEL(clock_label), time_s);
    gtk_label_set_text(GTK_LABEL(date_label), date_s);
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
    GdkWindow *pw = gtk_widget_get_window(panel_win);
    int ox = 0, oy = 0;
    gdk_window_get_origin(pw, &ox, &oy);
    GtkAllocation a;
    gtk_widget_get_allocation(GTK_WIDGET(b), &a);
    int x = ox + a.x + a.width - nat.width;
    if (x < ox) x = ox;
    gtk_window_move(GTK_WINDOW(cal_win), x, oy - nat.height - 6);
    gtk_widget_show(cal_win);
    gtk_window_present_with_time(GTK_WINDOW(cal_win), gtk_get_current_event_time());
}

/* ---------- Start menu ---------- */
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
        g_mkdir_with_parents(dir, 0755);
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
    e->key.time = time ? time : gdk_x11_get_server_time(gw);
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
    gtk_menu_popup_at_widget(GTK_MENU(app_menu), menu_btn, GDK_GRAVITY_NORTH_WEST, GDK_GRAVITY_SOUTH_WEST, trigger);
    if (trigger) gdk_event_free(trigger);
    if (keyboard) {
        DBG("menu popup by keyboard (time %u): mapped=%d retry=%d", time, gtk_widget_get_mapped(app_menu), menu_retry);
        if (!gtk_widget_get_mapped(app_menu) && debug_on) {
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

static void toggle_menu(guint32 time)
{
    DBG("toggle menu: search_visible=%d menu_mapped=%d", hde_search_visible(), app_menu && gtk_widget_get_mapped(app_menu));
    if (hde_search_visible()) { hde_search_hide(); return; }
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
    WnckScreen *scr = wnck_screen_get_default();
    if (!scr) return;
    wnck_screen_force_update(scr);
    wnck_screen_toggle_showing_desktop(scr, !wnck_screen_get_showing_desktop(scr));
}

/* ---------- command channel (hde-ipc.h) ---------- */
typedef struct { long cmd, arg; guint32 time; } PanelCmd;

static gboolean run_panel_cmd(gpointer p)
{
    PanelCmd *c = p;
    DBG("command %ld (time %u, arg %ld)", c->cmd, c->time, c->arg);
    switch (c->cmd) {
    case HDE_CMD_MENU:   toggle_menu(c->time); break;
    case HDE_CMD_SEARCH:
        if (app_menu && gtk_widget_get_mapped(app_menu)) gtk_menu_shell_deactivate(GTK_MENU_SHELL(app_menu));
        hde_search_show(NULL, c->time);
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
    default: break;
    }
    g_free(c);
    return G_SOURCE_REMOVE;
}

static GdkFilterReturn cmd_filter(GdkXEvent *xev, GdkEvent *ev, gpointer data)
{
    (void)ev; (void)data;
    XEvent *x = (XEvent *)xev;
    if (x->type == ClientMessage && cmd_atom && x->xclient.message_type == cmd_atom) {
        PanelCmd *c = g_new0(PanelCmd, 1);
        c->cmd = x->xclient.data.l[0];
        c->time = (guint32)x->xclient.data.l[1];
        c->arg = x->xclient.data.l[2];
        g_idle_add(run_panel_cmd, c);       /* do not run dialogs/menus from inside the filter */
        return GDK_FILTER_REMOVE;
    }
    return GDK_FILTER_CONTINUE;
}

static void publish_panel_window(void)
{
    GdkWindow *gw = gtk_widget_get_window(panel_win);
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
    if (!panel_win || !gtk_widget_get_window(panel_win)) return;
    GdkWindow *gw = gtk_widget_get_window(panel_win);
    Display *d = GDK_WINDOW_XDISPLAY(gw);
    Atom prop = XInternAtom(d, HDE_PANEL_WINDOW_ATOM, False);
    if (hde_ipc_window_prop(d, DefaultRootWindow(d), prop) == GDK_WINDOW_XID(gw))
        XDeleteProperty(d, DefaultRootWindow(d), prop);
    XFlush(d);
}

static int send_cli_command(long cmd)
{
    Display *d = XOpenDisplay(NULL);
    if (!d) { fprintf(stderr, "hde-panel: cannot open X display\n"); return 2; }
    int rc = hde_ipc_send(d, cmd, 0, CurrentTime);
    XCloseDisplay(d);
    if (rc != 0) { fprintf(stderr, "hde-panel: no running hde-panel found\n"); return 1; }
    return 0;
}

static gboolean panel_running(void)
{
    Display *d = XOpenDisplay(NULL);
    if (!d) return FALSE;
    Window w = hde_ipc_find_panel(d);
    XCloseDisplay(d);
    return w != 0;
}

/* ---------- strut: reserve space so windows do not cover the panel ---------- */
static void set_strut(GtkWidget *win, GdkRectangle *mon)
{
    GdkWindow *gw = gtk_widget_get_window(win);
    GdkScreen *scr = gtk_widget_get_screen(win);
    gulong st[12] = { 0 };
    st[3]  = PANEL_HEIGHT + (gdk_screen_get_height(scr) - (mon->y + mon->height));
    st[10] = mon->x;
    st[11] = mon->x + mon->width - 1;
    gdk_property_change(gw, gdk_atom_intern("_NET_WM_STRUT_PARTIAL", FALSE),
                        gdk_atom_intern("CARDINAL", FALSE), 32, GDK_PROP_MODE_REPLACE,
                        (const guchar *)st, 12);
    gulong s4[4] = { 0, 0, 0, st[3] };
    gdk_property_change(gw, gdk_atom_intern("_NET_WM_STRUT", FALSE),
                        gdk_atom_intern("CARDINAL", FALSE), 32, GDK_PROP_MODE_REPLACE,
                        (const guchar *)s4, 4);
}

/* ---------- CSS: light/dark according to Settings > Appearance ---------- */
static void load_css(void)
{
    HdeThemeInfo ti;
    hde_theme_info_load(&ti);
    gboolean dark = ti.style != HDE_STYLE_LIGHT;
    const char *bg     = dark ? "#1e222a" : "#f3f4f6";
    const char *fg     = dark ? "#e6e9ef" : "#1f2329";
    const char *hover  = dark ? "#343b48" : "#dfe3e8";
    const char *sub    = dark ? "#aeb7c6" : "#5f6672";
    const char *runbg  = dark ? "#303744" : "#e4e7eb";
    const char *border = dark ? "#2c323d" : "#cfd4da";
    const char *popbg  = dark ? "#252a33" : "#ffffff";
    const char *popbd  = dark ? "#3a4150" : "#d0d4da";
    const char *entry  = dark ? "#1b1f26" : "#f4f5f7";
    char *css = g_strdup_printf(
        ".hde-panel { background: %s; color: %s; border-top: 1px solid %s; }"
        ".hde-panel button { background: transparent; background-image: none; border: none; border-radius: 4px;"
        "  padding: 2px 8px; color: %s; box-shadow: none; text-shadow: none; -gtk-icon-shadow: none; }"
        ".hde-panel button:hover { background: %s; }"
        ".hde-panel button:checked { background: %s; color: white; }"
        ".hde-panel .menu-btn, .hde-panel .menu-btn label { font-weight: bold; background: %s; color: white; }"
        ".hde-panel .menu-btn:hover { background: shade(%s, 1.15); }"
        ".hde-panel .run-btn { background: %s; }"
        ".hde-panel .run-btn:hover { background: %s; }"
        ".hde-panel .clock-btn { padding: 0 8px; }"
        ".hde-panel .clock-box { min-width: 108px; }"
        ".hde-panel .clock-time { font-weight: 700; font-size: 12px; }"
        ".hde-panel .clock-date { font-size: 8px; color: %s; }"
        ".hde-panel .status-btn { padding: 2px 5px; }"
        ".hde-panel .notif-count { background: %s; color: white; border-radius: 8px; padding: 0 5px;"
        "  font-size: 9px; font-weight: bold; }"
        ".hde-panel label { color: %s; }"
        ".hde-osd, .hde-notification, .hde-search, .hde-calendar { background: %s; color: %s;"
        "  border: 1px solid %s; border-radius: 0; }"
        /* rounded corners only when the WM composites (hde_popup_setup_alpha adds the .rounded class), avoiding black corners */
        ".hde-osd.rounded, .hde-notification.rounded, .hde-search.rounded, .hde-calendar.rounded { border-radius: 10px; }"
        ".hde-osd label, .hde-notification label, .hde-search label, .hde-calendar label { color: %s; }"
        ".hde-osd levelbar trough { min-height: 6px; border-radius: 3px; background: %s; border: none; }"
        ".hde-osd levelbar block.filled { background: %s; border-radius: 3px; border: none; min-height: 6px; }"
        ".hde-osd .osd-text { font-weight: bold; }"
        ".hde-notification.critical { border-left: 4px solid #e01b24; }"
        ".hde-notification .notif-summary { font-weight: bold; }"
        ".hde-notification .notif-app { font-size: 9px; color: %s; }"
        ".hde-notification .notif-body { color: %s; }"
        ".hde-notification button { padding: 3px 8px; background: %s; background-image: none; border: 1px solid %s;"
        "  border-radius: 6px; box-shadow: none; text-shadow: none; }"
        ".hde-notification button:hover { background: shade(%s, 1.2); }"
        ".hde-notification button label { color: %s; }"
        ".hde-notification button.notif-close { background: transparent; border: none; padding: 2px; }"
        ".hde-search entry { background: %s; color: %s; border-radius: 8px; padding: 6px 8px; min-height: 26px; }"
        ".hde-search list, .hde-search scrolledwindow, .hde-search viewport { background: transparent; }"
        ".hde-search row { border-radius: 8px; color: %s; }"
        ".hde-search row:selected { background: %s; color: white; }"
        ".hde-search row:selected label { color: white; }"
        ".hde-search .search-title { font-weight: 600; }"
        ".hde-search .search-desc { font-size: 9px; color: %s; }"
        ".hde-calendar .cal-title { font-weight: bold; font-size: 12px; }"
        ".hde-calendar calendar { background: transparent; color: %s; border: none; }"
        ".hde-calendar calendar:selected { background: %s; color: white; border-radius: 4px; }",
        bg, fg, border,
        fg, hover, ti.accent, ti.accent, ti.accent, runbg, hover, sub, ti.accent, fg,
        popbg, fg, popbd, fg, hover, ti.accent,
        sub, fg,
        runbg, popbd, runbg, fg,
        entry, fg, fg, ti.accent, sub,
        fg, ti.accent);
    if (!panel_css) {
        panel_css = gtk_css_provider_new();
        gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(panel_css),
                                                  GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
    GError *err = NULL;
    if (!gtk_css_provider_load_from_data(panel_css, css, -1, &err)) {
        g_printerr("hde-panel: CSS: %s\n", err->message);
        g_clear_error(&err);
    }
    g_free(css);
    hde_theme_info_clear(&ti);
}

static void on_theme_changed(gpointer d)
{
    (void)d;
    load_css();
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
           "       hde-panel --osd-volume    show the volume OSD\n"
           "       hde-panel --refresh       refresh the status area\n");
}

int main(int argc, char **argv)
{
    static const struct { const char *opt; long cmd; } cli[] = {
        { "--menu", HDE_CMD_MENU }, { "--search", HDE_CMD_SEARCH }, { "--run", HDE_CMD_RUN },
        { "--power", HDE_CMD_POWER }, { "--osd-volume", HDE_CMD_OSD_VOLUME }, { "--refresh", HDE_CMD_REFRESH },
    };
    for (int i = 1; i < argc; i++) {
        for (guint k = 0; k < G_N_ELEMENTS(cli); k++)
            if (!strcmp(argv[i], cli[k].opt)) return send_cli_command(cli[k].cmd);
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(); return 0; }
    }
    if (panel_running()) {
        fprintf(stderr, "hde-panel: another hde-panel is already running\n");
        return 0;
    }

    gtk_init(&argc, &argv);
    debug_on = g_getenv("HDE_DEBUG") != NULL;
    wnck_set_client_type(WNCK_CLIENT_TYPE_PAGER);
    hde_theme_apply_process();
    load_css();
    hde_theme_watch(on_theme_changed, NULL);
    hde_notify_init();

    GdkDisplay *dpy = gdk_display_get_default();
    GdkMonitor *m = gdk_display_get_primary_monitor(dpy);
    if (!m) m = gdk_display_get_monitor(dpy, 0);
    GdkRectangle geo;
    gdk_monitor_get_geometry(m, &geo);

    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    panel_win = win;
    gtk_window_set_title(GTK_WINDOW(win), "hde-panel");
    gtk_window_set_type_hint(GTK_WINDOW(win), GDK_WINDOW_TYPE_HINT_DOCK);
    gtk_window_set_decorated(GTK_WINDOW(win), FALSE);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(win), TRUE);
    gtk_window_set_skip_pager_hint(GTK_WINDOW(win), TRUE);
    gtk_window_stick(GTK_WINDOW(win));
    gtk_window_set_keep_above(GTK_WINDOW(win), TRUE);   /* always above the desktop, even if the WM ignores the DOCK hint */
    gtk_widget_set_size_request(win, geo.width, PANEL_HEIGHT);
    gtk_window_set_default_size(GTK_WINDOW(win), geo.width, PANEL_HEIGHT);
    gtk_window_move(GTK_WINDOW(win), geo.x, geo.y + geo.height - PANEL_HEIGHT);
    gtk_style_context_add_class(gtk_widget_get_style_context(win), "hde-panel");
    g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(box), 3);
    gtk_container_add(GTK_CONTAINER(win), box);

    menu_btn = gtk_button_new_with_label("  ☰  Menu  ");
    gtk_style_context_add_class(gtk_widget_get_style_context(menu_btn), "menu-btn");
    gtk_widget_set_tooltip_text(menu_btn, "Start menu (Super)");
    g_signal_connect(menu_btn, "clicked", G_CALLBACK(on_menu_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(box), menu_btn, FALSE, FALSE, 0);

    GtkWidget *desk_btn = gtk_button_new_from_icon_name("user-desktop-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(desk_btn, "Show Desktop (Super+D)");
    g_signal_connect(desk_btn, "clicked", G_CALLBACK(on_show_desktop), NULL);
    gtk_box_pack_start(GTK_BOX(box), desk_btn, FALSE, FALSE, 0);

    GtkWidget *run_btn = gtk_button_new_with_label("⌘ Run");
    gtk_style_context_add_class(gtk_widget_get_style_context(run_btn), "run-btn");
    gtk_widget_set_tooltip_text(run_btn, "Run a command (Super+R)");
    g_signal_connect(run_btn, "clicked", G_CALLBACK(on_run_command), NULL);
    gtk_box_pack_start(GTK_BOX(box), run_btn, FALSE, FALSE, 0);

    GtkWidget *tasks = wnck_tasklist_new();
    wnck_tasklist_set_grouping(WNCK_TASKLIST(tasks), WNCK_TASKLIST_AUTO_GROUP);
    wnck_tasklist_set_include_all_workspaces(WNCK_TASKLIST(tasks), FALSE);
    wnck_tasklist_set_button_relief(WNCK_TASKLIST(tasks), GTK_RELIEF_NONE);
    gtk_box_pack_start(GTK_BOX(box), tasks, TRUE, TRUE, 0);

    GtkWidget *pager = wnck_pager_new();
    wnck_pager_set_display_mode(WNCK_PAGER(pager), WNCK_PAGER_DISPLAY_CONTENT);
    wnck_pager_set_n_rows(WNCK_PAGER(pager), 1);
    gtk_box_pack_start(GTK_BOX(box), pager, FALSE, FALSE, 4);

    /* right side, from right to left: clock | notification bell | status | system tray */
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
    gtk_box_pack_end(GTK_BOX(box), hde_notify_button_new(), FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(box), hde_status_new(), FALSE, FALSE, 2);
    gtk_box_pack_end(GTK_BOX(box), hde_tray_new(), FALSE, FALSE, 4);

    update_clock(NULL);
    g_timeout_add_seconds(1, update_clock, NULL);

    gtk_widget_show_all(win);
    set_strut(win, &geo);
    publish_panel_window();
    gdk_window_raise(gtk_widget_get_window(win));   /* never covered by hde-desktop if they start in the wrong order */

    g_unix_signal_add(SIGTERM, on_unix_signal, NULL);
    g_unix_signal_add(SIGINT, on_unix_signal, NULL);
    gtk_main();
    unpublish_panel_window();
    return 0;
}
