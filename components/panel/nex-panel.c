#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

/*
 * nex-panel.c - NexPanel
 * Status bar / taskbar / workspace pager for NexWM / Hyggshi OS
 *
 * Visual layout (left → right):
 *
 *  [⊞ Nex]  [⏍Files] [⚙Sett] [▣Term]  │  [①][②][③]  │  taskbar …  │  🔊 HH:MM  [⏻]
 *   start     quick-launchers   sep       workspace pager   window list   right zone
 */

#include "nex-panel.h"
#include "nex_icon.h"

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

/* ═══════════════════════════════════════════════════════════════════════════
 * ATOMS
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    xcb_atom_t net_client_list;
    xcb_atom_t net_active_window;
    xcb_atom_t net_current_desktop;
    xcb_atom_t net_number_of_desktops;
    xcb_atom_t net_wm_name;
    xcb_atom_t net_wm_desktop;
    xcb_atom_t net_wm_strut;
    xcb_atom_t net_wm_strut_partial;
    xcb_atom_t net_wm_window_type;
    xcb_atom_t net_wm_window_type_dock;
    xcb_atom_t net_wm_state;
    xcb_atom_t net_wm_state_above;
    xcb_atom_t net_wm_state_skip_taskbar;
    xcb_atom_t utf8_string;
} panel_atoms_t;

static panel_atoms_t g_atoms;

static xcb_atom_t intern_atom(xcb_connection_t *conn, const char *name)
{
    xcb_intern_atom_cookie_t c =
        xcb_intern_atom(conn, 0, (uint16_t)strlen(name), name);
    xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(conn, c, NULL);
    xcb_atom_t a = r ? r->atom : XCB_ATOM_NONE;
    free(r);
    return a;
}

static void init_atoms(xcb_connection_t *conn)
{
    g_atoms.net_client_list         = intern_atom(conn, "_NET_CLIENT_LIST");
    g_atoms.net_active_window       = intern_atom(conn, "_NET_ACTIVE_WINDOW");
    g_atoms.net_current_desktop     = intern_atom(conn, "_NET_CURRENT_DESKTOP");
    g_atoms.net_number_of_desktops  = intern_atom(conn, "_NET_NUMBER_OF_DESKTOPS");
    g_atoms.net_wm_name             = intern_atom(conn, "_NET_WM_NAME");
    g_atoms.net_wm_desktop          = intern_atom(conn, "_NET_WM_DESKTOP");
    g_atoms.net_wm_strut            = intern_atom(conn, "_NET_WM_STRUT");
    g_atoms.net_wm_strut_partial    = intern_atom(conn, "_NET_WM_STRUT_PARTIAL");
    g_atoms.net_wm_window_type      = intern_atom(conn, "_NET_WM_WINDOW_TYPE");
    g_atoms.net_wm_window_type_dock = intern_atom(conn, "_NET_WM_WINDOW_TYPE_DOCK");
    g_atoms.net_wm_state            = intern_atom(conn, "_NET_WM_STATE");
    g_atoms.net_wm_state_above      = intern_atom(conn, "_NET_WM_STATE_ABOVE");
    g_atoms.net_wm_state_skip_taskbar = intern_atom(conn, "_NET_WM_STATE_SKIP_TASKBAR");
    g_atoms.utf8_string             = intern_atom(conn, "UTF8_STRING");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * ICON CACHE
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef enum {
    ICON_FILES = 0,
    ICON_SETTINGS,
    ICON_TERMINAL,
    ICON_POWER,
    ICON_VOLUME,
    ICON__COUNT
} panel_icon_id_t;

static const char *ICON_NAMES[ICON__COUNT] = {
    "system-file-manager",
    "preferences-system",
    "utilities-terminal",
    "system-shutdown",
    "audio-speakers",
};

static nex_icon_t *g_icons[ICON__COUNT];

static void icons_load(void)
{
    for (int i = 0; i < ICON__COUNT; i++) {
        g_icons[i] = nex_icon_load(ICON_NAMES[i], NEX_PANEL_ICON_SIZE);
    }
}

static void icons_free(void)
{
    for (int i = 0; i < ICON__COUNT; i++) {
        nex_icon_free(g_icons[i]);
        g_icons[i] = NULL;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * PROPERTY HELPERS
 * ═══════════════════════════════════════════════════════════════════════════ */

static uint32_t get_cardinal(xcb_connection_t *conn, xcb_window_t win,
                              xcb_atom_t prop)
{
    xcb_get_property_cookie_t c =
        xcb_get_property(conn, 0, win, prop, XCB_ATOM_CARDINAL, 0, 1);
    xcb_get_property_reply_t *r = xcb_get_property_reply(conn, c, NULL);
    uint32_t val = 0;
    if (r && r->value_len) val = *(uint32_t *)xcb_get_property_value(r);
    free(r);
    return val;
}

static int get_windows(xcb_connection_t *conn, xcb_window_t root,
                       xcb_atom_t prop, xcb_window_t *out, int max)
{
    xcb_get_property_cookie_t c =
        xcb_get_property(conn, 0, root, prop, XCB_ATOM_WINDOW, 0, (uint32_t)max);
    xcb_get_property_reply_t *r = xcb_get_property_reply(conn, c, NULL);
    if (!r) return 0;
    int n = xcb_get_property_value_length(r) / (int)sizeof(xcb_window_t);
    if (n > max) n = max;
    memcpy(out, xcb_get_property_value(r), (size_t)n * sizeof(xcb_window_t));
    free(r);
    return n;
}

static int get_wm_name(xcb_connection_t *conn, xcb_window_t win,
                       char *buf, int bufsz)
{
    xcb_get_property_cookie_t c =
        xcb_get_property(conn, 0, win, g_atoms.net_wm_name,
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
    c = xcb_get_property(conn, 0, win, XCB_ATOM_WM_NAME,
                         XCB_ATOM_STRING, 0, 128);
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

static int window_skip_taskbar(xcb_connection_t *conn, xcb_window_t win)
{
    xcb_get_property_cookie_t c =
        xcb_get_property(conn, 0, win, g_atoms.net_wm_state,
                         XCB_ATOM_ATOM, 0, 32);
    xcb_get_property_reply_t *r = xcb_get_property_reply(conn, c, NULL);
    if (!r) return 0;
    int n = xcb_get_property_value_length(r) / (int)sizeof(xcb_atom_t);
    xcb_atom_t *atoms = (xcb_atom_t *)xcb_get_property_value(r);
    for (int i = 0; i < n; i++) {
        if (atoms[i] == g_atoms.net_wm_state_skip_taskbar) {
            free(r);
            return 1;
        }
    }
    free(r);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * IPC
 * ═══════════════════════════════════════════════════════════════════════════ */

static void ipc_send(const char *cmd)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    const char *path = getenv("NEX_SOCKET");
    strncpy(addr.sun_path, path ? path : NEX_PANEL_SOCKET,
            sizeof(addr.sun_path) - 1);
    addr.sun_path[sizeof(addr.sun_path) - 1] = '\0';
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0)
        send(fd, cmd, strlen(cmd), 0);
    close(fd);
}

static void ewmh_activate(xcb_connection_t *conn, xcb_window_t root,
                           xcb_window_t win)
{
    xcb_client_message_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.response_type  = XCB_CLIENT_MESSAGE;
    ev.format         = 32;
    ev.window         = win;
    ev.type           = g_atoms.net_active_window;
    ev.data.data32[0] = 2;
    ev.data.data32[1] = XCB_CURRENT_TIME;
    xcb_send_event(conn, 0, root,
                   XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT |
                   XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                   (const char *)&ev);
    xcb_flush(conn);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * DRAWING UTILITIES
 * ═══════════════════════════════════════════════════════════════════════════ */

static xcb_gc_t make_gc(xcb_connection_t *conn, xcb_window_t win,
                         uint32_t fg, uint32_t bg)
{
    xcb_gc_t gc = xcb_generate_id(conn);
    uint32_t mask = XCB_GC_FOREGROUND | XCB_GC_BACKGROUND |
                    XCB_GC_GRAPHICS_EXPOSURES;
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

static void fill_rounded_rect(xcb_connection_t *conn, xcb_drawable_t win,
                               xcb_gc_t gc, int x, int y, int w, int h, int r)
{
    if (r <= 0 || w <= 0 || h <= 0) {
        fill_rect(conn, win, gc, x, y, w, h);
        return;
    }
    if (r > w/2) r = w/2;
    if (r > h/2) r = h/2;

    fill_rect(conn, win, gc, x + r, y,     w - 2*r, h);
    fill_rect(conn, win, gc, x,     y + r, r,       h - 2*r);
    fill_rect(conn, win, gc, x+w-r, y + r, r,       h - 2*r);
    fill_rect(conn, win, gc, x + r/2, y,   r - r/2, r);
    fill_rect(conn, win, gc, x,       y + r/2, r, r - r/2);
    fill_rect(conn, win, gc, x+w-r, y,       r - r/2, r);
    fill_rect(conn, win, gc, x+w-r+r/2, y + r/2, r - r/2, r - r/2);
    fill_rect(conn, win, gc, x + r/2, y+h-r, r - r/2, r);
    fill_rect(conn, win, gc, x, y+h-r+r/2-1, r, r - r/2 + 1);
    fill_rect(conn, win, gc, x+w-r, y+h-r, r - r/2, r);
    fill_rect(conn, win, gc, x+w-r+r/2, y+h-r+r/2-1, r - r/2, r - r/2 + 1);
}

static void draw_button_shape(xcb_connection_t *conn, xcb_window_t win,
                              int x, int y, int w, int h, int r, uint32_t fill_color)
{
    xcb_gc_t gc = make_gc(conn, win, fill_color, fill_color);
    fill_rounded_rect(conn, win, gc, x, y, w, h, r);
    xcb_free_gc(conn, gc);
}

static void draw_button_text(xcb_connection_t *conn, xcb_window_t win,
                             int x, int y, const char *text,
                             uint32_t fg_color, uint32_t bg_color)
{
    xcb_gc_t gc = make_gc(conn, win, fg_color, bg_color);
    size_t len = strlen(text);
    if (len > 255) len = 255;
    xcb_image_text_8(conn, (uint8_t)len, win, gc, (int16_t)x, (int16_t)y, text);
    xcb_free_gc(conn, gc);
}

static void draw_icon_or_placeholder(xcb_connection_t *conn,
                                     xcb_drawable_t    win,
                                     panel_icon_id_t   id,
                                     int x, int y, int sz,
                                     uint32_t bg_rgb)
{
    if (id >= 0 && id < ICON__COUNT && g_icons[id]) {
        xcb_gc_t gc = make_gc(conn, win, 0xffffff, bg_rgb);
        nex_icon_draw(conn, win, gc, g_icons[id], x, y, bg_rgb);
        xcb_free_gc(conn, gc);
        return;
    }

    static const uint32_t FALLBACK_COLORS[ICON__COUNT] = {
        0x5b8dd9, 0xf38ba8, 0xa6e3a1, 0xe78284, 0x89dceb,
    };
    uint32_t col = (id >= 0 && id < ICON__COUNT) ? FALLBACK_COLORS[id] : 0x888888;
    draw_button_shape(conn, win, x, y, sz, sz, 4, col);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * STRUT / DOCK SETUP
 * ═══════════════════════════════════════════════════════════════════════════ */

static void set_strut(xcb_connection_t *conn, xcb_window_t win,
                      int screen_w, int panel_h, int position)
{
    uint32_t strut[4] = { 0, 0, 0, 0 };
    if (position == 0) strut[2] = (uint32_t)panel_h;
    else               strut[3] = (uint32_t)panel_h;
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, win,
                        g_atoms.net_wm_strut, XCB_ATOM_CARDINAL, 32, 4, strut);

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
                        g_atoms.net_wm_strut_partial,
                        XCB_ATOM_CARDINAL, 32, 12, partial);

    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, win,
                        g_atoms.net_wm_window_type, XCB_ATOM_ATOM, 32, 1,
                        &g_atoms.net_wm_window_type_dock);

    xcb_atom_t state_atoms[1] = { g_atoms.net_wm_state_above };
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, win,
                        g_atoms.net_wm_state, XCB_ATOM_ATOM, 32, 1, state_atoms);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * PANEL STATE
 * ═══════════════════════════════════════════════════════════════════════════ */

#define MAX_CLIENTS 128

typedef struct {
    xcb_window_t wins[MAX_CLIENTS];
    int          count;
    xcb_window_t active;
    int          current_ws;
    int          ws_count;
    char         titles[MAX_CLIENTS][96];
} panel_state_t;

static void update_state(xcb_connection_t *conn, xcb_window_t root,
                         panel_state_t *s)
{
    xcb_window_t all[MAX_CLIENTS];
    int n = get_windows(conn, root, g_atoms.net_client_list, all, MAX_CLIENTS);

    s->count = 0;
    for (int i = 0; i < n; i++) {
        if (!window_skip_taskbar(conn, all[i]))
            s->wins[s->count++] = all[i];
    }

    s->active     = (xcb_window_t)get_cardinal(conn, root, g_atoms.net_active_window);
    s->current_ws = (int)get_cardinal(conn, root, g_atoms.net_current_desktop);
    s->ws_count   = (int)get_cardinal(conn, root, g_atoms.net_number_of_desktops);
    if (s->ws_count <= 0) s->ws_count = 1;

    for (int i = 0; i < s->count; i++)
        get_wm_name(conn, s->wins[i], s->titles[i], 96);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * LAYOUT CONSTANTS
 * ═══════════════════════════════════════════════════════════════════════════ */

#define PANEL_PAD           5
#define PANEL_TEXT_Y(H)     ((H) / 2 + 5)
#define PANEL_ICON_Y(H)     (((H) - NEX_PANEL_ICON_SIZE) / 2)

#define START_W             64
#define START_CORNER        6

#define QLNCH_ICON_W        (NEX_PANEL_ICON_SIZE + 4)
#define QLNCH_LABEL_CHARS   5
#define QLNCH_CHAR_W        7
#define QLNCH_LABEL_W       (QLNCH_LABEL_CHARS * QLNCH_CHAR_W)
#define QLNCH_W             (QLNCH_ICON_W + QLNCH_LABEL_W + 4)
#define QLNCH_CORNER        5
#define QLNCH_GAP           4

#define SEP_W               1
#define SEP_PAD             6

#define WS_BTN_W            26
#define WS_BTN_GAP          2
#define WS_CORNER           5

#define TASK_MAX_W          180
#define TASK_MIN_W          60
#define TASK_ACCENT_H       3
#define TASK_CORNER         4

#define RZ_POWER_W          32
#define RZ_POWER_CORNER     5
#define RZ_CLOCK_W          110
#define RZ_VOL_W            (NEX_PANEL_ICON_SIZE + 4)
#define RZ_TOTAL            (RZ_VOL_W + 4 + RZ_CLOCK_W + 4 + RZ_POWER_W + PANEL_PAD)

static void trunc_str(const char *src, char *dst, int max_chars)
{
    if (!src || !dst || max_chars <= 0) {
        if (dst && max_chars > 0) dst[0] = '\0';
        return;
    }
    int len = (int)strlen(src);
    if (len < max_chars) {
        memcpy(dst, src, (size_t)len + 1);
    } else {
        memcpy(dst, src, (size_t)(max_chars - 1));
        dst[max_chars - 1] = '\0';
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * RENDER
 * ═══════════════════════════════════════════════════════════════════════════ */

static void render(xcb_connection_t *conn, xcb_window_t win,
                   int width, int height, const panel_state_t *s)
{
    int text_y = PANEL_TEXT_Y(height);
    int icon_y = PANEL_ICON_Y(height);
    int btn_y  = 4;
    int btn_h  = height - 8;
    uint32_t bg = NEX_PANEL_BG;

    /* ── Clear background ── */
    draw_button_shape(conn, win, 0, 0, width, height, 0, bg);

    /* ── Bottom border accent ── */
    draw_button_shape(conn, win, 0, height - 1, width, 1, 0, NEX_PANEL_BORDER_BOT);

    int x = PANEL_PAD;

    /* ── 1. Start button ── */
    {
        draw_button_shape(conn, win, x, btn_y, START_W, btn_h, START_CORNER, NEX_PANEL_START_BG);
        draw_button_text(conn, win, x + (START_W - 21)/2, text_y, "Nex",
                         NEX_PANEL_START_FG, NEX_PANEL_START_BG);
        x += START_W + PANEL_PAD;
    }

    /* ── 2. Quick-launch buttons ── */
    typedef struct { panel_icon_id_t id; const char *label; } ql_t;
    static const ql_t QL[] = {
        { ICON_FILES,    "Files" },
        { ICON_SETTINGS, "Sett"  },
        { ICON_TERMINAL, "Term"  },
    };
    for (int i = 0; i < 3; i++) {
        draw_button_shape(conn, win, x, btn_y, QLNCH_W, btn_h, QLNCH_CORNER, NEX_PANEL_QLNCH_BG);
        draw_icon_or_placeholder(conn, win, QL[i].id, x + 4, icon_y, NEX_PANEL_ICON_SIZE, NEX_PANEL_QLNCH_BG);
        draw_button_text(conn, win, x + QLNCH_ICON_W + 2, text_y, QL[i].label,
                         NEX_PANEL_FG, NEX_PANEL_QLNCH_BG);
        x += QLNCH_W + QLNCH_GAP;
    }

    /* ── Separator ── */
    {
        draw_button_shape(conn, win, x + SEP_PAD, btn_y + 3, SEP_W, btn_h - 6, 0, NEX_PANEL_SEP);
        x += SEP_W + SEP_PAD * 2;
    }

    /* ── 3. Workspace pager ── */
    for (int i = 0; i < s->ws_count; i++) {
        int active = (i == s->current_ws);
        uint32_t wbg = active ? NEX_PANEL_WS_ACTIVE   : NEX_PANEL_WS_INACTIVE;
        uint32_t wfg = active ? NEX_PANEL_WS_FG_ACT   : NEX_PANEL_WS_FG_INACT;

        draw_button_shape(conn, win, x, btn_y, WS_BTN_W, btn_h, WS_CORNER, wbg);

        int ws_num = (i + 1 > 99) ? 99 : (i + 1);
        char lbl[8];
        snprintf(lbl, sizeof(lbl), "%d", ws_num);
        int lx = x + (WS_BTN_W - (int)strlen(lbl) * QLNCH_CHAR_W) / 2;
        draw_button_text(conn, win, lx, text_y, lbl, wfg, wbg);
        x += WS_BTN_W + WS_BTN_GAP;
    }
    x += SEP_PAD;

    /* ── 4. Taskbar ── */
    int taskbar_right = width - RZ_TOTAL;
    int taskbar_avail = taskbar_right - x;
    int task_w = (s->count > 0) ? (taskbar_avail / s->count) : 0;
    if (task_w > TASK_MAX_W) task_w = TASK_MAX_W;
    if (task_w < TASK_MIN_W) task_w = TASK_MIN_W;

    for (int i = 0; i < s->count && x + task_w <= taskbar_right; i++) {
        int active = (s->wins[i] == s->active);
        uint32_t tbg = active ? NEX_PANEL_TASK_ACTIVE : NEX_PANEL_TASK_BG;

        draw_button_shape(conn, win, x + 1, btn_y, task_w - 2, btn_h, TASK_CORNER, tbg);

        if (active) {
            draw_button_shape(conn, win, x + 5, btn_y + btn_h - TASK_ACCENT_H,
                              task_w - 12, TASK_ACCENT_H, 0, NEX_PANEL_TASK_ACCENT);
        }

        char title[24];
        trunc_str(s->titles[i], title, (int)sizeof(title) - 1);
        draw_button_text(conn, win, x + 7, text_y, title, NEX_PANEL_FG, tbg);
        x += task_w;
    }

    /* ── 5. Right zone ── */
    int rx = width - RZ_TOTAL + PANEL_PAD;

    /* Volume icon */
    {
        draw_icon_or_placeholder(conn, win, ICON_VOLUME, rx, icon_y, NEX_PANEL_ICON_SIZE, bg);
        rx += RZ_VOL_W + 4;
    }

    /* Clock */
    {
        time_t now = time(NULL);
        struct tm *t = localtime(&now);
        char clock_str[24];
        strftime(clock_str, sizeof(clock_str), "%a %d %b %H:%M", t);
        draw_button_text(conn, win, rx, text_y, clock_str, NEX_PANEL_FG, bg);
        rx += RZ_CLOCK_W + 4;
    }

    /* Power button */
    {
        draw_button_shape(conn, win, rx, btn_y, RZ_POWER_W, btn_h, RZ_POWER_CORNER, NEX_PANEL_POWER_BG);
        if (g_icons[ICON_POWER]) {
            int ix = rx + (RZ_POWER_W - NEX_PANEL_ICON_SIZE) / 2;
            xcb_gc_t gc_pw = make_gc(conn, win, NEX_PANEL_POWER_FG, NEX_PANEL_POWER_BG);
            nex_icon_draw(conn, win, gc_pw, g_icons[ICON_POWER], ix, icon_y, NEX_PANEL_POWER_BG);
            xcb_free_gc(conn, gc_pw);
        } else {
            draw_button_text(conn, win, rx + (RZ_POWER_W - 7)/2, text_y, "I",
                             NEX_PANEL_POWER_FG, NEX_PANEL_POWER_BG);
        }
    }

    xcb_flush(conn);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * CLICK HANDLING
 * ═══════════════════════════════════════════════════════════════════════════ */

static void spawn_app(const char *name, const char *path1, const char *path2)
{
    pid_t pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        setsid();
        for (int fd = 3; fd < 256; fd++) close(fd);
        execlp(name, name, (char *)NULL);
        if (path1) execl(path1, name, (char *)NULL);
        if (path2) execl(path2, name, (char *)NULL);
        _exit(1);
    }
    (void)waitpid(pid, NULL, WNOHANG);
}

static void handle_click(xcb_connection_t *conn, xcb_window_t root,
                          panel_state_t *s, int cx, int panel_w,
                          int panel_h, uint8_t button)
{
    (void)panel_h;
    int x = PANEL_PAD;

    /* 1. Start button */
    if (cx >= x && cx < x + START_W) {
        spawn_app("nex-launcher", "./bin/nex-launcher", "/usr/local/bin/nex-launcher");
        return;
    }
    x += START_W + PANEL_PAD;

    /* 2. Quick launchers */
    typedef struct { const char *name; const char *p1; const char *p2; } ql_app_t;
    static const ql_app_t QL_APPS[] = {
        { "nex-fm",       "./bin/nex-fm",       "/usr/local/bin/nex-fm"       },
        { "nex-settings", "./bin/nex-settings", "/usr/local/bin/nex-settings" },
        { "nex-terminal", "./bin/nex-terminal", "/usr/local/bin/nex-terminal" },
    };
    for (int i = 0; i < 3; i++) {
        if (cx >= x && cx < x + QLNCH_W) {
            spawn_app(QL_APPS[i].name, QL_APPS[i].p1, QL_APPS[i].p2);
            return;
        }
        x += QLNCH_W + QLNCH_GAP;
    }
    x += SEP_W + SEP_PAD * 2;

    /* 3. Workspace buttons */
    for (int i = 0; i < s->ws_count; i++) {
        if (cx >= x && cx < x + WS_BTN_W) {
            char cmd[32];
            snprintf(cmd, sizeof(cmd), "workspace %d", i + 1);
            ipc_send(cmd);
            return;
        }
        x += WS_BTN_W + WS_BTN_GAP;
    }
    x += SEP_PAD;

    /* 4. Right zone */
    int rx = panel_w - RZ_TOTAL + PANEL_PAD;
    rx += RZ_VOL_W + 4;
    rx += RZ_CLOCK_W + 4;

    /* Power button */
    if (cx >= rx && cx < rx + RZ_POWER_W) {
        spawn_app("nex-session", "./bin/nex-session", "/usr/local/bin/nex-session");
        return;
    }

    /* 4. Taskbar */
    int taskbar_right = panel_w - RZ_TOTAL;
    int taskbar_avail = taskbar_right - x;
    int task_w = (s->count > 0) ? (taskbar_avail / s->count) : 0;
    if (task_w > TASK_MAX_W) task_w = TASK_MAX_W;
    if (task_w < TASK_MIN_W) task_w = TASK_MIN_W;

    for (int i = 0; i < s->count && x + task_w <= taskbar_right; i++) {
        if (cx >= x && cx < x + task_w) {
            if (button == XCB_BUTTON_INDEX_2) {
                char cmd[48];
                snprintf(cmd, sizeof(cmd), "close %u", (unsigned)s->wins[i]);
                ipc_send(cmd);
            } else if (button == XCB_BUTTON_INDEX_3) {
                char cmd[48];
                snprintf(cmd, sizeof(cmd), "toggle-floating %u", (unsigned)s->wins[i]);
                ipc_send(cmd);
            } else {
                if (s->wins[i] == s->active) {
                    char cmd[48];
                    snprintf(cmd, sizeof(cmd), "minimize %u", (unsigned)s->wins[i]);
                    ipc_send(cmd);
                } else {
                    ewmh_activate(conn, root, s->wins[i]);
                }
            }
            return;
        }
        x += task_w;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * PUBLIC API
 * ═══════════════════════════════════════════════════════════════════════════ */

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
    ctx->width   = (int)screen->width_in_pixels;
    ctx->height  = NEX_PANEL_HEIGHT;
    ctx->running = 1;

    init_atoms(ctx->conn);
    icons_load();

    int y = (NEX_PANEL_POSITION == 0)
            ? 0
            : (int)screen->height_in_pixels - NEX_PANEL_HEIGHT;

    uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK;
    uint32_t vals[3] = {
        NEX_PANEL_BG,
        1,
        XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_BUTTON_PRESS |
        XCB_EVENT_MASK_PROPERTY_CHANGE
    };

    ctx->win = xcb_generate_id(ctx->conn);
    xcb_create_window(ctx->conn, XCB_COPY_FROM_PARENT,
                      ctx->win, screen->root,
                      0, (int16_t)y, (uint16_t)ctx->width,
                      (uint16_t)ctx->height, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      screen->root_visual, mask, vals);

    xcb_change_property(ctx->conn, XCB_PROP_MODE_REPLACE, ctx->win,
                        XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8,
                        9, "nex-panel");

    set_strut(ctx->conn, ctx->win, ctx->width, ctx->height, NEX_PANEL_POSITION);

    uint32_t root_mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
    xcb_change_window_attributes(ctx->conn, screen->root,
                                 XCB_CW_EVENT_MASK, &root_mask);

    xcb_map_window(ctx->conn, ctx->win);

    uint32_t raise_vals[1] = { XCB_STACK_MODE_ABOVE };
    xcb_configure_window(ctx->conn, ctx->win,
                         XCB_CONFIG_WINDOW_STACK_MODE, raise_vals);

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
                if (bp->event == ctx->win) {
                    handle_click(ctx->conn, root, &state,
                                 bp->event_x, ctx->width, ctx->height,
                                 bp->detail);
                    update_state(ctx->conn, root, &state);
                    need_redraw = 1;
                }
                break;
            }
            case XCB_PROPERTY_NOTIFY: {
                xcb_property_notify_event_t *pn =
                    (xcb_property_notify_event_t *)ev;
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

        if (need_redraw) {
            uint32_t raise_vals[1] = { XCB_STACK_MODE_ABOVE };
            xcb_configure_window(ctx->conn, ctx->win,
                                 XCB_CONFIG_WINDOW_STACK_MODE, raise_vals);
            render(ctx->conn, ctx->win, ctx->width, ctx->height, &state);
        }

        if (xcb_connection_has_error(ctx->conn)) break;
    }
}

void nex_panel_cleanup(nex_panel_ctx_t *ctx)
{
    icons_free();
    if (ctx->conn) {
        xcb_destroy_window(ctx->conn, ctx->win);
        xcb_disconnect(ctx->conn);
        ctx->conn = NULL;
    }
}

int main(void)
{
    nex_panel_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    if (nex_panel_init(&ctx) < 0) return 1;
    nex_panel_run(&ctx);
    nex_panel_cleanup(&ctx);
    return 0;
}
