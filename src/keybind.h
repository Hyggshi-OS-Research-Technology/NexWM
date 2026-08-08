/*
 * keybind.h - Keyboard shortcut management for NexWM
 */

#ifndef NEXWM_KEYBIND_H
#define NEXWM_KEYBIND_H

#include <xcb/xcb.h>
#include <xcb/xcb_keysyms.h>

typedef struct {
    xcb_key_symbols_t *symbols;
} nex_keybind_ctx_t;

extern nex_keybind_ctx_t g_keybind_ctx;

int nex_keybind_init(xcb_connection_t *conn);
void nex_keybind_grab(xcb_connection_t *conn, xcb_window_t root);
void nex_keybind_handle(xcb_key_press_event_t *ev);
void nex_keybind_cleanup(void);

#endif
