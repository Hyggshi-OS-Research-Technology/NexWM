/*
 * atoms.c - X11 atom initialization for NexWM
 */

#include "atoms.h"
#include "log.h"
#include <string.h>
#include <stdlib.h>

static xcb_atom_t intern_atom(xcb_connection_t *conn, const char *name)
{
    xcb_intern_atom_cookie_t cookie = xcb_intern_atom(conn, 0, strlen(name), name);
    xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(conn, cookie, NULL);
    xcb_atom_t atom = XCB_ATOM_NONE;
    if (reply) {
        atom = reply->atom;
        free(reply);
    }
    return atom;
}

int nex_atoms_init(xcb_connection_t *conn, nex_atoms_t *atoms)
{
    if (!conn || !atoms) {
        NEX_ERROR("Invalid connection or atoms pointer");
        return -1;
    }

    memset(atoms, 0, sizeof(nex_atoms_t));

    atoms->net_supported             = intern_atom(conn, "_NET_SUPPORTED");
    atoms->net_client_list           = intern_atom(conn, "_NET_CLIENT_LIST");
    atoms->net_active_window         = intern_atom(conn, "_NET_ACTIVE_WINDOW");
    atoms->net_current_desktop       = intern_atom(conn, "_NET_CURRENT_DESKTOP");
    atoms->net_number_of_desktops    = intern_atom(conn, "_NET_NUMBER_OF_DESKTOPS");
    atoms->net_desktop_names         = intern_atom(conn, "_NET_DESKTOP_NAMES");
    atoms->net_wm_desktop            = intern_atom(conn, "_NET_WM_DESKTOP");
    atoms->net_wm_state              = intern_atom(conn, "_NET_WM_STATE");
    atoms->net_wm_state_fullscreen   = intern_atom(conn, "_NET_WM_STATE_FULLSCREEN");
    atoms->net_wm_state_maximized_vert = intern_atom(conn, "_NET_WM_STATE_MAXIMIZED_VERT");
    atoms->net_wm_state_maximized_horz = intern_atom(conn, "_NET_WM_STATE_MAXIMIZED_HORZ");
    atoms->net_wm_window_type        = intern_atom(conn, "_NET_WM_WINDOW_TYPE");
    atoms->net_wm_name               = intern_atom(conn, "_NET_WM_NAME");
    atoms->net_workarea              = intern_atom(conn, "_NET_WORKAREA");
    atoms->net_wm_pid                = intern_atom(conn, "_NET_WM_PID");
    atoms->wm_protocols              = intern_atom(conn, "WM_PROTOCOLS");
    atoms->wm_delete_window          = intern_atom(conn, "WM_DELETE_WINDOW");
    atoms->wm_state                  = intern_atom(conn, "WM_STATE");
    atoms->wm_take_focus             = intern_atom(conn, "WM_TAKE_FOCUS");
    atoms->wm_normal_hints           = intern_atom(conn, "WM_NORMAL_HINTS");

    NEX_INFO("Atoms initialized successfully");
    return 0;
}
