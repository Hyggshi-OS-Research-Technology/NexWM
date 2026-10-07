/* window.c — Hyggshi Files: a window.
 *
 *  [<] [>] [^]  [ Home › Documents › Work      ] [search] [view] [menu]     tool bar (path bar / location / search)
 *  ┌──────────┬──────────────────────────────────────────────┐
 *  │ places   │ (tabs)                                        │
 *  │ sidebar  │ the trash bar (Restore / Empty Trash)         │
 *  │          │ the files (icons or list)                     │
 *  └──────────┴──────────────────────────────────────────────┘
 *  status: what is selected · file operations (progress, Cancel) · free space · zoom
 *
 * Every command is a GAction of the window ("win.copy"...) with its keyboard shortcut; the menus call them.
 */
#include "files.h"
#include <string.h>

static GList *windows;

GList *files_windows(void)
{
    return windows;
}

FilesWindow *files_window_for_widget(GtkWidget *wid)
{
    GtkWidget *top = gtk_widget_get_toplevel(wid);
    for (GList *l = windows; l; l = l->next)
        if (((FilesWindow *)l->data)->window == top) return l->data;
    return NULL;
}

static Pane *pane_of_page(GtkWidget *page)
{
    return page ? g_object_get_data(G_OBJECT(page), "pane") : NULL;
}

#define FOREACH_PANE(w, p, body) do { \
    int n_ = gtk_notebook_get_n_pages(GTK_NOTEBOOK((w)->notebook)); \
    for (int i_ = 0; i_ < n_; i_++) { \
        Pane *p = pane_of_page(gtk_notebook_get_nth_page(GTK_NOTEBOOK((w)->notebook), i_)); \
        if (p) { body } \
    } } while (0)

static void enable(FilesWindow *w, const char *name, gboolean on)
{
    GAction *a = g_action_map_lookup_action(G_ACTION_MAP(w->window), name);
    if (a) g_simple_action_set_enabled(G_SIMPLE_ACTION(a), on);
}

static void set_state(FilesWindow *w, const char *name, GVariant *v)
{
    GAction *a = g_action_map_lookup_action(G_ACTION_MAP(w->window), name);
    if (a) g_simple_action_set_state(G_SIMPLE_ACTION(a), v);
    else g_variant_unref(g_variant_ref_sink(v));
}

static gboolean has_archive(GList *files)
{
    for (GList *l = files; l; l = l->next) {
        char *b = g_file_get_basename(l->data);
        gboolean a = b && files_is_archive(b);
        g_free(b);
        if (a) return TRUE;
    }
    return FALSE;
}

static void update_actions(FilesWindow *w)
{
    Pane *p = w->pane;
    if (!p) return;
    int n = pane_selected_count(p);
    gboolean drop = pane_drop_dir(p) != NULL;
    gboolean native = p->location && g_file_is_native(p->location) && !p->is_trash;
    enable(w, "back", p->back != NULL);
    enable(w, "forward", p->forward != NULL);
    GFile *parent = p->location && !p->is_trash && !p->is_recent ? g_file_get_parent(p->location) : NULL;
    enable(w, "up", parent != NULL);
    if (parent) g_object_unref(parent);
    enable(w, "open", n > 0);
    enable(w, "open-tab", n > 0);
    enable(w, "open-window", n > 0);
    enable(w, "open-with", n > 0 && !p->is_trash);
    enable(w, "open-location", n > 0 && (p->is_search || p->is_recent));
    enable(w, "cut", n > 0 && !p->is_trash && !p->is_recent);
    enable(w, "copy", n > 0 && !p->is_trash);
    enable(w, "paste", drop && files_clipboard_can_paste());
    enable(w, "paste-into", n == 1 && pane_selected_is_dir(p) && !p->is_trash && files_clipboard_can_paste());
    enable(w, "rename", n == 1 && !p->is_trash);
    enable(w, "trash", n > 0 && !p->is_trash);
    enable(w, "delete", n > 0);
    enable(w, "restore", n > 0 && p->is_trash);
    enable(w, "empty-trash", trash_count() > 0);
    enable(w, "new-folder", drop);
    enable(w, "new-document", drop);
    enable(w, "make-link", n > 0 && drop && native);
    enable(w, "compress", n > 0 && native && !p->is_recent && !p->is_search);
    GList *sel = n > 0 && native ? pane_selected_files(p) : NULL;
    enable(w, "extract", sel && has_archive(sel));
    files_list_free(sel);
    enable(w, "open-terminal", native && !p->is_recent);
    enable(w, "bookmark", native && !p->is_recent && !p->is_search && p->location && !files_bookmark_has(p->location));
    char *undo = files_undo_label();
    enable(w, "undo", undo != NULL);
    g_free(undo);
    enable(w, "zoom-in", !prefs.list_view && prefs_zoom_level() < 4);
    enable(w, "zoom-out", !prefs.list_view && prefs_zoom_level() > 0);
    enable(w, "zoom-normal", !prefs.list_view);
    gtk_widget_set_sensitive(w->trash_restore_btn, n > 0);
    gtk_widget_set_sensitive(w->trash_empty_btn, trash_count() > 0);
}

/* ------------------------------------------------------------------ status bar */
static void free_done(GObject *src, GAsyncResult *res, gpointer d)
{
    GError *e = NULL;
    GFileInfo *i = g_file_query_filesystem_info_finish(G_FILE(src), res, &e);
    if (g_error_matches(e, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_error_free(e);
        if (i) g_object_unref(i);
        return;                                     /* (the window may be gone) */
    }
    FilesWindow *w = d;
    if (i && g_file_info_has_attribute(i, G_FILE_ATTRIBUTE_FILESYSTEM_FREE)) {
        char *f = g_format_size(g_file_info_get_attribute_uint64(i, G_FILE_ATTRIBUTE_FILESYSTEM_FREE));
        char *s = g_strdup_printf("%s free", f);
        gtk_label_set_text(GTK_LABEL(w->free_label), s);
        g_free(s);
        g_free(f);
    } else gtk_label_set_text(GTK_LABEL(w->free_label), "");
    g_clear_error(&e);
    if (i) g_object_unref(i);
}

static void update_free(FilesWindow *w)
{
    if (w->free_cancel) g_cancellable_cancel(w->free_cancel);
    g_clear_object(&w->free_cancel);
    Pane *p = w->pane;
    GFile *where = NULL;
    if (p && p->is_trash) {
        char *t = trash_dir();
        where = g_file_new_for_path(t);
        g_free(t);
    } else if (p && p->location && !p->is_recent) where = g_object_ref(p->location);
    if (!where) {
        gtk_label_set_text(GTK_LABEL(w->free_label), "");
        return;
    }
    w->free_cancel = g_cancellable_new();
    g_file_query_filesystem_info_async(where, G_FILE_ATTRIBUTE_FILESYSTEM_FREE, G_PRIORITY_LOW, w->free_cancel, free_done, w);
    g_object_unref(where);
}

static gboolean status_cb(gpointer d)
{
    FilesWindow *w = d;
    w->status_id = 0;
    Pane *p = w->pane;
    if (!p) return G_SOURCE_REMOVE;
    int n, dirs;
    goffset bytes;
    char *first = NULL;
    pane_selection_summary(p, &n, &dirs, &bytes, &first);
    char *s;
    if (n == 1) {
        char *sz = dirs ? g_strdup("folder") : files_format_size(bytes, FALSE);
        s = g_strdup_printf("“%s” selected (%s)", first, sz);
        g_free(sz);
    } else if (n > 1) {
        char *sz = files_format_size(bytes, FALSE);
        if (dirs && n - dirs) s = g_strdup_printf("%d items selected (%d folder%s, %d file%s, %s)", n, dirs, dirs > 1 ? "s" : "",
                                                  n - dirs, n - dirs > 1 ? "s" : "", sz);
        else if (dirs) s = g_strdup_printf("%d folders selected", n);
        else s = g_strdup_printf("%d files selected (%s)", n, sz);
        g_free(sz);
    } else if (p->loading) s = g_strdup("Loading…");
    else {
        int shown = gtk_tree_model_iter_n_children(p->sort, NULL);
        if (p->is_search) s = g_strdup_printf("%d result%s", shown, shown == 1 ? "" : "s");
        else if (p->n_hidden && !prefs.show_hidden && !p->is_trash)
            s = g_strdup_printf("%d item%s (%d hidden)", shown, shown == 1 ? "" : "s", p->n_hidden);
        else s = g_strdup_printf("%d item%s", shown, shown == 1 ? "" : "s");
    }
    gtk_label_set_text(GTK_LABEL(w->status_label), s);
    g_free(s);
    g_free(first);
    update_actions(w);
    return G_SOURCE_REMOVE;
}

void files_window_update_status(FilesWindow *w)
{
    if (w && !w->status_id) w->status_id = g_timeout_add(60, status_cb, w);
}

void files_window_selection_changed(FilesWindow *w)
{
    files_window_update_status(w);
}

void files_windows_jobs_changed(void)
{
    static gboolean were_running;
    double frac = -1;
    char *text = files_jobs_status(&frac);
    for (GList *l = windows; l; l = l->next) {
        FilesWindow *w = l->data;
        if (text) {
            gtk_label_set_text(GTK_LABEL(w->job_label), text);
            if (frac >= 0) gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(w->job_bar), frac);
            else gtk_progress_bar_pulse(GTK_PROGRESS_BAR(w->job_bar));
            gtk_widget_show(w->job_box);
        } else gtk_widget_hide(w->job_box);
        if (!text && were_running) update_free(w);
        update_actions(w);
    }
    were_running = text != NULL;
    g_free(text);
}

/* ------------------------------------------------------------------ path bar, location entry, search */
static void on_crumb(GtkButton *b, gpointer d)
{
    FilesWindow *w = d;
    GFile *f = g_object_get_data(G_OBJECT(b), "file");
    if (f && w->pane) pane_go(w->pane, f, TRUE);
}

static gboolean scroll_end(gpointer d)
{
    GtkWidget *sw = d;
    GtkAdjustment *a = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(sw));
    gtk_adjustment_set_value(a, gtk_adjustment_get_upper(a));
    g_object_unref(sw);
    return G_SOURCE_REMOVE;
}

static GtkWidget *crumb(FilesWindow *w, GFile *f, const char *label, const char *icon, gboolean current)
{
    GtkWidget *b = gtk_button_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    if (icon) gtk_box_pack_start(GTK_BOX(box), gtk_image_new_from_icon_name(icon, GTK_ICON_SIZE_MENU), FALSE, FALSE, 0);
    if (label) gtk_box_pack_start(GTK_BOX(box), gtk_label_new(label), FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(b), box);
    gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
    gtk_widget_set_focus_on_click(b, FALSE);
    if (current) gtk_style_context_add_class(gtk_widget_get_style_context(b), "current");
    g_object_set_data_full(G_OBJECT(b), "file", g_object_ref(f), g_object_unref);
    char *tip = files_display_path(f);
    gtk_widget_set_tooltip_text(b, tip);
    g_free(tip);
    g_signal_connect(b, "clicked", G_CALLBACK(on_crumb), w);
    return b;
}

static void pathbar_rebuild(FilesWindow *w)
{
    GList *kids = gtk_container_get_children(GTK_CONTAINER(w->pathbar));
    for (GList *l = kids; l; l = l->next) gtk_widget_destroy(l->data);
    g_list_free(kids);
    Pane *p = w->pane;
    if (!p || !p->location) return;
    GList *chain = NULL;                          /* from the top (home or /) down to the location */
    GFile *home = g_file_new_for_path(g_get_home_dir());
    if (p->is_trash || p->is_recent || !g_file_is_native(p->location)) chain = g_list_prepend(NULL, g_object_ref(p->location));
    else {
        GFile *f = g_object_ref(p->location);
        while (f) {
            chain = g_list_prepend(chain, f);
            if (g_file_equal(f, home)) break;
            f = g_file_get_parent(f);
        }
    }
    for (GList *l = chain; l; l = l->next) {
        GFile *f = l->data;
        gboolean last = l->next == NULL;
        char *title = files_location_title(f);
        const char *icon = l == chain ? files_location_icon(f) : NULL;
        gboolean root = g_file_is_native(f) && !g_file_has_parent(f, NULL);
        GtkWidget *b = crumb(w, f, root && l == chain ? NULL : title, icon, last);
        gtk_box_pack_start(GTK_BOX(w->pathbar), b, FALSE, FALSE, 0);
        if (!last) {
            GtkWidget *sep = gtk_label_new("›");
            gtk_style_context_add_class(gtk_widget_get_style_context(sep), "sep");
            gtk_box_pack_start(GTK_BOX(w->pathbar), sep, FALSE, FALSE, 0);
        }
        g_free(title);
    }
    files_list_free(chain);
    g_object_unref(home);
    gtk_widget_show_all(w->pathbar);
    g_idle_add(scroll_end, g_object_ref(w->pathbar_scroll));
}

static void show_path(FilesWindow *w)
{
    gtk_stack_set_visible_child_name(GTK_STACK(w->path_stack), "path");
}

void files_window_start_location(FilesWindow *w, const char *text)
{
    if (w->search_btn && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w->search_btn)))
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w->search_btn), FALSE);
    gtk_stack_set_visible_child_name(GTK_STACK(w->path_stack), "entry");
    char *cur;
    if (text) cur = g_strdup(text);
    else if (w->pane && w->pane->location && g_file_is_native(w->pane->location)) cur = g_file_get_path(w->pane->location);
    else cur = w->pane && w->pane->location ? g_file_get_uri(w->pane->location) : g_strdup("");
    gtk_entry_set_text(GTK_ENTRY(w->location_entry), cur);
    gtk_widget_grab_focus(w->location_entry);
    if (text) gtk_editable_set_position(GTK_EDITABLE(w->location_entry), -1);
    else gtk_editable_select_region(GTK_EDITABLE(w->location_entry), 0, -1);
    files_log("location entry: %s", cur);
    g_free(cur);
}

static void on_location_activate(GtkEntry *e, gpointer d)
{
    FilesWindow *w = d;
    const char *text = gtk_entry_get_text(e);
    GFile *f = files_parse_location(text, w->pane ? w->pane->location : NULL);
    if (!f) return;
    files_log("location: %s", text);
    show_path(w);
    pane_focus(w->pane);
    if (g_file_is_native(f)) {
        GFileType t = g_file_query_file_type(f, G_FILE_QUERY_INFO_NONE, NULL);
        if (t == G_FILE_TYPE_UNKNOWN) {
            char *msg = g_strdup_printf("“%s” does not exist.", text);
            files_error(GTK_WINDOW(w->window), "The location cannot be opened", msg);
            g_free(msg);
            g_object_unref(f);
            return;
        }
        if (t != G_FILE_TYPE_DIRECTORY) {             /* a file: its folder, with the file selected */
            GFile *dir = g_file_get_parent(f);
            GList *one = g_list_append(NULL, f);
            files_window_select(w, dir, one);
            g_list_free(one);
            g_object_unref(dir);
            g_object_unref(f);
            return;
        }
    }
    pane_go(w->pane, f, TRUE);
    g_object_unref(f);
}

static gboolean on_location_key(GtkWidget *e, GdkEventKey *ev, gpointer d)
{
    (void)e;
    FilesWindow *w = d;
    if (ev->keyval == GDK_KEY_Escape) {
        show_path(w);
        pane_focus(w->pane);
        return TRUE;
    }
    return FALSE;
}

static gboolean on_location_focus_out(GtkWidget *e, GdkEventFocus *ev, gpointer d)
{
    (void)e; (void)ev;
    FilesWindow *w = d;
    if (!g_strcmp0(gtk_stack_get_visible_child_name(GTK_STACK(w->path_stack)), "entry")) show_path(w);
    return FALSE;
}

/* completion of folder names while typing a path */
static void on_location_changed(GtkEditable *ed, gpointer d)
{
    (void)d;
    const char *text = gtk_entry_get_text(GTK_ENTRY(ed));
    const char *slash = strrchr(text, '/');
    if (!slash || strstr(text, "://")) return;
    char *dir = g_strndup(text, (gsize)(slash - text + 1));
    const char *old = g_object_get_data(G_OBJECT(ed), "compl-dir");
    if (!g_strcmp0(old, dir)) {
        g_free(dir);
        return;
    }
    GtkListStore *store = g_object_get_data(G_OBJECT(ed), "compl-store");
    gtk_list_store_clear(store);
    char *path = dir[0] == '~' ? g_build_filename(g_get_home_dir(), dir + 1, NULL) : g_strdup(dir);
    GDir *gd = g_dir_open(path, 0, NULL);
    const char *n;
    int count = 0;
    gboolean dots = slash[1] == '.';
    while (gd && (n = g_dir_read_name(gd)) && count < 400) {
        if (n[0] == '.' && !dots) continue;
        char *full = g_build_filename(path, n, NULL);
        if (g_file_test(full, G_FILE_TEST_IS_DIR)) {
            char *show = g_strconcat(dir, n, "/", NULL);
            gtk_list_store_insert_with_values(store, NULL, -1, 0, show, -1);
            g_free(show);
            count++;
        }
        g_free(full);
    }
    if (gd) g_dir_close(gd);
    g_free(path);
    g_object_set_data_full(G_OBJECT(ed), "compl-dir", dir, g_free);
}

static void on_search_toggled(GtkToggleButton *t, gpointer d)
{
    FilesWindow *w = d;
    if (gtk_toggle_button_get_active(t)) {
        gtk_stack_set_visible_child_name(GTK_STACK(w->path_stack), "search");
        gtk_widget_grab_focus(w->search_entry);
        files_log("search: on");
    } else {
        w->in_search_update = TRUE;
        gtk_entry_set_text(GTK_ENTRY(w->search_entry), "");
        w->in_search_update = FALSE;
        show_path(w);
        if (w->pane) {
            pane_set_search(w->pane, NULL);
            pane_focus(w->pane);
        }
        files_log("search: off");
    }
}

static void on_search_changed(GtkSearchEntry *e, gpointer d)
{
    FilesWindow *w = d;
    if (w->in_search_update || !w->pane) return;
    pane_set_search(w->pane, gtk_entry_get_text(GTK_ENTRY(e)));
}

static void on_search_stop(GtkSearchEntry *e, gpointer d)
{
    (void)e;
    FilesWindow *w = d;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w->search_btn), FALSE);
}

static gboolean on_search_key(GtkWidget *e, GdkEventKey *ev, gpointer d)
{
    (void)e;
    FilesWindow *w = d;
    if (ev->keyval == GDK_KEY_Down || ev->keyval == GDK_KEY_Return) {     /* to the results */
        if (w->pane && gtk_tree_model_iter_n_children(w->pane->sort, NULL) > 0) {
            pane_focus(w->pane);
            if (!pane_selected_count(w->pane)) {
                GtkTreePath *first = gtk_tree_path_new_first();
                if (prefs.list_view) gtk_tree_view_set_cursor(GTK_TREE_VIEW(w->pane->list_view), first, NULL, FALSE);
                else {
                    gtk_icon_view_set_cursor(GTK_ICON_VIEW(w->pane->icon_view), first, NULL, FALSE);
                    gtk_icon_view_select_path(GTK_ICON_VIEW(w->pane->icon_view), first);
                }
                gtk_tree_path_free(first);
            }
        }
        return TRUE;
    }
    return FALSE;
}

/* ------------------------------------------------------------------ the window follows its tab */
void files_window_update_tab(Pane *p)
{
    if (!p->tab_label) return;
    char *title = p->is_search ? g_strdup_printf("Search: %s", p->search) : files_location_title(p->location);
    gtk_label_set_text(GTK_LABEL(p->tab_label), title);
    gtk_image_set_from_icon_name(GTK_IMAGE(p->tab_icon), p->is_search ? "edit-find" : files_location_icon(p->location),
                                 GTK_ICON_SIZE_MENU);
    char *tip = files_display_path(p->location);
    gtk_widget_set_tooltip_text(p->tab_box, tip);
    g_free(tip);
    g_free(title);
}

void files_window_update(FilesWindow *w)
{
    Pane *p = w->pane;
    if (!p) return;
    char *title = files_location_title(p->location);
    char *wt = g_strdup_printf("%s - %s", title, FILES_TITLE);
    gtk_window_set_title(GTK_WINDOW(w->window), wt);
    g_free(wt);
    g_free(title);
    pathbar_rebuild(w);
    if (p->location) gtk_places_sidebar_set_location(GTK_PLACES_SIDEBAR(w->sidebar), p->location);
    gtk_widget_set_visible(w->trash_bar, p->is_trash);
    w->in_search_update = TRUE;
    gboolean searching = p->search && *p->search;
    if (searching) {
        gtk_entry_set_text(GTK_ENTRY(w->search_entry), p->search);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w->search_btn), TRUE);
    } else {
        gtk_entry_set_text(GTK_ENTRY(w->search_entry), "");
        if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w->search_btn))) {
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w->search_btn), FALSE);
        }
        if (!g_strcmp0(gtk_stack_get_visible_child_name(GTK_STACK(w->path_stack)), "search")) show_path(w);
    }
    w->in_search_update = FALSE;
    gtk_image_set_from_icon_name(GTK_IMAGE(w->view_img), prefs.list_view ? "view-grid-symbolic" : "view-list-symbolic",
                                 GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(w->view_btn, prefs.list_view ? "Show as icons (Ctrl+1)" : "Show as a list (Ctrl+2)");
    gtk_widget_set_visible(w->zoom_scale, !prefs.list_view);
    update_free(w);
    update_actions(w);
    files_window_update_status(w);
}

void files_window_location_changed(Pane *p)
{
    files_window_update_tab(p);
    if (p->win->pane == p) files_window_update(p->win);
}

/* ------------------------------------------------------------------ tabs */
static void on_tab_close(GtkButton *b, gpointer d)
{
    (void)b;
    Pane *p = d;
    files_window_close_tab(p->win, p);
}

static gboolean on_tab_button(GtkWidget *wid, GdkEventButton *ev, gpointer d)
{
    (void)wid;
    Pane *p = d;
    if (ev->type == GDK_BUTTON_PRESS && ev->button == 2) {
        files_window_close_tab(p->win, p);
        return TRUE;
    }
    return FALSE;
}

static void tabs_visible(FilesWindow *w)
{
    gtk_notebook_set_show_tabs(GTK_NOTEBOOK(w->notebook), gtk_notebook_get_n_pages(GTK_NOTEBOOK(w->notebook)) > 1);
}

void files_window_open_tab(FilesWindow *w, GFile *location, gboolean switch_to)
{
    Pane *p = pane_new(w);
    GtkWidget *ev = gtk_event_box_new();
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(ev), FALSE);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    p->tab_icon = gtk_image_new_from_icon_name("folder", GTK_ICON_SIZE_MENU);
    p->tab_label = gtk_label_new("");
    gtk_label_set_ellipsize(GTK_LABEL(p->tab_label), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(p->tab_label), 22);
    gtk_label_set_width_chars(GTK_LABEL(p->tab_label), 6);
    GtkWidget *close = gtk_button_new_from_icon_name("window-close-symbolic", GTK_ICON_SIZE_MENU);
    gtk_button_set_relief(GTK_BUTTON(close), GTK_RELIEF_NONE);
    gtk_widget_set_focus_on_click(close, FALSE);
    gtk_widget_set_tooltip_text(close, "Close the tab (Ctrl+W)");
    gtk_style_context_add_class(gtk_widget_get_style_context(close), "files-tab-close");
    g_signal_connect(close, "clicked", G_CALLBACK(on_tab_close), p);
    gtk_box_pack_start(GTK_BOX(box), p->tab_icon, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), p->tab_label, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), close, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(ev), box);
    gtk_widget_show_all(ev);
    g_signal_connect(ev, "button-press-event", G_CALLBACK(on_tab_button), p);
    p->tab_box = ev;
    int cur = gtk_notebook_get_current_page(GTK_NOTEBOOK(w->notebook));
    int idx = gtk_notebook_insert_page(GTK_NOTEBOOK(w->notebook), p->page, ev, cur + 1);
    gtk_notebook_set_tab_reorderable(GTK_NOTEBOOK(w->notebook), p->page, TRUE);
    gtk_container_child_set(GTK_CONTAINER(w->notebook), p->page, "tab-expand", TRUE, NULL);
    tabs_visible(w);
    if (!w->pane) w->pane = p;
    pane_go(p, location, FALSE);
    if (switch_to) gtk_notebook_set_current_page(GTK_NOTEBOOK(w->notebook), idx);
    files_log("tab opened (%d tabs)", gtk_notebook_get_n_pages(GTK_NOTEBOOK(w->notebook)));
}

void files_window_close_tab(FilesWindow *w, Pane *p)
{
    int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(w->notebook));
    if (n <= 1) {
        gtk_widget_destroy(w->window);
        return;
    }
    int idx = gtk_notebook_page_num(GTK_NOTEBOOK(w->notebook), p->page);
    if (w->pane == p) w->pane = NULL;
    GtkWidget *page = g_object_ref(p->page), *tab = p->tab_box ? g_object_ref(p->tab_box) : NULL;
    gtk_notebook_remove_page(GTK_NOTEBOOK(w->notebook), idx);
    pane_free(p);                                   /* (its widgets are still alive: it lets go of them first) */
    gtk_widget_destroy(page);
    g_object_unref(page);
    if (tab) {
        gtk_widget_destroy(tab);
        g_object_unref(tab);
    }
    if (!w->pane) {
        w->pane = pane_of_page(gtk_notebook_get_nth_page(GTK_NOTEBOOK(w->notebook),
                                                         gtk_notebook_get_current_page(GTK_NOTEBOOK(w->notebook))));
        files_window_update(w);
    }
    tabs_visible(w);
    files_log("tab closed (%d tabs)", gtk_notebook_get_n_pages(GTK_NOTEBOOK(w->notebook)));
}

static void on_switch_page(GtkNotebook *nb, GtkWidget *page, guint num, gpointer d)
{
    (void)nb; (void)num;
    FilesWindow *w = d;
    Pane *p = pane_of_page(page);
    if (!p || p == w->pane) return;
    w->pane = p;
    files_window_update(w);
    pane_focus(p);
}

/* ------------------------------------------------------------------ opening */
void files_window_activate_items(FilesWindow *w, GList *files, int how)
{
    GList *dirs = NULL, *others = NULL;
    for (GList *l = files; l; l = l->next) {
        GFileType t = g_file_query_file_type(l->data, G_FILE_QUERY_INFO_NONE, NULL);
        if (t == G_FILE_TYPE_DIRECTORY || t == G_FILE_TYPE_MOUNTABLE) dirs = g_list_append(dirs, l->data);
        else others = g_list_append(others, l->data);
    }
    int i = 0;
    for (GList *l = dirs; l; l = l->next, i++) {
        if (how == 2) files_window_new(gtk_window_get_application(GTK_WINDOW(w->window)), l->data);
        else if (how == 1 || i > 0) files_window_open_tab(w, l->data, how == 1 && !l->next ? TRUE : FALSE);
        else pane_go(w->pane, l->data, TRUE);
    }
    if (others) files_open_files(w, others);
    g_list_free(dirs);
    g_list_free(others);
}

void files_window_select(FilesWindow *w, GFile *dir, GList *files)
{
    if (!w->pane->location || !g_file_equal(w->pane->location, dir) || w->pane->is_search) pane_go(w->pane, dir, TRUE);
    pane_select_files(w->pane, files);
    gtk_window_present(GTK_WINDOW(w->window));
}

void files_windows_job_done(GFile *dest_dir, GList *created)
{
    if (!dest_dir || !created) return;
    for (GList *l = windows; l; l = l->next) {
        FilesWindow *w = l->data;
        FOREACH_PANE(w, p, {
            if (p->location && g_file_equal(p->location, dest_dir) && !p->is_search) pane_select_files(p, created);
        });
        update_actions(w);
    }
}

void files_windows_icons_changed(void)
{
    files_icon_cache_clear();
    for (GList *l = windows; l; l = l->next) {
        FilesWindow *w = l->data;
        FOREACH_PANE(w, p, { pane_icons_changed(p); files_window_update_tab(p); });
        pathbar_rebuild(w);
    }
}

void files_windows_cut_changed(void)
{
    for (GList *l = windows; l; l = l->next) {
        FilesWindow *w = l->data;
        FOREACH_PANE(w, p, { pane_update_cut(p); });
        update_actions(w);
    }
}

static void all_panes(void (*fn)(Pane *))
{
    for (GList *l = windows; l; l = l->next) {
        FilesWindow *w = l->data;
        FOREACH_PANE(w, p, { fn(p); });
        files_window_update(w);
    }
}

/* ------------------------------------------------------------------ actions */
#define W FilesWindow *w = d; (void)a; (void)v
#define SEL GList *sel = pane_selected_files(w->pane)

static void act_new_window(GSimpleAction *a, GVariant *v, gpointer d)
{
    W;
    FilesWindow *nw = files_window_new(gtk_window_get_application(GTK_WINDOW(w->window)), w->pane->location);
    (void)nw;
}

static void act_new_tab(GSimpleAction *a, GVariant *v, gpointer d)
{
    W;
    files_window_open_tab(w, w->pane->location, TRUE);
}

static void act_close_tab(GSimpleAction *a, GVariant *v, gpointer d)
{
    W;
    files_window_close_tab(w, w->pane);
}

static void act_close_window(GSimpleAction *a, GVariant *v, gpointer d)
{
    W;
    gtk_widget_destroy(w->window);
}

static void act_next_tab(GSimpleAction *a, GVariant *v, gpointer d)
{
    W;
    GtkNotebook *nb = GTK_NOTEBOOK(w->notebook);
    int n = gtk_notebook_get_n_pages(nb);
    if (n > 1) gtk_notebook_set_current_page(nb, (gtk_notebook_get_current_page(nb) + 1) % n);
}

static void act_prev_tab(GSimpleAction *a, GVariant *v, gpointer d)
{
    W;
    GtkNotebook *nb = GTK_NOTEBOOK(w->notebook);
    int n = gtk_notebook_get_n_pages(nb);
    if (n > 1) gtk_notebook_set_current_page(nb, (gtk_notebook_get_current_page(nb) + n - 1) % n);
}

static void act_back(GSimpleAction *a, GVariant *v, gpointer d) { W; pane_back(w->pane); }
static void act_forward(GSimpleAction *a, GVariant *v, gpointer d) { W; pane_forward(w->pane); }
static void act_up(GSimpleAction *a, GVariant *v, gpointer d) { W; pane_up(w->pane); }
static void act_reload(GSimpleAction *a, GVariant *v, gpointer d) { W; pane_reload(w->pane); }

static void act_home(GSimpleAction *a, GVariant *v, gpointer d)
{
    W;
    GFile *h = g_file_new_for_path(g_get_home_dir());
    pane_go(w->pane, h, TRUE);
    g_object_unref(h);
}

static void act_location(GSimpleAction *a, GVariant *v, gpointer d) { W; files_window_start_location(w, NULL); }

static void act_search(GSimpleAction *a, GVariant *v, gpointer d)
{
    W;
    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w->search_btn))) gtk_widget_grab_focus(w->search_entry);
    else gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w->search_btn), TRUE);
}

static void act_show_hidden(GSimpleAction *a, GVariant *v, gpointer d)
{
    (void)v; (void)a;
    FilesWindow *w = d;
    prefs.show_hidden = !prefs.show_hidden;
    prefs_save();
    all_panes(pane_apply_prefs);
    for (GList *l = windows; l; l = l->next) set_state(l->data, "show-hidden", g_variant_new_boolean(prefs.show_hidden));
    files_log("hidden files: %s (%d shown)", prefs.show_hidden ? "shown" : "hidden",
              gtk_tree_model_iter_n_children(w->pane->sort, NULL));
}

static void set_view(gboolean list)
{
    if (prefs.list_view == list) return;
    prefs.list_view = list;
    prefs_save();
    files_log("view: %s", list ? "list" : "icons");
    all_panes(pane_apply_view);
}

static void act_view_icons(GSimpleAction *a, GVariant *v, gpointer d) { W; set_view(FALSE); pane_focus(w->pane); }
static void act_view_list(GSimpleAction *a, GVariant *v, gpointer d) { W; set_view(TRUE); pane_focus(w->pane); }
static void act_toggle_view(GSimpleAction *a, GVariant *v, gpointer d) { W; set_view(!prefs.list_view); pane_focus(w->pane); }

static void set_zoom(int level)
{
    level = CLAMP(level, 0, 4);
    if (files_icon_sizes[level] == prefs.icon_size) return;
    prefs.icon_size = files_icon_sizes[level];
    prefs_save();
    files_log("zoom: %d px", prefs.icon_size);
    for (GList *l = windows; l; l = l->next) {
        FilesWindow *w = l->data;
        w->in_zoom_update = TRUE;
        gtk_range_set_value(GTK_RANGE(w->zoom_scale), level);
        w->in_zoom_update = FALSE;
    }
    all_panes(pane_apply_zoom);
}

static void act_zoom_in(GSimpleAction *a, GVariant *v, gpointer d) { W; (void)w; set_zoom(prefs_zoom_level() + 1); }
static void act_zoom_out(GSimpleAction *a, GVariant *v, gpointer d) { W; (void)w; set_zoom(prefs_zoom_level() - 1); }
static void act_zoom_normal(GSimpleAction *a, GVariant *v, gpointer d) { W; (void)w; set_zoom(2); }

static void on_zoom_scale(GtkRange *r, gpointer d)
{
    FilesWindow *w = d;
    if (w->in_zoom_update) return;
    set_zoom((int)(gtk_range_get_value(r) + 0.5));
}

static void act_show_sidebar(GSimpleAction *a, GVariant *v, gpointer d)
{
    (void)a; (void)v; (void)d;
    prefs.show_sidebar = !prefs.show_sidebar;
    prefs_save();
    files_log("sidebar: %s", prefs.show_sidebar ? "shown" : "hidden");
    for (GList *l = windows; l; l = l->next) {
        FilesWindow *w = l->data;
        gtk_widget_set_visible(w->sidebar, prefs.show_sidebar);
        set_state(w, "show-sidebar", g_variant_new_boolean(prefs.show_sidebar));
    }
}

static void act_sort(GSimpleAction *a, GVariant *v, gpointer d)
{
    (void)a; (void)d;
    const char *s = g_variant_get_string(v, NULL);
    static const char *const names[] = { "name", "size", "type", "modified" };
    for (int i = 0; i < SORT_N; i++)
        if (!strcmp(s, names[i])) prefs.sort_by = i;
    prefs_save();
    files_log("sort: %s%s", names[prefs.sort_by], prefs.sort_desc ? " (reversed)" : "");
    all_panes(pane_apply_prefs);
}

static void act_sort_desc(GSimpleAction *a, GVariant *v, gpointer d)
{
    (void)a; (void)v; (void)d;
    prefs.sort_desc = !prefs.sort_desc;
    prefs_save();
    files_log("sort: reversed %s", prefs.sort_desc ? "on" : "off");
    all_panes(pane_apply_prefs);
}

static void act_folders_first(GSimpleAction *a, GVariant *v, gpointer d)
{
    (void)a; (void)v; (void)d;
    prefs.folders_first = !prefs.folders_first;
    prefs_save();
    files_log("folders first: %s", prefs.folders_first ? "on" : "off");
    all_panes(pane_apply_prefs);
}

static void act_open(GSimpleAction *a, GVariant *v, gpointer d) { W; SEL; files_window_activate_items(w, sel, 0); files_list_free(sel); }
static void act_open_tab(GSimpleAction *a, GVariant *v, gpointer d) { W; SEL; files_window_activate_items(w, sel, 1); files_list_free(sel); }
static void act_open_window(GSimpleAction *a, GVariant *v, gpointer d) { W; SEL; files_window_activate_items(w, sel, 2); files_list_free(sel); }
static void act_open_with(GSimpleAction *a, GVariant *v, gpointer d) { W; SEL; files_open_with_dialog(w, sel); files_list_free(sel); }

static void act_open_location(GSimpleAction *a, GVariant *v, gpointer d)
{
    W;
    SEL;
    if (sel) {
        GFile *dir = g_file_get_parent(sel->data);
        GList *one = g_list_append(NULL, sel->data);
        if (dir) files_window_select(w, dir, one);
        g_list_free(one);
        if (dir) g_object_unref(dir);
    }
    files_list_free(sel);
}

static void act_open_terminal(GSimpleAction *a, GVariant *v, gpointer d)
{
    W;
    GFile *dir = NULL;
    if (pane_selected_is_dir(w->pane)) {
        SEL;
        dir = g_object_ref(sel->data);
        files_list_free(sel);
    } else if (w->pane->location) dir = g_object_ref(w->pane->location);
    if (dir) files_open_terminal(dir, GTK_WINDOW(w->window));
    if (dir) g_object_unref(dir);
}

static void act_cut(GSimpleAction *a, GVariant *v, gpointer d) { W; SEL; files_clipboard_set(sel, TRUE); files_list_free(sel); }
static void act_copy(GSimpleAction *a, GVariant *v, gpointer d) { W; SEL; files_clipboard_set(sel, FALSE); files_list_free(sel); }
static void act_paste(GSimpleAction *a, GVariant *v, gpointer d) { W; files_clipboard_paste(w, pane_drop_dir(w->pane)); }

static void act_paste_into(GSimpleAction *a, GVariant *v, gpointer d)
{
    W;
    SEL;
    if (sel) files_clipboard_paste(w, sel->data);
    files_list_free(sel);
}

static void act_copy_location(GSimpleAction *a, GVariant *v, gpointer d)
{
    W;
    SEL;
    GString *s = g_string_new(NULL);
    if (!sel && w->pane->location) sel = g_list_append(NULL, g_object_ref(w->pane->location));
    for (GList *l = sel; l; l = l->next) {
        char *p = g_file_get_parse_name(l->data);
        g_string_append_printf(s, "%s%s", s->len ? "\n" : "", p);
        g_free(p);
    }
    if (s->len) files_clipboard_copy_text(s->str);
    g_string_free(s, TRUE);
    files_list_free(sel);
}

static void act_select_all(GSimpleAction *a, GVariant *v, gpointer d) { W; pane_select_all(w->pane); }
static void act_invert(GSimpleAction *a, GVariant *v, gpointer d) { W; pane_invert_selection(w->pane); }

static void act_rename(GSimpleAction *a, GVariant *v, gpointer d)
{
    W;
    SEL;
    if (sel && !sel->next) files_rename_dialog(w, sel->data);
    files_list_free(sel);
}

static void confirm_delete(FilesWindow *w, GList *sel)
{
    char *names = files_names_text(sel, 3);
    guint n = g_list_length(sel);
    char *title = n == 1 ? g_strdup_printf("Permanently delete “%s”?", names)
                         : g_strdup_printf("Permanently delete %u items?", n);
    if (files_confirm(GTK_WINDOW(w->window), title, "Deleted items cannot be brought back.", "_Delete", TRUE))
        files_job_start(GTK_WINDOW(w->window), JOB_DELETE, sel, NULL);
    g_free(title);
    g_free(names);
}

static void act_trash(GSimpleAction *a, GVariant *v, gpointer d)
{
    W;
    SEL;
    if (sel && w->pane->is_trash) confirm_delete(w, sel);    /* Delete in the trash: for good */
    else if (sel) files_job_start(GTK_WINDOW(w->window), JOB_TRASH, sel, NULL);
    files_list_free(sel);
}

static void act_delete(GSimpleAction *a, GVariant *v, gpointer d)
{
    W;
    SEL;
    if (sel) confirm_delete(w, sel);
    files_list_free(sel);
}

static void act_restore(GSimpleAction *a, GVariant *v, gpointer d)
{
    W;
    SEL;
    if (sel) files_job_start(GTK_WINDOW(w->window), JOB_RESTORE, sel, NULL);
    files_list_free(sel);
}

static void act_empty_trash(GSimpleAction *a, GVariant *v, gpointer d)
{
    W;
    int n = trash_count();
    char *detail = g_strdup_printf("All %d item%s in the trash will be deleted permanently.", n, n == 1 ? "" : "s");
    if (files_confirm(GTK_WINDOW(w->window), "Empty the trash?", detail, "_Empty Trash", TRUE))
        files_job_start(GTK_WINDOW(w->window), JOB_EMPTY_TRASH, NULL, NULL);
    g_free(detail);
}

static void act_new_folder(GSimpleAction *a, GVariant *v, gpointer d)
{
    W;
    GFile *dir = pane_drop_dir(w->pane);
    if (dir) files_new_folder_dialog(w, dir);
}

static void act_new_document(GSimpleAction *a, GVariant *v, gpointer d)
{
    (void)a;
    FilesWindow *w = d;
    GFile *dir = pane_drop_dir(w->pane);
    const char *path = v ? g_variant_get_string(v, NULL) : "";
    GFile *t = *path ? g_file_new_for_path(path) : NULL;
    if (dir) files_new_document_dialog(w, dir, t);
    if (t) g_object_unref(t);
}

static void act_make_link(GSimpleAction *a, GVariant *v, gpointer d)
{
    W;
    SEL;
    if (sel) files_job_start(GTK_WINDOW(w->window), JOB_LINK, sel, pane_drop_dir(w->pane));
    files_list_free(sel);
}

static void act_compress(GSimpleAction *a, GVariant *v, gpointer d)
{
    (void)a;
    FilesWindow *w = d;
    SEL;
    const char *fmt = v ? g_variant_get_string(v, NULL) : "zip";
    if (!strcmp(fmt, "zip") && !files_have_zip()) fmt = "tar.gz";
    if (sel) files_compress(GTK_WINDOW(w->window), sel, w->pane->location, fmt);
    files_list_free(sel);
}

static void act_extract(GSimpleAction *a, GVariant *v, gpointer d)
{
    W;
    SEL;
    GList *arch = NULL;
    for (GList *l = sel; l; l = l->next) {
        char *b = g_file_get_basename(l->data);
        if (b && files_is_archive(b)) arch = g_list_append(arch, l->data);
        g_free(b);
    }
    if (arch) files_extract(GTK_WINDOW(w->window), arch);
    g_list_free(arch);
    files_list_free(sel);
}

static void act_properties(GSimpleAction *a, GVariant *v, gpointer d)
{
    W;
    SEL;
    if (!sel && w->pane->location && !w->pane->is_trash && !w->pane->is_recent)
        sel = g_list_append(NULL, g_object_ref(w->pane->location));
    if (sel) files_properties_dialog(w, sel);
    files_list_free(sel);
}

static void act_bookmark(GSimpleAction *a, GVariant *v, gpointer d)
{
    W;
    if (w->pane->location && files_bookmark_add(w->pane->location)) update_actions(w);
}

static void act_undo(GSimpleAction *a, GVariant *v, gpointer d)
{
    W;
    files_undo(GTK_WINDOW(w->window));
    for (GList *l = windows; l; l = l->next) update_actions(l->data);
}

static void popup_main_menu(FilesWindow *w);

static void act_menu(GSimpleAction *a, GVariant *v, gpointer d)
{
    W;
    popup_main_menu(w);
}

static void act_about(GSimpleAction *a, GVariant *v, gpointer d) { W; files_about(GTK_WINDOW(w->window)); }
static void act_shortcuts(GSimpleAction *a, GVariant *v, gpointer d) { W; files_shortcuts(GTK_WINDOW(w->window)); }

static const GActionEntry win_actions[] = {
    { "new-window", act_new_window, NULL, NULL, NULL, { 0 } },
    { "new-tab", act_new_tab, NULL, NULL, NULL, { 0 } },
    { "close-tab", act_close_tab, NULL, NULL, NULL, { 0 } },
    { "close-window", act_close_window, NULL, NULL, NULL, { 0 } },
    { "next-tab", act_next_tab, NULL, NULL, NULL, { 0 } },
    { "prev-tab", act_prev_tab, NULL, NULL, NULL, { 0 } },
    { "back", act_back, NULL, NULL, NULL, { 0 } },
    { "forward", act_forward, NULL, NULL, NULL, { 0 } },
    { "up", act_up, NULL, NULL, NULL, { 0 } },
    { "home", act_home, NULL, NULL, NULL, { 0 } },
    { "reload", act_reload, NULL, NULL, NULL, { 0 } },
    { "location", act_location, NULL, NULL, NULL, { 0 } },
    { "search", act_search, NULL, NULL, NULL, { 0 } },
    { "show-hidden", act_show_hidden, NULL, "false", NULL, { 0 } },
    { "show-sidebar", act_show_sidebar, NULL, "true", NULL, { 0 } },
    { "view-icons", act_view_icons, NULL, NULL, NULL, { 0 } },
    { "view-list", act_view_list, NULL, NULL, NULL, { 0 } },
    { "toggle-view", act_toggle_view, NULL, NULL, NULL, { 0 } },
    { "zoom-in", act_zoom_in, NULL, NULL, NULL, { 0 } },
    { "zoom-out", act_zoom_out, NULL, NULL, NULL, { 0 } },
    { "zoom-normal", act_zoom_normal, NULL, NULL, NULL, { 0 } },
    { "sort", act_sort, "s", NULL, NULL, { 0 } },
    { "sort-desc", act_sort_desc, NULL, NULL, NULL, { 0 } },
    { "folders-first", act_folders_first, NULL, NULL, NULL, { 0 } },
    { "open", act_open, NULL, NULL, NULL, { 0 } },
    { "open-tab", act_open_tab, NULL, NULL, NULL, { 0 } },
    { "open-window", act_open_window, NULL, NULL, NULL, { 0 } },
    { "open-with", act_open_with, NULL, NULL, NULL, { 0 } },
    { "open-location", act_open_location, NULL, NULL, NULL, { 0 } },
    { "open-terminal", act_open_terminal, NULL, NULL, NULL, { 0 } },
    { "cut", act_cut, NULL, NULL, NULL, { 0 } },
    { "copy", act_copy, NULL, NULL, NULL, { 0 } },
    { "paste", act_paste, NULL, NULL, NULL, { 0 } },
    { "paste-into", act_paste_into, NULL, NULL, NULL, { 0 } },
    { "copy-location", act_copy_location, NULL, NULL, NULL, { 0 } },
    { "select-all", act_select_all, NULL, NULL, NULL, { 0 } },
    { "invert-selection", act_invert, NULL, NULL, NULL, { 0 } },
    { "rename", act_rename, NULL, NULL, NULL, { 0 } },
    { "trash", act_trash, NULL, NULL, NULL, { 0 } },
    { "delete", act_delete, NULL, NULL, NULL, { 0 } },
    { "restore", act_restore, NULL, NULL, NULL, { 0 } },
    { "empty-trash", act_empty_trash, NULL, NULL, NULL, { 0 } },
    { "new-folder", act_new_folder, NULL, NULL, NULL, { 0 } },
    { "new-document", act_new_document, "s", NULL, NULL, { 0 } },
    { "make-link", act_make_link, NULL, NULL, NULL, { 0 } },
    { "compress", act_compress, "s", NULL, NULL, { 0 } },
    { "extract", act_extract, NULL, NULL, NULL, { 0 } },
    { "properties", act_properties, NULL, NULL, NULL, { 0 } },
    { "bookmark", act_bookmark, NULL, NULL, NULL, { 0 } },
    { "undo", act_undo, NULL, NULL, NULL, { 0 } },
    { "menu", act_menu, NULL, NULL, NULL, { 0 } },
    { "about", act_about, NULL, NULL, NULL, { 0 } },
    { "shortcuts", act_shortcuts, NULL, NULL, NULL, { 0 } },
};

void files_window_set_accels(GtkApplication *app)
{
    static const struct { const char *action; const char *keys[4]; } accels[] = {
        { "win.new-window", { "<Control>n" } }, { "win.new-tab", { "<Control>t" } },
        { "win.close-tab", { "<Control>w" } }, { "app.quit", { "<Control>q" } },
        { "win.next-tab", { "<Control>Page_Down" } }, { "win.prev-tab", { "<Control>Page_Up" } },
        { "win.back", { "<Alt>Left", "Back" } }, { "win.forward", { "<Alt>Right", "Forward" } },
        { "win.up", { "<Alt>Up" } }, { "win.home", { "<Alt>Home", "HomePage" } },
        { "win.reload", { "F5", "<Control>r", "Reload" } }, { "win.location", { "<Control>l", "F6" } },
        { "win.search", { "<Control>f", "Search" } }, { "win.show-hidden", { "<Control>h" } },
        { "win.view-icons", { "<Control>1" } }, { "win.view-list", { "<Control>2" } },
        { "win.zoom-in", { "<Control>plus", "<Control>equal", "<Control>KP_Add" } },
        { "win.zoom-out", { "<Control>minus", "<Control>KP_Subtract" } },
        { "win.zoom-normal", { "<Control>0", "<Control>KP_0" } }, { "win.show-sidebar", { "F9" } },
        { "win.cut", { "<Control>x" } }, { "win.copy", { "<Control>c" } }, { "win.paste", { "<Control>v" } },
        { "win.select-all", { "<Control>a" } }, { "win.invert-selection", { "<Control><Shift>i" } },
        { "win.rename", { "F2" } }, { "win.trash", { "Delete", "KP_Delete" } },
        { "win.delete", { "<Shift>Delete", "<Shift>KP_Delete" } }, { "win.new-folder", { "<Control><Shift>n" } },
        { "win.properties", { "<Alt>Return", "<Control>i" } }, { "win.bookmark", { "<Control>d" } },
        { "win.undo", { "<Control>z" } }, { "win.open-tab", { "<Control>Return" } },
        { "win.open-window", { "<Shift>Return" } }, { "win.menu", { "F10" } },
        { "win.shortcuts", { "<Control>question", "<Control>F1" } },
    };
    for (guint i = 0; i < G_N_ELEMENTS(accels); i++)
        gtk_application_set_accels_for_action(app, accels[i].action, accels[i].keys);
}

/* ------------------------------------------------------------------ menus */
typedef struct {
    FilesWindow *w;
    GtkWidget *menu;
    GString *log;
} MenuBuild;

static void on_menu_item(GtkMenuItem *it, gpointer d)
{
    FilesWindow *w = d;
    const char *name = g_object_get_data(G_OBJECT(it), "action");
    GVariant *param = g_object_get_data(G_OBJECT(it), "param");
    if (!g_list_find(windows, w)) return;
    files_log("menu item: %s", name);
    g_action_group_activate_action(G_ACTION_GROUP(w->window), name, param);
}

static void strip_mnemonic(GString *out, const char *label)
{
    for (const char *c = label; *c; c++)
        if (*c != '_') g_string_append_c(out, *c);
}

static GtkWidget *add_item(MenuBuild *m, GtkWidget *menu, const char *icons, const char *label, const char *accel,
                           const char *action, GVariant *param)
{
    GtkWidget *it = files_menu_item(icons, label, accel);
    if (action) {
        g_object_set_data_full(G_OBJECT(it), "action", g_strdup(action), g_free);
        if (param) g_object_set_data_full(G_OBJECT(it), "param", g_variant_ref_sink(param), (GDestroyNotify)g_variant_unref);
        g_signal_connect(it, "activate", G_CALLBACK(on_menu_item), m->w);
        gtk_widget_set_sensitive(it, g_action_group_get_action_enabled(G_ACTION_GROUP(m->w->window), action));
    }
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), it);
    if (menu == m->menu) {
        if (m->log->len) g_string_append(m->log, " | ");
        strip_mnemonic(m->log, label);
    }
    return it;
}

static GtkWidget *add_check(MenuBuild *m, GtkWidget *menu, const char *label, gboolean on, const char *accel,
                            const char *action)
{
    GtkWidget *it = files_menu_check(label, on, accel);
    g_object_set_data_full(G_OBJECT(it), "action", g_strdup(action), g_free);
    g_signal_connect(it, "activate", G_CALLBACK(on_menu_item), m->w);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), it);
    if (menu == m->menu) {
        if (m->log->len) g_string_append(m->log, " | ");
        strip_mnemonic(m->log, label);
    }
    return it;
}

static GtkWidget *add_submenu(MenuBuild *m, GtkWidget *menu, const char *icons, const char *label)
{
    GtkWidget *it = files_menu_item(icons, label, NULL);
    GtkWidget *sub = gtk_menu_new();
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(it), sub);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), it);
    if (menu == m->menu) {
        if (m->log->len) g_string_append(m->log, " | ");
        strip_mnemonic(m->log, label);
    }
    return sub;
}

static void add_sort_menu(MenuBuild *m, GtkWidget *menu)
{
    GtkWidget *sub = add_submenu(m, menu, "view-sort-ascending", "_Sort By");
    static const char *const names[] = { "name", "size", "type", "modified" };
    static const char *const labels[] = { "_Name", "_Size", "_Type", "_Modified" };
    GSList *group = NULL;
    for (int i = 0; i < SORT_N; i++) {
        GtkWidget *it = files_menu_radio(&group, labels[i], prefs.sort_by == i);
        g_object_set_data_full(G_OBJECT(it), "action", g_strdup("sort"), g_free);
        g_object_set_data_full(G_OBJECT(it), "param", g_variant_ref_sink(g_variant_new_string(names[i])),
                               (GDestroyNotify)g_variant_unref);
        g_signal_connect(it, "activate", G_CALLBACK(on_menu_item), m->w);
        gtk_menu_shell_append(GTK_MENU_SHELL(sub), it);
    }
    files_menu_sep(sub);
    add_check(m, sub, "_Reversed Order", prefs.sort_desc, NULL, "sort-desc");
    add_check(m, sub, "_Folders First", prefs.folders_first, NULL, "folders-first");
}

static void add_new_document_menu(MenuBuild *m, GtkWidget *menu)
{
    GtkWidget *sub = add_submenu(m, menu, "document-new", "New _Document");
    add_item(m, sub, "text-x-generic", "_Empty Document", NULL, "new-document", g_variant_new_string(""));
    GList *templates = files_templates();
    if (templates) files_menu_sep(sub);
    for (GList *l = templates; l; l = l->next) {
        char *b = g_file_get_basename(l->data), *base;
        const char *ext;
        files_split_ext(b, &base, &ext);
        char *path = g_file_get_path(l->data);
        GString *label = g_string_new(NULL);               /* (an underscore of the name is not a mnemonic) */
        for (const char *c = base; *c; c++) {
            if (*c == '_') g_string_append_c(label, '_');
            g_string_append_c(label, *c);
        }
        add_item(m, sub, "text-x-generic", label->str, NULL, "new-document", g_variant_new_string(path));
        g_string_free(label, TRUE);
        g_free(path);
        g_free(base);
        g_free(b);
    }
    files_list_free(templates);
    gtk_widget_set_sensitive(gtk_menu_get_attach_widget(GTK_MENU(sub)),
                             g_action_group_get_action_enabled(G_ACTION_GROUP(m->w->window), "new-document"));
}

static void free_menu_later(GtkMenuShell *menu, gpointer d)
{
    (void)d;
    g_object_set_data(G_OBJECT(menu), "done", GINT_TO_POINTER(1));
}

static GtkWidget *new_menu(FilesWindow *w)
{
    GtkWidget *old = g_object_get_data(G_OBJECT(w->window), "ctx-menu");
    if (old) gtk_widget_destroy(old);
    GtkWidget *menu = gtk_menu_new();
    gtk_menu_attach_to_widget(GTK_MENU(menu), w->window, NULL);
    g_object_set_data(G_OBJECT(w->window), "ctx-menu", menu);
    g_signal_connect(menu, "deactivate", G_CALLBACK(free_menu_later), NULL);
    return menu;
}

static void popup_rect(FilesWindow *w, GtkWidget *menu)
{
    Pane *p = w->pane;
    GtkWidget *view = pane_view(p);
    GdkRectangle r = { 0, 0, 1, 1 };
    GtkAllocation a;
    gtk_widget_get_allocation(view, &a);
    r.x = a.width / 3;
    r.y = MIN(a.height / 3, 80);
    GList *paths = prefs.list_view ? gtk_tree_selection_get_selected_rows(gtk_tree_view_get_selection(GTK_TREE_VIEW(view)), NULL)
                                   : gtk_icon_view_get_selected_items(GTK_ICON_VIEW(view));
    if (paths) {
        paths = g_list_sort(paths, (GCompareFunc)gtk_tree_path_compare);
        GdkRectangle c;
        if (prefs.list_view) {
            gtk_tree_view_get_cell_area(GTK_TREE_VIEW(view), paths->data, p->col_name, &c);
            int x, y;
            gtk_tree_view_convert_bin_window_to_widget_coords(GTK_TREE_VIEW(view), c.x, c.y, &x, &y);
            r = (GdkRectangle){ x + 24, y, 1, c.height };
        } else if (gtk_icon_view_get_cell_rect(GTK_ICON_VIEW(view), paths->data, NULL, &c)) {
            r = (GdkRectangle){ c.x + c.width / 2, c.y + c.height / 2, 1, 1 };
        }
    }
    g_list_free_full(paths, (GDestroyNotify)gtk_tree_path_free);
    gtk_menu_popup_at_rect(GTK_MENU(menu), gtk_widget_get_window(view), &r, GDK_GRAVITY_SOUTH_WEST, GDK_GRAVITY_NORTH_WEST, NULL);
}

void files_window_popup(FilesWindow *w, const GdkEvent *ev, gboolean on_items)
{
    Pane *p = w->pane;
    update_actions(w);
    GtkWidget *menu = new_menu(w);
    MenuBuild mb = { w, menu, g_string_new(NULL) };
    MenuBuild *m = &mb;
    if (on_items) {
        GList *sel = pane_selected_files(p);
        int n = g_list_length(sel);
        gboolean all_dirs = TRUE;
        for (GList *l = sel; l; l = l->next)
            if (g_file_query_file_type(l->data, G_FILE_QUERY_INFO_NONE, NULL) != G_FILE_TYPE_DIRECTORY) all_dirs = FALSE;
        if (p->is_trash) {
            add_item(m, menu, "edit-undo|document-revert", "_Restore", NULL, "restore", NULL);
            add_item(m, menu, "edit-delete", "_Delete Permanently", "Delete", "delete", NULL);
            files_menu_sep(menu);
            add_item(m, menu, "document-properties", "_Properties", "Alt+Enter", "properties", NULL);
        } else {
            if (all_dirs) {
                add_item(m, menu, "folder-open", "_Open", "Enter", "open", NULL);
                add_item(m, menu, "tab-new|list-add", "Open in New Ta_b", "Ctrl+Enter", "open-tab", NULL);
                add_item(m, menu, "window-new", "Open in New _Window", "Shift+Enter", "open-window", NULL);
            } else {
                char *ct = pane_selected_type(p);
                GAppInfo *app = ct ? g_app_info_get_default_for_type(ct, FALSE) : NULL;
                char *label = app ? g_strdup_printf("_Open With %s", g_app_info_get_display_name(app)) : g_strdup("_Open");
                add_item(m, menu, "document-open", label, "Enter", "open", NULL);
                g_free(label);
                if (app) g_object_unref(app);
                g_free(ct);
            }
            add_item(m, menu, "system-run|document-open", "Open With Other _Application…", NULL, "open-with", NULL);
            if (p->is_search || p->is_recent)
                add_item(m, menu, "folder-open", "Open Item Locatio_n", NULL, "open-location", NULL);
            files_menu_sep(menu);
            add_item(m, menu, "edit-cut", "Cu_t", "Ctrl+X", "cut", NULL);
            add_item(m, menu, "edit-copy", "_Copy", "Ctrl+C", "copy", NULL);
            if (n == 1 && all_dirs) add_item(m, menu, "edit-paste", "Pa_ste Into Folder", NULL, "paste-into", NULL);
            add_item(m, menu, "edit-copy|insert-link", "Copy Pat_h", NULL, "copy-location", NULL);
            files_menu_sep(menu);
            add_item(m, menu, "edit-rename|document-edit", "_Rename…", "F2", "rename", NULL);
            add_item(m, menu, "emblem-symbolic-link|insert-link", "Make Lin_k", NULL, "make-link", NULL);
            add_item(m, menu, "user-trash", "_Move to Trash", "Delete", "trash", NULL);
            add_item(m, menu, "edit-delete", "_Delete Permanently", "Shift+Delete", "delete", NULL);
            files_menu_sep(menu);
            if (has_archive(sel)) add_item(m, menu, "archive-extract|package-x-generic", "E_xtract Here", NULL, "extract", NULL);
            GtkWidget *sub = add_submenu(m, menu, "package-x-generic|archive-insert", "Compr_ess");
            add_item(m, sub, "package-x-generic", "As _ZIP (.zip)", NULL, "compress", g_variant_new_string("zip"));
            add_item(m, sub, "package-x-generic", "As _Tar (.tar.gz)", NULL, "compress", g_variant_new_string("tar.gz"));
            gtk_widget_set_sensitive(gtk_menu_get_attach_widget(GTK_MENU(sub)),
                                     g_action_group_get_action_enabled(G_ACTION_GROUP(w->window), "compress"));
            if (n == 1 && all_dirs) add_item(m, menu, "utilities-terminal", "Open in Termina_l", NULL, "open-terminal", NULL);
            files_menu_sep(menu);
            add_item(m, menu, "document-properties", "_Properties", "Alt+Enter", "properties", NULL);
        }
        files_list_free(sel);
    } else {
        if (p->is_trash) {
            add_item(m, menu, "user-trash", "_Empty Trash", NULL, "empty-trash", NULL);
            add_item(m, menu, "edit-select-all", "Select _All", "Ctrl+A", "select-all", NULL);
        } else {
            add_item(m, menu, "folder-new", "New _Folder…", "Ctrl+Shift+N", "new-folder", NULL);
            add_new_document_menu(m, menu);
            files_menu_sep(menu);
            add_item(m, menu, "edit-paste", "_Paste", "Ctrl+V", "paste", NULL);
            add_item(m, menu, "edit-select-all", "Select _All", "Ctrl+A", "select-all", NULL);
            files_menu_sep(menu);
            add_item(m, menu, "utilities-terminal", "Open in _Terminal", NULL, "open-terminal", NULL);
        }
        files_menu_sep(menu);
        add_check(m, menu, "Show _Hidden Files", prefs.show_hidden, "Ctrl+H", "show-hidden");
        GtkWidget *view = add_submenu(m, menu, "view-grid|view-list", "_View");
        GSList *group = NULL;
        GtkWidget *ri = files_menu_radio(&group, "_Icons", !prefs.list_view);
        g_object_set_data_full(G_OBJECT(ri), "action", g_strdup("view-icons"), g_free);
        g_signal_connect(ri, "activate", G_CALLBACK(on_menu_item), w);
        gtk_menu_shell_append(GTK_MENU_SHELL(view), ri);
        GtkWidget *rl = files_menu_radio(&group, "_List", prefs.list_view);
        g_object_set_data_full(G_OBJECT(rl), "action", g_strdup("view-list"), g_free);
        g_signal_connect(rl, "activate", G_CALLBACK(on_menu_item), w);
        gtk_menu_shell_append(GTK_MENU_SHELL(view), rl);
        add_sort_menu(m, menu);
        files_menu_sep(menu);
        if (!p->is_trash && !p->is_recent && !p->is_search)
            add_item(m, menu, "bookmark-new|user-bookmarks", "_Bookmark This Folder", "Ctrl+D", "bookmark", NULL);
        add_item(m, menu, "document-properties", p->is_trash ? "_Properties" : "P_roperties", "Alt+Enter", "properties", NULL);
    }
    files_log("menu: %s", mb.log->str);
    g_string_free(mb.log, TRUE);
    if (ev) gtk_menu_popup_at_pointer(GTK_MENU(menu), ev);
    else popup_rect(w, menu);
}

/* the menu of the menu button: made again each time it opens (current states) */
static void fill_main_menu(GtkWidget *menu, FilesWindow *w)
{
    GList *kids = gtk_container_get_children(GTK_CONTAINER(menu));
    for (GList *l = kids; l; l = l->next) gtk_widget_destroy(l->data);
    g_list_free(kids);
    update_actions(w);
    MenuBuild mb = { w, menu, g_string_new(NULL) };
    MenuBuild *m = &mb;
    add_item(m, menu, "window-new", "New _Window", "Ctrl+N", "new-window", NULL);
    add_item(m, menu, "tab-new|list-add", "New _Tab", "Ctrl+T", "new-tab", NULL);
    add_item(m, menu, "folder-new", "New _Folder…", "Ctrl+Shift+N", "new-folder", NULL);
    files_menu_sep(menu);
    char *undo = files_undo_label();
    add_item(m, menu, "edit-undo", undo ? undo : "_Undo", "Ctrl+Z", "undo", NULL);
    g_free(undo);
    add_item(m, menu, "edit-paste", "_Paste", "Ctrl+V", "paste", NULL);
    add_item(m, menu, "edit-select-all", "Select _All", "Ctrl+A", "select-all", NULL);
    add_item(m, menu, "edit-select-all", "_Invert Selection", "Ctrl+Shift+I", "invert-selection", NULL);
    files_menu_sep(menu);
    add_check(m, menu, "Show _Hidden Files", prefs.show_hidden, "Ctrl+H", "show-hidden");
    add_check(m, menu, "Show Side_bar", prefs.show_sidebar, "F9", "show-sidebar");
    add_check(m, menu, "Show as _List", prefs.list_view, "Ctrl+2", "toggle-view");
    add_sort_menu(m, menu);
    add_item(m, menu, "zoom-in", "Zoom I_n", "Ctrl++", "zoom-in", NULL);
    add_item(m, menu, "zoom-out", "Zoom _Out", "Ctrl+-", "zoom-out", NULL);
    add_item(m, menu, "zoom-original", "Normal Si_ze", "Ctrl+0", "zoom-normal", NULL);
    files_menu_sep(menu);
    add_item(m, menu, "bookmark-new|user-bookmarks", "Boo_kmark This Folder", "Ctrl+D", "bookmark", NULL);
    add_item(m, menu, "edit-copy", "_Copy Location", NULL, "copy-location", NULL);
    add_item(m, menu, "utilities-terminal", "Open in T_erminal", NULL, "open-terminal", NULL);
    add_item(m, menu, "user-trash", "Empty T_rash", NULL, "empty-trash", NULL);
    files_menu_sep(menu);
    add_item(m, menu, "input-keyboard|preferences-desktop-keyboard", "Ke_yboard Shortcuts", "Ctrl+?", "shortcuts", NULL);
    add_item(m, menu, "help-about", "About Hyggshi Files", NULL, "about", NULL);
    add_item(m, menu, "window-close", "Close Win_dow", NULL, "close-window", NULL);
    files_log("main menu: %s", mb.log->str);
    g_string_free(mb.log, TRUE);
}

static void popup_main_menu(FilesWindow *w)
{
    GtkWidget *menu = g_object_get_data(G_OBJECT(w->menu_btn), "menu");
    fill_main_menu(menu, w);
    gtk_menu_popup_at_widget(GTK_MENU(menu), w->menu_btn, GDK_GRAVITY_SOUTH_EAST, GDK_GRAVITY_NORTH_EAST, NULL);
}

static void on_menu_btn(GtkButton *b, gpointer d)
{
    (void)b;
    popup_main_menu(d);
}

/* ------------------------------------------------------------------ the sidebar */
static void on_sidebar_open(GtkPlacesSidebar *s, GObject *location, GtkPlacesOpenFlags flags, gpointer d)
{
    (void)s;
    FilesWindow *w = d;
    GFile *f = G_FILE(location);
    char *where = files_display_path(f);
    files_log("sidebar: %s", where);
    g_free(where);
    if (flags & GTK_PLACES_OPEN_NEW_TAB) files_window_open_tab(w, f, TRUE);
    else if (flags & GTK_PLACES_OPEN_NEW_WINDOW) files_window_new(gtk_window_get_application(GTK_WINDOW(w->window)), f);
    else pane_go(w->pane, f, TRUE);
    pane_focus(w->pane);
}

static void on_sidebar_error(GtkPlacesSidebar *s, const char *primary, const char *secondary, gpointer d)
{
    (void)s;
    FilesWindow *w = d;
    files_error(GTK_WINDOW(w->window), primary, secondary);
}

static int on_sidebar_drag_action(GtkPlacesSidebar *s, GdkDragContext *ctx, GFile *dest, GList *sources, gpointer d)
{
    (void)sources; (void)d;
    if (files_is_trash(dest)) return GDK_ACTION_MOVE;
    GdkModifierType mods = 0;
    gdk_window_get_device_position(gtk_widget_get_window(GTK_WIDGET(s)), gdk_drag_context_get_device(ctx), NULL, NULL,
                                   &mods);
    if ((mods & GDK_CONTROL_MASK) && (mods & GDK_SHIFT_MASK)) return GDK_ACTION_LINK;
    if (mods & GDK_CONTROL_MASK) return GDK_ACTION_COPY;
    if (mods & GDK_SHIFT_MASK) return GDK_ACTION_MOVE;
    return g_file_is_native(dest) ? GDK_ACTION_MOVE : GDK_ACTION_COPY;
}

static void on_sidebar_drop(GtkPlacesSidebar *s, GObject *dest, GList *sources, GdkDragAction action, gpointer d)
{
    (void)s;
    FilesWindow *w = d;
    GFile *df = G_FILE(dest);
    char *where = files_display_path(df);
    files_log("sidebar drop: %u item(s) on %s", g_list_length(sources), where);
    g_free(where);
    if (files_is_trash(df)) files_job_start(GTK_WINDOW(w->window), JOB_TRASH, sources, NULL);
    else files_job_start(GTK_WINDOW(w->window), action == GDK_ACTION_MOVE ? JOB_MOVE : action == GDK_ACTION_LINK ? JOB_LINK
                                                                                                                 : JOB_COPY,
                         sources, df);
}

/* ------------------------------------------------------------------ keys, window state */
static gboolean on_key(GtkWidget *win, GdkEventKey *ev, gpointer d)
{
    FilesWindow *w = d;
    GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(win));
    if (focus && GTK_IS_EDITABLE(focus) && gtk_window_propagate_key_event(GTK_WINDOW(win), ev)) return TRUE;
    guint mods = ev->state & gtk_accelerator_get_default_mod_mask();
    if (ev->keyval == GDK_KEY_Escape && !mods) {
        if (!g_strcmp0(gtk_stack_get_visible_child_name(GTK_STACK(w->path_stack)), "entry")) {
            show_path(w);
            pane_focus(w->pane);
            return TRUE;
        }
        if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w->search_btn))) {
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w->search_btn), FALSE);
            return TRUE;
        }
        if (pane_selected_count(w->pane) > 0) {
            pane_unselect_all(w->pane);
            return TRUE;
        }
    }
    if (ev->keyval == GDK_KEY_F10 && (mods & GDK_SHIFT_MASK)) {      /* Shift+F10 = the Menu key */
        files_window_popup(w, NULL, pane_selected_count(w->pane) > 0);
        return TRUE;
    }
    return FALSE;
}

static gboolean on_configure(GtkWidget *win, GdkEventConfigure *ev, gpointer d)
{
    (void)ev; (void)d;
    if (!prefs.maximized) {
        int ww, hh;
        gtk_window_get_size(GTK_WINDOW(win), &ww, &hh);
        prefs.win_w = ww;
        prefs.win_h = hh;
    }
    return FALSE;
}

static gboolean on_window_state(GtkWidget *win, GdkEventWindowState *ev, gpointer d)
{
    (void)win; (void)d;
    prefs.maximized = (ev->new_window_state & GDK_WINDOW_STATE_MAXIMIZED) != 0;
    return FALSE;
}

static void on_paned_position(GObject *o, GParamSpec *ps, gpointer d)
{
    (void)ps; (void)d;
    int pos = gtk_paned_get_position(GTK_PANED(o));
    if (pos >= 120 && pos <= 480) prefs.sidebar_width = pos;
}

static void on_destroy(GtkWidget *win, gpointer d)
{
    (void)win;
    FilesWindow *w = d;
    windows = g_list_remove(windows, w);
    prefs_save();
    if (w->status_id) g_source_remove(w->status_id);
    if (w->free_cancel) g_cancellable_cancel(w->free_cancel);
    g_clear_object(&w->free_cancel);
    GList *panes = NULL;
    FOREACH_PANE(w, p, { panes = g_list_prepend(panes, p); });
    for (GList *l = panes; l; l = l->next) pane_free(l->data);    /* (their signal handlers go first) */
    g_list_free(panes);
    files_log("window closed (%u left)", g_list_length(windows));
    g_free(w);
}

static void on_restore_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    FilesWindow *w = d;
    g_action_group_activate_action(G_ACTION_GROUP(w->window), "restore", NULL);
}

static void on_empty_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    FilesWindow *w = d;
    g_action_group_activate_action(G_ACTION_GROUP(w->window), "empty-trash", NULL);
}

static GtkWidget *tool_button(const char *icon, const char *tip, const char *action)
{
    GtkWidget *b = gtk_button_new_from_icon_name(icon, GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(b, tip);
    gtk_widget_set_focus_on_click(b, FALSE);
    if (action) gtk_actionable_set_action_name(GTK_ACTIONABLE(b), action);
    return b;
}

FilesWindow *files_window_new(GtkApplication *app, GFile *location)
{
    FilesWindow *w = g_new0(FilesWindow, 1);
    w->window = gtk_application_window_new(app);
    gtk_window_set_icon_name(GTK_WINDOW(w->window), "system-file-manager");
    gtk_window_set_role(GTK_WINDOW(w->window), "hde-files-window");
    gtk_window_set_default_size(GTK_WINDOW(w->window), prefs.win_w, prefs.win_h);
    if (prefs.maximized) gtk_window_maximize(GTK_WINDOW(w->window));
    g_action_map_add_action_entries(G_ACTION_MAP(w->window), win_actions, G_N_ELEMENTS(win_actions), w);
    set_state(w, "show-hidden", g_variant_new_boolean(prefs.show_hidden));
    set_state(w, "show-sidebar", g_variant_new_boolean(prefs.show_sidebar));

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(w->window), vbox);

    /* tool bar */
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_style_context_add_class(gtk_widget_get_style_context(bar), "files-toolbar");
    GtkWidget *nav = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(nav), "linked");
    w->back_btn = tool_button("go-previous-symbolic", "Back (Alt+Left)", "win.back");
    w->fwd_btn = tool_button("go-next-symbolic", "Forward (Alt+Right)", "win.forward");
    gtk_box_pack_start(GTK_BOX(nav), w->back_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(nav), w->fwd_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), nav, FALSE, FALSE, 0);
    w->up_btn = tool_button("go-up-symbolic", "Up one folder (Alt+Up)", "win.up");
    gtk_box_pack_start(GTK_BOX(bar), w->up_btn, FALSE, FALSE, 0);

    w->path_stack = gtk_stack_new();
    gtk_stack_set_homogeneous(GTK_STACK(w->path_stack), FALSE);
    gtk_widget_set_hexpand(w->path_stack, TRUE);
    w->pathbar_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(w->pathbar_scroll), GTK_POLICY_EXTERNAL, GTK_POLICY_NEVER);
    w->pathbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(w->pathbar), "files-pathbar");
    gtk_container_add(GTK_CONTAINER(w->pathbar_scroll), w->pathbar);
    GtkWidget *pathbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(pathbox), "files-pathbox");
    gtk_box_pack_start(GTK_BOX(pathbox), w->pathbar_scroll, TRUE, TRUE, 0);
    GtkWidget *edit = tool_button("document-edit-symbolic", "Type a location (Ctrl+L)", "win.location");
    gtk_button_set_relief(GTK_BUTTON(edit), GTK_RELIEF_NONE);
    gtk_box_pack_end(GTK_BOX(pathbox), edit, FALSE, FALSE, 0);
    gtk_stack_add_named(GTK_STACK(w->path_stack), pathbox, "path");
    w->location_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(w->location_entry), "Type a folder, a file or an address (~/Documents, sftp://…)");
    gtk_entry_set_icon_from_icon_name(GTK_ENTRY(w->location_entry), GTK_ENTRY_ICON_PRIMARY, "folder-symbolic");
    GtkListStore *cs = gtk_list_store_new(1, G_TYPE_STRING);
    GtkEntryCompletion *comp = gtk_entry_completion_new();
    gtk_entry_completion_set_model(comp, GTK_TREE_MODEL(cs));
    gtk_entry_completion_set_text_column(comp, 0);
    gtk_entry_completion_set_inline_completion(comp, TRUE);
    gtk_entry_completion_set_popup_single_match(comp, FALSE);
    gtk_entry_set_completion(GTK_ENTRY(w->location_entry), comp);
    g_object_set_data_full(G_OBJECT(w->location_entry), "compl-store", cs, g_object_unref);
    g_object_unref(comp);
    g_signal_connect(w->location_entry, "activate", G_CALLBACK(on_location_activate), w);
    g_signal_connect(w->location_entry, "key-press-event", G_CALLBACK(on_location_key), w);
    g_signal_connect(w->location_entry, "focus-out-event", G_CALLBACK(on_location_focus_out), w);
    g_signal_connect(w->location_entry, "changed", G_CALLBACK(on_location_changed), w);
    gtk_stack_add_named(GTK_STACK(w->path_stack), w->location_entry, "entry");
    w->search_entry = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(w->search_entry), "Search in this folder and its subfolders");
    g_signal_connect(w->search_entry, "search-changed", G_CALLBACK(on_search_changed), w);
    g_signal_connect(w->search_entry, "stop-search", G_CALLBACK(on_search_stop), w);
    g_signal_connect(w->search_entry, "key-press-event", G_CALLBACK(on_search_key), w);
    gtk_stack_add_named(GTK_STACK(w->path_stack), w->search_entry, "search");
    gtk_box_pack_start(GTK_BOX(bar), w->path_stack, TRUE, TRUE, 4);

    w->search_btn = gtk_toggle_button_new();
    gtk_container_add(GTK_CONTAINER(w->search_btn), gtk_image_new_from_icon_name("edit-find-symbolic", GTK_ICON_SIZE_BUTTON));
    gtk_widget_set_tooltip_text(w->search_btn, "Search (Ctrl+F)");
    gtk_widget_set_focus_on_click(w->search_btn, FALSE);
    g_signal_connect(w->search_btn, "toggled", G_CALLBACK(on_search_toggled), w);
    gtk_box_pack_start(GTK_BOX(bar), w->search_btn, FALSE, FALSE, 0);
    w->view_btn = gtk_button_new();
    w->view_img = gtk_image_new_from_icon_name("view-list-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_container_add(GTK_CONTAINER(w->view_btn), w->view_img);
    gtk_widget_set_focus_on_click(w->view_btn, FALSE);
    gtk_actionable_set_action_name(GTK_ACTIONABLE(w->view_btn), "win.toggle-view");
    gtk_box_pack_start(GTK_BOX(bar), w->view_btn, FALSE, FALSE, 0);
    w->menu_btn = gtk_button_new_from_icon_name("open-menu-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(w->menu_btn, "Menu (F10)");
    gtk_widget_set_focus_on_click(w->menu_btn, FALSE);
    GtkWidget *main_menu = gtk_menu_new();
    gtk_menu_attach_to_widget(GTK_MENU(main_menu), w->menu_btn, NULL);
    g_object_set_data(G_OBJECT(w->menu_btn), "menu", main_menu);
    g_signal_connect(w->menu_btn, "clicked", G_CALLBACK(on_menu_btn), w);
    gtk_box_pack_start(GTK_BOX(bar), w->menu_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), bar, FALSE, FALSE, 0);

    /* sidebar | tabs */
    w->paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    w->sidebar = gtk_places_sidebar_new();
    GtkPlacesSidebar *sb = GTK_PLACES_SIDEBAR(w->sidebar);
    gtk_places_sidebar_set_open_flags(sb, GTK_PLACES_OPEN_NORMAL | GTK_PLACES_OPEN_NEW_TAB | GTK_PLACES_OPEN_NEW_WINDOW);
    gtk_places_sidebar_set_show_recent(sb, TRUE);
    gtk_places_sidebar_set_show_trash(sb, TRUE);
    gtk_places_sidebar_set_show_desktop(sb, TRUE);
    gtk_places_sidebar_set_show_other_locations(sb, FALSE);
    gtk_places_sidebar_set_show_enter_location(sb, FALSE);
    gtk_places_sidebar_set_local_only(sb, FALSE);
    gtk_widget_set_size_request(w->sidebar, 120, -1);
    g_signal_connect(sb, "open-location", G_CALLBACK(on_sidebar_open), w);
    g_signal_connect(sb, "show-error-message", G_CALLBACK(on_sidebar_error), w);
    g_signal_connect(sb, "drag-action-requested", G_CALLBACK(on_sidebar_drag_action), w);
    g_signal_connect(sb, "drag-perform-drop", G_CALLBACK(on_sidebar_drop), w);
    gtk_paned_pack1(GTK_PANED(w->paned), w->sidebar, FALSE, FALSE);
    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    w->trash_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_style_context_add_class(gtk_widget_get_style_context(w->trash_bar), "files-trashbar");
    gtk_box_pack_start(GTK_BOX(w->trash_bar), gtk_image_new_from_icon_name("user-trash", GTK_ICON_SIZE_LARGE_TOOLBAR),
                       FALSE, FALSE, 0);
    w->trash_label = gtk_label_new("Items in the trash can be restored to where they were, or deleted for good.");
    gtk_label_set_ellipsize(GTK_LABEL(w->trash_label), PANGO_ELLIPSIZE_END);
    gtk_label_set_xalign(GTK_LABEL(w->trash_label), 0);
    gtk_box_pack_start(GTK_BOX(w->trash_bar), w->trash_label, TRUE, TRUE, 0);
    w->trash_restore_btn = gtk_button_new_with_mnemonic("_Restore");
    g_signal_connect(w->trash_restore_btn, "clicked", G_CALLBACK(on_restore_clicked), w);
    gtk_box_pack_start(GTK_BOX(w->trash_bar), w->trash_restore_btn, FALSE, FALSE, 0);
    w->trash_empty_btn = gtk_button_new_with_mnemonic("_Empty Trash");
    gtk_style_context_add_class(gtk_widget_get_style_context(w->trash_empty_btn), "destructive-action");
    g_signal_connect(w->trash_empty_btn, "clicked", G_CALLBACK(on_empty_clicked), w);
    gtk_box_pack_start(GTK_BOX(w->trash_bar), w->trash_empty_btn, FALSE, FALSE, 0);
    gtk_widget_set_no_show_all(w->trash_bar, TRUE);
    gtk_widget_show_all(w->trash_bar);
    gtk_widget_hide(w->trash_bar);
    gtk_box_pack_start(GTK_BOX(right), w->trash_bar, FALSE, FALSE, 0);
    w->notebook = gtk_notebook_new();
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(w->notebook), TRUE);
    gtk_notebook_set_show_border(GTK_NOTEBOOK(w->notebook), FALSE);
    gtk_style_context_add_class(gtk_widget_get_style_context(w->notebook), "files-tabs");
    g_signal_connect(w->notebook, "switch-page", G_CALLBACK(on_switch_page), w);
    gtk_box_pack_start(GTK_BOX(right), w->notebook, TRUE, TRUE, 0);
    gtk_paned_pack2(GTK_PANED(w->paned), right, TRUE, FALSE);
    gtk_paned_set_position(GTK_PANED(w->paned), prefs.sidebar_width);
    g_signal_connect(w->paned, "notify::position", G_CALLBACK(on_paned_position), w);
    gtk_box_pack_start(GTK_BOX(vbox), w->paned, TRUE, TRUE, 0);

    /* status bar */
    GtkWidget *status = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_style_context_add_class(gtk_widget_get_style_context(status), "files-status");
    w->status_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(w->status_label), 0);
    gtk_label_set_ellipsize(GTK_LABEL(w->status_label), PANGO_ELLIPSIZE_MIDDLE);
    gtk_box_pack_start(GTK_BOX(status), w->status_label, TRUE, TRUE, 0);
    w->job_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    w->job_label = gtk_label_new("");
    gtk_label_set_ellipsize(GTK_LABEL(w->job_label), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_max_width_chars(GTK_LABEL(w->job_label), 48);
    w->job_bar = gtk_progress_bar_new();
    gtk_widget_set_valign(w->job_bar, GTK_ALIGN_CENTER);
    gtk_widget_set_size_request(w->job_bar, 120, -1);
    w->job_cancel = gtk_button_new_from_icon_name("process-stop-symbolic", GTK_ICON_SIZE_MENU);
    gtk_button_set_relief(GTK_BUTTON(w->job_cancel), GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text(w->job_cancel, "Cancel");
    g_signal_connect(w->job_cancel, "clicked", G_CALLBACK(files_jobs_cancel), NULL);
    gtk_box_pack_start(GTK_BOX(w->job_box), w->job_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(w->job_box), w->job_bar, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(w->job_box), w->job_cancel, FALSE, FALSE, 0);
    gtk_widget_set_no_show_all(w->job_box, TRUE);
    gtk_widget_show_all(w->job_box);
    gtk_widget_hide(w->job_box);
    gtk_box_pack_start(GTK_BOX(status), w->job_box, FALSE, FALSE, 0);
    w->free_label = gtk_label_new("");
    gtk_style_context_add_class(gtk_widget_get_style_context(w->free_label), "dim-label");
    gtk_box_pack_start(GTK_BOX(status), w->free_label, FALSE, FALSE, 0);
    w->zoom_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 4, 1);
    gtk_scale_set_draw_value(GTK_SCALE(w->zoom_scale), FALSE);
    gtk_range_set_round_digits(GTK_RANGE(w->zoom_scale), 0);
    gtk_range_set_value(GTK_RANGE(w->zoom_scale), prefs_zoom_level());
    gtk_widget_set_size_request(w->zoom_scale, 96, -1);
    gtk_widget_set_tooltip_text(w->zoom_scale, "Size of the icons (Ctrl+plus / Ctrl+minus)");
    gtk_widget_set_no_show_all(w->zoom_scale, TRUE);
    g_signal_connect(w->zoom_scale, "value-changed", G_CALLBACK(on_zoom_scale), w);
    gtk_box_pack_end(GTK_BOX(status), w->zoom_scale, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), status, FALSE, FALSE, 0);

    g_signal_connect(w->window, "key-press-event", G_CALLBACK(on_key), w);
    g_signal_connect(w->window, "configure-event", G_CALLBACK(on_configure), w);
    g_signal_connect(w->window, "window-state-event", G_CALLBACK(on_window_state), w);
    g_signal_connect(w->window, "destroy", G_CALLBACK(on_destroy), w);
    windows = g_list_prepend(windows, w);

    gtk_widget_show_all(vbox);
    gtk_widget_set_visible(w->sidebar, prefs.show_sidebar);
    gtk_widget_set_visible(w->zoom_scale, !prefs.list_view);
    GFile *home = NULL;
    if (!location) location = home = g_file_new_for_path(g_get_home_dir());
    files_window_open_tab(w, location, TRUE);
    if (home) g_object_unref(home);
    files_window_update(w);
    gtk_window_present(GTK_WINDOW(w->window));
    pane_focus(w->pane);
    char *where = files_display_path(location ? location : w->pane->location);
    files_log("window opened: %s", where);
    g_free(where);
    return w;
}
