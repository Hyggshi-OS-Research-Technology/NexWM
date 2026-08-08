/*
 * client.h - Client window management for NexWM
 */

#ifndef NEXWM_CLIENT_H
#define NEXWM_CLIENT_H

#include <xcb/xcb.h>
#include <xcb/xcb_icccm.h>

typedef enum {
    NEX_CLIENT_FLOATING = (1 << 0),
    NEX_CLIENT_FULLSCREEN = (1 << 1),
    NEX_CLIENT_MAXIMIZED = (1 << 2),
    NEX_CLIENT_MINIMIZED = (1 << 3),
    NEX_CLIENT_URGENT = (1 << 4),
    NEX_CLIENT_FIXED = (1 << 5),
    NEX_CLIENT_NEVER_FOCUS = (1 << 6)
} nex_client_flags_t;

typedef struct nex_client {
    xcb_window_t window;
    int x, y;
    int width, height;
    int old_x, old_y;
    int old_width, old_height;
    int basew, baseh;
    int incw, inch;
    int maxw, maxh;
    int minw, minh;
    int workspace;
    unsigned int flags;
    char class[64];
    char instance[64];
    char title[256];
    struct nex_client *next;
    struct nex_client *prev;
} nex_client_t;

extern nex_client_t *g_clients;
extern nex_client_t *g_focused;

nex_client_t *nex_client_create(xcb_window_t window);
void nex_client_destroy(nex_client_t *c);
nex_client_t *nex_client_find(xcb_window_t window);
void nex_client_focus(nex_client_t *c);
void nex_client_raise(nex_client_t *c);
void nex_client_move(nex_client_t *c, int x, int y);
void nex_client_resize(nex_client_t *c, int w, int h);
void nex_client_set_border(nex_client_t *c, uint32_t color);
void nex_client_map(nex_client_t *c);
void nex_client_unmap(nex_client_t *c);
void nex_client_minimize(nex_client_t *c);
void nex_client_unminimize(nex_client_t *c);
void nex_client_toggle_fullscreen(nex_client_t *c);
void nex_client_toggle_maximize(nex_client_t *c);
void nex_client_grab_buttons(nex_client_t *c);
void nex_client_kill(nex_client_t *c);
void nex_client_list_add(nex_client_t *c);
void nex_client_list_remove(nex_client_t *c);

#endif
