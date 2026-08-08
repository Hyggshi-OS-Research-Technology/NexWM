#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

/*
 * nex-wallpaper.c - NexWallpaper
 * Standalone wallpaper daemon and setter for NexWM
 *
 * Usage:
 *   nex-wallpaper --set <image> [--mode scale|stretch|center|tile]
 *   nex-wallpaper --color <0xRRGGBB>
 *   nex-wallpaper --restore
 *
 * Renders image onto the X11 root window using XLib + Imlib2.
 * Stores the last wallpaper path in ~/.cache/nexwm/wallpaper so it
 * can be restored on session start.
 */

#include "nex-wallpaper.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>

/* ─── cache helpers ─────────────────────────────────────────────────────────── */

static int ensure_cache_dir(char *buf, size_t bufsz)
{
    const char *home = getenv("HOME");
    if (!home) home = "/root";
    snprintf(buf, bufsz, "%s/.cache/nexwm", home);
    struct stat st;
    if (stat(buf, &st) != 0) {
        if (mkdir(buf, 0755) != 0 && errno != EEXIST) return -1;
    }
    return 0;
}

static int cache_write(const char *path)
{
    char dir[512];
    if (ensure_cache_dir(dir, sizeof(dir)) < 0) return -1;
    char file[600];
    snprintf(file, sizeof(file), "%s/wallpaper", dir);
    FILE *f = fopen(file, "w");
    if (!f) return -1;
    fprintf(f, "%s\n", path);
    fclose(f);
    return 0;
}

static int cache_read(char *buf, size_t bufsz)
{
    char dir[512];
    if (ensure_cache_dir(dir, sizeof(dir)) < 0) return -1;
    char file[600];
    snprintf(file, sizeof(file), "%s/wallpaper", dir);
    FILE *f = fopen(file, "r");
    if (!f) return -1;
    if (!fgets(buf, (int)bufsz, f)) { fclose(f); return -1; }
    fclose(f);
    /* Strip newline */
    size_t len = strlen(buf);
    if (len > 0 && buf[len-1] == '\n') buf[len-1] = '\0';
    return 0;
}

/* ─── X11 root window painting ──────────────────────────────────────────────── */

/*
 * Set solid color on root window (always available without Imlib2).
 */
static int set_solid_color(Display *dpy, unsigned int rgb)
{
    Window root = DefaultRootWindow(dpy);
    int scr     = DefaultScreen(dpy);

    XColor col;
    col.red   = ((rgb >> 16) & 0xff) * 257;
    col.green = ((rgb >>  8) & 0xff) * 257;
    col.blue  = ((rgb)       & 0xff) * 257;
    col.flags = DoRed | DoGreen | DoBlue;

    if (!XAllocColor(dpy, DefaultColormap(dpy, scr), &col)) {
        fprintf(stderr, "nex-wallpaper: XAllocColor failed\n");
        return -1;
    }

    GC gc = XCreateGC(dpy, root, 0, NULL);
    XSetForeground(dpy, gc, col.pixel);
    XFillRectangle(dpy, root, gc,
                   0, 0,
                   (unsigned int)DisplayWidth(dpy, scr),
                   (unsigned int)DisplayHeight(dpy, scr));
    XFreeGC(dpy, gc);

    /* Persist via _XROOTPMAP_ID / ESETROOT_PMAP_ID for compositors */
    Atom prop = XInternAtom(dpy, "_XROOTPMAP_ID", False);
    Pixmap pix = XCreatePixmap(dpy, root, 1, 1, (unsigned int)DefaultDepth(dpy, scr));
    XChangeProperty(dpy, root, prop, XA_PIXMAP, 32,
                    PropModeReplace, (unsigned char *)&pix, 1);
    XSetWindowBackground(dpy, root, col.pixel);
    XClearWindow(dpy, root);
    XFlush(dpy);
    return 0;
}

/* ─── public API ──────────────────────────────────────────────────────────────*/

int nex_wallpaper_set(const nex_wp_config_t *cfg)
{
    if (!cfg) return -1;

    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "nex-wallpaper: cannot open display\n");
        return -1;
    }

    int ret = 0;

    if (!cfg->path || cfg->path[0] == '\0') {
        /* Solid color only */
        ret = set_solid_color(dpy, cfg->bg_color);
    } else {
#ifdef HAVE_IMLIB2
        /* Imlib2 image rendering path */
        int scr = DefaultScreen(dpy);
        int sw  = DisplayWidth(dpy, scr);
        int sh  = DisplayHeight(dpy, scr);
        Window root = DefaultRootWindow(dpy);

        imlib_context_set_display(dpy);
        imlib_context_set_visual(DefaultVisual(dpy, scr));
        imlib_context_set_colormap(DefaultColormap(dpy, scr));

        Imlib_Image img = imlib_load_image(cfg->path);
        if (!img) {
            fprintf(stderr, "nex-wallpaper: failed to load image '%s'\n", cfg->path);
            /* Fall back to solid color */
            ret = set_solid_color(dpy, cfg->bg_color);
            goto done;
        }

        imlib_context_set_image(img);
        int iw = imlib_image_get_width();
        int ih = imlib_image_get_height();

        Pixmap pix = XCreatePixmap(dpy, root,
                                   (unsigned int)sw, (unsigned int)sh,
                                   (unsigned int)DefaultDepth(dpy, scr));
        imlib_context_set_drawable(pix);

        int dx = 0, dy = 0, dw = sw, dh = sh;
        if (cfg->mode == NEX_WP_CENTER) {
            dw = iw; dh = ih;
            dx = (sw - iw) / 2;
            dy = (sh - ih) / 2;
            /* Fill background first */
            set_solid_color(dpy, cfg->bg_color);
        } else if (cfg->mode == NEX_WP_SCALE) {
            /* Maintain aspect, crop to fill */
            double rw = (double)sw / iw;
            double rh = (double)sh / ih;
            double r  = (rw > rh) ? rw : rh;
            dw = (int)(iw * r);
            dh = (int)(ih * r);
            dx = (sw - dw) / 2;
            dy = (sh - dh) / 2;
        }

        imlib_render_image_on_drawable_at_size(dx, dy, dw, dh);
        imlib_free_image();

        XSetWindowBackgroundPixmap(dpy, root, pix);
        XClearWindow(dpy, root);

        /* Persist pixmap for compositor */
        Atom prop  = XInternAtom(dpy, "_XROOTPMAP_ID",     False);
        Atom prop2 = XInternAtom(dpy, "ESETROOT_PMAP_ID",  False);
        XChangeProperty(dpy, root, prop,  XA_PIXMAP, 32, PropModeReplace, (unsigned char *)&pix, 1);
        XChangeProperty(dpy, root, prop2, XA_PIXMAP, 32, PropModeReplace, (unsigned char *)&pix, 1);
        XFlush(dpy);
        cache_write(cfg->path);
done:
        ;
#else
        /* Without Imlib2, fall back to solid color */
        fprintf(stderr, "nex-wallpaper: built without Imlib2 — using solid color fallback\n");
        ret = set_solid_color(dpy, cfg->bg_color);
#endif
    }

    XCloseDisplay(dpy);
    return ret;
}

void nex_wallpaper_restore(void)
{
    char cached[512];
    if (cache_read(cached, sizeof(cached)) < 0) return;

    nex_wp_config_t cfg = {
        .path     = cached,
        .mode     = NEX_WP_SCALE,
        .bg_color = 0x1a1a2e,
    };
    nex_wallpaper_set(&cfg);
}

/* ─── CLI entry point ────────────────────────────────────────────────────────── */

static void print_usage(void)
{
    printf("Usage:\n");
    printf("  nex-wallpaper --set <image> [--mode scale|stretch|center|tile]\n");
    printf("  nex-wallpaper --color <0xRRGGBB>\n");
    printf("  nex-wallpaper --restore\n\n");
    printf("Modes: scale (default), stretch, center, tile\n");
}

int main(int argc, char **argv)
{
    if (argc < 2) { print_usage(); return 1; }

    nex_wp_config_t cfg = {
        .path     = NULL,
        .mode     = NEX_WP_SCALE,
        .bg_color = 0x1a1a2e,
    };

    if (strcmp(argv[1], "--restore") == 0) {
        nex_wallpaper_restore();
        return 0;
    }

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--set") == 0 && i+1 < argc) {
            cfg.path = argv[++i];
        } else if (strcmp(argv[i], "--color") == 0 && i+1 < argc) {
            cfg.bg_color = (unsigned int)strtoul(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--mode") == 0 && i+1 < argc) {
            i++;
            if (strcmp(argv[i], "stretch") == 0)  cfg.mode = NEX_WP_STRETCH;
            else if (strcmp(argv[i], "center") == 0) cfg.mode = NEX_WP_CENTER;
            else if (strcmp(argv[i], "tile")   == 0) cfg.mode = NEX_WP_TILE;
            else cfg.mode = NEX_WP_SCALE;
        } else if (strcmp(argv[i], "--help") == 0) {
            print_usage(); return 0;
        }
    }

    return nex_wallpaper_set(&cfg);
}
