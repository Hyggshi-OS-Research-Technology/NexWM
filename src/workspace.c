/*
 * workspace.c - Workspace management implementation for NexWM
 */

#include "workspace.h"
#include "config.h"
#include "layout.h"
#include "log.h"
#include <string.h>

nex_workspace_t g_workspaces[NEX_MAX_WORKSPACES];
int g_current_workspace = 0;

extern xcb_connection_t *g_conn;

void nex_workspace_init(int count)
{
    int i;
    for (i = 0; i < count && i < NEX_MAX_WORKSPACES; i++) {
        snprintf(g_workspaces[i].name, sizeof(g_workspaces[i].name), "Workspace %d", i + 1);
        g_workspaces[i].id = i;
        g_workspaces[i].clients = NULL;
        g_workspaces[i].layout = g_config.default_layout;
        g_workspaces[i].client_count = 0;
    }
    g_current_workspace = 0;
    NEX_INFO("Initialized %d workspaces", count);
}

void nex_workspace_switch(int ws)
{
    if (ws < 0 || ws >= g_config.workspace_count || ws == g_current_workspace) return;

    NEX_INFO("Switching to workspace %d", ws + 1);

    nex_client_t *c;
    for (c = g_clients; c; c = c->next) {
        if (c->workspace == g_current_workspace && !(c->flags & NEX_CLIENT_FIXED)) {
            nex_client_unmap(c);
        }
    }

    g_current_workspace = ws;

    for (c = g_clients; c; c = c->next) {
        if (c->workspace == g_current_workspace) {
            nex_client_map(c);
        }
    }

    nex_workspace_arrange(ws);
}

void nex_workspace_add_client(int ws, nex_client_t *c)
{
    if (!c || ws < 0 || ws >= NEX_MAX_WORKSPACES) return;
    c->workspace = ws;
    g_workspaces[ws].client_count++;
    NEX_DEBUG("Added client 0x%x to workspace %d", c->window, ws + 1);
}

void nex_workspace_remove_client(int ws, nex_client_t *c)
{
    if (!c || ws < 0 || ws >= NEX_MAX_WORKSPACES) return;
    if (c->workspace == ws) {
        g_workspaces[ws].client_count--;
        if (g_workspaces[ws].client_count < 0) g_workspaces[ws].client_count = 0;
        NEX_DEBUG("Removed client 0x%x from workspace %d", c->window, ws + 1);
    }
}

void nex_workspace_arrange(int ws)
{
    if (ws < 0 || ws >= NEX_MAX_WORKSPACES) return;

    nex_layout_apply(nex_monitor_current(), g_workspaces[ws].layout);

    nex_client_t *c;
    for (c = g_clients; c; c = c->next) {
        if (c->workspace == ws && c == g_focused) {
            nex_client_raise(c);
        }
    }
}
