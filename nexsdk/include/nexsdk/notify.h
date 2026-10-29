/* nexsdk/notify.h — a notification for the user, from any program, without a toolkit.
 *
 * The notification goes through the desktop's own notification service (org.freedesktop.Notifications,
 * which HDE implements and which every other desktop has too) by running notify-send with an argv array:
 * no shell is involved, so a summary with a quote, a backtick or a semicolon in it is delivered as text
 * instead of being run as a command.
 *
 *   NexSdkNotification n = NEXSDK_NOTIFICATION_INIT;
 *   n.summary = "Backup finished";
 *   n.body = "1,204 files copied to the external drive.";
 *   n.icon = "drive-harddisk-symbolic";
 *   nexsdk_notify_send(&n);
 */
#ifndef NEXSDK_NOTIFY_H
#define NEXSDK_NOTIFY_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NEXSDK_URGENCY_LOW = 0,
    NEXSDK_URGENCY_NORMAL,
    NEXSDK_URGENCY_CRITICAL
} NexSdkUrgency;

typedef struct {
    const char *summary;      /* one line, required */
    const char *body;         /* may be NULL or "" */
    const char *icon;         /* an icon name or an absolute path; NULL = the application's icon */
    const char *app_name;     /* NULL = "HDE" */
    int timeout_ms;           /* 0 = the desktop decides, <0 = stays until it is dismissed */
    NexSdkUrgency urgency;
    /* optionally, what the notification is about: an application's own id, so that two programs' popups
     * can be told apart and a new notification can replace an older one */
    const char *replace_id;   /* NULL for a new notification */
} NexSdkNotification;

#define NEXSDK_NOTIFICATION_INIT { NULL, NULL, NULL, NULL, 0, NEXSDK_URGENCY_NORMAL, NULL }

/* Build the argv of notify-send for n.  The strings are copied into buf (which must be at least
 * NEXSDK_NOTIFY_ARGV_SIZE bytes) and argv points inside it, ending with NULL: ready for posix_spawn() or
 * execvp().  Returns the number of arguments (without the NULL), or -1 when n has no summary or the buffer
 * is too small.  This is the whole platform-independent part of sending, and it is what the unit test checks. */
#define NEXSDK_NOTIFY_ARGV_SIZE 2048
int nexsdk_notify_argv(const NexSdkNotification *n, char *buf, size_t buf_size, char *argv[], int argv_max);

/* Send it.  Returns 0 when notify-send accepted the notification, -1 when it could not be run, or the exit
 * status of notify-send when it ran and refused (e.g. no notification service in a bare TTY session). */
int nexsdk_notify_send(const NexSdkNotification *n);

/* 1 when a notification service can be reached from this process: notify-send is installed and there is a
 * session bus (DBUS_SESSION_BUS_ADDRESS, or a display so that notify-send can find the bus itself). */
int nexsdk_notify_available(void);

const char *nexsdk_urgency_name(NexSdkUrgency urgency);   /* "low" / "normal" / "critical" */

#ifdef __cplusplus
}
#endif
#endif
