/* thumbs.c — Hyggshi Files: thumbnails (freedesktop.org thumbnail specification).
 *
 * Looked up first in ~/.cache/thumbnails/{normal,large}/MD5(uri).png (valid while Thumb::MTime is the file's
 * modification time), else made: pictures with gdk-pixbuf, other types with the thumbnailers installed
 * (/usr/share/thumbnailers/NAME.thumbnailer: totem / ffmpegthumbnailer for videos, evince for PDF, ...), then saved
 * there for every program. A file that cannot be thumbnailed is remembered in ~/.cache/thumbnails/fail/hde-files.
 * Two worker threads; results come back to the main thread.
 */
#include "files.h"
#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    GFile *file;
    char *uri, *path, *content_type;
    gint64 mtime;
    int size, scale;
    GCancellable *cancel;
    ThumbDone done;
    gpointer data;
    GdkPixbuf *result;
} ThumbReq;

static GThreadPool *pool;
static GHashTable *thumbnailers;     /* mime type -> Exec */
static char *timeout_prog;

static void load_thumbnailers(void)
{
    thumbnailers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    GPtrArray *dirs = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(dirs, g_build_filename(g_get_user_data_dir(), "thumbnailers", NULL));
    for (const char *const *d = g_get_system_data_dirs(); *d; d++) g_ptr_array_add(dirs, g_build_filename(*d, "thumbnailers", NULL));
    for (guint i = 0; i < dirs->len; i++) {
        GDir *dir = g_dir_open(dirs->pdata[i], 0, NULL);
        const char *n;
        while (dir && (n = g_dir_read_name(dir))) {
            if (!g_str_has_suffix(n, ".thumbnailer")) continue;
            char *p = g_build_filename(dirs->pdata[i], n, NULL);
            GKeyFile *kf = g_key_file_new();
            if (g_key_file_load_from_file(kf, p, G_KEY_FILE_NONE, NULL)) {
                char *exec = g_key_file_get_string(kf, "Thumbnailer Entry", "Exec", NULL);
                char *try_exec = g_key_file_get_string(kf, "Thumbnailer Entry", "TryExec", NULL);
                char **mimes = g_key_file_get_string_list(kf, "Thumbnailer Entry", "MimeType", NULL, NULL);
                char *found = NULL;
                if (exec) {
                    char **argv = NULL;
                    if (g_shell_parse_argv(try_exec ? try_exec : exec, NULL, &argv, NULL) && argv[0])
                        found = g_find_program_in_path(argv[0]);
                    g_strfreev(argv);
                }
                for (int k = 0; found && mimes && mimes[k]; k++)
                    if (*mimes[k] && !g_hash_table_contains(thumbnailers, mimes[k]))
                        g_hash_table_insert(thumbnailers, g_strdup(mimes[k]), g_strdup(exec));
                g_free(found);
                g_strfreev(mimes);
                g_free(try_exec);
                g_free(exec);
            }
            g_key_file_free(kf);
            g_free(p);
        }
        if (dir) g_dir_close(dir);
    }
    g_ptr_array_unref(dirs);
    timeout_prog = g_find_program_in_path("timeout");
}

static char *cache_path(const char *uri, const char *kind)
{
    char *md5 = g_compute_checksum_for_string(G_CHECKSUM_MD5, uri, -1);
    char *file = g_strconcat(md5, ".png", NULL);
    char *p = g_build_filename(g_get_user_cache_dir(), "thumbnails", kind, file, NULL);
    g_free(file);
    g_free(md5);
    return p;
}

static GdkPixbuf *fit(GdkPixbuf *pb, int px)
{
    int w = gdk_pixbuf_get_width(pb), h = gdk_pixbuf_get_height(pb);
    if (w <= px && h <= px) return g_object_ref(pb);
    double k = MIN((double)px / w, (double)px / h);
    return gdk_pixbuf_scale_simple(pb, MAX(1, (int)(w * k + 0.5)), MAX(1, (int)(h * k + 0.5)), GDK_INTERP_BILINEAR);
}

static gboolean up_to_date(GdkPixbuf *pb, gint64 mtime)
{
    const char *m = gdk_pixbuf_get_option(pb, "tEXt::Thumb::MTime");
    return m && g_ascii_strtoll(m, NULL, 10) == mtime;
}

static void save_png(GdkPixbuf *pb, const char *path, const char *uri, gint64 mtime)
{
    char *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0700);
    char *tmp = g_strdup_printf("%s.hde-files-%d.png", path, (int)getpid());
    char *mt = g_strdup_printf("%" G_GINT64_FORMAT, mtime);
    if (gdk_pixbuf_save(pb, tmp, "png", NULL, "tEXt::Thumb::URI", uri, "tEXt::Thumb::MTime", mt,
                        "tEXt::Software", "Hyggshi Files", NULL)) {
        g_chmod(tmp, 0600);
        if (g_rename(tmp, path) != 0) g_unlink(tmp);
    } else g_unlink(tmp);
    g_free(mt);
    g_free(tmp);
    g_free(dir);
}

/* runs the thumbnailer of this type: Exec with %u (URI) %i (path) %o (output PNG) %s (size) */
static GdkPixbuf *run_thumbnailer(ThumbReq *r, const char *exec, int px)
{
    char **argv = NULL;
    if (!g_shell_parse_argv(exec, NULL, &argv, NULL)) return NULL;
    char *out = g_build_filename(g_get_tmp_dir(), "hde-files-thumb-XXXXXX.png", NULL);
    int fd = g_mkstemp(out);
    if (fd < 0) {
        g_strfreev(argv);
        g_free(out);
        return NULL;
    }
    close(fd);
    GPtrArray *a = g_ptr_array_new_with_free_func(g_free);
    if (timeout_prog) {
        g_ptr_array_add(a, g_strdup(timeout_prog));
        g_ptr_array_add(a, g_strdup("20"));
    }
    char *sz = g_strdup_printf("%d", px);
    for (int i = 0; argv[i]; i++) {
        GString *s = g_string_new(NULL);
        for (const char *c = argv[i]; *c; c++) {
            if (c[0] == '%' && c[1]) {
                c++;
                if (*c == 'u') g_string_append(s, r->uri);
                else if (*c == 'i') g_string_append(s, r->path);
                else if (*c == 'o') g_string_append(s, out);
                else if (*c == 's') g_string_append(s, sz);
                else if (*c == '%') g_string_append_c(s, '%');
            } else g_string_append_c(s, *c);
        }
        g_ptr_array_add(a, g_string_free(s, FALSE));
    }
    g_ptr_array_add(a, NULL);
    int status = -1;
    GdkPixbuf *pb = NULL;
    if (g_spawn_sync(NULL, (char **)a->pdata, NULL,
                     G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                     NULL, NULL, NULL, NULL, &status, NULL) && g_spawn_check_wait_status(status, NULL))
        pb = gdk_pixbuf_new_from_file(out, NULL);
    g_unlink(out);
    g_free(sz);
    g_ptr_array_unref(a);
    g_strfreev(argv);
    g_free(out);
    return pb;
}

static void worker(gpointer data, gpointer user)
{
    (void)user;
    ThumbReq *r = data;
    if (g_cancellable_is_cancelled(r->cancel)) goto out;
    int px = r->size * r->scale;
    gboolean large = px > 128;
    const char *kinds[2] = { large ? "large" : "normal", large ? "normal" : "large" };
    for (int k = 0; k < 2 && !r->result; k++) {
        char *p = cache_path(r->uri, kinds[k]);
        GdkPixbuf *pb = g_file_test(p, G_FILE_TEST_EXISTS) ? gdk_pixbuf_new_from_file(p, NULL) : NULL;
        if (pb && up_to_date(pb, r->mtime) && (k == 0 || gdk_pixbuf_get_width(pb) >= px || gdk_pixbuf_get_height(pb) >= px))
            r->result = fit(pb, px);
        if (pb) g_object_unref(pb);
        g_free(p);
    }
    if (r->result || !r->path) goto out;
    char *cache_root = g_build_filename(g_get_user_cache_dir(), "thumbnails", NULL);
    gboolean in_cache = g_str_has_prefix(r->path, cache_root);
    g_free(cache_root);
    char *fail = cache_path(r->uri, "fail/hde-files");
    GdkPixbuf *failed = g_file_test(fail, G_FILE_TEST_EXISTS) ? gdk_pixbuf_new_from_file(fail, NULL) : NULL;
    gboolean known_bad = failed && up_to_date(failed, r->mtime);
    if (failed) g_object_unref(failed);
    if (known_bad || g_cancellable_is_cancelled(r->cancel)) {
        g_free(fail);
        goto out;
    }
    int tsize = large ? 256 : 128;
    GdkPixbuf *made = NULL;
    if (files_can_thumbnail(r->content_type)) {
        GdkPixbuf *pb = gdk_pixbuf_new_from_file_at_scale(r->path, tsize, tsize, TRUE, NULL);
        if (pb) {
            made = gdk_pixbuf_apply_embedded_orientation(pb);
            g_object_unref(pb);
        }
    }
    if (!made) {
        char *mime = g_content_type_get_mime_type(r->content_type);
        const char *exec = mime ? g_hash_table_lookup(thumbnailers, mime) : NULL;
        if (exec) made = run_thumbnailer(r, exec, tsize);
        g_free(mime);
    }
    if (made) {
        if (!in_cache) {
            char *dest = cache_path(r->uri, large ? "large" : "normal");
            save_png(made, dest, r->uri, r->mtime);
            g_free(dest);
        }
        r->result = fit(made, px);
        g_object_unref(made);
    } else if (!in_cache) {
        GdkPixbuf *dot = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, 1, 1);
        gdk_pixbuf_fill(dot, 0);
        save_png(dot, fail, r->uri, r->mtime);
        g_object_unref(dot);
    }
    g_free(fail);
out:
    return;
}

static gboolean deliver(gpointer d)
{
    ThumbReq *r = d;
    if (!g_cancellable_is_cancelled(r->cancel)) {
        cairo_surface_t *s = r->result ? gdk_cairo_surface_create_from_pixbuf(r->result, r->scale, NULL) : NULL;
        r->done(r->file, s, r->data);
        if (s) cairo_surface_destroy(s);
    }
    if (r->result) g_object_unref(r->result);
    g_object_unref(r->file);
    g_object_unref(r->cancel);
    g_free(r->uri);
    g_free(r->path);
    g_free(r->content_type);
    g_free(r);
    return G_SOURCE_REMOVE;
}

static void worker_then_deliver(gpointer data, gpointer user)
{
    worker(data, user);
    g_idle_add(deliver, data);
}

void thumbs_init(void)
{
    if (pool) return;
    files_can_thumbnail("image/png");          /* (fills its table here, in the main thread) */
    load_thumbnailers();
    pool = g_thread_pool_new(worker_then_deliver, NULL, 2, FALSE, NULL);
}

gboolean thumbs_possible(const char *content_type, goffset size)
{
    if (!content_type || size <= 0 || size > 200 * 1024 * 1024) return FALSE;
    if (files_can_thumbnail(content_type)) return size <= 100 * 1024 * 1024;
    char *mime = g_content_type_get_mime_type(content_type);
    gboolean r = mime && thumbnailers && g_hash_table_contains(thumbnailers, mime);
    g_free(mime);
    return r;
}

void thumbs_request(GFile *file, const char *content_type, gint64 mtime, int size, int scale,
                    GCancellable *cancel, ThumbDone done, gpointer data)
{
    thumbs_init();
    ThumbReq *r = g_new0(ThumbReq, 1);
    r->file = g_object_ref(file);
    r->uri = g_file_get_uri(file);
    r->path = g_file_get_path(file);
    r->content_type = g_strdup(content_type);
    r->mtime = mtime;
    r->size = size;
    r->scale = MAX(1, scale);
    r->cancel = cancel ? g_object_ref(cancel) : g_cancellable_new();
    r->done = done;
    r->data = data;
    g_thread_pool_push(pool, r, NULL);
}
