/*
 * ewmh.h - Extended Window Manager Hints for NexWM
 */

#ifndef NEXWM_EWMH_H
#define NEXWM_EWMH_H

#include <xcb/xcb.h>
#include <xcb/xcb_ewmh.h>

typedef struct {
    xcb_ewmh_connection_t ewmh;
} nex_ewmh_ctx_t;

extern nex_ewmh_ctx_t g_ewmh;

int nex_ewmh_init(xcb_connection_t *conn, xcb_screen_t *screen);
void nex_ewmh_set_supported(xcb_screen_t *screen);
void nex_ewmh_set_client_list(void);
void nex_ewmh_set_active_window(xcb_window_t window);
void nex_ewmh_set_current_desktop(int desktop);
void nex_ewmh_set_number_of_desktops(int count);
void nex_ewmh_set_wm_desktop(xcb_window_t window, int desktop);
void nex_ewmh_set_wm_state_hidden(xcb_window_t window, int hidden);
void nex_ewmh_cleanup(void);

#endif
