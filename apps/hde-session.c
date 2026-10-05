#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "hde/session.h"
#include "hde/core.h"
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop = 0;
static volatile sig_atomic_t g_restart = 0;   /* SIGUSR1: chạy lại desktop + panel, không logout */
static char g_bindir[4096];
static int g_start_desktop = 1, g_start_panel = 1;
static pid_t g_desktop = -1;
static pid_t g_panel = -1;
static pid_t g_wm = -1;
static pid_t g_hotkeys = -1;

static const char *const wm_list[] = { "xfwm4", "openbox", "marco", "metacity", "icewm", "fluxbox", "nexwm", NULL };

static void on_signal(int sig) { (void)sig; g_stop = 1; }
static void on_usr1(int sig) { (void)sig; g_restart = 1; }

static int executable(const char *p) { return p && access(p, X_OK) == 0; }

static int self_dir(char *out, size_t n, const char *argv0) {
    char buf[4096];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf)-1);
    const char *src = argv0;
    if (len > 0) { buf[len] = '\0'; src = buf; }
    const char *slash = strrchr(src, '/');
    if (!slash) { snprintf(out, n, "."); return 0; }
    size_t l = (size_t)(slash - src);
    if (l >= n) return -1;
    memcpy(out, src, l); out[l] = '\0';
    return 0;
}

/* Trả về chuỗi malloc'd (caller free). Trước đây dùng static buffer nên "desktop" và
 * "panel" cùng trỏ về một chuỗi -> session chạy hde-panel 2 lần và không chạy hde-desktop. */
static char *resolve_component(const char *name, const char *bindir) {
    char path[4096];
    if (bindir) {
        snprintf(path, sizeof(path), "%s/%s", bindir, name);
        if (executable(path)) return strdup(path);
    }
    const char *p = getenv("PATH");
    if (p) {
        char *copy = strdup(p);
        if (!copy) return NULL;
        for (char *d = strtok(copy, ":"); d; d = strtok(NULL, ":")) {
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

static pid_t spawn(const char *path) {
    pid_t p = fork();
    if (p < 0) { perror("hde-session: fork"); return -1; }
    if (p == 0) {
        execl(path, path, (char *)NULL);
        perror(path);
        _exit(127);
    }
    return p;
}

static void stop_child(pid_t *p) {
    if (*p > 0) {
        kill(*p, SIGTERM);
        /* chờ tối đa ~2s rồi SIGKILL để restart không bị treo */
        for (int i = 0; i < 20; ++i) {
            if (waitpid(*p, NULL, WNOHANG) != 0) { *p = -1; return; }
            usleep(100 * 1000);
        }
        kill(*p, SIGKILL);
        waitpid(*p, NULL, 0);
        *p = -1;
    }
}

/* Chạy hde-desktop rồi hde-panel (resolve lại mỗi lần -> lấy đúng bản vừa build/cài). */
static void start_components(void) {
    char *desktop = g_start_desktop ? resolve_component("hde-desktop", g_bindir) : NULL;
    char *panel   = g_start_panel   ? resolve_component("hde-panel", g_bindir) : NULL;

    if (g_start_desktop && desktop) {
        printf("hde-session: starting desktop: %s\n", desktop);
        g_desktop = spawn(desktop);
        usleep(400 * 1000);   /* để desktop map trước; panel còn tự raise lên trên */
    } else if (g_start_desktop) {
        fprintf(stderr, "hde-session: hde-desktop not found; session will stay alive but desktop is unavailable.\n");
    }

    if (g_start_panel && panel) {
        printf("hde-session: starting panel: %s\n", panel);
        g_panel = spawn(panel);
    } else if (g_start_panel) {
        fprintf(stderr, "hde-session: hde-panel not found; panel is unavailable.\n");
    }
    fflush(stdout);
    free(desktop);
    free(panel);
}

static void restart_components(void) {
    printf("hde-session: restarting desktop + panel (no logout)\n");
    stop_child(&g_panel);
    stop_child(&g_desktop);
    start_components();
}

/* "hde-session restart": báo cho hde-session đang chạy (cùng user) chạy lại desktop/panel. */
static int request_restart(void) {
    const char *env = getenv("HDE_SESSION_PID");
    pid_t target = env ? (pid_t)atoi(env) : 0;
    if (target > 1 && target != getpid() && kill(target, 0) == 0) {
        return kill(target, SIGUSR1) == 0 ? 0 : -1;
    }
    DIR *d = opendir("/proc");
    if (!d) return -1;
    struct dirent *e;
    int rc = -1;
    while ((e = readdir(d))) {
        pid_t pid = (pid_t)atoi(e->d_name);
        if (pid <= 1 || pid == getpid()) continue;
        char path[64], comm[64] = "";
        snprintf(path, sizeof path, "/proc/%d/comm", (int)pid);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        if (fgets(comm, sizeof comm, f)) comm[strcspn(comm, "\n")] = '\0';
        fclose(f);
        if (strcmp(comm, "hde-session") == 0 && kill(pid, SIGUSR1) == 0) { rc = 0; break; }
    }
    closedir(d);
    return rc;
}

static int action(const char *name) {
    if (!strcmp(name, "logout")) return hde_session_logout();
    if (!strcmp(name, "reboot")) return hde_session_reboot();
    if (!strcmp(name, "shutdown")) return hde_session_shutdown();
    if (!strcmp(name, "suspend")) return hde_session_suspend();
    if (!strcmp(name, "lock")) return hde_session_lock();
    return -1;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "restart")) {
        if (request_restart() == 0) { printf("hde-session: restart requested\n"); return 0; }
        fprintf(stderr, "hde-session: no running hde-session found\n");
        return 1;
    }
    if (argc > 1 && action(argv[1]) == 0) return 0;

    if (hde_session_init() != 0) {
        fprintf(stderr, "hde-session: backend init failed (DISPLAY=%s)\n",
                getenv("DISPLAY") ? getenv("DISPLAY") : "unset");
        return 1;
    }

    /* Chạy lồng trong Xephyr trên host Wayland (hoặc HDE_CORE_EVENTS=1): GTK3 dùng XInput2 sẽ không nhận
     * được click/chuột thật từ Xephyr (xdotool/XTEST vẫn chạy nên khó thấy). Ép GTK dùng core events.
     * Đặt HDE_XI2=1 để tắt hành vi này. Phải làm TRƯỚC khi unsetenv(WAYLAND_DISPLAY). */
    if (!getenv("HDE_XI2") && (getenv("HDE_CORE_EVENTS") || getenv("WAYLAND_DISPLAY")))
        setenv("GDK_CORE_DEVICE_EVENTS", "1", 1);

    /* Các component là GTK3/X11. Nếu host đang chạy Wayland (WAYLAND_DISPLAY được set) mà DISPLAY=:2
     * (Xephyr) thì GTK sẽ chọn Wayland và mở panel/desktop trên host thay vì trong phiên HDE. */
    if (!hde_core_backend() || hde_core_backend()->type != HDE_BACKEND_WAYLAND) {
        unsetenv("WAYLAND_DISPLAY");
        setenv("GDK_BACKEND", "x11", 1);
    }
    {
        char pidbuf[32];
        snprintf(pidbuf, sizeof pidbuf, "%d", (int)getpid());
        setenv("HDE_SESSION_PID", pidbuf, 1);   /* panel dùng để Log Out */
        setenv("XDG_CURRENT_DESKTOP", "HDE", 1);
        setenv("XDG_SESSION_DESKTOP", "HDE", 1);
        setenv("DESKTOP_SESSION", "hde", 1);
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGUSR1, on_usr1);
    signal(SIGCHLD, SIG_DFL);

    self_dir(g_bindir, sizeof(g_bindir), argv[0]);
    const char *bindir = g_bindir;

    int start_wm = getenv("HDE_NO_WM") == NULL;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--no-desktop")) g_start_desktop = 0;
        else if (!strcmp(argv[i], "--no-panel")) g_start_panel = 0;
        else if (!strcmp(argv[i], "--no-wm")) start_wm = 0;
        else if (!strcmp(argv[i], "--help")) {
            printf("Usage: hde-session [--no-wm] [--no-desktop] [--no-panel]\n");
            printf("       hde-session restart     (chạy lại desktop+panel của phiên đang chạy, không logout)\n");
            printf("       hde-session {logout|reboot|shutdown|suspend|lock}\n");
            hde_session_shutdown_core();
            return 0;
        }
    }

    /* Window manager trước mọi GTK app. HDE_WM=auto (mặc định) dùng fallback.
     * HDE_WM=xfwm4/openbox/... cho phép chọn WM trên máy thật. */
    if (start_wm) {
        const char *requested_wm = getenv("HDE_WM");
        if (requested_wm && *requested_wm && strcmp(requested_wm, "auto") != 0) {
            char *wm = resolve_component(requested_wm, bindir);
            if (wm) {
                printf("hde-session: starting requested window manager: %s\n", wm);
                g_wm = spawn(wm);
                free(wm);
            } else {
                fprintf(stderr, "hde-session: requested WM '%s' not found; using automatic fallback.\n", requested_wm);
            }
        }
        for (int i = 0; wm_list[i] && g_wm < 0; ++i) {
            char *wm = resolve_component(wm_list[i], bindir);
            if (!wm) continue;
            printf("hde-session: starting window manager: %s\n", wm);
            g_wm = spawn(wm);
            free(wm);
        }
        if (g_wm < 0)
            fprintf(stderr, "hde-session: no window manager found (xfwm4/openbox/...); windows will have no decorations or focus.\n");
        else
            usleep(400 * 1000);
    }

    /* System shortcuts are independent of GTK and run in the same X11 session. */
    if (g_start_panel) {
        char *hotkeys = resolve_component("hde-hotkeys", bindir);
        if (hotkeys) {
            printf("hde-session: starting system hotkeys: %s\n", hotkeys);
            g_hotkeys = spawn(hotkeys);
            free(hotkeys);
        } else {
            fprintf(stderr, "hde-session: hde-hotkeys not found; PrtSc/SysRq shortcuts unavailable.\n");
        }
    }

    start_components();

    printf("HDE session running on backend: %s\n",
           hde_core_backend() ? hde_core_backend()->name : "none");
    fflush(stdout);

    while (!g_stop) {
        if (g_restart) { g_restart = 0; restart_components(); }
        int status;
        pid_t p = waitpid(-1, &status, WNOHANG);
        if (p <= 0) {
            if (p == -1 && errno != ECHILD && errno != EINTR) perror("hde-session: waitpid");
        } else if (p == g_desktop) {
            g_desktop = -1;
            fprintf(stderr, "hde-session: desktop exited (status=%d)\n", status);
        } else if (p == g_panel) {
            g_panel = -1;
            fprintf(stderr, "hde-session: panel exited (status=%d)\n", status);
        } else if (p == g_wm) {
            g_wm = -1;
            fprintf(stderr, "hde-session: window manager exited (status=%d)\n", status);
        } else if (p == g_hotkeys) {
            g_hotkeys = -1;
            fprintf(stderr, "hde-session: hotkeys daemon exited (status=%d)\n", status);
        }
        sleep(1);
    }

    stop_child(&g_panel);
    stop_child(&g_desktop);
    stop_child(&g_wm);
    stop_child(&g_hotkeys);
    hde_session_shutdown_core();
    return 0;
}
