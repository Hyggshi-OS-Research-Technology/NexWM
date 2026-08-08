/*
 * xcb_ewmh.h - XCB EWMH header fallback
 */

#ifndef XCB_EWMH_H
#define XCB_EWMH_H

#include <xcb/xcb.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    xcb_connection_t *connection;
    int screen;
} xcb_ewmh_connection_t;

xcb_intern_atom_cookie_t *xcb_ewmh_init_atoms(xcb_connection_t *c, xcb_ewmh_connection_t *ewmh);
uint8_t xcb_ewmh_init_atoms_replies(xcb_ewmh_connection_t *ewmh, xcb_intern_atom_cookie_t *cookies, xcb_generic_error_t **e);
void xcb_ewmh_connection_wipe(xcb_ewmh_connection_t *ewmh);

#ifdef __cplusplus
}
#endif

#endif
