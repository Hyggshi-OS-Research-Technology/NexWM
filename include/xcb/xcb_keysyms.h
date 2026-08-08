/*
 * xcb_keysyms.h - XCB key symbols header fallback
 */

#ifndef XCB_KEYSYMS_H
#define XCB_KEYSYMS_H

#include <xcb/xcb.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct xcb_key_symbols_t xcb_key_symbols_t;

xcb_key_symbols_t *xcb_key_symbols_alloc(xcb_connection_t *c);
void xcb_key_symbols_free(xcb_key_symbols_t *syms);
xcb_keysym_t xcb_key_symbols_get_keysym(xcb_key_symbols_t *syms, xcb_keycode_t keycode, int col);
xcb_keycode_t *xcb_key_symbols_get_keycode(xcb_key_symbols_t *syms, xcb_keysym_t keysym);

#ifdef __cplusplus
}
#endif

#endif
