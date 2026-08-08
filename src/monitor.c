/*
 * monitor.c - Monitor management implementation for NexWM
 * Phase 1: Single monitor fallback (RandR in Phase 8)
 */

#include "monitor.h"
#include "log.h"
#include <string.h>

nex_monitor_t g_monitors[NEX_MAX_MONITORS];
int g_monitor_count = 0;
int g_current_monitor = 0;

void nex_monitor_init(xcb_connection_t *conn, xcb_screen_t *screen)
{
    (void)conn;
    memset(g_monitors, 0, sizeof(g_monitors));

    g_monitors[0].x = 0;
    g_monitors[0].y = 0;
    g_monitors[0].width = screen->width_in_pixels;
    g_monitors[0].height = screen->height_in_pixels;
    g_monitors[0].workspace = 0;
    strncpy(g_monitors[0].name, "primary", sizeof(g_monitors[0].name) - 1);

    g_monitor_count = 1;
    g_current_monitor = 0;

    NEX_INFO("Monitor initialized: %dx%d at (%d,%d)",
             g_monitors[0].width, g_monitors[0].height,
             g_monitors[0].x, g_monitors[0].y);
}

nex_monitor_t *nex_monitor_current(void)
{
    return &g_monitors[g_current_monitor];
}
