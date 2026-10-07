/* tests/nexwm-client.c — a window for tests/nexwm-test.sh to put a window manager to work on.
 *
 * A window manager needs windows: this is a plain X client (XCB, nothing else) that puts one on the screen and then
 * stays there, so the test can look at what NexWM does with it — the frame around it, where it is, its size after a
 * maximize, a snap, a full screen, whether it moved to another workspace, and whether it comes back.
 *
 *   nexwm-client                     a 600x400 window titled "nexwm-client"
 *   nexwm-client --title T           its _NET_WM_NAME and WM_NAME
 *   nexwm-client --class C           its WM_CLASS (the instance and the class, both C)
 *   nexwm-client --size 800x600      the size it asks for
 *   nexwm-client --pos 200,150       where it asks to be (NexWM keeps a window where it was: X11 programs place
 *                                    themselves, the window manager frames them where they are)
 *   nexwm-client --dock 24           a dock instead: _NET_WM_WINDOW_TYPE_DOCK with a strut of 24 pixels at the top
 *                                    (what HDE's panel is to the window manager: room reserved on the screen)
 *   nexwm-client --timeout 60        leave by itself after N seconds (the test never hangs because of this window)
 *
 * It prints one line per thing that happens, and the first one is what the test reads:
 *
 *   id=0x200003                      the window id (before it is mapped)
 *   mapped 600x400 at 0,0            it asked for that
 *   configured 512x744               the window manager gave it another size or place
 *   the window manager asked me to close    WM_DELETE_WINDOW (the polite close of ICCCM)
 *   a signal, leaving                SIGTERM/SIGINT: what the test does at the end
 *   timeout                          --timeout ran out (exit status 3, the test failed to clean up)
 *
 * It answers WM_DELETE_WINDOW by leaving with status 0: that is how a close is told from a kill.
 * Built by the Makefile into build/nexwm-client (see `make check-nexwm`).
 */
#include <xcb/xcb.h>

#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static xcb_connection_t *conn;
static xcb_window_t win;
static xcb_atom_t a_delete_window, a_wm_protocols;
static volatile sig_atomic_t leaving;

static void on_signal(int sig)
{
    (void)sig;
    leaving = 1;
}

static void on_alarm(int sig)
{
    (void)sig;
    printf("timeout\n");
    fflush(stdout);
    _exit(3);
}

static xcb_atom_t atom(const char *name)
{
    xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(conn, xcb_intern_atom(conn, 0, (uint16_t)strlen(name), name), NULL);
    if (!r) {
        fprintf(stderr, "nexwm-client: cannot intern %s\n", name);
        exit(2);
    }
    xcb_atom_t a = r->atom;
    free(r);
    return a;
}

static void set_string(xcb_atom_t prop, xcb_atom_t type, const char *s, int bytes)
{
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, win, prop, type, 8, (uint32_t)(bytes >= 0 ? bytes : (int)strlen(s)),
                        s);
}

static void set_atoms(xcb_atom_t prop, xcb_atom_t type, const xcb_atom_t *list, int n)
{
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, win, prop, type, 32, (uint32_t)n, list);
}

static void set_cardinals(xcb_atom_t prop, const uint32_t *list, int n)
{
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, win, prop, XCB_ATOM_CARDINAL, 32, (uint32_t)n, list);
}

int main(int argc, char **argv)
{
    const char *title = "nexwm-client";
    const char *class_name = "nexwm-client";
    int w = 600, h = 400, dock = 0, timeout = 60;
    int x = 60, y = 60;              /* not the top left corner: "it moved" has to be visible */

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--title") && i + 1 < argc) { title = argv[++i]; continue; }
        if (!strcmp(a, "--class") && i + 1 < argc) { class_name = argv[++i]; continue; }
        if (!strcmp(a, "--dock") && i + 1 < argc) { dock = atoi(argv[++i]); continue; }
        if (!strcmp(a, "--timeout") && i + 1 < argc) { timeout = atoi(argv[++i]); continue; }
        if (!strcmp(a, "--pos") && i + 1 < argc) {
            if (sscanf(argv[++i], "%d,%d", &x, &y) != 2) {
                fprintf(stderr, "nexwm-client: --pos wants X,Y\n");
                return 2;
            }
            continue;
        }
        if (!strcmp(a, "--size") && i + 1 < argc) {
            if (sscanf(argv[++i], "%dx%d", &w, &h) != 2 || w < 1 || h < 1) {
                fprintf(stderr, "nexwm-client: --size wants WxH\n");
                return 2;
            }
            continue;
        }
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            printf("Usage: nexwm-client [--title T] [--class C] [--size WxH] [--pos X,Y] [--dock N] [--timeout N]\n");
            return 0;
        }
        fprintf(stderr, "nexwm-client: unknown option '%s'\n", a);
        return 2;
    }

    int scr = 0;
    conn = xcb_connect(NULL, &scr);
    if (xcb_connection_has_error(conn)) {
        fprintf(stderr, "nexwm-client: cannot open the display %s\n", getenv("DISPLAY") ? getenv("DISPLAY") : "(none)");
        return 2;
    }
    const xcb_setup_t *setup = xcb_get_setup(conn);
    xcb_screen_iterator_t it = xcb_setup_roots_iterator(setup);
    for (int i = 0; i < scr; i++) xcb_screen_next(&it);
    const xcb_screen_t *screen = it.data;

    if (dock) {                      /* the panel: the whole width, N pixels tall, at the top of the screen */
        w = screen->width_in_pixels;
        h = dock;
        x = 0; y = 0;
    }

    win = xcb_generate_id(conn);
    uint32_t values[2] = { screen->white_pixel, XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_EXPOSURE };
    xcb_create_window(conn, XCB_COPY_FROM_PARENT, win, screen->root, (int16_t)x, (int16_t)y, (uint16_t)w, (uint16_t)h, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
                      XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, values);

    /* the name (both kinds: a modern toolkit reads _NET_WM_NAME, an old one WM_NAME), the class, the pid */
    set_string(XCB_ATOM_WM_NAME, XCB_ATOM_STRING, title, -1);
    set_string(atom("_NET_WM_NAME"), atom("UTF8_STRING"), title, -1);
    char cls[256];                   /* WM_CLASS: instance and class, each NUL-terminated */
    snprintf(cls, sizeof cls, "%s", class_name);
    size_t n = strlen(cls) + 1;
    snprintf(cls + n, sizeof cls - n, "%s", class_name);
    set_string(XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, cls, (int)(n + strlen(cls + n) + 1));
    uint32_t pid = (uint32_t)getpid();
    set_cardinals(atom("_NET_WM_PID"), &pid, 1);

    /* WM_DELETE_WINDOW: without this the window manager is right to kill the window instead of asking it to close */
    a_wm_protocols = atom("WM_PROTOCOLS");
    a_delete_window = atom("WM_DELETE_WINDOW");
    set_atoms(a_wm_protocols, XCB_ATOM_ATOM, &a_delete_window, 1);

    if (dock) {
        xcb_atom_t type = atom("_NET_WM_WINDOW_TYPE_DOCK");
        set_atoms(atom("_NET_WM_WINDOW_TYPE"), XCB_ATOM_ATOM, &type, 1);
        /* _NET_WM_STRUT_PARTIAL: left, right, top, bottom, then the start and end of the edge each one covers */
        uint32_t strut[12] = { 0, 0, (uint32_t)dock, 0, 0, 0, 0, 0, 0, (uint32_t)w, 0, 0 };
        set_cardinals(atom("_NET_WM_STRUT_PARTIAL"), strut, 12);
        uint32_t strut4[4] = { 0, 0, (uint32_t)dock, 0 };
        set_cardinals(atom("_NET_WM_STRUT"), strut4, 4);
    }

    xcb_map_window(conn, win);
    xcb_flush(conn);
    printf("id=0x%x\n", (unsigned)win);
    printf("mapped %dx%d at %d,%d\n", w, h, x, y);
    fflush(stdout);

    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    if (timeout > 0) {
        signal(SIGALRM, on_alarm);
        alarm((unsigned)timeout);
    }

    int fd = xcb_get_file_descriptor(conn);
    int rc = 0;
    while (!leaving) {
        struct pollfd pfd = { fd, POLLIN, 0 };
        poll(&pfd, 1, 200);
        xcb_generic_event_t *ev;
        while ((ev = xcb_poll_for_event(conn))) {
            switch (ev->response_type & 0x7f) {
            case XCB_CONFIGURE_NOTIFY: {
                xcb_configure_notify_event_t *c = (xcb_configure_notify_event_t *)ev;
                printf("configured %dx%d\n", c->width, c->height);
                fflush(stdout);
                break;
            }
            case XCB_CLIENT_MESSAGE: {
                xcb_client_message_event_t *m = (xcb_client_message_event_t *)ev;
                if (m->type == a_wm_protocols && m->data.data32[0] == a_delete_window) {
                    printf("the window manager asked me to close\n");
                    fflush(stdout);
                    xcb_disconnect(conn);
                    return 0;                  /* a close is a polite exit, a kill is not: the test tells them apart */
                }
                break;
            }
            default:
                break;
            }
            free(ev);
        }
        if (xcb_connection_has_error(conn)) {
            printf("the display went away\n");
            rc = 4;
            break;
        }
    }

    if (leaving) printf("a signal, leaving\n");
    fflush(stdout);
    xcb_disconnect(conn);
    return rc;
}
