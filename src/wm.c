#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

/*
 * wm.c - Window Manager core implementation for NexWM
 */

#include "wm.h"
#include "events.h"
#include "client.h"
#include "workspace.h"
#include "monitor.h"
#include "keybind.h"
#include "ewmh.h"
#include "rules.h"
#include "ipc.h"
#include "log.h"
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <string.h>
#include <errno.h>
#include <poll.h>
#include <X11/cursorfont.h>

xcb_connection_t *g_conn = NULL;
xcb_screen_t *g_screen = NULL;
xcb_window_t g_root = XCB_NONE;
nex_atoms_t g_atoms;
int g_running = 1;

static void sigchld_handler(int sig)
{
    (void)sig;
    while (waitpid(-1, NULL, WNOHANG) > 0);
}

void nex_wm_scan_existing_windows(void)
{
    xcb_query_tree_cookie_t tree_cookie = xcb_query_tree(g_conn, g_root);
    xcb_query_tree_reply_t *tree = xcb_query_tree_reply(g_conn, tree_cookie, NULL);
    if (!tree) {
        NEX_WARN("Failed to query window tree");
        return;
    }

    int len = xcb_query_tree_children_length(tree);
    xcb_window_t *children = xcb_query_tree_children(tree);
    NEX_INFO("Scanning %d existing windows", len);

    for (int i = 0; i < len; i++) {
        xcb_window_t w = children[i];
        if (w == g_root) continue;

        xcb_get_window_attributes_cookie_t attr_cookie = xcb_get_window_attributes(g_conn, w);
        xcb_get_window_attributes_reply_t *attr = xcb_get_window_attributes_reply(g_conn, attr_cookie, NULL);
        if (!attr) continue;

        if (attr->map_state == XCB_MAP_STATE_VIEWABLE && !attr->override_redirect) {
            nex_client_t *c = nex_client_create(w);
            if (c) {
                nex_rules_apply(c);
                if (c->workspace < 0 || c->workspace >= g_config.workspace_count) {
                    c->workspace = g_current_workspace;
                }
                nex_ewmh_set_wm_desktop(w, c->workspace);
                xcb_map_window(g_conn, w);
                if (c->workspace == g_current_workspace) nex_client_focus(c);
            }
        }
        free(attr);
    }

    free(tree);
    nex_ewmh_set_client_list();
    xcb_flush(g_conn);
}

int nex_wm_init(void)
{
    NEX_INFO("NexWM initializing...");

    struct sigaction sa;
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_NOCLDSTOP | SA_RESTART;
    sigaction(SIGCHLD, &sa, NULL);

    int screen_num;
    g_conn = xcb_connect(NULL, &screen_num);
    if (xcb_connection_has_error(g_conn)) {
        NEX_FATAL("Failed to connect to X server");
        return -1;
    }
    NEX_INFO("X11 connection established");

    const xcb_setup_t *setup = xcb_get_setup(g_conn);
    xcb_screen_iterator_t iter = xcb_setup_roots_iterator(setup);
    for (int i = 0; i < screen_num; i++) xcb_screen_next(&iter);
    g_screen = iter.data;
    g_root = g_screen->root;
    NEX_INFO("Root window acquired: 0x%x", g_root);

    uint32_t mask = XCB_CW_EVENT_MASK;
    uint32_t values[] = {
        XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT |
        XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY |
        XCB_EVENT_MASK_STRUCTURE_NOTIFY |
        XCB_EVENT_MASK_PROPERTY_CHANGE |
        XCB_EVENT_MASK_ENTER_WINDOW |
        XCB_EVENT_MASK_LEAVE_WINDOW
    };

    xcb_void_cookie_t cookie = xcb_change_window_attributes_checked(g_conn, g_root, mask, values);
    xcb_generic_error_t *err = xcb_request_check(g_conn, cookie);
    if (err) {
        if (err->error_code == XCB_ACCESS) {
            NEX_FATAL("Another window manager is already running");
        } else {
            NEX_FATAL("XCB error while selecting input: %d", err->error_code);
        }
        free(err);
        return -1;
    }
    NEX_INFO("Substructure redirect acquired");

    if (nex_atoms_init(g_conn, &g_atoms) < 0) {
        NEX_FATAL("Failed to initialize atoms");
        return -1;
    }
    if (nex_ewmh_init(g_conn, g_screen) < 0) {
        NEX_FATAL("Failed to initialize EWMH");
        return -1;
    }
    if (nex_keybind_init(g_conn) < 0) {
        NEX_FATAL("Failed to initialize keybinds");
        return -1;
    }
    nex_keybind_grab(g_conn, g_root);
    nex_monitor_init(g_conn, g_screen);
    nex_workspace_init(g_config.workspace_count);
    nex_ipc_init();

    xcb_font_t font = xcb_generate_id(g_conn);
    xcb_open_font(g_conn, font, strlen("cursor"), "cursor");
    xcb_cursor_t cursor = xcb_generate_id(g_conn);
    xcb_create_glyph_cursor(g_conn, cursor, font, font,
                            XC_left_ptr, XC_left_ptr + 1,
                            0, 0, 0, 0xffff, 0xffff, 0xffff);
    xcb_change_window_attributes(g_conn, g_root, XCB_CW_CURSOR, &cursor);
    xcb_close_font(g_conn, font);
    xcb_free_cursor(g_conn, cursor);

    xcb_grab_button(g_conn, 0, g_root,
                    XCB_EVENT_MASK_BUTTON_PRESS,
                    XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
                    XCB_NONE, XCB_NONE,
                    XCB_BUTTON_INDEX_ANY, XCB_MOD_MASK_ANY);

    xcb_flush(g_conn);
    nex_wm_scan_existing_windows();
    NEX_INFO("NexWM ready");
    return 0;
}

void nex_wm_run(void)
{
    NEX_INFO("Entering main event loop");

    int xcb_fd = xcb_get_file_descriptor(g_conn);
    int ipc_fd = nex_ipc_get_fd();

    struct pollfd fds[2];
    fds[0].fd = xcb_fd;
    fds[0].events = POLLIN;
    fds[1].fd = ipc_fd;
    fds[1].events = POLLIN;

    while (g_running) {
        int nfds = (ipc_fd >= 0) ? 2 : 1;
        int ready = poll(fds, (nfds_t)nfds, -1);
        if (ready < 0) {
            if (errno == EINTR) continue;
            NEX_ERROR("poll() error: %s", strerror(errno));
            break;
        }

        /* Handle IPC commands */
        if (ipc_fd >= 0 && (fds[1].revents & POLLIN)) {
            nex_ipc_handle();
        }

        /* Handle X11 events */
        if (fds[0].revents & POLLIN) {
            xcb_generic_event_t *ev;
            while ((ev = xcb_poll_for_event(g_conn)) != NULL) {
                nex_events_handle(ev);
                free(ev);
            }
            xcb_flush(g_conn);
        }

        if (xcb_connection_has_error(g_conn)) {
            NEX_ERROR("X11 connection error, exiting");
            break;
        }
    }

    NEX_INFO("Exiting main event loop");
}


void nex_wm_cleanup(void)
{
    NEX_INFO("NexWM shutting down...");

    while (g_clients) {
        nex_client_t *c = g_clients;
        uint32_t values[] = { 0 };
        xcb_change_window_attributes(g_conn, c->window, XCB_CW_BORDER_PIXEL, values);
        xcb_unmap_window(g_conn, c->window);
        nex_client_destroy(c);
    }

    nex_keybind_cleanup();
    nex_ewmh_cleanup();
    nex_ipc_cleanup();

    if (g_conn) {
        xcb_disconnect(g_conn);
        g_conn = NULL;
    }
    NEX_INFO("NexWM shutdown complete");
}
