/* x11.c — NexWM on an X11 session: the window manager (nexwm --x11, or plain `nexwm` when $DISPLAY is set).
 *
 * What it is: a small but real ICCCM/EWMH window manager, built on XCB alone (no libX11, no xcb-util: the atoms are
 * interned here, the key mapping is read here). It frames the windows it manages (a border of `border` px, the focused
 * one in another colour), gives them the focus, moves them between workspaces, maximizes, snaps and closes them, and
 * tells the rest of the desktop about all of it through the EWMH properties the panel, the taskbar and the task
 * managers read: _NET_CLIENT_LIST, _NET_ACTIVE_WINDOW, _NET_CURRENT_DESKTOP, _NET_WORKAREA (the struts of a panel are
 * honoured, which is how HDE's panel keeps maximized windows clear of itself), _NET_WM_STATE, _NET_SUPPORTING_WM_CHECK
 * ("NexWM"), and _NEXWM_KEYS with the key bindings it listens to.
 *
 * Not there yet (each one is a step of its own): title bars drawn with GTK, the mouse moving and resizing windows,
 * window rules, and a compositor for shadows and transparency.
 *
 * Everything it does that can be seen from outside is logged on stdout with the "nexwm: " prefix (the display and its
 * size, every window it manages, the focus, the workspaces, the frames, the keys) — tests/nexwm-test.sh reads those
 * lines and asks the X server the same questions with xprop/xdotool.
 */
#define _POSIX_C_SOURCE 200809L

#include "nexwm.h"

#include <xcb/xcb.h>

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

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
    char title[200];
    char class_name[120];
} NexwmClient;

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
    };
    prop_set_atoms(w, c->id, w->atoms[NEXWM_ATOM_NET_WM_ALLOWED_ACTIONS], list, sizeof list / sizeof list[0]);
}

/* the frame is the border (HDE has no title bar here yet): _NET_FRAME_EXTENTS tells the programs how thick it is */
static void client_set_frame_extents(Nexwm *w, NexwmClient *c)
{
    uint32_t e[4] = { (uint32_t)(c->framed ? w->cfg.border : 0), (uint32_t)(c->framed ? w->cfg.border : 0),
                      (uint32_t)(c->framed ? w->cfg.border : 0), (uint32_t)(c->framed ? w->cfg.border : 0) };
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

static void frame_paint(NexwmClient *c)
{
    if (!c->frame) return;
    uint32_t pixel = (wm.focused == c->id) ? wm.pixel_focus : wm.pixel_border;
    xcb_change_window_attributes(wm.conn, c->frame, XCB_CW_BACK_PIXEL, &pixel);
    xcb_clear_area(wm.conn, 1, c->frame, 0, 0, (uint16_t)(c->w + 2 * wm.cfg.border),
                   (uint16_t)(c->h + 2 * wm.cfg.border));
}

/* move and size the frame and the client inside it (the client is at (border, border)) */
static void client_place(NexwmClient *c)
{
    if (!c->framed) {
        uint32_t v[4] = { (uint32_t)c->x, (uint32_t)c->y, (uint32_t)c->w, (uint32_t)c->h };
        xcb_configure_window(wm.conn, c->id, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH |
                                             XCB_CONFIG_WINDOW_HEIGHT, v);
        return;
    }
    uint32_t f[4] = { (uint32_t)(c->x - wm.cfg.border), (uint32_t)(c->y - wm.cfg.border),
                      (uint32_t)(c->w + 2 * wm.cfg.border), (uint32_t)(c->h + 2 * wm.cfg.border) };
    xcb_configure_window(wm.conn, c->frame, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH |
                                             XCB_CONFIG_WINDOW_HEIGHT, f);
    uint32_t i[2] = { (uint32_t)wm.cfg.border, (uint32_t)wm.cfg.border };
    xcb_configure_window(wm.conn, c->id, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, i);
    uint32_t s[2] = { (uint32_t)c->w, (uint32_t)c->h };
    xcb_configure_window(wm.conn, c->id, XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, s);
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

static void focus_client(NexwmClient *c, int raise_it)
{
    xcb_window_t was = wm.focused;
    wm.focused = c ? c->id : XCB_NONE;
    if (c) {
        if (raise_it && c->frame) {
            uint32_t mode = XCB_STACK_MODE_ABOVE;
            xcb_configure_window(wm.conn, c->frame, XCB_CONFIG_WINDOW_STACK_MODE, &mode);
        }
        /* the keyboard goes to the client itself, so that every program sees a normal focus */
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
    c->frame = xcb_generate_id(wm.conn);
    uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
    uint32_t values[2];
    values[0] = (uint32_t)wm.pixel_border;
    values[1] = XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY | XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_BUTTON_PRESS |
                XCB_EVENT_MASK_EXPOSURE;
    xcb_create_window(wm.conn, XCB_COPY_FROM_PARENT, c->frame, wm.root, (int16_t)(c->x - wm.cfg.border),
                      (int16_t)(c->y - wm.cfg.border), (uint16_t)(c->w + 2 * wm.cfg.border),
                      (uint16_t)(c->h + 2 * wm.cfg.border), 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, XCB_COPY_FROM_PARENT,
                      mask, values);
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

    /* which workspace: the window itself may say (_NET_WM_DESKTOP), a dock is on all of them */
    uint32_t want = prop_get_cardinal(&wm, id, wm.atoms[NEXWM_ATOM_NET_WM_DESKTOP], 0xffffffffu);
    if (!c->dock && !desktop_win && want != 0xffffffffu && (int)want < wm.cfg.desktops) c->desktop = (int)want;

    uint32_t events[] = { XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_STRUCTURE_NOTIFY |
                          XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_FOCUS_CHANGE };
    if (c->framed) {
        client_frame_create(c);
        xcb_reparent_window(wm.conn, id, c->frame, (int16_t)wm.cfg.border, (int16_t)wm.cfg.border);
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

static void client_unmanage(NexwmClient *c, int destroyed)
{
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
        xcb_client_message_event_t ev;
        memset(&ev, 0, sizeof ev);
        ev.response_type = XCB_CLIENT_MESSAGE;
        ev.window = c->id;
        ev.type = wm.atoms[NEXWM_ATOM_WM_PROTOCOLS];
        ev.format = 32;
        ev.data.data32[0] = wm.atoms[NEXWM_ATOM_WM_DELETE_WINDOW];
        ev.data.data32[1] = XCB_CURRENT_TIME;
        xcb_send_event(wm.conn, 0, c->id, XCB_EVENT_MASK_NO_EVENT, (const char *)&ev);
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
        c->x = wm.wx; c->y = wm.wy; c->w = wm.ww; c->h = wm.wh;
        c->maximized = 1;
        client_place(c);
        client_set_state(&wm, c);
        wm_log("maximized 0x%x '%s' (%dx%d at %d,%d: the workarea, the panel's struts left out)", (unsigned)c->id,
               c->title, c->w, c->h, c->x, c->y);
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
    switch (edge) {
    case NEXWM_EDGE_LEFT:  c->x = wm.wx;                c->y = wm.wy; c->w = wm.ww / 2; c->h = wm.wh; break;
    case NEXWM_EDGE_RIGHT: c->x = wm.wx + wm.ww / 2;    c->y = wm.wy; c->w = wm.ww - wm.ww / 2; c->h = wm.wh; break;
    case NEXWM_EDGE_UP:    c->x = wm.wx; c->y = wm.wy;                c->w = wm.ww; c->h = wm.wh / 2; break;
    default:               c->x = wm.wx; c->y = wm.wy + wm.wh / 2;    c->w = wm.ww; c->h = wm.wh - wm.wh / 2; break;
    }
    client_place(c);
    client_set_state(&wm, c);
    static const char *names[] = { "left", "right", "up", "down" };
    wm_log("snapped %s: 0x%x '%s' (%dx%d at %d,%d)", names[edge], (unsigned)c->id, c->title, c->w, c->h, c->x, c->y);
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
        c->x = 0; c->y = 0; c->w = wm.screen_w; c->h = wm.screen_h;
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
    case NEXWM_ACTION_SNAP:       action_snap(c, arg); break;
    case NEXWM_ACTION_QUIT:       action_quit(); break;
    default: break;
    }
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
                /* the same key with Shift is another keysym (a and A): both spellings are grabbed */
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
    /* _NEXWM_KEYS: every binding as one NUL-terminated string ("Super+Q close\0Super+Return spawn xterm\0"), so that
     * anything that wants to show them (a Settings page, a help window) has the list the window manager really uses */
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
        if (ev->value_mask & XCB_CONFIG_WINDOW_STACK_MODE) values[n++] = ev->stack;
        if (n) xcb_configure_window(wm.conn, ev->window, ev->value_mask, values);
        return;
    }
    if (c->maximized || c->fullscreen) {
        /* the size is ours while the window is maximized or full screen; the stack mode is still the client's */
        if (ev->value_mask & XCB_CONFIG_WINDOW_STACK_MODE) {
            uint32_t mode = ev->stack;
            xcb_configure_window(wm.conn, c->frame ? c->frame : c->id, XCB_CONFIG_WINDOW_STACK_MODE, &mode);
        }
        return;
    }
    if (ev->value_mask & XCB_CONFIG_WINDOW_WIDTH) c->w = ev->width;
    if (ev->value_mask & XCB_CONFIG_WINDOW_HEIGHT) c->h = ev->height;
    if (ev->value_mask & XCB_CONFIG_WINDOW_X) c->x = ev->x + (c->framed ? wm.cfg.border : 0);
    if (ev->value_mask & XCB_CONFIG_WINDOW_Y) c->y = ev->y + (c->framed ? wm.cfg.border : 0);
    client_place(c);
    if (ev->value_mask & XCB_CONFIG_WINDOW_STACK_MODE)
        xcb_configure_window(wm.conn, c->frame ? c->frame : c->id, XCB_CONFIG_WINDOW_STACK_MODE, &ev->stack);
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
    /* the client resized itself (a program that changed its own size): the frame follows */
    if (c->w == ev->width && c->h == ev->height) return;
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
        if (strcmp(before, c->title)) wm_log("0x%x is now '%s'", (unsigned)c->id, c->title);
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
    NexwmClient *c = client_of_window(ev->window);
    if (ev->type == wm.atoms[NEXWM_ATOM_NET_CURRENT_DESKTOP]) {
        action_workspace((int)ev->data.data32[0]);
    } else if (ev->type == wm.atoms[NEXWM_ATOM_NET_ACTIVE_WINDOW]) {
        if (!c) return;
        if (c->desktop != wm.desktop) action_workspace(c->desktop);
        if (c->hidden) client_hide(c, 0);
        focus_client(c, 1);
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
                if (c->framed) client_hide(c, want);
            }
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
    case XCB_UNMAP_NOTIFY:       handle_unmap_notify((xcb_unmap_notify_event_t *)ev); break;
    case XCB_DESTROY_NOTIFY:     handle_destroy_notify((xcb_destroy_notify_event_t *)ev); break;
    case XCB_CONFIGURE_NOTIFY:   handle_configure_notify((xcb_configure_notify_event_t *)ev); break;
    case XCB_PROPERTY_NOTIFY:    handle_property_notify((xcb_property_notify_event_t *)ev); break;
    case XCB_CLIENT_MESSAGE:     handle_client_message((xcb_client_message_event_t *)ev); break;
    case XCB_KEY_PRESS:          handle_key_press((xcb_key_press_event_t *)ev); break;
    case XCB_BUTTON_PRESS: {
        xcb_button_press_event_t *b = (xcb_button_press_event_t *)ev;
        NexwmClient *c = client_of_window(b->event);
        if (c && c->framed && !c->dock) focus_client(c, 1);
        break;
    }
    case XCB_ENTER_NOTIFY: {
        xcb_enter_notify_event_t *e = (xcb_enter_notify_event_t *)ev;
        if (!wm.cfg.focus_mouse) break;
        NexwmClient *c = client_of_window(e->event);
        if (c && c->framed && !c->dock && !c->hidden && c->desktop == wm.desktop && wm.focused != c->id)
            focus_client(c, 1);
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
static int wm_selection_claim(Nexwm *w, int screen_num)
{
    char name[16];
    snprintf(name, sizeof name, "WM_S%d", screen_num);
    xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(w->conn, xcb_intern_atom(w->conn, 0, (uint16_t)strlen(name), name),
                                                       NULL);
    if (!r) return -1;
    w->wm_sn = r->atom;
    free(r);
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
    for (size_t i = 0; i < w->n; i++) {
        NexwmClient *c = w->clients[i];
        if (c->frame) {
            xcb_reparent_window(w->conn, c->id, w->root, (int16_t)c->x, (int16_t)c->y);
            xcb_destroy_window(w->conn, c->frame);
        }
        prop_delete(w, c->id, w->atoms[NEXWM_ATOM_NET_WM_STATE]);
        prop_delete(w, c->id, w->atoms[NEXWM_ATOM_NET_FRAME_EXTENTS]);
        free(c);
    }
    w->n = 0;
    prop_delete(w, w->root, w->atoms[NEXWM_ATOM_NET_SUPPORTED]);
    prop_delete(w, w->root, w->atoms[NEXWM_ATOM_NET_CLIENT_LIST]);
    prop_delete(w, w->root, w->atoms[NEXWM_ATOM_NET_ACTIVE_WINDOW]);
    prop_delete(w, w->root, w->atoms[NEXWM_ATOM_NET_SUPPORTING_WM_CHECK]);
    prop_delete(w, w->root, w->atoms[NEXWM_ATOM_NET_KEYS]);
    if (w->wm_check) xcb_destroy_window(w->conn, w->wm_check);
    xcb_set_input_focus(w->conn, XCB_INPUT_FOCUS_POINTER_ROOT, XCB_NONE, XCB_CURRENT_TIME);
    xcb_flush(w->conn);
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

    if (wm_selection_claim(&wm, screen_num) != 0)
        wm_log("the WM_S%d selection is held by another window manager", screen_num);
    if (wm_take_over(&wm) != 0) {
        fprintf(stderr, "nexwm: another window manager is running on %s\n"
                        "       (log out, or start NexWM with --replace to take over)\n",
                getenv("DISPLAY") ? getenv("DISPLAY") : "(no display)");
        xcb_disconnect(wm.conn);
        nexwm_config_free(&wm.cfg);
        return 3;
    }

    /* the pixel values of the frames: on a TrueColor screen (every screen this runs on) the pixel is the colour */
    wm.pixel_border = wm.cfg.border_color;
    wm.pixel_focus = wm.cfg.focus_color;

    wm_log("%s %s on %s (screen %d: %dx%d)", NEXWM_NAME, NEXWM_RELEASE,
           getenv("DISPLAY") ? getenv("DISPLAY") : "?", screen_num, wm.screen_w, wm.screen_h);
    wm_log("frame %d px, %s to focus, %d workspaces%s", wm.cfg.border, wm.cfg.focus_mouse ? "the mouse" : "a click",
           wm.cfg.desktops, replace ? ", taking over from another window manager" : "");
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
