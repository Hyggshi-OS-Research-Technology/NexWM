#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

/*
 * nexwmctl.c - NexWM IPC Controller
 *
 * Usage:
 *   nexwmctl <command> [args...]
 *
 * Commands:
 *   minimize <window_id>          Minimize a window
 *   unminimize <window_id>        Restore a minimized window
 *   focus <window_id>             Focus a window
 *   close <window_id>             Close a window
 *   workspace <num>               Switch to workspace (1-indexed)
 *   workspace-count <num>         Set number of workspaces
 *   set-gap <pixels>              Set gap size
 *   set-layout <tiled|floating|monocle>  Set layout mode
 *   reload                        Reload nexwm.conf
 *   quit                          Quit NexWM
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>

#define NEX_IPC_SOCKET_PATH "/tmp/nexwm.sock"
#define NEX_IPC_MAX_MSG     256

static const char *socket_path(void)
{
    const char *env = getenv("NEX_SOCKET");
    return env ? env : NEX_IPC_SOCKET_PATH;
}

static int send_command(const char *msg)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        fprintf(stderr, "nexwmctl: socket error: %s\n", strerror(errno));
        return 1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path(), sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "nexwmctl: cannot connect to NexWM socket '%s': %s\n",
                socket_path(), strerror(errno));
        close(fd);
        return 1;
    }

    if (send(fd, msg, strlen(msg), 0) < 0) {
        fprintf(stderr, "nexwmctl: send error: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    char resp[64];
    ssize_t n = recv(fd, resp, sizeof(resp) - 1, 0);
    if (n > 0) {
        resp[n] = '\0';
        /* Silent on "ok" response; print errors */
        if (strncmp(resp, "ok", 2) != 0) {
            fprintf(stderr, "nexwmctl: %s\n", resp);
        }
    }

    close(fd);
    return 0;
}

static void print_usage(void)
{
    printf("Usage: nexwmctl <command> [args...]\n\n");
    printf("Commands:\n");
    printf("  minimize <window_id>              Minimize a window\n");
    printf("  unminimize <window_id>            Restore a minimized window\n");
    printf("  focus <window_id>                 Focus a window\n");
    printf("  close <window_id>                 Close a window\n");
    printf("  workspace <num>                   Switch to workspace (1-indexed)\n");
    printf("  workspace-count <num>             Set number of workspaces\n");
    printf("  set-gap <pixels>                  Set gap size\n");
    printf("  set-layout <tiled|floating|monocle> Set layout mode\n");
    printf("  reload                            Reload nexwm.conf\n");
    printf("  quit                              Quit NexWM\n");
    printf("\nEnvironment:\n");
    printf("  NEX_SOCKET  Override default socket path (%s)\n", NEX_IPC_SOCKET_PATH);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        print_usage();
        return 1;
    }

    /* Build IPC command string from argv */
    char msg[NEX_IPC_MAX_MSG];
    int pos = 0;
    for (int i = 1; i < argc; i++) {
        int n = snprintf(msg + pos, sizeof(msg) - (size_t)pos, "%s%s",
                         (i > 1) ? " " : "", argv[i]);
        if (n < 0 || (size_t)n >= sizeof(msg) - (size_t)pos) {
            fprintf(stderr, "nexwmctl: command too long\n");
            return 1;
        }
        pos += n;
    }

    return send_command(msg);
}
