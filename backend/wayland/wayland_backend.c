/* The Wayland backend of hde-core: the "HDE (Wayland)" session (labwc compositor, see apps/hde-session.c).
 * Session actions are the same shell commands as on X11 (systemd / logind; HDE_SH_LOCK runs HDE's own lock screen,
 * hde-lock, first and only falls back to another locker). */
#include "hde/backend.h"
#include "hde-commands.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

static int run(const char *cmd)
{
    if (!cmd || !*cmd) return -1;
    pid_t p = fork();
    if (p < 0) return -1;
    if (p == 0) {
        setsid();
        execl("/bin/sh", "sh", "-c", cmd, (char *)0);
        _exit(127);
    }
    return 0;
}

static int init(HDEBackend *b) { (void)b; return getenv("WAYLAND_DISPLAY") || getenv("HDE_BACKEND") ? 0 : -1; }
static void fini(HDEBackend *b) { (void)b; }
static int launch(HDEBackend *b, const char *c) { (void)b; return run(c); }
static int logout(HDEBackend *b)
{
    (void)b;
    return run("if [ -n \"$HDE_SESSION_PID\" ]; then kill -TERM \"$HDE_SESSION_PID\"; "
               "elif [ -n \"$LABWC_PID\" ]; then kill -TERM \"$LABWC_PID\"; "
               "elif [ -n \"$XDG_SESSION_ID\" ]; then loginctl terminate-session \"$XDG_SESSION_ID\"; fi");
}
static int reboot_(HDEBackend *b) { (void)b; return run("systemctl reboot"); }
static int poweroff(HDEBackend *b) { (void)b; return run("systemctl poweroff"); }
static int suspend_(HDEBackend *b) { (void)b; return run("systemctl suspend"); }
static int lock_(HDEBackend *b) { (void)b; return run(HDE_SH_LOCK); }

HDEBackend *hde_backend_wayland_create(void)
{
    HDEBackend *b = calloc(1, sizeof(*b));
    if (!b) return NULL;
    b->type = HDE_BACKEND_WAYLAND;
    b->name = "wayland";
    b->init = init;
    b->shutdown = fini;
    b->launch = launch;
    b->logout = logout;
    b->reboot = reboot_;
    b->poweroff = poweroff;
    b->suspend = suspend_;
    b->lock = lock_;
    return b;
}
