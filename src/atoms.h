/*
 * atoms.h - X11 atom management for NexWM
 */

#ifndef NEXWM_ATOMS_H
#define NEXWM_ATOMS_H

#include <xcb/xcb.h>

typedef struct {
    xcb_atom_t net_supported;
    xcb_atom_t net_client_list;
    xcb_atom_t net_active_window;
    xcb_atom_t net_current_desktop;
    xcb_atom_t net_number_of_desktops;
    xcb_atom_t net_desktop_names;
    xcb_atom_t net_wm_desktop;
    xcb_atom_t net_wm_state;
    xcb_atom_t net_wm_state_fullscreen;
    xcb_atom_t net_wm_state_maximized_vert;
    xcb_atom_t net_wm_state_maximized_horz;
    xcb_atom_t net_wm_window_type;
    xcb_atom_t net_wm_name;
    xcb_atom_t net_workarea;
    xcb_atom_t net_wm_pid;
    xcb_atom_t wm_protocols;
    xcb_atom_t wm_delete_window;
    xcb_atom_t wm_state;
    xcb_atom_t wm_take_focus;
    xcb_atom_t wm_normal_hints;
} nex_atoms_t;

int nex_atoms_init(xcb_connection_t *conn, nex_atoms_t *atoms);

#endif
