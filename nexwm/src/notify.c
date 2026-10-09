/* notify.c — deliver app-responsiveness warnings through the Freedesktop notification service. */
#define _POSIX_C_SOURCE 200809L

#include "notify.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

static void append_escaped_title(char *out, size_t capacity, const char *title)
{
    if (!title || !*title) title = "An application";
    size_t used = 0;
    for (const unsigned char *p = (const unsigned char *)title; *p && used + 7 < capacity; p++) {
        const char *replacement = NULL;
        switch (*p) {
        case '&': replacement = "&amp;"; break;
        case '<': replacement = "&lt;"; break;
        case '>': replacement = "&gt;"; break;
        case '"': replacement = "&quot;"; break;
        case '\'': replacement = "&apos;"; break;
        default: break;
        }
        if (replacement) {
            size_t length = strlen(replacement);
            memcpy(out + used, replacement, length);
            used += length;
        } else {
            out[used++] = (*p < 0x20 || *p == 0x7f) ? ' ' : (char)*p;
        }
    }
    while (used && out[used - 1] == ' ') used--;
    out[used] = '\0';
}

static void append_gvariant_string(char *out, size_t capacity, const char *text)
{
    if (capacity < 3) {
        if (capacity) out[0] = '\0';
        return;
    }
    size_t used = 0;
    out[used++] = '"';
    for (const unsigned char *p = (const unsigned char *)text; *p && used + 3 < capacity; p++) {
        if (*p == '"' || *p == '\\') out[used++] = '\\';
        if (*p < 0x20 || *p == 0x7f) out[used++] = ' ';
        else out[used++] = (char)*p;
    }
    out[used++] = '"';
    out[used] = '\0';
}

void nexwm_notify_app_unresponsive(const char *title)
{
    char escaped_title[384];
    char body[512];
    char summary_argument[64];
    char body_argument[640];
    append_escaped_title(escaped_title, sizeof escaped_title, title);
    snprintf(body, sizeof body, "%s is not responding. You can wait or close the application.", escaped_title);
    append_gvariant_string(summary_argument, sizeof summary_argument, "Application not responding");
    append_gvariant_string(body_argument, sizeof body_argument, body);

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "nexwm: warning: cannot start the unresponsive-application notification: %s\n",
                strerror(errno));
        return;
    }
    if (pid != 0) return;

    (void)setsid();
    int null_fd = open("/dev/null", O_RDWR);
    if (null_fd >= 0) {
        (void)dup2(null_fd, STDIN_FILENO);
        (void)dup2(null_fd, STDOUT_FILENO);
        (void)dup2(null_fd, STDERR_FILENO);
        if (null_fd > STDERR_FILENO) close(null_fd);
    }
    execlp("notify-send", "notify-send", "--app-name=NexWM", "--icon=dialog-warning",
           "--urgency=critical", "--", "Application not responding", body, (char *)NULL);
    if (errno == ENOENT) {
        execlp("gdbus", "gdbus", "call", "--session", "--dest", "org.freedesktop.Notifications",
               "--object-path", "/org/freedesktop/Notifications", "--method",
               "org.freedesktop.Notifications.Notify", "NexWM", "0", "dialog-warning",
               summary_argument, body_argument, "[]", "{}", "-1", (char *)NULL);
    }
    _exit(127);
}
