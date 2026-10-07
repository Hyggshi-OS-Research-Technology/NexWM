/* ops.c — Hyggshi Files: file operations.
 *
 * Copy / move / link / move to the trash / delete / restore / empty the trash run in a worker thread (GTask), one
 * job per operation; the status bar of every window shows the progress (and a Cancel button). Conflicts are asked in
 * the main thread while the worker waits: Replace (folders: Merge), Skip, Keep Both, for this item or all of them.
 * Moving within one disk is a rename (instant); across disks it is copy + delete. Copying into the same folder
 * makes "name (copy).ext". Errors do not stop the job: they are listed at the end.
 * Undo (Ctrl+Z): the last 10 operations — copy, move, move to the trash, rename, new folder / document.
 * Compress / Extract use the usual programs (zip, tar, unzip, 7z, bsdtar), asynchronously.
 */
#include "files.h"
#include <string.h>

#define ATTRS_SCAN "standard::name,standard::type,standard::size,standard::is-symlink,id::filesystem"

typedef enum { UNDO_TRASH_CREATED, UNDO_MOVE_BACK, UNDO_RESTORE, UNDO_RENAME } UndoKind;
typedef struct {
    UndoKind kind;
    GList *a;            /* created / moved to / renamed to / (restore) original locations */
    GList *b;            /* (move back, rename) the original locations */
    char *label;
} Undo;
static GQueue undo_stack = G_QUEUE_INIT;

typedef struct {
    JobKind kind;
    GList *sources;      /* GFile */
    GList *targets;      /* exact destinations (undo of a move), else NULL */
    GFile *dest;         /* the folder (copy / move / link) */
    GCancellable *cancel;
    gboolean no_undo;
    char *undo_label;
    gboolean external;   /* compress / extract: a program runs */
    char *external_text;
    GFile *external_result;
    /* progress */
    GMutex lock;
    goffset total_bytes, done_bytes, cur_bytes;
    int total_items, done_items;
    char *current;
    gboolean scanning, same_fs;
    gint64 started;
    /* conflicts */
    int conflict_all;    /* -1: ask */
    /* results */
    GList *created;      /* top-level destinations */
    GList *originals;    /* (move) where they were */
    GList *trashed;      /* (trash) GFile of the original location */
    GList *no_trash;     /* (trash) cannot be moved to the trash */
    GString *errors;
    int n_errors;
} Job;

static GList *jobs;
static guint jobs_tick;

static GtkWindow *any_window(GtkWindow *preferred)
{
    if (preferred && GTK_IS_WINDOW(preferred)) {
        for (GList *l = files_windows(); l; l = l->next)
            if (GTK_WINDOW(((FilesWindow *)l->data)->window) == preferred) return preferred;
    }
    GList *w = files_windows();
    return w ? GTK_WINDOW(((FilesWindow *)w->data)->window) : NULL;
}

static void job_error(Job *j, GFile *f, const char *what, const GError *e)
{
    char *name = f ? g_file_get_parse_name(f) : g_strdup("");
    g_mutex_lock(&j->lock);
    if (j->n_errors < 12) g_string_append_printf(j->errors, "%s%s: %s%s%s", j->errors->len ? "\n" : "", name, what,
                                                 e ? " — " : "", e ? e->message : "");
    j->n_errors++;
    g_mutex_unlock(&j->lock);
    g_printerr("hde-files: %s: %s%s%s\n", name, what, e ? ": " : "", e ? e->message : "");
    g_free(name);
}

static void set_current(Job *j, GFile *f)
{
    char *b = f ? g_file_get_basename(f) : NULL;
    char *d = b ? g_filename_display_name(b) : NULL;
    g_mutex_lock(&j->lock);
    g_free(j->current);
    j->current = d;
    j->cur_bytes = 0;
    g_mutex_unlock(&j->lock);
    g_free(b);
}

/* ------------------------------------------------------------------ asking in the main thread */
typedef struct {
    Job *job;
    GFile *src, *dest;
    int answer;
    gboolean all;
    char *new_name;
    gboolean done;
    GMutex m;
    GCond c;
} Ask;

static gboolean ask_main(gpointer d)
{
    Ask *a = d;
    a->answer = files_conflict_dialog(any_window(NULL), a->src, a->dest, &a->all, &a->new_name);
    g_mutex_lock(&a->m);
    a->done = TRUE;
    g_cond_signal(&a->c);
    g_mutex_unlock(&a->m);
    return G_SOURCE_REMOVE;
}

static int ask_conflict(Job *j, GFile *src, GFile *dest, char **new_name)
{
    *new_name = NULL;
    if (j->conflict_all >= 0) return j->conflict_all;
    Ask a = { j, src, dest, CONFLICT_CANCEL, FALSE, NULL, FALSE, { 0 }, { 0 } };
    g_mutex_init(&a.m);
    g_cond_init(&a.c);
    g_main_context_invoke(NULL, ask_main, &a);
    g_mutex_lock(&a.m);
    while (!a.done) g_cond_wait(&a.c, &a.m);
    g_mutex_unlock(&a.m);
    g_mutex_clear(&a.m);
    g_cond_clear(&a.c);
    if (a.all && (a.answer == CONFLICT_SKIP || a.answer == CONFLICT_REPLACE || a.answer == CONFLICT_KEEP_BOTH))
        j->conflict_all = a.answer;
    *new_name = a.new_name;
    return a.answer;
}

typedef struct {
    gboolean answer, done;
    char *title, *detail;
    GMutex m;
    GCond c;
} Question;

static gboolean question_main(gpointer d)
{
    Question *q = d;
    q->answer = files_confirm(any_window(NULL), q->title, q->detail, "_Delete", TRUE);
    g_mutex_lock(&q->m);
    q->done = TRUE;
    g_cond_signal(&q->c);
    g_mutex_unlock(&q->m);
    return G_SOURCE_REMOVE;
}

static gboolean ask_delete_instead(GList *files)
{
    char *names = files_names_text(files, 3);
    Question q = { FALSE, FALSE, g_strdup("Cannot move to the trash. Delete permanently?"),
                   g_strdup_printf("%s cannot be moved to the trash (this disk has no trash). Deleted items cannot be "
                                   "brought back.", names), { 0 }, { 0 } };
    g_free(names);
    g_mutex_init(&q.m);
    g_cond_init(&q.c);
    g_main_context_invoke(NULL, question_main, &q);
    g_mutex_lock(&q.m);
    while (!q.done) g_cond_wait(&q.c, &q.m);
    g_mutex_unlock(&q.m);
    g_mutex_clear(&q.m);
    g_cond_clear(&q.c);
    g_free(q.title);
    g_free(q.detail);
    return q.answer;
}

/* ------------------------------------------------------------------ the worker */
static void scan(Job *j, GFile *f, int depth)
{
    if (g_cancellable_is_cancelled(j->cancel) || depth > 64) return;
    GFileInfo *info = g_file_query_info(f, ATTRS_SCAN, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, j->cancel, NULL);
    if (!info) return;
    g_mutex_lock(&j->lock);
    j->total_items++;
    if (g_file_info_get_file_type(info) == G_FILE_TYPE_REGULAR) j->total_bytes += g_file_info_get_size(info);
    g_mutex_unlock(&j->lock);
    if (g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY) {
        GFileEnumerator *en = g_file_enumerate_children(f, ATTRS_SCAN, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, j->cancel, NULL);
        GFileInfo *ci;
        while (en && (ci = g_file_enumerator_next_file(en, j->cancel, NULL))) {
            GFile *child = g_file_get_child(f, g_file_info_get_name(ci));
            if (g_file_info_get_file_type(ci) == G_FILE_TYPE_DIRECTORY) scan(j, child, depth + 1);
            else {
                g_mutex_lock(&j->lock);
                j->total_items++;
                if (g_file_info_get_file_type(ci) == G_FILE_TYPE_REGULAR) j->total_bytes += g_file_info_get_size(ci);
                g_mutex_unlock(&j->lock);
            }
            g_object_unref(child);
            g_object_unref(ci);
        }
        if (en) g_object_unref(en);
    }
    g_object_unref(info);
}

static void progress_cb(goffset current, goffset total, gpointer d)
{
    (void)total;
    Job *j = d;
    g_mutex_lock(&j->lock);
    j->cur_bytes = current;
    g_mutex_unlock(&j->lock);
}

static void add_done(Job *j, goffset bytes, int items)
{
    g_mutex_lock(&j->lock);
    j->done_bytes += bytes;
    j->done_items += items;
    j->cur_bytes = 0;
    g_mutex_unlock(&j->lock);
}

static char *fs_id(GFile *f)
{
    GFileInfo *i = g_file_query_info(f, G_FILE_ATTRIBUTE_ID_FILESYSTEM, G_FILE_QUERY_INFO_NONE, NULL, NULL);
    char *id = i ? g_strdup(g_file_info_get_attribute_string(i, G_FILE_ATTRIBUTE_ID_FILESYSTEM)) : NULL;
    if (i) g_object_unref(i);
    return id;
}

static gboolean transfer(Job *j, GFile *src, GFile *dest, gboolean move, gboolean top, GFile **final);

/* copy / move the children of folder SRC into the existing folder DEST */
static gboolean transfer_children(Job *j, GFile *src, GFile *dest, gboolean move)
{
    GError *e = NULL;
    GFileEnumerator *en = g_file_enumerate_children(src, G_FILE_ATTRIBUTE_STANDARD_NAME, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                                    j->cancel, &e);
    if (!en) {
        if (g_error_matches(e, G_IO_ERROR, G_IO_ERROR_CANCELLED)) { g_error_free(e); return FALSE; }
        job_error(j, src, "cannot read the folder", e);
        g_clear_error(&e);
        return TRUE;
    }
    GFileInfo *ci;
    gboolean go_on = TRUE;
    while (go_on && (ci = g_file_enumerator_next_file(en, j->cancel, NULL))) {
        GFile *child = g_file_get_child(src, g_file_info_get_name(ci));
        GFile *dchild = g_file_get_child(dest, g_file_info_get_name(ci));
        go_on = transfer(j, child, dchild, move, FALSE, NULL);
        g_object_unref(dchild);
        g_object_unref(child);
        g_object_unref(ci);
    }
    g_object_unref(en);
    if (g_cancellable_is_cancelled(j->cancel)) return FALSE;
    return go_on;
}

static gboolean transfer(Job *j, GFile *src, GFile *dest, gboolean move, gboolean top, GFile **final)
{
    if (g_cancellable_is_cancelled(j->cancel)) return FALSE;
    GError *e = NULL;
    GFileInfo *info = g_file_query_info(src, ATTRS_SCAN, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, j->cancel, &e);
    if (!info) {
        if (g_error_matches(e, G_IO_ERROR, G_IO_ERROR_CANCELLED)) { g_error_free(e); return FALSE; }
        job_error(j, src, "cannot be read", e);
        g_clear_error(&e);
        return TRUE;
    }
    gboolean is_dir = g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY;
    goffset size = g_file_info_get_file_type(info) == G_FILE_TYPE_REGULAR ? g_file_info_get_size(info) : 0;
    g_object_unref(info);
    set_current(j, src);
    dest = g_object_ref(dest);

    if (g_file_equal(src, dest)) {
        if (move) {                                   /* moved onto itself: nothing to do */
            add_done(j, size, 1);
            g_object_unref(dest);
            return TRUE;
        }
        GFile *parent = g_file_get_parent(dest);       /* copied into its own folder: "name (copy)" */
        char *b = g_file_get_basename(src);
        char *n = files_unique_name(parent, b, TRUE);
        g_object_unref(dest);
        dest = g_file_get_child(parent, n);
        g_free(n);
        g_free(b);
        g_object_unref(parent);
    }
    if (is_dir && (g_file_equal(dest, src) || g_file_has_prefix(dest, src))) {
        job_error(j, src, move ? "a folder cannot be moved into itself" : "a folder cannot be copied into itself", NULL);
        g_object_unref(dest);
        return TRUE;
    }

    gboolean overwrite = FALSE, merge = FALSE;
    for (;;) {
        GFileType dt = g_file_query_file_type(dest, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, j->cancel);
        if (dt == G_FILE_TYPE_UNKNOWN) break;
        char *new_name = NULL;
        int a = ask_conflict(j, src, dest, &new_name);
        if (a == CONFLICT_CANCEL) {
            g_cancellable_cancel(j->cancel);
            g_object_unref(dest);
            return FALSE;
        }
        if (a == CONFLICT_SKIP) {
            add_done(j, size, 1);
            g_object_unref(dest);
            return TRUE;
        }
        GFile *parent = g_file_get_parent(dest);
        if (a == CONFLICT_KEEP_BOTH || a == CONFLICT_RENAME) {
            char *b = new_name && *new_name ? g_strdup(new_name) : g_file_get_basename(dest);
            char *n = a == CONFLICT_RENAME ? g_strdup(b) : files_unique_name(parent, b, FALSE);
            g_object_unref(dest);
            dest = g_file_get_child(parent, n);
            g_free(n);
            g_free(b);
            g_free(new_name);
            g_object_unref(parent);
            continue;                                 /* (a new name given by hand may exist too) */
        }
        g_free(new_name);
        g_object_unref(parent);
        /* Replace: folder onto folder = merge; otherwise the old one goes to the trash first (it can be got back) */
        if (is_dir && dt == G_FILE_TYPE_DIRECTORY) merge = TRUE;
        else if (dt == G_FILE_TYPE_DIRECTORY || is_dir) {
            if (!g_file_trash(dest, j->cancel, NULL) && !files_delete_recursive(dest, j->cancel, &e)) {
                job_error(j, dest, "cannot be replaced", e);
                g_clear_error(&e);
                g_object_unref(dest);
                return TRUE;
            }
        } else overwrite = TRUE;
        break;
    }

    gboolean ok = TRUE;
    if (move && !merge) {                             /* a rename when both are on the same disk */
        if (g_file_move(src, dest, G_FILE_COPY_NOFOLLOW_SYMLINKS | G_FILE_COPY_NO_FALLBACK_FOR_MOVE |
                        (overwrite ? G_FILE_COPY_OVERWRITE : 0), j->cancel, NULL, NULL, &e)) {
            add_done(j, j->same_fs ? 0 : size, 1);
            goto done;
        }
        if (g_error_matches(e, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            g_error_free(e);
            g_object_unref(dest);
            return FALSE;
        }
        if (!g_error_matches(e, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED) &&
            !g_error_matches(e, G_IO_ERROR, G_IO_ERROR_WOULD_RECURSE)) {
            job_error(j, src, "cannot be moved", e);
            g_clear_error(&e);
            g_object_unref(dest);
            return TRUE;
        }
        g_clear_error(&e);                             /* another disk: copy, then delete */
    }
    if (is_dir) {
        if (!merge && !g_file_make_directory(dest, j->cancel, &e)) {
            if (g_error_matches(e, G_IO_ERROR, G_IO_ERROR_CANCELLED)) { g_error_free(e); g_object_unref(dest); return FALSE; }
            job_error(j, dest, "the folder cannot be created", e);
            g_clear_error(&e);
            g_object_unref(dest);
            return TRUE;
        }
        add_done(j, 0, 1);
        ok = transfer_children(j, src, dest, move);
        g_file_copy_attributes(src, dest, G_FILE_COPY_NOFOLLOW_SYMLINKS | G_FILE_COPY_ALL_METADATA, NULL, NULL);
        if (ok && move) g_file_delete(src, NULL, NULL);   /* fails when something inside was skipped: fine */
    } else {
        if (!g_file_copy(src, dest, G_FILE_COPY_NOFOLLOW_SYMLINKS | G_FILE_COPY_ALL_METADATA |
                         (overwrite ? G_FILE_COPY_OVERWRITE : 0), j->cancel, progress_cb, j, &e)) {
            if (g_error_matches(e, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
                g_error_free(e);
                g_file_delete(dest, NULL, NULL);       /* no half-copied file */
                g_object_unref(dest);
                return FALSE;
            }
            job_error(j, src, move ? "cannot be moved" : "cannot be copied", e);
            g_clear_error(&e);
            g_object_unref(dest);
            return TRUE;
        }
        add_done(j, size, 1);
        if (move && !g_file_delete(src, NULL, &e)) {
            job_error(j, src, "was copied but cannot be removed from its folder", e);
            g_clear_error(&e);
        }
    }
done:
    if (top) {
        g_mutex_lock(&j->lock);
        j->created = g_list_append(j->created, g_object_ref(dest));
        if (move) j->originals = g_list_append(j->originals, g_object_ref(src));
        g_mutex_unlock(&j->lock);
    }
    if (final) *final = g_object_ref(dest);
    g_object_unref(dest);
    return ok && !g_cancellable_is_cancelled(j->cancel);
}

static void do_link(Job *j, GFile *src)
{
    char *target = g_file_get_path(src);
    char *b = g_file_get_basename(src);
    GFile *srcdir = g_file_get_parent(src);
    char *name = srcdir && g_file_equal(srcdir, j->dest) ? g_strdup_printf("Link to %s", b) : g_strdup(b);
    char *uname = files_unique_name(j->dest, name, FALSE);
    GFile *link = g_file_get_child(j->dest, uname);
    GError *e = NULL;
    set_current(j, src);
    if (!target) job_error(j, src, "links can only point to files of this computer", NULL);
    else if (g_file_make_symbolic_link(link, target, j->cancel, &e)) {
        g_mutex_lock(&j->lock);
        j->created = g_list_append(j->created, g_object_ref(link));
        g_mutex_unlock(&j->lock);
    } else {
        job_error(j, link, "the link cannot be made", e);
        g_clear_error(&e);
    }
    add_done(j, 0, 1);
    g_object_unref(link);
    if (srcdir) g_object_unref(srcdir);
    g_free(uname);
    g_free(name);
    g_free(b);
    g_free(target);
}

static void do_restore(Job *j, GFile *trashed)
{
    char *name = g_file_get_basename(trashed);
    char *orig = NULL;
    GError *e = NULL;
    set_current(j, trashed);
    if (!trash_read_info(name, &orig, NULL) || !orig) {
        job_error(j, trashed, "where it came from is not known (no .trashinfo)", NULL);
        g_free(name);
        add_done(j, 0, 1);
        return;
    }
    GFile *dest = g_file_new_for_path(orig);
    gboolean go = TRUE;
    while (go && g_file_query_file_type(dest, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL) != G_FILE_TYPE_UNKNOWN) {
        char *new_name = NULL;
        int a = ask_conflict(j, trashed, dest, &new_name);
        GFile *parent = g_file_get_parent(dest);
        if (a == CONFLICT_CANCEL) { g_cancellable_cancel(j->cancel); go = FALSE; }
        else if (a == CONFLICT_SKIP) go = FALSE;
        else if (a == CONFLICT_REPLACE) {
            if (!g_file_trash(dest, j->cancel, &e)) {
                job_error(j, dest, "cannot be replaced", e);
                g_clear_error(&e);
                go = FALSE;
            }
        } else {
            char *b = new_name && *new_name ? g_strdup(new_name) : g_file_get_basename(dest);
            char *n = a == CONFLICT_RENAME ? g_strdup(b) : files_unique_name(parent, b, FALSE);
            g_object_unref(dest);
            dest = g_file_get_child(parent, n);
            g_free(n);
            g_free(b);
        }
        g_free(new_name);
        g_object_unref(parent);
    }
    if (go) {
        if (trash_restore_to(trashed, dest, j->cancel, &e)) {
            g_mutex_lock(&j->lock);
            j->created = g_list_append(j->created, g_object_ref(dest));
            g_mutex_unlock(&j->lock);
            char *p = g_file_get_parse_name(dest);
            files_log("restored: %s -> %s", name, p);
            g_free(p);
        } else {
            job_error(j, trashed, "cannot be restored", e);
            g_clear_error(&e);
        }
    }
    add_done(j, 0, 1);
    g_object_unref(dest);
    g_free(orig);
    g_free(name);
}

static void job_thread(GTask *task, gpointer src_obj, gpointer data, GCancellable *c)
{
    (void)src_obj; (void)c;
    Job *j = data;
    GError *e = NULL;
    switch (j->kind) {
    case JOB_COPY:
    case JOB_MOVE: {
        gboolean move = j->kind == JOB_MOVE;
        j->same_fs = FALSE;
        if (move && j->dest) {                         /* all on the same disk: renames, no need to count bytes */
            char *d = fs_id(j->dest);
            j->same_fs = d != NULL;
            for (GList *l = j->sources; l && j->same_fs; l = l->next) {
                char *s = fs_id(l->data);
                j->same_fs = s && !strcmp(s, d);
                g_free(s);
            }
            g_free(d);
        }
        if (j->targets && move) j->same_fs = TRUE;
        if (!j->same_fs) {
            j->scanning = TRUE;
            for (GList *l = j->sources; l; l = l->next) scan(j, l->data, 0);
            j->scanning = FALSE;
        } else j->total_items = (int)g_list_length(j->sources);
        GList *t = j->targets;
        for (GList *l = j->sources; l; l = l->next) {
            GFile *dest;
            if (t) { dest = g_object_ref(t->data); t = t->next; }
            else {
                char *b = g_file_get_basename(l->data);
                dest = g_file_get_child(j->dest, b);
                g_free(b);
            }
            gboolean go = transfer(j, l->data, dest, move, TRUE, NULL);
            g_object_unref(dest);
            if (!go) break;
        }
        break;
    }
    case JOB_LINK:
        j->total_items = (int)g_list_length(j->sources);
        for (GList *l = j->sources; l && !g_cancellable_is_cancelled(j->cancel); l = l->next) do_link(j, l->data);
        break;
    case JOB_TRASH:
        j->total_items = (int)g_list_length(j->sources);
        for (GList *l = j->sources; l && !g_cancellable_is_cancelled(j->cancel); l = l->next) {
            set_current(j, l->data);
            if (g_file_trash(l->data, j->cancel, &e)) {
                g_mutex_lock(&j->lock);
                j->trashed = g_list_append(j->trashed, g_object_ref(l->data));
                g_mutex_unlock(&j->lock);
            } else if (g_error_matches(e, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED)) {
                j->no_trash = g_list_append(j->no_trash, g_object_ref(l->data));
                g_clear_error(&e);
            } else if (!g_error_matches(e, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
                job_error(j, l->data, "cannot be moved to the trash", e);
                g_clear_error(&e);
            } else g_clear_error(&e);
            add_done(j, 0, 1);
        }
        if (j->no_trash && !g_cancellable_is_cancelled(j->cancel) && ask_delete_instead(j->no_trash)) {
            for (GList *l = j->no_trash; l && !g_cancellable_is_cancelled(j->cancel); l = l->next) {
                set_current(j, l->data);
                if (!files_delete_recursive(l->data, j->cancel, &e)) {
                    job_error(j, l->data, "cannot be deleted", e);
                    g_clear_error(&e);
                }
            }
        }
        break;
    case JOB_DELETE:
        j->total_items = (int)g_list_length(j->sources);
        for (GList *l = j->sources; l && !g_cancellable_is_cancelled(j->cancel); l = l->next) {
            set_current(j, l->data);
            gboolean ok = files_in_trash_dir(l->data) ? trash_delete(l->data, j->cancel, &e)
                                                      : files_delete_recursive(l->data, j->cancel, &e);
            if (!ok) {
                if (!g_error_matches(e, G_IO_ERROR, G_IO_ERROR_CANCELLED)) job_error(j, l->data, "cannot be deleted", e);
                g_clear_error(&e);
            }
            add_done(j, 0, 1);
        }
        break;
    case JOB_RESTORE:
        j->total_items = (int)g_list_length(j->sources);
        for (GList *l = j->sources; l && !g_cancellable_is_cancelled(j->cancel); l = l->next) do_restore(j, l->data);
        break;
    case JOB_EMPTY_TRASH:
        j->total_items = 1;
        if (!trash_empty(j->cancel, &e)) {
            if (!g_error_matches(e, G_IO_ERROR, G_IO_ERROR_CANCELLED)) job_error(j, NULL, "the trash cannot be emptied", e);
            g_clear_error(&e);
        }
        add_done(j, 0, 1);
        break;
    }
    g_task_return_boolean(task, TRUE);
}

/* ------------------------------------------------------------------ jobs in the main thread */
static const char *kind_word(JobKind k)
{
    switch (k) {
    case JOB_COPY: return "copied";
    case JOB_MOVE: return "moved";
    case JOB_LINK: return "linked";
    case JOB_TRASH: return "moved to the trash";
    case JOB_DELETE: return "deleted";
    case JOB_RESTORE: return "restored";
    case JOB_EMPTY_TRASH: return "trash emptied";
    }
    return "";
}

static void undo_push(UndoKind kind, GList *a, GList *b, const char *label)
{
    if (!a) return;
    Undo *u = g_new0(Undo, 1);
    u->kind = kind;
    u->a = files_list_copy(a);
    u->b = files_list_copy(b);
    u->label = g_strdup(label);
    g_queue_push_tail(&undo_stack, u);
    while (g_queue_get_length(&undo_stack) > 10) {
        Undo *old = g_queue_pop_head(&undo_stack);
        files_list_free(old->a);
        files_list_free(old->b);
        g_free(old->label);
        g_free(old);
    }
}

static gboolean jobs_tick_cb(gpointer d)
{
    (void)d;
    files_windows_jobs_changed();
    if (!jobs) {
        jobs_tick = 0;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void job_free(Job *j)
{
    files_list_free(j->sources);
    files_list_free(j->targets);
    files_list_free(j->created);
    files_list_free(j->originals);
    files_list_free(j->trashed);
    files_list_free(j->no_trash);
    if (j->dest) g_object_unref(j->dest);
    if (j->external_result) g_object_unref(j->external_result);
    g_object_unref(j->cancel);
    g_string_free(j->errors, TRUE);
    g_mutex_clear(&j->lock);
    g_free(j->current);
    g_free(j->undo_label);
    g_free(j->external_text);
    g_free(j);
}

static void job_finished(Job *j)
{
    jobs = g_list_remove(jobs, j);
    gboolean cancelled = g_cancellable_is_cancelled(j->cancel);
    char *dest = j->dest ? files_display_path(j->dest) : NULL;
    guint n = g_list_length(j->created) + g_list_length(j->trashed);
    if (j->kind == JOB_DELETE || j->kind == JOB_EMPTY_TRASH) n = (guint)j->done_items;
    if (j->external) files_log("job: done: %s%s", j->external_text, j->n_errors ? " (failed)" : "");
    else files_log("job: %s %u item(s)%s%s%s%s", kind_word(j->kind), n, dest ? " -> " : "", dest ? dest : "",
                   cancelled ? " (cancelled)" : "", j->n_errors ? " (with errors)" : "");
    g_free(dest);
    if (!j->no_undo) {
        if (j->kind == JOB_COPY || j->kind == JOB_LINK)
            undo_push(UNDO_TRASH_CREATED, j->created, NULL, j->kind == JOB_COPY ? "Undo Copy" : "Undo Make Link");
        else if (j->kind == JOB_MOVE) undo_push(UNDO_MOVE_BACK, j->created, j->originals, "Undo Move");
        else if (j->kind == JOB_TRASH) undo_push(UNDO_RESTORE, j->trashed, NULL, "Undo Move to Trash");
        else if (j->kind == JOB_RESTORE) undo_push(UNDO_TRASH_CREATED, j->created, NULL, "Undo Restore");
    }
    if (j->created) {
        GFile *dir = j->dest ? g_object_ref(j->dest) : g_file_get_parent(j->created->data);
        files_windows_job_done(dir, j->created);
        if (dir) g_object_unref(dir);
    }
    if (j->n_errors) {
        char *title = g_strdup_printf("%d item(s) could not be %s", j->n_errors, kind_word(j->kind));
        files_error(any_window(NULL), title, j->errors->str);
        g_free(title);
    }
    job_free(j);
    files_windows_jobs_changed();
    g_application_release(G_APPLICATION(files_app));
}

static void job_done(GObject *src, GAsyncResult *res, gpointer data)
{
    (void)src; (void)res;
    job_finished(data);
}

static Job *job_new(JobKind kind, GList *sources, GFile *dest)
{
    Job *j = g_new0(Job, 1);
    j->kind = kind;
    j->sources = files_list_copy(sources);
    j->dest = dest ? g_object_ref(dest) : NULL;
    j->cancel = g_cancellable_new();
    j->conflict_all = -1;
    j->errors = g_string_new(NULL);
    j->started = g_get_monotonic_time();
    g_mutex_init(&j->lock);
    return j;
}

static void job_run(Job *j)
{
    jobs = g_list_append(jobs, j);
    g_application_hold(G_APPLICATION(files_app));     /* the window may close: the job goes on */
    GTask *t = g_task_new(NULL, NULL, job_done, j);
    g_task_set_task_data(t, j, NULL);
    g_task_run_in_thread(t, job_thread);
    g_object_unref(t);
    if (!jobs_tick) jobs_tick = g_timeout_add(200, jobs_tick_cb, NULL);
    files_windows_jobs_changed();
}

void files_job_start(GtkWindow *parent, JobKind kind, GList *sources, GFile *dest_dir)
{
    (void)parent;
    if (!sources && kind != JOB_EMPTY_TRASH) return;
    char *names = files_names_text(sources, 3);
    char *dest = dest_dir ? files_display_path(dest_dir) : NULL;
    files_log("job: start %s: %s%s%s", kind == JOB_COPY ? "copy" : kind == JOB_MOVE ? "move" : kind == JOB_LINK ? "link" :
              kind == JOB_TRASH ? "trash" : kind == JOB_DELETE ? "delete" : kind == JOB_RESTORE ? "restore" : "empty trash",
              names, dest ? " -> " : "", dest ? dest : "");
    g_free(dest);
    g_free(names);
    job_run(job_new(kind, sources, dest_dir));
}

gboolean files_jobs_running(void)
{
    return jobs != NULL;
}

void files_jobs_cancel(void)
{
    for (GList *l = jobs; l; l = l->next) g_cancellable_cancel(((Job *)l->data)->cancel);
}

char *files_jobs_status(double *fraction)
{
    if (fraction) *fraction = -1;
    if (!jobs) return NULL;
    Job *j = g_list_last(jobs)->data;
    if (j->external) {
        return g_strdup(j->external_text);
    }
    g_mutex_lock(&j->lock);
    char *cur = g_strdup(j->current ? j->current : "");
    goffset tb = j->total_bytes, db = j->done_bytes + j->cur_bytes;
    int ti = j->total_items, di = j->done_items;
    gboolean scanning = j->scanning;
    g_mutex_unlock(&j->lock);
    GString *s = g_string_new(NULL);
    guint more = g_list_length(jobs) - 1;
    if (scanning) g_string_append_printf(s, "Preparing… %d item(s), %s", ti, "counting");
    else switch (j->kind) {
    case JOB_COPY:
    case JOB_MOVE:
        if (tb > 0 && !j->same_fs) {
            char *a = g_format_size(db), *b = g_format_size(tb);
            g_string_append_printf(s, "%s “%s” — %s of %s", j->kind == JOB_COPY ? "Copying" : "Moving", cur, a, b);
            g_free(a);
            g_free(b);
            if (fraction) *fraction = CLAMP((double)db / (double)tb, 0, 1);
            gint64 el = (g_get_monotonic_time() - j->started) / G_USEC_PER_SEC;
            if (el >= 3 && db > 0 && db < tb) {
                gint64 left = (gint64)((double)el * (double)(tb - db) / (double)db);
                if (left >= 120) g_string_append_printf(s, ", about %" G_GINT64_FORMAT " minutes left", left / 60);
                else if (left > 0) g_string_append_printf(s, ", %" G_GINT64_FORMAT " s left", left);
            }
        } else {
            g_string_append_printf(s, "%s “%s” (%d of %d)", j->kind == JOB_COPY ? "Copying" : "Moving", cur,
                                   MIN(di + 1, MAX(ti, 1)), MAX(ti, 1));
            if (fraction && ti > 0) *fraction = CLAMP((double)di / ti, 0, 1);
        }
        break;
    case JOB_LINK: g_string_append_printf(s, "Making links (%d of %d)", MIN(di + 1, ti), ti); break;
    case JOB_TRASH:
        g_string_append_printf(s, "Moving “%s” to the trash (%d of %d)", cur, MIN(di + 1, ti), ti);
        if (fraction && ti > 0) *fraction = CLAMP((double)di / ti, 0, 1);
        break;
    case JOB_DELETE:
        g_string_append_printf(s, "Deleting “%s” (%d of %d)", cur, MIN(di + 1, ti), ti);
        if (fraction && ti > 0) *fraction = CLAMP((double)di / ti, 0, 1);
        break;
    case JOB_RESTORE: g_string_append_printf(s, "Restoring “%s” (%d of %d)", cur, MIN(di + 1, ti), ti); break;
    case JOB_EMPTY_TRASH: g_string_append(s, "Emptying the trash…"); break;
    }
    if (more) g_string_append_printf(s, " (+%u more operation%s)", more, more > 1 ? "s" : "");
    g_free(cur);
    return g_string_free(s, FALSE);
}

/* ------------------------------------------------------------------ undo */
void files_undo_push_rename(GFile *from, GFile *to)
{
    GList *a = g_list_append(NULL, to), *b = g_list_append(NULL, from);
    undo_push(UNDO_RENAME, a, b, "Undo Rename");
    g_list_free(a);
    g_list_free(b);
}

void files_undo_push_created(GFile *created)
{
    GList *a = g_list_append(NULL, created);
    undo_push(UNDO_TRASH_CREATED, a, NULL, "Undo New Item");
    g_list_free(a);
}

char *files_undo_label(void)
{
    Undo *u = g_queue_peek_tail(&undo_stack);
    return u ? g_strdup(u->label) : NULL;
}

void files_undo(GtkWindow *parent)
{
    Undo *u = g_queue_pop_tail(&undo_stack);
    if (!u) {
        files_log("undo: nothing to undo");
        return;
    }
    files_log("undo: %s (%u item(s))", u->label, g_list_length(u->a));
    Job *j = NULL;
    switch (u->kind) {
    case UNDO_TRASH_CREATED:
        j = job_new(JOB_TRASH, u->a, NULL);
        break;
    case UNDO_MOVE_BACK:
        j = job_new(JOB_MOVE, u->a, NULL);
        j->targets = files_list_copy(u->b);
        break;
    case UNDO_RESTORE: {
        GList *trashed = NULL;
        char *fd = trash_files_dir();
        for (GList *l = u->a; l; l = l->next) {
            char *orig = g_file_get_path(l->data);
            char *name = orig ? trash_find_newest(orig) : NULL;
            if (name) {
                char *p = g_build_filename(fd, name, NULL);
                trashed = g_list_append(trashed, g_file_new_for_path(p));
                g_free(p);
            }
            g_free(name);
            g_free(orig);
        }
        g_free(fd);
        if (trashed) j = job_new(JOB_RESTORE, trashed, NULL);
        else files_error(parent, "Cannot undo", "The items are no longer in the trash.");
        files_list_free(trashed);
        break;
    }
    case UNDO_RENAME: {
        GError *e = NULL;
        char *old = g_file_get_basename(u->b->data);
        GFile *back = g_file_set_display_name(u->a->data, old, NULL, &e);
        if (back) {
            files_log("renamed back to %s", old);
            g_object_unref(back);
        } else {
            files_error(parent, "Cannot undo the rename", e ? e->message : NULL);
            g_clear_error(&e);
        }
        g_free(old);
        break;
    }
    }
    if (j) {
        j->no_undo = TRUE;
        job_run(j);
    }
    files_list_free(u->a);
    files_list_free(u->b);
    g_free(u->label);
    g_free(u);
}

/* ------------------------------------------------------------------ compress / extract (external programs) */
gboolean files_is_archive(const char *name)
{
    static const char *const exts[] = { ".zip", ".tar", ".tar.gz", ".tgz", ".tar.bz2", ".tbz2", ".tbz", ".tar.xz", ".txz",
                                        ".tar.zst", ".7z", ".rar", ".jar", ".tar.lz", NULL };
    for (int i = 0; exts[i]; i++)
        if (g_str_has_suffix(name, exts[i]) || (strlen(name) > strlen(exts[i]) &&
            !g_ascii_strcasecmp(name + strlen(name) - strlen(exts[i]), exts[i]))) return TRUE;
    return FALSE;
}

gboolean files_have_zip(void)
{
    char *p = g_find_program_in_path("zip");
    gboolean r = p != NULL;
    g_free(p);
    return r;
}

typedef struct {
    Job *job;
    GFile *folder;       /* extract: the new folder */
    GFile *dir;          /* where the result is */
} Ext;

/* extracted into "name/": a single item inside goes up one level (no name/name/...) */
static GFile *flatten(GFile *folder)
{
    GFileEnumerator *en = g_file_enumerate_children(folder, G_FILE_ATTRIBUTE_STANDARD_NAME, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                                    NULL, NULL);
    if (!en) return g_object_ref(folder);
    GFileInfo *a = g_file_enumerator_next_file(en, NULL, NULL);
    GFileInfo *b = a ? g_file_enumerator_next_file(en, NULL, NULL) : NULL;
    g_object_unref(en);
    GFile *result = g_object_ref(folder);
    if (a && !b) {
        GFile *inner = g_file_get_child(folder, g_file_info_get_name(a));
        GFile *parent = g_file_get_parent(folder);
        GFile *up = g_file_get_child(parent, g_file_info_get_name(a));
        if (g_file_query_file_type(up, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL) == G_FILE_TYPE_UNKNOWN &&
            g_file_move(inner, up, G_FILE_COPY_NOFOLLOW_SYMLINKS | G_FILE_COPY_NO_FALLBACK_FOR_MOVE, NULL, NULL, NULL, NULL)) {
            g_file_delete(folder, NULL, NULL);
            g_object_unref(result);
            result = g_object_ref(up);
        }
        g_object_unref(up);
        g_object_unref(parent);
        g_object_unref(inner);
    }
    if (a) g_object_unref(a);
    if (b) g_object_unref(b);
    return result;
}

static void on_external_done(GObject *src, GAsyncResult *res, gpointer d)
{
    Ext *x = d;
    GError *e = NULL;
    gboolean ok = g_subprocess_wait_check_finish(G_SUBPROCESS(src), res, &e);
    if (!ok) {
        job_error(x->job, x->job->external_result, "the program failed", e);
        g_clear_error(&e);
        if (x->folder) files_delete_recursive(x->folder, NULL, NULL);
    } else {
        GFile *result = x->folder ? flatten(x->folder) : g_object_ref(x->job->external_result);
        x->job->created = g_list_append(x->job->created, result);
    }
    x->job->no_undo = TRUE;
    if (x->job->dest) g_object_unref(x->job->dest);
    x->job->dest = g_object_ref(x->dir);
    job_finished(x->job);
    if (x->folder) g_object_unref(x->folder);
    g_object_unref(x->dir);
    g_free(x);
}

static void run_external(Job *j, char **argv, const char *cwd, GFile *folder, GFile *dir)
{
    GSubprocessLauncher *sl = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDIN_PIPE);
    g_subprocess_launcher_set_cwd(sl, cwd);
    GError *e = NULL;
    GSubprocess *sp = g_subprocess_launcher_spawnv(sl, (const char *const *)argv, &e);
    g_object_unref(sl);
    char *cmd = g_strjoinv(" ", argv);
    files_log("running: %s (in %s)", cmd, cwd);
    g_free(cmd);
    jobs = g_list_append(jobs, j);
    g_application_hold(G_APPLICATION(files_app));
    if (!jobs_tick) jobs_tick = g_timeout_add(200, jobs_tick_cb, NULL);
    Ext *x = g_new0(Ext, 1);
    x->job = j;
    x->folder = folder ? g_object_ref(folder) : NULL;
    x->dir = g_object_ref(dir);
    if (!sp) {
        job_error(j, NULL, "the program cannot be started", e);
        g_clear_error(&e);
        if (folder) g_file_delete(folder, NULL, NULL);
        if (x->folder) g_object_unref(x->folder);
        x->folder = NULL;
        j->no_undo = TRUE;
        job_finished(j);
        g_object_unref(x->dir);
        g_free(x);
        return;
    }
    g_subprocess_wait_check_async(sp, j->cancel, on_external_done, x);
    g_object_unref(sp);
    files_windows_jobs_changed();
}

void files_compress(GtkWindow *parent, GList *files, GFile *dir, const char *format)
{
    char *cwd = dir ? g_file_get_path(dir) : NULL;
    if (!files || !cwd) {
        files_error(parent, "Cannot compress here", "Only files of this computer can be compressed.");
        g_free(cwd);
        return;
    }
    gboolean zip = !g_strcmp0(format, "zip");
    char *base = files->next ? g_strdup("Archive") : g_file_get_basename(files->data);
    char *want = g_strconcat(base, zip ? ".zip" : ".tar.gz", NULL);
    char *name = files_unique_name(dir, want, FALSE);
    GPtrArray *argv = g_ptr_array_new_with_free_func(g_free);
    if (zip) {
        g_ptr_array_add(argv, g_strdup("zip"));
        g_ptr_array_add(argv, g_strdup("-r"));
        g_ptr_array_add(argv, g_strdup("-q"));
        g_ptr_array_add(argv, g_strdup("-y"));
        g_ptr_array_add(argv, g_strdup(name));
    } else {
        g_ptr_array_add(argv, g_strdup("tar"));
        g_ptr_array_add(argv, g_strdup("-czf"));
        g_ptr_array_add(argv, g_strdup(name));
        g_ptr_array_add(argv, g_strdup("--"));
    }
    for (GList *l = files; l; l = l->next) g_ptr_array_add(argv, g_file_get_basename(l->data));
    g_ptr_array_add(argv, NULL);
    Job *j = job_new(JOB_COPY, NULL, NULL);
    j->external = TRUE;
    j->external_text = g_strdup_printf("Compressing into “%s”…", name);
    j->external_result = g_file_get_child(dir, name);
    run_external(j, (char **)argv->pdata, cwd, NULL, dir);
    g_ptr_array_unref(argv);
    g_free(name);
    g_free(want);
    g_free(base);
    g_free(cwd);
}

static char *find_prog(const char *a, const char *b)
{
    char *p = g_find_program_in_path(a);
    if (!p && b) p = g_find_program_in_path(b);
    return p;
}

void files_extract(GtkWindow *parent, GList *archives)
{
    for (GList *l = archives; l; l = l->next) {
        GFile *dir = g_file_get_parent(l->data);
        char *cwd = dir ? g_file_get_path(dir) : NULL;
        char *path = g_file_get_path(l->data);
        if (!cwd || !path) {
            files_error(parent, "Cannot extract", "Only archives on this computer can be extracted.");
            g_free(cwd);
            g_free(path);
            if (dir) g_object_unref(dir);
            continue;
        }
        char *bn = g_file_get_basename(l->data), *base;
        const char *ext;
        files_split_ext(bn, &base, &ext);
        char *fname = files_unique_name(dir, base, FALSE);
        GFile *folder = g_file_get_child(dir, fname);
        char *fpath = g_file_get_path(folder);
        char *lower = g_ascii_strdown(bn, -1);
        char *argv[8] = { NULL };
        char *prog = NULL, *o = NULL;
        if (g_str_has_suffix(lower, ".zip") || g_str_has_suffix(lower, ".jar")) {
            if ((prog = find_prog("unzip", NULL))) {
                argv[0] = prog; argv[1] = "-q"; argv[2] = path; argv[3] = "-d"; argv[4] = fpath;
            } else if ((prog = find_prog("bsdtar", NULL))) {
                argv[0] = prog; argv[1] = "-xf"; argv[2] = path; argv[3] = "-C"; argv[4] = fpath;
            } else if ((prog = find_prog("python3", NULL))) {
                argv[0] = prog; argv[1] = "-m"; argv[2] = "zipfile"; argv[3] = "-e"; argv[4] = path; argv[5] = fpath;
            }
        } else if (g_str_has_suffix(lower, ".7z")) {
            if ((prog = find_prog("7z", "7za"))) {
                o = g_strconcat("-o", fpath, NULL);
                argv[0] = prog; argv[1] = "x"; argv[2] = "-y"; argv[3] = o; argv[4] = path;
            }
        } else if (g_str_has_suffix(lower, ".rar")) {
            if ((prog = find_prog("unrar", NULL))) {
                argv[0] = prog; argv[1] = "x"; argv[2] = "-o-"; argv[3] = path; argv[4] = fpath;
            } else if ((prog = find_prog("bsdtar", NULL))) {
                argv[0] = prog; argv[1] = "-xf"; argv[2] = path; argv[3] = "-C"; argv[4] = fpath;
            }
        } else if ((prog = find_prog("tar", "bsdtar"))) {
            argv[0] = prog; argv[1] = "-xf"; argv[2] = path; argv[3] = "-C"; argv[4] = fpath;
        }
        GError *e = NULL;
        if (!prog) files_error(parent, "No program can extract this archive", "Install unzip, p7zip-full or unrar.");
        else if (!g_file_make_directory(folder, NULL, &e)) {
            files_error(parent, "Cannot extract", e->message);
            g_clear_error(&e);
        } else {
            Job *j = job_new(JOB_COPY, NULL, NULL);
            j->external = TRUE;
            j->external_text = g_strdup_printf("Extracting “%s”…", bn);
            j->external_result = g_object_ref(l->data);
            run_external(j, argv, cwd, folder, dir);
        }
        g_free(prog);
        g_free(o);
        g_free(lower);
        g_free(fpath);
        g_object_unref(folder);
        g_free(fname);
        g_free(base);
        g_free(bn);
        g_free(path);
        g_free(cwd);
        g_object_unref(dir);
    }
}
