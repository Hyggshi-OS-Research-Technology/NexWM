/* hde-search: ô tìm ứng dụng của Start menu. */
#include "hde-search.h"
#include "hde-osd.h"
#include <gio/gdesktopappinfo.h>
#include <gdk/gdkx.h>
#include <string.h>

#define PANEL_HEIGHT 34
#define SEARCH_W 480
#define SEARCH_H 520
#define MAX_RESULTS 40

static GtkWidget *swin, *sentry, *slist, *sscroll, *sempty;
static guint32 show_time;

void hde_window_force_activate(GtkWidget *window, guint32 time)
{
    GdkWindow *gw = gtk_widget_get_window(window);
    if (!gw || !GDK_IS_X11_WINDOW(gw)) return;
    Display *d = GDK_WINDOW_XDISPLAY(gw);
    if (!time) time = gdk_x11_get_server_time(gw);
    XEvent e;
    memset(&e, 0, sizeof e);
    e.xclient.type = ClientMessage;
    e.xclient.window = GDK_WINDOW_XID(gw);
    e.xclient.message_type = XInternAtom(d, "_NET_ACTIVE_WINDOW", False);
    e.xclient.format = 32;
    e.xclient.data.l[0] = 2;            /* nguồn: pager -> WM không áp dụng chống-cướp-focus */
    e.xclient.data.l[1] = (long)time;
    XSendEvent(d, DefaultRootWindow(d), False, SubstructureRedirectMask | SubstructureNotifyMask, &e);
    XFlush(d);
}

static void search_hide(void)
{
    if (swin) gtk_widget_hide(swin);
}

void hde_search_hide(void)
{
    search_hide();
}

gboolean hde_search_visible(void)
{
    return swin && gtk_widget_get_visible(swin);
}

static void add_row(GAppInfo *app, const char *command)
{
    GtkWidget *row = gtk_list_box_row_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_margin_start(box, 10); gtk_widget_set_margin_end(box, 10);
    gtk_widget_set_margin_top(box, 5); gtk_widget_set_margin_bottom(box, 5);

    GIcon *icon = app ? g_app_info_get_icon(app) : NULL;
    GtkWidget *img = icon ? gtk_image_new_from_gicon(icon, GTK_ICON_SIZE_DND)
                          : gtk_image_new_from_icon_name(command ? "utilities-terminal" : "application-x-executable",
                                                         GTK_ICON_SIZE_DND);
    gtk_image_set_pixel_size(GTK_IMAGE(img), 32);
    gtk_box_pack_start(GTK_BOX(box), img, FALSE, FALSE, 0);

    GtkWidget *texts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
    gtk_widget_set_valign(texts, GTK_ALIGN_CENTER);
    char *title = app ? g_strdup(g_app_info_get_display_name(app)) : g_strdup_printf("Run “%s”", command);
    const char *desc = NULL;
    if (app) {
        desc = g_app_info_get_description(app);
        if ((!desc || !*desc) && G_IS_DESKTOP_APP_INFO(app)) desc = g_desktop_app_info_get_generic_name(G_DESKTOP_APP_INFO(app));
        if (!desc || !*desc) desc = g_app_info_get_executable(app);
    } else {
        desc = "Run this command";
    }
    GtkWidget *t = gtk_label_new(title);
    gtk_label_set_xalign(GTK_LABEL(t), 0);
    gtk_label_set_ellipsize(GTK_LABEL(t), PANGO_ELLIPSIZE_END);
    gtk_style_context_add_class(gtk_widget_get_style_context(t), "search-title");
    GtkWidget *dl = gtk_label_new(desc ? desc : "");
    gtk_label_set_xalign(GTK_LABEL(dl), 0);
    gtk_label_set_ellipsize(GTK_LABEL(dl), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(dl), 1);
    gtk_widget_set_hexpand(dl, TRUE);
    gtk_style_context_add_class(gtk_widget_get_style_context(dl), "search-desc");
    gtk_box_pack_start(GTK_BOX(texts), t, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(texts), dl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), texts, TRUE, TRUE, 0);
    g_free(title);

    gtk_container_add(GTK_CONTAINER(row), box);
    if (app) g_object_set_data_full(G_OBJECT(row), "hde-app", g_object_ref(app), g_object_unref);
    if (command) g_object_set_data_full(G_OBJECT(row), "hde-cmd", g_strdup(command), g_free);
    gtk_widget_show_all(row);
    gtk_container_add(GTK_CONTAINER(slist), row);
}

static void destroy_cb(GtkWidget *w, gpointer d) { (void)d; gtk_widget_destroy(w); }

static gint cmp_app(gconstpointer a, gconstpointer b)
{
    return g_utf8_collate(g_app_info_get_display_name((GAppInfo *)a), g_app_info_get_display_name((GAppInfo *)b));
}

static void update_results(void)
{
    gtk_container_foreach(GTK_CONTAINER(slist), destroy_cb, NULL);
    char *query = g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(sentry))));
    int n = 0;
    if (*query) {
        GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
        gchar ***groups = g_desktop_app_info_search(query);
        for (int g = 0; groups && groups[g]; g++) {
            for (int i = 0; groups[g][i] && n < MAX_RESULTS; i++) {
                const char *id = groups[g][i];
                if (g_hash_table_contains(seen, id)) continue;
                GDesktopAppInfo *app = g_desktop_app_info_new(id);
                if (app && g_app_info_should_show(G_APP_INFO(app))) {
                    add_row(G_APP_INFO(app), NULL);
                    g_hash_table_add(seen, g_strdup(id));
                    n++;
                }
                g_clear_object(&app);
            }
            g_strfreev(groups[g]);
        }
        g_free(groups);
        g_hash_table_destroy(seen);

        char **argv = NULL;
        if (g_shell_parse_argv(query, NULL, &argv, NULL) && argv && argv[0]) {
            char *p = g_find_program_in_path(argv[0]);
            if (p) { add_row(NULL, query); n++; }
            g_free(p);
        }
        g_strfreev(argv);
    } else {
        GList *all = g_app_info_get_all(), *apps = NULL;
        for (GList *l = all; l; l = l->next)
            if (G_IS_DESKTOP_APP_INFO(l->data) && g_app_info_should_show(l->data)) apps = g_list_prepend(apps, l->data);
        apps = g_list_sort(apps, cmp_app);
        for (GList *l = apps; l; l = l->next) { add_row(l->data, NULL); n++; }
        g_list_free(apps);
        g_list_free_full(all, g_object_unref);
    }
    g_free(query);
    gtk_widget_set_visible(sempty, n == 0);
    gtk_widget_set_visible(sscroll, n > 0);
    GtkListBoxRow *first = gtk_list_box_get_row_at_index(GTK_LIST_BOX(slist), 0);
    if (first) gtk_list_box_select_row(GTK_LIST_BOX(slist), first);
    gtk_adjustment_set_value(gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(sscroll)), 0);
}

static void launch_row(GtkListBoxRow *row)
{
    if (!row) return;
    GAppInfo *app = g_object_get_data(G_OBJECT(row), "hde-app");
    const char *cmd = g_object_get_data(G_OBJECT(row), "hde-cmd");
    GdkAppLaunchContext *ctx = gdk_display_get_app_launch_context(gdk_display_get_default());
    gdk_app_launch_context_set_timestamp(ctx, gtk_get_current_event_time());
    GError *e = NULL;
    if (app) g_app_info_launch(app, NULL, G_APP_LAUNCH_CONTEXT(ctx), &e);
    else if (cmd) g_spawn_command_line_async(cmd, &e);
    if (e) {
        g_printerr("hde-search: %s\n", e->message);
        g_clear_error(&e);
    }
    g_object_unref(ctx);
    search_hide();
}

static void scroll_to_row(GtkListBoxRow *r)
{
    GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(sscroll));
    GtkAllocation a;
    gtk_widget_get_allocation(GTK_WIDGET(r), &a);
    double v = gtk_adjustment_get_value(adj), page = gtk_adjustment_get_page_size(adj);
    if (a.y < v) gtk_adjustment_set_value(adj, a.y);
    else if (a.y + a.height > v + page) gtk_adjustment_set_value(adj, a.y + a.height - page);
}

static void move_selection(int delta)
{
    GtkListBoxRow *cur = gtk_list_box_get_selected_row(GTK_LIST_BOX(slist));
    int idx = cur ? gtk_list_box_row_get_index(cur) + delta : 0;
    GtkListBoxRow *r = gtk_list_box_get_row_at_index(GTK_LIST_BOX(slist), MAX(idx, 0));
    if (r) {
        gtk_list_box_select_row(GTK_LIST_BOX(slist), r);
        scroll_to_row(r);
    }
}

static gboolean on_entry_key(GtkWidget *w, GdkEventKey *e, gpointer d)
{
    (void)w; (void)d;
    switch (e->keyval) {
    case GDK_KEY_Escape: search_hide(); return TRUE;
    case GDK_KEY_Down: move_selection(1); return TRUE;
    case GDK_KEY_Up: move_selection(-1); return TRUE;
    case GDK_KEY_Page_Down: move_selection(6); return TRUE;
    case GDK_KEY_Page_Up: move_selection(-6); return TRUE;
    default: return FALSE;
    }
}

static void on_entry_activate(GtkEntry *e, gpointer d)
{
    (void)e; (void)d;
    launch_row(gtk_list_box_get_selected_row(GTK_LIST_BOX(slist)));
}

static void on_row_activated(GtkListBox *b, GtkListBoxRow *r, gpointer d)
{
    (void)b; (void)d;
    launch_row(r);
}

static void on_search_changed(GtkSearchEntry *e, gpointer d)
{
    (void)e; (void)d;
    update_results();
}

static gboolean on_focus_out(GtkWidget *w, GdkEventFocus *e, gpointer d)
{
    (void)w; (void)e; (void)d;
    search_hide();
    return FALSE;
}

static gboolean on_map(GtkWidget *w, GdkEvent *e, gpointer d)
{
    (void)e; (void)d;
    hde_window_force_activate(w, show_time);
    return FALSE;
}

static void build(void)
{
    swin = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(swin), "Search applications");
    gtk_window_set_decorated(GTK_WINDOW(swin), FALSE);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(swin), TRUE);
    gtk_window_set_skip_pager_hint(GTK_WINDOW(swin), TRUE);
    gtk_window_set_keep_above(GTK_WINDOW(swin), TRUE);
    gtk_window_set_type_hint(GTK_WINDOW(swin), GDK_WINDOW_TYPE_HINT_DIALOG);
    gtk_window_set_resizable(GTK_WINDOW(swin), FALSE);
    gtk_widget_set_size_request(swin, SEARCH_W, SEARCH_H);
    gtk_style_context_add_class(gtk_widget_get_style_context(swin), "hde-search");
    hde_popup_setup_alpha(swin);
    g_signal_connect(swin, "delete-event", G_CALLBACK(gtk_widget_hide_on_delete), NULL);
    g_signal_connect(swin, "focus-out-event", G_CALLBACK(on_focus_out), NULL);
    g_signal_connect(swin, "map-event", G_CALLBACK(on_map), NULL);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);
    gtk_container_add(GTK_CONTAINER(swin), box);

    sentry = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(sentry), "Type to search apps, or enter a command…");
    g_signal_connect(sentry, "search-changed", G_CALLBACK(on_search_changed), NULL);
    g_signal_connect(sentry, "activate", G_CALLBACK(on_entry_activate), NULL);
    g_signal_connect(sentry, "key-press-event", G_CALLBACK(on_entry_key), NULL);
    gtk_box_pack_start(GTK_BOX(box), sentry, FALSE, FALSE, 0);

    sscroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sscroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    slist = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(slist), GTK_SELECTION_BROWSE);
    gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(slist), TRUE);
    g_signal_connect(slist, "row-activated", G_CALLBACK(on_row_activated), NULL);
    gtk_container_add(GTK_CONTAINER(sscroll), slist);
    gtk_box_pack_start(GTK_BOX(box), sscroll, TRUE, TRUE, 0);

    sempty = gtk_label_new("No matching applications");
    gtk_style_context_add_class(gtk_widget_get_style_context(sempty), "search-desc");
    gtk_box_pack_start(GTK_BOX(box), sempty, TRUE, TRUE, 0);
    gtk_widget_show_all(box);
}

void hde_search_show(const char *initial_text, guint32 time)
{
    if (!swin) build();
    GdkDisplay *dpy = gdk_display_get_default();
    GdkMonitor *m = gdk_display_get_primary_monitor(dpy);
    if (!m) m = gdk_display_get_monitor(dpy, 0);
    GdkRectangle geo = { 0, 0, 1024, 768 };
    if (m) gdk_monitor_get_geometry(m, &geo);
    gtk_window_move(GTK_WINDOW(swin), geo.x + 6, geo.y + geo.height - PANEL_HEIGHT - 6 - SEARCH_H);

    g_signal_handlers_block_by_func(sentry, on_search_changed, NULL);
    gtk_entry_set_text(GTK_ENTRY(sentry), initial_text ? initial_text : "");
    g_signal_handlers_unblock_by_func(sentry, on_search_changed, NULL);
    update_results();

    show_time = time;
    gtk_widget_show(swin);
    gtk_window_present_with_time(GTK_WINDOW(swin), time ? time : GDK_CURRENT_TIME);
    if (gtk_widget_get_mapped(swin)) hde_window_force_activate(swin, time);
    gtk_widget_grab_focus(sentry);
    gtk_editable_select_region(GTK_EDITABLE(sentry), -1, -1);
    gtk_editable_set_position(GTK_EDITABLE(sentry), -1);
}
