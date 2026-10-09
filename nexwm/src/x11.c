/* x11.c — NexWM on an X11 session: the window manager (nexwm --x11, or plain `nexwm` when $DISPLAY is set).
 *
 * What it is: a full ICCCM/EWMH window manager, built on XCB alone (no libX11, no xcb-util: the atoms are interned
 * here, the key mapping is read here, the title bar is drawn with the X primitives and a core font). It puts a frame
 * and a title bar around every window it manages — the title, the icon of the program (_NET_WM_ICON), and the
 * minimize / maximize / close buttons of the bar (frame.c works out where they go) — gives the windows their focus,
 * moves them between workspaces, maximizes, snaps, minimizes and closes them, and moves or resizes them with the
 * mouse: the bar drags them, the edges of the frame resize them, a double click on the bar maximizes them, and a
 * window let go at the edge of the screen takes that half of it (or the whole work area, at the top).
 *
 * It tells the rest of the desktop about all of it through the EWMH properties the panel, the taskbar and the task
 * managers read: _NET_CLIENT_LIST, _NET_ACTIVE_WINDOW, _NET_CURRENT_DESKTOP, _NET_WORKAREA (the struts of a panel are
 * honoured, which is how HDE's panel keeps maximized windows clear of itself), _NET_WM_STATE (maximized, full screen,
 * hidden, above, below, skip taskbar/pager), _NET_FRAME_EXTENTS, _NET_SUPPORTING_WM_CHECK ("NexWM"), and _NEXWM_KEYS
 * with the key bindings it listens to. Programs may ask it to move or resize them (_NET_WM_MOVERESIZE), a window that
 * does not place itself is put in the middle of the work area, and a window that takes its own focus (WM_TAKE_FOCUS)
 * is told when it is its turn, the way ICCCM says.
 *
 * Not there yet (each one is a step of its own): window rules, and a compositor for shadows and transparency.
 *
 * Everything it does that can be seen from outside is logged on stdout with the "nexwm: " prefix (the display and its
 * size, every window it manages, the focus, the workspaces, the frames, the keys) — tests/nexwm-test.sh reads those
 * lines and asks the X server the same questions with xprop/xdotool.
 */
#define _POSIX_C_SOURCE 200809L

#include "nexwm.h"
#include "frame.h"
#include "notify.h"

#ifdef NEXWM_HAVE_XCB
#include <xcb/xcb.h>
#endif

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef NEXWM_HAVE_XCB

#define NEXWM_PING_INTERVAL_MS 2000u
#define NEXWM_PING_TIMEOUT_MS  5000u

/* ---------------------------------------------------------------- saying what happens */

static void wm_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("nexwm: ");
    vprintf(fmt, ap);
    printf("\n");
    fflush(stdout);
    va_end(ap);
}

/* ---------------------------------------------------------------- the state */

typedef struct {
    xcb_window_t id;              /* the client window */
    xcb_window_t frame;           /* our frame around it (0: a window that is not framed — a dock, the desktop) */
    int x, y, w, h;               /* the client, in root coordinates */
    int sx, sy, sw, sh;           /* what it was before a maximize or a snap (to come back to) */
    int has_saved;
    int desktop;
    int framed;
    int dock;                     /* its struts reserve room (the panel) */
    int mapped;                   /* the frame is mapped (a window on another workspace is not) */
    int hidden;                   /* minimized by us */
    int maximized, fullscreen;
    int ping_pending, unresponsive;
    uint64_t ping_sent_ms, ping_last_sent_ms;
    uint32_t ping_timestamp;
    int above, below;             /* _NET_WM_STATE_ABOVE / _NET_WM_STATE_BELOW (the panel's "Always on top") */
    int min_w, min_h;             /* WM_NORMAL_HINTS: how small a program lets its window get (0: no limit) */
    uint32_t *icon;               /* the _NET_WM_ICON picture of the program (the pixels of the one that was picked) */
    uint32_t *icon_alloc;         /* ... and the property it points into (what free() gets) */
    int icon_w, icon_h;           /* the size of that picture */
    int asked_position;           /* WM_NORMAL_HINTS said USPosition or PPosition: the program chose its place */
    int style_key;                /* the frame the last drawing was for (a full screen window has no frame at all) */
    int hover;                    /* the NEXWM_BUTTON_* under the pointer, 0 = none (it is drawn pressed) */
    int cursor;                   /* the cursor this frame is showing (a NexwmCursor, 0 = the default one) */
    /* where the frame is on the screen right now: a drag asks for the same numbers again and again, and X is told
     * only when they change (and the frame is painted when they do) */
    int fx, fy, fw, fh;
    /* ... and where the window itself was last told to be (root coordinates) and how big: the frame's rectangle is
     * not the whole of the answer client_place gives — the window inside it moves with it, and the two must not be
     * allowed to drift apart (a frame that did not move can hold a window that did) */
    int px, py, pw, ph;
    /* The sizes this window was asked to take, the most recent last (a ring). X answers every configuration request
     * with a ConfigureNotify, and moving and resizing a window are two of them: the answer to the first carries the
     * size the window still had. That is not the program resizing itself — a window that is told what it already is
     * has not changed. */
    int want_w[8], want_h[8];
    int want_n;
    char title[200];
    char class_name[120];
} NexwmClient;

/* the cursor a frame shows: one per thing the pointer can do at the edge of a frame */
typedef enum {
    NEXWM_CURSOR_DEFAULT = 0,
    NEXWM_CURSOR_MOVE,
    NEXWM_CURSOR_BUTTON,
    NEXWM_CURSOR_T, NEXWM_CURSOR_B, NEXWM_CURSOR_L, NEXWM_CURSOR_R,
    NEXWM_CURSOR_TL, NEXWM_CURSOR_TR, NEXWM_CURSOR_BL, NEXWM_CURSOR_BR,
    NEXWM_CURSOR_COUNT
} NexwmCursor;

/* a window being moved or resized with the mouse (or asked to be, with _NET_WM_MOVERESIZE) */
typedef struct {
    NexwmClient *c;
    int      active;
    int      move;                /* 1: the window is being moved, 0: it is being resized */
    unsigned sides;               /* resizing: which sides follow the pointer */
    int      x0, y0;              /* the pointer when it began (root coordinates) */
    NexwmRect start;              /* the window when it began */
    int      moved;               /* it really went somewhere (the log says so, and nothing else does) */
    int      restored;            /* a maximized window pulled off the top of the screen already came back */
    int      button;              /* the button the drag began with (the pointer is grabbed for it) */
    int      pressed;             /* a press on a button of the title bar, waiting for the release */
    int      pressed_button;      /* ... which one (NEXWM_BUTTON_*) */
} NexwmDrag;

typedef struct {
    xcb_connection_t *conn;
    const xcb_screen_t *screen;
    xcb_window_t root;
    xcb_window_t wm_check;                 /* the window _NET_SUPPORTING_WM_CHECK points at */
    xcb_atom_t atoms[NEXWM_ATOM_COUNT];
    xcb_atom_t wm_sn;                      /* the WM_Sn selection: holding it is being the window manager */

    HdeNexwmConfig cfg;

    NexwmClient **clients;
    size_t n, cap;

    xcb_window_t focused;                  /* a client window, or XCB_NONE */
    int desktop;                           /* the current workspace */
    int screen_w, screen_h;
    int wx, wy, ww, wh;                    /* the workarea: the screen without the struts */
    int strut_top, strut_left, strut_right, strut_bottom;

    unsigned lock_masks[8];                /* the modifier masks that mean "no modifier" on this keyboard */
    int n_lock_masks;

    unsigned pixel_border, pixel_focus;    /* what the frames are painted with */
    xcb_gcontext_t gc;                     /* the one GC every frame is drawn with (its foreground changes) */
    xcb_font_t font;                       /* the core font of the titles (0: there is none, titles without text) */
    int font_ascent, font_descent;
    xcb_font_t cursor_font;                /* the "cursor" font, where the cursors of the mouse come from */
    xcb_cursor_t cursors[NEXWM_CURSOR_COUNT];

    NexwmDrag drag;                        /* what the mouse is doing right now, if anything */
    xcb_timestamp_t last_click_time;       /* for a double click on the title bar (maximize / back) */
    xcb_window_t last_click_window;
    int last_click_x, last_click_y;

    int running;
    int replace;                           /* --replace: take the role from a window manager that is running */
    const char *config_path;
} Nexwm;

static Nexwm wm;
static volatile sig_atomic_t got_signal = 0;

static void on_signal(int sig) { (void)sig; got_signal = 1; }

/* ---------------------------------------------------------------- the property helpers */

static void atom_intern_all(Nexwm *w)
{
    xcb_intern_atom_cookie_t cookies[NEXWM_ATOM_COUNT];
    for (int i = 0; i < (int)NEXWM_ATOM_COUNT; i++) {
        const char *name = nexwm_atom_names[i];
        cookies[i] = xcb_intern_atom(wm.conn, 0, (uint16_t)strlen(name), name);
    }
    for (int i = 0; i < (int)NEXWM_ATOM_COUNT; i++) {
        xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(w->conn, cookies[i], NULL);
        w->atoms[i] = r ? r->atom : XCB_ATOM_NONE;
        free(r);
    }
}

static void prop_set32(Nexwm *w, xcb_window_t win, xcb_atom_t prop, xcb_atom_t type, const uint32_t *data, uint32_t n)
{
    xcb_change_property(w->conn, XCB_PROP_MODE_REPLACE, win, prop, type, 32, n, data);
}

static void prop_set_cardinal(Nexwm *w, xcb_window_t win, xcb_atom_t prop, const uint32_t *data, uint32_t n)
{
    prop_set32(w, win, prop, XCB_ATOM_CARDINAL, data, n);
}

static void prop_set_atoms(Nexwm *w, xcb_window_t win, xcb_atom_t prop, const xcb_atom_t *list, uint32_t n)
{
    prop_set32(w, win, prop, XCB_ATOM_ATOM, (const uint32_t *)list, n);
}

static void prop_set_window(Nexwm *w, xcb_window_t win, xcb_atom_t prop, xcb_window_t value)
{
    uint32_t v = value;
    prop_set32(w, win, prop, XCB_ATOM_WINDOW, &v, 1);
}

static void prop_set_string(Nexwm *w, xcb_window_t win, xcb_atom_t prop, xcb_atom_t type, const char *s)
{
    xcb_change_property(w->conn, XCB_PROP_MODE_REPLACE, win, prop, type, 8, (uint32_t)strlen(s), s);
}

/* the ICCCM way of asking a program something (WM_DELETE_WINDOW, WM_TAKE_FOCUS): a ClientMessage about WM_PROTOCOLS */
static void client_send_protocol(xcb_window_t win, HdeNexwmAtom protocol, xcb_timestamp_t time)
{
    xcb_client_message_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.response_type = XCB_CLIENT_MESSAGE;
    ev.window = win;
    ev.type = wm.atoms[NEXWM_ATOM_WM_PROTOCOLS];
    ev.format = 32;
    ev.data.data32[0] = wm.atoms[protocol];
    ev.data.data32[1] = time;
    xcb_send_event(wm.conn, 0, win, XCB_EVENT_MASK_NO_EVENT, (const char *)&ev);
}

static void prop_delete(Nexwm *w, xcb_window_t win, xcb_atom_t prop)
{
    xcb_delete_property(w->conn, win, prop);
}

/* the whole value of a property, malloc'ed (the caller frees); *n is the number of 32-bit items or of bytes */
static void *prop_get(Nexwm *w, xcb_window_t win, xcb_atom_t prop, xcb_atom_t type, int *is32, uint32_t *n)
{
    if (is32) *is32 = 0;
    if (n) *n = 0;
    xcb_get_property_cookie_t cookie = xcb_get_property(w->conn, 0, win, prop, type, 0, 4096);
    xcb_get_property_reply_t *r = xcb_get_property_reply(w->conn, cookie, NULL);
    if (!r) return NULL;
    int len = xcb_get_property_value_length(r);
    uint8_t format = r->format;
    void *out = NULL;
    if (len > 0) {
        out = malloc((size_t)len + 1);
        if (out) {
            memcpy(out, xcb_get_property_value(r), (size_t)len);
            memset((uint8_t *)out + len, 0, 1);
        }
    }
    if (is32) *is32 = format == 32;
    if (n) *n = format == 32 ? (uint32_t)(len / 4) : (uint32_t)len;
    free(r);
    return out;
}

static char *prop_get_string(Nexwm *w, xcb_window_t win, xcb_atom_t prop, xcb_atom_t type)
{
    char *s = prop_get(w, win, prop, type, NULL, NULL);
    return s;
}

static uint32_t prop_get_cardinal(Nexwm *w, xcb_window_t win, xcb_atom_t prop, uint32_t missing)
{
    uint32_t n = 0;
    uint32_t *v = prop_get(w, win, prop, XCB_ATOM_CARDINAL, NULL, &n);
    uint32_t out = (v && n) ? v[0] : missing;
    free(v);
    return out;
}

/* the window types are a list of atoms: is `type` one of them? */
static int window_is_type(Nexwm *w, xcb_window_t win, HdeNexwmAtom type)
{
    uint32_t n = 0;
    uint32_t *v = prop_get(w, win, w->atoms[NEXWM_ATOM_NET_WM_WINDOW_TYPE], XCB_ATOM_ATOM, NULL, &n);
    int found = 0;
    for (uint32_t i = 0; v && i < n; i++)
        if (v[i] == w->atoms[type]) found = 1;
    free(v);
    return found;
}

static int window_supports(Nexwm *w, xcb_window_t win, HdeNexwmAtom protocol)
{
    uint32_t n = 0;
    uint32_t *v = prop_get(w, win, w->atoms[NEXWM_ATOM_WM_PROTOCOLS], XCB_ATOM_ATOM, NULL, &n);
    int found = 0;
    for (uint32_t i = 0; v && i < n; i++)
        if (v[i] == w->atoms[protocol]) found = 1;
    free(v);
    return found;
}

static uint64_t monotonic_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

static void client_ping_start(NexwmClient *c, uint64_t now)
{
    xcb_client_message_event_t event;
    memset(&event, 0, sizeof event);
    event.response_type = XCB_CLIENT_MESSAGE;
    event.window = c->id;
    event.type = wm.atoms[NEXWM_ATOM_WM_PROTOCOLS];
    event.format = 32;
    c->ping_timestamp = XCB_CURRENT_TIME;
    event.data.data32[0] = wm.atoms[NEXWM_ATOM_NET_WM_PING];
    event.data.data32[1] = c->ping_timestamp;
    event.data.data32[2] = c->id;
    c->ping_pending = 1;
    c->ping_sent_ms = now;
    c->ping_last_sent_ms = now;
    xcb_send_event(wm.conn, 0, c->id, XCB_EVENT_MASK_NO_EVENT, (const char *)&event);
    xcb_flush(wm.conn);
}

static void clients_check_responsive(void)
{
    uint64_t now = monotonic_ms();
    for (size_t i = 0; i < wm.n; i++) {
        NexwmClient *c = wm.clients[i];
        if (!c->mapped || c->dock) continue;
        if (c->ping_pending) {
            if (!c->unresponsive && now >= c->ping_sent_ms &&
                now - c->ping_sent_ms >= NEXWM_PING_TIMEOUT_MS) {
                c->unresponsive = 1;
                wm_log("warning: 0x%x '%s' is not responding to WM_PROTOCOLS _NET_WM_PING",
                       (unsigned)c->id, c->title);
                nexwm_notify_app_unresponsive(c->title);
            }
            continue;
        }
        if (now < c->ping_last_sent_ms || now - c->ping_last_sent_ms < NEXWM_PING_INTERVAL_MS) continue;
        c->ping_last_sent_ms = now;
        if (window_supports(&wm, c->id, NEXWM_ATOM_NET_WM_PING)) client_ping_start(c, now);
    }
}

/* ---------------------------------------------------------------- the clients */

static NexwmClient *client_of_window(xcb_window_t win)
{
    for (size_t i = 0; i < wm.n; i++) {
        NexwmClient *c = wm.clients[i];
        if (c->id == win || (c->frame && c->frame == win)) return c;
    }
    return NULL;
}

static NexwmClient *client_focused(void) { return client_of_window(wm.focused); }

/* was this window asked to be this size a moment ago? (see the ring in NexwmClient) */
static int client_wanted_size(const NexwmClient *c, int w, int h)
{
    int n = c->want_n < 8 ? c->want_n : 8;
    for (int i = 0; i < n; i++) {
        int j = (c->want_n - 1 - i) % 8;
        if (j < 0) j += 8;
        if (c->want_w[j] == w && c->want_h[j] == h) return 1;
    }
    return 0;
}

static void client_want_size(NexwmClient *c, int w, int h)
{
    c->want_w[c->want_n % 8] = w;
    c->want_h[c->want_n % 8] = h;
    c->want_n++;
}

static NexwmClient *client_add(xcb_window_t id)
{
    if (wm.n == wm.cap) {
        size_t cap = wm.cap ? wm.cap * 2 : 16;
        NexwmClient **n = realloc(wm.clients, cap * sizeof *n);
        if (!n) return NULL;
        wm.clients = n;
        wm.cap = cap;
    }
    NexwmClient *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->id = id;
    c->desktop = wm.desktop;
    c->mapped = 1;
    wm.clients[wm.n++] = c;
    return c;
}

static void client_remove(NexwmClient *c)
{
    for (size_t i = 0; i < wm.n; i++) {
        if (wm.clients[i] != c) continue;
        memmove(&wm.clients[i], &wm.clients[i + 1], (wm.n - i - 1) * sizeof *wm.clients);
        wm.n--;
        break;
    }
    if (wm.focused == c->id) wm.focused = XCB_NONE;
    free(c->icon_alloc);
    free(c);
}

/* the title we use in the logs and the window list: _NET_WM_NAME, else WM_NAME, else the class */
static void client_read_title(NexwmClient *c)
{
    char *name = prop_get_string(&wm, c->id, wm.atoms[NEXWM_ATOM_NET_WM_NAME], wm.atoms[NEXWM_ATOM_UTF8_STRING]);
    if (!name || !*name) {
        free(name);
        name = prop_get_string(&wm, c->id, wm.atoms[NEXWM_ATOM_WM_NAME], XCB_ATOM_STRING);
    }
    snprintf(c->title, sizeof c->title, "%s", name && *name ? name : (c->class_name[0] ? c->class_name : "?"));
    free(name);
}

static void client_read_class(NexwmClient *c)
{
    uint32_t n = 0;
    char *s = prop_get(&wm, c->id, wm.atoms[NEXWM_ATOM_WM_CLASS], XCB_ATOM_STRING, NULL, &n);
    if (s && n > 0) {
        /* the two NUL-terminated strings of WM_CLASS: instance, then class */
        size_t first = strlen(s);
        const char *cls = (first + 1 < n) ? s + first + 1 : s;
        snprintf(c->class_name, sizeof c->class_name, "%s", cls);
    }
    free(s);
}

/* WM_NORMAL_HINTS: an XSizeHints, flags first. USPosition or PPosition in those flags means the program chose its own
 * place (it is not for us to move it); PMinSize means min_width and min_height are there, and the mouse may not resize
 * the window under them (the fields of an XSizeHints come in a fixed order: flags, x, y, width, height, min_width,
 * min_height, ... — see ICCCM 4.1.2.3). */
static void client_read_hints(NexwmClient *c)
{
    c->asked_position = 0;
    c->min_w = 0;
    c->min_h = 0;
    uint32_t n = 0;
    int is32 = 0;
    uint32_t *h = prop_get(&wm, c->id, wm.atoms[NEXWM_ATOM_WM_NORMAL_HINTS], wm.atoms[NEXWM_ATOM_WM_SIZE_HINTS], &is32, &n);
    if (!h || !is32 || n < 3) { free(h); return; }
    if (h[0] & (1u << 0)) c->asked_position = 1;        /* USPosition */
    if (h[0] & (1u << 2)) c->asked_position = 1;        /* PPosition */
    if ((h[0] & (1u << 4)) && n >= 7) {                 /* PMinSize */
        if ((int32_t)h[5] > 0 && (int32_t)h[5] <= wm.screen_w) c->min_w = (int)h[5];
        if ((int32_t)h[6] > 0 && (int32_t)h[6] <= wm.screen_h) c->min_h = (int)h[6];
    }
    free(h);
}

/* _NET_WM_ICON: the pictures of the program, the biggest of them for the title bar (frame.c picks the one that fits). */
static void client_read_icon(NexwmClient *c)
{
    free(c->icon_alloc);
    c->icon = NULL;
    c->icon_alloc = NULL;
    c->icon_w = 0;
    c->icon_h = 0;
    uint32_t n = 0;
    int is32 = 0;
    uint32_t *data = prop_get(&wm, c->id, wm.atoms[NEXWM_ATOM_NET_WM_ICON], XCB_ATOM_CARDINAL, &is32, &n);
    if (!data || !is32 || n < 3) { free(data); return; }
    int want = wm.cfg.titlebar > 0 ? wm.cfg.titlebar - 2 * NEXWM_FRAME_PAD : 16;
    if (want > 16) want = 16;
    if (want < 8) want = 8;
    const uint32_t *pixels = NULL;
    int w = 0, h = 0;
    if (!nexwm_frame_icon_pick(data, n, want, &w, &h, &pixels)) {
        free(data);                                      /* no picture in it, or a list that says it is one and is not */
        return;
    }
    c->icon = (uint32_t *)(void *)pixels;
    c->icon_alloc = data;
    c->icon_w = w;
    c->icon_h = h;
}

/* ---------------------------------------------------------------- what the desktop reads about us */

static void ewmh_update_supported(Nexwm *w)
{
    xcb_atom_t list[NEXWM_ATOM_COUNT];
    uint32_t n = 0;
    list[n++] = w->atoms[NEXWM_ATOM_NET_CLIENT_LIST];
    list[n++] = w->atoms[NEXWM_ATOM_NET_CLIENT_LIST_STACKING];
    list[n++] = w->atoms[NEXWM_ATOM_NET_ACTIVE_WINDOW];
    list[n++] = w->atoms[NEXWM_ATOM_NET_CLOSE_WINDOW];
    list[n++] = w->atoms[NEXWM_ATOM_NET_CURRENT_DESKTOP];
    list[n++] = w->atoms[NEXWM_ATOM_NET_NUMBER_OF_DESKTOPS];
    list[n++] = w->atoms[NEXWM_ATOM_NET_DESKTOP_NAMES];
    list[n++] = w->atoms[NEXWM_ATOM_NET_DESKTOP_GEOMETRY];
    list[n++] = w->atoms[NEXWM_ATOM_NET_DESKTOP_VIEWPORT];
    list[n++] = w->atoms[NEXWM_ATOM_NET_WM_DESKTOP];
    list[n++] = w->atoms[NEXWM_ATOM_NET_WORKAREA];
    list[n++] = w->atoms[NEXWM_ATOM_NET_SUPPORTING_WM_CHECK];
    list[n++] = w->atoms[NEXWM_ATOM_NET_WM_NAME];
    list[n++] = w->atoms[NEXWM_ATOM_NET_WM_STATE];
    list[n++] = w->atoms[NEXWM_ATOM_NET_WM_STATE_MAXIMIZED_VERT];
    list[n++] = w->atoms[NEXWM_ATOM_NET_WM_STATE_MAXIMIZED_HORZ];
    list[n++] = w->atoms[NEXWM_ATOM_NET_WM_STATE_FULLSCREEN];
    list[n++] = w->atoms[NEXWM_ATOM_NET_WM_STATE_HIDDEN];
    list[n++] = w->atoms[NEXWM_ATOM_NET_WM_STATE_ABOVE];
    list[n++] = w->atoms[NEXWM_ATOM_NET_WM_STATE_BELOW];
    list[n++] = w->atoms[NEXWM_ATOM_NET_WM_WINDOW_TYPE];
    list[n++] = w->atoms[NEXWM_ATOM_NET_WM_WINDOW_TYPE_NORMAL];
    list[n++] = w->atoms[NEXWM_ATOM_NET_WM_WINDOW_TYPE_DIALOG];
    list[n++] = w->atoms[NEXWM_ATOM_NET_WM_WINDOW_TYPE_DOCK];
    list[n++] = w->atoms[NEXWM_ATOM_NET_WM_WINDOW_TYPE_MENU];
    list[n++] = w->atoms[NEXWM_ATOM_NET_WM_WINDOW_TYPE_SPLASH];
    list[n++] = w->atoms[NEXWM_ATOM_NET_WM_WINDOW_TYPE_DESKTOP];
    list[n++] = w->atoms[NEXWM_ATOM_NET_WM_WINDOW_TYPE_NOTIFICATION];
    list[n++] = w->atoms[NEXWM_ATOM_NET_WM_STRUT];
    list[n++] = w->atoms[NEXWM_ATOM_NET_WM_STRUT_PARTIAL];
    list[n++] = w->atoms[NEXWM_ATOM_NET_WM_ALLOWED_ACTIONS];
    list[n++] = w->atoms[NEXWM_ATOM_NET_FRAME_EXTENTS];
    list[n++] = w->atoms[NEXWM_ATOM_NET_WM_MOVERESIZE];
    list[n++] = w->atoms[NEXWM_ATOM_NET_WM_PING];
    prop_set_atoms(w, w->root, w->atoms[NEXWM_ATOM_NET_SUPPORTED], list, n);
}

static void ewmh_update_desktops(Nexwm *w)
{
    uint32_t n = (uint32_t)w->cfg.desktops;
    prop_set_cardinal(w, w->root, w->atoms[NEXWM_ATOM_NET_NUMBER_OF_DESKTOPS], &n, 1);
    uint32_t cur = (uint32_t)w->desktop;
    prop_set_cardinal(w, w->root, w->atoms[NEXWM_ATOM_NET_CURRENT_DESKTOP], &cur, 1);

    /* the names: "1", "2", ... each one NUL-terminated, which is what the panel's workspace switcher shows */
    char names[256];
    size_t len = 0;
    for (int i = 0; i < w->cfg.desktops && len < sizeof names - 4; i++)
        len += (size_t)snprintf(names + len, sizeof names - len, "%d", i + 1) + 1;
    xcb_change_property(w->conn, XCB_PROP_MODE_REPLACE, w->root, w->atoms[NEXWM_ATOM_NET_DESKTOP_NAMES],
                        w->atoms[NEXWM_ATOM_UTF8_STRING], 8, (uint32_t)len, names);
}

static void ewmh_update_workarea(Nexwm *w)
{
    uint32_t values[4];
    /* the same area on every workspace (one workarea per desktop is allowed; HDE has one panel, one screen) */
    uint32_t *list = calloc((size_t)w->cfg.desktops * 4, sizeof *list);
    if (!list) return;
    for (int i = 0; i < w->cfg.desktops; i++) {
        values[0] = (uint32_t)w->wx;
        values[1] = (uint32_t)w->wy;
        values[2] = (uint32_t)w->ww;
        values[3] = (uint32_t)w->wh;
        memcpy(list + i * 4, values, sizeof values);
    }
    prop_set_cardinal(w, w->root, w->atoms[NEXWM_ATOM_NET_WORKAREA], list, (uint32_t)w->cfg.desktops * 4);
    free(list);
}

static void ewmh_update_client_list(Nexwm *w)
{
    uint32_t list[256];
    uint32_t n = 0;
    for (size_t i = 0; i < w->n && n < 256; i++) list[n++] = w->clients[i]->id;
    prop_set32(w, w->root, w->atoms[NEXWM_ATOM_NET_CLIENT_LIST], XCB_ATOM_WINDOW, list, n);
    prop_set32(w, w->root, w->atoms[NEXWM_ATOM_NET_CLIENT_LIST_STACKING], XCB_ATOM_WINDOW, list, n);
}

static void ewmh_update_active(Nexwm *w)
{
    if (w->focused != XCB_NONE) prop_set_window(w, w->root, w->atoms[NEXWM_ATOM_NET_ACTIVE_WINDOW], w->focused);
    else prop_delete(w, w->root, w->atoms[NEXWM_ATOM_NET_ACTIVE_WINDOW]);
}

/* what a window may be asked to do (the panel greys out the rest) */
static void client_set_allowed_actions(Nexwm *w, NexwmClient *c)
{
    xcb_atom_t list[] = {
        w->atoms[NEXWM_ATOM_NET_WM_ACTION_CLOSE],        w->atoms[NEXWM_ATOM_NET_WM_ACTION_MOVE],
        w->atoms[NEXWM_ATOM_NET_WM_ACTION_RESIZE],       w->atoms[NEXWM_ATOM_NET_WM_ACTION_MAXIMIZE_HORZ],
        w->atoms[NEXWM_ATOM_NET_WM_ACTION_MAXIMIZE_VERT], w->atoms[NEXWM_ATOM_NET_WM_ACTION_FULLSCREEN],
        w->atoms[NEXWM_ATOM_NET_WM_ACTION_CHANGE_DESKTOP], w->atoms[NEXWM_ATOM_NET_WM_ACTION_MINIMIZE],
        w->atoms[NEXWM_ATOM_NET_WM_ACTION_ABOVE],       w->atoms[NEXWM_ATOM_NET_WM_ACTION_BELOW],
    };
    prop_set_atoms(w, c->id, w->atoms[NEXWM_ATOM_NET_WM_ALLOWED_ACTIONS], list, sizeof list / sizeof list[0]);
    (void)0;
}

/* how thick the frame of a window is, in the order EWMH asks for it (left, right, top, bottom — the title bar is
 * part of the top): what the panel measures a window with, and what a program that places its own dialogs reads */
static void client_set_frame_extents(Nexwm *w, NexwmClient *c)
{
    uint32_t e[4] = { 0, 0, 0, 0 };
    if (c->framed && !c->fullscreen) {
        int border = w->cfg.border > 0 ? w->cfg.border : 0;
        int bar = w->cfg.titlebar > 0 ? w->cfg.titlebar : 0;
        e[0] = (uint32_t)border;
        e[1] = (uint32_t)border;
        e[2] = (uint32_t)(border + bar);
        e[3] = (uint32_t)border;
    }
    prop_set_cardinal(w, c->id, w->atoms[NEXWM_ATOM_NET_FRAME_EXTENTS], e, 4);
}

static void client_set_state(Nexwm *w, NexwmClient *c)
{
    xcb_atom_t list[8];
    uint32_t n = 0;
    if (c->maximized) {
        list[n++] = w->atoms[NEXWM_ATOM_NET_WM_STATE_MAXIMIZED_VERT];
        list[n++] = w->atoms[NEXWM_ATOM_NET_WM_STATE_MAXIMIZED_HORZ];
    }
    if (c->fullscreen) list[n++] = w->atoms[NEXWM_ATOM_NET_WM_STATE_FULLSCREEN];
    if (c->hidden) list[n++] = w->atoms[NEXWM_ATOM_NET_WM_STATE_HIDDEN];
    if (c->above) list[n++] = w->atoms[NEXWM_ATOM_NET_WM_STATE_ABOVE];
    if (c->below) list[n++] = w->atoms[NEXWM_ATOM_NET_WM_STATE_BELOW];
    if (c->dock) {
        list[n++] = w->atoms[NEXWM_ATOM_NET_WM_STATE_SKIP_TASKBAR];
        list[n++] = w->atoms[NEXWM_ATOM_NET_WM_STATE_SKIP_PAGER];
    }
    prop_set_atoms(w, c->id, w->atoms[NEXWM_ATOM_NET_WM_STATE], list, n);

    /* the ICCCM view of the same thing: 1 = the window is on the screen, 3 = it is minimized */
    uint32_t state[2] = { c->hidden ? 3u : 1u, XCB_ATOM_NONE };
    prop_set32(w, c->id, w->atoms[NEXWM_ATOM_WM_STATE], w->atoms[NEXWM_ATOM_WM_STATE], state, 2);
}

/* ---------------------------------------------------------------- the frames, the workarea, the focus */

/* what the frames look like: the numbers of the configuration file, in the shape frame.c works with */
static NexwmFrameStyle frame_style(void)
{
    NexwmFrameStyle st;
    st.border = wm.cfg.border;
    st.titlebar = wm.cfg.titlebar;
    st.buttons = wm.cfg.buttons;
    st.n_buttons = wm.cfg.n_buttons;
    return st;
}

/* the frame of one window: the same as the configuration's, except that a window in full screen has none (the bar of it
 * would cover the program the user asked to see alone, and _NET_FRAME_EXTENTS of it is zeros) */
static NexwmFrameStyle frame_style_of(const NexwmClient *c)
{
    NexwmFrameStyle st = frame_style();
    if (!c || !c->framed || c->fullscreen) {
        st.border = 0;
        st.titlebar = 0;
        st.buttons = NULL;
        st.n_buttons = 0;
    }
    return st;
}

/* where the frame of a window is: the window plus what is drawn around it */
static NexwmRect frame_rect(const NexwmClient *c)
{
    NexwmFrameStyle st = frame_style_of(c);
    NexwmRect client = { c->x, c->y, c->w, c->h };
    return nexwm_frame_around(&st, client);
}

static int frame_w(const NexwmClient *c) { return frame_rect(c).w; }
static int frame_h(const NexwmClient *c) { return frame_rect(c).h; }

/* the style as one number: it is compared to redraw a frame whose frame did not change size but whose look did (a window
 * that goes full screen loses its title bar without moving at all) */
static int style_key(const NexwmFrameStyle *st) { return st->border * 1000 + st->titlebar; }

/* The window rectangle that makes the *frame* cover `area`: a maximized window is that much smaller than the work area
 * (its title bar is above it, and there is a border around it), and the same for a snapped one. */
static NexwmRect client_in_area(const NexwmClient *c, NexwmRect area)
{
    NexwmFrameStyle st = frame_style_of(c);
    NexwmRect r = nexwm_frame_content(&st, area);
    if (r.w < 1) r.w = 1;
    if (r.h < 1) r.h = 1;
    return r;
}

/* Where the pointer is inside the frame of a window, in the coordinates of the frame: the events of a grab carry root
 * coordinates only (the frame is not the window they happened to), so every caller comes through here. */
static int frame_hit_at(const NexwmClient *c, int root_x, int root_y, int *button, unsigned *sides)
{
    NexwmFrameStyle st = frame_style_of(c);
    return nexwm_frame_hit(&st, frame_w(c), frame_h(c), root_x - c->fx, root_y - c->fy, button, sides);
}

/* ---------------------------------------------------------------- the drawing of a frame */

/* one of the cursors of the "cursor" font: the glyph numbers of X11/cursorfont.h */
static unsigned cursor_glyph(NexwmCursor k)
{
    switch (k) {
    case NEXWM_CURSOR_MOVE:   return 52;       /* XC_fleur */
    case NEXWM_CURSOR_BUTTON: return 60;       /* XC_hand2 */
    case NEXWM_CURSOR_T:      return 138;      /* XC_top_side */
    case NEXWM_CURSOR_B:      return 16;       /* XC_bottom_side */
    case NEXWM_CURSOR_L:      return 70;       /* XC_left_side */
    case NEXWM_CURSOR_R:      return 96;       /* XC_right_side */
    case NEXWM_CURSOR_TL:     return 134;      /* XC_top_left_corner */
    case NEXWM_CURSOR_TR:     return 136;      /* XC_top_right_corner */
    case NEXWM_CURSOR_BL:     return 12;       /* XC_bottom_left_corner */
    case NEXWM_CURSOR_BR:     return 14;       /* XC_bottom_right_corner */
    default:                  return 68;       /* XC_left_ptr */
    }
}

/* which cursor a place in the frame shows: the ends of the bar and the borders resize, the buttons are clickable */
static NexwmCursor cursor_of_hit(int hit, unsigned sides)
{
    if (hit == NEXWM_HIT_BUTTON) return NEXWM_CURSOR_BUTTON;
    if (hit == NEXWM_HIT_TITLE) return NEXWM_CURSOR_DEFAULT;
    if (hit != NEXWM_HIT_EDGE) return NEXWM_CURSOR_DEFAULT;
    switch (sides) {
    case NEXWM_SIDE_TOP:                          return NEXWM_CURSOR_T;
    case NEXWM_SIDE_BOTTOM:                       return NEXWM_CURSOR_B;
    case NEXWM_SIDE_LEFT:                         return NEXWM_CURSOR_L;
    case NEXWM_SIDE_RIGHT:                        return NEXWM_CURSOR_R;
    case NEXWM_SIDE_TOP | NEXWM_SIDE_LEFT:        return NEXWM_CURSOR_TL;
    case NEXWM_SIDE_TOP | NEXWM_SIDE_RIGHT:       return NEXWM_CURSOR_TR;
    case NEXWM_SIDE_BOTTOM | NEXWM_SIDE_LEFT:     return NEXWM_CURSOR_BL;
    case NEXWM_SIDE_BOTTOM | NEXWM_SIDE_RIGHT:    return NEXWM_CURSOR_BR;
    default:                                      return NEXWM_CURSOR_DEFAULT;
    }
}

/* The cursors of the mouse, out of the "cursor" font the X server has had since forever (no theme, no Xcursor: the
 * window manager does not read a user's cursor theme, it draws nothing itself). A machine without that font gets the
 * default pointer and the frames work all the same. */
static void cursors_create(void)
{
    const char *name = "cursor";
    wm.cursor_font = xcb_generate_id(wm.conn);
    if (xcb_request_check(wm.conn, xcb_open_font_checked(wm.conn, wm.cursor_font, (uint16_t)strlen(name), name))) {
        wm.cursor_font = 0;
        wm_log("the 'cursor' font is not on this X server: the frames use the ordinary pointer");
        return;
    }
    for (int i = 0; i < NEXWM_CURSOR_COUNT; i++) {
        unsigned glyph = cursor_glyph((NexwmCursor)i);
        xcb_cursor_t cur = xcb_generate_id(wm.conn);
        /* the glyphs of that font come in pairs: the shape, then the mask of it */
        xcb_create_glyph_cursor(wm.conn, cur, wm.cursor_font, wm.cursor_font, (uint16_t)glyph, (uint16_t)(glyph + 1),
                                0, 0, 0, 0xffff, 0xffff, 0xffff);
        wm.cursors[i] = cur;
    }
}

/* the cursor a frame shows (only when it changes: a motion event is not a time to talk to the server) */
static void frame_cursor(NexwmClient *c, NexwmCursor want)
{
    if (!c->frame || c->cursor == (int)want) return;
    c->cursor = (int)want;
    xcb_change_window_attributes(wm.conn, c->frame, XCB_CW_CURSOR, &wm.cursors[want]);
}

/* the font of the titles: a core font of the X server ("fixed"), which is why a title is drawn in Latin-1 */
static void title_font_open(void)
{
    const char *name = wm.cfg.font[0] ? wm.cfg.font : "fixed";
    wm.font = xcb_generate_id(wm.conn);
    if (xcb_request_check(wm.conn, xcb_open_font_checked(wm.conn, wm.font, (uint16_t)strlen(name), name))) {
        wm.font = 0;
        wm_log("the font '%s' of the titles is not on this X server: the title bars show no text", name);
        return;
    }
    xcb_query_font_reply_t *info = xcb_query_font_reply(wm.conn, xcb_query_font(wm.conn, wm.font), NULL);
    if (info) {
        wm.font_ascent = info->font_ascent;
        wm.font_descent = info->font_descent;
        free(info);
    } else {
        wm.font_ascent = 8;
        wm.font_descent = 2;
    }
}

/* the drawing of one button of the title bar: its square, and the little sign in it */
static void frame_draw_button(xcb_window_t frame, const NexwmFrameButton *b, unsigned long bg, unsigned long fg,
                              int maximized)
{
    uint32_t value = (uint32_t)bg;
    xcb_change_gc(wm.conn, wm.gc, XCB_GC_FOREGROUND, &value);
    xcb_rectangle_t r = { (int16_t)b->rect.x, (int16_t)b->rect.y, (uint16_t)b->rect.w, (uint16_t)b->rect.h };
    xcb_poly_fill_rectangle(wm.conn, frame, wm.gc, 1, &r);

    int x = b->rect.x, y = b->rect.y, w = b->rect.w, h = b->rect.h;
    int pad = w / 4;
    if (pad < 3) pad = 3;
    int x1 = x + pad, y1 = y + pad, x2 = x + w - pad, y2 = y + h - pad;
    value = (uint32_t)fg;
    xcb_change_gc(wm.conn, wm.gc, XCB_GC_FOREGROUND, &value);

    xcb_point_t p[8];
    int n = 0;
    switch (b->kind) {
    case NEXWM_BUTTON_MINIMIZE:                     /* a line at the bottom of the square */
        p[0] = (xcb_point_t){ (int16_t)x1, (int16_t)y2 };
        p[1] = (xcb_point_t){ (int16_t)x2, (int16_t)y2 };
        n = 2;
        break;
    case NEXWM_BUTTON_MAXIMIZE:
        if (maximized) {                            /* two squares on top of each other: "give the old size back" */
            p[0] = (xcb_point_t){ (int16_t)(x1 + pad / 2), (int16_t)y1 };
            p[1] = (xcb_point_t){ (int16_t)x2, (int16_t)y1 };
            p[2] = (xcb_point_t){ (int16_t)x2, (int16_t)(y2 - pad / 2) };
            xcb_poly_line(wm.conn, XCB_COORD_MODE_ORIGIN, frame, wm.gc, 3, p);
            p[0] = (xcb_point_t){ (int16_t)x1, (int16_t)(y1 + pad / 2) };
            p[1] = (xcb_point_t){ (int16_t)(x2 - pad / 2), (int16_t)(y1 + pad / 2) };
            p[2] = (xcb_point_t){ (int16_t)(x2 - pad / 2), (int16_t)y2 };
            p[3] = (xcb_point_t){ (int16_t)x1, (int16_t)y2 };
            p[4] = (xcb_point_t){ (int16_t)x1, (int16_t)(y1 + pad / 2) };
            xcb_poly_line(wm.conn, XCB_COORD_MODE_ORIGIN, frame, wm.gc, 5, p);
        } else {                                    /* one square: maximize */
            p[0] = (xcb_point_t){ (int16_t)x1, (int16_t)y1 };
            p[1] = (xcb_point_t){ (int16_t)x2, (int16_t)y1 };
            p[2] = (xcb_point_t){ (int16_t)x2, (int16_t)y2 };
            p[3] = (xcb_point_t){ (int16_t)x1, (int16_t)y2 };
            p[4] = (xcb_point_t){ (int16_t)x1, (int16_t)y1 };
            n = 5;
        }
        break;
    default:                                        /* close: a cross */
        p[0] = (xcb_point_t){ (int16_t)x1, (int16_t)y1 };
        p[1] = (xcb_point_t){ (int16_t)x2, (int16_t)y2 };
        xcb_poly_line(wm.conn, XCB_COORD_MODE_ORIGIN, frame, wm.gc, 2, p);
        p[0] = (xcb_point_t){ (int16_t)x2, (int16_t)y1 };
        p[1] = (xcb_point_t){ (int16_t)x1, (int16_t)y2 };
        n = 2;
        break;
    }
    if (n) xcb_poly_line(wm.conn, XCB_COORD_MODE_ORIGIN, frame, wm.gc, (uint32_t)n, p);
}

/* Paint a frame: the border of it, then the title bar with its icon, its title and its buttons. Everything X is not
 * asked to keep (a window that is covered and comes back, a resize) is drawn again — this is the only place that
 * draws a frame, so what is on the screen is what this function draws. */
static void frame_paint(NexwmClient *c)
{
    if (!c->frame) return;
    int focused = wm.focused == c->id;
    int fw = frame_w(c), fh = frame_h(c);
    NexwmFrameStyle st = frame_style_of(c);
    int border = st.border > 0 ? st.border : 0;
    int bar = st.titlebar > 0 ? st.titlebar : 0;

    /* the frame itself: the border colour (the focused one in the colour of the focus) */
    uint32_t value = (uint32_t)(focused ? wm.pixel_focus : wm.pixel_border);
    xcb_change_window_attributes(wm.conn, c->frame, XCB_CW_BACK_PIXEL, &value);
    xcb_clear_area(wm.conn, 0, c->frame, 0, 0, (uint16_t)fw, (uint16_t)fh);
    if (bar <= 0) return;                      /* a full screen window (or a bar of 0 px): the border is all of it */


    unsigned long bar_bg = focused ? wm.cfg.titlebar_color : wm.cfg.titlebar_color_unfocused;
    unsigned long text_px = focused ? wm.cfg.titlebar_text : wm.cfg.titlebar_text_unfocused;

    /* the bar */
    value = (uint32_t)bar_bg;
    xcb_change_gc(wm.conn, wm.gc, XCB_GC_FOREGROUND, &value);
    xcb_rectangle_t bar_rect = { 0, (int16_t)border, (uint16_t)fw, (uint16_t)bar };
    xcb_poly_fill_rectangle(wm.conn, c->frame, wm.gc, 1, &bar_rect);

    /* the buttons (the one under the pointer is a little lighter, so it is visible what a click would do) */
    NexwmFrameButton b[8];
    int n = nexwm_frame_buttons(&st, fw, b, 8);
    for (int i = 0; i < n; i++) {
        unsigned long bg = bar_bg;
        if (c->hover && c->hover == b[i].kind) bg = nexwm_frame_shade(bar_bg, focused ? 140 : 120);
        frame_draw_button(c->frame, &b[i], bg, text_px, c->maximized || c->fullscreen);
    }

    /* the icon of the program, at the left end of the bar */
    int icon_end = 0;
    if (c->icon && c->icon_w > 0) {
        int want = bar - 2 * NEXWM_FRAME_PAD;
        if (want > 16) want = 16;
        if (want >= 8) {
            uint8_t *pixels = malloc((size_t)want * (size_t)want * 4);
            if (pixels) {
                nexwm_frame_icon_draw(c->icon, c->icon_w, c->icon_h, want, bar_bg, pixels);
                xcb_put_image(wm.conn, XCB_IMAGE_FORMAT_Z_PIXMAP, c->frame, wm.gc, (uint16_t)want, (uint16_t)want,
                              (int16_t)NEXWM_FRAME_PAD, (int16_t)(border + (bar - want) / 2), 0,
                              (uint8_t)wm.screen->root_depth, (uint32_t)(want * want * 4), pixels);
                free(pixels);
                icon_end = NEXWM_FRAME_PAD + want;
            }
        }
    }

    /* the title, centred in what the icon and the buttons leave of the bar */
    if (wm.font && c->title[0]) {
        char title[200];
        nexwm_frame_title(c->title, title, sizeof title);
        size_t len = strlen(title);
        if (len > 255) len = 255;                   /* ImageText8 carries the length in one byte */
        if (len) {
            /* a query takes the string as 16-bit characters (the font is the one of the X server: Latin-1 here) */
            xcb_char2b_t chars[256];
            for (size_t i = 0; i < len; i++) {
                chars[i].byte1 = 0;
                chars[i].byte2 = (uint8_t)title[i];
            }
            xcb_query_text_extents_reply_t *ext = xcb_query_text_extents_reply(
                wm.conn, xcb_query_text_extents(wm.conn, wm.font, (uint32_t)len, chars), NULL);
            int text_w = ext ? ext->overall_width : (int)len * 6;
            free(ext);
            int tx = nexwm_frame_title_x(&st, fw, text_w, icon_end);
            int ty = border + (bar + wm.font_ascent - wm.font_descent) / 2;
            int clip_x = icon_end > 0 ? icon_end + 2 : 2;
            int clip_r = n > 0 ? b[0].rect.x - 2 : fw - 2;
            if (clip_r < clip_x) clip_r = clip_x;
            xcb_rectangle_t clip = { (int16_t)clip_x, (int16_t)border, (uint16_t)(clip_r - clip_x), (uint16_t)bar };
            xcb_set_clip_rectangles(wm.conn, XCB_CLIP_ORDERING_UNSORTED, wm.gc, 0, 0, 1, &clip);
            xcb_image_text_8(wm.conn, (uint8_t)len, c->frame, wm.gc, (int16_t)tx, (int16_t)ty, title);
            /* ... and the clip is taken off again. An empty list of clip rectangles is not "no clipping": it is a clip
             * region that covers nothing, and every bar and every button drawn after it — of this frame, or of any
             * other — would not appear at all. A clip mask of None is what says "draw everywhere". */
            uint32_t no_mask = XCB_NONE;
            xcb_change_gc(wm.conn, wm.gc, XCB_GC_CLIP_MASK, &no_mask);
        }
    }
}

/* Move and size the frame and the window inside it. A window sits at (border, border + title bar) of its frame, so
 * the frame is the window plus the frame around it. The X server is told only when something really changed — a drag
 * asks for the same numbers again and again — and a frame that changed size is painted again, because a resize
 * leaves the new part of a window in its background colour (the bar would lose its title). */
static void client_place(NexwmClient *c)
{
    NexwmFrameStyle st = frame_style_of(c);
    int nfx = c->x, nfy = c->y, nfw = c->w, nfh = c->h, cx = 0, cy = 0;
    int px = c->x, py = c->y;                 /* where the client window goes (root coordinates) */
    if (c->framed) {
        NexwmRect f = frame_rect(c);
        nfx = f.x; nfy = f.y; nfw = f.w; nfh = f.h;
        cx = st.border;
        cy = st.border + (st.titlebar > 0 ? st.titlebar : 0);
        px = nfx + cx;
        py = nfy + cy;
    }
    int key = c->framed ? style_key(&st) : 0;
    int resized = c->fw != nfw || c->fh != nfh || c->fw == 0 || c->style_key != key;
    int restyled = c->framed && c->style_key != key;          /* the frame itself changed: no border, no bar, ... */
    /* Nothing to say only when *all* of it is what was said last time: the frame's rectangle, the look of it, and the
     * window inside it — place and size alike. A window that is somewhere else than where this function left it (a
     * full screen window of the same size put back, a configuration request that never arrived) is told again here,
     * instead of being left in a frame that no longer agrees with it. */
    if (!resized && c->fx == nfx && c->fy == nfy && c->px == px && c->py == py && c->pw == c->w && c->ph == c->h)
        return;
    c->fx = nfx; c->fy = nfy; c->fw = nfw; c->fh = nfh; c->style_key = key;
    c->px = px; c->py = py; c->pw = c->w; c->ph = c->h;
    if (restyled) client_set_frame_extents(&wm, c);            /* a window that goes full screen has no frame any more */

    if (!c->framed) {
        uint32_t v[4] = { (uint32_t)c->x, (uint32_t)c->y, (uint32_t)c->w, (uint32_t)c->h };
        xcb_configure_window(wm.conn, c->id, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH |
                                             XCB_CONFIG_WINDOW_HEIGHT, v);
        client_want_size(c, c->w, c->h);
        return;
    }
    uint32_t f[4] = { (uint32_t)nfx, (uint32_t)nfy, (uint32_t)nfw, (uint32_t)nfh };
    xcb_configure_window(wm.conn, c->frame, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH |
                                             XCB_CONFIG_WINDOW_HEIGHT, f);
    uint32_t where[2] = { (uint32_t)cx, (uint32_t)cy };
    xcb_configure_window(wm.conn, c->id, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, where);
    uint32_t size[2] = { (uint32_t)c->w, (uint32_t)c->h };
    xcb_configure_window(wm.conn, c->id, XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, size);
    client_want_size(c, c->w, c->h);
    if (resized || restyled) frame_paint(c);
}

/* Is the window (and its frame) on the screen? A window of another workspace is unmapped, not destroyed: coming back
 * is instant and the program keeps everything it had. The client itself is unmapped with the frame, because a
 * toolkit that only sees its parent go away keeps thinking it is on the screen. */
static void client_mapped_set(NexwmClient *c, int want)
{
    if (!c->framed) {
        xcb_map_window(wm.conn, c->id);            /* a dock or the desktop: always there */
        c->mapped = 1;
        return;
    }
    if (want == c->mapped) return;
    if (want) {
        xcb_map_window(wm.conn, c->id);            /* the client first, so the frame never shows an empty hole */
        xcb_map_window(wm.conn, c->frame);
        c->mapped = 1;
    } else {
        c->mapped = 0;                             /* before the requests: the UnmapNotify they cause is ours */
        c->ping_pending = 0;
        c->unresponsive = 0;
        xcb_unmap_window(wm.conn, c->id);
        xcb_unmap_window(wm.conn, c->frame);
    }
}

static void client_hide(NexwmClient *c, int hide)
{
    if (hide == c->hidden) return;
    c->hidden = hide;
    client_mapped_set(c, !hide && c->desktop == wm.desktop);
    client_set_state(&wm, c);
}

/* called when the window's workspace changes, or the current one does */
static void client_show_for_desktop(NexwmClient *c)
{
    if (!c->framed) {
        client_mapped_set(c, 1);
        return;
    }
    client_mapped_set(c, c->desktop == wm.desktop && !c->hidden);
}

static void workarea_update(Nexwm *w)
{
    int top = 0, left = 0, right = 0, bottom = 0;
    for (size_t i = 0; i < w->n; i++) {
        NexwmClient *c = w->clients[i];
        if (!c->dock) continue;
        uint32_t n = 0;
        uint32_t *v = prop_get(w, c->id, w->atoms[NEXWM_ATOM_NET_WM_STRUT_PARTIAL], XCB_ATOM_CARDINAL, NULL, &n);
        if (!v || n < 4) {
            free(v);
            v = prop_get(w, c->id, w->atoms[NEXWM_ATOM_NET_WM_STRUT], XCB_ATOM_CARDINAL, NULL, &n);
        }
        if (v && n >= 4) {
            if ((int)v[0] > left) left = (int)v[0];
            if ((int)v[1] > right) right = (int)v[1];
            if ((int)v[2] > top) top = (int)v[2];
            if ((int)v[3] > bottom) bottom = (int)v[3];
        }
        free(v);
    }
    int was = (w->strut_top != top || w->strut_left != left || w->strut_right != right || w->strut_bottom != bottom);
    w->strut_top = top;
    w->strut_left = left;
    w->strut_right = right;
    w->strut_bottom = bottom;

    w->wx = left;
    w->wy = top;
    w->ww = w->screen_w - left - right;
    w->wh = w->screen_h - top - bottom;
    ewmh_update_workarea(w);
    if (was) {
        if (top || left || right || bottom)
            wm_log("workarea %dx%d at %d,%d (a panel reserves %d at the top, %d at the bottom, %d left, %d right)",
                   w->ww, w->wh, w->wx, w->wy, top, bottom, left, right);
        else
            wm_log("workarea %dx%d at %d,%d (nothing reserved)", w->ww, w->wh, w->wx, w->wy);
    }
}

/* The order EWMH asks for: _NET_WM_STATE_BELOW windows under everything, then the ordinary ones, then the ones that
 * asked for _NET_WM_STATE_ABOVE. Only the frames are put in order (each one right above the one before it in the list,
 * which is the order they are in now), so a panel or a menu that is not ours keeps the place it chose. */
static void restack_clients(void)
{
    int interesting = 0;
    for (size_t i = 0; i < wm.n; i++)
        if (wm.clients[i]->above || wm.clients[i]->below) interesting = 1;
    if (!interesting || wm.n < 2) return;
    xcb_query_tree_reply_t *tree = xcb_query_tree_reply(wm.conn, xcb_query_tree(wm.conn, wm.root), NULL);
    if (!tree) return;
    int kids = (int)xcb_query_tree_children_length(tree);
    xcb_window_t *child = xcb_query_tree_children(tree);

    NexwmClient **order = calloc(wm.n, sizeof *order);
    size_t k = 0;
    if (order) {
        for (int pass = 0; pass < 3; pass++) {           /* 0: below, 1: the ordinary ones, 2: above */
            for (int i = 0; i < kids; i++) {
                NexwmClient *c = client_of_window(child[i]);
                if (!c || !c->frame || k >= wm.n) continue;
                int group = c->below ? 0 : (c->above ? 2 : 1);
                if (group == pass) order[k++] = c;
            }
        }
        for (size_t i = 1; i < k; i++) {
            xcb_window_t sibling = order[i - 1]->frame;
            uint32_t mode = XCB_STACK_MODE_ABOVE;
            xcb_configure_window(wm.conn, order[i]->frame, XCB_CONFIG_WINDOW_SIBLING, &sibling);
            xcb_configure_window(wm.conn, order[i]->frame, XCB_CONFIG_WINDOW_STACK_MODE, &mode);
        }
        free(order);
    }
    free(tree);
}

/* a click brings a window to the front — unless it asked to stay under the others */
static void raise_client(NexwmClient *c)
{
    if (!c) return;
    if (!c->below) {
        /* a window without a frame of its own (a splash screen, a dialog that asked for no frame, the dock) is raised
         * as it is: it is still a window of the screen, and a click on it puts it at the top like any other */
        uint32_t mode = XCB_STACK_MODE_ABOVE;
        xcb_configure_window(wm.conn, c->frame ? c->frame : c->id, XCB_CONFIG_WINDOW_STACK_MODE, &mode);
    }
    if (c->frame) restack_clients();
}

static void focus_client(NexwmClient *c, int raise_it)
{
    xcb_window_t was = wm.focused;
    wm.focused = c ? c->id : XCB_NONE;
    if (c) {
        if (raise_it) raise_client(c);
        /* ICCCM: a program that asked to be told when it may take the focus gets the message (the same one the panel
         * sends on a click). The keyboard goes to the client as well, so a program that ignores the message still has it. */
        if (window_supports(&wm, c->id, NEXWM_ATOM_WM_TAKE_FOCUS))
            client_send_protocol(c->id, NEXWM_ATOM_WM_TAKE_FOCUS, XCB_CURRENT_TIME);
        xcb_set_input_focus(wm.conn, XCB_INPUT_FOCUS_POINTER_ROOT, c->id, XCB_CURRENT_TIME);
    } else {
        xcb_set_input_focus(wm.conn, XCB_INPUT_FOCUS_POINTER_ROOT, XCB_NONE, XCB_CURRENT_TIME);
    }
    ewmh_update_active(&wm);
    for (size_t i = 0; i < wm.n; i++)
        if (wm.clients[i]->framed && (wm.clients[i]->id == was || wm.clients[i]->id == wm.focused))
            frame_paint(wm.clients[i]);
    if (c) wm_log("focus 0x%x '%s'", (unsigned)c->id, c->title);
    else wm_log("no window has the focus");
}

/* the windows of the current workspace, in the order they were added */
static NexwmClient *client_next_in_desktop(NexwmClient *from, int direction)
{
    NexwmClient *first = NULL;
    for (size_t i = 0; i < wm.n; i++) {
        NexwmClient *c = wm.clients[i];
        if (!c->framed || c->desktop != wm.desktop || c->hidden) continue;
        if (!first) first = c;
    }
    if (!from || !first) return first;
    /* walk the list from `from` (or from the start) in the direction asked, wrapping around */
    long start = 0;
    for (size_t i = 0; i < wm.n; i++)
        if (wm.clients[i] == from) start = (long)i;
    for (long k = 1; k <= (long)wm.n; k++) {
        long i = start + direction * k;
        while (i < 0) i += (long)wm.n;
        i %= (long)wm.n;
        NexwmClient *c = wm.clients[i];
        if (c->framed && c->desktop == wm.desktop && !c->hidden && c != from) return c;
    }
    return from;
}

/* ---------------------------------------------------------------- managing and unmanaging */

static void client_frame_create(NexwmClient *c)
{
    NexwmFrameStyle st = frame_style_of(c);
    NexwmRect f = frame_rect(c);
    c->frame = xcb_generate_id(wm.conn);
    uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
    uint32_t values[2];
    values[0] = (uint32_t)wm.pixel_border;
    values[1] = XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY | XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_LEAVE_WINDOW |
                XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_POINTER_MOTION |
                XCB_EVENT_MASK_EXPOSURE;
    xcb_create_window(wm.conn, XCB_COPY_FROM_PARENT, c->frame, wm.root, (int16_t)f.x, (int16_t)f.y,
                      (uint16_t)(f.w > 0 ? f.w : 1), (uint16_t)(f.h > 0 ? f.h : 1), 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      XCB_COPY_FROM_PARENT, mask, values);
    c->fx = f.x; c->fy = f.y; c->fw = f.w; c->fh = f.h; c->style_key = style_key(&st);
    c->cursor = NEXWM_CURSOR_DEFAULT;
}

static void client_manage(xcb_window_t id)
{
    xcb_get_window_attributes_reply_t *attr = xcb_get_window_attributes_reply(
        wm.conn, xcb_get_window_attributes(wm.conn, id), NULL);
    if (!attr) return;
    if (attr->override_redirect) {                 /* menus, tooltips, the OSD: theirs to place */
        free(attr);
        xcb_map_window(wm.conn, id);
        return;
    }
    int map_state = attr->map_state;
    free(attr);

    NexwmClient *c = client_add(id);
    if (!c) return;
    client_read_class(c);
    client_read_title(c);
    client_read_hints(c);

    xcb_get_geometry_reply_t *geo = xcb_get_geometry_reply(wm.conn, xcb_get_geometry(wm.conn, id), NULL);
    if (geo) {
        c->x = geo->x;
        c->y = geo->y;
        c->w = geo->width;
        c->h = geo->height;
        free(geo);
    }

    int dock = window_is_type(&wm, id, NEXWM_ATOM_NET_WM_WINDOW_TYPE_DOCK);
    int desktop_win = window_is_type(&wm, id, NEXWM_ATOM_NET_WM_WINDOW_TYPE_DESKTOP);
    c->dock = dock;
    c->framed = !(dock || desktop_win);
    client_read_icon(c);

    /* a program that did not say where it wants to be (no USPosition, no PPosition) and that is sitting in the corner of
     * the screen is asking the window manager for a place: the middle of the work area, a little aside for every window
     * that is already there. A program that chose its place keeps it — and so does one that only looks like it did. */
    if (c->framed && !c->asked_position && c->x == 0 && c->y == 0) {
        int already = 0;
        for (size_t i = 0; i < wm.n; i++)
            if (wm.clients[i] != c && wm.clients[i]->framed && wm.clients[i]->desktop == c->desktop) already++;
        NexwmRect work = { wm.wx, wm.wy, wm.ww, wm.wh };
        NexwmRect out;
        nexwm_frame_place(work, c->w, c->h, already * 24, &out);
        c->x = out.x;
        c->y = out.y;
        c->w = out.w;
        c->h = out.h;
        wm_log("0x%x '%s' asked for no place: %dx%d in the middle of the work area", (unsigned)c->id, c->title, c->w,
               c->h);
    }

    /* which workspace: the window itself may say (_NET_WM_DESKTOP), a dock is on all of them */
    uint32_t want = prop_get_cardinal(&wm, id, wm.atoms[NEXWM_ATOM_NET_WM_DESKTOP], 0xffffffffu);
    if (!c->dock && !desktop_win && want != 0xffffffffu && (int)want < wm.cfg.desktops) c->desktop = (int)want;

    uint32_t events[] = { XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_STRUCTURE_NOTIFY |
                          XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_FOCUS_CHANGE };
    if (c->framed) {
        client_frame_create(c);
        /* the window sits *inside* the frame: the border on the left, and the border plus the title bar above it (the
         * same place client_place keeps it in: a program's window is never drawn under its own title bar) */
        xcb_reparent_window(wm.conn, id, c->frame, (int16_t)wm.cfg.border,
                            (int16_t)(wm.cfg.border + (wm.cfg.titlebar > 0 ? wm.cfg.titlebar : 0)));
        uint32_t zero = 0;
        xcb_configure_window(wm.conn, id, XCB_CONFIG_WINDOW_BORDER_WIDTH, &zero);
        client_place(c);
        xcb_map_window(wm.conn, c->frame);
        xcb_map_window(wm.conn, id);
        c->mapped = 1;
    } else {
        /* a dock or the desktop keeps its own geometry; the desktop goes to the bottom of the stack */
        xcb_change_window_attributes(wm.conn, id, XCB_CW_EVENT_MASK, events);
        if (desktop_win) {
            uint32_t mode = XCB_STACK_MODE_BELOW;
            xcb_configure_window(wm.conn, id, XCB_CONFIG_WINDOW_STACK_MODE, &mode);
        }
        xcb_map_window(wm.conn, id);
        c->mapped = 1;
        if (dock) workarea_update(&wm);
    }
    xcb_change_window_attributes(wm.conn, id, XCB_CW_EVENT_MASK, events);

    /* every managed window says how thick its frame is — 0 for the ones that have none (the panel, the desktop), which
     * is what EWMH asks for and what the panel reads before it places a window */
    client_set_frame_extents(&wm, c);
    client_set_allowed_actions(&wm, c);
    client_set_state(&wm, c);
    ewmh_update_client_list(&wm);
    wm_log("managing 0x%x '%s' %dx%d at %d,%d%s", (unsigned)id, c->title, c->w, c->h, c->x, c->y,
           c->framed ? "" : (dock ? " (a panel: not framed, its struts are reserved)" : " (the desktop: not framed)"));

    if (c->framed) {
        if (c->desktop == wm.desktop) {
            focus_client(c, 1);
        } else {
            client_show_for_desktop(c);         /* it belongs to another workspace: it stays off this one */
            wm_log("0x%x '%s' opens on workspace %d", (unsigned)id, c->title, c->desktop + 1);
        }
        wm_log("workspace %d/%d: %d window(s)", wm.desktop + 1, wm.cfg.desktops, (int)wm.n);
    }
    (void)map_state;
}

/* the mouse may be in the middle of moving a window when it goes away (the program quit, the session ended it): the
 * drag is let go of with it (the pointer is ungrabbed; the function itself is down in the mouse section) */
static void drag_stop(void);

static void client_unmanage(NexwmClient *c, int destroyed)
{
    if (wm.drag.c == c) drag_stop();
    if (wm.focused == c->id) wm.focused = XCB_NONE;
    if (c->frame && !destroyed) {
        uint32_t zero = 0;
        xcb_reparent_window(wm.conn, c->id, wm.root, (int16_t)c->x, (int16_t)c->y);
        xcb_configure_window(wm.conn, c->id, XCB_CONFIG_WINDOW_BORDER_WIDTH, &zero);
    }
    if (c->frame) {
        xcb_destroy_window(wm.conn, c->frame);
        c->frame = 0;
    }
    int was_dock = c->dock;
    wm_log("unmanaging 0x%x '%s'", (unsigned)c->id, c->title);
    client_remove(c);
    ewmh_update_client_list(&wm);
    if (was_dock) workarea_update(&wm);
    if (wm.focused == XCB_NONE) {
        NexwmClient *n = client_next_in_desktop(NULL, 1);
        if (n) focus_client(n, 0);
        else ewmh_update_active(&wm);
    }
}

/* ---------------------------------------------------------------- the actions of the key bindings */

static void action_spawn(const char *command)
{
    /* what the user asked for, if the program is not installed: say so instead of doing nothing (the CI test uses
     * this: a spawn of something that is not there has to be visible in the log) */
    char program[256];
    size_t i = 0;
    while (command[i] && command[i] != ' ' && i < sizeof program - 1) {
        program[i] = command[i];
        i++;
    }
    program[i] = '\0';
    if (program[0] == '/' || strchr(program, '/')) {
        if (access(program, X_OK) != 0) {
            wm_log("cannot run '%s': not there (%s)", command, strerror(errno));
            return;
        }
    } else if (*program) {
        const char *path = getenv("PATH");
        int found = 0;
        char *paths = strdup(path && *path ? path : "/usr/bin:/bin");
        for (char *p = strtok(paths, ":"); p && !found; p = strtok(NULL, ":")) {
            char full[1024];
            snprintf(full, sizeof full, "%s/%s", p, program);
            if (access(full, X_OK) == 0) found = 1;
        }
        free(paths);
        if (!found) {
            wm_log("cannot run '%s': '%s' is not installed (set the key in %s)", command, program,
                   wm.config_path ? wm.config_path : "~/.config/hde/nexwm.conf");
            return;
        }
    }

    wm_log("spawn: %s", command);
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        execl("/bin/sh", "sh", "-c", command, (char *)NULL);
        _exit(127);
    }
    if (pid < 0) wm_log("cannot start '%s': %s", command, strerror(errno));
}

static void action_close(NexwmClient *c, int politely)
{
    if (!c) {
        wm_log("no window to close");
        return;
    }
    if (politely && window_supports(&wm, c->id, NEXWM_ATOM_WM_DELETE_WINDOW)) {
        client_send_protocol(c->id, NEXWM_ATOM_WM_DELETE_WINDOW, XCB_CURRENT_TIME);
        wm_log("asked 0x%x '%s' to close", (unsigned)c->id, c->title);
        return;
    }
    wm_log("kill 0x%x '%s'", (unsigned)c->id, c->title);
    xcb_kill_client(wm.conn, c->id);
}

static void action_workspace(int which)
{
    if (which < 0 || which >= wm.cfg.desktops) return;
    if (which == wm.desktop) {
        wm_log("already on workspace %d", which + 1);
        return;
    }
    wm.desktop = which;
    wm.focused = XCB_NONE;
    for (size_t i = 0; i < wm.n; i++) client_show_for_desktop(wm.clients[i]);
    ewmh_update_desktops(&wm);
    NexwmClient *n = client_next_in_desktop(NULL, 1);
    if (n) focus_client(n, 1);
    ewmh_update_active(&wm);
    wm_log("workspace %d/%d (%d window(s))", which + 1, wm.cfg.desktops, (int)wm.n);
}

static void action_move_to(int which)
{
    NexwmClient *c = client_focused();
    if (!c || c->dock) {
        wm_log("no window to move");
        return;
    }
    if (which < 0 || which >= wm.cfg.desktops || which == c->desktop) return;
    uint32_t v = (uint32_t)which;
    prop_set_cardinal(&wm, c->id, wm.atoms[NEXWM_ATOM_NET_WM_DESKTOP], &v, 1);
    c->desktop = which;
    client_show_for_desktop(c);
    wm_log("0x%x '%s' to workspace %d", (unsigned)c->id, c->title, which + 1);
    NexwmClient *n = client_next_in_desktop(NULL, 1);
    if (n) focus_client(n, 0);
    else ewmh_update_active(&wm);
}

static void action_maximize(NexwmClient *c, int on)
{
    if (!c) {
        wm_log("no window to maximize");
        return;
    }
    if (on && !c->maximized) {
        c->sx = c->x; c->sy = c->y; c->sw = c->w; c->sh = c->h;
        c->has_saved = 1;
        /* the *frame* covers the work area: the window is that much smaller than it (the title bar is above it, and the
         * border is around it), so a maximized window shows its title and its buttons like any other */
        NexwmRect area = { wm.wx, wm.wy, wm.ww, wm.wh };
        NexwmRect r = client_in_area(c, area);
        c->x = r.x; c->y = r.y; c->w = r.w; c->h = r.h;
        c->maximized = 1;
        client_place(c);
        client_set_state(&wm, c);
        wm_log("maximized 0x%x '%s' (%dx%d at %d,%d: the workarea without the panel's struts and without its frame)",
               (unsigned)c->id, c->title, c->w, c->h, c->x, c->y);
    } else if (!on && c->maximized) {
        c->maximized = 0;
        if (c->has_saved) {
            c->x = c->sx; c->y = c->sy; c->w = c->sw; c->h = c->sh;
        }
        client_place(c);
        client_set_state(&wm, c);
        wm_log("unmaximized 0x%x '%s' (%dx%d at %d,%d)", (unsigned)c->id, c->title, c->w, c->h, c->x, c->y);
    }
}

static void action_snap(NexwmClient *c, int edge)
{
    if (!c) {
        wm_log("no window to snap");
        return;
    }
    if (!c->has_saved) {
        c->sx = c->x; c->sy = c->y; c->sw = c->w; c->sh = c->h;
        c->has_saved = 1;
    }
    c->maximized = 0;
    NexwmRect area = { wm.wx, wm.wy, wm.ww, wm.wh };
    switch (edge) {
    case NEXWM_EDGE_LEFT:
        area.w /= 2;
        break;
    case NEXWM_EDGE_RIGHT:
        area.w -= area.w / 2;
        area.x += wm.ww / 2;
        break;
    case NEXWM_EDGE_UP:
        area.h /= 2;
        break;
    default:
        area.h -= area.h / 2;
        area.y += wm.wh / 2;
        break;
    }
    NexwmRect r = client_in_area(c, area);
    c->x = r.x; c->y = r.y; c->w = r.w; c->h = r.h;
    client_place(c);
    client_set_state(&wm, c);
    static const char *names[] = { "left", "right", "up", "down" };
    wm_log("snapped %s: 0x%x '%s' (%dx%d at %d,%d)", names[edge], (unsigned)c->id, c->title, c->w, c->h, c->x, c->y);
}

/* the Minimize button (and the panel's taskbar): the window is unmapped and says _NET_WM_STATE_HIDDEN, the panel keeps
 * listing it, and clicking it once more brings it back */
static void action_minimize(NexwmClient *c)
{
    if (!c) {
        wm_log("no window to minimize");
        return;
    }
    if (c->hidden) {
        client_hide(c, 0);
        focus_client(c, 1);
        wm_log("0x%x '%s' is back from the taskbar", (unsigned)c->id, c->title);
        return;
    }
    client_hide(c, 1);
    wm_log("minimized 0x%x '%s' (the panel sees _NET_WM_STATE_HIDDEN)", (unsigned)c->id, c->title);
    if (wm.focused == c->id) {
        wm.focused = XCB_NONE;
        NexwmClient *n = client_next_in_desktop(c, 1);
        if (n && n != c) focus_client(n, 0);
        else ewmh_update_active(&wm);
    }
}

/* _NET_WM_STATE_ABOVE and _NET_WM_STATE_BELOW: "always on top" and its other way round (the panel offers the first) */
static void action_above(NexwmClient *c, int on)
{
    if (!c) {
        wm_log("no window for it");
        return;
    }
    c->above = on;
    if (on) c->below = 0;
    client_set_state(&wm, c);
    restack_clients();
    wm_log("0x%x '%s' is %s the other windows", (unsigned)c->id, c->title, on ? "now above" : "no longer above");
}

static void action_below(NexwmClient *c, int on)
{
    if (!c) {
        wm_log("no window for it");
        return;
    }
    c->below = on;
    if (on) c->above = 0;
    client_set_state(&wm, c);
    restack_clients();
    wm_log("0x%x '%s' is %s the other windows", (unsigned)c->id, c->title, on ? "now below" : "no longer below");
}

static void action_fullscreen(NexwmClient *c)
{
    if (!c) {
        wm_log("no window for full screen");
        return;
    }
    if (!c->fullscreen) {
        if (!c->has_saved) {
            c->sx = c->x; c->sy = c->y; c->sw = c->w; c->sh = c->h;
            c->has_saved = 1;
        }
        c->fullscreen = 1;
        c->maximized = 0;
        c->x = 0; c->y = 0; c->w = wm.screen_w; c->h = wm.screen_h;      /* the whole screen: no bar, no border */
        client_place(c);
        client_set_state(&wm, c);
        wm_log("full screen 0x%x '%s' (%dx%d)", (unsigned)c->id, c->title, c->w, c->h);
    } else {
        c->fullscreen = 0;
        if (c->has_saved) {
            c->x = c->sx; c->y = c->sy; c->w = c->sw; c->h = c->sh;
        }
        client_place(c);
        client_set_state(&wm, c);
        wm_log("0x%x '%s' back from full screen (%dx%d at %d,%d)", (unsigned)c->id, c->title, c->w, c->h, c->x,
               c->y);
    }
}

static void action_quit(void)
{
    wm_log("leaving the window manager");
    wm.running = 0;
}

static void action_run(HdeNexwmAction action, int arg, const char *command)
{
    NexwmClient *c = client_focused();
    switch (action) {
    case NEXWM_ACTION_SPAWN:      action_spawn(command); break;
    case NEXWM_ACTION_CLOSE:      action_close(c, 1); break;
    case NEXWM_ACTION_KILL:       action_close(c, 0); break;
    case NEXWM_ACTION_NEXT: {
        NexwmClient *n = client_next_in_desktop(c, 1);
        if (n && n != c) focus_client(n, 1);
        else wm_log("only one window here");
        break;
    }
    case NEXWM_ACTION_PREV: {
        NexwmClient *n = client_next_in_desktop(c, -1);
        if (n && n != c) focus_client(n, 1);
        else wm_log("only one window here");
        break;
    }
    case NEXWM_ACTION_WORKSPACE:  action_workspace(arg); break;
    case NEXWM_ACTION_MOVE_TO:    action_move_to(arg); break;
    case NEXWM_ACTION_MAXIMIZE:   action_maximize(c, 1); break;
    case NEXWM_ACTION_UNMAXIMIZE: action_maximize(c, 0); break;
    case NEXWM_ACTION_FULLSCREEN: action_fullscreen(c); break;
    case NEXWM_ACTION_MINIMIZE:   action_minimize(c); break;
    case NEXWM_ACTION_SNAP:       action_snap(c, arg); break;
    case NEXWM_ACTION_QUIT:       action_quit(); break;
    default: break;
    }
}

/* ---------------------------------------------------------------- the mouse on the frames */

/* how near the edge of the work area a window dropped there counts as "on that edge" (nexwm_frame_drop) */
#define NEXWM_DROP_MARGIN 12

/* what the pointer is on inside a frame: the button it is over is drawn lit, and the cursor says what a drag here would
 * do. Every motion over a frame comes through here, so it talks to the X server only when something changed. */
static void frame_pointer_update(NexwmClient *c, int root_x, int root_y)
{
    if (!c || !c->frame || !c->framed || c->dock || c->fullscreen) return;
    int button = 0;
    unsigned sides = 0;
    int hit = frame_hit_at(c, root_x, root_y, &button, &sides);
    int hover = hit == NEXWM_HIT_BUTTON ? button : 0;
    if (hover != c->hover) {
        c->hover = hover;
        frame_paint(c);
    }
    frame_cursor(c, cursor_of_hit(hit, sides));
}

/* the modifier keys the bindings use, with the lock ones taken out (Caps Lock is not part of what the user pressed) */
static unsigned mods_clean(unsigned state)
{
    for (int l = 1; l < wm.n_lock_masks; l++) state &= ~wm.lock_masks[l];
    return state & (XCB_MOD_MASK_4 | XCB_MOD_MASK_CONTROL | XCB_MOD_MASK_1 | XCB_MOD_MASK_SHIFT);
}

/* what a press on one of the buttons of the title bar does when the mouse lets go over it */
static void button_action(NexwmClient *c, int kind)
{
    switch (kind) {
    case NEXWM_BUTTON_MINIMIZE: action_minimize(c); break;
    case NEXWM_BUTTON_MAXIMIZE: action_maximize(c, !c->maximized); break;
    default:                    action_close(c, 1); break;
    }
}

/* A drag holds the pointer (a grab of the root window): the window has to follow the pointer even when the pointer has
 * left it. Without the grab the window under the pointer gets the motion, and a window dragged under the pointer would
 * stop moving. */
static void drag_grab(xcb_cursor_t cursor)
{
    uint32_t mask = XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_POINTER_MOTION;
    xcb_grab_pointer_reply_t *r = xcb_grab_pointer_reply(
        wm.conn, xcb_grab_pointer(wm.conn, 0, wm.root, mask, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC, XCB_NONE,
                                  cursor, XCB_CURRENT_TIME), NULL);
    if (!r) return;
    if (r->status != XCB_GRAB_STATUS_SUCCESS)
        wm_log("another program holds the pointer: the window follows it inside its frame only");
    free(r);
}

/* A maximized window that is taken by the title bar comes back to the size it had, under the pointer: the user grabbed
 * the bar of a window, not the whole work area. The pointer keeps the place it had in the frame — where it took hold is
 * where it holds on. */
static void drag_restore(NexwmClient *c, int px, int py)
{
    NexwmRect before = frame_rect(c);
    int fw0 = before.w > 0 ? before.w : 1;
    int fh0 = before.h > 0 ? before.h : 1;
    int rx = px - before.x;
    int ry = py - before.y;
    if (c->fullscreen) action_fullscreen(c);
    else action_maximize(c, 0);
    NexwmRect after = frame_rect(c);
    NexwmFrameStyle st = frame_style_of(c);
    int fw1 = after.w > 0 ? after.w : 1;
    int fh1 = after.h > 0 ? after.h : 1;
    int fx = px - (int)((long)rx * fw1 / fw0);             /* the frame back under the pointer */
    int fy = py - (int)((long)ry * fh1 / fh0);
    c->x = fx + st.border;
    c->y = fy + st.border + st.titlebar;
    client_place(c);
    wm.drag.restored = 1;
    wm_log("0x%x '%s': taken off the work area, back to %dx%d at %d,%d", (unsigned)c->id, c->title, c->w, c->h, c->x,
           c->y);
}

/* the sides of a resize as the two protocols that name them do (_NET_WM_MOVERESIZE: 0..7 clockwise from the top left) */
static unsigned sides_of_direction(unsigned direction)
{
    switch (direction) {
    case 0:  return NEXWM_SIDE_TOP | NEXWM_SIDE_LEFT;
    case 1:  return NEXWM_SIDE_TOP;
    case 2:  return NEXWM_SIDE_TOP | NEXWM_SIDE_RIGHT;
    case 3:  return NEXWM_SIDE_RIGHT;
    case 4:  return NEXWM_SIDE_BOTTOM | NEXWM_SIDE_RIGHT;
    case 5:  return NEXWM_SIDE_BOTTOM;
    case 6:  return NEXWM_SIDE_BOTTOM | NEXWM_SIDE_LEFT;
    default: return NEXWM_SIDE_LEFT;
    }
}

/* begin a drag: `move` 1 moves the window, 0 resizes it by `sides`; (px, py) is where the pointer was when it began */
static void drag_start(NexwmClient *c, int move, unsigned sides, int button, int px, int py)
{
    memset(&wm.drag, 0, sizeof wm.drag);
    wm.drag.c = c;
    wm.drag.active = 1;
    wm.drag.move = move;
    wm.drag.sides = sides;
    wm.drag.button = button;
    wm.drag.x0 = px;
    wm.drag.y0 = py;
    wm.drag.start.x = c->x;
    wm.drag.start.y = c->y;
    wm.drag.start.w = c->w;
    wm.drag.start.h = c->h;
    drag_grab(move ? wm.cursors[NEXWM_CURSOR_MOVE]
                   : (sides ? wm.cursors[cursor_of_hit(NEXWM_HIT_EDGE, sides)] : XCB_NONE));
    if (move) frame_cursor(c, NEXWM_CURSOR_MOVE);
    if (move || sides)
        wm_log("%s 0x%x '%s' with the mouse (button %d)", move ? "moving" : "resizing", (unsigned)c->id, c->title,
               button);
}

/* end it: the pointer is let go, and the cursor goes back to the plain one (the caller says what the pointer is on now) */
static void drag_stop(void)
{
    xcb_ungrab_pointer(wm.conn, XCB_CURRENT_TIME);
    NexwmClient *c = wm.drag.c;
    if (c && c->frame) frame_cursor(c, NEXWM_CURSOR_DEFAULT);
    memset(&wm.drag, 0, sizeof wm.drag);
}

static void drag_motion(xcb_motion_notify_event_t *ev)
{
    NexwmClient *c = wm.drag.c;
    if (!c || !c->framed) {
        drag_stop();
        return;
    }
    if (wm.drag.pressed) return;                       /* on a button of the bar: nothing moves until the mouse lets go */
    int dx = (int)ev->root_x - wm.drag.x0;
    int dy = (int)ev->root_y - wm.drag.y0;
    if (!dx && !dy) return;

    /* A window that fills the work area (maximized, full screen) comes back to the size it had once the mouse has really
     * moved: taking hold of the title bar of a maximized window is a click (that is what a click on it does — it gives
     * the window the focus), and it is the *drag* that says the window is meant to be moved or resized. It comes back
     * under the pointer, where the mouse took hold of it. */
    if (!wm.drag.restored && (c->maximized || c->fullscreen) && (dx > 8 || dx < -8 || dy > 8 || dy < -8)) {
        if (wm.drag.move) drag_restore(c, ev->root_x, ev->root_y);
        else if (c->fullscreen) action_fullscreen(c);
        else action_maximize(c, 0);
        wm.drag.x0 = ev->root_x;
        wm.drag.y0 = ev->root_y;
        wm.drag.start.x = c->x;
        wm.drag.start.y = c->y;
        wm.drag.start.w = c->w;
        wm.drag.start.h = c->h;
        wm.drag.restored = 1;
        return;                                        /* this motion took the window out of its place */
    }
    NexwmFrameStyle st = frame_style_of(c);
    int border = st.border;
    int bar = st.border + st.titlebar;                 /* from the top of the frame to the top of the window */
    if (wm.drag.move) {
        /* the frame the drag began with, moved by the pointer */
        int fx = wm.drag.start.x - border;
        int fy = wm.drag.start.y - border - st.titlebar;
        int fw = wm.drag.start.w + 2 * border;
        int nx = fx + dx;
        int ny = fy + dy;
        if (nx < wm.wx - fw + 40) nx = wm.wx - fw + 40;            /* 40 px of it stay on the screen... */
        if (nx > wm.wx + wm.ww - 40) nx = wm.wx + wm.ww - 40;
        if (ny < wm.wy)                                            /* ... and the bar stays on the work area: a window
                                                                    * that already sits above the edge (a program put it
                                                                    * there) keeps its place instead of jumping down */
            ny = (wm.drag.start.y - border - st.titlebar < wm.wy) ? wm.drag.start.y - border - st.titlebar : wm.wy;
        if (ny + bar > wm.wy + wm.wh) ny = wm.wy + wm.wh - bar;
        c->x = nx + border;
        c->y = ny + border + st.titlebar;
    } else {
        NexwmRect r = nexwm_frame_resize(wm.drag.start, wm.drag.sides, dx, dy, c->min_w, c->min_h);
        c->x = r.x;
        c->y = r.y;
        c->w = r.w;
        c->h = r.h;
    }
    client_place(c);
    wm.drag.moved = 1;
}

/* the mouse let go: a button of the title bar acts, a window dropped on the edge of the work area is maximized or takes
 * half of it, and anything else is just where it was left */
static void drag_finish(xcb_button_release_event_t *ev)
{
    NexwmClient *c = wm.drag.c;
    int move = wm.drag.move;
    int moved = wm.drag.moved;
    int pressed = wm.drag.pressed;
    int which = wm.drag.pressed_button;
    if (!c) {
        drag_stop();
        return;
    }
    if (pressed) {
        int button = 0;
        unsigned sides = 0;
        if (frame_hit_at(c, ev->root_x, ev->root_y, &button, &sides) == NEXWM_HIT_BUTTON && button == which)
            button_action(c, which);
        else
            wm_log("0x%x '%s': the mouse left the button before it let go, so nothing was asked", (unsigned)c->id,
                   c->title);
    } else if (move && moved) {
        NexwmRect work = { wm.wx, wm.wy, wm.ww, wm.wh };
        int drop = nexwm_frame_drop(&work, ev->root_x, ev->root_y, NEXWM_DROP_MARGIN);
        if (drop == NEXWM_DROP_MAXIMIZE) {
            wm_log("0x%x '%s' let go at the top of the work area", (unsigned)c->id, c->title);
            action_maximize(c, 1);
        } else if (drop == NEXWM_DROP_LEFT || drop == NEXWM_DROP_RIGHT) {
            wm_log("0x%x '%s' let go at the %s edge of the work area", (unsigned)c->id, c->title,
                   drop == NEXWM_DROP_LEFT ? "left" : "right");
            action_snap(c, drop == NEXWM_DROP_LEFT ? NEXWM_EDGE_LEFT : NEXWM_EDGE_RIGHT);
        } else {
            wm_log("moved 0x%x '%s' to %d,%d", (unsigned)c->id, c->title, c->x, c->y);
        }
    } else if (moved) {
        wm_log("resized 0x%x '%s' to %dx%d at %d,%d", (unsigned)c->id, c->title, c->w, c->h, c->x, c->y);
    }
    drag_stop();
    frame_pointer_update(c, ev->root_x, ev->root_y);   /* the pointer is somewhere: show what is under it */
}

/* the press of a button of the mouse: focus, then what it begins (a move, a resize, a button of the title bar) */
static void handle_button_press(xcb_button_press_event_t *b)
{
    NexwmClient *c = client_of_window(b->event);
    if (!c || c->dock || !c->framed) return;
    int on_frame = c->frame && b->event == c->frame;

    /* a click on a window is a click on that window: it comes to the front and takes the focus (the click itself is the
     * program's; this is only what every window manager does with it) */
    if (c->desktop == wm.desktop && !c->hidden) {
        if (wm.focused == c->id) raise_client(c);
        else focus_client(c, 1);
    }
    if (wm.drag.active) return;

    if (!on_frame) {
        /* Super and the first button moves a window from anywhere of it, Alt and the third one resizes it from its
         * bottom right: what a window manager offers when the window has nothing left to grab (no title bar, or a bar
         * that is off the screen) */
        unsigned mods = mods_clean(b->state);
        if (b->detail == 1 && mods == XCB_MOD_MASK_4) drag_start(c, 1, 0, 1, b->root_x, b->root_y);
        else if (b->detail == 3 && mods == XCB_MOD_MASK_1)
            drag_start(c, 0, NEXWM_SIDE_BOTTOM | NEXWM_SIDE_RIGHT, 3, b->root_x, b->root_y);
        return;
    }

    int button = 0;
    unsigned sides = 0;
    int hit = frame_hit_at(c, b->root_x, b->root_y, &button, &sides);
    if (hit == NEXWM_HIT_BUTTON) {
        if (b->detail != 1) return;
        drag_start(c, 0, 0, b->detail, b->root_x, b->root_y);
        wm.drag.pressed = 1;
        wm.drag.pressed_button = button;
        return;
    }
    if (hit == NEXWM_HIT_TITLE && b->detail == 1) {
        /* a double click on the bar gives the window the work area and takes it back: the same as the Maximize button */
        if (wm.last_click_window == c->id && wm.last_click_time != 0 && b->time - wm.last_click_time < 400) {
            wm.last_click_window = 0;
            wm.last_click_time = 0;
            action_maximize(c, !c->maximized);
            return;
        }
        wm.last_click_window = c->id;
        wm.last_click_time = b->time;
        drag_start(c, 1, 0, b->detail, b->root_x, b->root_y);
        return;
    }
    if (hit == NEXWM_HIT_EDGE) drag_start(c, 0, sides, b->detail, b->root_x, b->root_y);
}

/* ---------------------------------------------------------------- the keys */

static void keys_grab(Nexwm *w)
{
    /* the lock modifiers of this keyboard: Caps Lock is always Mod2's neighbour in the standard mapping, Num Lock
     * and Scroll Lock are wherever the mapping puts them (that is why they are looked up) */
    unsigned locks[8];
    int n_locks = 0;
    locks[n_locks++] = 0;
    locks[n_locks++] = XCB_MOD_MASK_LOCK;
    xcb_get_modifier_mapping_reply_t *mod = xcb_get_modifier_mapping_reply(w->conn, xcb_get_modifier_mapping(w->conn),
                                                                           NULL);
    unsigned extra = 0;
    if (mod) {
        int per = mod->keycodes_per_modifier;
        xcb_keycode_t *kc = xcb_get_modifier_mapping_keycodes(mod);
        for (int m = 0; m < 8; m++) {
            for (int k = 0; k < per; k++) {
                xcb_keycode_t code = kc[m * per + k];
                if (!code) continue;
                /* the keysym of that keycode: Num Lock (0xff7f) or Scroll Lock (0xff14) */
                xcb_get_keyboard_mapping_reply_t *km = xcb_get_keyboard_mapping_reply(
                    w->conn, xcb_get_keyboard_mapping(w->conn, code, 1), NULL);
                if (!km) continue;
                xcb_keysym_t *syms = xcb_get_keyboard_mapping_keysyms(km);
                int per_key = km->keysyms_per_keycode;
                for (int s = 0; s < per_key; s++) {
                    if (syms[s] == 0xff7f || syms[s] == 0xff14) extra |= (unsigned)(1u << m);
                }
                free(km);
            }
        }
        free(mod);
    }
    if (extra) {
        int base = n_locks;
        for (int i = 0; i < base; i++) locks[n_locks++] = locks[i] | extra;
    }
    w->n_lock_masks = n_locks;
    for (int i = 0; i < n_locks && i < 8; i++) w->lock_masks[i] = locks[i];

    /* the keyboard mapping: which keycode has the keysym of a binding */
    xcb_get_keyboard_mapping_reply_t *km = xcb_get_keyboard_mapping_reply(
        w->conn, xcb_get_keyboard_mapping(w->conn, 8, 248), NULL);
    if (!km) return;
    xcb_keysym_t *syms = xcb_get_keyboard_mapping_keysyms(km);
    int per = km->keysyms_per_keycode;

    unsigned base_mods[] = { XCB_MOD_MASK_4, XCB_MOD_MASK_CONTROL, XCB_MOD_MASK_1, XCB_MOD_MASK_SHIFT };
    for (size_t b = 0; b < w->cfg.n_keys; b++) {
        HdeNexwmBinding *bind = &w->cfg.keys[b];
        xcb_keycode_t code = 0;
        for (int k = 0; k < 248 && !code; k++) {
            for (int s = 0; s < per; s++) {
                if (syms[k * per + s] != bind->keysym) continue;
                /* bindings store letters as their base keysym; Shift is matched separately in bind->x11_mods */
                code = (xcb_keycode_t)(8 + k);
                break;
            }
        }
        if (!code) {
            wm_log("key %s: this keyboard has no key for '%s'", bind->combo, bind->key);
            continue;
        }
        unsigned mods = 0;
        if (bind->mods & NEXWM_MOD_SUPER) mods |= base_mods[0];
        if (bind->mods & NEXWM_MOD_CTRL)  mods |= base_mods[1];
        if (bind->mods & NEXWM_MOD_ALT)   mods |= base_mods[2];
        if (bind->mods & NEXWM_MOD_SHIFT) mods |= base_mods[3];
        bind->keycode = code;
        bind->x11_mods = mods;
        for (int l = 0; l < n_locks; l++)
            xcb_grab_key(w->conn, 0, w->root, (uint16_t)(mods | w->lock_masks[l]), code, XCB_GRAB_MODE_ASYNC,
                         XCB_GRAB_MODE_ASYNC);
        wm_log("key %s -> %s%s%s (keycode %u)", bind->combo, nexwm_action_name(bind->action),
               bind->action == NEXWM_ACTION_SPAWN ? " " : "", bind->command ? bind->command : "", code);
    }
    free(km);
}

static void keys_publish(Nexwm *w)
{
    /* _NEXWM_KEYS: every binding as one NUL-terminated string ("Super+Q close\0Super+Return spawn hde-choose terminal\0").
     * Anything that wants to show them (a Settings page, a help window) has the list the window manager really uses. */
    char *list = calloc(1, 1);
    size_t len = 0;
    for (size_t i = 0; i < w->cfg.n_keys; i++) {
        HdeNexwmBinding *b = &w->cfg.keys[i];
        char line[256];
        char arg[24] = "";                 /* the workspace number, or the edge of a snap: it is part of the binding */
        switch (b->action) {
        case NEXWM_ACTION_WORKSPACE:
        case NEXWM_ACTION_MOVE_TO: snprintf(arg, sizeof arg, " %d", b->arg + 1); break;
        case NEXWM_ACTION_SNAP: {
            static const char *edges[] = { "left", "right", "up", "down" };
            snprintf(arg, sizeof arg, " %s", edges[b->arg & 3]);
            break;
        }
        default: break;
        }
        int n = snprintf(line, sizeof line, "%s %s%s%s%s", b->combo, nexwm_action_name(b->action), arg,
                         b->command ? " " : "", b->command ? b->command : "");
        char *bigger = realloc(list, len + (size_t)n + 2);
        if (!bigger) break;
        list = bigger;
        memcpy(list + len, line, (size_t)n);
        len += (size_t)n;
        list[len++] = '\0';
    }
    xcb_change_property(w->conn, XCB_PROP_MODE_REPLACE, w->root, w->atoms[NEXWM_ATOM_NET_KEYS],
                        w->atoms[NEXWM_ATOM_UTF8_STRING], 8, (uint32_t)len, list);
    free(list);
}

static int key_is_binding(Nexwm *w, xcb_key_press_event_t *ev, HdeNexwmBinding **found)
{
    for (size_t i = 0; i < w->cfg.n_keys; i++) {
        HdeNexwmBinding *b = &w->cfg.keys[i];
        if (b->keycode == 0) continue;
        if (ev->detail != b->keycode) continue;
        unsigned want = b->x11_mods;
        unsigned have = ev->state;
        /* the lock bits are not part of what the user wrote */
        for (int l = 1; l < w->n_lock_masks; l++) {
            have &= ~w->lock_masks[l];
            want &= ~w->lock_masks[l];
        }
        if ((have & (XCB_MOD_MASK_4 | XCB_MOD_MASK_CONTROL | XCB_MOD_MASK_1 | XCB_MOD_MASK_SHIFT)) == want) {
            *found = b;
            return 1;
        }
    }
    return 0;
}

/* ---------------------------------------------------------------- the events */

static void handle_map_request(xcb_map_request_event_t *ev)
{
    if (client_of_window(ev->window)) {
        NexwmClient *c = client_of_window(ev->window);
        client_show_for_desktop(c);
        if (c->framed) focus_client(c, 1);
        return;
    }
    client_manage(ev->window);
}

/* The values of a ConfigureRequest follow its fixed part — the XCB event struct stops at value_mask and has no member
 * (and no accessor) for the list, so it is read from where the X server put it: right after those 32 bytes, in the order
 * of the mask's bits (X, Y, width, height, border width, sibling, stack mode), each one only when its bit is set. That
 * is why the position of a value is not the position of its bit. */
#define NEXWM_CONFIGURE_MASK_AT 26
#define NEXWM_CONFIGURE_VALUES_AT 32
static uint32_t configure_value(const xcb_configure_request_event_t *ev, uint32_t bit)
{
    static const uint32_t bits[] = { XCB_CONFIG_WINDOW_X, XCB_CONFIG_WINDOW_Y, XCB_CONFIG_WINDOW_WIDTH,
                                     XCB_CONFIG_WINDOW_HEIGHT, XCB_CONFIG_WINDOW_BORDER_WIDTH,
                                     XCB_CONFIG_WINDOW_SIBLING, XCB_CONFIG_WINDOW_STACK_MODE };
    _Static_assert(offsetof(xcb_configure_request_event_t, value_mask) == NEXWM_CONFIGURE_MASK_AT,
                   "the fixed part of a ConfigureRequest is 32 bytes (the values follow it)");
    const uint32_t *values = (const uint32_t *)(const void *)((const char *)ev + NEXWM_CONFIGURE_VALUES_AT);
    uint32_t nth = 0;
    for (size_t i = 0; i < sizeof bits / sizeof bits[0]; i++) {
        if (bits[i] == bit) return values[nth];
        if (ev->value_mask & bits[i]) nth++;
    }
    return 0;
}

static void handle_configure_request(xcb_configure_request_event_t *ev)
{
    NexwmClient *c = client_of_window(ev->window);
    if (!c) {
        /* a window we do not manage: give it exactly what it asked for */
        uint32_t values[7];
        int n = 0;
        if (ev->value_mask & XCB_CONFIG_WINDOW_X) values[n++] = (uint32_t)ev->x;
        if (ev->value_mask & XCB_CONFIG_WINDOW_Y) values[n++] = (uint32_t)ev->y;
        if (ev->value_mask & XCB_CONFIG_WINDOW_WIDTH) values[n++] = ev->width;
        if (ev->value_mask & XCB_CONFIG_WINDOW_HEIGHT) values[n++] = ev->height;
        if (ev->value_mask & XCB_CONFIG_WINDOW_BORDER_WIDTH) values[n++] = ev->border_width;
        if (ev->value_mask & XCB_CONFIG_WINDOW_SIBLING) values[n++] = ev->sibling;
        if (ev->value_mask & XCB_CONFIG_WINDOW_STACK_MODE)
            values[n++] = configure_value(ev, XCB_CONFIG_WINDOW_STACK_MODE);
        if (n) xcb_configure_window(wm.conn, ev->window, ev->value_mask, values);
        return;
    }
    if (c->maximized || c->fullscreen) {
        /* the size is ours while the window is maximized or full screen; the stack mode is still the client's */
        if (ev->value_mask & XCB_CONFIG_WINDOW_STACK_MODE) {
            uint32_t mode = configure_value(ev, XCB_CONFIG_WINDOW_STACK_MODE);
            xcb_configure_window(wm.conn, c->frame ? c->frame : c->id, XCB_CONFIG_WINDOW_STACK_MODE, &mode);
        }
        return;
    }
    if (ev->value_mask & XCB_CONFIG_WINDOW_WIDTH) c->w = ev->width;
    if (ev->value_mask & XCB_CONFIG_WINDOW_HEIGHT) c->h = ev->height;
    /* A program that asks to be at (x, y) means the window *and its frame*: a toolkit that moves a window of a
     * reparenting window manager moves the frame it was given. So the place asked for is the frame's, and the window
     * inside it sits where the frame puts it (the border, and the title bar under the top of the frame). */
    NexwmFrameStyle st = frame_style_of(c);
    int offx = c->framed ? st.border : 0;
    int offy = c->framed ? st.border + (st.titlebar > 0 ? st.titlebar : 0) : 0;
    if (ev->value_mask & XCB_CONFIG_WINDOW_X) c->x = (int)ev->x + offx;
    if (ev->value_mask & XCB_CONFIG_WINDOW_Y) c->y = (int)ev->y + offy;
    client_place(c);
    if (ev->value_mask & XCB_CONFIG_WINDOW_STACK_MODE) {
        uint32_t mode = configure_value(ev, XCB_CONFIG_WINDOW_STACK_MODE);
        xcb_configure_window(wm.conn, c->frame ? c->frame : c->id, XCB_CONFIG_WINDOW_STACK_MODE, &mode);
    }
}

static void handle_map_notify(xcb_map_notify_event_t *ev)
{
    NexwmClient *c = client_of_window(ev->window);
    if (!c || !c->framed || c->id != ev->window) return;   /* the frame of a window being mapped: our own doing */
    if (c->hidden) {
        /* we put the window away (the taskbar, or the Minimize button) and the program mapped it again: it is on the
         * screen now, so it is not hidden — and the panel is told the same thing */
        c->hidden = 0;
        client_set_state(&wm, c);
        wm_log("0x%x '%s' put itself back on the screen", (unsigned)c->id, c->title);
    }
}

static void handle_unmap_notify(xcb_unmap_notify_event_t *ev)
{
    NexwmClient *c = client_of_window(ev->window);
    if (!c) return;
    if (!c->mapped) return;                    /* the window manager itself put it away (another workspace, minimized) */
    /* the program unmapped its own window: it is gone from the screen, so it is not ours to manage any more */
    client_unmanage(c, 0);
}

static void handle_destroy_notify(xcb_destroy_notify_event_t *ev)
{
    NexwmClient *c = client_of_window(ev->window);
    if (c) client_unmanage(c, 1);
}

static void handle_configure_notify(xcb_configure_notify_event_t *ev)
{
    if (ev->window == wm.root) {
        /* the screen itself changed size (a resolution change in Settings, a projector plugged in) */
        wm.screen_w = ev->width;
        wm.screen_h = ev->height;
        workarea_update(&wm);
        wm_log("the screen is now %dx%d", wm.screen_w, wm.screen_h);
        return;
    }
    NexwmClient *c = client_of_window(ev->window);
    if (!c || ev->window != c->id) return;
    if (!c->framed) {
        c->x = ev->x;
        c->y = ev->y;
        c->w = ev->width;
        c->h = ev->height;
        return;
    }
    /* The size of a window that fills the work area is the window manager's: the same rule the requests above are
     * answered with. A program that resizes its window while it is maximized or full screen does not get to shrink its
     * own frame around it — the next maximize or full screen would put the work area back, and until then what the
     * screen shows and what the frame extents say stay what they were. */
    if (c->maximized || c->fullscreen) return;
    /* the client resized itself (a program that changed its own size): the frame follows — unless the size is one this
     * window manager asked for a moment ago, which makes the event the echo of its own request (the answer to the move
     * of a move-and-resize carries the size from before the resize, and following it would undo that resize) */
    if (c->w == ev->width && c->h == ev->height) return;
    if (client_wanted_size(c, ev->width, ev->height)) return;
    c->w = ev->width;
    c->h = ev->height;
    client_place(c);
}

static void handle_property_notify(xcb_property_notify_event_t *ev)
{
    NexwmClient *c = client_of_window(ev->window);
    if (!c) return;
    if (ev->atom == wm.atoms[NEXWM_ATOM_NET_WM_NAME] || ev->atom == wm.atoms[NEXWM_ATOM_WM_NAME]) {
        char before[200];
        snprintf(before, sizeof before, "%s", c->title);
        client_read_title(c);
        if (strcmp(before, c->title)) {
            wm_log("0x%x is now '%s'", (unsigned)c->id, c->title);
            frame_paint(c);                      /* the title bar shows what the program calls itself now */
        }
    } else if (ev->atom == wm.atoms[NEXWM_ATOM_NET_WM_ICON]) {
        client_read_icon(c);
        frame_paint(c);
    } else if (ev->atom == wm.atoms[NEXWM_ATOM_WM_NORMAL_HINTS]) {
        client_read_hints(c);                    /* a program that changes how small it lets itself get */
    } else if (ev->atom == wm.atoms[NEXWM_ATOM_NET_WM_STRUT] || ev->atom == wm.atoms[NEXWM_ATOM_NET_WM_STRUT_PARTIAL]) {
        workarea_update(&wm);
    } else if (ev->atom == wm.atoms[NEXWM_ATOM_NET_WM_WINDOW_TYPE]) {
        int dock = window_is_type(&wm, c->id, NEXWM_ATOM_NET_WM_WINDOW_TYPE_DOCK);
        if (dock != c->dock) {
            c->dock = dock;
            client_set_state(&wm, c);
            workarea_update(&wm);
        }
    }
}

static void handle_client_message(xcb_client_message_event_t *ev)
{
    if (ev->type == wm.atoms[NEXWM_ATOM_WM_PROTOCOLS] &&
        ev->data.data32[0] == wm.atoms[NEXWM_ATOM_NET_WM_PING]) {
        xcb_window_t id = (xcb_window_t)ev->data.data32[2];
        NexwmClient *pinged = client_of_window(id);
        if (pinged && pinged->id == id && pinged->ping_pending &&
            pinged->ping_timestamp == ev->data.data32[1] &&
            (ev->window == id || ev->window == wm.root)) {
            pinged->ping_pending = 0;
            pinged->ping_last_sent_ms = monotonic_ms();
            if (pinged->unresponsive) {
                pinged->unresponsive = 0;
                wm_log("0x%x '%s' is responding again", (unsigned)id, pinged->title);
            }
        }
        return;
    }
    NexwmClient *c = client_of_window(ev->window);
    if (ev->type == wm.atoms[NEXWM_ATOM_NET_CURRENT_DESKTOP]) {
        action_workspace((int)ev->data.data32[0]);
    } else if (ev->type == wm.atoms[NEXWM_ATOM_NET_ACTIVE_WINDOW]) {
        if (!c) return;
        if (c->desktop != wm.desktop) action_workspace(c->desktop);
        /* the taskbar of the panel asks for a window the same way the Minimize button brings one back: it is the same
         * path, so a window that was put away comes back on the screen (and the log says the same thing) */
        if (c->hidden) action_minimize(c);
        else focus_client(c, 1);
    } else if (ev->type == wm.atoms[NEXWM_ATOM_NET_CLOSE_WINDOW]) {
        action_close(c, 1);
    } else if (ev->type == wm.atoms[NEXWM_ATOM_NET_WM_STATE] && c) {
        /* _NET_WM_STATE: 0 remove, 1 add, 2 toggle; then the two properties asked about */
        uint32_t action = ev->data.data32[0];
        for (int i = 1; i <= 2; i++) {
            xcb_atom_t prop = ev->data.data32[i];
            if (prop == XCB_ATOM_NONE) continue;
            if (prop == wm.atoms[NEXWM_ATOM_NET_WM_STATE_FULLSCREEN]) {
                int want = action == 1 ? 1 : (action == 0 ? 0 : !c->fullscreen);
                if (want != c->fullscreen) action_fullscreen(c);
            } else if (prop == wm.atoms[NEXWM_ATOM_NET_WM_STATE_MAXIMIZED_VERT] ||
                       prop == wm.atoms[NEXWM_ATOM_NET_WM_STATE_MAXIMIZED_HORZ]) {
                int want = action == 1 ? 1 : (action == 0 ? 0 : !c->maximized);
                if (want != c->maximized) action_maximize(c, want);
            } else if (prop == wm.atoms[NEXWM_ATOM_NET_WM_STATE_HIDDEN]) {
                int want = action == 1 ? 1 : (action == 0 ? 0 : !c->hidden);
                if (c->framed && want != c->hidden) action_minimize(c);
            } else if (prop == wm.atoms[NEXWM_ATOM_NET_WM_STATE_ABOVE]) {
                action_above(c, action == 1 ? 1 : (action == 0 ? 0 : !c->above));
            } else if (prop == wm.atoms[NEXWM_ATOM_NET_WM_STATE_BELOW]) {
                action_below(c, action == 1 ? 1 : (action == 0 ? 0 : !c->below));
            }
        }
    } else if (ev->type == wm.atoms[NEXWM_ATOM_NET_WM_MOVERESIZE] && c) {
        /* a program (the panel, or a window that draws its own decorations) asks for a move or a resize with the mouse:
         * the same drag as a press on the title bar — the program took the press, so the button is the one it says */
        unsigned direction = ev->data.data32[2];
        if (direction == 11) {                             /* _NET_WM_MOVERESIZE_CANCEL */
            if (wm.drag.active && wm.drag.c == c) drag_stop();
        } else if (c->framed && !c->dock && c->desktop == wm.desktop && !c->hidden) {
            int button = (int)ev->data.data32[3] ? (int)ev->data.data32[3] : 1;
            if (direction == 8) drag_start(c, 1, 0, button, (int)ev->data.data32[0], (int)ev->data.data32[1]);
            else if (direction < 8)
                drag_start(c, 0, sides_of_direction(direction), button, (int)ev->data.data32[0],
                           (int)ev->data.data32[1]);
        }
    } else if (ev->type == wm.atoms[NEXWM_ATOM_WM_CHANGE_STATE] && c) {
        /* ICCCM: 3 = IconicState, the program asks to be minimized */
        if (ev->data.data32[0] == 3 && c->framed) client_hide(c, 1);
    }
}

static void handle_key_press(xcb_key_press_event_t *ev)
{
    HdeNexwmBinding *b = NULL;
    if (!key_is_binding(&wm, ev, &b)) return;
    wm_log("key %s: %s%s%s", b->combo, nexwm_action_name(b->action), b->command ? " " : "",
           b->command ? b->command : "");
    action_run(b->action, b->arg, b->command);
}

static void handle_event(xcb_generic_event_t *ev)
{
    switch (ev->response_type & 0x7f) {
    case XCB_MAP_REQUEST:        handle_map_request((xcb_map_request_event_t *)ev); break;
    case XCB_CONFIGURE_REQUEST:  handle_configure_request((xcb_configure_request_event_t *)ev); break;
    case XCB_MAP_NOTIFY:         handle_map_notify((xcb_map_notify_event_t *)ev); break;
    case XCB_UNMAP_NOTIFY:       handle_unmap_notify((xcb_unmap_notify_event_t *)ev); break;
    case XCB_DESTROY_NOTIFY:     handle_destroy_notify((xcb_destroy_notify_event_t *)ev); break;
    case XCB_CONFIGURE_NOTIFY:   handle_configure_notify((xcb_configure_notify_event_t *)ev); break;
    case XCB_PROPERTY_NOTIFY:    handle_property_notify((xcb_property_notify_event_t *)ev); break;
    case XCB_CLIENT_MESSAGE:     handle_client_message((xcb_client_message_event_t *)ev); break;
    case XCB_KEY_PRESS:          handle_key_press((xcb_key_press_event_t *)ev); break;
    case XCB_BUTTON_PRESS:       handle_button_press((xcb_button_press_event_t *)ev); break;
    case XCB_BUTTON_RELEASE: {
        if (wm.drag.active) drag_finish((xcb_button_release_event_t *)ev);
        break;
    }
    case XCB_MOTION_NOTIFY: {
        xcb_motion_notify_event_t *m = (xcb_motion_notify_event_t *)ev;
        if (wm.drag.active) {
            drag_motion(m);
            break;
        }
        NexwmClient *c = client_of_window(m->event);
        if (c) frame_pointer_update(c, m->root_x, m->root_y);
        break;
    }
    case XCB_EXPOSE: {
        /* the bar has to be drawn again: the window under it was covered, or it changed size (the new part of it is in
         * the background colour, which is not the bar) */
        xcb_expose_event_t *e = (xcb_expose_event_t *)ev;
        NexwmClient *c = client_of_window(e->window);
        if (c && c->frame == e->window && e->count == 0) frame_paint(c);
        break;
    }
    case XCB_ENTER_NOTIFY: {
        xcb_enter_notify_event_t *e = (xcb_enter_notify_event_t *)ev;
        if (e->mode != XCB_NOTIFY_MODE_NORMAL) break;               /* a grab, or a window that opened under the pointer */
        NexwmClient *c = client_of_window(e->event);
        if (!c) break;
        frame_pointer_update(c, e->root_x, e->root_y);
        if (!wm.cfg.focus_mouse) break;
        if (c->framed && !c->dock && !c->hidden && c->desktop == wm.desktop && wm.focused != c->id)
            focus_client(c, 1);
        break;
    }
    case XCB_LEAVE_NOTIFY: {
        xcb_leave_notify_event_t *e = (xcb_leave_notify_event_t *)ev;
        if (e->mode != XCB_NOTIFY_MODE_NORMAL) break;
        NexwmClient *c = client_of_window(e->event);
        if (!c || c->frame != e->event) break;
        if (c->hover) {                                             /* the pointer is not over a button any more */
            c->hover = 0;
            frame_paint(c);
        }
        frame_cursor(c, NEXWM_CURSOR_DEFAULT);
        break;
    }
    case XCB_FOCUS_IN: {
        xcb_focus_in_event_t *f = (xcb_focus_in_event_t *)ev;
        NexwmClient *c = client_of_window(f->event);
        if (c && wm.focused != c->id) {
            wm.focused = c->id;
            ewmh_update_active(&wm);
            frame_paint(c);
        }
        break;
    }
    case XCB_SELECTION_CLEAR: {
        xcb_selection_clear_event_t *s = (xcb_selection_clear_event_t *)ev;
        if (s->selection == wm.wm_sn && s->owner != wm.root) {
            wm_log("another window manager took over: leaving");
            wm.running = 0;
        }
        break;
    }
    default: break;
    }
}

/* ---------------------------------------------------------------- becoming the window manager */

/* the selection that says who the window manager is (WM_Sn on screen n) */
/* WM_S0 (or WM_S1, ...): the name of the window manager role on this screen. Interned before the screen is taken over
 * because taking it over from another window manager means asking that one to give the selection up (wm_take_over). */
static void wm_sn_intern(Nexwm *w, int screen_num)
{
    char name[16];
    snprintf(name, sizeof name, "WM_S%d", screen_num);
    xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(w->conn, xcb_intern_atom(w->conn, 0, (uint16_t)strlen(name), name),
                                                       NULL);
    if (r) {
        w->wm_sn = r->atom;
        free(r);
    }
}

/* ICCCM: being the window manager *is* holding the WM_Sn selection. This is called once the screen is ours — holding
 * SUBSTRUCTURE_REDIRECT is what makes it ours — and not before: a program that sets the selection while another window
 * manager holds it takes the role away from it, and a well behaved one (Metacity, for one) then gives the screen up and
 * exits. That is exactly what `--replace` asks for, and it happens in wm_take_over on purpose; a plain start must not do
 * it, or starting a second window manager by mistake would end the session's. */
static int wm_selection_claim(Nexwm *w)
{
    if (!w->wm_sn) return -1;
    xcb_set_selection_owner(w->conn, w->root, w->wm_sn, XCB_CURRENT_TIME);
    xcb_flush(w->conn);
    xcb_get_selection_owner_reply_t *owner = xcb_get_selection_owner_reply(
        w->conn, xcb_get_selection_owner(w->conn, w->wm_sn), NULL);
    int ours = owner && owner->owner == w->root;
    free(owner);
    return ours ? 0 : -1;
}

static int wm_take_over(Nexwm *w)
{
    uint32_t mask = XCB_CW_EVENT_MASK;
    uint32_t values = XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY |
                      XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_FOCUS_CHANGE |
                      XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_ENTER_WINDOW;

    /* another window manager is running: ask it to leave (the selection) and wait for the redirect to be free */
    int tries = w->replace ? 50 : 1;
    for (int i = 0; i < tries; i++) {
        xcb_generic_error_t *err = xcb_request_check(w->conn, xcb_change_window_attributes_checked(w->conn, w->root,
                                                                                                  mask, &values));
        if (!err) return 0;
        free(err);
        /* the error says Access: someone else is the window manager of this screen */
        if (i == 0 && w->replace) {
            wm_log("another window manager is running: asking it to hand over (--replace)");
            xcb_set_selection_owner(w->conn, XCB_NONE, w->wm_sn, XCB_CURRENT_TIME);
            /* the WM_Sn selection is not claimed by every window manager; the redirect mask is what matters */
            xcb_change_window_attributes(w->conn, w->root, mask, &values);
        }
        struct timespec ts = { 0, 100 * 1000 * 1000 };
        nanosleep(&ts, NULL);
        if (i && i % 10 == 0) wm_log("still waiting for the other window manager to leave (%d)", i + 1);
    }
    return -1;
}

static void scan_existing_windows(Nexwm *w)
{
    xcb_query_tree_reply_t *tree = xcb_query_tree_reply(w->conn, xcb_query_tree(w->conn, w->root), NULL);
    if (!tree) return;
    int n = xcb_query_tree_children_length(tree);
    xcb_window_t *children = xcb_query_tree_children(tree);
    for (int i = 0; i < n; i++) {
        xcb_window_t win = children[i];
        if (win == w->wm_check) continue;
        xcb_get_window_attributes_reply_t *attr = xcb_get_window_attributes_reply(
            w->conn, xcb_get_window_attributes(w->conn, win), NULL);
        if (!attr) continue;
        int viewable = attr->map_state == XCB_MAP_STATE_VIEWABLE && !attr->override_redirect;
        free(attr);
        if (viewable) client_manage(win);
    }
    free(tree);
}

static void wm_cleanup(Nexwm *w)
{
    if (wm.drag.active) drag_stop();
    for (size_t i = 0; i < w->n; i++) {
        NexwmClient *c = w->clients[i];
        if (c->frame) {
            xcb_reparent_window(w->conn, c->id, w->root, (int16_t)c->x, (int16_t)c->y);
            xcb_destroy_window(w->conn, c->frame);
        }
        prop_delete(w, c->id, w->atoms[NEXWM_ATOM_NET_WM_STATE]);
        prop_delete(w, c->id, w->atoms[NEXWM_ATOM_NET_FRAME_EXTENTS]);
        free(c->icon_alloc);
        free(c);
    }
    w->n = 0;
    prop_delete(w, w->root, w->atoms[NEXWM_ATOM_NET_SUPPORTED]);
    prop_delete(w, w->root, w->atoms[NEXWM_ATOM_NET_CLIENT_LIST]);
    prop_delete(w, w->root, w->atoms[NEXWM_ATOM_NET_ACTIVE_WINDOW]);
    prop_delete(w, w->root, w->atoms[NEXWM_ATOM_NET_SUPPORTING_WM_CHECK]);
    prop_delete(w, w->root, w->atoms[NEXWM_ATOM_NET_KEYS]);
    if (w->wm_check) xcb_destroy_window(w->conn, w->wm_check);
    for (int i = 0; i < NEXWM_CURSOR_COUNT; i++)
        if (w->cursors[i]) xcb_free_cursor(w->conn, w->cursors[i]);
    if (w->cursor_font) xcb_close_font(w->conn, w->cursor_font);
    if (w->font) xcb_close_font(w->conn, w->font);
    if (w->gc) xcb_free_gc(w->conn, w->gc);
    xcb_set_input_focus(w->conn, XCB_INPUT_FOCUS_POINTER_ROOT, XCB_NONE, XCB_CURRENT_TIME);
    xcb_flush(w->conn);
    /* Flush only sends the hand-back requests. Round-trip once so the X server has applied the reparents and property
     * deletes before the client connection closes; the next session manager may inspect the root as soon as we exit. */
    xcb_get_input_focus_reply_t *barrier = xcb_get_input_focus_reply(w->conn, xcb_get_input_focus(w->conn), NULL);
    free(barrier);
}

static void wm_setup_check_window(Nexwm *w)
{
    w->wm_check = xcb_generate_id(w->conn);
    uint32_t values[] = { XCB_EVENT_MASK_PROPERTY_CHANGE };
    xcb_create_window(w->conn, XCB_COPY_FROM_PARENT, w->wm_check, w->root, -1, -1, 1, 1, 0,
                      XCB_WINDOW_CLASS_INPUT_ONLY, XCB_COPY_FROM_PARENT, XCB_CW_EVENT_MASK, values);
    prop_set_window(w, w->root, w->atoms[NEXWM_ATOM_NET_SUPPORTING_WM_CHECK], w->wm_check);
    prop_set_window(w, w->wm_check, w->atoms[NEXWM_ATOM_NET_SUPPORTING_WM_CHECK], w->wm_check);
    prop_set_string(w, w->wm_check, w->atoms[NEXWM_ATOM_NET_WM_NAME], w->atoms[NEXWM_ATOM_UTF8_STRING], NEXWM_NAME);
    prop_set_string(w, w->wm_check, w->atoms[NEXWM_ATOM_WM_NAME], XCB_ATOM_STRING, NEXWM_NAME);
}

/* ---------------------------------------------------------------- the run loop */

int nexwm_x11_run(const char *config_path, int replace)
{
    memset(&wm, 0, sizeof wm);
    wm.running = 1;
    wm.replace = replace;
    wm.config_path = config_path;
    wm.focused = XCB_NONE;

    nexwm_config_defaults(&wm.cfg);
    if (config_path) {
        char err[256];
        if (nexwm_config_load(&wm.cfg, config_path, err, sizeof err) != 0) {
            fprintf(stderr, "nexwm: %s: %s\n", config_path, err);
            nexwm_config_free(&wm.cfg);
            return 2;
        }
    }

    int screen_num = 0;
    wm.conn = xcb_connect(NULL, &screen_num);
    if (xcb_connection_has_error(wm.conn)) {
        fprintf(stderr, "nexwm: cannot open the display %s\n", getenv("DISPLAY") ? getenv("DISPLAY") : "(none)");
        xcb_disconnect(wm.conn);
        nexwm_config_free(&wm.cfg);
        return 3;
    }
    const xcb_setup_t *setup = xcb_get_setup(wm.conn);
    xcb_screen_iterator_t it = xcb_setup_roots_iterator(setup);
    for (int i = 0; i < screen_num; i++) xcb_screen_next(&it);
    wm.screen = it.data;
    wm.root = wm.screen->root;
    wm.screen_w = wm.screen->width_in_pixels;
    wm.screen_h = wm.screen->height_in_pixels;

    atom_intern_all(&wm);

    wm_sn_intern(&wm, screen_num);
    if (wm_take_over(&wm) != 0) {
        fprintf(stderr, "nexwm: another window manager is running on %s\n"
                        "       (log out, or start NexWM with --replace to take over)\n",
                getenv("DISPLAY") ? getenv("DISPLAY") : "(no display)");
        xcb_disconnect(wm.conn);
        nexwm_config_free(&wm.cfg);
        return 3;
    }
    if (wm_selection_claim(&wm) != 0)
        wm_log("the WM_S%d selection could not be taken", screen_num);

    /* the pixel values of the frames: on a TrueColor screen (every screen this runs on) the pixel is the colour */
    wm.pixel_border = wm.cfg.border_color;
    wm.pixel_focus = wm.cfg.focus_color;

    wm_log("%s %s on %s (screen %d: %dx%d)", NEXWM_NAME, NEXWM_RELEASE,
           getenv("DISPLAY") ? getenv("DISPLAY") : "?", screen_num, wm.screen_w, wm.screen_h);
    /* the one GC every frame is drawn with (its colour and its font change between the drawings), the core font of the
     * titles, and the cursors of the mouse out of the font the X server has always had */
    wm.gc = xcb_generate_id(wm.conn);
    uint32_t gc_mask = XCB_GC_FOREGROUND | XCB_GC_BACKGROUND | XCB_GC_GRAPHICS_EXPOSURES;
    uint32_t gc_values[3] = { (uint32_t)wm.pixel_border, (uint32_t)wm.pixel_border, 0 };
    xcb_create_gc(wm.conn, wm.gc, wm.root, gc_mask, gc_values);
    title_font_open();
    if (wm.font) {
        uint32_t font = wm.font;
        xcb_change_gc(wm.conn, wm.gc, XCB_GC_FONT, &font);
    }
    cursors_create();

    char buttons[64] = "none";
    if (wm.cfg.titlebar > 0 && wm.cfg.n_buttons > 0) {
        size_t used = 0;
        buttons[0] = '\0';
        for (int i = 0; i < wm.cfg.n_buttons && used + 8 < sizeof buttons; i++) {
            const char *name = wm.cfg.buttons[i] == NEXWM_BUTTON_MINIMIZE ? "min"
                               : (wm.cfg.buttons[i] == NEXWM_BUTTON_MAXIMIZE ? "max" : "close");
            used += (size_t)snprintf(buttons + used, sizeof buttons - used, "%s%s", i ? "," : "", name);
        }
    }
    wm_log("frame %d px, title bar %d px (buttons %s), %s to focus, %d workspaces%s", wm.cfg.border, wm.cfg.titlebar,
           buttons, wm.cfg.focus_mouse ? "the mouse" : "a click", wm.cfg.desktops,
           replace ? ", taking over from another window manager" : "");
    if (config_path) wm_log("configuration: %s (%d keys)", config_path, (int)wm.cfg.n_keys);

    wm.wx = 0; wm.wy = 0; wm.ww = wm.screen_w; wm.wh = wm.screen_h;
    wm_setup_check_window(&wm);
    ewmh_update_supported(&wm);
    ewmh_update_desktops(&wm);
    ewmh_update_workarea(&wm);
    ewmh_update_client_list(&wm);
    ewmh_update_active(&wm);
    uint32_t geom[2] = { (uint32_t)wm.screen_w, (uint32_t)wm.screen_h };
    prop_set_cardinal(&wm, wm.root, wm.atoms[NEXWM_ATOM_NET_DESKTOP_GEOMETRY], geom, 2);
    uint32_t viewport[2] = { 0, 0 };
    prop_set_cardinal(&wm, wm.root, wm.atoms[NEXWM_ATOM_NET_DESKTOP_VIEWPORT], viewport, 2);

    /* the keys: resolve the keycodes once, keep them on the bindings (key_is_binding reads them back) */
    wm.n_lock_masks = 1;
    wm.lock_masks[0] = 0;
    keys_grab(&wm);
    keys_publish(&wm);

    scan_existing_windows(&wm);
    if (wm.n == 0) wm_log("no windows yet: waiting for the session's programs");
    xcb_flush(wm.conn);

    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGCHLD, SIG_IGN);         /* the programs a key started: they are not ours to wait for */
    signal(SIGHUP, on_signal);

    int fd = xcb_get_file_descriptor(wm.conn);
    while (wm.running) {
        xcb_generic_event_t *ev;
        int had = 0;
        while ((ev = xcb_poll_for_event(wm.conn))) {
            had = 1;
            handle_event(ev);
            free(ev);
        }
        if (got_signal) {
            wm_log("asked to leave (a signal)");
            break;
        }
        clients_check_responsive();
        if (had) xcb_flush(wm.conn);
        else {
            struct pollfd pfd = { fd, POLLIN, 0 };
            poll(&pfd, 1, 100);
        }
    }

    wm_log("leaving: %d window(s) given back", (int)wm.n);
    wm_cleanup(&wm);
    xcb_disconnect(wm.conn);
    nexwm_config_free(&wm.cfg);
    return 0;
}

#else  /* !NEXWM_HAVE_XCB: the Makefile found no libxcb, so there is no window manager in this program (the rest of
        * NexWM — --version, --help, the configuration file — works, and `nexwm --version` says what is missing) */

int nexwm_x11_run(const char *config_path, int replace)
{
    (void)config_path;
    (void)replace;
    fprintf(stderr,
            "nexwm: this build has no X11 window manager: it was made without libxcb.\n"
            "       Install it and build again — Debian/Ubuntu: sudo apt install libxcb1-dev; Fedora: sudo dnf\n"
            "       install libxcb-devel.\n");
    return 3;
}

#endif /* NEXWM_HAVE_XCB */
