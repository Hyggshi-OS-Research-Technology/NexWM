/*
 * wm.h - Window Manager core for NexWM
 */

#ifndef NEXWM_WM_H
#define NEXWM_WM_H

#include <xcb/xcb.h>
#include <xcb/xcb_keysyms.h>
#include "atoms.h"
#include "config.h"

extern xcb_connection_t *g_conn;
extern xcb_screen_t *g_screen;
extern xcb_window_t g_root;
extern nex_atoms_t g_atoms;
extern int g_running;

int nex_wm_init(void);
void nex_wm_run(void);
void nex_wm_cleanup(void);
void nex_wm_scan_existing_windows(void);

#endif
