/*
 * client.h - Client window management for NexWM
 */

#ifndef NEXWM_CLIENT_H
#define NEXWM_CLIENT_H

#include <xcb/xcb.h>
#include <xcb/xcb_icccm.h>

/* Titlebar height (px) used by the decoration frame. */
#define NEX_TITLEBAR_H 24
/* Titlebar button size (px) - each button is a square of this size. */
#define NEX_TITLEBAR_BTN 24
/* Hit-test tolerance (px) for interactive edge/corner resize. */
#define NEX_RESIZE_EDGE 8

/* Titlebar button hit-test results */
enum {
    NEX_BTN_NONE = 0,
    NEX_BTN_MINIMIZE = 1,
    NEX_BTN_MAXIMIZE = 2,
    NEX_BTN_CLOSE = 3
};

/* Interactive resize directions (bitmask; any edge/corner combination). */
enum {
    NEX_RESIZE_NONE = 0,
    NEX_RESIZE_N  = (1 << 0),
    NEX_RESIZE_NE = (1 << 1),
    NEX_RESIZE_E  = (1 << 2),
    NEX_RESIZE_SE = (1 << 3),
    NEX_RESIZE_S  = (1 << 4),
    NEX_RESIZE_SW = (1 << 5),
    NEX_RESIZE_W  = (1 << 6),
    NEX_RESIZE_NW = (1 << 7)
};

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
    xcb_window_t frame;          /* decoration frame (client is a child of it) */
    int x, y;
    int width, height;           /* FRAME geometry (includes the titlebar strip) */
    int old_x, old_y;
    int old_width, old_height;
    int basew, baseh;
    int incw, inch;
    int maxw, maxh;
    int minw, minh;
    int workspace;
    unsigned int flags;
    uint32_t border_color;       /* active/inactive titlebar color */
    char class[64];
    char instance[64];
    char title[256];
    struct nex_client *next;
    struct nex_client *prev;
} nex_client_t;

extern nex_client_t *g_clients;
extern nex_client_t *g_focused;

nex_client_t *nex_client_create(xcb_window_t window);
int nex_client_update_title(nex_client_t *c);
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

/* ── decoration (titlebar) helpers ───────────────────────────────────────── */
void nex_client_decorate(nex_client_t *c);
void nex_client_redraw_titlebar(nex_client_t *c);
void nex_client_reframe(nex_client_t *c);
int  nex_client_titlebar_hit(const nex_client_t *c, int fx, int fy, int *button);
int  nex_client_titlebar_press(const nex_client_t *c, int fy);

/* ── frame geometry helpers ──────────────────────────────────────────────── */
nex_client_t *nex_client_find_frame(xcb_window_t frame);
void nex_client_place_client(nex_client_t *c);
void nex_client_set_geometry(nex_client_t *c, int x, int y, int w, int h);
void nex_client_read_size_hints(nex_client_t *c);
void nex_client_apply_size_hints(nex_client_t *c, int *w, int *h);
int  nex_client_frame_resize_hit(const nex_client_t *c, int fx, int fy, int *edge);

#endif
