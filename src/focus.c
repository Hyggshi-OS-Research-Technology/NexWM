/*
 * focus.c - Focus management implementation for NexWM
 */

#include "focus.h"
#include "workspace.h"
#include "client.h"
#include "log.h"

extern nex_client_t *g_clients;
extern nex_client_t *g_focused;

void nex_focus_next(void)
{
    if (!g_focused) {
        nex_client_t *c;
        for (c = g_clients; c; c = c->next) {
            if (c->workspace == g_current_workspace) {
                nex_client_focus(c);
                return;
            }
        }
        return;
    }

    nex_client_t *c = g_focused->next;
    while (c && c->workspace != g_current_workspace) c = c->next;
    if (!c) {
        for (c = g_clients; c; c = c->next) {
            if (c->workspace == g_current_workspace) break;
        }
    }
    if (c && c != g_focused) nex_client_focus(c);
}

void nex_focus_prev(void)
{
    if (!g_focused) {
        nex_focus_next();
        return;
    }

    nex_client_t *c = g_focused->prev;
    while (c && c->workspace != g_current_workspace) c = c->prev;
    if (!c) {
        for (c = g_clients; c && c->next; c = c->next) {}
        while (c && c->workspace != g_current_workspace) c = c->prev;
    }
    if (c && c != g_focused) nex_client_focus(c);
}

void nex_focus_follow_mouse(xcb_window_t window)
{
    nex_client_t *c = nex_client_find(window);
    if (c && c != g_focused && g_config.focus_follows_mouse) {
        nex_client_focus(c);
    }
}
