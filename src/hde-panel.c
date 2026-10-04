/* hde-panel: bottom panel — application menu, taskbar, workspace, and clock */
#define WNCK_I_KNOW_THIS_IS_UNSTABLE 1
#include <gtk/gtk.h>
#include <gio/gdesktopappinfo.h>
#include <gdk/gdkx.h>
#include <libwnck/libwnck.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include "hde-tray.h"

#define PANEL_HEIGHT 34
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

typedef struct {
    const char *title;
    const char *icon;
    const char *cats[5];
} Category;

static const Category categories[] = {
    { "Internet",          "applications-internet",    { "Network", "WebBrowser", "Email", NULL } },
    { "Office",         "applications-office",      { "Office", NULL } },
    { "Graphics",            "applications-graphics",    { "Graphics", NULL } },
    { "Audio & Video",  "applications-multimedia",  { "AudioVideo", "Audio", "Video", NULL } },
    { "Programming",         "applications-development",{ "Development", NULL } },
    { "Games",          "applications-games",       { "Game", NULL } },
    { "Utilities",          "applications-utilities",   { "Utility", NULL } },
    { "System",          "applications-system",      { "System", "Settings", NULL } },
};
#define N_CATS G_N_ELEMENTS(categories)

static GtkWidget *clock_label;
static GtkWidget *date_label;
static GtkWidget *app_menu;

/* ---------- đồng hồ ---------- */
static gboolean update_clock(gpointer data)
{
    GDateTime *now = g_date_time_new_now_local();
    char *time_s = g_date_time_format(now, "%H:%M");
    char *date_s = g_date_time_format(now, "%a  %d/%m/%Y");
    gtk_label_set_text(GTK_LABEL(clock_label), time_s);
    gtk_label_set_text(GTK_LABEL(date_label), date_s);
    g_free(time_s);
    g_free(date_s);
    g_date_time_unref(now);
    return G_SOURCE_CONTINUE;
}

/* ---------- menu ---------- */
static GtkWidget *make_item(const char *label, GIcon *gicon, const char *icon_name)
{
    GtkWidget *item = gtk_menu_item_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *img = gicon ? gtk_image_new_from_gicon(gicon, GTK_ICON_SIZE_LARGE_TOOLBAR)
                           : gtk_image_new_from_icon_name(icon_name ? icon_name : "application-x-executable",
                                                          GTK_ICON_SIZE_LARGE_TOOLBAR);
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
    GAppInfo *app = g_object_get_data(G_OBJECT(item), "app");
    GError *err = NULL;
    if (!g_app_info_launch(app, NULL, NULL, &err)) {
        g_printerr("hde-panel: %s\n", err->message);
        g_clear_error(&err);
    }
}

static gboolean app_in_category(GAppInfo *app, const Category *c)
{
    const char *cats = g_desktop_app_info_get_categories(G_DESKTOP_APP_INFO(app));
    if (!cats) return FALSE;
    char *wrapped = g_strdup_printf(";%s", cats);
    gboolean found = FALSE;
    for (int i = 0; c->cats[i] && !found; i++) {
        char *needle = g_strdup_printf(";%s;", c->cats[i]);
        char *w2 = g_strdup_printf("%s;", wrapped);
        found = strstr(w2, needle) != NULL;
        g_free(needle); g_free(w2);
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
    g_object_set_data_full(G_OBJECT(it), "app", g_object_ref(app), g_object_unref);
    g_signal_connect(it, "activate", G_CALLBACK(on_app_activate), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), it);
}

static void run_cmd(GtkMenuItem *item, gpointer cmd)
{
    g_spawn_command_line_async((const char *)cmd, NULL);
}

static void do_logout(GtkMenuItem *item, gpointer data)
{
    const char *pid = g_getenv("HDE_SESSION_PID");
    if (pid) kill(atoi(pid), SIGTERM);
    else gtk_main_quit();
}

static GtkWidget *build_menu(void)
{
    GtkWidget *menu = gtk_menu_new();
    GList *all = g_app_info_get_all();
    GList *apps = NULL;
    for (GList *l = all; l; l = l->next)
        if (g_app_info_should_show(l->data) && G_IS_DESKTOP_APP_INFO(l->data))
            apps = g_list_prepend(apps, l->data);
    apps = g_list_sort(apps, cmp_app);

    GHashTable *placed = g_hash_table_new(g_direct_hash, g_direct_equal);

    for (guint i = 0; i < N_CATS; i++) {
        GtkWidget *sub = gtk_menu_new();
        int count = 0;
        for (GList *l = apps; l; l = l->next) {
            if (app_in_category(l->data, &categories[i]) && !g_hash_table_contains(placed, l->data)) {
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

    GtkWidget *other = gtk_menu_new();
    int n_other = 0;
    for (GList *l = apps; l; l = l->next)
        if (!g_hash_table_contains(placed, l->data)) { add_app(other, l->data); n_other++; }
    if (n_other) {
        GtkWidget *it = make_item("Other", NULL, "applications-other");
        gtk_menu_item_set_submenu(GTK_MENU_ITEM(it), other);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), it);
    } else gtk_widget_destroy(other);

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

    struct { const char *label, *icon, *cmd; } sys[] = {
        { "Log Out", "system-log-out",  NULL },
        { "Restart", "system-reboot", "systemctl reboot" },
        { "Shut Down", "system-shutdown",   "systemctl poweroff" },
    };
    for (int i = 0; i < 3; i++) {
        GtkWidget *it = make_item(sys[i].label, NULL, sys[i].icon);
        if (sys[i].cmd) g_signal_connect(it, "activate", G_CALLBACK(run_cmd), (gpointer)sys[i].cmd);
        else g_signal_connect(it, "activate", G_CALLBACK(do_logout), NULL);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), it);
    }

    g_hash_table_destroy(placed);
    g_list_free(apps);
    g_list_free_full(all, g_object_unref);
    gtk_widget_show_all(menu);
    return menu;
}

static void on_menu_clicked(GtkButton *btn, gpointer data)
{
    if (app_menu) gtk_widget_destroy(app_menu);   /* dựng lại để luôn cập nhật app mới cài */
    app_menu = build_menu();
    g_object_ref_sink(app_menu);
    gtk_menu_popup_at_widget(GTK_MENU(app_menu), GTK_WIDGET(btn),
                             GDK_GRAVITY_NORTH_WEST, GDK_GRAVITY_SOUTH_WEST, NULL);
}

static void on_run_command(GtkButton *btn, gpointer data)
{
    (void)btn;
    (void)data;

    GtkWidget *dialog = gtk_dialog_new_with_buttons(
        "Run Command", NULL, GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "Cancel", GTK_RESPONSE_CANCEL,
        "Run", GTK_RESPONSE_ACCEPT,
        NULL);

    gtk_window_set_default_size(GTK_WINDOW(dialog), 440, 120);
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
    gtk_widget_show_all(dialog);
    gtk_widget_grab_focus(entry);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        const char *cmd = gtk_entry_get_text(GTK_ENTRY(entry));
        if (cmd && *cmd) {
            GError *err = NULL;
            if (!g_spawn_command_line_async(cmd, &err) && err) {
                GtkWidget *m = gtk_message_dialog_new(GTK_WINDOW(dialog),
                    GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE,
                    "Could not run command: %s", err->message);
                gtk_dialog_run(GTK_DIALOG(m));
                gtk_widget_destroy(m);
                g_clear_error(&err);
            }
        }
    }
    gtk_widget_destroy(dialog);
}

static void on_show_desktop(GtkButton *btn, gpointer data)
{
    WnckScreen *scr = wnck_screen_get_default();
    if (!scr) return;
    wnck_screen_force_update(scr);
    wnck_screen_toggle_showing_desktop(scr, !wnck_screen_get_showing_desktop(scr));
}

/* ---------- strut: chừa chỗ để cửa sổ không đè lên panel ---------- */
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
}

static void load_css(void)
{
    const char *css =
        ".hde-panel { background: #1e222a; color: #e6e9ef; }"
        ".hde-panel button { background: transparent; border: none; border-radius: 4px; padding: 2px 8px;"
        "                    color: #e6e9ef; box-shadow: none; text-shadow: none; }"
        ".hde-panel button:hover { background: #343b48; }"
        ".hde-panel button:checked { background: #3d6fd9; color: white; }"
        ".hde-panel .menu-btn { font-weight: bold; background: #3d6fd9; }"
        ".hde-panel .menu-btn:hover { background: #5585ea; }"
        ".hde-panel .run-btn { background: #303744; }"
        ".hde-panel .run-btn:hover { background: #414b5d; }"
        ".hde-panel .clock-box { padding: 0 8px; min-width: 116px; }"
        ".hde-panel .clock-time { font-weight: 700; font-size: 12px; }"
        ".hde-panel .clock-date { font-size: 8px; color: #aeb7c6; }"
        ".hde-panel label { color: #e6e9ef; }";
    GtkCssProvider *p = gtk_css_provider_new();
    gtk_css_provider_load_from_data(p, css, -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(p), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(p);
}

int main(int argc, char **argv)
{
    gtk_init(&argc, &argv);
    wnck_set_client_type(WNCK_CLIENT_TYPE_PAGER);
    load_css();

    GdkDisplay *dpy = gdk_display_get_default();
    GdkMonitor *m = gdk_display_get_primary_monitor(dpy);
    if (!m) m = gdk_display_get_monitor(dpy, 0);
    GdkRectangle geo;
    gdk_monitor_get_geometry(m, &geo);

    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), "hde-panel");
    gtk_window_set_type_hint(GTK_WINDOW(win), GDK_WINDOW_TYPE_HINT_DOCK);
    gtk_window_set_decorated(GTK_WINDOW(win), FALSE);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(win), TRUE);
    gtk_window_set_skip_pager_hint(GTK_WINDOW(win), TRUE);
    gtk_window_stick(GTK_WINDOW(win));
    gtk_widget_set_size_request(win, geo.width, PANEL_HEIGHT);
    gtk_window_set_default_size(GTK_WINDOW(win), geo.width, PANEL_HEIGHT);
    gtk_window_move(GTK_WINDOW(win), geo.x, geo.y + geo.height - PANEL_HEIGHT);
    gtk_style_context_add_class(gtk_widget_get_style_context(win), "hde-panel");
    g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(box), 3);
    gtk_container_add(GTK_CONTAINER(win), box);

    /* nút menu */
    GtkWidget *menu_btn = gtk_button_new_with_label("  ☰  Menu  ");
    gtk_style_context_add_class(gtk_widget_get_style_context(menu_btn), "menu-btn");
    g_signal_connect(menu_btn, "clicked", G_CALLBACK(on_menu_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(box), menu_btn, FALSE, FALSE, 0);

    /* nút hiện desktop */
    GtkWidget *desk_btn = gtk_button_new_from_icon_name("user-desktop-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(desk_btn, "Show Desktop");
    g_signal_connect(desk_btn, "clicked", G_CALLBACK(on_show_desktop), NULL);
    gtk_box_pack_start(GTK_BOX(box), desk_btn, FALSE, FALSE, 0);

    /* nút mở lệnh */
    GtkWidget *run_btn = gtk_button_new_with_label("⌘ Run");
    gtk_style_context_add_class(gtk_widget_get_style_context(run_btn), "run-btn");
    gtk_widget_set_tooltip_text(run_btn, "Run a command");
    g_signal_connect(run_btn, "clicked", G_CALLBACK(on_run_command), NULL);
    gtk_box_pack_start(GTK_BOX(box), run_btn, FALSE, FALSE, 0);

    /* taskbar */
    GtkWidget *tasks = wnck_tasklist_new();
    wnck_tasklist_set_grouping(WNCK_TASKLIST(tasks), WNCK_TASKLIST_AUTO_GROUP);
    wnck_tasklist_set_include_all_workspaces(WNCK_TASKLIST(tasks), FALSE);
    wnck_tasklist_set_button_relief(WNCK_TASKLIST(tasks), GTK_RELIEF_NONE);
    gtk_box_pack_start(GTK_BOX(box), tasks, TRUE, TRUE, 0);

    /* workspace pager */
    GtkWidget *pager = wnck_pager_new();
    wnck_pager_set_display_mode(WNCK_PAGER(pager), WNCK_PAGER_DISPLAY_CONTENT);
    wnck_pager_set_n_rows(WNCK_PAGER(pager), 1);
    gtk_box_pack_start(GTK_BOX(box), pager, FALSE, FALSE, 4);

    /* đồng hồ: giờ lớn + ngày nhỏ, luôn cập nhật mỗi giây */
    GtkWidget *clock_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(clock_box), "clock-box");
    gtk_widget_set_valign(clock_box, GTK_ALIGN_CENTER);

    clock_label = gtk_label_new("");
    gtk_style_context_add_class(gtk_widget_get_style_context(clock_label), "clock-time");
    gtk_label_set_xalign(GTK_LABEL(clock_label), 0.5f);
    gtk_box_pack_start(GTK_BOX(clock_box), clock_label, FALSE, FALSE, 0);

    date_label = gtk_label_new("");
    gtk_style_context_add_class(gtk_widget_get_style_context(date_label), "clock-date");
    gtk_label_set_xalign(GTK_LABEL(date_label), 0.5f);
    gtk_box_pack_start(GTK_BOX(clock_box), date_label, FALSE, FALSE, 0);

    /* system tray (nằm bên trái đồng hồ) */
    gtk_box_pack_end(GTK_BOX(box), clock_box, FALSE, FALSE, 4);
    gtk_box_pack_end(GTK_BOX(box), hde_tray_new(), FALSE, FALSE, 4);
    update_clock(NULL);
    g_timeout_add_seconds(1, update_clock, NULL);

    gtk_widget_show_all(win);
    set_strut(win, &geo);

    gtk_main();
    return 0;
}
