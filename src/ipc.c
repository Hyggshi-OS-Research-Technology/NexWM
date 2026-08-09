#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

/*
 * ipc.c - Unix domain socket IPC server for NexWM
 *
 * Commands handled:
 *   minimize <win>       - Minimize (unmap + _NET_WM_STATE_HIDDEN)
 *   unminimize <win>     - Restore minimized window
 *   focus <win>          - Focus window
 *   close <win>          - Gracefully close window
 *   workspace <num>      - Switch to workspace N (1-indexed)
 *   workspace-count <n>  - Change number of workspaces
 *   set-gap <pixels>     - Set gap size and re-arrange
 *   set-layout <mode>    - Set layout: tiled | floating | monocle
 *   reload               - Reload nexwm.conf
 *   quit                 - Exit NexWM
 */

#include "ipc.h"
#include "client.h"
#include "workspace.h"
#include "ewmh.h"
#include "config.h"
#include "log.h"
#include "wm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>

static int g_ipc_fd = -1;

static const char *ipc_socket_path(void)
{
    const char *env = getenv("NEX_SOCKET");
    return env ? env : NEX_IPC_SOCKET_PATH;
}

static void set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags != -1) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int nex_ipc_init(void)
{
    const char *path = ipc_socket_path();
    unlink(path);  /* Remove stale socket */

    g_ipc_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_ipc_fd < 0) {
        NEX_ERROR("IPC: Failed to create socket: %s", strerror(errno));
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    addr.sun_path[sizeof(addr.sun_path) - 1] = '\0';

    if (bind(g_ipc_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        NEX_ERROR("IPC: Failed to bind socket: %s", strerror(errno));
        close(g_ipc_fd);
        g_ipc_fd = -1;
        return -1;
    }

    if (listen(g_ipc_fd, 8) < 0) {
        NEX_ERROR("IPC: Failed to listen on socket: %s", strerror(errno));
        close(g_ipc_fd);
        g_ipc_fd = -1;
        return -1;
    }

    set_nonblocking(g_ipc_fd);
    NEX_INFO("IPC: Listening on %s", path);
    return 0;
}

int nex_ipc_get_fd(void)
{
    return g_ipc_fd;
}

static void ipc_dispatch(const char *cmd)
{
    NEX_DEBUG("IPC command: '%s'", cmd);

    unsigned long win_id = 0;

    if (strncmp(cmd, "minimize ", 9) == 0) {
        win_id = strtoul(cmd + 9, NULL, 0);
        nex_client_t *c = nex_client_find((xcb_window_t)win_id);
        if (c) nex_client_minimize(c);
        else NEX_WARN("IPC: minimize: window 0x%lx not found", win_id);

    } else if (strncmp(cmd, "unminimize ", 11) == 0) {
        win_id = strtoul(cmd + 11, NULL, 0);
        nex_client_t *c = nex_client_find((xcb_window_t)win_id);
        if (c) nex_client_unminimize(c);
        else NEX_WARN("IPC: unminimize: window 0x%lx not found", win_id);

    } else if (strcmp(cmd, "fullscreen") == 0 || strncmp(cmd, "fullscreen ", 11) == 0) {
        if (strlen(cmd) > 11) {
            win_id = strtoul(cmd + 11, NULL, 0);
            nex_client_t *c = nex_client_find((xcb_window_t)win_id);
            if (c) nex_client_toggle_fullscreen(c);
            else NEX_WARN("IPC: fullscreen: window 0x%lx not found", win_id);
        } else if (g_focused) {
            nex_client_toggle_fullscreen(g_focused);
        }

    } else if (strcmp(cmd, "maximize") == 0 || strncmp(cmd, "maximize ", 9) == 0) {
        if (strlen(cmd) > 9) {
            win_id = strtoul(cmd + 9, NULL, 0);
            nex_client_t *c = nex_client_find((xcb_window_t)win_id);
            if (c) nex_client_toggle_maximize(c);
            else NEX_WARN("IPC: maximize: window 0x%lx not found", win_id);
        } else if (g_focused) {
            nex_client_toggle_maximize(g_focused);
        }

    } else if (strncmp(cmd, "focus ", 6) == 0) {
        win_id = strtoul(cmd + 6, NULL, 0);
        nex_client_t *c = nex_client_find((xcb_window_t)win_id);
        if (c) {
            nex_client_raise(c);
            nex_client_focus(c);
        } else NEX_WARN("IPC: focus: window 0x%lx not found", win_id);

    } else if (strncmp(cmd, "close ", 6) == 0) {
        win_id = strtoul(cmd + 6, NULL, 0);
        nex_client_t *c = nex_client_find((xcb_window_t)win_id);
        if (c) nex_client_kill(c);
        else NEX_WARN("IPC: close: window 0x%lx not found", win_id);

    } else if (strncmp(cmd, "workspace ", 10) == 0) {
        int ws = atoi(cmd + 10) - 1;  /* 1-indexed input */
        if (ws >= 0 && ws < g_config.workspace_count) {
            nex_workspace_switch(ws);
            nex_ewmh_set_current_desktop(ws);
        } else NEX_WARN("IPC: workspace: invalid workspace %d", ws + 1);

    } else if (strncmp(cmd, "workspace-count ", 16) == 0) {
        int count = atoi(cmd + 16);
        if (count > 0 && count <= NEX_MAX_WORKSPACES) {
            g_config.workspace_count = count;
            nex_ewmh_set_number_of_desktops(count);
            NEX_INFO("IPC: workspace count set to %d", count);
        }

    } else if (strncmp(cmd, "set-gap ", 8) == 0) {
        int gap = atoi(cmd + 8);
        if (gap >= 0 && gap <= 128) {
            g_config.gaps = (uint32_t)gap;
            nex_workspace_arrange(g_current_workspace);
            NEX_INFO("IPC: gap set to %d", gap);
        }

    } else if (strncmp(cmd, "set-layout ", 11) == 0) {
        const char *mode = cmd + 11;
        if (strcmp(mode, "tiled") == 0 || strcmp(mode, "tiling") == 0) {
            g_workspaces[g_current_workspace].layout = NEX_LAYOUT_TILED;
        } else if (strcmp(mode, "floating") == 0) {
            g_workspaces[g_current_workspace].layout = NEX_LAYOUT_FLOATING;
        } else if (strcmp(mode, "monocle") == 0) {
            g_workspaces[g_current_workspace].layout = NEX_LAYOUT_MONOCLE;
        } else {
            NEX_WARN("IPC: set-layout: unknown layout '%s'", mode);
        }
        nex_workspace_arrange(g_current_workspace);
        NEX_INFO("IPC: layout set to '%s'", mode);

    } else if (strcmp(cmd, "reload") == 0) {
        NEX_INFO("IPC: Reloading configuration");
        nex_config_load(NULL);  /* Re-load from default path */
        nex_ewmh_set_number_of_desktops(g_config.workspace_count);
        nex_workspace_arrange(g_current_workspace);

    } else if (strcmp(cmd, "quit") == 0) {
        NEX_INFO("IPC: Quit requested");
        g_running = 0;

    } else {
        NEX_WARN("IPC: Unknown command: '%s'", cmd);
    }

    xcb_flush(g_conn);
}

void nex_ipc_handle(void)
{
    if (g_ipc_fd < 0) return;

    int client_fd;
    while ((client_fd = accept(g_ipc_fd, NULL, NULL)) >= 0) {
        char buf[NEX_IPC_MAX_MSG];
        ssize_t n = recv(client_fd, buf, sizeof(buf) - 1, 0);
        if (n > 0) {
            buf[n] = '\0';
            /* Strip trailing newline */
            if (n > 0 && buf[n-1] == '\n') buf[n-1] = '\0';
            ipc_dispatch(buf);
            ssize_t sent = send(client_fd, "ok\n", 3, 0);
            (void)sent;
        }
        close(client_fd);
    }
}

void nex_ipc_cleanup(void)
{
    if (g_ipc_fd >= 0) {
        close(g_ipc_fd);
        g_ipc_fd = -1;
        unlink(ipc_socket_path());
        NEX_INFO("IPC: Socket closed");
    }
}
