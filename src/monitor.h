/*
 * monitor.h - Monitor management for NexWM
 */

#ifndef NEXWM_MONITOR_H
#define NEXWM_MONITOR_H

#include <xcb/xcb.h>
#include "config.h"

typedef struct {
    int x, y;
    int width, height;
    int workspace;
    char name[32];
} nex_monitor_t;

extern nex_monitor_t g_monitors[NEX_MAX_MONITORS];
extern int g_monitor_count;
extern int g_current_monitor;

void nex_monitor_init(xcb_connection_t *conn, xcb_screen_t *screen);
nex_monitor_t *nex_monitor_current(void);

#endif
