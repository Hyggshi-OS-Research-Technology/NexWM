/*
 * nex-desktop.h - NexDesktop header
 * Desktop icon manager for NexWM / Hyggshi OS
 */

#ifndef NEX_DESKTOP_H
#define NEX_DESKTOP_H

#include <xcb/xcb.h>

#define NEX_DESKTOP_ICON_SIZE   48
#define NEX_DESKTOP_GRID_W      96
#define NEX_DESKTOP_GRID_H      96
#define NEX_DESKTOP_MAX_FILES   256

typedef struct {
    char name[256];
    char path[1024];
    char exec[512];
    char icon[256];
    int is_desktop_file;
} nex_desktop_item_t;

typedef struct {
    nex_desktop_item_t items[NEX_DESKTOP_MAX_FILES];
    int count;
} nex_desktop_item_list_t;

int nex_desktop_scan(nex_desktop_item_list_t *list);
int nex_desktop_launch(const nex_desktop_item_t *item);

#endif
