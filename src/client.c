/*
 * client.c - Client window management implementation for NexWM
 */

#include "client.h"
#include "wm.h"
#include "atoms.h"
#include "ewmh.h"
#include "monitor.h"
#include "config.h"
#include "log.h"
#include <string.h>
#include <stdlib.h>

nex_client_t *g_clients = NULL;
nex_client_t *g_focused = NULL;

extern xcb_connection_t *g_conn;
extern xcb_screen_t *g_screen;
extern nex_atoms_t g_atoms;
extern nex_config_t g_config;

nex_client_t *nex_client_create(xcb_window_t window)
{
    nex_client_t *c = calloc(1, sizeof(nex_client_t));
    if (!c) {
        NEX_ERROR("Failed to allocate client");
        return NULL;
    }

    c->window = window;
    c->workspace = 0;
    c->flags = 0;
    c->next = NULL;
    c->prev = NULL;

    xcb_get_geometry_cookie_t geo_cookie = xcb_get_geometry(g_conn, window);
    xcb_get_geometry_reply_t *geo = xcb_get_geometry_reply(g_conn, geo_cookie, NULL);
    if (geo) {
        c->x = geo->x;
        c->y = geo->y;
        c->width = geo->width;
        c->height = geo->height;
        free(geo);
    }

    xcb_icccm_get_wm_class_reply_t wm_class;
    xcb_get_property_cookie_t class_cookie = xcb_icccm_get_wm_class(g_conn, window);
    if (xcb_icccm_get_wm_class_reply(g_conn, class_cookie, &wm_class, NULL)) {
        strncpy(c->class, wm_class.class_name ? wm_class.class_name : "", sizeof(c->class) - 1);
        strncpy(c->instance, wm_class.instance_name ? wm_class.instance_name : "", sizeof(c->instance) - 1);
        xcb_icccm_get_wm_class_reply_wipe(&wm_class);
    }

    xcb_icccm_get_text_property_reply_t title_reply;
    xcb_get_property_cookie_t title_cookie = xcb_icccm_get_wm_name(g_conn, window);
    if (xcb_icccm_get_wm_name_reply(g_conn, title_cookie, &title_reply, NULL)) {
        strncpy(c->title, title_reply.name ? title_reply.name : "", sizeof(c->title) - 1);
        xcb_icccm_get_text_property_reply_wipe(&title_reply);
    }

    uint32_t values[1];
    values[0] = g_config.border_width;
    xcb_configure_window(g_conn, window, XCB_CONFIG_WINDOW_BORDER_WIDTH, values);

    uint32_t mask = XCB_EVENT_MASK_ENTER_WINDOW |
                    XCB_EVENT_MASK_LEAVE_WINDOW |
                    XCB_EVENT_MASK_PROPERTY_CHANGE |
                    XCB_EVENT_MASK_STRUCTURE_NOTIFY;
    xcb_change_window_attributes(g_conn, window, XCB_CW_EVENT_MASK, &mask);

    nex_client_grab_buttons(c);

    NEX_INFO("Created client: window=0x%x, class=\"%s\", title=\"%s\"",
             window, c->class, c->title);

    nex_client_list_add(c);
    return c;
}

void nex_client_grab_buttons(nex_client_t *c)
{
    if (!c) return;
    xcb_ungrab_button(g_conn, XCB_BUTTON_INDEX_ANY, c->window, XCB_MOD_MASK_ANY);

    uint16_t modifiers[] = {
        g_config.modkey,
        g_config.modkey | XCB_MOD_MASK_2,
        g_config.modkey | XCB_MOD_MASK_LOCK,
        g_config.modkey | XCB_MOD_MASK_2 | XCB_MOD_MASK_LOCK
    };

    for (size_t i = 0; i < sizeof(modifiers)/sizeof(modifiers[0]); i++) {
        xcb_grab_button(g_conn, 0, c->window,
                        XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE,
                        XCB_GRAB_MODE_SYNC, XCB_GRAB_MODE_ASYNC,
                        XCB_NONE, XCB_NONE,
                        XCB_BUTTON_INDEX_1, modifiers[i]);
        xcb_grab_button(g_conn, 0, c->window,
                        XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE,
                        XCB_GRAB_MODE_SYNC, XCB_GRAB_MODE_ASYNC,
                        XCB_NONE, XCB_NONE,
                        XCB_BUTTON_INDEX_3, modifiers[i]);
    }
}

void nex_client_destroy(nex_client_t *c)
{
    if (!c) return;
    NEX_INFO("Destroying client: window=0x%x", c->window);
    if (g_focused == c) g_focused = NULL;
    nex_client_list_remove(c);
    free(c);
}

nex_client_t *nex_client_find(xcb_window_t window)
{
    nex_client_t *c;
    for (c = g_clients; c; c = c->next) {
        if (c->window == window) return c;
    }
    return NULL;
}

void nex_client_focus(nex_client_t *c)
{
    if (!c || c == g_focused) return;
    if (g_focused) nex_client_set_border(g_focused, g_config.border_normal);
    g_focused = c;
    nex_client_set_border(c, g_config.border_focus);
    xcb_set_input_focus(g_conn, XCB_INPUT_FOCUS_POINTER_ROOT, c->window, XCB_CURRENT_TIME);
    NEX_INFO("Focused client: window=0x%x", c->window);
}

void nex_client_raise(nex_client_t *c)
{
    if (!c) return;
    uint32_t values[] = { XCB_STACK_MODE_ABOVE };
    xcb_configure_window(g_conn, c->window, XCB_CONFIG_WINDOW_STACK_MODE, values);
}

void nex_client_move(nex_client_t *c, int x, int y)
{
    if (!c) return;
    c->x = x; c->y = y;
    uint32_t values[] = { (uint32_t)x, (uint32_t)y };
    xcb_configure_window(g_conn, c->window, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, values);
}

void nex_client_resize(nex_client_t *c, int w, int h)
{
    if (!c) return;
    c->width = w; c->height = h;
    uint32_t values[] = { (uint32_t)w, (uint32_t)h };
    xcb_configure_window(g_conn, c->window, XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, values);
}

void nex_client_set_border(nex_client_t *c, uint32_t color)
{
    if (!c) return;
    uint32_t values[] = { color };
    xcb_change_window_attributes(g_conn, c->window, XCB_CW_BORDER_PIXEL, values);
}

void nex_client_map(nex_client_t *c)
{
    if (!c) return;
    xcb_map_window(g_conn, c->window);
}

void nex_client_unmap(nex_client_t *c)
{
    if (!c) return;
    xcb_unmap_window(g_conn, c->window);
}

void nex_client_minimize(nex_client_t *c)
{
    if (!c || (c->flags & NEX_CLIENT_MINIMIZED)) return;
    c->flags |= NEX_CLIENT_MINIMIZED;
    xcb_unmap_window(g_conn, c->window);
    nex_ewmh_set_wm_state_hidden(c->window, 1);
    if (g_focused == c) {
        g_focused = NULL;
        /* Try to focus the next visible client */
        nex_client_t *n;
        for (n = g_clients; n; n = n->next) {
            if (n != c && !(n->flags & NEX_CLIENT_MINIMIZED) && n->workspace == c->workspace) {
                nex_client_focus(n);
                break;
            }
        }
    }
    xcb_flush(g_conn);
    NEX_INFO("Minimized client 0x%x", c->window);
}

void nex_client_unminimize(nex_client_t *c)
{
    if (!c || !(c->flags & NEX_CLIENT_MINIMIZED)) return;
    c->flags &= ~NEX_CLIENT_MINIMIZED;
    xcb_map_window(g_conn, c->window);
    nex_ewmh_set_wm_state_hidden(c->window, 0);
    nex_client_focus(c);
    xcb_flush(g_conn);
    NEX_INFO("Unminimized client 0x%x", c->window);
}

void nex_client_toggle_fullscreen(nex_client_t *c)
{
    if (!c) return;

    if (c->flags & NEX_CLIENT_FULLSCREEN) {
        c->flags &= ~NEX_CLIENT_FULLSCREEN;
        nex_client_move(c, c->old_x, c->old_y);
        nex_client_resize(c, c->old_width, c->old_height);
        uint32_t values[] = { g_config.border_width };
        xcb_configure_window(g_conn, c->window, XCB_CONFIG_WINDOW_BORDER_WIDTH, values);
    } else {
        c->old_x = c->x;
        c->old_y = c->y;
        c->old_width = c->width;
        c->old_height = c->height;
        c->flags |= NEX_CLIENT_FULLSCREEN;

        nex_monitor_t *m = nex_monitor_current();
        nex_client_move(c, m->x, m->y);
        nex_client_resize(c, m->width, m->height);
        uint32_t values[] = { 0 };
        xcb_configure_window(g_conn, c->window, XCB_CONFIG_WINDOW_BORDER_WIDTH, values);
    }
    nex_client_raise(c);
}

void nex_client_toggle_maximize(nex_client_t *c)
{
    if (!c) return;

    if (c->flags & NEX_CLIENT_MAXIMIZED) {
        c->flags &= ~NEX_CLIENT_MAXIMIZED;
        nex_client_move(c, c->old_x, c->old_y);
        nex_client_resize(c, c->old_width, c->old_height);
    } else {
        c->old_x = c->x;
        c->old_y = c->y;
        c->old_width = c->width;
        c->old_height = c->height;
        c->flags |= NEX_CLIENT_MAXIMIZED;

        nex_monitor_t *m = nex_monitor_current();
        int bw = g_config.border_width;
        nex_client_move(c, m->x + bw, m->y + bw);
        nex_client_resize(c, m->width - 2 * bw, m->height - 2 * bw);
    }
    nex_client_raise(c);
}

void nex_client_kill(nex_client_t *c)
{
    if (!c) return;
    xcb_icccm_get_wm_protocols_reply_t protocols;
    xcb_get_property_cookie_t proto_cookie = xcb_icccm_get_wm_protocols(g_conn, c->window, g_atoms.wm_protocols);
    int has_delete = 0;
    if (xcb_icccm_get_wm_protocols_reply(g_conn, proto_cookie, &protocols, NULL)) {
        for (unsigned int i = 0; i < protocols.atoms_len; i++) {
            if (protocols.atoms[i] == g_atoms.wm_delete_window) {
                has_delete = 1;
                break;
            }
        }
        xcb_icccm_get_wm_protocols_reply_wipe(&protocols);
    }

    if (has_delete) {
        xcb_client_message_event_t ev = {
            .response_type = XCB_CLIENT_MESSAGE,
            .format = 32,
            .window = c->window,
            .type = g_atoms.wm_protocols,
            .data = { .data32 = { g_atoms.wm_delete_window, XCB_CURRENT_TIME } }
        };
        xcb_send_event(g_conn, 0, c->window, XCB_EVENT_MASK_NO_EVENT, (const char *)&ev);
        NEX_INFO("Sent WM_DELETE_WINDOW to client 0x%x", c->window);
    } else {
        xcb_kill_client(g_conn, c->window);
        NEX_INFO("Killed client 0x%x (no WM_DELETE_WINDOW)", c->window);
    }
}

void nex_client_list_add(nex_client_t *c)
{
    if (!c) return;
    c->next = g_clients;
    if (g_clients) g_clients->prev = c;
    g_clients = c;
}

void nex_client_list_remove(nex_client_t *c)
{
    if (!c) return;
    if (c->next) c->next->prev = c->prev;
    if (c->prev) c->prev->next = c->next;
    else g_clients = c->next;
    c->next = NULL;
    c->prev = NULL;
}
