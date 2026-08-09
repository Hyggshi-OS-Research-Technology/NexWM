/*
 * nex-panel.h - NexPanel header
 * Status bar, taskbar, and workspace pager for NexWM
 *
 * Visual design:
 *   ┌──────────────────────────────────────────────────────────────────────┐
 *   │ [⊞ Nex] [📁Files] [⚙Sett] [▣Term] │ ①②③ │ [task][task] │ ⏻ HH:MM │
 *   └──────────────────────────────────────────────────────────────────────┘
 *
 * Icons are loaded from the Yaru icon theme (PNG, 22x22) via nex_icon.c.
 * No emoji.  No Cairo.  Just libpng + xcb_put_image.
 */

#ifndef NEX_PANEL_H
#define NEX_PANEL_H

#include <xcb/xcb.h>

/* ── Geometry ──────────────────────────────────────────────────────────── */
#define NEX_PANEL_HEIGHT      40      /* px — increased from 36 for better icon fit */
#define NEX_PANEL_POSITION    0       /* 0 = top, 1 = bottom                        */
#define NEX_PANEL_ICON_SIZE   22      /* px — XDG 22x22 icon set                    */

/* ── Colours (0xRRGGBB as used in XCB pixel values) ───────────────────── */
/* Background / surface */
#define NEX_PANEL_BG             0x11111b  /* very dark navy                   */
#define NEX_PANEL_BG_HOVER       0x1e1e2e  /* slightly lighter on hover        */

/* Foreground text */
#define NEX_PANEL_FG             0xcdd6f4  /* Catppuccin text                  */
#define NEX_PANEL_FG_DIM         0x7f849c  /* dimmed / secondary text          */

/* Start button */
#define NEX_PANEL_START_BG       0x3b5bdb  /* indigo blue                      */
#define NEX_PANEL_START_BG_HOV   0x4c6ef5  /* brighter on hover                */
#define NEX_PANEL_START_FG       0xffffff

/* Quick-launch buttons */
#define NEX_PANEL_QLNCH_BG       0x1c1c2e  /* subtle dark pill                 */
#define NEX_PANEL_QLNCH_HOV      0x313244

/* Workspace pager */
#define NEX_PANEL_WS_ACTIVE      0x5b8dd9  /* bright blue active               */
#define NEX_PANEL_WS_INACTIVE    0x1e1e2e
#define NEX_PANEL_WS_FG_ACT      0xffffff
#define NEX_PANEL_WS_FG_INACT    0x7f849c

/* Taskbar */
#define NEX_PANEL_TASK_ACTIVE    0x2a2a3e  /* slightly raised active task      */
#define NEX_PANEL_TASK_BG        0x161622
#define NEX_PANEL_TASK_ACCENT    0x5b8dd9  /* bottom indicator line            */

/* Right zone */
#define NEX_PANEL_POWER_BG       0x3d1515  /* dark red tint                    */
#define NEX_PANEL_POWER_HOV      0xa83232  /* brighter on hover                */
#define NEX_PANEL_POWER_FG       0xf38ba8

/* Decorative */
#define NEX_PANEL_BORDER_BOT     0x2a2a3e  /* 1px bottom border accent         */
#define NEX_PANEL_SEP            0x2e2e4e  /* vertical separator               */

/* ── IPC ───────────────────────────────────────────────────────────────── */
#define NEX_PANEL_SOCKET  "/tmp/nexwm.sock"

/* ── Context struct ────────────────────────────────────────────────────── */
typedef struct {
    xcb_connection_t *conn;
    xcb_screen_t     *screen;
    xcb_window_t      win;
    int               width;
    int               height;
    int               running;
} nex_panel_ctx_t;

int  nex_panel_init   (nex_panel_ctx_t *ctx);
void nex_panel_run    (nex_panel_ctx_t *ctx);
void nex_panel_cleanup(nex_panel_ctx_t *ctx);

#endif /* NEX_PANEL_H */
