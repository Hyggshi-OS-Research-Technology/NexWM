/*
 * nex-wallpaper.h - NexWallpaper header
 * Standalone wallpaper setter and daemon for NexWM
 */

#ifndef NEX_WALLPAPER_H
#define NEX_WALLPAPER_H

typedef enum {
    NEX_WP_SCALE,    /* Scale to fill screen, preserving aspect (crop) */
    NEX_WP_STRETCH,  /* Stretch to fill screen */
    NEX_WP_CENTER,   /* Center on solid background color */
    NEX_WP_TILE,     /* Tile across screen */
} nex_wp_mode_t;

typedef struct {
    const char   *path;      /* Path to image file (NULL = solid color only) */
    nex_wp_mode_t mode;
    unsigned int  bg_color;  /* 0xRRGGBB fallback / solid color */
} nex_wp_config_t;

int  nex_wallpaper_set(const nex_wp_config_t *cfg);
void nex_wallpaper_restore(void);  /* Re-apply last wallpaper after WM restart */

#endif
