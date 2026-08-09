/*
 * nex_icon.h - PNG icon loader for NexPanel
 *
 * Loads PNG icons from the filesystem and composites them onto an XCB window
 * using software rendering via libpng + XCB shared memory (XCB SHM or fallback
 * to xcb_put_image).  No Cairo dependency — only libpng + libxcb.
 *
 * Icon lookup order (XDG icon spec, simplified):
 *   1. /usr/share/icons/<ICON_THEME>/<SIZE>x<SIZE>/apps/<name>.png
 *   2. /usr/share/icons/<ICON_THEME>/<SIZE>x<SIZE>/categories/<name>.png
 *   3. /usr/share/icons/<ICON_THEME>/<SIZE>x<SIZE>/actions/<name>.png
 *   4. /usr/share/icons/<ICON_THEME>/<SIZE>x<SIZE>/devices/<name>.png
 *   5. /usr/share/icons/<ICON_THEME>/<SIZE>x<SIZE>/status/<name>.png
 *   6. /usr/share/pixmaps/<name>.png
 *   7. NULL  (caller should fall back to drawn placeholder)
 */

#ifndef NEX_ICON_H
#define NEX_ICON_H

#include <stdint.h>

/*
 * nex_icon_t — a decoded RGBA icon image
 *
 * pixels: raw BGRA (host-endian) pixels ready for XCB xcb_put_image.
 *         Stride = width * 4 bytes. Caller must free(pixels).
 * width/height: dimensions in pixels
 */
typedef struct {
    uint8_t *pixels;   /* BGRA, row-major, stride = width*4 */
    int      width;
    int      height;
} nex_icon_t;

/*
 * Load a named icon at the requested size.
 * name: XDG icon name (e.g. "utilities-terminal", "system-file-manager")
 * size: desired pixel size (16, 22, 24, 32, 48 …)
 * Returns a heap-allocated nex_icon_t (caller frees icon->pixels then the struct),
 * or NULL on failure.
 */
nex_icon_t *nex_icon_load(const char *name, int size);

/*
 * Free an icon returned by nex_icon_load.
 */
void nex_icon_free(nex_icon_t *icon);

/*
 * Blit icon onto an XCB drawable at (dx, dy).
 * The icon is alpha-composited over the background colour `bg` (0xRRGGBB).
 * Uses xcb_put_image (ZPixmap, 32bpp).
 */
#include <xcb/xcb.h>
void nex_icon_draw(xcb_connection_t *conn,
                   xcb_drawable_t    drawable,
                   xcb_gcontext_t    gc,
                   const nex_icon_t *icon,
                   int dx, int dy,
                   uint32_t bg_rgb);

#endif /* NEX_ICON_H */
