#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

/*
 * nex-panel.c - NexPanel
 * Status bar, taskbar, and workspace pager for NexWM.
 *
 * Architecture:
 *   - Creates a strut-reserved top/bottom XCB window (_NET_WM_STRUT)
 *   - Polls _NET_CLIENT_LIST, _NET_ACTIVE_WINDOW, _NET_CURRENT_DESKTOP
 *     from the root window via X11 property change events
 *   - Left zone:  workspace pager buttons
 *   - Centre zone: window taskbar (title truncated, click-to-focus / click-active-to-minimize)
 *   - Right zone: clock, system stats
 *   - Launcher button at far left
 *
 * Window interaction (click):
 *   focus     → set _NET_ACTIVE_WINDOW via ClientMessage
 *   minimize  → nexwmctl minimize <win_id>  (IPC)
 */

#include "nex-panel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <poll.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <xcb/xcb.h>

/* ─── colours ─────────────────────────────────────────────────────────────── */
#define CLR_BG          NEX_PANEL_BG
#define CLR_FG          NEX_PANEL_FG
#define CLR_WS_ACT      NEX_PANEL_WS_ACTIVE
#define CLR_WS_INACT    NEX_PANEL_WS_INACTIVE
#define CLR_WIN_ACT     NEX_PANEL_WIN_ACTIVE
#define CLR_WIN_BG      NEX_PANEL_WIN_BG
#define CLR_SEP         NEX_PANEL_SEPARATOR

/* ─── atoms ───────────────────────────────────────────────────────────────── */
typedef struct {
    xcb_atom_t net_client_list;
    xcb_atom_t net_active_window;
    xcb_atom_t net_current_desktop;
    xcb_atom_t net_number_of_desktops;
    xcb_atom_t net_wm_name;
    xcb_atom_t net_wm_strut;
    xcb_atom_t net_wm_strut_partial;
    xcb_atom_t net_wm_window_type;
    xcb_atom_t net_wm_window_type_dock;
    xcb_atom_t net_wm_state;
    xcb_atom_t net_wm_state_above;
    xcb_atom_t utf8_string;
    xcb_atom_t wm_protocols;
    xcb_atom_t wm_delete_window;
} panel_atoms_t;

static panel_atoms_t g_atoms;

static xcb_atom_t intern_atom(xcb_connection_t *conn, const char *name)
{
    xcb_intern_atom_cookie_t c = xcb_intern_atom(conn, 0, (uint16_t)strlen(name), name);
    xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(conn, c, NULL);
    xcb_atom_t a = r ? r->atom : XCB_ATOM_NONE;
    free(r);
    return a;
}

static void init_atoms(xcb_connection_t *conn)
{
    g_atoms.net_client_list          = intern_atom(conn, "_NET_CLIENT_LIST");
    g_atoms.net_active_window        = intern_atom(conn, "_NET_ACTIVE_WINDOW");
    g_atoms.net_current_desktop      = intern_atom(conn, "_NET_CURRENT_DESKTOP");
    g_atoms.net_number_of_desktops   = intern_atom(conn, "_NET_NUMBER_OF_DESKTOPS");
    g_atoms.net_wm_name              = intern_atom(conn, "_NET_WM_NAME");
    g_atoms.net_wm_strut             = intern_atom(conn, "_NET_WM_STRUT");
    g_atoms.net_wm_strut_partial     = intern_atom(conn, "_NET_WM_STRUT_PARTIAL");
    g_atoms.net_wm_window_type       = intern_atom(conn, "_NET_WM_WINDOW_TYPE");
    g_atoms.net_wm_window_type_dock  = intern_atom(conn, "_NET_WM_WINDOW_TYPE_DOCK");
    g_atoms.net_wm_state             = intern_atom(conn, "_NET_WM_STATE");
    g_atoms.net_wm_state_above       = intern_atom(conn, "_NET_WM_STATE_ABOVE");
    g_atoms.utf8_string              = intern_atom(conn, "UTF8_STRING");
    g_atoms.wm_protocols             = intern_atom(conn, "WM_PROTOCOLS");
    g_atoms.wm_delete_window         = intern_atom(conn, "WM_DELETE_WINDOW");
}

/* ─── property helpers ────────────────────────────────────────────────────── */

static uint32_t get_cardinal(xcb_connection_t *conn, xcb_window_t win, xcb_atom_t prop)
{
    xcb_get_property_cookie_t c = xcb_get_property(conn, 0, win, prop, XCB_ATOM_CARDINAL, 0, 1);
    xcb_get_property_reply_t *r = xcb_get_property_reply(conn, c, NULL);
    uint32_t val = 0;
    if (r && r->value_len) val = *(uint32_t *)xcb_get_property_value(r);
    free(r);
    return val;
}

static int get_windows(xcb_connection_t *conn, xcb_window_t root, xcb_atom_t prop,
                       xcb_window_t *out, int max)
{
    xcb_get_property_cookie_t c = xcb_get_property(conn, 0, root, prop, XCB_ATOM_WINDOW, 0, (uint32_t)max);
    xcb_get_property_reply_t *r = xcb_get_property_reply(conn, c, NULL);
    if (!r) return 0;
    int n = xcb_get_property_value_length(r) / (int)sizeof(xcb_window_t);
    if (n > max) n = max;
    memcpy(out, xcb_get_property_value(r), (size_t)n * sizeof(xcb_window_t));
    free(r);
    return n;
}

static int get_wm_name(xcb_connection_t *conn, xcb_window_t win, char *buf, int bufsz)
{
    /* Try _NET_WM_NAME first (UTF-8), fall back to WM_NAME */
    xcb_get_property_cookie_t c = xcb_get_property(conn, 0, win, g_atoms.net_wm_name,
                                                     g_atoms.utf8_string, 0, 128);
    xcb_get_property_reply_t *r = xcb_get_property_reply(conn, c, NULL);
    if (r && r->value_len > 0) {
        int len = xcb_get_property_value_length(r);
        if (len >= bufsz) len = bufsz - 1;
        memcpy(buf, xcb_get_property_value(r), (size_t)len);
        buf[len] = '\0';
        free(r);
        return len;
    }
    free(r);

    c = xcb_get_property(conn, 0, win, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 0, 128);
    r = xcb_get_property_reply(conn, c, NULL);
    if (r && r->value_len > 0) {
        int len = xcb_get_property_value_length(r);
        if (len >= bufsz) len = bufsz - 1;
        memcpy(buf, xcb_get_property_value(r), (size_t)len);
        buf[len] = '\0';
        free(r);
        return len;
    }
    free(r);
    strncpy(buf, "(unknown)", (size_t)bufsz - 1);
    return -1;
}

/* ─── IPC helpers ────────────────────────────────────────────────────────── */

static void ipc_send(const char *cmd)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    const char *path = getenv("NEX_SOCKET");
    strncpy(addr.sun_path, path ? path : NEX_PANEL_SOCKET, sizeof(addr.sun_path) - 1);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
        send(fd, cmd, strlen(cmd), 0);
    }
    close(fd);
}

static void ewmh_focus(xcb_connection_t *conn, xcb_window_t root, xcb_window_t win)
{
    xcb_client_message_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.response_type  = XCB_CLIENT_MESSAGE;
    ev.format         = 32;
    ev.window         = win;
    ev.type           = g_atoms.net_active_window;
    ev.data.data32[0] = 2;  /* source: pager */
    ev.data.data32[1] = XCB_CURRENT_TIME;
    ev.data.data32[2] = XCB_NONE;
    xcb_send_event(conn, 0, root,
                   XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                   (const char *)&ev);
    xcb_flush(conn);
}

/* ─── drawing ─────────────────────────────────────────────────────────────── */

static xcb_gc_t make_gc(xcb_connection_t *conn, xcb_window_t win, uint32_t fg, uint32_t bg)
{
    xcb_gc_t gc = xcb_generate_id(conn);
    uint32_t mask = XCB_GC_FOREGROUND | XCB_GC_BACKGROUND | XCB_GC_GRAPHICS_EXPOSURES;
    uint32_t vals[3] = { fg, bg, 0 };
    xcb_create_gc(conn, gc, win, mask, vals);
    return gc;
}

static void fill_rect(xcb_connection_t *conn, xcb_drawable_t win,
                      xcb_gc_t gc, int x, int y, int w, int h)
{
    xcb_rectangle_t r = { (int16_t)x, (int16_t)y, (uint16_t)w, (uint16_t)h };
    xcb_poly_fill_rectangle(conn, win, gc, 1, &r);
}

static void draw_text(xcb_connection_t *conn, xcb_drawable_t win,
                      xcb_gc_t gc, int x, int y, const char *text)
{
    xcb_image_text_8(conn, (uint8_t)strlen(text), win, gc, (int16_t)x, (int16_t)y, text);
}

/* Truncate text to fit within max_chars */
static void truncate_text(const char *src, char *dst, int max_chars)
{
    int len = (int)strlen(src);
    if (len <= max_chars) {
        strcpy(dst, src);
    } else {
        memcpy(dst, src, (size_t)(max_chars - 1));
        dst[max_chars - 1] = '\0';
    }
}

/* ─── strut / dock setup ─────────────────────────────────────────────────── */

static void set_strut(xcb_connection_t *conn, xcb_window_t win,
                      int screen_w, int panel_h, int position)
{
    /* _NET_WM_STRUT: left, right, top, bottom */
    uint32_t strut[4] = { 0, 0, 0, 0 };
    if (position == 0) strut[2] = (uint32_t)panel_h;  /* top */
    else               strut[3] = (uint32_t)panel_h;  /* bottom */
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, win,
                        g_atoms.net_wm_strut, XCB_ATOM_CARDINAL, 32, 4, strut);

    /* _NET_WM_STRUT_PARTIAL: l, r, top, bot, l_s, l_e, r_s, r_e, t_s, t_e, b_s, b_e */
    uint32_t partial[12] = { 0 };
    if (position == 0) {
        partial[2] = (uint32_t)panel_h;
        partial[8] = 0;
        partial[9] = (uint32_t)screen_w;
    } else {
        partial[3] = (uint32_t)panel_h;
        partial[10] = 0;
        partial[11] = (uint32_t)screen_w;
    }
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, win,
                        g_atoms.net_wm_strut_partial, XCB_ATOM_CARDINAL, 32, 12, partial);

    /* Window type: dock */
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, win,
                        g_atoms.net_wm_window_type, XCB_ATOM_ATOM, 32, 1,
                        &g_atoms.net_wm_window_type_dock);

    /* Always on top */
    xcb_atom_t state_atoms[1] = { g_atoms.net_wm_state_above };
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, win,
                        g_atoms.net_wm_state, XCB_ATOM_ATOM, 32, 1, state_atoms);
}

/* ─── panel state ────────────────────────────────────────────────────────── */

#define MAX_CLIENTS 128

typedef struct {
    xcb_window_t wins[MAX_CLIENTS];
    int          count;
    xcb_window_t active;
    int          current_ws;
    int          ws_count;
    char         titles[MAX_CLIENTS][128];
    /* click regions */
    int          ws_btn_w;
    int          task_x;
    int          task_btn_w;
} panel_state_t;

static void update_state(xcb_connection_t *conn, xcb_window_t root, panel_state_t *s)
{
    s->count      = get_windows(conn, root, g_atoms.net_client_list, s->wins, MAX_CLIENTS);
    s->active     = (xcb_window_t)get_cardinal(conn, root, g_atoms.net_active_window);
    s->current_ws = (int)get_cardinal(conn, root, g_atoms.net_current_desktop);
    s->ws_count   = (int)get_cardinal(conn, root, g_atoms.net_number_of_desktops);
    if (s->ws_count <= 0) s->ws_count = 1;
    for (int i = 0; i < s->count; i++) {
        get_wm_name(conn, s->wins[i], s->titles[i], 128);
    }
}

/* ─── render ──────────────────────────────────────────────────────────────── */

static void render(xcb_connection_t *conn, xcb_window_t win,
                   int width, int height, const panel_state_t *s)
{
    /* Background */
    xcb_gc_t gc_bg = make_gc(conn, win, CLR_BG, CLR_BG);
    fill_rect(conn, win, gc_bg, 0, 0, width, height);
    xcb_free_gc(conn, gc_bg);

    int x = 4;
    int text_y = height / 2 + 5;  /* vertical baseline for 8px font */

    /* ── Launcher button "⊞" ─────────────────────────── */
    xcb_gc_t gc_ws_act = make_gc(conn, win, CLR_FG, CLR_WS_ACT);
    xcb_gc_t gc_ws_in  = make_gc(conn, win, CLR_FG, CLR_WS_INACT);
    fill_rect(conn, win, gc_ws_act, x, 2, 26, height - 4);
    draw_text(conn, win, gc_ws_act, x + 6, text_y, "WM");
    x += 30;

    /* ── Workspace pager ─────────────────────────────── */
    int ws_btn_w = 24;
    for (int i = 0; i < s->ws_count; i++) {
        xcb_gc_t gc = (i == s->current_ws) ? gc_ws_act : gc_ws_in;
        fill_rect(conn, win, gc, x, 2, ws_btn_w, height - 4);
        char label[16];
        snprintf(label, sizeof(label), "%d", i + 1);
        draw_text(conn, win, gc, x + (ws_btn_w - 6) / 2, text_y, label);
        x += ws_btn_w + 2;
    }
    xcb_free_gc(conn, gc_ws_act);
    xcb_free_gc(conn, gc_ws_in);

    /* separator */
    xcb_gc_t gc_sep = make_gc(conn, win, CLR_SEP, CLR_BG);
    fill_rect(conn, win, gc_sep, x, 4, 1, height - 8);
    xcb_free_gc(conn, gc_sep);
    x += 6;

    /* ── Taskbar ─────────────────────────────────────── */
    /* Reserve right zone width (buttons + clock ~160px) */
    int right_zone_w = 160;
    int taskbar_w    = width - x - right_zone_w;
    int task_btn_w   = (s->count > 0) ? (taskbar_w / s->count) : 0;
    if (task_btn_w > 200) task_btn_w = 200;
    if (task_btn_w < 60)  task_btn_w = 60;

    for (int i = 0; i < s->count && x < width - right_zone_w; i++) {
        int is_active = (s->wins[i] == s->active);
        uint32_t bg = is_active ? CLR_WIN_ACT : CLR_WIN_BG;
        uint32_t fg = CLR_FG;
        xcb_gc_t gc_t = make_gc(conn, win, fg, bg);
        fill_rect(conn, win, gc_t, x + 1, 3, task_btn_w - 2, height - 6);
        char label[32];
        truncate_text(s->titles[i], label, (int)sizeof(label) - 1);
        draw_text(conn, win, gc_t, x + 4, text_y, label);
        xcb_free_gc(conn, gc_t);
        x += task_btn_w;
    }

    /* ── Right zone: Max & Full buttons, clock ───────── */
    int ctrl_btn_w = 36;
    int max_btn_x  = width - 156;
    int full_btn_x = width - 116;
    int clock_x    = width - 70;

    xcb_gc_t gc_ctrl = make_gc(conn, win, CLR_FG, CLR_WS_INACT);
    fill_rect(conn, win, gc_ctrl, max_btn_x, 2, ctrl_btn_w, height - 4);
    draw_text(conn, win, gc_ctrl, max_btn_x + 6, text_y, "Max");

    fill_rect(conn, win, gc_ctrl, full_btn_x, 2, ctrl_btn_w, height - 4);
    draw_text(conn, win, gc_ctrl, full_btn_x + 6, text_y, "Full");
    xcb_free_gc(conn, gc_ctrl);

    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char clock_str[16];
    strftime(clock_str, sizeof(clock_str), "%H:%M", t);

    xcb_gc_t gc_fg = make_gc(conn, win, CLR_FG, CLR_BG);
    draw_text(conn, win, gc_fg, clock_x, text_y, clock_str);
    xcb_free_gc(conn, gc_fg);

    xcb_flush(conn);
}

/* ─── click handling ─────────────────────────────────────────────────────── */

static void handle_click(xcb_connection_t *conn, xcb_window_t root,
                         panel_state_t *s, int click_x, int panel_w)
{
    int max_btn_x  = panel_w - 156;
    int full_btn_x = panel_w - 116;
    int ctrl_btn_w = 36;

    if (click_x >= max_btn_x && click_x < max_btn_x + ctrl_btn_w) {
        if (s->active) {
            char cmd[48];
            snprintf(cmd, sizeof(cmd), "maximize %u", (unsigned)s->active);
            ipc_send(cmd);
        } else {
            ipc_send("maximize");
        }
        return;
    }

    if (click_x >= full_btn_x && click_x < full_btn_x + ctrl_btn_w) {
        if (s->active) {
            char cmd[48];
            snprintf(cmd, sizeof(cmd), "fullscreen %u", (unsigned)s->active);
            ipc_send(cmd);
        } else {
            ipc_send("fullscreen");
        }
        return;
    }

    int x = 4;

    /* Launcher button */
    if (click_x < x + 26) {
        /* Spawn nex-launcher */
        pid_t pid = fork();
        if (pid == 0) {
            setsid();
            execlp("nex-launcher", "nex-launcher", NULL);
            _exit(1);
        }
        return;
    }
    x += 30;

    /* Workspace buttons */
    int ws_btn_w = 24;
    for (int i = 0; i < s->ws_count; i++) {
        if (click_x >= x && click_x < x + ws_btn_w) {
            char cmd[32];
            snprintf(cmd, sizeof(cmd), "workspace %d", i + 1);
            ipc_send(cmd);
            return;
        }
        x += ws_btn_w + 2;
    }
    x += 7;  /* separator + gap */

    /* Taskbar buttons */
    int right_zone_w = 160;
    int taskbar_w    = panel_w - x - right_zone_w;
    int task_btn_w   = (s->count > 0) ? (taskbar_w / s->count) : 0;
    if (task_btn_w > 200) task_btn_w = 200;
    if (task_btn_w < 60)  task_btn_w = 60;

    for (int i = 0; i < s->count; i++) {
        if (click_x >= x && click_x < x + task_btn_w) {
            if (s->wins[i] == s->active) {
                /* Already focused → minimize */
                char cmd[48];
                snprintf(cmd, sizeof(cmd), "minimize %u", (unsigned)s->wins[i]);
                ipc_send(cmd);
            } else {
                /* Focus via EWMH ClientMessage */
                ewmh_focus(conn, root, s->wins[i]);
            }
            return;
        }
        x += task_btn_w;
    }
    (void)panel_w;
}

/* ─── public API ──────────────────────────────────────────────────────────── */

int nex_panel_init(nex_panel_ctx_t *ctx)
{
    ctx->conn = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(ctx->conn)) {
        fprintf(stderr, "nex-panel: cannot connect to X server\n");
        return -1;
    }

    const xcb_setup_t    *setup  = xcb_get_setup(ctx->conn);
    xcb_screen_iterator_t iter   = xcb_setup_roots_iterator(setup);
    xcb_screen_t         *screen = iter.data;
    ctx->screen  = screen;
    ctx->width   = screen->width_in_pixels;
    ctx->height  = NEX_PANEL_HEIGHT;
    ctx->running = 1;

    init_atoms(ctx->conn);

    /* Panel Y position */
    int y = (NEX_PANEL_POSITION == 0) ? 0
                                       : (int)screen->height_in_pixels - NEX_PANEL_HEIGHT;

    uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK;
    uint32_t vals[3] = {
        CLR_BG,
        1,  /* override_redirect: skip WM management */
        XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_BUTTON_PRESS |
        XCB_EVENT_MASK_PROPERTY_CHANGE
    };

    ctx->win = xcb_generate_id(ctx->conn);
    xcb_create_window(ctx->conn, XCB_COPY_FROM_PARENT,
                      ctx->win, screen->root,
                      0, (int16_t)y, (uint16_t)ctx->width, (uint16_t)ctx->height, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      screen->root_visual, mask, vals);

    /* Window class / name */
    xcb_change_property(ctx->conn, XCB_PROP_MODE_REPLACE, ctx->win,
                        XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8,
                        strlen("nex-panel"), "nex-panel");

    set_strut(ctx->conn, ctx->win, ctx->width, ctx->height, NEX_PANEL_POSITION);

    /* Subscribe to root window property changes to track WM state */
    uint32_t root_mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
    xcb_change_window_attributes(ctx->conn, screen->root, XCB_CW_EVENT_MASK, &root_mask);

    xcb_map_window(ctx->conn, ctx->win);
    xcb_flush(ctx->conn);
    return 0;
}

void nex_panel_run(nex_panel_ctx_t *ctx)
{
    xcb_window_t root = ctx->screen->root;
    panel_state_t state;
    memset(&state, 0, sizeof(state));
    update_state(ctx->conn, root, &state);
    render(ctx->conn, ctx->win, ctx->width, ctx->height, &state);

    int xcb_fd = xcb_get_file_descriptor(ctx->conn);
    struct pollfd fds[1] = {{ xcb_fd, POLLIN, 0 }};

    while (ctx->running) {
        /* Poll with 1-second timeout to refresh clock */
        int ready = poll(fds, 1, 1000);
        if (ready < 0 && errno != EINTR) break;

        int need_redraw = 0;
        xcb_generic_event_t *ev;
        while ((ev = xcb_poll_for_event(ctx->conn)) != NULL) {
            uint8_t type = ev->response_type & ~0x80;
            switch (type) {
            case XCB_EXPOSE:
                need_redraw = 1;
                break;
            case XCB_BUTTON_PRESS: {
                xcb_button_press_event_t *bp = (xcb_button_press_event_t *)ev;
                if (bp->event == ctx->win && bp->detail == XCB_BUTTON_INDEX_1) {
                    handle_click(ctx->conn, root, &state, bp->event_x, ctx->width);
                    update_state(ctx->conn, root, &state);
                    need_redraw = 1;
                }
                break;
            }
            case XCB_PROPERTY_NOTIFY: {
                xcb_property_notify_event_t *pn = (xcb_property_notify_event_t *)ev;
                if (pn->atom == g_atoms.net_client_list  ||
                    pn->atom == g_atoms.net_active_window ||
                    pn->atom == g_atoms.net_current_desktop) {
                    update_state(ctx->conn, root, &state);
                    need_redraw = 1;
                }
                break;
            }
            default: break;
            }
            free(ev);
        }

        /* Refresh clock every poll tick regardless */
        need_redraw = 1;
        if (need_redraw) render(ctx->conn, ctx->win, ctx->width, ctx->height, &state);

        if (xcb_connection_has_error(ctx->conn)) break;
    }
}

void nex_panel_cleanup(nex_panel_ctx_t *ctx)
{
    if (ctx->conn) {
        xcb_destroy_window(ctx->conn, ctx->win);
        xcb_disconnect(ctx->conn);
        ctx->conn = NULL;
    }
}

/* ─── entry point ─────────────────────────────────────────────────────────── */

int main(void)
{
    nex_panel_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    if (nex_panel_init(&ctx) < 0) return 1;
    nex_panel_run(&ctx);
    nex_panel_cleanup(&ctx);
    return 0;
}
