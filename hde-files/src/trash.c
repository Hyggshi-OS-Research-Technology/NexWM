/* trash.c — Hyggshi Files: the trash of the freedesktop.org specification, without GVfs.
 *
 * $XDG_DATA_HOME/Trash/files/NAME      what was moved to the trash (g_file_trash() puts it there)
 * $XDG_DATA_HOME/Trash/info/NAME.trashinfo
 *     [Trash Info]
 *     Path=/home/me/Documents/report%20final.odt     (escaped like a URI)
 *     DeletionDate=2026-10-07T09:30:12               (local time)
 * Files of other disks go to $topdir/.Trash-$UID (GLib does that); this view shows the trash of the home folder.
 * Everything here may run in the worker thread of the file operations.
 */
#include "files.h"
#include <glib/gstdio.h>
#include <string.h>

char *trash_dir(void)
{
    return g_build_filename(g_get_user_data_dir(), "Trash", NULL);
}

char *trash_files_dir(void)
{
    return g_build_filename(g_get_user_data_dir(), "Trash", "files", NULL);
}

char *trash_info_dir(void)
{
    return g_build_filename(g_get_user_data_dir(), "Trash", "info", NULL);
}

static char *info_path(const char *name)
{
    char *file = g_strconcat(name, ".trashinfo", NULL);
    char *p = g_build_filename(g_get_user_data_dir(), "Trash", "info", file, NULL);
    g_free(file);
    return p;
}

gboolean trash_read_info(const char *name, char **orig_path, gint64 *deleted)
{
    if (orig_path) *orig_path = NULL;
    if (deleted) *deleted = 0;
    char *p = info_path(name);
    GKeyFile *kf = g_key_file_new();
    gboolean ok = g_key_file_load_from_file(kf, p, G_KEY_FILE_NONE, NULL);
    if (ok) {
        char *path = g_key_file_get_string(kf, "Trash Info", "Path", NULL);
        char *date = g_key_file_get_string(kf, "Trash Info", "DeletionDate", NULL);
        if (path && orig_path) {
            char *u = g_uri_unescape_string(path, NULL);
            if (u && u[0] != '/') {                      /* relative: to the top of the disk of this trash */
                char *abs = g_build_filename("/", u, NULL);
                g_free(u);
                u = abs;
            }
            *orig_path = u;
        }
        if (date && deleted) {
            GTimeZone *tz = g_time_zone_new_local();
            GDateTime *dt = g_date_time_new_from_iso8601(date, tz);
            if (dt) {
                *deleted = g_date_time_to_unix(dt);
                g_date_time_unref(dt);
            }
            g_time_zone_unref(tz);
        }
        ok = path != NULL;
        g_free(path);
        g_free(date);
    }
    g_key_file_free(kf);
    g_free(p);
    return ok;
}

gboolean files_delete_recursive(GFile *f, GCancellable *c, GError **error)
{
    if (g_cancellable_set_error_if_cancelled(c, error)) return FALSE;
    GFileType t = g_file_query_file_type(f, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, c);
    if (t == G_FILE_TYPE_DIRECTORY) {
        GFileEnumerator *en = g_file_enumerate_children(f, G_FILE_ATTRIBUTE_STANDARD_NAME,
                                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, c, error);
        if (!en) return FALSE;
        GFileInfo *info;
        GError *e = NULL;
        while ((info = g_file_enumerator_next_file(en, c, &e))) {
            GFile *child = g_file_get_child(f, g_file_info_get_name(info));
            gboolean ok = files_delete_recursive(child, c, &e);
            g_object_unref(child);
            g_object_unref(info);
            if (!ok) break;
        }
        g_file_enumerator_close(en, NULL, NULL);
        g_object_unref(en);
        if (e) {
            g_propagate_error(error, e);
            return FALSE;
        }
    }
    return g_file_delete(f, c, error);
}

gboolean trash_restore_to(GFile *trashed, GFile *dest, GCancellable *c, GError **error)
{
    GFile *parent = g_file_get_parent(dest);
    GError *e = NULL;
    if (parent && !g_file_make_directory_with_parents(parent, c, &e)) {
        if (!g_error_matches(e, G_IO_ERROR, G_IO_ERROR_EXISTS)) {
            g_propagate_error(error, e);
            g_object_unref(parent);
            return FALSE;
        }
        g_clear_error(&e);
    }
    if (parent) g_object_unref(parent);
    if (!g_file_move(trashed, dest, G_FILE_COPY_NOFOLLOW_SYMLINKS | G_FILE_COPY_ALL_METADATA, c, NULL, NULL, error))
        return FALSE;
    char *name = g_file_get_basename(trashed);
    char *ip = info_path(name);
    g_unlink(ip);
    g_free(ip);
    g_free(name);
    return TRUE;
}

gboolean trash_delete(GFile *trashed, GCancellable *c, GError **error)
{
    if (!files_delete_recursive(trashed, c, error)) return FALSE;
    char *name = g_file_get_basename(trashed);
    char *ip = info_path(name);
    g_unlink(ip);
    g_free(ip);
    g_free(name);
    return TRUE;
}

static gboolean empty_dir(const char *path, GCancellable *c, GError **error)
{
    GDir *d = g_dir_open(path, 0, NULL);
    if (!d) return TRUE;
    const char *n;
    gboolean ok = TRUE;
    while (ok && (n = g_dir_read_name(d))) {
        char *p = g_build_filename(path, n, NULL);
        GFile *f = g_file_new_for_path(p);
        ok = files_delete_recursive(f, c, error);
        g_object_unref(f);
        g_free(p);
    }
    g_dir_close(d);
    return ok;
}

gboolean trash_empty(GCancellable *c, GError **error)
{
    char *files = trash_files_dir(), *info = trash_info_dir(), *dir = trash_dir();
    gboolean ok = empty_dir(files, c, error) && empty_dir(info, c, error);
    char *sizes = g_build_filename(dir, "directorysizes", NULL);
    g_unlink(sizes);
    g_free(sizes);
    g_free(files);
    g_free(info);
    g_free(dir);
    return ok;
}

char *trash_find_newest(const char *orig_path)
{
    char *dir = trash_info_dir();
    GDir *d = g_dir_open(dir, 0, NULL);
    char *best = NULL;
    gint64 best_t = -1;
    const char *n;
    while (d && (n = g_dir_read_name(d))) {
        if (!g_str_has_suffix(n, ".trashinfo")) continue;
        char *name = g_strndup(n, strlen(n) - strlen(".trashinfo"));
        char *orig = NULL;
        gint64 t = 0;
        if (trash_read_info(name, &orig, &t) && orig && !strcmp(orig, orig_path) && t >= best_t) {
            g_free(best);
            best = g_strdup(name);
            best_t = t;
        }
        g_free(orig);
        g_free(name);
    }
    if (d) g_dir_close(d);
    g_free(dir);
    return best;
}

int trash_count(void)
{
    static gint64 when;                           /* (asked at every change of the selection: read once a second) */
    static int count;
    gint64 now = g_get_monotonic_time();
    if (when && now - when < G_USEC_PER_SEC / 2) return count;
    char *dir = trash_files_dir();
    GDir *d = g_dir_open(dir, 0, NULL);
    int n = 0;
    while (d && g_dir_read_name(d) && n < 100000) n++;
    if (d) g_dir_close(d);
    g_free(dir);
    when = now;
    count = n;
    return n;
}
