#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

/*
 * nex_icon.c - PNG icon loader / blitter for NexPanel
 *
 * Decodes PNG files via libpng into an BGRA pixel buffer and composites
 * them onto XCB drawables using xcb_put_image (ZPixmap, depth 32 → 24 bpp
 * with alpha pre-multiplied against the caller-supplied background colour).
 *
 * No Cairo, no rsvg needed — just libpng + libxcb.
 */

#include "nex_icon.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <png.h>
#include <xcb/xcb.h>

/* ── Icon theme search ───────────────────────────────────────────────────── */

#define MAX_SEARCH_PATHS 16
#define MAX_PATH_LEN     512

static const char *ICON_THEME = "Yaru";

static const char *ICON_CATS[] = {
    "apps", "actions", "categories", "devices", "status", "places", "mimetypes",
    NULL
};

/*
 * Try to locate <name>.png at <size>x<size> in the given icon theme.
 * Writes the found path into `out` (at least MAX_PATH_LEN bytes).
 * Returns 1 on success, 0 on failure.
 */
static int find_icon_path(const char *name, int size, char *out)
{
    char size_str[16];
    snprintf(size_str, sizeof(size_str), "%dx%d", size, size);

    /* Primary theme */
    for (int c = 0; ICON_CATS[c]; c++) {
        snprintf(out, MAX_PATH_LEN,
                 "/usr/share/icons/%s/%s/%s/%s.png",
                 ICON_THEME, size_str, ICON_CATS[c], name);
        FILE *f = fopen(out, "rb");
        if (f) { fclose(f); return 1; }
    }

    /* Fallback: hicolor */
    for (int c = 0; ICON_CATS[c]; c++) {
        snprintf(out, MAX_PATH_LEN,
                 "/usr/share/icons/hicolor/%s/%s/%s.png",
                 size_str, ICON_CATS[c], name);
        FILE *f = fopen(out, "rb");
        if (f) { fclose(f); return 1; }
    }

    /* pixmaps */
    snprintf(out, MAX_PATH_LEN, "/usr/share/pixmaps/%s.png", name);
    FILE *f = fopen(out, "rb");
    if (f) { fclose(f); return 1; }

    return 0;
}

/* ── libpng decode ───────────────────────────────────────────────────────── */

/*
 * Load a PNG file and return an RGBA pixel buffer.
 * Output pixels are 32-bit RGBA (R at byte 0, A at byte 3), row-major.
 * Caller must free(*pixels_out).
 */
static int load_png_rgba(const char *path,
                         uint8_t **pixels_out,
                         int *width_out,
                         int *height_out)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;

    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING,
                                             NULL, NULL, NULL);
    if (!png) { fclose(fp); return 0; }

    png_infop info = png_create_info_struct(png);
    if (!info) { png_destroy_read_struct(&png, NULL, NULL); fclose(fp); return 0; }

    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, NULL);
        fclose(fp);
        return 0;
    }

    png_init_io(png, fp);
    png_read_info(png, info);

    int width  = (int)png_get_image_width(png, info);
    int height = (int)png_get_image_height(png, info);

    /* Normalise to 8-bit RGBA */
    png_byte color_type = png_get_color_type(png, info);
    png_byte bit_depth  = png_get_bit_depth(png, info);

    if (bit_depth == 16)
        png_set_strip_16(png);
    if (color_type == PNG_COLOR_TYPE_PALETTE)
        png_set_palette_to_rgb(png);
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
        png_set_expand_gray_1_2_4_to_8(png);
    if (png_get_valid(png, info, PNG_INFO_tRNS))
        png_set_tRNS_to_alpha(png);
    if (color_type == PNG_COLOR_TYPE_RGB  ||
        color_type == PNG_COLOR_TYPE_GRAY ||
        color_type == PNG_COLOR_TYPE_PALETTE)
        png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
    if (color_type == PNG_COLOR_TYPE_GRAY ||
        color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(png);

    png_read_update_info(png, info);

    /* Allocate and decode */
    size_t stride = (size_t)width * 4;
    uint8_t *pixels = malloc(stride * (size_t)height);
    if (!pixels) {
        png_destroy_read_struct(&png, &info, NULL);
        fclose(fp);
        return 0;
    }

    png_bytep *rows = malloc(sizeof(png_bytep) * (size_t)height);
    if (!rows) {
        free(pixels);
        png_destroy_read_struct(&png, &info, NULL);
        fclose(fp);
        return 0;
    }
    for (int y = 0; y < height; y++)
        rows[y] = pixels + (size_t)y * stride;

    png_read_image(png, rows);
    free(rows);
    png_destroy_read_struct(&png, &info, NULL);
    fclose(fp);

    *pixels_out = pixels;
    *width_out  = width;
    *height_out = height;
    return 1;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

nex_icon_t *nex_icon_load(const char *name, int size)
{
    if (!name) return NULL;

    char path[MAX_PATH_LEN];
    if (!find_icon_path(name, size, path)) {
        /* try nearest larger size */
        static const int fallback_sizes[] = { 24, 22, 32, 16, 48, 0 };
        int found = 0;
        for (int i = 0; fallback_sizes[i]; i++) {
            if (find_icon_path(name, fallback_sizes[i], path)) {
                found = 1;
                break;
            }
        }
        if (!found) return NULL;
    }

    uint8_t *rgba = NULL;
    int w = 0, h = 0;
    if (!load_png_rgba(path, &rgba, &w, &h)) return NULL;

    /* Convert RGBA → BGRA (XCB ZPixmap byte order on little-endian x86) */
    for (int i = 0; i < w * h; i++) {
        uint8_t r = rgba[i*4+0];
        uint8_t g = rgba[i*4+1];
        uint8_t b = rgba[i*4+2];
        /* alpha stays in byte 3 */
        rgba[i*4+0] = b;
        rgba[i*4+1] = g;
        rgba[i*4+2] = r;
    }

    nex_icon_t *icon = malloc(sizeof(nex_icon_t));
    if (!icon) { free(rgba); return NULL; }
    icon->pixels = rgba;
    icon->width  = w;
    icon->height = h;
    return icon;
}

void nex_icon_free(nex_icon_t *icon)
{
    if (!icon) return;
    free(icon->pixels);
    free(icon);
}

/*
 * Alpha-composite a single BGRA pixel over a solid background colour.
 * bg_rgb is 0x00RRGGBB.
 * Returns the blended 0x00RRGGBB colour.
 */
static inline uint32_t blend(uint8_t br, uint8_t bg_c, uint8_t bb, uint8_t a,
                              uint8_t bg_r, uint8_t bg_g, uint8_t bg_b)
{
    uint32_t alpha = a;
    uint32_t ialpha = 255 - alpha;
    uint8_t or = (uint8_t)((br * alpha + bg_r * ialpha) / 255);
    uint8_t og = (uint8_t)((bg_c * alpha + bg_g * ialpha) / 255);
    uint8_t ob = (uint8_t)((bb * alpha + bg_b * ialpha) / 255);
    return ((uint32_t)or << 16) | ((uint32_t)og << 8) | ob;
}

void nex_icon_draw(xcb_connection_t *conn,
                   xcb_drawable_t    drawable,
                   xcb_gcontext_t    gc,
                   const nex_icon_t *icon,
                   int dx, int dy,
                   uint32_t bg_rgb)
{
    if (!conn || !icon || !icon->pixels) return;

    int w = icon->width;
    int h = icon->height;

    uint8_t bg_r = (bg_rgb >> 16) & 0xff;
    uint8_t bg_g = (bg_rgb >>  8) & 0xff;
    uint8_t bg_b =  bg_rgb        & 0xff;

    /* Build a 24-bit BGR ZPixmap buffer (XCB depth-24 requires 32-bit words,
     * byte order BGRA with the high byte ignored). */
    size_t stride32 = (size_t)w * 4;
    uint8_t *buf = malloc(stride32 * (size_t)h);
    if (!buf) return;

    const uint8_t *src = icon->pixels;
    uint8_t *dst = buf;

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint8_t b = src[0], g = src[1], r = src[2], a = src[3];
            src += 4;
            uint32_t c = blend(r, g, b, a, bg_r, bg_g, bg_b);
            dst[0] = (uint8_t)(c & 0xff);          /* B */
            dst[1] = (uint8_t)((c >> 8) & 0xff);   /* G */
            dst[2] = (uint8_t)((c >> 16) & 0xff);  /* R */
            dst[3] = 0xff;
            dst += 4;
        }
    }

    xcb_put_image(conn, XCB_IMAGE_FORMAT_Z_PIXMAP, drawable, gc,
                  (uint16_t)w, (uint16_t)h,
                  (int16_t)dx, (int16_t)dy,
                  0,         /* left pad */
                  24,        /* depth */
                  (uint32_t)(stride32 * (size_t)h),
                  buf);

    free(buf);
}
