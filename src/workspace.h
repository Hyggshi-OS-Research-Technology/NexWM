/*
 * workspace.h - Workspace management for NexWM
 */

#ifndef NEXWM_WORKSPACE_H
#define NEXWM_WORKSPACE_H

#include "client.h"
#include "config.h"

typedef struct {
    int id;
    char name[32];
    nex_client_t *clients;
    nex_layout_t layout;
    int client_count;
} nex_workspace_t;

extern nex_workspace_t g_workspaces[NEX_MAX_WORKSPACES];
extern int g_current_workspace;

void nex_workspace_init(int count);
void nex_workspace_switch(int ws);
void nex_workspace_add_client(int ws, nex_client_t *c);
void nex_workspace_remove_client(int ws, nex_client_t *c);
void nex_workspace_arrange(int ws);

#endif
