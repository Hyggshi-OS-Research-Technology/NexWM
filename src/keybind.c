#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

/*
 * keybind.c - Keyboard shortcut implementation for NexWM
 */

#include "keybind.h"
#include "config.h"
#include "client.h"
#include "workspace.h"
#include "focus.h"
#include "monitor.h"
#include "wm.h"
#include "log.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <X11/keysym.h>

nex_keybind_ctx_t g_keybind_ctx = { NULL };

extern xcb_connection_t *g_conn;
extern xcb_screen_t *g_screen;

static void action_spawn(void *arg);
static void action_kill(void *arg);
static void action_toggle_floating(void *arg);
static void action_toggle_fullscreen(void *arg);
static void action_maximize(void *arg);
static void action_exit(void *arg);

static void grab_key(xcb_connection_t *conn, xcb_window_t root,
                     xcb_keysym_t keysym, uint16_t mod)
{
    xcb_keycode_t *keycodes = xcb_key_symbols_get_keycode(g_keybind_ctx.symbols, keysym);
    if (!keycodes) return;

    for (int i = 0; keycodes[i] != XCB_NO_SYMBOL; i++) {
        xcb_grab_key(conn, 1, root, mod, keycodes[i],
                     XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
        xcb_grab_key(conn, 1, root, mod | XCB_MOD_MASK_2, keycodes[i],
                     XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
        xcb_grab_key(conn, 1, root, mod | XCB_MOD_MASK_LOCK, keycodes[i],
                     XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
        xcb_grab_key(conn, 1, root, mod | XCB_MOD_MASK_2 | XCB_MOD_MASK_LOCK, keycodes[i],
                     XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
    }
    free(keycodes);
}

int nex_keybind_init(xcb_connection_t *conn)
{
    g_keybind_ctx.symbols = xcb_key_symbols_alloc(conn);
    if (!g_keybind_ctx.symbols) {
        NEX_ERROR("Failed to allocate key symbols");
        return -1;
    }
    return 0;
}

void nex_keybind_grab(xcb_connection_t *conn, xcb_window_t root)
{
    xcb_ungrab_key(conn, XCB_GRAB_ANY, root, XCB_MOD_MASK_ANY);

    for (int i = 0; i < 9 && i < g_config.workspace_count; i++) {
        grab_key(conn, root, XK_1 + i, g_config.modkey);
        grab_key(conn, root, XK_1 + i, g_config.modkey | XCB_MOD_MASK_SHIFT);
    }

    grab_key(conn, root, XK_Return, g_config.modkey);
    grab_key(conn, root, XK_d, g_config.modkey);
    grab_key(conn, root, XK_q, g_config.modkey);
    grab_key(conn, root, XK_f, g_config.modkey);
    grab_key(conn, root, XK_m, g_config.modkey);
    grab_key(conn, root, XK_space, g_config.modkey);
    grab_key(conn, root, XK_Tab, g_config.modkey);
    grab_key(conn, root, XK_Tab, g_config.modkey | XCB_MOD_MASK_SHIFT);
    grab_key(conn, root, XK_Escape, g_config.modkey | XCB_MOD_MASK_SHIFT);
    grab_key(conn, root, XK_h, g_config.modkey);
    grab_key(conn, root, XK_j, g_config.modkey);
    grab_key(conn, root, XK_k, g_config.modkey);
    grab_key(conn, root, XK_l, g_config.modkey);

    xcb_flush(conn);
    NEX_INFO("Keybinds grabbed");
}

void nex_keybind_handle(xcb_key_press_event_t *ev)
{
    xcb_keysym_t keysym = xcb_key_symbols_get_keysym(g_keybind_ctx.symbols, ev->detail, 0);
    uint16_t mod = ev->state & ~(XCB_MOD_MASK_2 | XCB_MOD_MASK_LOCK);

    NEX_DEBUG("Key pressed: keysym=0x%x, mod=0x%x", keysym, mod);

    if (mod == g_config.modkey && keysym >= XK_1 && keysym <= XK_9) {
        int ws = keysym - XK_1;
        if (ws < g_config.workspace_count) nex_workspace_switch(ws);
        return;
    }

    if (mod == (g_config.modkey | XCB_MOD_MASK_SHIFT) && keysym >= XK_1 && keysym <= XK_9) {
        int ws = keysym - XK_1;
        if (g_focused && ws < g_config.workspace_count) {
            g_focused->workspace = ws;
            nex_client_unmap(g_focused);
            nex_workspace_arrange(g_current_workspace);
        }
        return;
    }

    if (mod == g_config.modkey) {
        switch (keysym) {
            case XK_Return: action_spawn(g_config.terminal); break;
            case XK_d: action_spawn(g_config.launcher); break;
            case XK_q: action_kill(NULL); break;
            case XK_f: action_toggle_fullscreen(NULL); break;
            case XK_m: action_maximize(NULL); break;
            case XK_space: action_toggle_floating(NULL); break;
            case XK_Tab: nex_focus_next(); break;
            case XK_h:
            case XK_j:
            case XK_k:
            case XK_l: nex_focus_next(); break;
            default: break;
        }
    } else if (mod == (g_config.modkey | XCB_MOD_MASK_SHIFT)) {
        switch (keysym) {
            case XK_Tab: nex_focus_prev(); break;
            case XK_Escape: action_exit(NULL); break;
            default: break;
        }
    }
}

void nex_keybind_cleanup(void)
{
    if (g_keybind_ctx.symbols) {
        xcb_key_symbols_free(g_keybind_ctx.symbols);
        g_keybind_ctx.symbols = NULL;
    }
}

static void action_spawn(void *arg)
{
    const char *cmd = (const char *)arg;
    if (!cmd) return;

    pid_t pid = fork();
    if (pid == 0) {
        if (g_conn) close(xcb_get_file_descriptor(g_conn));
        setsid();
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(EXIT_FAILURE);
    } else if (pid < 0) {
        NEX_ERROR("Failed to fork for command: %s", cmd);
    } else {
        NEX_INFO("Spawned: %s (pid=%d)", cmd, pid);
    }
}

static void action_kill(void *arg)
{
    (void)arg;
    if (g_focused) nex_client_kill(g_focused);
}

static void action_toggle_floating(void *arg)
{
    (void)arg;
    if (g_focused) {
        g_focused->flags ^= NEX_CLIENT_FLOATING;
        NEX_INFO("Toggled floating for client 0x%x", g_focused->window);
    }
}

static void action_toggle_fullscreen(void *arg)
{
    (void)arg;
    if (!g_focused) return;

    if (g_focused->flags & NEX_CLIENT_FULLSCREEN) {
        g_focused->flags &= ~NEX_CLIENT_FULLSCREEN;
        nex_client_move(g_focused, g_focused->old_x, g_focused->old_y);
        nex_client_resize(g_focused, g_focused->old_width, g_focused->old_height);
        uint32_t values[] = { g_config.border_width };
        xcb_configure_window(g_conn, g_focused->window, XCB_CONFIG_WINDOW_BORDER_WIDTH, values);
    } else {
        g_focused->old_x = g_focused->x;
        g_focused->old_y = g_focused->y;
        g_focused->old_width = g_focused->width;
        g_focused->old_height = g_focused->height;
        g_focused->flags |= NEX_CLIENT_FULLSCREEN;

        nex_monitor_t *m = nex_monitor_current();
        nex_client_move(g_focused, m->x, m->y);
        nex_client_resize(g_focused, m->width, m->height);
        uint32_t values[] = { 0 };
        xcb_configure_window(g_conn, g_focused->window, XCB_CONFIG_WINDOW_BORDER_WIDTH, values);
    }
    nex_client_raise(g_focused);
}

static void action_maximize(void *arg)
{
    (void)arg;
    if (!g_focused) return;

    if (g_focused->flags & NEX_CLIENT_MAXIMIZED) {
        g_focused->flags &= ~NEX_CLIENT_MAXIMIZED;
        nex_client_move(g_focused, g_focused->old_x, g_focused->old_y);
        nex_client_resize(g_focused, g_focused->old_width, g_focused->old_height);
    } else {
        g_focused->old_x = g_focused->x;
        g_focused->old_y = g_focused->y;
        g_focused->old_width = g_focused->width;
        g_focused->old_height = g_focused->height;
        g_focused->flags |= NEX_CLIENT_MAXIMIZED;

        nex_monitor_t *m = nex_monitor_current();
        int bw = g_config.border_width;
        nex_client_move(g_focused, m->x + bw, m->y + bw);
        nex_client_resize(g_focused, m->width - 2 * bw, m->height - 2 * bw);
    }
    nex_client_raise(g_focused);
}

static void action_exit(void *arg)
{
    (void)arg;
    NEX_INFO("Exit requested via keybind");
    extern int g_running;
    g_running = 0;
}
