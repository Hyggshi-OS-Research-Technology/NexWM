/* hde-run.c — see hde-run.h */
#include "hde-run.h"
#include <gio/gio.h>
#include <string.h>

#define DEFAULT_TIMEOUT 4

typedef struct {
    HdeRunCb cb;
    gpointer data;
    GSubprocess *proc;
    guint timer;
} Run;

gboolean hde_have(const char *program)
{
    if (!program || !*program) return FALSE;
    char *p = g_find_program_in_path(program);
    gboolean ok = p != NULL;
    g_free(p);
    return ok;
}

static char *valid_text(GBytes *b)
{
    if (!b) return g_strdup("");
    gsize n = 0;
    const char *d = g_bytes_get_data(b, &n);
    if (!d || !n) return g_strdup("");
    return g_utf8_make_valid(d, (gssize)n);
}

static gboolean run_timeout(gpointer p)
{
    Run *r = p;
    r->timer = 0;
    g_subprocess_force_exit(r->proc);
    return G_SOURCE_REMOVE;
}

static void run_done(GObject *src, GAsyncResult *res, gpointer data)
{
    Run *r = data;
    GBytes *out = NULL, *err = NULL;
    GSubprocess *p = G_SUBPROCESS(src);
    gboolean done = g_subprocess_communicate_finish(p, res, &out, &err, NULL);
    gboolean ok = done && g_subprocess_get_if_exited(p) && g_subprocess_get_exit_status(p) == 0;
    if (r->timer) g_source_remove(r->timer);
    char *o = valid_text(out), *e = valid_text(err);
    if (r->cb) r->cb(ok, o, e, r->data);
    g_free(o);
    g_free(e);
    if (out) g_bytes_unref(out);
    if (err) g_bytes_unref(err);
    g_object_unref(r->proc);
    g_free(r);
}

typedef struct { HdeRunCb cb; gpointer data; } Missing;

static gboolean report_missing(gpointer p)
{
    Missing *m = p;
    if (m->cb) m->cb(FALSE, NULL, "", m->data);
    g_free(m);
    return G_SOURCE_REMOVE;
}

/* untranslated messages, but keep the character set of the user's locale */
static void plain_messages(GSubprocessLauncher *l)
{
    const char *all = g_getenv("LC_ALL");
    if (all && *all) {
        g_subprocess_launcher_unsetenv(l, "LC_ALL");
        g_subprocess_launcher_setenv(l, "LC_CTYPE", all, TRUE);
    }
    g_subprocess_launcher_setenv(l, "LC_MESSAGES", "C", TRUE);
    g_subprocess_launcher_unsetenv(l, "LANGUAGE");
}

void hde_run(const char *const *argv, const char *input, int timeout_s, HdeRunCb cb, gpointer data)
{
    GSubprocess *p = NULL;
    if (argv && argv[0] && (argv[0][0] == '/' ? g_file_test(argv[0], G_FILE_TEST_IS_EXECUTABLE) : hde_have(argv[0]))) {
        GSubprocessFlags fl = G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE;
        if (input) fl |= G_SUBPROCESS_FLAGS_STDIN_PIPE;
        GSubprocessLauncher *l = g_subprocess_launcher_new(fl);
        plain_messages(l);
        p = g_subprocess_launcher_spawnv(l, argv, NULL);
        g_object_unref(l);
    }
    if (!p) {
        Missing *m = g_new0(Missing, 1);
        m->cb = cb;
        m->data = data;
        g_idle_add(report_missing, m);
        return;
    }
    Run *r = g_new0(Run, 1);
    r->cb = cb;
    r->data = data;
    r->proc = p;
    r->timer = g_timeout_add_seconds(timeout_s > 0 ? (guint)timeout_s : DEFAULT_TIMEOUT, run_timeout, r);
    GBytes *in = input ? g_bytes_new(input, strlen(input)) : NULL;
    g_subprocess_communicate_async(p, in, NULL, run_done, r);
    if (in) g_bytes_unref(in);
}

void hde_run_sh(const char *script, int timeout_s, HdeRunCb cb, gpointer data)
{
    const char *argv[] = { "/bin/sh", "-c", script, NULL };
    hde_run(argv, NULL, timeout_s, cb, data);
}

void hde_spawn(const char *script)
{
    gchar *argv[] = { (gchar *)"/bin/sh", (gchar *)"-c", (gchar *)script, NULL };
    GError *e = NULL;
    if (!g_spawn_async(NULL, argv, NULL, G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, NULL, &e))
        g_clear_error(&e);
}

gboolean hde_launch_first(const char *const *cmds)
{
    for (int i = 0; cmds && cmds[i]; i++) {
        char **w = g_strsplit(cmds[i], " ", 2);
        gboolean ok = w[0] && hde_have(w[0]);
        g_strfreev(w);
        if (!ok) continue;
        GError *e = NULL;
        if (g_spawn_command_line_async(cmds[i], &e)) return TRUE;
        g_clear_error(&e);
    }
    return FALSE;
}

char *hde_program_path(const char *name)
{
    char *self = g_file_read_link("/proc/self/exe", NULL);
    if (self) {
        char *dir = g_path_get_dirname(self);
        char *p = g_build_filename(dir, name, NULL);
        g_free(dir);
        g_free(self);
        if (g_file_test(p, G_FILE_TEST_IS_EXECUTABLE)) return p;
        g_free(p);
    }
    return g_find_program_in_path(name);
}

void hde_open_settings(const char *page)
{
    char *path = hde_program_path("hde-settings");
    if (!path) return;
    gchar *argv[] = { path, (gchar *)page, NULL };
    GError *e = NULL;
    if (!g_spawn_async(NULL, argv, NULL, G_SPAWN_DEFAULT, NULL, NULL, NULL, &e)) g_clear_error(&e);
    g_free(path);
}

void hde_run_settings(const char *const *args, int timeout_s, HdeRunCb cb, gpointer data)
{
    char *path = hde_program_path("hde-settings");
    GPtrArray *a = g_ptr_array_new();
    g_ptr_array_add(a, path ? path : (char *)"hde-settings");
    for (int i = 0; args && args[i]; i++) g_ptr_array_add(a, (gpointer)args[i]);
    g_ptr_array_add(a, NULL);
    hde_run((const char *const *)a->pdata, NULL, timeout_s, cb, data);
    g_ptr_array_free(a, TRUE);
    g_free(path);
}

GDBusConnection *hde_system_bus(void)
{
    static GDBusConnection *bus;
    static gboolean tried;
    if (!tried) {
        tried = TRUE;
        GError *e = NULL;
        bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &e);
        if (!bus) {
            if (g_getenv("HDE_DEBUG")) g_printerr("hde-panel: no system bus: %s\n", e ? e->message : "?");
            g_clear_error(&e);
        }
    }
    return bus;
}
