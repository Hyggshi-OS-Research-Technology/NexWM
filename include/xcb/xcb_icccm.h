/*
 * xcb_icccm.h - XCB ICCCM header fallback
 */

#ifndef XCB_ICCCM_H
#define XCB_ICCCM_H

#include <xcb/xcb.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char *name;
    uint32_t name_len;
    xcb_atom_t encoding;
    uint8_t format;
} xcb_icccm_get_text_property_reply_t;

typedef struct {
    char *instance_name;
    char *class_name;
} xcb_icccm_get_wm_class_reply_t;

typedef struct {
    uint32_t atoms_len;
    xcb_atom_t *atoms;
} xcb_icccm_get_wm_protocols_reply_t;

xcb_get_property_cookie_t xcb_icccm_get_wm_name(xcb_connection_t *c, xcb_window_t window);
uint8_t xcb_icccm_get_wm_name_reply(xcb_connection_t *c, xcb_get_property_cookie_t cookie, xcb_icccm_get_text_property_reply_t *prop, xcb_generic_error_t **e);
void xcb_icccm_get_text_property_reply_wipe(xcb_icccm_get_text_property_reply_t *prop);

xcb_get_property_cookie_t xcb_icccm_get_wm_class(xcb_connection_t *c, xcb_window_t window);
uint8_t xcb_icccm_get_wm_class_reply(xcb_connection_t *c, xcb_get_property_cookie_t cookie, xcb_icccm_get_wm_class_reply_t *prop, xcb_generic_error_t **e);
void xcb_icccm_get_wm_class_reply_wipe(xcb_icccm_get_wm_class_reply_t *prop);

xcb_get_property_cookie_t xcb_icccm_get_wm_protocols(xcb_connection_t *c, xcb_window_t window, xcb_atom_t property);
uint8_t xcb_icccm_get_wm_protocols_reply(xcb_connection_t *c, xcb_get_property_cookie_t cookie, xcb_icccm_get_wm_protocols_reply_t *prop, xcb_generic_error_t **e);
void xcb_icccm_get_wm_protocols_reply_wipe(xcb_icccm_get_wm_protocols_reply_t *prop);

#ifdef __cplusplus
}
#endif

#endif
