/* nexsdk/src/hde-notify.c — one notification, without a shell and without a toolkit.
 *
 * notify-send is run with an argv array (posix_spawn), never through /bin/sh: a summary that contains a
 * quote, a $, a backtick or a semicolon is delivered as text.  That is the whole point of building the argv
 * here instead of with system("notify-send '...'").
 */
#define _POSIX_C_SOURCE 200809L

#include "nexsdk/nexsdk.h"

#include <errno.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

const char *nexsdk_urgency_name(NexSdkUrgency urgency)
{
    switch (urgency) {
    case NEXSDK_URGENCY_LOW: return "low";
    case NEXSDK_URGENCY_CRITICAL: return "critical";
    case NEXSDK_URGENCY_NORMAL:
    default: return "normal";
    }
}

/* Copy src into the buffer at *used, leaving a NUL after it; returns the new offset or -1 when it does not
 * fit.  The buffers are NUL separated and the argv array points into them. */
static int put_arg(char *buf, size_t size, size_t *used, const char *src)
{
    if (!src) return -1;
    size_t n = strlen(src);
    if (*used + n + 1 > size) return -1;
    memcpy(buf + *used, src, n);
    buf[*used + n] = '\0';
    *used += n + 1;
    return 0;
}

/* Write src at *used and return where it starts, or NULL when the buffer is full. */
static char *push_arg(char *buf, size_t size, size_t *used, const char *src)
{
    size_t start = *used;
    if (put_arg(buf, size, used, src) != 0) return NULL;
    return buf + start;
}

int nexsdk_notify_argv(const NexSdkNotification *n, char *buf, size_t buf_size, char *argv[], int argv_max)
{
    if (!n || !buf || buf_size == 0 || !argv || argv_max < 2) return -1;
    if (!n->summary || !*n->summary) return -1;

    size_t used = 0;
    int argc = 0;
    char number[32];
    char *a;

#define PUSH(lit)                                                   \
    do {                                                            \
        if (argc + 2 > argv_max) return -1;                         \
        if (!(a = push_arg(buf, buf_size, &used, (lit)))) return -1; \
        argv[argc++] = a;                                           \
    } while (0)

    PUSH("notify-send");
    if (n->app_name && *n->app_name) {
        PUSH("--app-name");
        PUSH(n->app_name);
    }
    if (n->icon && *n->icon) {
        PUSH("--icon");
        PUSH(n->icon);
    }
    if (n->urgency != NEXSDK_URGENCY_NORMAL) {
        PUSH("--urgency");
        PUSH(nexsdk_urgency_name(n->urgency));
    }
    if (n->timeout_ms != 0) {
        snprintf(number, sizeof number, "%d", n->timeout_ms);
        PUSH("--expire-time");
        PUSH(number);
    }
    if (n->replace_id && *n->replace_id) {
        PUSH("--replace-id");
        PUSH(n->replace_id);
    }
    /* "--" so that a summary that begins with a dash is still a summary */
    PUSH("--");
    PUSH(n->summary);
    if (n->body && *n->body) PUSH(n->body);
#undef PUSH

    argv[argc] = NULL;
    return argc;
}

int nexsdk_notify_available(void)
{
    const char *bus = getenv("DBUS_SESSION_BUS_ADDRESS");
    if (!bus || !*bus) {
        /* notify-send finds the bus itself from DISPLAY, but only when there is a session to find it in */
        const char *display = getenv("DISPLAY");
        const char *wayland = getenv("WAYLAND_DISPLAY");
        if ((!display || !*display) && (!wayland || !*wayland)) return 0;
    }
    const char *path = getenv("PATH");
    if (!path || !*path) path = "/usr/local/bin:/usr/bin:/bin";
    char candidate[4096];
    const char *p = path;
    while (*p) {
        size_t len = strcspn(p, ":");
        snprintf(candidate, sizeof candidate, "%.*s/notify-send", (int)len, p);
        if (access(candidate, X_OK) == 0) return 1;
        p += len;
        if (*p == ':') p++;
    }
    return 0;
}

int nexsdk_notify_send(const NexSdkNotification *n)
{
    static char buf[NEXSDK_NOTIFY_ARGV_SIZE];
    char *argv[32];
    int argc = nexsdk_notify_argv(n, buf, sizeof buf, argv, (int)(sizeof argv / sizeof argv[0]));
    if (argc < 0) return -1;

    pid_t pid;
    int rc = posix_spawnp(&pid, "notify-send", NULL, NULL, argv, environ);
    if (rc != 0) return -1;                 /* notify-send is not installed */
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
}
