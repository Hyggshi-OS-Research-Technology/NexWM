/*
 * layout.c - Layout engine implementation for NexWM
 */

#include "layout.h"
#include "config.h"
#include "log.h"

extern xcb_connection_t *g_conn;

void nex_layout_tile(nex_monitor_t *m)
{
    (void)m;
    NEX_DEBUG("Tiling layout not yet implemented");
}

void nex_layout_floating(nex_monitor_t *m)
{
    (void)m;
    NEX_DEBUG("Floating layout active");
}

void nex_layout_monocle(nex_monitor_t *m)
{
    (void)m;
    NEX_DEBUG("Monocle layout not yet implemented");
}

void nex_layout_apply(nex_monitor_t *m, nex_layout_t layout)
{
    switch (layout) {
        case NEX_LAYOUT_TILED:
            nex_layout_tile(m);
            break;
        case NEX_LAYOUT_MONOCLE:
            nex_layout_monocle(m);
            break;
        case NEX_LAYOUT_FLOATING:
        default:
            nex_layout_floating(m);
            break;
    }
}
