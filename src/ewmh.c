/*
 * ewmh.c - Extended Window Manager Hints implementation for NexWM
 */

#include "ewmh.h"
#include "atoms.h"
#include "client.h"
#include "workspace.h"
#include "log.h"
#include <string.h>
#include <stdlib.h>

nex_ewmh_ctx_t g_ewmh;

extern xcb_connection_t *g_conn;
extern xcb_screen_t *g_screen;
extern nex_atoms_t g_atoms;

int nex_ewmh_init(xcb_connection_t *conn, xcb_screen_t *screen)
{
    (void)conn;
    nex_ewmh_set_supported(screen);
    nex_ewmh_set_number_of_desktops(g_config.workspace_count);
    nex_ewmh_set_current_desktop(0);

    NEX_INFO("EWMH initialized");
    return 0;
}

void nex_ewmh_set_supported(xcb_screen_t *screen)
{
    xcb_atom_t supported[] = {
        g_atoms.net_supported,
        g_atoms.net_client_list,
        g_atoms.net_active_window,
        g_atoms.net_current_desktop,
        g_atoms.net_number_of_desktops,
        g_atoms.net_desktop_names,
        g_atoms.net_wm_desktop,
        g_atoms.net_wm_state,
        g_atoms.net_wm_state_fullscreen,
        g_atoms.net_wm_state_maximized_vert,
        g_atoms.net_wm_state_maximized_horz,
        g_atoms.net_wm_window_type,
        g_atoms.net_wm_name,
        g_atoms.net_workarea,
        g_atoms.net_wm_pid,
        g_atoms.wm_protocols,
        g_atoms.wm_delete_window,
    };

    xcb_change_property(g_conn, XCB_PROP_MODE_REPLACE, screen->root,
                        g_atoms.net_supported, XCB_ATOM_ATOM, 32,
                        sizeof(supported) / sizeof(xcb_atom_t), supported);
}

void nex_ewmh_set_client_list(void)
{
    int count = 0;
    nex_client_t *c;
    for (c = g_clients; c; c = c->next) count++;

    if (count == 0) {
        xcb_delete_property(g_conn, g_screen->root, g_atoms.net_client_list);
        return;
    }

    xcb_window_t *windows = malloc(count * sizeof(xcb_window_t));
    if (!windows) return;

    int i = 0;
    for (c = g_clients; c; c = c->next) windows[i++] = c->window;

    xcb_change_property(g_conn, XCB_PROP_MODE_REPLACE, g_screen->root,
                        g_atoms.net_client_list, XCB_ATOM_WINDOW, 32, count, windows);
    free(windows);
}

void nex_ewmh_set_active_window(xcb_window_t window)
{
    xcb_change_property(g_conn, XCB_PROP_MODE_REPLACE, g_screen->root,
                        g_atoms.net_active_window, XCB_ATOM_WINDOW, 32, 1, &window);
}

void nex_ewmh_set_current_desktop(int desktop)
{
    uint32_t data = desktop;
    xcb_change_property(g_conn, XCB_PROP_MODE_REPLACE, g_screen->root,
                        g_atoms.net_current_desktop, XCB_ATOM_CARDINAL, 32, 1, &data);
}

void nex_ewmh_set_number_of_desktops(int count)
{
    uint32_t data = count;
    xcb_change_property(g_conn, XCB_PROP_MODE_REPLACE, g_screen->root,
                        g_atoms.net_number_of_desktops, XCB_ATOM_CARDINAL, 32, 1, &data);
}

void nex_ewmh_set_wm_desktop(xcb_window_t window, int desktop)
{
    uint32_t data = desktop;
    xcb_change_property(g_conn, XCB_PROP_MODE_REPLACE, window,
                        g_atoms.net_wm_desktop, XCB_ATOM_CARDINAL, 32, 1, &data);
}

void nex_ewmh_set_wm_state_hidden(xcb_window_t window, int hidden)
{
    if (hidden) {
        xcb_change_property(g_conn, XCB_PROP_MODE_REPLACE, window,
                            g_atoms.net_wm_state, XCB_ATOM_ATOM, 32, 1, &g_atoms.net_wm_state_hidden);
    } else {
        xcb_delete_property(g_conn, window, g_atoms.net_wm_state);
    }
}

void nex_ewmh_cleanup(void)
{
}
