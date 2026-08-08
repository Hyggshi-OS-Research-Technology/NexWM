/*
 * ipc.h - Inter-Process Communication for NexWM
 * Unix domain socket server
 */

#ifndef NEXWM_IPC_H
#define NEXWM_IPC_H

/* Default socket path (overridable via env NEX_SOCKET) */
#define NEX_IPC_SOCKET_PATH "/tmp/nexwm.sock"
#define NEX_IPC_MAX_MSG     256

/* IPC command strings:
 *   minimize <window_id>
 *   unminimize <window_id>
 *   focus <window_id>
 *   close <window_id>
 *   workspace <num>
 *   workspace-count <num>
 *   set-gap <pixels>
 *   set-layout <tiled|floating|monocle>
 *   reload
 *   quit
 */

int  nex_ipc_init(void);
void nex_ipc_handle(void);
void nex_ipc_cleanup(void);

/* Returns socket fd for integration into the event loop (poll alongside X11) */
int  nex_ipc_get_fd(void);

#endif
