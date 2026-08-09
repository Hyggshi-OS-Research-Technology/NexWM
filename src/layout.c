/*
 * layout.c - Layout engine implementation for NexWM
 */

#include "layout.h"
#include "config.h"
#include "monitor.h"
#include "log.h"

#define NEX_PANEL_HEIGHT 36

extern xcb_connection_t *g_conn;
extern int g_current_workspace;

void nex_layout_tile(nex_monitor_t *m)
{
    if (!m) m = nex_monitor_current();
    if (!m) return;

    int panel_h = NEX_PANEL_HEIGHT;
    int gaps = g_config.gaps;
    int bw = g_config.border_width;

    int wx = m->x + gaps;
    int wy = m->y + panel_h + gaps;
    int ww = m->width - 2 * gaps;
    int wh = m->height - panel_h - 2 * gaps;

    if (ww <= 0 || wh <= 0) return;

    /* Count tileable windows in current workspace */
    int count = 0;
    nex_client_t *c;
    for (c = g_clients; c; c = c->next) {
        if (c->workspace == g_current_workspace &&
            !(c->flags & (NEX_CLIENT_FLOATING | NEX_CLIENT_MINIMIZED | NEX_CLIENT_FULLSCREEN))) {
            count++;
        }
    }

    if (count == 0) return;

    if (count == 1) {
        for (c = g_clients; c; c = c->next) {
            if (c->workspace == g_current_workspace &&
                !(c->flags & (NEX_CLIENT_FLOATING | NEX_CLIENT_MINIMIZED | NEX_CLIENT_FULLSCREEN))) {
                nex_client_move(c, wx, wy);
                nex_client_resize(c, ww - 2 * bw, wh - 2 * bw);
                break;
            }
        }
        return;
    }

    int mw = (ww - gaps) / 2;
    int sw = ww - mw - gaps;
    int stack_count = count - 1;
    int sh = (wh - (stack_count - 1) * gaps) / stack_count;

    int idx = 0;
    for (c = g_clients; c; c = c->next) {
        if (c->workspace != g_current_workspace ||
            (c->flags & (NEX_CLIENT_FLOATING | NEX_CLIENT_MINIMIZED | NEX_CLIENT_FULLSCREEN))) {
            continue;
        }

        if (idx == 0) {
            /* Master window */
            nex_client_move(c, wx, wy);
            nex_client_resize(c, mw - 2 * bw, wh - 2 * bw);
        } else {
            /* Stack window */
            int s_idx = idx - 1;
            int sy = wy + s_idx * (sh + gaps);
            int cur_sh = (s_idx == stack_count - 1) ? (wy + wh - sy) : sh;
            nex_client_move(c, wx + mw + gaps, sy);
            nex_client_resize(c, sw - 2 * bw, cur_sh - 2 * bw);
        }
        idx++;
    }
}

void nex_layout_floating(nex_monitor_t *m)
{
    (void)m;
}

void nex_layout_monocle(nex_monitor_t *m)
{
    if (!m) m = nex_monitor_current();
    if (!m) return;

    int panel_h = NEX_PANEL_HEIGHT;
    int gaps = g_config.gaps;
    int bw = g_config.border_width;

    int wx = m->x + gaps;
    int wy = m->y + panel_h + gaps;
    int ww = m->width - 2 * gaps;
    int wh = m->height - panel_h - 2 * gaps;

    nex_client_t *c;
    for (c = g_clients; c; c = c->next) {
        if (c->workspace == g_current_workspace &&
            !(c->flags & (NEX_CLIENT_FLOATING | NEX_CLIENT_MINIMIZED | NEX_CLIENT_FULLSCREEN))) {
            nex_client_move(c, wx, wy);
            nex_client_resize(c, ww - 2 * bw, wh - 2 * bw);
        }
    }
}

void nex_layout_apply(nex_monitor_t *m, nex_layout_t layout)
{
    if (!m) m = nex_monitor_current();

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
