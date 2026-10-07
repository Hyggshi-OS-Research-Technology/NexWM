/* pane.c — Hyggshi Files: one tab.
 *
 * A GtkListStore with a row per file shown, sorted by itself (folders first, natural order: file2 before file10),
 * shown by a GtkIconView (icons, thumbnails) or a GtkTreeView (list with Size / Type / Modified, sortable columns).
 * Every item is kept in ENTRIES; the store only has those shown (hidden files, the words typed in the Trash / Recent).
 * No GtkTreeModelSort / GtkTreeModelFilter: GtkIconView releases the row at the path of a deleted row (after it went:
 * another one) and rows it found when it got the model, and those models count references (Gtk-CRITICAL
 * gtk_tree_model_sort_real_unref_node); a GtkListStore does not.
 * A folder is read asynchronously in batches and watched
 * (GFileMonitor): files appearing, changing, renamed or removed by any program show at once.
 * Trash (trash:///, read from ~/.local/share/Trash, no GVfs needed), Recent (recent:///, GtkRecentManager) and the
 * search in subfolders are read by a worker thread that sends batches of results.
 */
#include "files.h"
#include <string.h>

#define ATTRS "standard::name,standard::display-name,standard::type,standard::size,standard::icon," \
              "standard::content-type,standard::is-hidden,standard::is-backup,standard::is-symlink," \
              "standard::symlink-target,time::modified,access::can-execute,unix::mode"
#define BATCH 200

static const GtkTargetEntry dnd_targets[] = { { (char *)"text/uri-list", 0, 0 } };

typedef struct { Pane *p; int col; } SortData;

typedef struct {
    Pane *p;
    GCancellable *cancel;
    GFile *file;
} Ctx;

static Ctx *ctx_new(Pane *p, GFile *f)
{
    Ctx *c = g_new0(Ctx, 1);
    c->p = p;
    c->cancel = g_object_ref(p->cancel);
    c->file = f ? g_object_ref(f) : NULL;
    return c;
}

static void ctx_free(Ctx *c)
{
    g_object_unref(c->cancel);
    if (c->file) g_object_unref(c->file);
    g_free(c);
}

static gboolean ctx_dead(Ctx *c)
{
    return g_cancellable_is_cancelled(c->cancel);
}

static int scale_of(Pane *p)
{
    return MAX(1, gtk_widget_get_scale_factor(p->page));
}

GtkWidget *pane_view(Pane *p)
{
    return prefs.list_view ? p->list_view : p->icon_view;
}

void pane_focus(Pane *p)
{
    gtk_widget_grab_focus(pane_view(p));
}

/* ------------------------------------------------------------------ rows */
static GIcon *file_icon(GFileInfo *info)
{
    GIcon *icon = g_file_info_get_icon(info);
    if (icon) g_object_ref(icon);
    else icon = g_themed_icon_new(g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY ? "folder" : "text-x-generic");
    if (g_file_info_get_is_symlink(info)) {
        GIcon *em = g_themed_icon_new("emblem-symbolic-link");
        GEmblem *e = g_emblem_new(em);
        GIcon *r = g_emblemed_icon_new(icon, e);
        g_object_unref(e);
        g_object_unref(em);
        g_object_unref(icon);
        icon = r;
    }
    return icon;
}

static void row_set(Pane *p, GtkTreeIter *it, GFile *file, GFileInfo *info, const char *extra, gint64 time_override)
{
    const char *dname = g_file_info_get_display_name(info);
    if (!dname) dname = g_file_info_get_name(info);
    gboolean is_dir = g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY;
    gboolean hidden = g_file_info_get_is_hidden(info) || g_file_info_get_is_backup(info);
    gint64 size = is_dir ? -1 : g_file_info_get_size(info);
    gint64 mtime = time_override;
    if (!mtime) {
        GDateTime *dt = g_file_info_get_modification_date_time(info);
        if (dt) {
            mtime = g_date_time_to_unix(dt);
            g_date_time_unref(dt);
        }
    }
    const char *ct = g_file_info_get_content_type(info);
    if (!ct) ct = is_dir ? "inode/directory" : "application/octet-stream";
    char *type_text = files_type_description(ct);
    char *size_text = is_dir ? g_strdup("—") : files_format_size(size, FALSE);
    char *time_text = files_format_time(mtime);
    char *key = g_utf8_collate_key_for_filename(dname, -1);
    GIcon *icon = file_icon(info);
    cairo_surface_t *surf = files_icon_surface(icon, prefs.icon_size, scale_of(p));
    gboolean link = g_file_info_get_is_symlink(info);
    char *tip;
    if (link && g_file_info_get_symlink_target(info))
        tip = g_markup_printf_escaped("<b>%s</b>\n%s\nLink to %s", dname, type_text, g_file_info_get_symlink_target(info));
    else if (is_dir) tip = g_markup_printf_escaped("<b>%s</b>\n%s\nModified: %s", dname, type_text, time_text);
    else tip = g_markup_printf_escaped("<b>%s</b>\n%s, %s\nModified: %s", dname, type_text, size_text, time_text);
    gboolean can_exec = !is_dir && g_file_info_get_attribute_boolean(info, G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE);
    gtk_list_store_set(p->store, it,
                       COL_FILE, file, COL_NAME, dname, COL_KEY, key, COL_GICON, icon, COL_SURFACE, surf,
                       COL_IS_DIR, is_dir, COL_HIDDEN, hidden, COL_SIZE, size, COL_SIZE_TEXT, size_text,
                       COL_MTIME, mtime, COL_MTIME_TEXT, time_text, COL_TYPE, ct, COL_TYPE_TEXT, type_text,
                       COL_EXTRA, extra ? extra : "", COL_CUT, files_clipboard_is_cut(file), COL_TOOLTIP, tip,
                       COL_THUMB, 0, COL_CAN_EXEC, can_exec, COL_IS_LINK, link, COL_WEIGHT, PANGO_WEIGHT_NORMAL, -1);
    cairo_surface_destroy(surf);
    g_object_unref(icon);
    g_free(tip);
    g_free(key);
    g_free(time_text);
    g_free(size_text);
    g_free(type_text);
}

static gboolean store_iter_for(Pane *p, GFile *f, GtkTreeIter *it)
{
    char *uri = g_file_get_uri(f);
    GtkTreeIter *row = g_hash_table_lookup(p->rows, uri);   /* (GtkListStore's iters stay valid with their row) */
    g_free(uri);
    if (!row) return FALSE;
    *it = *row;
    return TRUE;
}

static GtkTreePath *view_path_for(Pane *p, GtkTreeIter *store_it)
{
    return gtk_tree_model_get_path(GTK_TREE_MODEL(p->store), store_it);   /* (the views show the store itself) */
}

static void on_thumb(GFile *file, cairo_surface_t *surface, gpointer data)
{
    Pane *p = data;
    GtkTreeIter it;
    if (!store_iter_for(p, file, &it)) return;
    if (surface) gtk_list_store_set(p->store, &it, COL_SURFACE, surface, COL_THUMB, 2, -1);
    else gtk_list_store_set(p->store, &it, COL_THUMB, 3, -1);
}

static void maybe_thumb(Pane *p, GtkTreeIter *it)
{
    if (!prefs.thumbnails || prefs.list_view) return;
    GFile *f = NULL;
    char *ct = NULL;
    gint64 size = 0, mtime = 0;
    int state = 0;
    gtk_tree_model_get(GTK_TREE_MODEL(p->store), it, COL_FILE, &f, COL_TYPE, &ct, COL_SIZE, &size, COL_MTIME, &mtime,
                       COL_THUMB, &state, -1);
    if (state == 0 && f && g_file_is_native(f) && thumbs_possible(ct, size)) {
        gtk_list_store_set(p->store, it, COL_THUMB, 1, -1);
        thumbs_request(f, ct, mtime, prefs.icon_size, scale_of(p), p->cancel, on_thumb, p);
    }
    if (f) g_object_unref(f);
    g_free(ct);
}

static void select_pending(Pane *p);

typedef struct {
    GFile *file;
    GFileInfo *info;
    char *extra;
    gint64 time;
} Entry;

static void entry_free(gpointer d)
{
    Entry *e = d;
    g_object_unref(e->file);
    g_object_unref(e->info);
    g_free(e->extra);
    g_free(e);
}

static gboolean info_hidden(GFileInfo *info)
{
    return g_file_info_get_is_hidden(info) || g_file_info_get_is_backup(info);
}

static gboolean words_match(const char *name, const char *query);

static gboolean entry_visible(Pane *p, Entry *e)
{
    if (info_hidden(e->info) && !prefs.show_hidden && !p->is_trash && !p->is_search) return FALSE;
    if (p->search && *p->search) {
        const char *name = g_file_info_get_display_name(e->info);
        return name && words_match(name, p->search);
    }
    return TRUE;
}

/* the item in the store, or not, as it should be; UPDATE: its row is filled again (new information) */
static void show_entry(Pane *p, const char *uri, Entry *e, gboolean update)
{
    GtkTreeIter it;
    GtkTreeIter *row = g_hash_table_lookup(p->rows, uri);
    if (row) it = *row;
    if (entry_visible(p, e)) {
        if (!row) {
            gtk_list_store_append(p->store, &it);
            GtkTreeIter *copy = g_new(GtkTreeIter, 1);
            *copy = it;
            g_hash_table_insert(p->rows, g_strdup(uri), copy);
            row_set(p, &it, e->file, e->info, e->extra, e->time);
        } else if (update) row_set(p, &it, e->file, e->info, e->extra, e->time);
        else return;
        maybe_thumb(p, &it);
    } else if (row) {
        gtk_list_store_remove(p->store, &it);
        g_hash_table_remove(p->rows, uri);
    }
}

static void row_add(Pane *p, GFile *file, GFileInfo *info, const char *extra, gint64 time_override)
{
    char *uri = g_file_get_uri(file);
    Entry *e = g_hash_table_lookup(p->entries, uri);
    if (e) {
        if (info_hidden(e->info)) p->n_hidden--;
        g_object_unref(e->info);
        e->info = g_object_ref(info);
        g_free(e->extra);
        e->extra = g_strdup(extra);
        e->time = time_override;
    } else {
        e = g_new0(Entry, 1);
        e->file = g_object_ref(file);
        e->info = g_object_ref(info);
        e->extra = g_strdup(extra);
        e->time = time_override;
        g_hash_table_insert(p->entries, g_strdup(uri), e);
        p->n_items++;
    }
    if (info_hidden(info)) p->n_hidden++;
    show_entry(p, uri, e, TRUE);
    g_free(uri);
}

static void row_remove(Pane *p, GFile *file)
{
    char *uri = g_file_get_uri(file);
    Entry *e = g_hash_table_lookup(p->entries, uri);
    if (e) {
        p->n_items--;
        if (info_hidden(e->info)) p->n_hidden--;
    }
    GtkTreeIter it;
    if (store_iter_for(p, file, &it)) gtk_list_store_remove(p->store, &it);
    g_hash_table_remove(p->rows, uri);
    g_hash_table_remove(p->entries, uri);
    g_free(uri);
}

/* hidden files shown or not, other words typed: add / remove rows */
static void apply_visibility(Pane *p)
{
    GHashTableIter hi;
    gpointer k, v;
    g_hash_table_iter_init(&hi, p->entries);
    while (g_hash_table_iter_next(&hi, &k, &v)) show_entry(p, k, v, FALSE);
}

/* ------------------------------------------------------------------ words, sorting */
static gboolean words_match(const char *name, const char *query)
{
    char *n = g_utf8_casefold(name, -1), *q = g_utf8_casefold(query, -1);
    char *nn = g_utf8_normalize(n, -1, G_NORMALIZE_ALL), *qn = g_utf8_normalize(q, -1, G_NORMALIZE_ALL);
    char **words = g_strsplit(qn ? qn : q, " ", -1);
    gboolean ok = TRUE;
    for (int i = 0; words[i] && ok; i++)
        if (*words[i] && !strstr(nn ? nn : n, words[i])) ok = FALSE;
    g_strfreev(words);
    g_free(nn);
    g_free(qn);
    g_free(n);
    g_free(q);
    return ok;
}

static int sort_func(GtkTreeModel *m, GtkTreeIter *a, GtkTreeIter *b, gpointer d)
{
    SortData *sd = d;
    gboolean da = FALSE, db = FALSE;
    gtk_tree_model_get(m, a, COL_IS_DIR, &da, -1);
    gtk_tree_model_get(m, b, COL_IS_DIR, &db, -1);
    if (prefs.folders_first && da != db) {
        int id;
        GtkSortType order = GTK_SORT_ASCENDING;
        gtk_tree_sortable_get_sort_column_id(GTK_TREE_SORTABLE(sd->p->sort), &id, &order);
        int r = da ? -1 : 1;
        return order == GTK_SORT_DESCENDING ? -r : r;      /* (the sort model reverses it: folders stay first) */
    }
    int r = 0;
    if (sd->col == SORT_SIZE || sd->col == SORT_MTIME) {
        gint64 x = 0, y = 0;
        int col = sd->col == SORT_SIZE ? COL_SIZE : COL_MTIME;
        gtk_tree_model_get(m, a, col, &x, -1);
        gtk_tree_model_get(m, b, col, &y, -1);
        r = x < y ? -1 : x > y ? 1 : 0;
    } else if (sd->col == SORT_TYPE) {
        char *x = NULL, *y = NULL;
        gtk_tree_model_get(m, a, COL_TYPE_TEXT, &x, -1);
        gtk_tree_model_get(m, b, COL_TYPE_TEXT, &y, -1);
        r = g_strcmp0(x, y);
        g_free(x);
        g_free(y);
    }
    if (r == 0) {
        char *x = NULL, *y = NULL;
        gtk_tree_model_get(m, a, COL_KEY, &x, -1);
        gtk_tree_model_get(m, b, COL_KEY, &y, -1);
        r = g_strcmp0(x, y);
        g_free(x);
        g_free(y);
    }
    return r;
}

static void on_sort_changed(GtkTreeSortable *s, gpointer d)
{
    Pane *p = d;
    int id;
    GtkSortType order;
    if (!gtk_tree_sortable_get_sort_column_id(s, &id, &order) || id < 0 || id >= SORT_N) return;
    if (id == prefs.sort_by && (order == GTK_SORT_DESCENDING) == prefs.sort_desc) return;
    prefs.sort_by = id;
    prefs.sort_desc = order == GTK_SORT_DESCENDING;
    prefs_save();
    static const char *const names[] = { "name", "size", "type", "modified" };
    files_log("sort: %s%s", names[id], prefs.sort_desc ? " (reversed)" : "");
    for (GList *l = files_windows(); l; l = l->next) {
        FilesWindow *w = l->data;
        int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(w->notebook));
        for (int i = 0; i < n; i++) {
            Pane *o = g_object_get_data(G_OBJECT(gtk_notebook_get_nth_page(GTK_NOTEBOOK(w->notebook), i)), "pane");
            if (o && o != p) pane_apply_prefs(o);
        }
    }
}

/* ------------------------------------------------------------------ status of the load */
static void show_message(Pane *p)
{
    GtkTreeModel *m = p->sort;
    gboolean empty = gtk_tree_model_iter_n_children(m, NULL) == 0;
    const char *text = NULL;
    if (p->error) text = p->error;
    else if (empty && !p->loading) {
        if (p->is_search || (p->search && *p->search)) text = "No results";
        else if (p->is_trash) text = "The trash is empty";
        else if (p->is_recent) text = "No recent files";
        else text = p->n_items > 0 ? "Only hidden files here (Ctrl+H shows them)" : "This folder is empty";
    }
    if (text) {
        gtk_label_set_text(GTK_LABEL(p->message), text);
        gtk_widget_show(p->message);
    } else gtk_widget_hide(p->message);
    if (p->loading) {
        gtk_widget_show(p->spinner);
        gtk_spinner_start(GTK_SPINNER(p->spinner));
    } else {
        gtk_spinner_stop(GTK_SPINNER(p->spinner));
        gtk_widget_hide(p->spinner);
    }
}

static gboolean geometry_cb(gpointer d)
{
    Pane *p = d;
    p->geom_id = 0;
    pane_item_geometry(p);
    return G_SOURCE_REMOVE;
}

static void schedule_geometry(Pane *p)
{
    if (!files_debug) return;
    if (p->geom_id) g_source_remove(p->geom_id);
    p->geom_id = g_timeout_add(400, geometry_cb, p);
}

static void load_finished(Pane *p)
{
    p->loading = FALSE;
    int shown = gtk_tree_model_iter_n_children(p->sort, NULL);
    char *where = files_display_path(p->location);
    gint64 ms = (g_get_monotonic_time() - p->load_start) / 1000;
    if (p->is_search)
        files_log("search '%s' in %s: %d result(s) (%" G_GINT64_FORMAT " ms)", p->search, where, shown, ms);
    else if (p->error) files_log("folder %s: error: %s", where, p->error);
    else files_log("folder %s: %d items (%d hidden), %d shown (%" G_GINT64_FORMAT " ms)", where, p->n_items, p->n_hidden,
                   shown, ms);
    g_free(where);
    show_message(p);
    select_pending(p);
    files_window_update_status(p->win);
    schedule_geometry(p);
}

/* ------------------------------------------------------------------ a folder: GIO enumerator + monitor */
static void next_ready(GObject *src, GAsyncResult *res, gpointer d);

static void enum_ready(GObject *src, GAsyncResult *res, gpointer d);

static void mounted(GObject *src, GAsyncResult *res, gpointer d)
{
    Ctx *c = d;
    GError *e = NULL;
    gboolean ok = g_file_mount_enclosing_volume_finish(G_FILE(src), res, &e);
    if (!ctx_dead(c)) {
        Pane *p = c->p;
        p->mounting = FALSE;
        if (ok) pane_reload(p);
        else {
            g_free(p->error);
            p->error = g_strdup_printf("Cannot open this place: %s", e ? e->message : "?");
            load_finished(p);
        }
    }
    g_clear_error(&e);
    ctx_free(c);
}

static void enum_ready(GObject *src, GAsyncResult *res, gpointer d)
{
    Ctx *c = d;
    GError *e = NULL;
    GFileEnumerator *en = g_file_enumerate_children_finish(G_FILE(src), res, &e);
    if (ctx_dead(c)) {
        if (en) g_object_unref(en);
        g_clear_error(&e);
        ctx_free(c);
        return;
    }
    Pane *p = c->p;
    if (!en) {
        if (g_error_matches(e, G_IO_ERROR, G_IO_ERROR_NOT_MOUNTED) && !p->mounting) {
            p->mounting = TRUE;
            GMountOperation *op = gtk_mount_operation_new(GTK_WINDOW(p->win->window));
            g_file_mount_enclosing_volume(p->location, G_MOUNT_MOUNT_NONE, op, p->cancel, mounted, ctx_new(p, NULL));
            g_object_unref(op);
        } else {
            g_free(p->error);
            if (g_error_matches(e, G_IO_ERROR, G_IO_ERROR_NOT_FOUND)) p->error = g_strdup("This folder does not exist");
            else if (g_error_matches(e, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED))
                p->error = g_strdup("You do not have permission to see the contents of this folder");
            else p->error = g_strdup_printf("This folder cannot be read: %s", e->message);
            load_finished(p);
        }
        g_clear_error(&e);
        ctx_free(c);
        return;
    }
    g_file_enumerator_next_files_async(en, BATCH, G_PRIORITY_DEFAULT, p->cancel, next_ready, c);
}

static void next_ready(GObject *src, GAsyncResult *res, gpointer d)
{
    Ctx *c = d;
    GFileEnumerator *en = G_FILE_ENUMERATOR(src);
    GError *e = NULL;
    GList *infos = g_file_enumerator_next_files_finish(en, res, &e);
    if (ctx_dead(c)) {
        g_list_free_full(infos, g_object_unref);
        g_clear_error(&e);
        g_object_unref(en);
        ctx_free(c);
        return;
    }
    Pane *p = c->p;
    for (GList *l = infos; l; l = l->next) {
        GFile *f = g_file_get_child(p->location, g_file_info_get_name(l->data));
        row_add(p, f, l->data, NULL, 0);
        g_object_unref(f);
    }
    if (infos) {
        g_list_free_full(infos, g_object_unref);
        show_message(p);
        select_pending(p);
        g_file_enumerator_next_files_async(en, BATCH, G_PRIORITY_DEFAULT, p->cancel, next_ready, c);
        return;
    }
    if (e) {
        g_printerr("hde-files: reading the folder: %s\n", e->message);
        g_clear_error(&e);
    }
    g_file_enumerator_close_async(en, G_PRIORITY_DEFAULT, NULL, NULL, NULL);
    g_object_unref(en);
    load_finished(p);
    ctx_free(c);
}

static void query_done(GObject *src, GAsyncResult *res, gpointer d)
{
    Ctx *c = d;
    GFileInfo *info = g_file_query_info_finish(G_FILE(src), res, NULL);
    if (!ctx_dead(c)) {
        Pane *p = c->p;
        if (info) row_add(p, c->file, info, NULL, 0);
        else row_remove(p, c->file);
        if (!p->loading) {
            show_message(p);
            select_pending(p);
            files_window_update_status(p->win);
            schedule_geometry(p);
        }
    }
    if (info) g_object_unref(info);
    ctx_free(c);
}

static void query_add(Pane *p, GFile *f)
{
    g_file_query_info_async(f, ATTRS, G_FILE_QUERY_INFO_NONE, G_PRIORITY_DEFAULT, p->cancel, query_done, ctx_new(p, f));
}

static gboolean reload_cb(gpointer d)
{
    Pane *p = d;
    p->reload_id = 0;
    pane_reload(p);
    return G_SOURCE_REMOVE;
}

static void on_monitor(GFileMonitor *m, GFile *file, GFile *other, GFileMonitorEvent ev, gpointer d)
{
    (void)m;
    Pane *p = d;
    if (p->is_trash || p->is_recent || p->is_search) {     /* lists made by the worker: read again, soon */
        if (!p->reload_id) p->reload_id = g_timeout_add(400, reload_cb, p);
        return;
    }
    switch (ev) {
    case G_FILE_MONITOR_EVENT_CREATED:
    case G_FILE_MONITOR_EVENT_MOVED_IN:
    case G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT:
    case G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED:
        query_add(p, file);
        break;
    case G_FILE_MONITOR_EVENT_DELETED:
    case G_FILE_MONITOR_EVENT_MOVED_OUT:
        row_remove(p, file);
        show_message(p);
        files_window_update_status(p->win);
        schedule_geometry(p);
        break;
    case G_FILE_MONITOR_EVENT_RENAMED:
        row_remove(p, file);
        if (other) query_add(p, other);
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ Trash, Recent, search: a worker thread */
typedef struct {
    GFile *file;
    GFileInfo *info;
    char *extra;
    gint64 time;
} Found;

typedef struct {
    Pane *p;
    GCancellable *cancel;
    int mode;                /* 0 search, 1 trash, 2 recent */
    GFile *root;
    char *query;
    gboolean hidden;
    char **recent;           /* URIs (GtkRecentManager is read in the main thread) */
    GPtrArray *batch;        /* Found */
    gint64 last_flush;
    int found, visited;
} Walk;

typedef struct {
    Pane *p;
    GCancellable *cancel;
    GPtrArray *items;
    gboolean last;
} Batch;

static void found_free(gpointer d)
{
    Found *f = d;
    g_object_unref(f->file);
    g_object_unref(f->info);
    g_free(f->extra);
    g_free(f);
}

static gboolean batch_main(gpointer d)
{
    Batch *b = d;
    if (!g_cancellable_is_cancelled(b->cancel)) {
        Pane *p = b->p;
        for (guint i = 0; i < b->items->len; i++) {
            Found *f = b->items->pdata[i];
            row_add(p, f->file, f->info, f->extra, f->time);
        }
        if (b->last) load_finished(p);
        else show_message(p);
    }
    g_ptr_array_unref(b->items);
    g_object_unref(b->cancel);
    g_free(b);
    return G_SOURCE_REMOVE;
}

static void walk_flush(Walk *w, gboolean last)
{
    Batch *b = g_new0(Batch, 1);
    b->p = w->p;
    b->cancel = g_object_ref(w->cancel);
    b->items = w->batch;
    b->last = last;
    w->batch = g_ptr_array_new_with_free_func(found_free);
    w->last_flush = g_get_monotonic_time();
    g_main_context_invoke(NULL, batch_main, b);
}

static void walk_add(Walk *w, GFile *f, GFileInfo *info, char *extra, gint64 t)
{
    Found *x = g_new0(Found, 1);
    x->file = g_object_ref(f);
    x->info = g_object_ref(info);
    x->extra = extra;
    x->time = t;
    g_ptr_array_add(w->batch, x);
    w->found++;
    if (w->batch->len >= 100 || g_get_monotonic_time() - w->last_flush > 150000) walk_flush(w, FALSE);
}

static char *relative_folder(Walk *w, GFile *dir)
{
    char *rel = g_file_get_relative_path(w->root, dir);
    if (rel) {
        char *d = g_filename_display_name(rel);
        g_free(rel);
        return d;
    }
    return files_location_title(dir);
}

static void search_dir(Walk *w, GFile *dir, int depth)
{
    if (g_cancellable_is_cancelled(w->cancel) || depth > 12 || w->found >= 5000 || w->visited >= 20000) return;
    w->visited++;
    GFileEnumerator *en = g_file_enumerate_children(dir, ATTRS, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, w->cancel, NULL);
    if (!en) return;
    GPtrArray *subdirs = g_ptr_array_new_with_free_func(g_object_unref);
    GFileInfo *info;
    char *rel = NULL;
    while ((info = g_file_enumerator_next_file(en, w->cancel, NULL))) {
        const char *name = g_file_info_get_display_name(info);
        gboolean hidden = g_file_info_get_is_hidden(info) || g_file_info_get_is_backup(info);
        if (hidden && !w->hidden) {
            g_object_unref(info);
            continue;
        }
        GFile *child = g_file_get_child(dir, g_file_info_get_name(info));
        if (name && words_match(name, w->query)) {
            if (!rel) rel = relative_folder(w, dir);
            walk_add(w, child, info, g_strdup(rel), 0);
        }
        if (g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY && !g_file_info_get_is_symlink(info))
            g_ptr_array_add(subdirs, g_object_ref(child));
        g_object_unref(child);
        g_object_unref(info);
    }
    g_object_unref(en);
    g_free(rel);
    for (guint i = 0; i < subdirs->len; i++) search_dir(w, subdirs->pdata[i], depth + 1);
    g_ptr_array_unref(subdirs);
}

static void walk_thread(GTask *task, gpointer so, gpointer data, GCancellable *c)
{
    (void)so; (void)c;
    Walk *w = data;
    if (w->mode == 0) search_dir(w, w->root, 0);
    else if (w->mode == 1) {
        char *dir = trash_files_dir();
        GFile *d = g_file_new_for_path(dir);
        GFileEnumerator *en = g_file_enumerate_children(d, ATTRS, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, w->cancel, NULL);
        GFileInfo *info;
        while (en && (info = g_file_enumerator_next_file(en, w->cancel, NULL))) {
            GFile *f = g_file_get_child(d, g_file_info_get_name(info));
            char *orig = NULL;
            gint64 deleted = 0;
            char *extra = NULL;
            if (trash_read_info(g_file_info_get_name(info), &orig, &deleted) && orig) {
                GFile *of = g_file_new_for_path(orig);
                GFile *op = g_file_get_parent(of);
                extra = op ? files_display_path(op) : g_strdup("/");
                char *bn = g_path_get_basename(orig);    /* the name it had (several items may have the same) */
                char *dn = g_filename_display_name(bn);
                g_file_info_set_display_name(info, dn);
                g_free(dn);
                g_free(bn);
                if (op) g_object_unref(op);
                g_object_unref(of);
            }
            walk_add(w, f, info, extra, deleted);
            g_free(orig);
            g_object_unref(f);
            g_object_unref(info);
        }
        if (en) g_object_unref(en);
        g_object_unref(d);
        g_free(dir);
    } else {
        for (int i = 0; w->recent && w->recent[i] && !g_cancellable_is_cancelled(w->cancel); i++) {
            GFile *f = g_file_new_for_uri(w->recent[i]);
            GFileInfo *info = g_file_query_info(f, ATTRS, G_FILE_QUERY_INFO_NONE, w->cancel, NULL);
            if (info) {
                GFile *parent = g_file_get_parent(f);
                walk_add(w, f, info, parent ? files_display_path(parent) : g_strdup(""), 0);
                if (parent) g_object_unref(parent);
                g_object_unref(info);
            }
            g_object_unref(f);
        }
    }
    walk_flush(w, TRUE);
    g_task_return_boolean(task, TRUE);
}

static void walk_free(gpointer d)
{
    Walk *w = d;
    g_object_unref(w->cancel);
    if (w->root) g_object_unref(w->root);
    g_free(w->query);
    g_strfreev(w->recent);
    g_ptr_array_unref(w->batch);
    g_free(w);
}

static void start_walk(Pane *p, int mode)
{
    Walk *w = g_new0(Walk, 1);
    w->p = p;
    w->cancel = g_object_ref(p->cancel);
    w->mode = mode;
    w->root = p->location ? g_object_ref(p->location) : NULL;
    w->query = g_strdup(p->search ? p->search : "");
    w->hidden = prefs.show_hidden;
    w->batch = g_ptr_array_new_with_free_func(found_free);
    w->last_flush = g_get_monotonic_time();
    if (mode == 2) {
        GList *items = gtk_recent_manager_get_items(gtk_recent_manager_get_default());
        GPtrArray *uris = g_ptr_array_new();
        for (GList *l = items; l; l = l->next) {
            GtkRecentInfo *ri = l->data;
            if (gtk_recent_info_is_local(ri) && gtk_recent_info_exists(ri) && uris->len < 200)
                g_ptr_array_add(uris, g_strdup(gtk_recent_info_get_uri(ri)));
        }
        g_ptr_array_add(uris, NULL);
        w->recent = (char **)g_ptr_array_free(uris, FALSE);
        g_list_free_full(items, (GDestroyNotify)gtk_recent_info_unref);
    }
    GTask *t = g_task_new(NULL, NULL, NULL, NULL);
    g_task_set_task_data(t, w, walk_free);
    g_task_run_in_thread(t, walk_thread);
    g_object_unref(t);
}

/* ------------------------------------------------------------------ going places */
static void update_columns(Pane *p)
{
    gtk_tree_view_column_set_visible(p->col_extra, p->is_trash || p->is_recent || p->is_search);
    gtk_tree_view_column_set_title(p->col_extra, p->is_trash ? "Original Location" : "Location");
    gtk_tree_view_column_set_title(p->col_mtime, p->is_trash ? "Deleted" : "Modified");
}

static void on_recent_changed(GtkRecentManager *m, gpointer d)
{
    (void)m;
    Pane *p = d;
    if (p->is_recent && !p->reload_id) p->reload_id = g_timeout_add(500, reload_cb, p);
}

void pane_reload(Pane *p)
{
    if (p->cancel) g_cancellable_cancel(p->cancel);
    g_clear_object(&p->cancel);
    p->cancel = g_cancellable_new();
    if (p->monitor) {
        g_signal_handlers_disconnect_by_data(p->monitor, p);
        g_file_monitor_cancel(p->monitor);
        g_clear_object(&p->monitor);
    }
    if (p->reload_id) {
        g_source_remove(p->reload_id);
        p->reload_id = 0;
    }
    g_hash_table_remove_all(p->rows);
    g_hash_table_remove_all(p->entries);
    gtk_list_store_clear(p->store);
    p->n_items = p->n_hidden = 0;
    g_clear_pointer(&p->error, g_free);
    p->loading = TRUE;
    p->mounting = FALSE;
    p->load_start = g_get_monotonic_time();
    p->is_trash = files_is_trash(p->location);
    p->is_recent = files_is_recent(p->location);
    p->is_search = p->search && *p->search && !p->is_trash && !p->is_recent;
    update_columns(p);
    show_message(p);

    GFile *watch = NULL;
    if (p->is_trash) {
        char *dir = trash_files_dir();
        g_mkdir_with_parents(dir, 0700);
        watch = g_file_new_for_path(dir);
        g_free(dir);
        start_walk(p, 1);
    } else if (p->is_recent) start_walk(p, 2);
    else if (p->is_search) start_walk(p, 0);
    else {
        watch = g_object_ref(p->location);
        g_file_enumerate_children_async(p->location, ATTRS, G_FILE_QUERY_INFO_NONE, G_PRIORITY_DEFAULT, p->cancel,
                                        enum_ready, ctx_new(p, NULL));
    }
    if (watch) {
        p->monitor = g_file_monitor_directory(watch, G_FILE_MONITOR_WATCH_MOVES, NULL, NULL);
        if (p->monitor) g_signal_connect(p->monitor, "changed", G_CALLBACK(on_monitor), p);
        g_object_unref(watch);
    }
    files_window_update_status(p->win);
}

void pane_go(Pane *p, GFile *location, gboolean add_history)
{
    if (!location) return;
    if (p->location && g_file_equal(p->location, location) && !(p->search && *p->search)) {
        pane_reload(p);
        return;
    }
    if (p->location && add_history) {
        p->back = g_list_prepend(p->back, g_object_ref(p->location));
        files_list_free(p->forward);
        p->forward = NULL;
    }
    GFile *old = p->location;                     /* (LOCATION may be the same object) */
    p->location = g_object_ref(location);
    if (old) g_object_unref(old);
    g_clear_pointer(&p->search, g_free);
    if (p->typeahead) g_string_truncate(p->typeahead, 0);
    char *where = files_display_path(location);
    files_log("go: %s", where);
    g_free(where);
    pane_reload(p);
    files_window_location_changed(p);
}

void pane_back(Pane *p)
{
    if (!p->back) return;
    GFile *to = p->back->data;
    p->back = g_list_delete_link(p->back, p->back);
    if (p->location) p->forward = g_list_prepend(p->forward, g_object_ref(p->location));
    GFile *from = p->location ? g_object_ref(p->location) : NULL;
    pane_go(p, to, FALSE);
    if (from) {                                       /* back in the parent: the folder we came from is selected */
        GFile *parent = g_file_get_parent(from);
        if (parent && g_file_equal(parent, to)) {
            GList *one = g_list_append(NULL, from);
            pane_select_files(p, one);
            g_list_free(one);
        }
        if (parent) g_object_unref(parent);
        g_object_unref(from);
    }
    g_object_unref(to);
}

void pane_forward(Pane *p)
{
    if (!p->forward) return;
    GFile *to = p->forward->data;
    p->forward = g_list_delete_link(p->forward, p->forward);
    if (p->location) p->back = g_list_prepend(p->back, g_object_ref(p->location));
    pane_go(p, to, FALSE);
    g_object_unref(to);
}

void pane_up(Pane *p)
{
    if (!p->location || p->is_trash || p->is_recent) return;
    GFile *parent = g_file_get_parent(p->location);
    if (!parent) return;
    GFile *from = g_object_ref(p->location);
    pane_go(p, parent, TRUE);
    GList *one = g_list_append(NULL, from);
    pane_select_files(p, one);
    g_list_free(one);
    g_object_unref(from);
    g_object_unref(parent);
}

void pane_set_search(Pane *p, const char *text)
{
    const char *t = text && *text ? text : NULL;
    if (!g_strcmp0(t, p->search)) return;
    g_free(p->search);
    p->search = g_strdup(t);
    if (p->is_trash || p->is_recent) {               /* only the list shown is filtered */
        apply_visibility(p);
        show_message(p);
        files_window_update_status(p->win);
        files_log("filter '%s': %d shown", t ? t : "", gtk_tree_model_iter_n_children(p->sort, NULL));
        return;
    }
    pane_reload(p);
    files_window_update_tab(p);
}

/* ------------------------------------------------------------------ preferences */
void pane_apply_prefs(Pane *p)
{
    apply_visibility(p);
    int id;
    GtkSortType order;
    gtk_tree_sortable_get_sort_column_id(GTK_TREE_SORTABLE(p->sort), &id, &order);
    GtkSortType want = prefs.sort_desc ? GTK_SORT_DESCENDING : GTK_SORT_ASCENDING;
    if (id != prefs.sort_by || order != want) {
        g_signal_handlers_block_by_func(p->sort, on_sort_changed, p);
        gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(p->sort), prefs.sort_by, want);
        g_signal_handlers_unblock_by_func(p->sort, on_sort_changed, p);
    } else {                                          /* folders first changed: sort again */
        g_signal_handlers_block_by_func(p->sort, on_sort_changed, p);
        gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(p->sort), GTK_TREE_SORTABLE_UNSORTED_SORT_COLUMN_ID, want);
        gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(p->sort), prefs.sort_by, want);
        g_signal_handlers_unblock_by_func(p->sort, on_sort_changed, p);
    }
    show_message(p);
    schedule_geometry(p);
}

static void icon_view_sizes(Pane *p)
{
    int s = prefs.icon_size;
    int item = MAX(s + 44, 100);
    gtk_icon_view_set_item_width(GTK_ICON_VIEW(p->icon_view), item);
    g_object_set(p->icon_text_cell, "wrap-width", item - 6, NULL);
    gtk_icon_view_set_column_spacing(GTK_ICON_VIEW(p->icon_view), s >= 96 ? 10 : 4);
}

static gboolean rerender_row(GtkTreeModel *m, GtkTreePath *path, GtkTreeIter *it, gpointer d)
{
    (void)path;
    Pane *p = d;
    GIcon *icon = NULL;
    int thumb = 0;
    gtk_tree_model_get(m, it, COL_GICON, &icon, COL_THUMB, &thumb, -1);
    cairo_surface_t *s = files_icon_surface(icon, prefs.icon_size, scale_of(p));
    gtk_list_store_set(p->store, it, COL_SURFACE, s, COL_THUMB, thumb == 3 ? 3 : 0, -1);   /* (failed: not again) */
    cairo_surface_destroy(s);
    if (icon) g_object_unref(icon);
    maybe_thumb(p, it);
    return FALSE;
}

void pane_apply_zoom(Pane *p)
{
    icon_view_sizes(p);
    gtk_tree_model_foreach(GTK_TREE_MODEL(p->store), rerender_row, p);
    schedule_geometry(p);
}

void pane_icons_changed(Pane *p)
{
    gtk_tree_model_foreach(GTK_TREE_MODEL(p->store), rerender_row, p);
}

static gboolean cut_row(GtkTreeModel *m, GtkTreePath *path, GtkTreeIter *it, gpointer d)
{
    (void)path;
    Pane *p = d;
    GFile *f = NULL;
    gboolean was = FALSE;
    gtk_tree_model_get(m, it, COL_FILE, &f, COL_CUT, &was, -1);
    gboolean cut = f && files_clipboard_is_cut(f);
    if (cut != was) gtk_list_store_set(p->store, it, COL_CUT, cut, -1);
    if (f) g_object_unref(f);
    return FALSE;
}

void pane_update_cut(Pane *p)
{
    gtk_tree_model_foreach(GTK_TREE_MODEL(p->store), cut_row, p);
}

GFile *pane_drop_dir(Pane *p)
{
    if (!p->location || p->is_trash || p->is_recent || p->is_search) return NULL;
    return p->location;
}

/* ------------------------------------------------------------------ selection */
static GList *selected_paths(Pane *p)
{
    GList *paths;
    if (prefs.list_view)
        paths = gtk_tree_selection_get_selected_rows(gtk_tree_view_get_selection(GTK_TREE_VIEW(p->list_view)), NULL);
    else paths = gtk_icon_view_get_selected_items(GTK_ICON_VIEW(p->icon_view));
    return g_list_sort(paths, (GCompareFunc)gtk_tree_path_compare);
}

GList *pane_selected_files(Pane *p)
{
    GList *paths = selected_paths(p), *files = NULL;
    for (GList *l = paths; l; l = l->next) {
        GtkTreeIter it;
        GFile *f = NULL;
        if (gtk_tree_model_get_iter(p->sort, &it, l->data)) gtk_tree_model_get(p->sort, &it, COL_FILE, &f, -1);
        if (f) files = g_list_prepend(files, f);
    }
    g_list_free_full(paths, (GDestroyNotify)gtk_tree_path_free);
    return g_list_reverse(files);
}

int pane_selected_count(Pane *p)
{
    if (prefs.list_view) return gtk_tree_selection_count_selected_rows(gtk_tree_view_get_selection(GTK_TREE_VIEW(p->list_view)));
    GList *paths = gtk_icon_view_get_selected_items(GTK_ICON_VIEW(p->icon_view));
    int n = (int)g_list_length(paths);
    g_list_free_full(paths, (GDestroyNotify)gtk_tree_path_free);
    return n;
}

void pane_selection_summary(Pane *p, int *n, int *dirs, goffset *bytes, char **first)
{
    *n = 0;
    *dirs = 0;
    *bytes = 0;
    if (first) *first = NULL;
    GList *paths = selected_paths(p);
    for (GList *l = paths; l; l = l->next) {
        GtkTreeIter it;
        if (!gtk_tree_model_get_iter(p->sort, &it, l->data)) continue;
        gboolean d = FALSE;
        gint64 size = 0;
        char *name = NULL;
        gtk_tree_model_get(p->sort, &it, COL_IS_DIR, &d, COL_SIZE, &size, COL_NAME, &name, -1);
        (*n)++;
        if (d) (*dirs)++;
        else if (size > 0) *bytes += size;
        if (first && !*first) *first = name;
        else g_free(name);
    }
    g_list_free_full(paths, (GDestroyNotify)gtk_tree_path_free);
}

gboolean pane_selected_is_dir(Pane *p)
{
    GList *paths = selected_paths(p);
    gboolean d = FALSE;
    GtkTreeIter it;
    if (paths && !paths->next && gtk_tree_model_get_iter(p->sort, &it, paths->data))
        gtk_tree_model_get(p->sort, &it, COL_IS_DIR, &d, -1);
    g_list_free_full(paths, (GDestroyNotify)gtk_tree_path_free);
    return d;
}

char *pane_selected_type(Pane *p)
{
    GList *paths = selected_paths(p);
    char *t = NULL;
    GtkTreeIter it;
    if (paths && gtk_tree_model_get_iter(p->sort, &it, paths->data)) gtk_tree_model_get(p->sort, &it, COL_TYPE, &t, -1);
    g_list_free_full(paths, (GDestroyNotify)gtk_tree_path_free);
    return t;
}

void pane_select_all(Pane *p)
{
    if (prefs.list_view) gtk_tree_selection_select_all(gtk_tree_view_get_selection(GTK_TREE_VIEW(p->list_view)));
    else gtk_icon_view_select_all(GTK_ICON_VIEW(p->icon_view));
}

void pane_unselect_all(Pane *p)
{
    if (prefs.list_view) gtk_tree_selection_unselect_all(gtk_tree_view_get_selection(GTK_TREE_VIEW(p->list_view)));
    else gtk_icon_view_unselect_all(GTK_ICON_VIEW(p->icon_view));
}

static gboolean path_selected(Pane *p, GtkTreePath *path)
{
    if (prefs.list_view) return gtk_tree_selection_path_is_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(p->list_view)), path);
    return gtk_icon_view_path_is_selected(GTK_ICON_VIEW(p->icon_view), path);
}

static void select_path(Pane *p, GtkTreePath *path, gboolean on)
{
    if (prefs.list_view) {
        GtkTreeSelection *s = gtk_tree_view_get_selection(GTK_TREE_VIEW(p->list_view));
        if (on) gtk_tree_selection_select_path(s, path);
        else gtk_tree_selection_unselect_path(s, path);
    } else if (on) gtk_icon_view_select_path(GTK_ICON_VIEW(p->icon_view), path);
    else gtk_icon_view_unselect_path(GTK_ICON_VIEW(p->icon_view), path);
}

/* GtkIconView keeps a "scroll to" row reference for an item it has not placed yet and does not follow the rows that
 * move or go meanwhile (it may scroll to another item). So the cursor goes to an item once the view has placed it. */
static gboolean icon_item_placed(Pane *p, GtkTreePath *path)
{
    GdkRectangle r = { 0, 0, 0, 0 };
    int pad = gtk_icon_view_get_item_padding(GTK_ICON_VIEW(p->icon_view));   /* (the rectangle includes it) */
    return gtk_widget_get_realized(p->icon_view) && gtk_icon_view_get_cell_rect(GTK_ICON_VIEW(p->icon_view), path, NULL, &r)
           && r.width > 2 * pad && r.height > 2 * pad;
}

static gboolean cursor_cb(gpointer d)
{
    Pane *p = d;
    GtkTreeIter it;
    GtkTreePath *path = NULL;
    if (p->cursor_file && !prefs.list_view && store_iter_for(p, p->cursor_file, &it)) path = view_path_for(p, &it);
    if (path && !icon_item_placed(p, path) && ++p->cursor_tries < 50) {
        gtk_tree_path_free(path);
        return G_SOURCE_CONTINUE;                    /* (not placed yet: again in a moment) */
    }
    if (path && icon_item_placed(p, path)) {
        gtk_icon_view_set_cursor(GTK_ICON_VIEW(p->icon_view), path, NULL, FALSE);
        gtk_icon_view_scroll_to_path(GTK_ICON_VIEW(p->icon_view), path, FALSE, 0, 0);
    }
    if (path) gtk_tree_path_free(path);
    g_clear_object(&p->cursor_file);
    p->cursor_id = 0;
    return G_SOURCE_REMOVE;
}

static void cursor_to(Pane *p, GtkTreePath *path)
{
    if (!prefs.list_view) {
        if (icon_item_placed(p, path)) {
            gtk_icon_view_set_cursor(GTK_ICON_VIEW(p->icon_view), path, NULL, FALSE);
            gtk_icon_view_scroll_to_path(GTK_ICON_VIEW(p->icon_view), path, FALSE, 0, 0);
            return;
        }
        GtkTreeIter it;
        GFile *f = NULL;
        if (gtk_tree_model_get_iter(p->sort, &it, path)) gtk_tree_model_get(p->sort, &it, COL_FILE, &f, -1);
        if (!f) return;
        if (p->cursor_file) g_object_unref(p->cursor_file);
        p->cursor_file = f;
        p->cursor_tries = 0;
        if (!p->cursor_id) p->cursor_id = g_timeout_add(60, cursor_cb, p);
        return;
    }
    {
        gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(p->list_view), path, NULL, FALSE, 0, 0);
        GtkTreeSelection *s = gtk_tree_view_get_selection(GTK_TREE_VIEW(p->list_view));
        GList *keep = gtk_tree_selection_get_selected_rows(s, NULL);
        gtk_tree_view_set_cursor(GTK_TREE_VIEW(p->list_view), path, NULL, FALSE);   /* (selects only it) */
        for (GList *l = keep; l; l = l->next) gtk_tree_selection_select_path(s, l->data);
        g_list_free_full(keep, (GDestroyNotify)gtk_tree_path_free);
    }
}

void pane_invert_selection(Pane *p)
{
    int n = gtk_tree_model_iter_n_children(p->sort, NULL);
    for (int i = 0; i < n; i++) {
        GtkTreePath *path = gtk_tree_path_new_from_indices(i, -1);
        select_path(p, path, !path_selected(p, path));
        gtk_tree_path_free(path);
    }
}

static void select_pending(Pane *p)
{
    if (!p->pending_select || !p->pending_select->len) return;
    gboolean first = TRUE;
    for (guint i = 0; i < p->pending_select->len;) {
        GFile *f = g_file_new_for_uri(p->pending_select->pdata[i]);
        GtkTreeIter it;
        GtkTreePath *path = store_iter_for(p, f, &it) ? view_path_for(p, &it) : NULL;
        g_object_unref(f);
        if (!path) {
            i++;
            continue;
        }
        if (p->pending_scroll) {                       /* the first one found: only it, and scrolled to */
            pane_unselect_all(p);
            cursor_to(p, path);
            p->pending_scroll = FALSE;
        }
        select_path(p, path, TRUE);
        if (first) files_log("selected: %s", (char *)p->pending_select->pdata[i]);
        first = FALSE;
        gtk_tree_path_free(path);
        g_ptr_array_remove_index(p->pending_select, i);
    }
}

void pane_select_files(Pane *p, GList *files)
{
    if (!p->pending_select) p->pending_select = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_set_size(p->pending_select, 0);
    for (GList *l = files; l; l = l->next) g_ptr_array_add(p->pending_select, g_file_get_uri(l->data));
    p->pending_scroll = TRUE;
    select_pending(p);
}

/* ------------------------------------------------------------------ type-ahead (icon view; the list has its own) */
static void typeahead(Pane *p, gunichar c)
{
    gint64 now = g_get_monotonic_time();
    if (!p->typeahead) p->typeahead = g_string_new(NULL);
    if (now - p->typeahead_time > G_USEC_PER_SEC) g_string_truncate(p->typeahead, 0);
    p->typeahead_time = now;
    g_string_append_unichar(p->typeahead, c);
    char *want = g_utf8_casefold(p->typeahead->str, -1);
    int n = gtk_tree_model_iter_n_children(p->sort, NULL);
    for (int i = 0; i < n; i++) {
        GtkTreeIter it;
        if (!gtk_tree_model_iter_nth_child(p->sort, &it, NULL, i)) break;
        char *name = NULL;
        gtk_tree_model_get(p->sort, &it, COL_NAME, &name, -1);
        char *cf = name ? g_utf8_casefold(name, -1) : NULL;
        gboolean hit = cf && g_str_has_prefix(cf, want);
        g_free(cf);
        if (hit) {
            GtkTreePath *path = gtk_tree_path_new_from_indices(i, -1);
            pane_unselect_all(p);
            cursor_to(p, path);
            select_path(p, path, TRUE);
            files_log("type-ahead '%s': %s", p->typeahead->str, name);
            gtk_tree_path_free(path);
            g_free(name);
            break;
        }
        g_free(name);
    }
    g_free(want);
}

/* ------------------------------------------------------------------ mouse, keyboard */
static GtkTreePath *path_at(Pane *p, GtkWidget *view, double x, double y)
{
    GtkTreePath *path = NULL;
    if (view == p->icon_view) path = gtk_icon_view_get_path_at_pos(GTK_ICON_VIEW(view), (int)x, (int)y);
    else if (!gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(view), (int)x, (int)y, &path, NULL, NULL, NULL)) path = NULL;
    return path;
}

static GFile *file_at_path(Pane *p, GtkTreePath *path, gboolean *is_dir)
{
    GtkTreeIter it;
    GFile *f = NULL;
    gboolean d = FALSE;
    if (path && gtk_tree_model_get_iter(p->sort, &it, path)) gtk_tree_model_get(p->sort, &it, COL_FILE, &f, COL_IS_DIR, &d, -1);
    if (is_dir) *is_dir = d;
    return f;
}

static gboolean on_view_button(GtkWidget *view, GdkEventButton *ev, gpointer d)
{
    Pane *p = d;
    if (ev->type != GDK_BUTTON_PRESS) return FALSE;
    if (ev->button == 8) { pane_back(p); return TRUE; }
    if (ev->button == 9) { pane_forward(p); return TRUE; }
    if (ev->button != 1 && ev->button != 2 && ev->button != 3) return FALSE;
    GtkTreePath *path = path_at(p, view, ev->x, ev->y);
    gboolean handled = FALSE;
    if (ev->button == 3) {
        gtk_widget_grab_focus(view);
        if (path) {
            if (!path_selected(p, path)) {
                pane_unselect_all(p);
                select_path(p, path, TRUE);
            }
        } else pane_unselect_all(p);
        files_window_popup(p->win, (GdkEvent *)ev, path != NULL);
        handled = TRUE;
    } else if (ev->button == 2 && path) {
        gboolean dir = FALSE;
        GFile *f = file_at_path(p, path, &dir);
        if (f && dir) files_window_open_tab(p->win, f, FALSE);
        if (f) g_object_unref(f);
        handled = TRUE;
    } else if (ev->button == 1 && !path && view == p->list_view &&
               !(ev->state & (GDK_CONTROL_MASK | GDK_SHIFT_MASK))) {
        pane_unselect_all(p);                          /* (the icon view does it itself) */
    }
    if (path) gtk_tree_path_free(path);
    return handled;
}

static gboolean on_view_popup_menu(GtkWidget *view, gpointer d)
{
    (void)view;
    Pane *p = d;
    files_window_popup(p->win, NULL, pane_selected_count(p) > 0);
    return TRUE;
}

static gboolean on_view_key(GtkWidget *view, GdkEventKey *ev, gpointer d)
{
    Pane *p = d;
    guint mods = ev->state & gtk_accelerator_get_default_mod_mask();
    if (ev->keyval == GDK_KEY_BackSpace && !mods) {
        GtkWidget *se = view == p->list_view ? GTK_WIDGET(gtk_tree_view_get_search_entry(GTK_TREE_VIEW(view))) : NULL;
        if (se && gtk_widget_get_mapped(se)) return FALSE;
        if (p->typeahead && p->typeahead->len && g_get_monotonic_time() - p->typeahead_time < G_USEC_PER_SEC) return TRUE;
        pane_back(p);
        return TRUE;
    }
    if (mods & (GDK_CONTROL_MASK | GDK_MOD1_MASK | GDK_SUPER_MASK)) return FALSE;
    gunichar c = gdk_keyval_to_unicode(ev->keyval);
    if (!c || !g_unichar_isprint(c)) return FALSE;
    gboolean fresh = !p->typeahead || !p->typeahead->len || g_get_monotonic_time() - p->typeahead_time > G_USEC_PER_SEC;
    if ((c == '/' || c == '~') && fresh) {
        char s[8] = { 0 };
        g_unichar_to_utf8(c, s);
        files_window_start_location(p->win, s);
        return TRUE;
    }
    if (view == p->icon_view && (c != ' ' || !fresh)) {
        typeahead(p, c);
        return TRUE;
    }
    return FALSE;
}

static void activate_path(Pane *p, GtkTreePath *path)
{
    gboolean dir = FALSE;
    GFile *f = file_at_path(p, path, &dir);
    if (!f) return;
    GdkModifierType state = 0;
    gtk_get_current_event_state(&state);
    int how = state & GDK_CONTROL_MASK ? 1 : state & GDK_SHIFT_MASK ? 2 : 0;
    GList *files;
    if (path_selected(p, path) && pane_selected_count(p) > 1) files = pane_selected_files(p);
    else files = g_list_append(NULL, g_object_ref(f));
    files_window_activate_items(p->win, files, how);
    files_list_free(files);
    g_object_unref(f);
}

static void on_item_activated(GtkIconView *iv, GtkTreePath *path, gpointer d)
{
    (void)iv;
    activate_path(d, path);
}

static void on_row_activated(GtkTreeView *tv, GtkTreePath *path, GtkTreeViewColumn *col, gpointer d)
{
    (void)tv; (void)col;
    activate_path(d, path);
}

static gboolean sel_cb(gpointer d)
{
    Pane *p = d;
    p->sel_id = 0;
    files_window_selection_changed(p->win);
    if (files_debug) {
        GList *files = pane_selected_files(p);
        char *names = files_names_text(files, 6);
        files_log("selection: %u item(s)%s%s", g_list_length(files), files ? ": " : "", names);
        g_free(names);
        files_list_free(files);
    }
    return G_SOURCE_REMOVE;
}

static void on_selection_changed(gpointer obj, gpointer d)
{
    (void)obj;
    Pane *p = d;
    if (!p->sel_id) p->sel_id = g_timeout_add(60, sel_cb, p);
}

/* ------------------------------------------------------------------ drag and drop */
static void on_drag_data_get(GtkWidget *w, GdkDragContext *ctx, GtkSelectionData *sd, guint info, guint time, gpointer d)
{
    (void)w; (void)ctx; (void)info; (void)time;
    Pane *p = d;
    GList *files = pane_selected_files(p);
    char **uris = g_new0(char *, g_list_length(files) + 1);
    int i = 0;
    for (GList *l = files; l; l = l->next) uris[i++] = g_file_get_uri(l->data);
    gtk_selection_data_set_uris(sd, uris);
    g_signal_stop_emission_by_name(w, "drag-data-get");      /* (the views would add their own row data) */
    files_log("drag: %d item(s)", i);
    g_strfreev(uris);
    files_list_free(files);
}

static void on_drag_data_delete(GtkWidget *w, GdkDragContext *ctx, gpointer d)
{
    (void)ctx; (void)d;
    g_signal_stop_emission_by_name(w, "drag-data-delete");     /* never remove rows: the monitor does */
}

/* the folder a drop at X,Y (widget coordinates) goes into: a folder under the pointer, else the folder shown */
static GFile *drop_target(Pane *p, GtkWidget *view, int x, int y, GtkTreePath **hit, gboolean internal)
{
    GtkTreePath *path = NULL;
    if (hit) *hit = NULL;
    if (view == p->icon_view) {
        GtkIconViewDropPosition pos;
        if (!gtk_icon_view_get_dest_item_at_pos(GTK_ICON_VIEW(view), x, y, &path, &pos)) path = NULL;
    } else {
        GtkTreeViewDropPosition pos;
        if (!gtk_tree_view_get_dest_row_at_pos(GTK_TREE_VIEW(view), x, y, &path, &pos)) path = NULL;
    }
    if (path) {
        gboolean dir = FALSE;
        GFile *f = file_at_path(p, path, &dir);
        if (f && dir && !(internal && path_selected(p, path))) {
            if (hit) *hit = path;
            else gtk_tree_path_free(path);
            return f;
        }
        if (f) g_object_unref(f);
        gtk_tree_path_free(path);
    }
    if (p->is_trash) return g_file_new_for_uri("trash:///");
    GFile *dd = pane_drop_dir(p);
    return dd ? g_object_ref(dd) : NULL;
}

static gboolean is_internal(Pane *p, GdkDragContext *ctx)
{
    GtkWidget *src = gtk_drag_get_source_widget(ctx);
    return src && (src == p->icon_view || src == p->list_view);
}

static GdkDragAction choose_action(GtkWidget *view, GdkDragContext *ctx, GFile *target, gboolean internal, Pane *p)
{
    GdkModifierType mods = 0;
    gdk_window_get_device_position(gtk_widget_get_window(view), gdk_drag_context_get_device(ctx), NULL, NULL, &mods);
    GdkDragAction actions = gdk_drag_context_get_actions(ctx), want;
    if (files_is_trash(target)) want = GDK_ACTION_MOVE;
    else if ((mods & GDK_CONTROL_MASK) && (mods & GDK_SHIFT_MASK)) want = GDK_ACTION_LINK;
    else if (mods & GDK_CONTROL_MASK) want = GDK_ACTION_COPY;
    else if (mods & GDK_SHIFT_MASK) want = GDK_ACTION_MOVE;
    else if (internal) want = p->location && g_file_is_native(p->location) && g_file_is_native(target) ? GDK_ACTION_MOVE
                                                                                                       : GDK_ACTION_COPY;
    else want = gdk_drag_context_get_suggested_action(ctx);
    if (!(actions & want)) want = gdk_drag_context_get_suggested_action(ctx);
    return want;
}

static void clear_highlight(Pane *p)
{
    gtk_icon_view_set_drag_dest_item(GTK_ICON_VIEW(p->icon_view), NULL, GTK_ICON_VIEW_DROP_INTO);
    gtk_tree_view_set_drag_dest_row(GTK_TREE_VIEW(p->list_view), NULL, GTK_TREE_VIEW_DROP_INTO_OR_AFTER);
}

static gboolean on_drag_motion(GtkWidget *view, GdkDragContext *ctx, int x, int y, guint time, gpointer d)
{
    Pane *p = d;
    gboolean internal = is_internal(p, ctx);
    GtkTreePath *hit = NULL;
    GFile *target = drop_target(p, view, x, y, &hit, internal);
    if (!target || (internal && !hit && p->location && g_file_equal(target, p->location))) {
        clear_highlight(p);
        gdk_drag_status(ctx, 0, time);
        if (target) g_object_unref(target);
        if (hit) gtk_tree_path_free(hit);
        return TRUE;
    }
    if (view == p->icon_view) gtk_icon_view_set_drag_dest_item(GTK_ICON_VIEW(view), hit, GTK_ICON_VIEW_DROP_INTO);
    else gtk_tree_view_set_drag_dest_row(GTK_TREE_VIEW(view), hit, GTK_TREE_VIEW_DROP_INTO_OR_BEFORE);
    gdk_drag_status(ctx, choose_action(view, ctx, target, internal, p), time);
    g_object_unref(target);
    if (hit) gtk_tree_path_free(hit);
    return TRUE;
}

static void on_drag_leave(GtkWidget *view, GdkDragContext *ctx, guint time, gpointer d)
{
    (void)view; (void)ctx; (void)time;
    clear_highlight(d);
}

static gboolean on_drag_drop(GtkWidget *view, GdkDragContext *ctx, int x, int y, guint time, gpointer d)
{
    (void)x; (void)y;
    (void)d;
    GdkAtom target = gtk_drag_dest_find_target(view, ctx, NULL);
    if (target == GDK_NONE) return FALSE;
    gtk_drag_get_data(view, ctx, target, time);
    return TRUE;
}

typedef struct {
    GList *files;
    GFile *dest;
    FilesWindow *win;
} DropAsk;

static void drop_ask_free(gpointer d)
{
    DropAsk *a = d;
    files_list_free(a->files);
    g_object_unref(a->dest);
    g_free(a);
}

static void on_drop_choice(GtkMenuItem *it, gpointer d)
{
    DropAsk *a = d;
    int kind = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(it), "kind"));
    if (g_list_find(files_windows(), a->win)) files_job_start(GTK_WINDOW(a->win->window), (JobKind)kind, a->files, a->dest);
}

static void on_drag_data_received(GtkWidget *view, GdkDragContext *ctx, int x, int y, GtkSelectionData *sd, guint info,
                                  guint time, gpointer d)
{
    (void)info;
    Pane *p = d;
    clear_highlight(p);
    gboolean internal = is_internal(p, ctx);
    char **uris = gtk_selection_data_get_uris(sd);
    GFile *target = drop_target(p, view, x, y, NULL, internal);
    if (!uris || !uris[0] || !target) {
        gtk_drag_finish(ctx, FALSE, FALSE, time);
        g_strfreev(uris);
        if (target) g_object_unref(target);
        return;
    }
    GList *files = NULL;
    for (int i = 0; uris[i]; i++) files = g_list_append(files, g_file_new_for_uri(uris[i]));
    GdkDragAction action = gdk_drag_context_get_selected_action(ctx);
    char *where = files_display_path(target);
    files_log("drop: %u item(s) on %s (%s)", g_list_length(files), where,
              action == GDK_ACTION_MOVE ? "move" : action == GDK_ACTION_LINK ? "link" : action == GDK_ACTION_ASK ? "ask" : "copy");
    g_free(where);
    if (files_is_trash(target)) files_job_start(GTK_WINDOW(p->win->window), JOB_TRASH, files, NULL);
    else if (action == GDK_ACTION_ASK) {
        GtkWidget *menu = gtk_menu_new();
        DropAsk *a = g_new0(DropAsk, 1);
        a->files = files_list_copy(files);
        a->dest = g_object_ref(target);
        a->win = p->win;
        g_object_set_data_full(G_OBJECT(menu), "ask", a, drop_ask_free);
        static const struct { const char *icon, *label; int kind; } choices[] = {
            { "go-jump", "_Move Here", JOB_MOVE }, { "edit-copy", "_Copy Here", JOB_COPY },
            { "emblem-symbolic-link|insert-link", "_Link Here", JOB_LINK } };
        for (guint i = 0; i < G_N_ELEMENTS(choices); i++) {
            GtkWidget *it = files_menu_item(choices[i].icon, choices[i].label, NULL);
            g_object_set_data(G_OBJECT(it), "kind", GINT_TO_POINTER(choices[i].kind));
            g_signal_connect(it, "activate", G_CALLBACK(on_drop_choice), a);
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), it);
        }
        files_menu_sep(menu);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), files_menu_item("process-stop", "C_ancel", NULL));
        gtk_menu_attach_to_widget(GTK_MENU(menu), view, NULL);
        gtk_menu_popup_at_pointer(GTK_MENU(menu), NULL);
    } else files_job_start(GTK_WINDOW(p->win->window), action == GDK_ACTION_MOVE ? JOB_MOVE :
                           action == GDK_ACTION_LINK ? JOB_LINK : JOB_COPY, files, target);
    gtk_drag_finish(ctx, TRUE, FALSE, time);
    files_list_free(files);
    g_object_unref(target);
    g_strfreev(uris);
}

static void setup_dnd(Pane *p, GtkWidget *view)
{
    GdkDragAction acts = GDK_ACTION_COPY | GDK_ACTION_MOVE | GDK_ACTION_LINK | GDK_ACTION_ASK;
    if (GTK_IS_ICON_VIEW(view))
        gtk_icon_view_enable_model_drag_source(GTK_ICON_VIEW(view), GDK_BUTTON1_MASK, dnd_targets, 1, acts);
    else gtk_tree_view_enable_model_drag_source(GTK_TREE_VIEW(view), GDK_BUTTON1_MASK, dnd_targets, 1, acts);
    gtk_drag_dest_set(view, 0, dnd_targets, 1, acts);
    g_signal_connect(view, "drag-data-get", G_CALLBACK(on_drag_data_get), p);
    g_signal_connect(view, "drag-data-delete", G_CALLBACK(on_drag_data_delete), p);
    g_signal_connect(view, "drag-motion", G_CALLBACK(on_drag_motion), p);
    g_signal_connect(view, "drag-leave", G_CALLBACK(on_drag_leave), p);
    g_signal_connect(view, "drag-drop", G_CALLBACK(on_drag_drop), p);
    g_signal_connect(view, "drag-data-received", G_CALLBACK(on_drag_data_received), p);
}

/* ------------------------------------------------------------------ HDE_DEBUG: where the items are */
void pane_item_geometry(Pane *p)
{
    if (!files_debug) return;
    GtkWidget *v = pane_view(p);
    GtkWidget *top = gtk_widget_get_toplevel(v);
    if (!gtk_widget_get_mapped(v) || !gtk_widget_get_window(top)) return;
    int ox = 0, oy = 0;
    gdk_window_get_origin(gtk_widget_get_window(top), &ox, &oy);
    int n = MIN(gtk_tree_model_iter_n_children(p->sort, NULL), 60);
    for (int i = 0; i < n; i++) {
        GtkTreeIter it;
        if (!gtk_tree_model_iter_nth_child(p->sort, &it, NULL, i)) break;
        char *name = NULL;
        gtk_tree_model_get(p->sort, &it, COL_NAME, &name, -1);
        GtkTreePath *path = gtk_tree_path_new_from_indices(i, -1);
        GdkRectangle r = { 0, 0, 0, 0 };
        int wx = 0, wy = 0;
        gboolean ok;
        if (v == p->icon_view) {
            ok = gtk_icon_view_get_cell_rect(GTK_ICON_VIEW(v), path, NULL, &r);
            wx = r.x;
            wy = r.y;
        } else {
            gtk_tree_view_get_cell_area(GTK_TREE_VIEW(v), path, p->col_name, &r);
            gtk_tree_view_convert_bin_window_to_widget_coords(GTK_TREE_VIEW(v), r.x, r.y, &wx, &wy);
            ok = r.height > 0;
        }
        int tx = 0, ty = 0;
        if (ok && gtk_widget_translate_coordinates(v, top, wx, wy, &tx, &ty))
            files_log("item %s at %d,%d %dx%d", name, ox + tx, oy + ty, r.width, r.height);
        gtk_tree_path_free(path);
        g_free(name);
    }
}

/* Ctrl + the mouse wheel: zoom */
static gboolean on_scroll(GtkWidget *w, GdkEventScroll *ev, gpointer d)
{
    (void)w;
    Pane *p = d;
    static double acc;
    if (!(ev->state & GDK_CONTROL_MASK) || prefs.list_view) return FALSE;
    double dy = 0;
    if (ev->direction == GDK_SCROLL_UP) dy = -1;
    else if (ev->direction == GDK_SCROLL_DOWN) dy = 1;
    else if (ev->direction == GDK_SCROLL_SMOOTH) {
        double dx = 0, sy = 0;
        gdk_event_get_scroll_deltas((GdkEvent *)ev, &dx, &sy);
        acc += sy;
        if (acc > -1 && acc < 1) return TRUE;
        dy = acc;
        acc = 0;
    }
    if (dy != 0) g_action_group_activate_action(G_ACTION_GROUP(p->win->window), dy < 0 ? "zoom-in" : "zoom-out", NULL);
    return TRUE;
}

static void on_view_map(GtkWidget *w, gpointer d)
{
    (void)w;
    schedule_geometry(d);
}

/* ------------------------------------------------------------------ the widgets */
static void cut_cell_func(GtkCellLayout *layout, GtkCellRenderer *cell, GtkTreeModel *m, GtkTreeIter *it, gpointer d)
{
    (void)layout; (void)d;
    gboolean cut = FALSE;
    gtk_tree_model_get(m, it, COL_CUT, &cut, -1);
    g_object_set(cell, "sensitive", !cut, NULL);
}

static void cut_col_func(GtkTreeViewColumn *col, GtkCellRenderer *cell, GtkTreeModel *m, GtkTreeIter *it, gpointer d)
{
    (void)col;
    cut_cell_func(NULL, cell, m, it, d);
}

static GtkTreeViewColumn *text_column(Pane *p, const char *title, int col, int sort_id, float xalign)
{
    GtkCellRenderer *r = gtk_cell_renderer_text_new();
    g_object_set(r, "xalign", xalign, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
    GtkTreeViewColumn *c = gtk_tree_view_column_new_with_attributes(title, r, "text", col, NULL);
    gtk_tree_view_column_set_cell_data_func(c, r, cut_col_func, NULL, NULL);
    gtk_tree_view_column_set_resizable(c, TRUE);
    gtk_tree_view_column_set_sort_column_id(c, sort_id);
    gtk_tree_view_append_column(GTK_TREE_VIEW(p->list_view), c);
    return c;
}

Pane *pane_new(FilesWindow *w)
{
    Pane *p = g_new0(Pane, 1);
    p->win = w;
    p->rows = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    p->entries = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, entry_free);
    p->cancel = g_cancellable_new();
    p->store = gtk_list_store_new(N_COLS, G_TYPE_FILE, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_ICON, CAIRO_GOBJECT_TYPE_SURFACE,
                                  G_TYPE_BOOLEAN, G_TYPE_BOOLEAN, G_TYPE_INT64, G_TYPE_STRING, G_TYPE_INT64, G_TYPE_STRING,
                                  G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_BOOLEAN, G_TYPE_STRING, G_TYPE_INT,
                                  G_TYPE_BOOLEAN, G_TYPE_BOOLEAN, G_TYPE_INT);
    p->sort = GTK_TREE_MODEL(g_object_ref(p->store));
    for (int i = 0; i < SORT_N; i++) {
        SortData *sd = g_new0(SortData, 1);
        sd->p = p;
        sd->col = i;
        gtk_tree_sortable_set_sort_func(GTK_TREE_SORTABLE(p->sort), i, sort_func, sd, g_free);
    }
    gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(p->sort), prefs.sort_by,
                                         prefs.sort_desc ? GTK_SORT_DESCENDING : GTK_SORT_ASCENDING);
    g_signal_connect(p->sort, "sort-column-changed", G_CALLBACK(on_sort_changed), p);

    /* icon view */
    p->icon_view = gtk_icon_view_new_with_model(p->sort);
    GtkIconView *iv = GTK_ICON_VIEW(p->icon_view);
    gtk_icon_view_set_selection_mode(iv, GTK_SELECTION_MULTIPLE);
    gtk_icon_view_set_margin(iv, 12);
    gtk_icon_view_set_row_spacing(iv, 6);
    gtk_icon_view_set_spacing(iv, 4);
    gtk_icon_view_set_tooltip_column(iv, COL_TOOLTIP);
    gtk_icon_view_set_activate_on_single_click(iv, prefs.single_click);
    GtkCellRenderer *pix = gtk_cell_renderer_pixbuf_new();
    g_object_set(pix, "xalign", 0.5, "yalign", 1.0, NULL);
    gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(iv), pix, FALSE);
    gtk_cell_layout_add_attribute(GTK_CELL_LAYOUT(iv), pix, "surface", COL_SURFACE);
    gtk_cell_layout_set_cell_data_func(GTK_CELL_LAYOUT(iv), pix, cut_cell_func, NULL, NULL);
    p->icon_text_cell = gtk_cell_renderer_text_new();
    g_object_set(p->icon_text_cell, "alignment", PANGO_ALIGN_CENTER, "xalign", 0.5, "yalign", 0.0,
                 "wrap-mode", PANGO_WRAP_WORD_CHAR, NULL);
    gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(iv), p->icon_text_cell, FALSE);
    gtk_cell_layout_add_attribute(GTK_CELL_LAYOUT(iv), p->icon_text_cell, "text", COL_NAME);
    gtk_cell_layout_set_cell_data_func(GTK_CELL_LAYOUT(iv), p->icon_text_cell, cut_cell_func, NULL, NULL);
    icon_view_sizes(p);
    g_signal_connect(iv, "item-activated", G_CALLBACK(on_item_activated), p);
    g_signal_connect(iv, "selection-changed", G_CALLBACK(on_selection_changed), p);
    p->icon_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(p->icon_scroll), p->icon_view);

    /* list view */
    p->list_view = gtk_tree_view_new_with_model(p->sort);
    GtkTreeView *tv = GTK_TREE_VIEW(p->list_view);
    gtk_tree_selection_set_mode(gtk_tree_view_get_selection(tv), GTK_SELECTION_MULTIPLE);
    gtk_tree_view_set_rubber_banding(tv, TRUE);
    gtk_tree_view_set_enable_search(tv, TRUE);
    gtk_tree_view_set_search_column(tv, COL_NAME);
    gtk_tree_view_set_activate_on_single_click(tv, prefs.single_click);
    GtkTreeViewColumn *c = p->col_name = gtk_tree_view_column_new();
    gtk_tree_view_column_set_title(c, "Name");
    GtkCellRenderer *ic = gtk_cell_renderer_pixbuf_new();
    g_object_set(ic, "stock-size", GTK_ICON_SIZE_MENU, "xpad", 2, NULL);
    gtk_tree_view_column_pack_start(c, ic, FALSE);
    gtk_tree_view_column_add_attribute(c, ic, "gicon", COL_GICON);
    gtk_tree_view_column_set_cell_data_func(c, ic, cut_col_func, NULL, NULL);
    GtkCellRenderer *nt = gtk_cell_renderer_text_new();
    g_object_set(nt, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
    gtk_tree_view_column_pack_start(c, nt, TRUE);
    gtk_tree_view_column_add_attribute(c, nt, "text", COL_NAME);
    gtk_tree_view_column_set_cell_data_func(c, nt, cut_col_func, NULL, NULL);
    gtk_tree_view_column_set_expand(c, TRUE);
    gtk_tree_view_column_set_resizable(c, TRUE);
    gtk_tree_view_column_set_min_width(c, 220);
    gtk_tree_view_column_set_sort_column_id(c, SORT_NAME);
    gtk_tree_view_append_column(tv, c);
    p->col_size = text_column(p, "Size", COL_SIZE_TEXT, SORT_SIZE, 1.0);
    p->col_type = text_column(p, "Type", COL_TYPE_TEXT, SORT_TYPE, 0.0);
    p->col_mtime = text_column(p, "Modified", COL_MTIME_TEXT, SORT_MTIME, 0.0);
    p->col_extra = text_column(p, "Location", COL_EXTRA, SORT_NAME, 0.0);
    gtk_tree_view_column_set_min_width(p->col_size, 90);
    gtk_tree_view_column_set_min_width(p->col_type, 150);
    gtk_tree_view_column_set_min_width(p->col_mtime, 140);
    gtk_tree_view_column_set_min_width(p->col_extra, 150);
    gtk_tree_view_column_set_sort_column_id(p->col_extra, -1);
    gtk_tree_view_column_set_visible(p->col_extra, FALSE);
    g_signal_connect(tv, "row-activated", G_CALLBACK(on_row_activated), p);
    g_signal_connect(gtk_tree_view_get_selection(tv), "changed", G_CALLBACK(on_selection_changed), p);
    p->list_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(p->list_scroll), p->list_view);

    GtkWidget *views[2] = { p->icon_view, p->list_view };
    for (int i = 0; i < 2; i++) {
        g_signal_connect(views[i], "button-press-event", G_CALLBACK(on_view_button), p);
        g_signal_connect(views[i], "popup-menu", G_CALLBACK(on_view_popup_menu), p);
        g_signal_connect(views[i], "key-press-event", G_CALLBACK(on_view_key), p);
        g_signal_connect(views[i], "map", G_CALLBACK(on_view_map), p);
        setup_dnd(p, views[i]);
    }

    g_signal_connect(p->icon_scroll, "scroll-event", G_CALLBACK(on_scroll), p);
    g_signal_connect(p->list_scroll, "scroll-event", G_CALLBACK(on_scroll), p);
    p->stack = gtk_stack_new();
    gtk_stack_add_named(GTK_STACK(p->stack), p->icon_scroll, "icons");
    gtk_stack_add_named(GTK_STACK(p->stack), p->list_scroll, "list");
    p->overlay = gtk_overlay_new();
    gtk_container_add(GTK_CONTAINER(p->overlay), p->stack);
    p->message = gtk_label_new("");
    gtk_label_set_line_wrap(GTK_LABEL(p->message), TRUE);
    gtk_label_set_justify(GTK_LABEL(p->message), GTK_JUSTIFY_CENTER);
    gtk_widget_set_halign(p->message, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(p->message, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_start(p->message, 24);
    gtk_widget_set_margin_end(p->message, 24);
    gtk_style_context_add_class(gtk_widget_get_style_context(p->message), "files-message");
    gtk_widget_set_no_show_all(p->message, TRUE);
    gtk_overlay_add_overlay(GTK_OVERLAY(p->overlay), p->message);
    gtk_overlay_set_overlay_pass_through(GTK_OVERLAY(p->overlay), p->message, TRUE);
    p->spinner = gtk_spinner_new();
    gtk_widget_set_halign(p->spinner, GTK_ALIGN_END);
    gtk_widget_set_valign(p->spinner, GTK_ALIGN_START);
    gtk_widget_set_margin_top(p->spinner, 8);
    gtk_widget_set_margin_end(p->spinner, 14);
    gtk_widget_set_no_show_all(p->spinner, TRUE);
    gtk_overlay_add_overlay(GTK_OVERLAY(p->overlay), p->spinner);
    gtk_overlay_set_overlay_pass_through(GTK_OVERLAY(p->overlay), p->spinner, TRUE);
    p->page = p->overlay;
    gtk_widget_show_all(p->page);
    gtk_stack_set_visible_child_name(GTK_STACK(p->stack), prefs.list_view ? "list" : "icons");
    g_object_set_data(G_OBJECT(p->page), "pane", p);
    g_signal_connect(gtk_recent_manager_get_default(), "changed", G_CALLBACK(on_recent_changed), p);
    return p;
}

void pane_apply_view(Pane *p)
{
    prefs.list_view = !prefs.list_view;              /* the view shown until now: keep its selection */
    GList *sel = pane_selected_files(p);
    prefs.list_view = !prefs.list_view;
    gtk_stack_set_visible_child_name(GTK_STACK(p->stack), prefs.list_view ? "list" : "icons");
    if (!prefs.list_view) {                           /* thumbnails are only for the icons */
        GtkTreeIter it;
        gboolean ok = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(p->store), &it);
        while (ok) {
            maybe_thumb(p, &it);
            ok = gtk_tree_model_iter_next(GTK_TREE_MODEL(p->store), &it);
        }
    }
    pane_unselect_all(p);
    if (sel) pane_select_files(p, sel);
    files_list_free(sel);
    schedule_geometry(p);
}

void pane_free(Pane *p)
{
    if (!p) return;
    g_cancellable_cancel(p->cancel);
    if (p->monitor) {
        g_signal_handlers_disconnect_by_data(p->monitor, p);
        g_file_monitor_cancel(p->monitor);
        g_object_unref(p->monitor);
    }
    if (p->geom_id) g_source_remove(p->geom_id);
    if (p->reload_id) g_source_remove(p->reload_id);
    if (p->sel_id) g_source_remove(p->sel_id);
    if (p->done_id) g_source_remove(p->done_id);
    if (p->cursor_id) g_source_remove(p->cursor_id);
    if (p->cursor_file) g_object_unref(p->cursor_file);
    g_signal_handlers_disconnect_by_data(gtk_recent_manager_get_default(), p);
    g_signal_handlers_disconnect_by_data(p->sort, p);
    g_signal_handlers_disconnect_by_data(p->icon_view, p);
    g_signal_handlers_disconnect_by_data(p->list_view, p);
    g_signal_handlers_disconnect_by_data(p->icon_scroll, p);
    g_signal_handlers_disconnect_by_data(p->list_scroll, p);
    g_signal_handlers_disconnect_by_data(gtk_tree_view_get_selection(GTK_TREE_VIEW(p->list_view)), p);
    if (p->tab_box) g_signal_handlers_disconnect_by_data(p->tab_box, p);
    g_object_unref(p->cancel);
    g_object_unref(p->sort);
    g_object_unref(p->store);
    g_hash_table_unref(p->rows);
    g_hash_table_unref(p->entries);
    files_list_free(p->back);
    files_list_free(p->forward);
    if (p->location) g_object_unref(p->location);
    if (p->pending_select) g_ptr_array_unref(p->pending_select);
    if (p->typeahead) g_string_free(p->typeahead, TRUE);
    g_free(p->search);
    g_free(p->error);
    g_free(p);
}
