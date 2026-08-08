/*
 * nex-notify.h - NexNotify header
 * Desktop Notification Daemon for NexWM / Hyggshi OS
 */

#ifndef NEX_NOTIFY_H
#define NEX_NOTIFY_H

#include <stdint.h>

#define NEX_NOTIFY_MAX_QUEUE 16
#define NEX_NOTIFY_WIDTH     320
#define NEX_NOTIFY_HEIGHT    80

typedef struct {
    uint32_t id;
    char app_name[64];
    char summary[128];
    char body[256];
    char icon[128];
    int timeout_ms;
    uint64_t created_at_ms;
} nex_notification_t;

typedef struct {
    nex_notification_t notifications[NEX_NOTIFY_MAX_QUEUE];
    int count;
} nex_notify_queue_t;

void nex_notify_push(const char *app_name, const char *summary, const char *body, int timeout_ms);

#endif
