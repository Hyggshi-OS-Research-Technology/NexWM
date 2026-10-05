/* hde-session — trình quản lý phiên HDE.
 *
 * Khởi động theo thứ tự: window manager -> hde-xsettings -> áp thiết lập (hde-settings --apply)
 * -> hde-hotkeys -> hde-desktop -> hde-panel -> (sau 2 giây) polkit agent + XDG autostart.
 *
 *  - WM: theo `wm=` trong ~/.config/hde/settings.ini (hoặc HDE_WM); "auto" ưu tiên WM dựa trên GTK
 *    (Metacity, Marco, Mutter, Muffin) rồi mới tới Xfwm4/Openbox/... (bảng trong src/hde-wm.h).
 *    WM không chạy được (thoát ngay) -> thử WM kế tiếp; WM crash -> chạy lại.
 *  - Đổi WM ngay, không cần đăng xuất: `hde-session wm` (hoặc SIGUSR2) — Settings > Window Management.
 *  - Thành phần crash (panel, desktop, hotkeys, xsettings, polkit agent) được chạy lại tự động
 *    (giới hạn số lần để không lặp vô hạn); thoát bình thường (exit 0 / SIGTERM) thì không.
 *  - `hde-session restart` (SIGUSR1): chạy lại desktop + panel mà không đăng xuất.
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "hde/session.h"
#include "hde/core.h"
#include "hde/settings.h"
#include "hde-wm.h"
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop = 0;
static volatile sig_atomic_t g_restart = 0;     /* SIGUSR1: chạy lại desktop + panel */
static volatile sig_atomic_t g_switch_wm = 0;   /* SIGUSR2: đổi WM theo settings.ini */
static char g_bindir[4096];

static void on_signal(int sig) { (void)sig; g_stop = 1; }
static void on_usr1(int sig) { (void)sig; g_restart = 1; }
static void on_usr2(int sig) { (void)sig; g_switch_wm = 1; }

static int executable(const char *p) { return p && *p && access(p, X_OK) == 0; }

static int self_dir(char *out, size_t n, const char *argv0)
{
    char buf[4096];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    const char *src = argv0;
    if (len > 0) { buf[len] = '\0'; src = buf; }
    const char *slash = strrchr(src, '/');
    if (!slash) { snprintf(out, n, "."); return 0; }
    size_t l = (size_t)(slash - src);
    if (l >= n) return -1;
    memcpy(out, src, l);
    out[l] = '\0';
    return 0;
}

/* Tìm chương trình: cạnh hde-session (bản vừa build) -> PATH -> /usr/local/bin, /usr/bin. Kết quả malloc'd. */
static char *resolve_component(const char *name, const char *bindir)
{
    char path[4096];
    if (!name || !*name) return NULL;
    if (name[0] == '/') return executable(name) ? strdup(name) : NULL;
    if (bindir && *bindir) {
        snprintf(path, sizeof(path), "%s/%s", bindir, name);
        if (executable(path)) return strdup(path);
    }
    const char *p = getenv("PATH");
    if (p) {
        char *copy = strdup(p);
        if (!copy) return NULL;
        char *save = NULL;
        for (char *d = strtok_r(copy, ":", &save); d; d = strtok_r(NULL, ":", &save)) {
            snprintf(path, sizeof(path), "%s/%s", *d ? d : ".", name);
            if (executable(path)) { free(copy); return strdup(path); }
        }
        free(copy);
    }
    const char *fallbacks[] = { "/usr/local/bin", "/usr/bin", NULL };
    for (int i = 0; fallbacks[i]; ++i) {
        snprintf(path, sizeof(path), "%s/%s", fallbacks[i], name);
        if (executable(path)) return strdup(path);
    }
    return NULL;
}

static void child_reset_signals(void)
{
    signal(SIGCHLD, SIG_DFL);
    signal(SIGUSR1, SIG_DFL);
    signal(SIGUSR2, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    signal(SIGINT, SIG_DFL);
    signal(SIGHUP, SIG_DFL);
    signal(SIGPIPE, SIG_DFL);
    sigset_t s;
    sigemptyset(&s);
    sigprocmask(SIG_SETMASK, &s, NULL);
}

static pid_t spawn_argv(const char *path, const char *const *args)
{
    pid_t p = fork();
    if (p < 0) { perror("hde-session: fork"); return -1; }
    if (p == 0) {
        child_reset_signals();
        const char *argv[32];
        int n = 0;
        argv[n++] = path;
        for (int i = 0; args && args[i] && n < 31; i++) argv[n++] = args[i];
        argv[n] = NULL;
        execv(path, (char *const *)argv);
        perror(path);
        _exit(127);
    }
    return p;
}

static pid_t spawn_shell(const char *cmd)
{
    pid_t p = fork();
    if (p < 0) return -1;
    if (p == 0) {
        child_reset_signals();
        setsid();
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    return p;
}

/* Chạy và chờ tối đa timeout_ms (dùng cho các lệnh ngắn lúc khởi động). */
static void run_wait(const char *const *argv, int timeout_ms)
{
    char *path = resolve_component(argv[0], NULL);
    if (!path) return;
    pid_t p = spawn_argv(path, argv + 1);
    free(path);
    if (p <= 0) return;
    for (int waited = 0; waited < timeout_ms; waited += 50) {
        if (waitpid(p, NULL, WNOHANG) == p) return;
        usleep(50 * 1000);
    }
    kill(p, SIGKILL);
    waitpid(p, NULL, 0);
}

static void stop_pid(pid_t *p)
{
    if (*p <= 0) return;
    kill(*p, SIGTERM);
    for (int i = 0; i < 20; ++i) {                /* chờ tối đa ~2s rồi SIGKILL */
        if (waitpid(*p, NULL, WNOHANG) != 0) { *p = -1; return; }
        usleep(100 * 1000);
    }
    kill(*p, SIGKILL);
    waitpid(*p, NULL, 0);
    *p = -1;
}

/* ===================== thành phần được giám sát ===================== */
typedef struct {
    const char *name;           /* tên chương trình (NULL = đường dẫn đặt lúc chạy, vd. polkit agent) */
    const char *label;
    char path[4096];
    pid_t pid;
    int enabled;
    int max_fails;
    int fails;
    time_t fail_window;
    time_t started;
    time_t restart_at;
} Component;

enum { C_XSETTINGS, C_HOTKEYS, C_DESKTOP, C_PANEL, C_POLKIT, N_COMP };
static Component comps[N_COMP] = {
    [C_XSETTINGS] = { "hde-xsettings", "XSETTINGS daemon (live theme / Dark mode)", "", -1, 0, 5, 0, 0, 0, 0 },
    [C_HOTKEYS]   = { "hde-hotkeys", "system hotkeys (Super, F1-F3, media keys)", "", -1, 0, 5, 0, 0, 0, 0 },
    [C_DESKTOP]   = { "hde-desktop", "desktop", "", -1, 0, 5, 0, 0, 0, 0 },
    [C_PANEL]     = { "hde-panel", "panel", "", -1, 0, 5, 0, 0, 0, 0 },
    [C_POLKIT]    = { NULL, "polkit authentication agent", "", -1, 0, 1, 0, 0, 0, 0 },
};

static void comp_start(Component *c)
{
    if (!c->enabled || c->pid > 0) return;
    if (c->name) {                                  /* resolve lại mỗi lần -> dùng đúng bản vừa build/cài */
        char *p = resolve_component(c->name, g_bindir);
        if (!p) {
            fprintf(stderr, "hde-session: %s not found; %s unavailable\n", c->name, c->label);
            c->enabled = 0;
            return;
        }
        snprintf(c->path, sizeof c->path, "%s", p);
        free(p);
    }
    printf("hde-session: starting %s: %s\n", c->label, c->path);
    fflush(stdout);
    c->pid = spawn_argv(c->path, NULL);
    c->started = time(NULL);
}

static void comp_exited(Component *c, int status)
{
    c->pid = -1;
    if (WIFSIGNALED(status)) fprintf(stderr, "hde-session: %s killed by signal %d\n", c->label, WTERMSIG(status));
    else fprintf(stderr, "hde-session: %s exited (status %d)\n", c->label, WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    if (g_stop || !c->enabled) return;
    int abnormal = WIFSIGNALED(status)
        ? (WTERMSIG(status) != SIGTERM && WTERMSIG(status) != SIGINT && WTERMSIG(status) != SIGHUP)
        : (WIFEXITED(status) && WEXITSTATUS(status) != 0);
    if (!abnormal) return;                          /* thoát bình thường: không chạy lại */
    time_t now = time(NULL);
    if (now - c->fail_window > 60) { c->fail_window = now; c->fails = 0; }
    if (++c->fails > c->max_fails) {
        fprintf(stderr, "hde-session: %s keeps crashing; not restarting it again (see this log)\n", c->label);
        return;
    }
    c->restart_at = now + (c->fails > 2 ? 3 : 1);
    fprintf(stderr, "hde-session: restarting %s (attempt %d)\n", c->label, c->fails);
}

static void start_components(void)
{
    if (comps[C_DESKTOP].enabled) {
        comp_start(&comps[C_DESKTOP]);
        usleep(400 * 1000);                          /* để desktop map trước; panel còn tự raise lên trên */
    }
    comp_start(&comps[C_PANEL]);
}

static void restart_components(void)
{
    printf("hde-session: restarting desktop + panel (no logout)\n");
    stop_pid(&comps[C_PANEL].pid);
    stop_pid(&comps[C_DESKTOP].pid);
    for (int i = 0; i < N_COMP; i++) { comps[i].fails = 0; comps[i].restart_at = 0; }
    start_components();
}

/* ===================== window manager ===================== */
static const HdeWm *wm_cands[HDE_N_WMS + 1];
static int wm_ncands, wm_idx;
static HdeWm wm_custom;                 /* HDE_WM / wm= là tên chương trình không có trong bảng */
static char wm_custom_name[256];
static pid_t g_wm = -1, g_wm_old = -1;
static const HdeWm *wm_cur;
static time_t wm_started, wm_old_deadline, wm_restart_at, wm_fail_window;
static int wm_fails, wm_old_signal;
static int g_use_wm = 1;

static const char *requested_wm(void)
{
    /* Settings ghi wm= ; HDE_WM (từ hde-start hoặc người dùng) dùng khi settings.ini chưa có */
    const char *s = hde_settings_get("wm", NULL);
    if (s && *s) return s;
    const char *e = getenv("HDE_WM");
    return e && *e ? e : "auto";
}

static void wm_build_candidates(const char *req)
{
    wm_ncands = 0;
    const HdeWm *r = NULL;
    if (req && *req && strcmp(req, "auto") != 0) {
        r = hde_wm_find(req);
        if (!r) {
            snprintf(wm_custom_name, sizeof wm_custom_name, "%s", req);
            wm_custom.id = wm_custom.binary = wm_custom.name = wm_custom_name;
            wm_custom.args = hde_wm_args_none;
            wm_custom.description = "";
            r = &wm_custom;
        }
        wm_cands[wm_ncands++] = r;
    }
    for (unsigned i = 0; i < HDE_N_WMS; i++)
        if (&hde_wms[i] != r) wm_cands[wm_ncands++] = &hde_wms[i];
    wm_idx = 0;
}

static int wm_start_next(void)
{
    while (wm_idx < wm_ncands) {
        const HdeWm *w = wm_cands[wm_idx++];
        char *path = resolve_component(w->binary, g_bindir);
        if (!path) continue;
        printf("hde-session: starting window manager %s: %s\n", w->name, path);
        fflush(stdout);
        g_wm = spawn_argv(path, w->args);
        free(path);
        if (g_wm > 0) {
            wm_cur = w;
            wm_started = time(NULL);
            char buf[64];
            snprintf(buf, sizeof buf, "%s", w->id);
            setenv("HDE_WM_RUNNING", buf, 1);
            return 0;
        }
    }
    wm_cur = NULL;
    fprintf(stderr, "hde-session: no window manager could be started; windows will have no title bars.\n"
                    "             Install one, e.g.: sudo apt install metacity\n");
    return -1;
}

static int wm_index_of(const HdeWm *w)
{
    for (int i = 0; i < wm_ncands; i++) if (wm_cands[i] == w) return i;
    return wm_ncands;
}

static void wm_exited(int status)
{
    g_wm = -1;
    if (g_stop) return;
    time_t now = time(NULL);
    int clean = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    int termsig = WIFSIGNALED(status) && (WTERMSIG(status) == SIGTERM || WTERMSIG(status) == SIGINT);
    fprintf(stderr, "hde-session: window manager %s exited (%s %d)\n", wm_cur ? wm_cur->name : "?",
            WIFSIGNALED(status) ? "signal" : "status", WIFSIGNALED(status) ? WTERMSIG(status) : WEXITSTATUS(status));
    if (clean || termsig) {
        /* thay bởi WM khác (--replace) hoặc người dùng thoát WM: tôn trọng, không giành lại */
        fprintf(stderr, "hde-session: not restarting the window manager (clean exit)\n");
        return;
    }
    if (now - wm_started < 5) {                      /* không chạy được: thử WM kế tiếp */
        fprintf(stderr, "hde-session: %s failed to start; trying the next window manager\n", wm_cur ? wm_cur->name : "?");
        wm_restart_at = now;
        return;
    }
    if (now - wm_fail_window > 60) { wm_fail_window = now; wm_fails = 0; }
    if (++wm_fails <= 3) wm_idx = wm_index_of(wm_cur);    /* crash: chạy lại chính WM đó */
    wm_restart_at = now + 1;
}

static void wm_switch(void)
{
    const char *req = requested_wm();
    printf("hde-session: switching window manager to '%s'\n", req);
    fflush(stdout);
    wm_build_candidates(req);
    const HdeWm *target = NULL;
    for (int i = 0; i < wm_ncands && !target; i++) {
        char *p = resolve_component(wm_cands[i]->binary, g_bindir);
        if (p) { target = wm_cands[i]; free(p); }
    }
    if (!target) { fprintf(stderr, "hde-session: no installed window manager to switch to\n"); return; }
    if (g_wm > 0) {
        if (g_wm_old > 0) stop_pid(&g_wm_old);
        if (!target->can_replace || !wm_cur || !wm_cur->can_replace) {
            pid_t old = g_wm;                         /* WM không hỗ trợ --replace: dừng WM cũ trước */
            g_wm = -1;
            stop_pid(&old);
        } else {
            g_wm_old = g_wm;                          /* WM mới tự thay thế (giao thức WM_S0) */
            g_wm = -1;
            wm_old_deadline = time(NULL) + 4;
            wm_old_signal = 0;
        }
    }
    wm_fails = 0;
    wm_restart_at = 0;
    wm_start_next();
}

static void wm_tick(time_t now)
{
    if (g_wm_old > 0 && now >= wm_old_deadline) {    /* WM cũ không chịu nhường: ép thoát */
        kill(g_wm_old, wm_old_signal ? SIGKILL : SIGTERM);
        wm_old_signal = 1;
        wm_old_deadline = now + 2;
    }
    if (wm_restart_at && now >= wm_restart_at && g_wm <= 0 && !g_stop) {
        wm_restart_at = 0;
        wm_start_next();
    }
}

/* ===================== polkit agent ===================== */
static const char *const polkit_agents[] = {
    "/usr/lib/policykit-1-gnome/polkit-gnome-authentication-agent-1",
    "/usr/libexec/polkit-gnome-authentication-agent-1",
    "/usr/lib/polkit-gnome/polkit-gnome-authentication-agent-1",
    "/usr/lib/x86_64-linux-gnu/polkit-mate/polkit-mate-authentication-agent-1",
    "/usr/lib/aarch64-linux-gnu/polkit-mate/polkit-mate-authentication-agent-1",
    "/usr/libexec/polkit-mate-authentication-agent-1",
    "/usr/lib/mate-polkit/polkit-mate-authentication-agent-1",
    "/usr/bin/lxpolkit",
    "/usr/bin/lxqt-policykit-agent",
    "/usr/libexec/xfce-polkit",
    "/usr/lib/xfce-polkit/xfce-polkit",
    "/usr/lib/x86_64-linux-gnu/libexec/polkit-kde-authentication-agent-1",
    "/usr/libexec/kf6/polkit-kde-authentication-agent-1",
    "/usr/libexec/kf5/polkit-kde-authentication-agent-1",
    "/usr/lib/polkit-kde-authentication-agent-1",
    NULL
};

static void start_polkit_agent(void)
{
    if (getenv("HDE_NO_POLKIT")) return;
    for (int i = 0; polkit_agents[i]; i++) {
        if (!executable(polkit_agents[i])) continue;
        snprintf(comps[C_POLKIT].path, sizeof comps[C_POLKIT].path, "%s", polkit_agents[i]);
        comps[C_POLKIT].enabled = 1;
        comp_start(&comps[C_POLKIT]);
        return;
    }
    fprintf(stderr, "hde-session: no polkit authentication agent found; apps that need administrator rights "
                    "cannot ask for your password. Install one: sudo apt install policykit-1-gnome\n");
}

/* ===================== XDG autostart ===================== */
static int list_has(const char *list, const char *item)
{
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", list);
    char *save = NULL;
    for (char *t = strtok_r(buf, ";", &save); t; t = strtok_r(NULL, ";", &save))
        if (!strcmp(t, item)) return 1;
    return 0;
}

static void strip_field_codes(const char *in, char *out, size_t n)
{
    size_t o = 0;
    for (const char *p = in; *p && o + 1 < n; p++) {
        if (*p == '%' && p[1]) {
            if (p[1] == '%') out[o++] = '%';
            p++;                                       /* bỏ %f %F %u %U %i %c %k ... */
            continue;
        }
        out[o++] = *p;
    }
    out[o] = '\0';
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    size_t l = strlen(s);
    while (l && (s[l - 1] == '\n' || s[l - 1] == '\r' || s[l - 1] == ' ' || s[l - 1] == '\t')) s[--l] = '\0';
    return s;
}

static int is_true(const char *v) { return !strcasecmp(v, "true") || !strcmp(v, "1"); }

static void autostart_entry(const char *file, int have_polkit)
{
    FILE *f = fopen(file, "r");
    if (!f) return;
    char line[4096], exec[4096] = "", tryexec[1024] = "", only[1024] = "", notshow[1024] = "", type[64] = "Application";
    int in_entry = 0, hidden = 0, disabled = 0, terminal = 0;
    while (fgets(line, sizeof line, f)) {
        char *l = trim(line);
        if (*l == '#' || !*l) continue;
        if (*l == '[') { in_entry = !strcmp(l, "[Desktop Entry]"); continue; }
        if (!in_entry) continue;
        char *eq = strchr(l, '=');
        if (!eq) continue;
        *eq = '\0';
        char *k = trim(l), *v = trim(eq + 1);
        if (!strcmp(k, "Exec")) snprintf(exec, sizeof exec, "%s", v);
        else if (!strcmp(k, "TryExec")) snprintf(tryexec, sizeof tryexec, "%s", v);
        else if (!strcmp(k, "OnlyShowIn")) snprintf(only, sizeof only, "%s", v);
        else if (!strcmp(k, "NotShowIn")) snprintf(notshow, sizeof notshow, "%s", v);
        else if (!strcmp(k, "Type")) snprintf(type, sizeof type, "%s", v);
        else if (!strcmp(k, "Hidden")) hidden = is_true(v);
        else if (!strcmp(k, "Terminal")) terminal = is_true(v);
        else if (!strcmp(k, "X-GNOME-Autostart-enabled") || !strcmp(k, "X-HDE-Autostart-enabled") ||
                 !strcmp(k, "X-MATE-Autostart-enabled")) disabled |= !is_true(v);
    }
    fclose(f);
    const char *base = strrchr(file, '/');
    base = base ? base + 1 : file;
    if (hidden || disabled || terminal || strcmp(type, "Application") || !*exec) return;
    if (*only && !list_has(only, "HDE")) return;
    if (*notshow && list_has(notshow, "HDE")) return;
    if (*tryexec) {
        char *p = resolve_component(tryexec, NULL);
        if (!p) return;
        free(p);
    }
    char cmd[4096];
    strip_field_codes(exec, cmd, sizeof cmd);
    /* tên chương trình (từ đầu tiên, bỏ thư mục) để lọc các mục HDE đã tự chạy */
    char prog[256];
    size_t pl = strcspn(cmd, " \t");
    snprintf(prog, sizeof prog, "%.*s", (int)(pl < sizeof prog ? pl : sizeof prog - 1), cmd);
    const char *pb = strrchr(prog, '/');
    pb = pb ? pb + 1 : prog;
    if (!strncmp(pb, "hde-", 4)) return;                                   /* HDE tự chạy các thành phần của mình */
    if (have_polkit && strstr(pb, "polkit")) return;                       /* đã có polkit agent */
    printf("hde-session: autostart %s: %s\n", base, cmd);
    spawn_shell(cmd);
}

static void run_autostart(void)
{
    if (getenv("HDE_NO_AUTOSTART")) return;
    char dirs[16][1024];
    int nd = 0;
    const char *xch = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    if (xch && *xch) snprintf(dirs[nd++], sizeof dirs[0], "%s/autostart", xch);
    else if (home) snprintf(dirs[nd++], sizeof dirs[0], "%s/.config/autostart", home);
    const char *xcd = getenv("XDG_CONFIG_DIRS");
    char buf[2048];
    snprintf(buf, sizeof buf, "%s", xcd && *xcd ? xcd : "/etc/xdg");
    char *save = NULL;
    for (char *d = strtok_r(buf, ":", &save); d && nd < 16; d = strtok_r(NULL, ":", &save))
        snprintf(dirs[nd++], sizeof dirs[0], "%s/autostart", d);

    char seen[256][256];
    int ns = 0;
    int have_polkit = comps[C_POLKIT].pid > 0;
    for (int i = 0; i < nd; i++) {
        DIR *dp = opendir(dirs[i]);
        if (!dp) continue;
        struct dirent *e;
        while ((e = readdir(dp))) {
            size_t l = strlen(e->d_name);
            if (l < 9 || strcmp(e->d_name + l - 8, ".desktop")) continue;
            int dup = 0;                               /* file cùng tên trong thư mục người dùng thắng */
            for (int s = 0; s < ns && !dup; s++) dup = !strcmp(seen[s], e->d_name);
            if (dup) continue;
            if (ns < 256) snprintf(seen[ns++], sizeof seen[0], "%s", e->d_name);
            char path[1300];
            snprintf(path, sizeof path, "%.1023s/%.255s", dirs[i], e->d_name);
            autostart_entry(path, have_polkit);
        }
        closedir(dp);
    }
    fflush(stdout);
}

/* ===================== điều khiển từ dòng lệnh ===================== */
static int request_signal(int sig)
{
    const char *env = getenv("HDE_SESSION_PID");
    pid_t target = env ? (pid_t)atoi(env) : 0;
    if (target > 1 && target != getpid() && kill(target, 0) == 0)
        return kill(target, sig) == 0 ? 0 : -1;
    DIR *d = opendir("/proc");
    if (!d) return -1;
    struct dirent *e;
    int rc = -1;
    uid_t me = getuid();
    while ((e = readdir(d))) {
        pid_t pid = (pid_t)atoi(e->d_name);
        if (pid <= 1 || pid == getpid()) continue;
        char path[64], comm[64] = "";
        snprintf(path, sizeof path, "/proc/%d/comm", (int)pid);
        struct stat st;
        if (stat(path, &st) != 0 || st.st_uid != me) continue;
        FILE *f = fopen(path, "r");
        if (!f) continue;
        if (fgets(comm, sizeof comm, f)) comm[strcspn(comm, "\n")] = '\0';
        fclose(f);
        if (strcmp(comm, "hde-session") == 0 && kill(pid, sig) == 0) { rc = 0; break; }
    }
    closedir(d);
    return rc;
}

static int action(const char *name)
{
    if (!strcmp(name, "logout")) {
        if (request_signal(SIGTERM) == 0) return 0;
        return hde_session_logout();
    }
    if (!strcmp(name, "reboot")) return hde_session_reboot();
    if (!strcmp(name, "shutdown")) return hde_session_shutdown();
    if (!strcmp(name, "suspend")) return hde_session_suspend();
    if (!strcmp(name, "lock")) return hde_session_lock();
    return -1;
}

static void usage(void)
{
    printf("Usage: hde-session [--no-wm] [--no-desktop] [--no-panel]\n"
           "       hde-session restart   restart desktop + panel of the running session (no logout)\n"
           "       hde-session wm        switch to the window manager selected in Settings (no logout)\n"
           "       hde-session {logout|reboot|shutdown|suspend|lock}\n");
}

static void reap_children(void)
{
    int status;
    pid_t p;
    while ((p = waitpid(-1, &status, WNOHANG)) > 0) {
        if (p == g_wm) { wm_exited(status); continue; }
        if (p == g_wm_old) { g_wm_old = -1; continue; }    /* WM cũ nhường chỗ như mong đợi */
        for (int i = 0; i < N_COMP; i++)
            if (comps[i].pid == p) { comp_exited(&comps[i], status); break; }
        /* các tiến trình khác (autostart, lệnh phụ) chỉ cần được thu dọn */
    }
}

int main(int argc, char **argv)
{
    if (argc > 1 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) { usage(); return 0; }
    if (argc > 1 && !strcmp(argv[1], "restart")) {
        if (request_signal(SIGUSR1) == 0) { printf("hde-session: restart requested\n"); return 0; }
        fprintf(stderr, "hde-session: no running hde-session found\n");
        return 1;
    }
    if (argc > 1 && !strcmp(argv[1], "wm")) {
        if (request_signal(SIGUSR2) == 0) { printf("hde-session: window manager switch requested\n"); return 0; }
        fprintf(stderr, "hde-session: no running hde-session found; the window manager will change at next login\n");
        return 1;
    }
    if (argc > 1 && argv[1][0] != '-') {
        if (hde_session_init() == 0 && action(argv[1]) == 0) return 0;
        fprintf(stderr, "hde-session: unknown command or action failed: %s\n", argv[1]);
        usage();
        return 1;
    }

    if (hde_session_init() != 0) {
        fprintf(stderr, "hde-session: backend init failed (DISPLAY=%s)\n", getenv("DISPLAY") ? getenv("DISPLAY") : "unset");
        return 1;
    }

    /* Lồng trong Xephyr trên host Wayland (hoặc HDE_CORE_EVENTS=1): GTK3 dùng XInput2 sẽ không nhận được
     * click thật từ Xephyr -> ép core events. Đặt HDE_XI2=1 để tắt. Phải làm TRƯỚC khi unsetenv(WAYLAND_DISPLAY). */
    if (!getenv("HDE_XI2") && (getenv("HDE_CORE_EVENTS") || getenv("WAYLAND_DISPLAY")))
        setenv("GDK_CORE_DEVICE_EVENTS", "1", 1);
    if (!hde_core_backend() || hde_core_backend()->type != HDE_BACKEND_WAYLAND) {
        unsetenv("WAYLAND_DISPLAY");
        setenv("GDK_BACKEND", "x11", 1);
    }
    {
        char pidbuf[32];
        snprintf(pidbuf, sizeof pidbuf, "%d", (int)getpid());
        setenv("HDE_SESSION_PID", pidbuf, 1);      /* panel (Log Out), Settings (đổi WM) dùng */
        setenv("XDG_CURRENT_DESKTOP", "HDE", 1);
        setenv("XDG_SESSION_DESKTOP", "HDE", 1);
        setenv("DESKTOP_SESSION", "hde", 1);
        setenv("XDG_SESSION_TYPE", "x11", 0);
        setenv("QT_QPA_PLATFORMTHEME", "gtk3", 0);   /* ứng dụng Qt theo theme GTK / Dark mode */
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    sa.sa_handler = on_usr1;
    sigaction(SIGUSR1, &sa, NULL);
    sa.sa_handler = on_usr2;
    sigaction(SIGUSR2, &sa, NULL);
    signal(SIGCHLD, SIG_DFL);
    signal(SIGPIPE, SIG_IGN);

    self_dir(g_bindir, sizeof(g_bindir), argv[0]);
    g_use_wm = getenv("HDE_NO_WM") == NULL;
    int start_desktop = 1, start_panel = 1;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--no-desktop")) start_desktop = 0;
        else if (!strcmp(argv[i], "--no-panel")) start_panel = 0;
        else if (!strcmp(argv[i], "--no-wm")) g_use_wm = 0;
    }

    /* Dịch vụ D-Bus kích hoạt theo yêu cầu (portal, keyring, ...) cần biết DISPLAY của phiên này. */
    {
        const char *dua[] = { "dbus-update-activation-environment", "--systemd", "DISPLAY", "XAUTHORITY",
                              "XDG_CURRENT_DESKTOP", "XDG_SESSION_DESKTOP", "DESKTOP_SESSION", "XDG_SESSION_TYPE",
                              "GDK_BACKEND", "QT_QPA_PLATFORMTHEME", "HDE_SESSION_PID", NULL };
        run_wait(dua, 3000);
    }

    /* 1. Window manager trước mọi ứng dụng GTK */
    if (g_use_wm) {
        wm_build_candidates(requested_wm());
        if (wm_start_next() == 0) usleep(400 * 1000);
    }

    /* 2. XSETTINGS (theme / Dark mode cho mọi app GTK) + áp lại thiết lập bàn phím, chuột, màn hình */
    comps[C_XSETTINGS].enabled = 1;
    comp_start(&comps[C_XSETTINGS]);
    {
        char *s = resolve_component("hde-settings", g_bindir);
        if (s) {
            const char *args[] = { "--apply", NULL };
            spawn_argv(s, args);
            free(s);
        }
    }

    /* 3. Phím tắt hệ thống, desktop, panel */
    comps[C_HOTKEYS].enabled = start_panel;
    comps[C_DESKTOP].enabled = start_desktop;
    comps[C_PANEL].enabled = start_panel;
    comp_start(&comps[C_HOTKEYS]);
    start_components();

    printf("HDE session running on backend: %s\n", hde_core_backend() ? hde_core_backend()->name : "none");
    fflush(stdout);

    time_t autostart_at = time(NULL) + 2;   /* sau khi panel có khay hệ thống cho các applet */
    int autostart_done = 0;
    while (!g_stop) {
        reap_children();
        if (g_restart) { g_restart = 0; restart_components(); }
        if (g_switch_wm) { g_switch_wm = 0; if (g_use_wm) wm_switch(); }
        time_t now = time(NULL);
        for (int i = 0; i < N_COMP; i++)
            if (comps[i].restart_at && now >= comps[i].restart_at) { comps[i].restart_at = 0; comp_start(&comps[i]); }
        if (g_use_wm) wm_tick(now);
        if (!autostart_done && now >= autostart_at) {
            autostart_done = 1;
            start_polkit_agent();
            run_autostart();
        }
        struct timespec ts = { 0, 250 * 1000000L };
        nanosleep(&ts, NULL);
    }

    printf("hde-session: logging out\n");
    fflush(stdout);
    stop_pid(&comps[C_PANEL].pid);
    stop_pid(&comps[C_DESKTOP].pid);
    stop_pid(&comps[C_HOTKEYS].pid);
    stop_pid(&comps[C_POLKIT].pid);
    stop_pid(&comps[C_XSETTINGS].pid);
    stop_pid(&g_wm_old);
    stop_pid(&g_wm);
    hde_session_shutdown_core();
    return 0;
}
