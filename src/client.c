/*
 * client.c - Client window management implementation for NexWM
 *
 * NOTE ON DECORATION FRAMES:
 * client.h declares a `frame` field and a set of decoration helpers
 * (nex_client_decorate, nex_client_reframe, nex_client_titlebar_hit, ...)
 * for a future titlebar/reparenting model, matching the diagram below.
 * The lifecycle code in this file (create/destroy/move/resize/...) does
 * NOT reparent clients yet - it manages c->window directly, borders only,
 * no titlebar - matching how wm.c/events.c currently call it. `c->frame`
 * is therefore unused (left 0 / XCB_WINDOW_NONE) until reparenting is
 * implemented. The size-hint helpers below already account for a titlebar
 * strip so they're ready to drop in once nex_client_reframe/nex_client_decorate
 * are written; until then only nex_client_read_size_hints() is called
 * (from nex_client_create()) and nex_client_apply_size_hints() sits ready
 * but unused.
 *
 *   ┌──────────────────────────────── frame (c->x, c->y, c->width, c->height)
 *   │  ┌──────────────────────── titlebar (NEX_TITLEBAR_H, painted)
 *   │  │  Title text        [—] [□] [X]
 *   │  ├─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─
 *   │  │  ┌───────────────── client window (reparented, holds app content)
 *   │  │  │
 *   │  │  │
 */

#include "client.h"
#include "wm.h"
#include "atoms.h"
#include "ewmh.h"
#include "monitor.h"
#include "config.h"
#include "log.h"
#include <string.h>
#include <stdlib.h>
#include <limits.h>

nex_client_t *g_clients = NULL;
nex_client_t *g_focused = NULL;

extern xcb_connection_t *g_conn;
extern xcb_screen_t *g_screen;
extern xcb_window_t g_root;
extern nex_atoms_t g_atoms;
extern nex_config_t g_config;

/* ICCCM WM_STATE values (set on the client window as WM_STATE property). */
enum {
    NEX_WM_STATE_WITHDRAWN = 0,
    NEX_WM_STATE_NORMAL = 1,
    NEX_WM_STATE_ICONIC = 3
};

/* Flag-bits of the X WM_NORMAL_HINTS property. */
enum {
    NEX_HINT_POSITION      = 4,    /* PPosition  */
    NEX_HINT_SIZE          = 8,    /* PSize      */
    NEX_HINT_MIN_SIZE      = 16,   /* PMinSize   */
    NEX_HINT_MAX_SIZE      = 32,   /* PMaxSize   */
    NEX_HINT_RESIZE_INC    = 64,   /* PResizeInc */
    NEX_HINT_ASPECT        = 128,  /* PAspect    */
    NEX_HINT_BASE_SIZE     = 256,  /* PBaseSize  */
    NEX_HINT_WIN_GRAVITY   = 512   /* PWinGravity*/
};

/* ── decoration drawing helpers ───────────────────────────────────────── */
#define NEX_TITLEBAR_BG       0x20242b
#define NEX_TITLEBAR_BG_FOCUS 0x2d6cdf
#define NEX_TITLEBAR_FG       0xffffff

static xcb_gcontext_t make_gc(uint32_t fg, uint32_t bg)
{
    xcb_gcontext_t gc = xcb_generate_id(g_conn);
    uint32_t vals[] = { fg, bg, 0 };
    xcb_create_gc(g_conn, gc, g_root,
                  XCB_GC_FOREGROUND | XCB_GC_BACKGROUND | XCB_GC_GRAPHICS_EXPOSURES,
                  vals);
    return gc;
}

static void fill_rect(xcb_drawable_t d, xcb_gcontext_t gc,
                      int x, int y, int w, int h)
{
    xcb_rectangle_t r = { (int16_t)x, (int16_t)y,
                          (uint16_t)(w > 0 ? w : 0),
                          (uint16_t)(h > 0 ? h : 0) };
    xcb_poly_fill_rectangle(g_conn, d, gc, 1, &r);
}

static void draw_text8(xcb_drawable_t d, xcb_gcontext_t gc,
                       int x, int y, const char *text)
{
    if (!text || !*text) return;
    xcb_image_text_8(g_conn, (uint8_t)strlen(text), d, gc,
                     (int16_t)x, (int16_t)y, text);
}

static void draw_line(xcb_drawable_t d, xcb_gcontext_t gc,
                      int x1, int y1, int x2, int y2)
{
    xcb_point_t pts[2] = {
        { (int16_t)x1, (int16_t)y1 },
        { (int16_t)x2, (int16_t)y2 }
    };
    xcb_poly_line(g_conn, XCB_COORD_MODE_ORIGIN, d, gc, 2, pts);
}

static int titlebar_button_x(const nex_client_t *c, int button)
{
    if (!c) return -1;
    switch (button) {
        case NEX_BTN_MINIMIZE: return c->width - 3 * NEX_TITLEBAR_BTN;
        case NEX_BTN_MAXIMIZE: return c->width - 2 * NEX_TITLEBAR_BTN;
        case NEX_BTN_CLOSE:    return c->width - NEX_TITLEBAR_BTN;
        default: return -1;
    }
}

void nex_client_redraw_titlebar(nex_client_t *c)
{
    if (!c || c->frame == XCB_WINDOW_NONE) return;

    uint32_t bg = (c == g_focused) ? NEX_TITLEBAR_BG_FOCUS : NEX_TITLEBAR_BG;
    xcb_gcontext_t gc = make_gc(bg, bg);
    fill_rect(c->frame, gc, 0, 0, c->width, NEX_TITLEBAR_H);
    xcb_free_gc(g_conn, gc);

    gc = make_gc(NEX_TITLEBAR_FG, bg);
    int text_y = (NEX_TITLEBAR_H + 8) > 12 ? (NEX_TITLEBAR_H + 8) : 12;
    draw_text8(c->frame, gc, 8, text_y, c->title[0] ? c->title : c->class);

    /* Minimize: horizontal line */
    int x = titlebar_button_x(c, NEX_BTN_MINIMIZE);
    draw_line(c->frame, gc, x + 8, 12, x + 16, 12);

    /* Maximize: square outline */
    x = titlebar_button_x(c, NEX_BTN_MAXIMIZE);
    xcb_rectangle_t maxr = { (int16_t)(x + 8), 7, 9, 9 };
    xcb_poly_rectangle(g_conn, c->frame, gc, 1, &maxr);

    /* Close: X */
    x = titlebar_button_x(c, NEX_BTN_CLOSE);
    draw_line(c->frame, gc, x + 8, 8, x + 16, 16);
    draw_line(c->frame, gc, x + 16, 8, x + 8, 16);
    xcb_free_gc(g_conn, gc);

    xcb_flush(g_conn);
}

void nex_client_decorate(nex_client_t *c)
{
    if (!c || c->frame != XCB_WINDOW_NONE) return;

    c->frame = xcb_generate_id(g_conn);
    uint32_t values[] = {
        NEX_TITLEBAR_BG,
        XCB_EVENT_MASK_EXPOSURE |
        XCB_EVENT_MASK_BUTTON_PRESS |
        XCB_EVENT_MASK_BUTTON_RELEASE |
        XCB_EVENT_MASK_POINTER_MOTION |
        XCB_EVENT_MASK_ENTER_WINDOW |
        XCB_EVENT_MASK_LEAVE_WINDOW |
        XCB_EVENT_MASK_STRUCTURE_NOTIFY
    };

    xcb_create_window(g_conn,
                      XCB_COPY_FROM_PARENT,
                      c->frame,
                      g_root,
                      (int16_t)c->x, (int16_t)c->y,
                      (uint16_t)(c->width > 1 ? c->width : 1),
                      (uint16_t)(c->height > NEX_TITLEBAR_H ? c->height : NEX_TITLEBAR_H + 1),
                      (uint16_t)g_config.border_width,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      g_screen->root_visual,
                      XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK,
                      values);

    /* The application becomes a child of the WM-owned frame. */
    xcb_reparent_window(g_conn, c->window, c->frame,
                        (int16_t)g_config.border_width,
                        (int16_t)(NEX_TITLEBAR_H));

    /* Client borders are owned visually by the frame. */
    uint32_t bw = 0;
    xcb_configure_window(g_conn, c->window, XCB_CONFIG_WINDOW_BORDER_WIDTH, &bw);

    nex_client_reframe(c);
    nex_client_redraw_titlebar(c);
}

void nex_client_place_client(nex_client_t *c)
{
    if (!c || c->frame == XCB_WINDOW_NONE) return;
    int bw = (int)g_config.border_width;
    int cx = bw;
    int cy = NEX_TITLEBAR_H;
    int cw = c->width - 2 * bw;
    int ch = c->height - NEX_TITLEBAR_H - bw;

    if (c->flags & NEX_CLIENT_FULLSCREEN) {
        cx = 0;
        cy = 0;
        cw = c->width;
        ch = c->height;
    }

    if (cw < 1) cw = 1;
    if (ch < 1) ch = 1;

    uint32_t values[] = { (uint32_t)cx, (uint32_t)cy,
                          (uint32_t)cw, (uint32_t)ch };
    xcb_configure_window(g_conn, c->window,
                         XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                         XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
                         values);
}

void nex_client_reframe(nex_client_t *c)
{
    if (!c || c->frame == XCB_WINDOW_NONE) return;

    if (c->width < 1) c->width = 1;
    if (c->height < NEX_TITLEBAR_H + 1) c->height = NEX_TITLEBAR_H + 1;

    uint32_t frame_values[] = {
        (uint32_t)c->x, (uint32_t)c->y,
        (uint32_t)c->width, (uint32_t)c->height
    };
    xcb_configure_window(g_conn, c->frame,
                         XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                         XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
                         frame_values);
    nex_client_place_client(c);
    if (!(c->flags & NEX_CLIENT_FULLSCREEN))
        nex_client_redraw_titlebar(c);
}

int nex_client_titlebar_hit(const nex_client_t *c, int fx, int fy, int *button)
{
    if (button) *button = NEX_BTN_NONE;
    if (!c || c->frame == XCB_WINDOW_NONE) return 0;
    if (fy < 0 || fy >= NEX_TITLEBAR_H) return 0;

    if (fx >= titlebar_button_x(c, NEX_BTN_CLOSE)) {
        if (button) *button = NEX_BTN_CLOSE;
    } else if (fx >= titlebar_button_x(c, NEX_BTN_MAXIMIZE)) {
        if (button) *button = NEX_BTN_MAXIMIZE;
    } else if (fx >= titlebar_button_x(c, NEX_BTN_MINIMIZE)) {
        if (button) *button = NEX_BTN_MINIMIZE;
    }
    return 1;
}

int nex_client_titlebar_press(const nex_client_t *c, int fy)
{
    return c && c->frame != XCB_WINDOW_NONE && fy >= 0 && fy < NEX_TITLEBAR_H;
}

int nex_client_frame_resize_hit(const nex_client_t *c, int fx, int fy, int *edge)
{
    if (edge) *edge = NEX_RESIZE_NONE;
    if (!c) return 0;

    int e = NEX_RESIZE_NONE;
    int right = c->width - 1;
    int bottom = c->height - 1;
    if (fx <= NEX_RESIZE_EDGE) e |= NEX_RESIZE_W;
    if (fx >= right - NEX_RESIZE_EDGE) e |= NEX_RESIZE_E;
    if (fy <= NEX_RESIZE_EDGE) e |= NEX_RESIZE_N;
    if (fy >= bottom - NEX_RESIZE_EDGE) e |= NEX_RESIZE_S;
    if (edge) *edge = e;
    return e != NEX_RESIZE_NONE;
}

/* ── lifecycle ───────────────────────────────────────────────────────────── */

nex_client_t *nex_client_create(xcb_window_t window)
{
    nex_client_t *c = calloc(1, sizeof(nex_client_t));
    if (!c) {
        NEX_ERROR("Failed to allocate client");
        return NULL;
    }

    c->window = window;
    c->frame = XCB_WINDOW_NONE;
    c->workspace = 0;
    c->flags = 0;
    c->next = NULL;
    c->prev = NULL;

    xcb_get_geometry_cookie_t geo_cookie = xcb_get_geometry(g_conn, window);
    xcb_get_geometry_reply_t *geo = xcb_get_geometry_reply(g_conn, geo_cookie, NULL);
    if (geo) {
        c->x = geo->x;
        c->y = geo->y;
        c->width = geo->width;
        c->height = geo->height;
        free(geo);
    }

    xcb_icccm_get_wm_class_reply_t wm_class;
    xcb_get_property_cookie_t class_cookie = xcb_icccm_get_wm_class(g_conn, window);
    if (xcb_icccm_get_wm_class_reply(g_conn, class_cookie, &wm_class, NULL)) {
        strncpy(c->class, wm_class.class_name ? wm_class.class_name : "", sizeof(c->class) - 1);
        strncpy(c->instance, wm_class.instance_name ? wm_class.instance_name : "", sizeof(c->instance) - 1);
        xcb_icccm_get_wm_class_reply_wipe(&wm_class);
    }

    xcb_icccm_get_text_property_reply_t title_reply;
    xcb_get_property_cookie_t title_cookie = xcb_icccm_get_wm_name(g_conn, window);
    if (xcb_icccm_get_wm_name_reply(g_conn, title_cookie, &title_reply, NULL)) {
        strncpy(c->title, title_reply.name ? title_reply.name : "", sizeof(c->title) - 1);
        xcb_icccm_get_text_property_reply_wipe(&title_reply);
    }

    uint32_t values[1];
    values[0] = g_config.border_width;
    xcb_configure_window(g_conn, window, XCB_CONFIG_WINDOW_BORDER_WIDTH, values);

    uint32_t mask = XCB_EVENT_MASK_ENTER_WINDOW |
                    XCB_EVENT_MASK_LEAVE_WINDOW |
                    XCB_EVENT_MASK_PROPERTY_CHANGE |
                    XCB_EVENT_MASK_STRUCTURE_NOTIFY;
    xcb_change_window_attributes(g_conn, window, XCB_CW_EVENT_MASK, &mask);

    nex_client_grab_buttons(c);
    nex_client_read_size_hints(c);
    nex_client_decorate(c);

    NEX_INFO("Created client: window=0x%x, class=\"%s\", title=\"%s\"",
             window, c->class, c->title);

    nex_client_list_add(c);
    return c;
}

void nex_client_grab_buttons(nex_client_t *c)
{
    if (!c) return;
    xcb_ungrab_button(g_conn, XCB_BUTTON_INDEX_ANY, c->window, XCB_MOD_MASK_ANY);

    uint16_t modifiers[] = {
        g_config.modkey,
        g_config.modkey | XCB_MOD_MASK_2,
        g_config.modkey | XCB_MOD_MASK_LOCK,
        g_config.modkey | XCB_MOD_MASK_2 | XCB_MOD_MASK_LOCK
    };

    for (size_t i = 0; i < sizeof(modifiers)/sizeof(modifiers[0]); i++) {
        xcb_grab_button(g_conn, 0, c->window,
                        XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE,
                        XCB_GRAB_MODE_SYNC, XCB_GRAB_MODE_ASYNC,
                        XCB_NONE, XCB_NONE,
                        XCB_BUTTON_INDEX_1, modifiers[i]);
        xcb_grab_button(g_conn, 0, c->window,
                        XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE,
                        XCB_GRAB_MODE_SYNC, XCB_GRAB_MODE_ASYNC,
                        XCB_NONE, XCB_NONE,
                        XCB_BUTTON_INDEX_3, modifiers[i]);
    }
}

void nex_client_destroy(nex_client_t *c)
{
    if (!c) return;
    NEX_INFO("Destroying client: window=0x%x", c->window);
    if (g_focused == c) g_focused = NULL;
    if (c->frame != XCB_WINDOW_NONE) {
        xcb_reparent_window(g_conn, c->window, g_root, (int16_t)c->x, (int16_t)c->y);
        xcb_destroy_window(g_conn, c->frame);
        c->frame = XCB_WINDOW_NONE;
    }
    nex_client_list_remove(c);
    free(c);
}

/* ── lookup helpers ─────────────────────────────────────────────────────── */

nex_client_t *nex_client_find(xcb_window_t window)
{
    nex_client_t *c;
    for (c = g_clients; c; c = c->next) {
        if (c->window == window) return c;
    }
    return NULL;
}

nex_client_t *nex_client_find_frame(xcb_window_t frame)
{
    nex_client_t *c;
    for (c = g_clients; c; c = c->next) {
        if (c->frame == frame) return c;
    }
    return NULL;
}

/* ── focus / stacking / geometry ────────────────────────────────────────── */

void nex_client_focus(nex_client_t *c)
{
    if (!c || c == g_focused) return;
    if (g_focused) {
        g_focused->border_color = g_config.border_normal;
        if (g_focused->frame != XCB_WINDOW_NONE) {
            uint32_t bc = g_config.border_normal;
            xcb_change_window_attributes(g_conn, g_focused->frame, XCB_CW_BORDER_PIXEL, &bc);
        }
        nex_client_redraw_titlebar(g_focused);
    }
    g_focused = c;
    c->border_color = g_config.border_focus;
    if (c->frame != XCB_WINDOW_NONE) {
        uint32_t bc = g_config.border_focus;
        xcb_change_window_attributes(g_conn, c->frame, XCB_CW_BORDER_PIXEL, &bc);
    }
    nex_client_redraw_titlebar(c);
    xcb_set_input_focus(g_conn, XCB_INPUT_FOCUS_POINTER_ROOT, c->window, XCB_CURRENT_TIME);
    NEX_INFO("Focused client: window=0x%x", c->window);
}

void nex_client_raise(nex_client_t *c)
{
    if (!c) return;
    uint32_t values[] = { XCB_STACK_MODE_ABOVE };
    xcb_configure_window(g_conn, c->frame != XCB_WINDOW_NONE ? c->frame : c->window, XCB_CONFIG_WINDOW_STACK_MODE, values);
}

void nex_client_move(nex_client_t *c, int x, int y)
{
    if (!c) return;
    c->x = x; c->y = y;
    if (c->frame != XCB_WINDOW_NONE) {
        c->x = x; c->y = y;
        nex_client_reframe(c);
    } else {
        uint32_t values[] = { (uint32_t)x, (uint32_t)y };
        xcb_configure_window(g_conn, c->window, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, values);
    }
}

void nex_client_resize(nex_client_t *c, int w, int h)
{
    if (!c) return;
    c->width = w; c->height = h;
    if (c->frame != XCB_WINDOW_NONE) {
        nex_client_reframe(c);
    } else {
        uint32_t values[] = { (uint32_t)w, (uint32_t)h };
        xcb_configure_window(g_conn, c->window, XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, values);
    }
}

void nex_client_set_border(nex_client_t *c, uint32_t color)
{
    if (!c) return;
    c->border_color = color;
    uint32_t values[] = { color };
    xcb_change_window_attributes(g_conn, c->frame != XCB_WINDOW_NONE ? c->frame : c->window, XCB_CW_BORDER_PIXEL, values);
}

void nex_client_map(nex_client_t *c)
{
    if (!c) return;
    xcb_map_window(g_conn, c->frame != XCB_WINDOW_NONE ? c->frame : c->window);
}

void nex_client_unmap(nex_client_t *c)
{
    if (!c) return;
    xcb_unmap_window(g_conn, c->frame != XCB_WINDOW_NONE ? c->frame : c->window);
}

/* ── minimize / fullscreen / maximize ───────────────────────────────────── */

void nex_client_minimize(nex_client_t *c)
{
    if (!c || (c->flags & NEX_CLIENT_MINIMIZED)) return;
    c->flags |= NEX_CLIENT_MINIMIZED;
    xcb_unmap_window(g_conn, c->frame != XCB_WINDOW_NONE ? c->frame : c->window);
    nex_ewmh_set_wm_state_hidden(c->window, 1);
    if (g_focused == c) {
        g_focused = NULL;
        /* Try to focus the next visible client */
        nex_client_t *n;
        for (n = g_clients; n; n = n->next) {
            if (n != c && !(n->flags & NEX_CLIENT_MINIMIZED) && n->workspace == c->workspace) {
                nex_client_focus(n);
                break;
            }
        }
    }
    xcb_flush(g_conn);
    NEX_INFO("Minimized client 0x%x", c->window);
}

void nex_client_unminimize(nex_client_t *c)
{
    if (!c || !(c->flags & NEX_CLIENT_MINIMIZED)) return;
    c->flags &= ~NEX_CLIENT_MINIMIZED;
    xcb_map_window(g_conn, c->frame != XCB_WINDOW_NONE ? c->frame : c->window);
    nex_ewmh_set_wm_state_hidden(c->window, 0);
    nex_client_focus(c);
    xcb_flush(g_conn);
    NEX_INFO("Unminimized client 0x%x", c->window);
}

void nex_client_toggle_fullscreen(nex_client_t *c)
{
    if (!c) return;

    if (c->flags & NEX_CLIENT_FULLSCREEN) {
        c->flags &= ~NEX_CLIENT_FULLSCREEN;
        nex_client_move(c, c->old_x, c->old_y);
        nex_client_resize(c, c->old_width, c->old_height);
        uint32_t bw = g_config.border_width;
        if (c->frame != XCB_WINDOW_NONE)
            xcb_configure_window(g_conn, c->frame, XCB_CONFIG_WINDOW_BORDER_WIDTH, &bw);
        else
            xcb_configure_window(g_conn, c->window, XCB_CONFIG_WINDOW_BORDER_WIDTH, &bw);
        nex_client_redraw_titlebar(c);
    } else {
        c->old_x = c->x;
        c->old_y = c->y;
        c->old_width = c->width;
        c->old_height = c->height;
        c->flags |= NEX_CLIENT_FULLSCREEN;

        nex_monitor_t *m = nex_monitor_current();
        nex_client_move(c, m->x, m->y);
        nex_client_resize(c, m->width, m->height);
        uint32_t bw = 0;
        if (c->frame != XCB_WINDOW_NONE)
            xcb_configure_window(g_conn, c->frame, XCB_CONFIG_WINDOW_BORDER_WIDTH, &bw);
        else
            xcb_configure_window(g_conn, c->window, XCB_CONFIG_WINDOW_BORDER_WIDTH, &bw);
    }
    nex_client_raise(c);
}

void nex_client_toggle_maximize(nex_client_t *c)
{
    if (!c) return;

    if (c->flags & NEX_CLIENT_MAXIMIZED) {
        c->flags &= ~NEX_CLIENT_MAXIMIZED;
        nex_client_move(c, c->old_x, c->old_y);
        nex_client_resize(c, c->old_width, c->old_height);
    } else {
        c->old_x = c->x;
        c->old_y = c->y;
        c->old_width = c->width;
        c->old_height = c->height;
        c->flags |= NEX_CLIENT_MAXIMIZED;

        nex_monitor_t *m = nex_monitor_current();
        int bw = g_config.border_width;
        nex_client_move(c, m->x + bw, m->y + bw);
        nex_client_resize(c, m->width - 2 * bw, m->height - 2 * bw);
    }
    nex_client_raise(c);
}

void nex_client_kill(nex_client_t *c)
{
    if (!c) return;
    xcb_icccm_get_wm_protocols_reply_t protocols;
    xcb_get_property_cookie_t proto_cookie = xcb_icccm_get_wm_protocols(g_conn, c->window, g_atoms.wm_protocols);
    int has_delete = 0;
    if (xcb_icccm_get_wm_protocols_reply(g_conn, proto_cookie, &protocols, NULL)) {
        for (unsigned int i = 0; i < protocols.atoms_len; i++) {
            if (protocols.atoms[i] == g_atoms.wm_delete_window) {
                has_delete = 1;
                break;
            }
        }
        xcb_icccm_get_wm_protocols_reply_wipe(&protocols);
    }

    if (has_delete) {
        xcb_client_message_event_t ev = {
            .response_type = XCB_CLIENT_MESSAGE,
            .format = 32,
            .window = c->window,
            .type = g_atoms.wm_protocols,
            .data = { .data32 = { g_atoms.wm_delete_window, XCB_CURRENT_TIME } }
        };
        xcb_send_event(g_conn, 0, c->window, XCB_EVENT_MASK_NO_EVENT, (const char *)&ev);
        NEX_INFO("Sent WM_DELETE_WINDOW to client 0x%x", c->window);
    } else {
        xcb_kill_client(g_conn, c->window);
        NEX_INFO("Killed client 0x%x (no WM_DELETE_WINDOW)", c->window);
    }
}

/* ── client list ─────────────────────────────────────────────────────────── */

void nex_client_list_add(nex_client_t *c)
{
    if (!c) return;
    c->next = g_clients;
    if (g_clients) g_clients->prev = c;
    g_clients = c;
}

void nex_client_list_remove(nex_client_t *c)
{
    if (!c) return;
    if (c->next) c->next->prev = c->prev;
    if (c->prev) c->prev->next = c->next;
    else g_clients = c->next;
    c->next = NULL;
    c->prev = NULL;
}

/* ── size hints (WM_NORMAL_HINTS / WM_SIZE_HINTS) ──────────────────────── */

void nex_client_read_size_hints(nex_client_t *c)
{
    if (!c) return;
    c->minw = 1;
    c->minh = 1;
    c->maxw = INT_MAX;
    c->maxh = INT_MAX;
    c->basew = 0;
    c->baseh = 0;
    c->incw = 1;
    c->inch = 1;

    /* WM_SIZE_HINTS is a packed array of 18 int32 values (ICCCM §4.1.2.3).
     * Field offsets: 0=flags 1=x 2=y 3=w 4=h 5=min_w 6=min_h 7=max_w
     * 8=max_h 9=w_inc 10=h_inc 11=min_aspect_x 12=min_aspect_y
     * 13=max_aspect_x 14=max_aspect_y 15=base_w 16=base_h 17=win_gravity. */
    xcb_get_property_cookie_t pc = xcb_get_property(
        g_conn, 0, c->window, g_atoms.wm_normal_hints,
        XCB_ATOM_WM_SIZE_HINTS, 0, 18);
    xcb_get_property_reply_t *rep = xcb_get_property_reply(g_conn, pc, NULL);
    if (!rep || rep->type == XCB_ATOM_NONE) {
        free(rep);
        return;
    }

    int len = xcb_get_property_value_length(rep);
    if (len < 9 * (int)sizeof(int32_t)) {
        free(rep);
        return;
    }
    const int32_t *d = (const int32_t *)xcb_get_property_value(rep);
    int n = len / (int)sizeof(int32_t);
    if (n > 18) n = 18;
    uint32_t flags = (uint32_t)d[0];

    if ((flags & NEX_HINT_MIN_SIZE) && n > 6) {
        c->minw = (int)d[5];
        c->minh = (int)d[6];
    }
    if ((flags & NEX_HINT_MAX_SIZE) && n > 8) {
        c->maxw = (int)d[7];
        c->maxh = (int)d[8];
    }
    if ((flags & NEX_HINT_RESIZE_INC) && n > 10) {
        c->incw = (int)d[9];
        c->inch = (int)d[10];
    }
    if ((flags & NEX_HINT_BASE_SIZE) && n > 16) {
        c->basew = (int)d[15];
        c->baseh = (int)d[16];
    }

    /* Sanitize: increments >= 1, maximums >= minimums. */
    if (c->incw < 1) c->incw = 1;
    if (c->inch < 1) c->inch = 1;
    if (c->minw < 1) c->minw = 1;
    if (c->minh < 1) c->minh = 1;
    if (c->maxw > 0 && c->maxw < c->minw) c->maxw = c->minw;
    if (c->maxh > 0 && c->maxh < c->minh) c->maxh = c->minh;

    free(rep);
    NEX_DEBUG("Size hints for 0x%x: min %dx%d max %dx%d inc %dx%d base %dx%d",
              c->window, c->minw, c->minh,
              c->maxw == INT_MAX ? -1 : c->maxw,
              c->maxh == INT_MAX ? -1 : c->maxh,
              c->incw, c->inch, c->basew, c->baseh);
}

/* Apply WM_NORMAL_HINTS to a desired FRAME size. *w / *h are in frame
 * coordinates; hints are expressed in client coordinates, so the titlebar
 * strip and the border are subtracted before clamping.
 *
 * Not called yet: nothing reparents clients into a decoration frame, so
 * there's no titlebar strip to subtract. Wire this into nex_client_reframe()
 * / interactive resize once that lands. */
void nex_client_apply_size_hints(nex_client_t *c, int *w, int *h)
{
    if (!c || !w || !h) return;
    int bw = (int)g_config.border_width;
    int cw = *w - 2 * bw;               /* client width  */
    int ch = *h - NEX_TITLEBAR_H - bw;  /* client height */

    if (cw < 1) cw = 1;
    if (ch < 1) ch = 1;

    if (cw < c->minw) cw = c->minw;
    if (ch < c->minh) ch = c->minh;
    if (c->maxw > 0 && cw > c->maxw) cw = c->maxw;
    if (c->maxh > 0 && ch > c->maxh) ch = c->maxh;

    /* Honor resize increments (round up, then fall back below the maximum). */
    if (c->incw > 1) {
        int rw = cw - c->basew;
        if (rw < 0) rw = 0;
        int stepped = c->basew + ((rw + c->incw - 1) / c->incw) * c->incw;
        if (c->maxw > 0 && stepped > c->maxw) {
            int cur = stepped - c->incw;
            stepped = (cur >= c->minw) ? cur : c->minw;
        }
        cw = stepped;
    }
    if (c->inch > 1) {
        int rh = ch - c->baseh;
        if (rh < 0) rh = 0;
        int stepped = c->baseh + ((rh + c->inch - 1) / c->inch) * c->inch;
        if (c->maxh > 0 && stepped > c->maxh) {
            int cur = stepped - c->inch;
            stepped = (cur >= c->minh) ? cur : c->minh;
        }
        ch = stepped;
    }

    *w = cw + 2 * bw;
    *h = ch + NEX_TITLEBAR_H + bw;
}
