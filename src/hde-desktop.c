/* hde-desktop: hình nền + icon trên màn hình nền (đọc thư mục ~/Desktop) */
#include <gtk/gtk.h>
#include <gio/gdesktopappinfo.h>
#include <string.h>

#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#define PANEL_HEIGHT 34
#define CELL_W 100
#define CELL_H 108
#define MARGIN 16

static GtkWidget *win, *fixed;
static GdkPixbuf *wallpaper;
static char *wallpaper_path;
static GtkWidget *selected;
static GdkRectangle mon;

/* ---------- cấu hình ---------- */
static char *config_file(void)
{
    return g_build_filename(g_get_user_config_dir(), "hde", "config.ini", NULL);
}

static void load_wallpaper(const char *path)
{
    g_clear_object(&wallpaper);
    g_free(wallpaper_path);
    wallpaper_path = path ? g_strdup(path) : NULL;
    if (path) wallpaper = gdk_pixbuf_new_from_file(path, NULL);
    if (win) gtk_widget_queue_draw(win);
}

static void load_config(void)
{
    GKeyFile *kf = g_key_file_new();
    char *cf = config_file();
    if (g_key_file_load_from_file(kf, cf, G_KEY_FILE_NONE, NULL)) {
        char *wp = g_key_file_get_string(kf, "desktop", "wallpaper", NULL);
        if (wp) { load_wallpaper(wp); g_free(wp); }
    }
    g_key_file_free(kf);
    g_free(cf);
}

static void save_config(void)
{
    GKeyFile *kf = g_key_file_new();
    char *cf = config_file();
    g_key_file_load_from_file(kf, cf, G_KEY_FILE_NONE, NULL);
    if (wallpaper_path) g_key_file_set_string(kf, "desktop", "wallpaper", wallpaper_path);
    char *dir = g_path_get_dirname(cf);
    g_mkdir_with_parents(dir, 0755);
    g_key_file_save_to_file(kf, cf, NULL);
    g_free(dir); g_free(cf);
    g_key_file_free(kf);
}

/* ---------- vẽ nền ---------- */
static gboolean on_draw(GtkWidget *w, cairo_t *cr, gpointer data)
{
    int W = gtk_widget_get_allocated_width(w);
    int H = gtk_widget_get_allocated_height(w);

    if (wallpaper) {
        double pw = gdk_pixbuf_get_width(wallpaper), ph = gdk_pixbuf_get_height(wallpaper);
        double s = MAX(W / pw, H / ph);
        cairo_save(cr);
        cairo_translate(cr, -(pw * s - W) / 2, -(ph * s - H) / 2);
        cairo_scale(cr, s, s);
        gdk_cairo_set_source_pixbuf(cr, wallpaper, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
        cairo_paint(cr);
        cairo_restore(cr);
    } else {
        cairo_pattern_t *g = cairo_pattern_create_linear(0, 0, W, H);
        cairo_pattern_add_color_stop_rgb(g, 0.0, 0.10, 0.18, 0.36);
        cairo_pattern_add_color_stop_rgb(g, 1.0, 0.22, 0.42, 0.62);
        cairo_set_source(cr, g);
        cairo_paint(cr);
        cairo_pattern_destroy(g);
    }
    return FALSE;   /* tiếp tục vẽ các icon con */
}

/* ---------- icon ---------- */
static void activate_target(const char *path)
{
    if (g_str_has_suffix(path, ".desktop")) {
        GDesktopAppInfo *a = g_desktop_app_info_new_from_filename(path);
        if (a) { g_app_info_launch(G_APP_INFO(a), NULL, NULL, NULL); g_object_unref(a); return; }
    }
    char *uri = g_filename_to_uri(path, NULL, NULL);
    if (uri) { g_app_info_launch_default_for_uri(uri, NULL, NULL); g_free(uri); }
}

static void select_icon(GtkWidget *ev)
{
    if (selected) gtk_style_context_remove_class(gtk_widget_get_style_context(selected), "selected");
    selected = ev;
    if (ev) gtk_style_context_add_class(gtk_widget_get_style_context(ev), "selected");
}

static gboolean on_icon_press(GtkWidget *ev, GdkEventButton *e, gpointer data)
{
    if (e->button != 1) return FALSE;
    select_icon(ev);
    if (e->type == GDK_2BUTTON_PRESS) {
        const char *path = g_object_get_data(G_OBJECT(ev), "target");
        if (path) activate_target(path);
    }
    return TRUE;
}

static GtkWidget *make_icon(const char *name, GIcon *icon, const char *path)
{
    GtkWidget *ev = gtk_event_box_new();
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(ev), FALSE);
    gtk_widget_add_events(ev, GDK_BUTTON_PRESS_MASK);
    gtk_style_context_add_class(gtk_widget_get_style_context(ev), "desk-icon");
    g_object_set_data_full(G_OBJECT(ev), "target", g_strdup(path), g_free);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_size_request(box, CELL_W - 8, CELL_H - 8);
    GtkWidget *img = gtk_image_new_from_gicon(icon, GTK_ICON_SIZE_DIALOG);
    gtk_image_set_pixel_size(GTK_IMAGE(img), 48);
    GtkWidget *lbl = gtk_label_new(name);
    gtk_label_set_line_wrap(GTK_LABEL(lbl), TRUE);
    gtk_label_set_line_wrap_mode(GTK_LABEL(lbl), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
    gtk_label_set_lines(GTK_LABEL(lbl), 2);
    gtk_label_set_max_width_chars(GTK_LABEL(lbl), 12);
    gtk_label_set_justify(GTK_LABEL(lbl), GTK_JUSTIFY_CENTER);
    gtk_box_pack_start(GTK_BOX(box), img, FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(box), lbl, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(ev), box);
    g_signal_connect(ev, "button-press-event", G_CALLBACK(on_icon_press), NULL);
    return ev;
}

static gint cmp_names(gconstpointer a, gconstpointer b)
{
    return g_utf8_collate(a, b);
}

static void destroy_child(GtkWidget *w, gpointer d) { gtk_widget_destroy(w); }

static void reload_icons(void)
{
    selected = NULL;
    gtk_container_foreach(GTK_CONTAINER(fixed), destroy_child, NULL);

    int rows = MAX(1, (mon.height - PANEL_HEIGHT - 2 * MARGIN) / CELL_H);
    int idx = 0;

    #define PLACE(w) do { gtk_fixed_put(GTK_FIXED(fixed), (w), \
            mon.x + MARGIN + (idx / rows) * CELL_W, mon.y + MARGIN + (idx % rows) * CELL_H); idx++; } while (0)

    /* Thư mục Home luôn có */
    GIcon *home_icon = g_themed_icon_new("user-home");
    GtkWidget *home = make_icon("Thư mục cá nhân", home_icon, g_get_home_dir());
    g_object_unref(home_icon);
    PLACE(home);

    const char *ddir = g_get_user_special_dir(G_USER_DIRECTORY_DESKTOP);
    GDir *d = ddir ? g_dir_open(ddir, 0, NULL) : NULL;
    if (d) {
        GList *names = NULL;
        const char *fn;
        while ((fn = g_dir_read_name(d)))
            if (fn[0] != '.') names = g_list_prepend(names, g_strdup(fn));
        g_dir_close(d);
        names = g_list_sort(names, cmp_names);

        for (GList *l = names; l; l = l->next) {
            char *path = g_build_filename(ddir, l->data, NULL);
            GIcon *icon = NULL;
            char *label = g_strdup(l->data);

            if (g_str_has_suffix(path, ".desktop")) {
                GDesktopAppInfo *a = g_desktop_app_info_new_from_filename(path);
                if (a) {
                    icon = g_object_ref(g_app_info_get_icon(G_APP_INFO(a)) ?
                                        g_app_info_get_icon(G_APP_INFO(a)) : g_themed_icon_new("application-x-executable"));
                    g_free(label);
                    label = g_strdup(g_app_info_get_display_name(G_APP_INFO(a)));
                    g_object_unref(a);
                }
            }
            if (!icon) {
                GFile *f = g_file_new_for_path(path);
                GFileInfo *fi = g_file_query_info(f, "standard::icon", 0, NULL, NULL);
                if (fi && g_file_info_get_icon(fi)) icon = g_object_ref(g_file_info_get_icon(fi));
                g_clear_object(&fi);
                g_object_unref(f);
            }
            if (!icon) icon = g_themed_icon_new("text-x-generic");

            GtkWidget *w = make_icon(label, icon, path);
            PLACE(w);
            g_object_unref(icon); g_free(label); g_free(path);
        }
        g_list_free_full(names, g_free);
    }
    gtk_widget_show_all(fixed);
}

/* ---------- menu chuột phải ---------- */
static void m_terminal(GtkMenuItem *i, gpointer d)
{
    const char *terms[] = { "x-terminal-emulator", "xfce4-terminal", "lxterminal", "gnome-terminal",
                            "konsole", "xterm", NULL };
    for (int k = 0; terms[k]; k++) {
        char *p = g_find_program_in_path(terms[k]);
        if (p) { g_spawn_command_line_async(p, NULL); g_free(p); return; }
    }
}

static void m_wallpaper(GtkMenuItem *i, gpointer d)
{
    GtkWidget *dlg = gtk_file_chooser_dialog_new("Chọn hình nền", NULL, GTK_FILE_CHOOSER_ACTION_OPEN,
        "_Hủy", GTK_RESPONSE_CANCEL, "_Chọn", GTK_RESPONSE_ACCEPT, NULL);
    GtkFileFilter *ff = gtk_file_filter_new();
    gtk_file_filter_set_name(ff, "Hình ảnh");
    gtk_file_filter_add_pixbuf_formats(ff);
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), ff);
    gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dlg), g_get_user_special_dir(G_USER_DIRECTORY_PICTURES));
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char *f = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        load_wallpaper(f);
        save_config();
        g_free(f);
    }
    gtk_widget_destroy(dlg);
}

static void m_refresh(GtkMenuItem *i, gpointer d) { reload_icons(); }

static gboolean on_bg_press(GtkWidget *w, GdkEventButton *e, gpointer d)
{
    if (e->type != GDK_BUTTON_PRESS) return FALSE;
    if (e->button == 1) { select_icon(NULL); return FALSE; }
    if (e->button != 3) return FALSE;

    GtkWidget *menu = gtk_menu_new();
    struct { const char *l; GCallback cb; } items[] = {
        { "Mở Terminal",     G_CALLBACK(m_terminal) },
        { "Đổi hình nền…",   G_CALLBACK(m_wallpaper) },
        { "Làm mới",         G_CALLBACK(m_refresh) },
    };
    for (guint k = 0; k < G_N_ELEMENTS(items); k++) {
        GtkWidget *it = gtk_menu_item_new_with_label(items[k].l);
        g_signal_connect(it, "activate", items[k].cb, NULL);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), it);
    }
    gtk_widget_show_all(menu);
    g_signal_connect(menu, "selection-done", G_CALLBACK(gtk_widget_destroy), NULL);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)e);
    return TRUE;
}

static void on_dir_changed(GFileMonitor *m, GFile *f, GFile *o, GFileMonitorEvent ev, gpointer d)
{
    reload_icons();
}

static void load_css(void)
{
    const char *css =
        ".desk-icon label { color: white; text-shadow: 1px 1px 2px black, 0 0 4px black; }"
        ".desk-icon.selected { background: rgba(61,111,217,0.55); border-radius: 6px; }"
        ".desk-icon:hover { background: rgba(255,255,255,0.12); border-radius: 6px; }";
    GtkCssProvider *p = gtk_css_provider_new();
    gtk_css_provider_load_from_data(p, css, -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(p), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(p);
}

int main(int argc, char **argv)
{
    gtk_init(&argc, &argv);
    load_css();

    GdkDisplay *dpy = gdk_display_get_default();
    GdkMonitor *m = gdk_display_get_primary_monitor(dpy);
    if (!m) m = gdk_display_get_monitor(dpy, 0);
    gdk_monitor_get_geometry(m, &mon);

    GdkScreen *scr = gdk_display_get_default_screen(dpy);
    int sw = gdk_screen_get_width(scr), sh = gdk_screen_get_height(scr);

    win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), "hde-desktop");
    gtk_window_set_type_hint(GTK_WINDOW(win), GDK_WINDOW_TYPE_HINT_DESKTOP);
    gtk_window_set_decorated(GTK_WINDOW(win), FALSE);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(win), TRUE);
    gtk_window_set_skip_pager_hint(GTK_WINDOW(win), TRUE);
    gtk_window_stick(GTK_WINDOW(win));
    gtk_widget_set_size_request(win, sw, sh);
    gtk_window_set_default_size(GTK_WINDOW(win), sw, sh);
    gtk_window_move(GTK_WINDOW(win), 0, 0);
    gtk_widget_set_app_paintable(win, TRUE);
    gtk_widget_add_events(win, GDK_BUTTON_PRESS_MASK);
    g_signal_connect(win, "draw", G_CALLBACK(on_draw), NULL);
    g_signal_connect(win, "button-press-event", G_CALLBACK(on_bg_press), NULL);
    g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    fixed = gtk_fixed_new();
    gtk_container_add(GTK_CONTAINER(win), fixed);

    load_config();
    reload_icons();

    const char *ddir = g_get_user_special_dir(G_USER_DIRECTORY_DESKTOP);
    if (ddir) {
        GFile *f = g_file_new_for_path(ddir);
        GFileMonitor *fm = g_file_monitor_directory(f, G_FILE_MONITOR_NONE, NULL, NULL);
        if (fm) g_signal_connect(fm, "changed", G_CALLBACK(on_dir_changed), NULL);
        g_object_unref(f);
    }

    gtk_widget_show_all(win);
    gtk_main();
    return 0;
}
