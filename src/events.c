/*
 * events.c - Event handling implementation for NexWM
 */

#include "events.h"
#include "wm.h"
#include "client.h"
#include "workspace.h"
#include "focus.h"
#include "keybind.h"
#include "ewmh.h"
#include "rules.h"
#include "monitor.h"
#include "log.h"
#include <xcb/xcb_icccm.h>
#include <string.h>
#include <stdlib.h>

static void handle_map_request(xcb_map_request_event_t *ev)
{
    xcb_window_t window = ev->window;
    NEX_INFO("MapRequest: window=0x%x", window);

    if (nex_client_find(window)) {
        xcb_map_window(g_conn, window);
        return;
    }

    nex_client_t *c = nex_client_create(window);
    if (!c) {
        xcb_map_window(g_conn, window);
        return;
    }

    nex_rules_apply(c);
    if (c->workspace < 0 || c->workspace >= g_config.workspace_count) {
        c->workspace = g_current_workspace;
    }

    nex_ewmh_set_wm_desktop(window, c->workspace);
    nex_client_map(c);

    if (c->workspace == g_current_workspace) {
        nex_client_focus(c);
        nex_client_raise(c);
    }

    nex_ewmh_set_client_list();
    xcb_flush(g_conn);
}

static void handle_configure_request(xcb_configure_request_event_t *ev)
{
    xcb_window_t window = ev->window;
    nex_client_t *c = nex_client_find(window);

    if (!c) {
        uint32_t mask = ev->value_mask;
        uint32_t values[7];
        int i = 0;
        if (mask & XCB_CONFIG_WINDOW_X) values[i++] = ev->x;
        if (mask & XCB_CONFIG_WINDOW_Y) values[i++] = ev->y;
        if (mask & XCB_CONFIG_WINDOW_WIDTH) values[i++] = ev->width;
        if (mask & XCB_CONFIG_WINDOW_HEIGHT) values[i++] = ev->height;
        if (mask & XCB_CONFIG_WINDOW_BORDER_WIDTH) values[i++] = ev->border_width;
        if (mask & XCB_CONFIG_WINDOW_SIBLING) values[i++] = ev->sibling;
        if (mask & XCB_CONFIG_WINDOW_STACK_MODE) values[i++] = ev->stack_mode;
        xcb_configure_window(g_conn, window, mask, values);
        return;
    }

    if (c->flags & NEX_CLIENT_FULLSCREEN) return;

    if (c->flags & NEX_CLIENT_FLOATING) {
        int x = c->x;
        int y = c->y;
        int w = c->width;
        int h = c->height;

        if (ev->value_mask & XCB_CONFIG_WINDOW_X) x = ev->x;
        if (ev->value_mask & XCB_CONFIG_WINDOW_Y) y = ev->y;
        if (ev->value_mask & XCB_CONFIG_WINDOW_WIDTH)
            w = ev->width + 2 * (int)g_config.border_width;
        if (ev->value_mask & XCB_CONFIG_WINDOW_HEIGHT)
            h = ev->height + NEX_TITLEBAR_H + (int)g_config.border_width;

        if (ev->value_mask & (XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT))
            nex_client_apply_size_hints(c, &w, &h);

        nex_client_move(c, x, y);
        nex_client_resize(c, w, h);
        if (ev->value_mask & XCB_CONFIG_WINDOW_STACK_MODE)
            nex_client_raise(c);
    } else {
        /* The WM owns the frame geometry. Reply with the client's actual
         * geometry so applications do not fight the decoration frame. */
        xcb_configure_notify_event_t notify = {
            .response_type = XCB_CONFIGURE_NOTIFY,
            .event = window,
            .window = window,
            .above_sibling = XCB_NONE,
            .x = 0,
            .y = NEX_TITLEBAR_H,
            .width = (uint16_t)(c->width > 2 * (int)g_config.border_width
                                 ? c->width - 2 * (int)g_config.border_width : 1),
            .height = (uint16_t)(c->height > NEX_TITLEBAR_H + (int)g_config.border_width
                                  ? c->height - NEX_TITLEBAR_H - (int)g_config.border_width : 1),
            .border_width = 0,
            .override_redirect = 0
        };
        xcb_send_event(g_conn, 0, window, XCB_EVENT_MASK_STRUCTURE_NOTIFY,
                       (char *)&notify);
    }
}

static void handle_destroy_notify(xcb_destroy_notify_event_t *ev)
{
    xcb_window_t window = ev->window;
    nex_client_t *c = nex_client_find(window);
    if (!c) {
        c = nex_client_find_frame(window);
        if (c) return;
    }
    if (c) {
        nex_workspace_remove_client(c->workspace, c);
        nex_client_destroy(c);
        nex_ewmh_set_client_list();
        nex_focus_next();
    }
}

static void handle_unmap_notify(xcb_unmap_notify_event_t *ev)
{
    xcb_window_t window = ev->window;
    nex_client_t *c = nex_client_find(window);
    if (!c) {
        c = nex_client_find_frame(window);
        if (c) return;
    }
    if (c) {
        xcb_get_window_attributes_cookie_t cookie = xcb_get_window_attributes(g_conn, window);
        xcb_get_window_attributes_reply_t *reply = xcb_get_window_attributes_reply(g_conn, cookie, NULL);
        if (reply) {
            if (reply->map_state == XCB_MAP_STATE_UNMAPPED && !(ev->response_type & 0x80)) {
                nex_workspace_remove_client(c->workspace, c);
                nex_client_destroy(c);
                nex_ewmh_set_client_list();
                nex_focus_next();
            }
            free(reply);
        }
    }
}

static void handle_property_notify(xcb_property_notify_event_t *ev)
{
    xcb_window_t window = ev->window;
    xcb_atom_t atom = ev->atom;
    nex_client_t *c = nex_client_find(window);
    if (!c) return;

    if (atom == g_atoms.net_wm_name || atom == XCB_ATOM_WM_NAME) {
        xcb_icccm_get_text_property_reply_t reply;
        xcb_get_property_cookie_t cookie = xcb_icccm_get_wm_name(g_conn, window);
        if (xcb_icccm_get_wm_name_reply(g_conn, cookie, &reply, NULL)) {
            strncpy(c->title, reply.name ? reply.name : "", sizeof(c->title) - 1);
            c->title[sizeof(c->title) - 1] = '\0';
            xcb_icccm_get_text_property_reply_wipe(&reply);
            nex_client_redraw_titlebar(c);
        }
    } else if (atom == XCB_ATOM_WM_CLASS) {
        xcb_icccm_get_wm_class_reply_t wm_class;
        xcb_get_property_cookie_t cookie = xcb_icccm_get_wm_class(g_conn, window);
        if (xcb_icccm_get_wm_class_reply(g_conn, cookie, &wm_class, NULL)) {
            strncpy(c->class, wm_class.class_name ? wm_class.class_name : "", sizeof(c->class) - 1);
            c->class[sizeof(c->class) - 1] = '\0';
            strncpy(c->instance, wm_class.instance_name ? wm_class.instance_name : "", sizeof(c->instance) - 1);
            c->instance[sizeof(c->instance) - 1] = '\0';
            xcb_icccm_get_wm_class_reply_wipe(&wm_class);
        }
    }
}

static void handle_client_message(xcb_client_message_event_t *ev)
{
    xcb_window_t window = ev->window;
    xcb_atom_t type = ev->type;
    nex_client_t *c = nex_client_find(window);
    if (!c) return;

    if (type == g_atoms.net_wm_state) {
        xcb_atom_t action = ev->data.data32[0];
        xcb_atom_t prop1 = ev->data.data32[1];
        xcb_atom_t prop2 = ev->data.data32[2];

        if (prop1 == g_atoms.net_wm_state_fullscreen || prop2 == g_atoms.net_wm_state_fullscreen) {
            if (action == 1) {
                c->flags |= NEX_CLIENT_FULLSCREEN;
                nex_monitor_t *m = nex_monitor_current();
                c->old_x = c->x; c->old_y = c->y;
                c->old_width = c->width; c->old_height = c->height;
                nex_client_move(c, m->x, m->y);
                nex_client_resize(c, m->width, m->height);
                uint32_t bw = 0;
                xcb_configure_window(g_conn, c->window, XCB_CONFIG_WINDOW_BORDER_WIDTH, &bw);
            } else if (action == 0) {
                c->flags &= ~NEX_CLIENT_FULLSCREEN;
                nex_client_move(c, c->old_x, c->old_y);
                nex_client_resize(c, c->old_width, c->old_height);
                uint32_t bw = g_config.border_width;
                xcb_configure_window(g_conn, c->window, XCB_CONFIG_WINDOW_BORDER_WIDTH, &bw);
            }
        }
    } else if (type == g_atoms.wm_protocols) {
        if (ev->data.data32[0] == g_atoms.wm_take_focus) {
            xcb_get_window_attributes_cookie_t ac =
                xcb_get_window_attributes(g_conn, window);
            xcb_get_window_attributes_reply_t *ar =
                xcb_get_window_attributes_reply(g_conn, ac, NULL);
            if (ar) {
                if (ar->map_state == XCB_MAP_STATE_VIEWABLE) {
                    xcb_set_input_focus(g_conn, XCB_INPUT_FOCUS_POINTER_ROOT,
                                        window, XCB_CURRENT_TIME);
                }
                free(ar);
            }
        }
    }
}

static void handle_enter_notify(xcb_enter_notify_event_t *ev)
{
    if (ev->mode != XCB_NOTIFY_MODE_NORMAL || ev->detail == XCB_NOTIFY_DETAIL_INFERIOR) return;
    nex_focus_follow_mouse(ev->event);
}

static nex_client_t *g_drag_client = NULL;
static int g_drag_start_x = 0;
static int g_drag_start_y = 0;
static int g_drag_win_x = 0;
static int g_drag_win_y = 0;
static int g_drag_win_w = 0;
static int g_drag_win_h = 0;
static int g_drag_mode = 0; /* 0 = none, 1 = move, 2 = resize */
static int g_resize_edge = NEX_RESIZE_NONE;

static void begin_resize(nex_client_t *c, xcb_button_press_event_t *ev, int edge)
{
    if (!c || edge == NEX_RESIZE_NONE) return;
    if (c->flags & (NEX_CLIENT_FULLSCREEN | NEX_CLIENT_MAXIMIZED | NEX_CLIENT_FIXED)) return;

    if (!(c->flags & NEX_CLIENT_FLOATING))
        c->flags |= NEX_CLIENT_FLOATING;

    g_drag_client = c;
    g_drag_start_x = ev->root_x;
    g_drag_start_y = ev->root_y;
    g_drag_win_x = c->x;
    g_drag_win_y = c->y;
    g_drag_win_w = c->width;
    g_drag_win_h = c->height;
    g_resize_edge = edge;
    g_drag_mode = 2;

    xcb_grab_pointer(g_conn, 0, g_screen->root,
                     XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_POINTER_MOTION,
                     XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
                     XCB_NONE, XCB_NONE, XCB_CURRENT_TIME);
}

static void handle_button_press(xcb_button_press_event_t *ev)
{
    nex_client_t *c = nex_client_find(ev->event);
    if (!c) c = nex_client_find(ev->child);

    nex_client_t *frame_client = nex_client_find_frame(ev->event);
    if (!frame_client && ev->child != XCB_NONE)
        frame_client = nex_client_find_frame(ev->child);
    if (frame_client) c = frame_client;

    if (c) {
        nex_client_focus(c);
        nex_client_raise(c);

        /* Server-side frame resize: 4 edges + 4 corners. */
        if (frame_client && ev->detail == XCB_BUTTON_INDEX_1 &&
            !(c->flags & (NEX_CLIENT_FULLSCREEN | NEX_CLIENT_MAXIMIZED | NEX_CLIENT_FIXED))) {
            int edge = NEX_RESIZE_NONE;
            int fx = ev->event_x - c->x;
            int fy = ev->event_y - c->y;
            if (nex_client_frame_resize_hit(c, fx, fy, &edge)) {
                begin_resize(c, ev, edge);
                xcb_allow_events(g_conn, XCB_ALLOW_ASYNC_POINTER, ev->time);
                xcb_flush(g_conn);
                return;
            }
        }

        /* Server-side titlebar controls. */
        if (frame_client && ev->detail == XCB_BUTTON_INDEX_1) {
            int button = NEX_BTN_NONE;
            int fx = ev->event_x - c->x;
            int fy = ev->event_y - c->y;
            if (nex_client_titlebar_hit(c, fx, fy, &button)) {
                if (button == NEX_BTN_MINIMIZE) {
                    nex_client_minimize(c);
                } else if (button == NEX_BTN_MAXIMIZE) {
                    nex_client_toggle_maximize(c);
                } else if (button == NEX_BTN_CLOSE) {
                    nex_client_kill(c);
                } else {
                    g_drag_client = c;
                    g_drag_start_x = ev->root_x;
                    g_drag_start_y = ev->root_y;
                    g_drag_win_x = c->x;
                    g_drag_win_y = c->y;
                    g_drag_mode = 1;
                    xcb_grab_pointer(g_conn, 0, g_screen->root,
                                     XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_POINTER_MOTION,
                                     XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
                                     XCB_NONE, XCB_NONE, XCB_CURRENT_TIME);
                }
                xcb_allow_events(g_conn, XCB_ALLOW_ASYNC_POINTER, ev->time);
                xcb_flush(g_conn);
                return;
            }
        }

        uint16_t mod = ev->state & (XCB_MOD_MASK_SHIFT | XCB_MOD_MASK_CONTROL |
                                    XCB_MOD_MASK_1 | XCB_MOD_MASK_2 |
                                    XCB_MOD_MASK_3 | XCB_MOD_MASK_4 | XCB_MOD_MASK_5);
        mod &= ~(XCB_MOD_MASK_2 | XCB_MOD_MASK_LOCK);

        if (mod == g_config.modkey) {
            if (!(c->flags & NEX_CLIENT_FLOATING))
                c->flags |= NEX_CLIENT_FLOATING;

            g_drag_client = c;
            g_drag_start_x = ev->root_x;
            g_drag_start_y = ev->root_y;
            g_drag_win_x = c->x;
            g_drag_win_y = c->y;
            g_drag_win_w = c->width;
            g_drag_win_h = c->height;

            if (ev->detail == XCB_BUTTON_INDEX_1)
                g_drag_mode = 1;
            else if (ev->detail == XCB_BUTTON_INDEX_3)
                g_drag_mode = 2;

            if (g_drag_mode != 0) {
                xcb_grab_pointer(g_conn, 0, g_screen->root,
                                 XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_POINTER_MOTION,
                                 XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
                                 XCB_NONE, XCB_NONE, XCB_CURRENT_TIME);
                xcb_allow_events(g_conn, XCB_ALLOW_ASYNC_POINTER, ev->time);
                xcb_flush(g_conn);
                return;
            }
        }
    }

    xcb_allow_events(g_conn, XCB_ALLOW_REPLAY_POINTER, ev->time);
}

static void handle_motion_notify(xcb_motion_notify_event_t *ev)
{
    if (g_drag_mode == 0 || !g_drag_client) return;

    int dx = ev->root_x - g_drag_start_x;
    int dy = ev->root_y - g_drag_start_y;

    if (g_drag_mode == 1) {
        nex_client_move(g_drag_client, g_drag_win_x + dx, g_drag_win_y + dy);
    } else if (g_drag_mode == 2) {
        int new_x = g_drag_win_x;
        int new_y = g_drag_win_y;
        int new_w = g_drag_win_w;
        int new_h = g_drag_win_h;

        /* Resize from the selected edge/corner. The opposite edge stays fixed. */
        if (g_resize_edge & NEX_RESIZE_W) {
            new_x = g_drag_win_x + dx;
            new_w = g_drag_win_w - dx;
        }
        if (g_resize_edge & NEX_RESIZE_E) {
            new_w = g_drag_win_w + dx;
        }
        if (g_resize_edge & NEX_RESIZE_N) {
            new_y = g_drag_win_y + dy;
            new_h = g_drag_win_h - dy;
        }
        if (g_resize_edge & NEX_RESIZE_S) {
            new_h = g_drag_win_h + dy;
        }

        /* Keep the frame large enough for the titlebar and client content. */
        if (new_w < 50) {
            if (g_resize_edge & NEX_RESIZE_W)
                new_x = g_drag_win_x + g_drag_win_w - 50;
            new_w = 50;
        }
        if (new_h < NEX_TITLEBAR_H + 30) {
            if (g_resize_edge & NEX_RESIZE_N)
                new_y = g_drag_win_y + g_drag_win_h - (NEX_TITLEBAR_H + 30);
            new_h = NEX_TITLEBAR_H + 30;
        }

        /* Apply the application's WM_NORMAL_HINTS to the frame size. */
        nex_client_apply_size_hints(g_drag_client, &new_w, &new_h);

        /* Re-anchor the frame after hint rounding/clamping. */
        if (g_resize_edge & NEX_RESIZE_W)
            new_x = g_drag_win_x + g_drag_win_w - new_w;
        if (g_resize_edge & NEX_RESIZE_N)
            new_y = g_drag_win_y + g_drag_win_h - new_h;

        nex_client_set_geometry(g_drag_client, new_x, new_y, new_w, new_h);
    }
    xcb_flush(g_conn);
}

static void handle_button_release(xcb_button_release_event_t *ev)
{
    (void)ev;
    if (g_drag_mode != 0) {
        g_drag_mode = 0;
        g_resize_edge = NEX_RESIZE_NONE;
        g_drag_client = NULL;
        xcb_ungrab_pointer(g_conn, XCB_CURRENT_TIME);
        xcb_flush(g_conn);
    }
}

static void handle_expose(xcb_expose_event_t *ev)
{
    nex_client_t *c = nex_client_find_frame(ev->window);
    if (c) nex_client_redraw_titlebar(c);
}

static void handle_key_press(xcb_key_press_event_t *ev)
{
    nex_keybind_handle(ev);
}

void nex_events_handle(xcb_generic_event_t *ev)
{
    if (!ev) return;
    uint8_t type = ev->response_type & ~0x80;

    switch (type) {
        case XCB_MAP_REQUEST:        handle_map_request((xcb_map_request_event_t *)ev); break;
        case XCB_CONFIGURE_REQUEST:  handle_configure_request((xcb_configure_request_event_t *)ev); break;
        case XCB_DESTROY_NOTIFY:     handle_destroy_notify((xcb_destroy_notify_event_t *)ev); break;
        case XCB_UNMAP_NOTIFY:       handle_unmap_notify((xcb_unmap_notify_event_t *)ev); break;
        case XCB_PROPERTY_NOTIFY:    handle_property_notify((xcb_property_notify_event_t *)ev); break;
        case XCB_CLIENT_MESSAGE:     handle_client_message((xcb_client_message_event_t *)ev); break;
        case XCB_ENTER_NOTIFY:       handle_enter_notify((xcb_enter_notify_event_t *)ev); break;
        case XCB_EXPOSE:             handle_expose((xcb_expose_event_t *)ev); break;
        case XCB_BUTTON_PRESS:       handle_button_press((xcb_button_press_event_t *)ev); break;
        case XCB_MOTION_NOTIFY:      handle_motion_notify((xcb_motion_notify_event_t *)ev); break;
        case XCB_BUTTON_RELEASE:     handle_button_release((xcb_button_release_event_t *)ev); break;
        case XCB_KEY_PRESS:          handle_key_press((xcb_key_press_event_t *)ev); break;
        case XCB_CREATE_NOTIFY:      NEX_DEBUG("CreateNotify: 0x%x", ((xcb_create_notify_event_t *)ev)->window); break;
        case XCB_REPARENT_NOTIFY:    NEX_DEBUG("ReparentNotify: 0x%x", ((xcb_reparent_notify_event_t *)ev)->window); break;
        case XCB_MAP_NOTIFY:         NEX_DEBUG("MapNotify: 0x%x", ((xcb_map_notify_event_t *)ev)->window); break;
        case XCB_CONFIGURE_NOTIFY:   NEX_DEBUG("ConfigureNotify: 0x%x", ((xcb_configure_notify_event_t *)ev)->window); break;
        default:                     NEX_DEBUG("Unhandled event type: %d", type); break;
    }
}
