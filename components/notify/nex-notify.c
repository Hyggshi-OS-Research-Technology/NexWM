#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

/*
 * nex-notify.c - NexNotify
 * Desktop Notification Daemon for NexWM / Hyggshi OS
 *
 * Renders sleek toast notification popups in top-right corner of screen.
 * Supports auto-expire timers and click-to-dismiss.
 */

#include "nex-notify.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <poll.h>

#include <xcb/xcb.h>

#define CLR_NOTIFY_BG    0x1a1a2e
#define CLR_NOTIFY_FG    0xffffff
#define CLR_NOTIFY_BORDER 0x5b8dd9
#define CLR_NOTIFY_SUB   0xa0a0c0

static xcb_atom_t intern_atom(xcb_connection_t *conn, const char *name)
{
    xcb_intern_atom_cookie_t c = xcb_intern_atom(conn, 0, (uint16_t)strlen(name), name);
    xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(conn, c, NULL);
    xcb_atom_t a = r ? r->atom : XCB_ATOM_NONE;
    free(r);
    return a;
}

static void show_toast(const char *summary, const char *body, int timeout_ms)
{
    xcb_connection_t *conn = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(conn)) return;

    const xcb_setup_t *setup = xcb_get_setup(conn);
    xcb_screen_iterator_t iter = xcb_setup_roots_iterator(setup);
    xcb_screen_t *screen = iter.data;

    int w = NEX_NOTIFY_WIDTH;
    int h = NEX_NOTIFY_HEIGHT;
    int x = (int)screen->width_in_pixels - w - 16;
    int y = 40; /* below top panel */

    uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_BORDER_PIXEL |
                    XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK;
    uint32_t vals[4] = {
        CLR_NOTIFY_BG,
        CLR_NOTIFY_BORDER,
        1, /* override_redirect */
        XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_BUTTON_PRESS
    };

    xcb_window_t win = xcb_generate_id(conn);
    xcb_create_window(conn, XCB_COPY_FROM_PARENT, win, screen->root,
                      (int16_t)x, (int16_t)y, (uint16_t)w, (uint16_t)h, 2,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, mask, vals);

    xcb_atom_t type_atom = intern_atom(conn, "_NET_WM_WINDOW_TYPE");
    xcb_atom_t notification_atom = intern_atom(conn, "_NET_WM_WINDOW_TYPE_NOTIFICATION");
    if (type_atom != XCB_ATOM_NONE && notification_atom != XCB_ATOM_NONE) {
        xcb_change_property(conn, XCB_PROP_MODE_REPLACE, win,
                            type_atom, XCB_ATOM_ATOM, 32, 1, &notification_atom);
    }

    xcb_map_window(conn, win);
    xcb_flush(conn);

    xcb_gc_t gc_fg = xcb_generate_id(conn);
    uint32_t gc_vals[2] = { CLR_NOTIFY_FG, CLR_NOTIFY_BG };
    xcb_create_gc(conn, gc_fg, win, XCB_GC_FOREGROUND | XCB_GC_BACKGROUND, gc_vals);

    xcb_gc_t gc_sub = xcb_generate_id(conn);
    uint32_t gc_sub_vals[2] = { CLR_NOTIFY_SUB, CLR_NOTIFY_BG };
    xcb_create_gc(conn, gc_sub, win, XCB_GC_FOREGROUND | XCB_GC_BACKGROUND, gc_sub_vals);

    int xcb_fd = xcb_get_file_descriptor(conn);
    struct pollfd fds[1] = {{ xcb_fd, POLLIN, 0 }};

    uint64_t start_time = (uint64_t)time(NULL);
    int duration_sec = timeout_ms / 1000;
    if (duration_sec <= 0) duration_sec = 4;

    int running = 1;
    while (running) {
        int ready = poll(fds, 1, 100);
        if (ready < 0 && errno != EINTR) break;

        uint64_t elapsed = (uint64_t)time(NULL) - start_time;
        if (elapsed >= (uint64_t)duration_sec) break;

        xcb_generic_event_t *ev;
        while ((ev = xcb_poll_for_event(conn)) != NULL) {
            uint8_t type = ev->response_type & ~0x80;
            if (type == XCB_EXPOSE) {
                /* Draw Toast Summary & Body */
                xcb_image_text_8(conn, (uint8_t)strlen(summary), win, gc_fg, 16, 28, summary);
                if (body && body[0] != '\0') {
                    char truncated[40];
                    strncpy(truncated, body, sizeof(truncated) - 1);
                    truncated[sizeof(truncated) - 1] = '\0';
                    xcb_image_text_8(conn, (uint8_t)strlen(truncated), win, gc_sub, 16, 52, truncated);
                }
                xcb_flush(conn);
            } else if (type == XCB_BUTTON_PRESS) {
                running = 0; /* dismiss on click */
            }
            free(ev);
        }
    }

    xcb_free_gc(conn, gc_fg);
    xcb_free_gc(conn, gc_sub);
    xcb_destroy_window(conn, win);
    xcb_disconnect(conn);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: nex-notify <summary> [body] [timeout_ms]\n");
        return 1;
    }

    const char *summary = argv[1];
    const char *body = (argc > 2) ? argv[2] : "";
    int timeout_ms = (argc > 3) ? atoi(argv[3]) : 4000;

    show_toast(summary, body, timeout_ms);
    return 0;
}
