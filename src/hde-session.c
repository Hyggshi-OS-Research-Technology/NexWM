/* hde-session: quản lý phiên — khởi chạy WM, desktop, panel, autostart; tự restart khi crash */
#include <gio/gio.h>
#include <gio/gdesktopappinfo.h>
#include <glib-unix.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    const char *name;
    const char *const *candidates;   /* NULL-terminated, thử lần lượt */
    GSubprocess *proc;
    int fails;
    gint64 last_start;
} Comp;

static const char *wm_list[]      = { "xfwm4", "openbox", "marco", "metacity", "icewm", "fluxbox", "nexwm", NULL };
static const char *desktop_list[] = { "hde-desktop", NULL };
static const char *panel_list[]   = { "hde-panel", NULL };

static Comp comps[] = {
    { "window-manager", wm_list,      NULL, 0, 0 },
    { "desktop",        desktop_list, NULL, 0, 0 },
    { "panel",          panel_list,   NULL, 0, 0 },
};
#define N_COMPS (G_N_ELEMENTS(comps))

static GMainLoop *loop;
static gboolean quitting = FALSE;

static char *find_bin(const char *name)
{
    char *self = g_file_read_link("/proc/self/exe", NULL);
    if (self) {
        char *dir = g_path_get_dirname(self);
        char *p = g_build_filename(dir, name, NULL);
        g_free(self); g_free(dir);
        if (g_file_test(p, G_FILE_TEST_IS_EXECUTABLE)) return p;
        g_free(p);
    }
    return g_find_program_in_path(name);
}

static void start_comp(Comp *c);

static gboolean restart_cb(gpointer data)
{
    if (!quitting) start_comp(data);
    return G_SOURCE_REMOVE;
}

static void on_exit_cb(GObject *src, GAsyncResult *res, gpointer data)
{
    Comp *c = data;
    g_subprocess_wait_finish(G_SUBPROCESS(src), res, NULL);
    if (quitting) return;

    g_clear_object(&c->proc);
    if (g_get_monotonic_time() - c->last_start < 3 * G_USEC_PER_SEC) c->fails++;
    else c->fails = 0;

    if (c->fails >= 5) {
        g_printerr("hde-session: %s failed repeatedly; stopping restart attempts\n", c->name);
        return;
    }
    g_printerr("hde-session: %s exited; restarting...\n", c->name);
    g_timeout_add_seconds(1, restart_cb, c);
}

static void start_comp(Comp *c)
{
    for (const char *const *n = c->candidates; *n; n++) {
        char *path = find_bin(*n);
        if (!path) continue;
        GError *err = NULL;
        c->last_start = g_get_monotonic_time();
        c->proc = g_subprocess_new(G_SUBPROCESS_FLAGS_INHERIT_FDS, &err, path, NULL);
        g_free(path);
        if (!c->proc) {
            g_printerr("hde-session: failed to start %s: %s\n", *n, err->message);
            g_clear_error(&err);
            continue;
        }
        g_printerr("hde-session: started %s (%s)\n", c->name, *n);
        g_subprocess_wait_async(c->proc, NULL, on_exit_cb, c);
        return;
    }
    g_printerr("hde-session: WARNING: %s not found\n", c->name);
}

static gboolean run_autostart(gpointer data)
{
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    char *dirs[2] = { g_build_filename(g_get_user_config_dir(), "autostart", NULL),
                      g_strdup("/etc/xdg/autostart") };

    for (int i = 0; i < 2; i++) {
        GDir *d = g_dir_open(dirs[i], 0, NULL);
        if (!d) continue;
        const char *fn;
        while ((fn = g_dir_read_name(d))) {
            if (!g_str_has_suffix(fn, ".desktop") || g_hash_table_contains(seen, fn)) continue;
            g_hash_table_add(seen, g_strdup(fn));
            char *path = g_build_filename(dirs[i], fn, NULL);
            GDesktopAppInfo *info = g_desktop_app_info_new_from_filename(path);
            if (info && !g_desktop_app_info_get_is_hidden(info) &&
                g_desktop_app_info_get_show_in(info, "HDE")) {
                g_app_info_launch(G_APP_INFO(info), NULL, NULL, NULL);
            }
            g_clear_object(&info);
            g_free(path);
        }
        g_dir_close(d);
    }
    g_free(dirs[0]); g_free(dirs[1]);
    g_hash_table_destroy(seen);
    return G_SOURCE_REMOVE;
}

static gboolean finish_cb(gpointer data)
{
    for (guint i = 0; i < N_COMPS; i++)
        if (comps[i].proc) g_subprocess_force_exit(comps[i].proc);
    g_main_loop_quit(loop);
    return G_SOURCE_REMOVE;
}

static gboolean on_signal(gpointer data)
{
    if (quitting) return G_SOURCE_CONTINUE;
    quitting = TRUE;
    g_printerr("hde-session: logging out...\n");
    for (guint i = N_COMPS; i-- > 0;)
        if (comps[i].proc) g_subprocess_send_signal(comps[i].proc, SIGTERM);
    g_timeout_add(800, finish_cb, NULL);
    return G_SOURCE_CONTINUE;
}

int main(void)
{
    char pid[32];
    snprintf(pid, sizeof pid, "%d", (int)getpid());
    g_setenv("HDE_SESSION_PID", pid, TRUE);
    g_setenv("XDG_CURRENT_DESKTOP", "HDE", TRUE);
    g_setenv("XDG_SESSION_DESKTOP", "HDE", TRUE);
    g_setenv("DESKTOP_SESSION", "hde", TRUE);

    loop = g_main_loop_new(NULL, FALSE);
    g_unix_signal_add(SIGTERM, on_signal, NULL);
    g_unix_signal_add(SIGINT,  on_signal, NULL);
    g_unix_signal_add(SIGHUP,  on_signal, NULL);

    for (guint i = 0; i < N_COMPS; i++) start_comp(&comps[i]);
    g_timeout_add_seconds(2, run_autostart, NULL);

    g_main_loop_run(loop);
    return 0;
}
