/*
 * nex-panel.h - NexPanel header
 * Status bar, taskbar, and workspace pager for NexWM
 */

#ifndef NEX_PANEL_H
#define NEX_PANEL_H

#include <xcb/xcb.h>

#define NEX_PANEL_HEIGHT      36      /* px */
#define NEX_PANEL_POSITION    0       /* 0 = top, 1 = bottom */
#define NEX_PANEL_FONT        "monospace:size=10"
#define NEX_PANEL_BG          0x161622
#define NEX_PANEL_FG          0xcdd6f4
#define NEX_PANEL_BORDER      0x2a2a3e
#define NEX_PANEL_WS_ACTIVE   0x5b8dd9
#define NEX_PANEL_WS_INACTIVE 0x242438
#define NEX_PANEL_WIN_ACTIVE  0x313244
#define NEX_PANEL_WIN_BG      0x1c1c2e
#define NEX_PANEL_SEPARATOR   0x2e2e4e
#define NEX_PANEL_ACCENT      0x5b8dd9

/* IPC socket path (mirrors NexWM) */
#define NEX_PANEL_SOCKET      "/tmp/nexwm.sock"

typedef struct {
    xcb_connection_t *conn;
    xcb_screen_t     *screen;
    xcb_window_t      win;
    int               width;
    int               height;
    int               running;
} nex_panel_ctx_t;

int  nex_panel_init(nex_panel_ctx_t *ctx);
void nex_panel_run(nex_panel_ctx_t *ctx);
void nex_panel_cleanup(nex_panel_ctx_t *ctx);

#endif
