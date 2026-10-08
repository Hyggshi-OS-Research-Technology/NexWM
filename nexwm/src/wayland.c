/* wayland.c — NexWM's Wayland compositor.
 *
 * This is the Wayland half of the same `nexwm` program as x11.c. It uses wlroots for the device, protocol and scene
 * plumbing; NexWM owns the policy: focus, workspaces, key bindings, window placement, and the HDE session it starts.
 * The X11 build remains independent of wlroots.
 */
#define _POSIX_C_SOURCE 200809L

#include "nexwm.h"

#include <stdio.h>

#ifndef NEXWM_HAVE_WLROOTS

int nexwm_wayland_run(const char *config_path, int replace, const char *session)
{
    (void)config_path;
    (void)replace;
    (void)session;
    fprintf(stderr,
            "nexwm: this build has no Wayland compositor: it was made without wlroots.\n"
            "       Install wlroots 0.17+ development files (Debian: libwlroots-0.18-dev/libwlroots-0.19-dev;\n"
            "       Ubuntu: libwlroots-dev; Fedora: wlroots-devel), build again, or use nexwm --x11.\n");
    return 4;
}

#else /* NEXWM_HAVE_WLROOTS */

#ifndef NEXWM_WLROOTS_MINOR
#define NEXWM_WLROOTS_MINOR 19
#endif

#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/edges.h>
#include <wlr/util/log.h>

#define NEXWM_LAYER_COUNT 4
#define NEXWM_CHILD_POLL_MS 100
#define NEXWM_DEFAULT_WIDTH 800
#define NEXWM_DEFAULT_HEIGHT 600
/* Wayland keyboard keycodes are evdev codes; xkbcommon keycodes include the 8-code offset. */
#define NEXWM_XKB_KEYCODE_OFFSET 8u

#define NEXWM_ADD_LISTENER(signal, listener, callback) do { \
    (listener).notify = (callback); \
    wl_signal_add((signal), &(listener)); \
} while (0)

struct NexwmServer;
struct NexwmOutput;
struct NexwmView;
struct NexwmKeyboard;
struct NexwmLayerSurface;
struct NexwmPopup;

typedef struct NexwmServer NexwmServer;
typedef struct NexwmOutput NexwmOutput;
typedef struct NexwmView NexwmView;
typedef struct NexwmKeyboard NexwmKeyboard;
typedef struct NexwmLayerSurface NexwmLayerSurface;
typedef struct NexwmPopup NexwmPopup;

enum NexwmCursorMode {
    NEXWM_CURSOR_PASSTHROUGH,
    NEXWM_CURSOR_MOVE,
    NEXWM_CURSOR_RESIZE,
};

struct NexwmOutput {
    NexwmServer *server;
    struct wl_list link;
    struct wlr_output *wlr;
    struct wlr_output_layout_output *layout_output;
    struct wlr_scene_output *scene_output;
    struct wlr_scene_tree *layer_trees[NEXWM_LAYER_COUNT];
    struct wlr_box full_area;
    struct wlr_box usable_area;
    struct wlr_box layout_box;
    struct wl_listener frame;
    struct wl_listener destroy;
};

struct NexwmView {
    NexwmServer *server;
    struct wl_list link;
    struct wlr_xdg_toplevel *xdg;
    struct wlr_scene_tree *scene_tree;
    struct wlr_foreign_toplevel_handle_v1 *foreign;
    NexwmOutput *output;
    int workspace;
    int x, y, width, height;
    int restore_x, restore_y, restore_width, restore_height;
    bool restore_valid;
    bool mapped;
    bool minimized;
    bool maximized;
    bool fullscreen;
    struct wl_listener map;
    struct wl_listener unmap;
    struct wl_listener destroy;
    struct wl_listener commit;
    struct wl_listener request_maximize;
    struct wl_listener request_fullscreen;
    struct wl_listener request_minimize;
    struct wl_listener request_move;
    struct wl_listener request_resize;
    struct wl_listener set_title;
    struct wl_listener set_app_id;
    struct wl_listener foreign_request_activate;
    struct wl_listener foreign_request_maximize;
    struct wl_listener foreign_request_minimize;
    struct wl_listener foreign_request_fullscreen;
    struct wl_listener foreign_request_close;
};

struct NexwmKeyboard {
    NexwmServer *server;
    struct wl_list link;
    struct wlr_input_device *device;
    struct wlr_keyboard *wlr;
    struct wl_listener key;
    struct wl_listener modifiers;
    struct wl_listener destroy;
};

struct NexwmLayerSurface {
    NexwmServer *server;
    struct wl_list link;
    struct wlr_layer_surface_v1 *wlr;
    struct wlr_scene_layer_surface_v1 *scene;
    NexwmOutput *output;
    struct wl_listener map;
    struct wl_listener unmap;
    struct wl_listener destroy;
    struct wl_listener new_popup;
};

struct NexwmPopup {
    NexwmServer *server;
    struct wl_list link;
    struct wlr_xdg_popup *wlr;
    struct wlr_scene_tree *scene_tree;
    struct wl_listener new_popup;
    struct wl_listener destroy;
};

struct NexwmServer {
    struct wl_display *display;
    struct wl_event_loop *event_loop;
    struct wlr_backend *backend;
    struct wlr_session *session;
    struct wlr_renderer *renderer;
    struct wlr_allocator *allocator;
    struct wlr_scene *scene;
    struct wlr_output_layout *output_layout;
    struct wlr_scene_output_layout *scene_output_layout;
    struct wlr_xdg_shell *xdg_shell;
    struct wlr_layer_shell_v1 *layer_shell;
    struct wlr_foreign_toplevel_manager_v1 *foreign_manager;
    struct wlr_seat *seat;
    struct wlr_cursor *cursor;
    struct wlr_xcursor_manager *cursor_manager;

    struct wlr_scene_tree *scene_layers[NEXWM_LAYER_COUNT];
    struct wlr_scene_tree *views_tree;
    struct wl_list outputs;
    struct wl_list views;
    struct wl_list keyboards;
    struct wl_list layer_surfaces;
    struct wl_list popups;
    NexwmView *focused;
    NexwmLayerSurface *keyboard_layer;

    HdeNexwmConfig config;
    int workspace;
    bool pointer_present;
    bool running;
    int exit_code;
    int session_status;
    pid_t session_pid;
    struct wl_event_source *child_timer;
    struct wl_event_source *sigint_source;
    struct wl_event_source *sigterm_source;

    struct wl_listener new_output;
    struct wl_listener new_input;
    struct wl_listener new_toplevel;
    struct wl_listener new_popup;
    struct wl_listener new_layer_surface;
    struct wl_listener cursor_motion;
    struct wl_listener cursor_motion_absolute;
    struct wl_listener cursor_button;
    struct wl_listener request_set_cursor;
    struct wl_listener request_set_selection;

    enum NexwmCursorMode cursor_mode;
    NexwmView *grabbed_view;
    double grab_x, grab_y;
    int grab_start_x, grab_start_y, grab_start_width, grab_start_height;
    uint32_t resize_edges;
};

static void view_focus(NexwmServer *server, NexwmView *view);
static void view_sync_foreign_state(NexwmView *view);
static void arrange_layers(NexwmOutput *output);
static void server_update_seat_capabilities(NexwmServer *server);
static void create_scene_popup(NexwmServer *server, struct wlr_xdg_popup *popup,
                               struct wlr_scene_tree *parent_tree);

static void log_message(const char *level, const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    fprintf(stderr, "nexwm: %s: ", level);
    vfprintf(stderr, format, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static NexwmOutput *first_output(NexwmServer *server)
{
    if (wl_list_empty(&server->outputs)) return NULL;
    NexwmOutput *output;
    return wl_container_of(server->outputs.next, output, link);
}

static struct wlr_box output_layout_box(NexwmOutput *output)
{
    struct wlr_box box = { .x = 0, .y = 0,
                           .width = output->full_area.width,
                           .height = output->full_area.height };
    wlr_output_layout_get_box(output->server->output_layout, output->wlr, &box);
    return box;
}

static struct wlr_box server_workarea(NexwmServer *server)
{
    NexwmOutput *output = first_output(server);
    if (!output) return (struct wlr_box){ .x = 0, .y = 0,
                                           .width = 1280, .height = 720 };
    struct wlr_box box = output->usable_area;
    box.x += output->layout_box.x;
    box.y += output->layout_box.y;
    if (box.width < 1) box.width = output->full_area.width;
    if (box.height < 1) box.height = output->full_area.height;
    return box;
}

static void output_refresh_geometry(NexwmOutput *output)
{
    wlr_output_effective_resolution(output->wlr, &output->full_area.width, &output->full_area.height);
    output->full_area.x = 0;
    output->full_area.y = 0;
    output->layout_box = output_layout_box(output);
    output->usable_area = output->full_area;
    for (int i = 0; i < NEXWM_LAYER_COUNT; i++) {
        if (output->layer_trees[i])
            wlr_scene_node_set_position(&output->layer_trees[i]->node,
                                        output->layout_box.x, output->layout_box.y);
    }
}

static void arrange_layers(NexwmOutput *output)
{
    if (!output) return;
    output_refresh_geometry(output);
    output->usable_area = output->full_area;

    /* The protocol's topmost layers reserve their exclusive zones first. Within each layer, reserve the positive
     * zones before placing the non-exclusive (-1 or 0) surfaces. */
    for (int layer = ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY;
         layer >= ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND; layer--) {
        for (int exclusive = 1; exclusive >= 0; exclusive--) {
            NexwmLayerSurface *surface;
            wl_list_for_each(surface, &output->server->layer_surfaces, link) {
                if (surface->output != output || !surface->scene) continue;
                if ((int)surface->wlr->current.layer != layer) continue;
                if ((surface->wlr->current.exclusive_zone > 0) != (exclusive != 0)) continue;
                wlr_scene_layer_surface_v1_configure(surface->scene,
                                                      &output->full_area,
                                                      &output->usable_area);
            }
        }
    }
}

static void update_output_layout(NexwmOutput *output)
{
    output_refresh_geometry(output);
    arrange_layers(output);
    if (output->scene_output) wlr_output_schedule_frame(output->wlr);
}

static void output_frame(struct wl_listener *listener, void *data)
{
    (void)data;
    NexwmOutput *output = wl_container_of(listener, output, frame);
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (!wlr_scene_output_commit(output->scene_output, NULL)) return;
    wlr_scene_output_send_frame_done(output->scene_output, &now);
}

static void output_destroy(struct wl_listener *listener, void *data)
{
    (void)data;
    NexwmOutput *output = wl_container_of(listener, output, destroy);
    NexwmServer *server = output->server;
    wl_list_remove(&output->frame.link);
    wl_list_remove(&output->destroy.link);
    wl_list_remove(&output->link);
    wlr_output_layout_remove(server->output_layout, output->wlr);

    NexwmLayerSurface *layer;
    wl_list_for_each(layer, &server->layer_surfaces, link) {
        if (layer->output == output) {
            layer->output = NULL;
            layer->wlr->output = NULL;
            if (layer->scene) {
                wlr_scene_node_set_enabled(&layer->scene->tree->node, false);
                layer->scene = NULL;
            }
        }
    }
    NexwmView *view;
    wl_list_for_each(view, &server->views, link) {
        if (view->output == output) {
            if (view->foreign) wlr_foreign_toplevel_handle_v1_output_leave(view->foreign, output->wlr);
            view->output = NULL;
        }
    }
    for (int i = 0; i < NEXWM_LAYER_COUNT; i++) {
        if (output->layer_trees[i]) wlr_scene_node_destroy(&output->layer_trees[i]->node);
    }
    /* wlroots destroys the scene output automatically with its wlr_output. */
    free(output);
    NexwmOutput *remaining;
    wl_list_for_each(remaining, &server->outputs, link) update_output_layout(remaining);
}

static bool output_enable(NexwmServer *server, struct wlr_output *wlr)
{
    if (!wlr_output_init_render(wlr, server->allocator, server->renderer)) {
        log_message("error", "could not initialize rendering on output %s", wlr->name);
        return false;
    }
    struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr);
#if NEXWM_WLROOTS_MINOR >= 18
    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, true);
    if (mode) wlr_output_state_set_mode(&state, mode);
    bool ok = wlr_output_commit_state(wlr, &state);
    wlr_output_state_finish(&state);
#else
    wlr_output_enable(wlr, true);
    if (mode) wlr_output_set_mode(wlr, mode);
    bool ok = wlr_output_commit(wlr);
#endif
    if (!ok) log_message("error", "could not enable output %s", wlr->name);
    return ok;
}

static void server_new_output(struct wl_listener *listener, void *data)
{
    NexwmServer *server = wl_container_of(listener, server, new_output);
    struct wlr_output *wlr = data;
    if (!output_enable(server, wlr)) return;

    NexwmOutput *output = calloc(1, sizeof *output);
    if (!output) {
        log_message("error", "out of memory while adding output %s", wlr->name);
        return;
    }
    output->server = server;
    output->wlr = wlr;
    output->layout_output = wlr_output_layout_add_auto(server->output_layout, wlr);
    if (!output->layout_output) {
        log_message("error", "could not place output %s in the output layout", wlr->name);
        free(output);
        return;
    }

    for (int i = 0; i < NEXWM_LAYER_COUNT; i++) {
        output->layer_trees[i] = wlr_scene_tree_create(server->scene_layers[i]);
        if (!output->layer_trees[i]) {
            log_message("error", "could not create the layer scene for output %s", wlr->name);
            for (int j = 0; j < i; j++)
                if (output->layer_trees[j]) wlr_scene_node_destroy(&output->layer_trees[j]->node);
            wlr_output_layout_remove(server->output_layout, wlr);
            free(output);
            return;
        }
    }
    output->scene_output = wlr_scene_output_create(server->scene, wlr);
    if (!output->scene_output) {
        log_message("error", "could not create the scene output for %s", wlr->name);
        for (int i = 0; i < NEXWM_LAYER_COUNT; i++)
            if (output->layer_trees[i]) wlr_scene_node_destroy(&output->layer_trees[i]->node);
        wlr_output_layout_remove(server->output_layout, wlr);
        free(output);
        return;
    }
    wlr_scene_output_layout_add_output(server->scene_output_layout,
                                      output->layout_output, output->scene_output);
    NEXWM_ADD_LISTENER(&wlr->events.frame, output->frame, output_frame);
    NEXWM_ADD_LISTENER(&wlr->events.destroy, output->destroy, output_destroy);
    wl_list_insert(&server->outputs, &output->link);
    output_refresh_geometry(output);
    wlr_xcursor_manager_load(server->cursor_manager, wlr->scale);
    wlr_cursor_set_xcursor(server->cursor, server->cursor_manager, "left_ptr");
    arrange_layers(output);
    wlr_output_schedule_frame(wlr);
    log_message("info", "output %s: %dx%d at %d,%d", wlr->name,
                output->full_area.width, output->full_area.height,
                output->layout_box.x, output->layout_box.y);
}

static void server_update_seat_capabilities(NexwmServer *server)
{
    uint32_t capabilities = 0;
    if (server->pointer_present) capabilities |= WL_SEAT_CAPABILITY_POINTER;
    if (!wl_list_empty(&server->keyboards)) capabilities |= WL_SEAT_CAPABILITY_KEYBOARD;
    wlr_seat_set_capabilities(server->seat, capabilities);
}

static void view_sync_foreign_state(NexwmView *view)
{
    if (!view->foreign) return;
    wlr_foreign_toplevel_handle_v1_set_activated(view->foreign,
        view->server->focused == view && view->mapped && !view->minimized &&
        view->workspace == view->server->workspace);
    wlr_foreign_toplevel_handle_v1_set_maximized(view->foreign, view->maximized);
    wlr_foreign_toplevel_handle_v1_set_minimized(view->foreign, view->minimized);
    wlr_foreign_toplevel_handle_v1_set_fullscreen(view->foreign, view->fullscreen);
}

static void view_raise(NexwmView *view)
{
    if (view && view->scene_tree)
        wlr_scene_node_raise_to_top(&view->scene_tree->node);
}

static void view_focus(NexwmServer *server, NexwmView *view)
{
    if (view && (!view->mapped || view->workspace != server->workspace)) return;
    NexwmView *old = server->focused;
    if (old == view) {
        if (view && view->minimized) {
            view->minimized = false;
            wlr_scene_node_set_enabled(&view->scene_tree->node, true);
        }
        if (view) view_raise(view);
        view_sync_foreign_state(view);
    } else {
        server->focused = view;
        if (old) view_sync_foreign_state(old);
        if (view) {
            if (view->minimized) {
                view->minimized = false;
                wlr_scene_node_set_enabled(&view->scene_tree->node, true);
            }
            view_raise(view);
            struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(server->seat);
            if (keyboard) {
                wlr_seat_keyboard_notify_enter(server->seat, view->xdg->base->surface,
                                                keyboard->keycodes, keyboard->num_keycodes,
                                                &keyboard->modifiers);
            }
            view_sync_foreign_state(view);
        } else {
            wlr_seat_keyboard_notify_clear_focus(server->seat);
        }
    }
}

static NexwmView *first_visible_view(NexwmServer *server)
{
    NexwmView *view;
    wl_list_for_each(view, &server->views, link) {
        if (view->mapped && !view->minimized && view->workspace == server->workspace) return view;
    }
    return NULL;
}

static void focus_workspace_view(NexwmServer *server)
{
    view_focus(server, first_visible_view(server));
}

static void set_workspace(NexwmServer *server, int workspace)
{
    if (workspace < 0) workspace = 0;
    if (workspace >= server->config.desktops) workspace = server->config.desktops - 1;
    if (workspace == server->workspace) return;
    server->workspace = workspace;
    NexwmView *view;
    wl_list_for_each(view, &server->views, link) {
        bool enabled = view->mapped && !view->minimized && view->workspace == workspace;
        wlr_scene_node_set_enabled(&view->scene_tree->node, enabled);
        view_sync_foreign_state(view);
    }
    server->focused = NULL;
    wlr_seat_keyboard_notify_clear_focus(server->seat);
    focus_workspace_view(server);
    log_message("info", "workspace %d", workspace + 1);
}

static void view_save_restore(NexwmView *view)
{
    if (view->restore_valid) return;
    view->restore_x = view->x;
    view->restore_y = view->y;
    view->restore_width = view->width;
    view->restore_height = view->height;
    view->restore_valid = true;
}

static void view_set_position_and_size(NexwmView *view, int x, int y, int width, int height)
{
    if (width < 1) width = 1;
    if (height < 1) height = 1;
    view->x = x;
    view->y = y;
    view->width = width;
    view->height = height;
    wlr_scene_node_set_position(&view->scene_tree->node, x, y);
    wlr_xdg_toplevel_set_size(view->xdg, width, height);
}

static void view_restore(NexwmView *view)
{
    if (!view->restore_valid) {
        struct wlr_box area = server_workarea(view->server);
        view_set_position_and_size(view, area.x + 32, area.y + 32,
                                   NEXWM_DEFAULT_WIDTH, NEXWM_DEFAULT_HEIGHT);
    } else {
        view_set_position_and_size(view, view->restore_x, view->restore_y,
                                   view->restore_width, view->restore_height);
    }
    view->restore_valid = false;
}

static void view_set_maximized(NexwmView *view, bool maximized)
{
    if (!view) return;
    bool was_maximized = view->maximized;
    if (maximized && !view->maximized && !view->fullscreen) view_save_restore(view);
    view->maximized = maximized;
    if (maximized) {
        view->fullscreen = false;
        wlr_xdg_toplevel_set_fullscreen(view->xdg, false);
        struct wlr_box area = server_workarea(view->server);
        view_set_position_and_size(view, area.x, area.y, area.width, area.height);
    } else if (was_maximized && !view->fullscreen) {
        view_restore(view);
    }
    wlr_xdg_toplevel_set_maximized(view->xdg, maximized);
    if (view->foreign) wlr_foreign_toplevel_handle_v1_set_fullscreen(view->foreign, view->fullscreen);
    view_sync_foreign_state(view);
}

static void view_set_fullscreen(NexwmView *view, bool fullscreen)
{
    if (!view) return;
    bool was_fullscreen = view->fullscreen;
    if (fullscreen && !view->fullscreen && !view->maximized) view_save_restore(view);
    view->fullscreen = fullscreen;
    if (fullscreen) {
        view->maximized = false;
        wlr_xdg_toplevel_set_maximized(view->xdg, false);
        NexwmOutput *output = view->output ? view->output : first_output(view->server);
        if (output) {
            struct wlr_box area = output->full_area;
            area.x += output->layout_box.x;
            area.y += output->layout_box.y;
            view_set_position_and_size(view, area.x, area.y, area.width, area.height);
        }
    } else if (was_fullscreen && !view->maximized) {
        view_restore(view);
    }
    wlr_xdg_toplevel_set_fullscreen(view->xdg, fullscreen);
    if (view->foreign) wlr_foreign_toplevel_handle_v1_set_maximized(view->foreign, view->maximized);
    view_sync_foreign_state(view);
}

static void view_snap(NexwmView *view, int edge)
{
    struct wlr_box area = server_workarea(view->server);
    int half_w = area.width / 2;
    int half_h = area.height / 2;
    view->maximized = false;
    view->fullscreen = false;
    view->restore_valid = false;
    wlr_xdg_toplevel_set_maximized(view->xdg, false);
    wlr_xdg_toplevel_set_fullscreen(view->xdg, false);
    switch (edge) {
    case NEXWM_EDGE_LEFT:
        view_set_position_and_size(view, area.x, area.y, half_w, area.height);
        break;
    case NEXWM_EDGE_RIGHT:
        view_set_position_and_size(view, area.x + half_w, area.y, area.width - half_w, area.height);
        break;
    case NEXWM_EDGE_UP:
        view_set_position_and_size(view, area.x, area.y, area.width, half_h);
        break;
    case NEXWM_EDGE_DOWN:
        view_set_position_and_size(view, area.x, area.y + half_h, area.width, area.height - half_h);
        break;
    default:
        return;
    }
    view_sync_foreign_state(view);
}

static void view_minimize(NexwmView *view, bool minimized)
{
    if (!view || !view->mapped) return;
    view->minimized = minimized;
    wlr_scene_node_set_enabled(&view->scene_tree->node,
                               !minimized && view->workspace == view->server->workspace);
    view_sync_foreign_state(view);
    if (minimized && view->server->focused == view) {
        view->server->focused = NULL;
        focus_workspace_view(view->server);
    }
}

static NexwmView *next_view(NexwmServer *server, int direction)
{
    if (wl_list_empty(&server->views)) return NULL;
    if (!server->focused) return first_visible_view(server);
    struct wl_list *link = direction > 0 ? server->focused->link.next : server->focused->link.prev;
    for (size_t n = 0; n < 256; n++) {
        if (link == &server->views) link = direction > 0 ? link->next : link->prev;
        if (link == &server->views) return NULL;
        NexwmView *view = wl_container_of(link, view, link);
        if (view->mapped && !view->minimized && view->workspace == server->workspace) return view;
        link = direction > 0 ? link->next : link->prev;
    }
    return NULL;
}

static void spawn_command(NexwmServer *server, const char *command)
{
    if (!command || !*command) return;
    pid_t pid = fork();
    if (pid < 0) {
        log_message("error", "cannot start '%s': %s", command, strerror(errno));
        return;
    }
    if (pid == 0) {
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGCHLD, SIG_DFL);
        execl("/bin/sh", "sh", "-c", command, (char *)NULL);
        _exit(127);
    }
    log_message("info", "started %s (pid %ld)", command, (long)pid);
    (void)server;
}

static void run_action(NexwmServer *server, const HdeNexwmBinding *binding)
{
    NexwmView *view = server->focused;
    switch (binding->action) {
    case NEXWM_ACTION_SPAWN:
        spawn_command(server, binding->command);
        break;
    case NEXWM_ACTION_CLOSE:
        if (view) wlr_xdg_toplevel_send_close(view->xdg);
        break;
    case NEXWM_ACTION_KILL:
        if (view) {
            pid_t pid = 0;
            uid_t uid;
            gid_t gid;
            wl_client_get_credentials(view->xdg->base->client->client, &pid, &uid, &gid);
            if (pid > 0 && kill(pid, SIGKILL) != 0)
                log_message("error", "cannot kill window client %ld: %s", (long)pid, strerror(errno));
        }
        break;
    case NEXWM_ACTION_NEXT:
        view_focus(server, next_view(server, 1));
        break;
    case NEXWM_ACTION_PREV:
        view_focus(server, next_view(server, -1));
        break;
    case NEXWM_ACTION_WORKSPACE:
        set_workspace(server, binding->arg);
        break;
    case NEXWM_ACTION_MOVE_TO:
        if (view && binding->arg >= 0 && binding->arg < server->config.desktops) {
            view->workspace = binding->arg;
            wlr_scene_node_set_enabled(&view->scene_tree->node,
                                       !view->minimized && view->workspace == server->workspace);
            view_sync_foreign_state(view);
            if (view->workspace != server->workspace) {
                server->focused = NULL;
                wlr_seat_keyboard_notify_clear_focus(server->seat);
                focus_workspace_view(server);
            } else {
                view_focus(server, view);
            }
        }
        break;
    case NEXWM_ACTION_MAXIMIZE:
        if (view) view_set_maximized(view, !view->maximized);
        break;
    case NEXWM_ACTION_UNMAXIMIZE:
        if (view) {
            if (view->fullscreen) view_set_fullscreen(view, false);
            else view_set_maximized(view, false);
        }
        break;
    case NEXWM_ACTION_FULLSCREEN:
        if (view) view_set_fullscreen(view, !view->fullscreen);
        break;
    case NEXWM_ACTION_SNAP:
        if (view) view_snap(view, binding->arg);
        break;
    case NEXWM_ACTION_QUIT:
        log_message("info", "leaving the Wayland session on Super+Shift+Q (or the configured quit key)");
        wl_display_terminate(server->display);
        break;
    default:
        break;
    }
}

static unsigned wayland_modifiers(struct wlr_keyboard *keyboard)
{
    uint32_t state = wlr_keyboard_get_modifiers(keyboard);
    unsigned mods = 0;
    if (state & WLR_MODIFIER_SHIFT) mods |= NEXWM_MOD_SHIFT;
    if (state & WLR_MODIFIER_CTRL) mods |= NEXWM_MOD_CTRL;
    if (state & WLR_MODIFIER_ALT) mods |= NEXWM_MOD_ALT;
    if (state & WLR_MODIFIER_LOGO) mods |= NEXWM_MOD_SUPER;
    return mods;
}

static bool handle_binding(NexwmServer *server, struct wlr_keyboard *keyboard, uint32_t keycode)
{
    if (!keyboard->xkb_state) return false;
    xkb_keysym_t sym = xkb_state_key_get_one_sym(keyboard->xkb_state,
                                                   keycode + NEXWM_XKB_KEYCODE_OFFSET);
    if (sym >= XKB_KEY_A && sym <= XKB_KEY_Z) sym += XKB_KEY_a - XKB_KEY_A;
    unsigned modifiers = wayland_modifiers(keyboard);
    for (size_t i = 0; i < server->config.n_keys; i++) {
        HdeNexwmBinding *binding = &server->config.keys[i];
        if (binding->keysym != (unsigned)sym || binding->mods != modifiers) continue;
        log_message("key", "%s %s%s%s", binding->combo, nexwm_action_name(binding->action),
                    binding->command ? " " : "", binding->command ? binding->command : "");
        run_action(server, binding);
        return true;
    }
    return false;
}

static void keyboard_key(struct wl_listener *listener, void *data)
{
    NexwmKeyboard *keyboard = wl_container_of(listener, keyboard, key);
    struct wlr_keyboard_key_event *event = data;
    if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED &&
        handle_binding(keyboard->server, keyboard->wlr, event->keycode)) return;
    wlr_seat_set_keyboard(keyboard->server->seat, keyboard->wlr);
    wlr_seat_keyboard_notify_key(keyboard->server->seat, event->time_msec,
                                 event->keycode, event->state);
}

static void keyboard_modifiers(struct wl_listener *listener, void *data)
{
    (void)data;
    NexwmKeyboard *keyboard = wl_container_of(listener, keyboard, modifiers);
    wlr_seat_set_keyboard(keyboard->server->seat, keyboard->wlr);
    wlr_seat_keyboard_notify_modifiers(keyboard->server->seat, &keyboard->wlr->modifiers);
}

static void keyboard_destroy(struct wl_listener *listener, void *data)
{
    (void)data;
    NexwmKeyboard *keyboard = wl_container_of(listener, keyboard, destroy);
    NexwmServer *server = keyboard->server;
    if (wlr_seat_get_keyboard(server->seat) == keyboard->wlr)
        wlr_seat_set_keyboard(server->seat, NULL);
    wl_list_remove(&keyboard->key.link);
    wl_list_remove(&keyboard->modifiers.link);
    wl_list_remove(&keyboard->destroy.link);
    wl_list_remove(&keyboard->link);
    free(keyboard);
    server_update_seat_capabilities(server);
    focus_workspace_view(server);
}

static void server_new_input(struct wl_listener *listener, void *data)
{
    NexwmServer *server = wl_container_of(listener, server, new_input);
    struct wlr_input_device *device = data;
    switch (device->type) {
    case WLR_INPUT_DEVICE_KEYBOARD: {
        struct wlr_keyboard *wlr_keyboard = wlr_keyboard_from_input_device(device);
        if (!wlr_keyboard) break;
        struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
        struct xkb_keymap *keymap = context ? xkb_keymap_new_from_names(context, NULL,
                                            XKB_KEYMAP_COMPILE_NO_FLAGS) : NULL;
        if (keymap) {
            if (!wlr_keyboard_set_keymap(wlr_keyboard, keymap))
                log_message("warning", "could not install the system keyboard map on %s", device->name ? device->name : "keyboard");
            xkb_keymap_unref(keymap);
        } else {
            log_message("warning", "could not create a keyboard map for %s", device->name ? device->name : "keyboard");
        }
        if (context) xkb_context_unref(context);
        wlr_keyboard_set_repeat_info(wlr_keyboard, 25, 600);

        NexwmKeyboard *keyboard = calloc(1, sizeof *keyboard);
        if (!keyboard) {
            log_message("error", "out of memory while adding keyboard");
            break;
        }
        keyboard->server = server;
        keyboard->device = device;
        keyboard->wlr = wlr_keyboard;
        NEXWM_ADD_LISTENER(&wlr_keyboard->events.key, keyboard->key, keyboard_key);
        NEXWM_ADD_LISTENER(&wlr_keyboard->events.modifiers, keyboard->modifiers, keyboard_modifiers);
        NEXWM_ADD_LISTENER(&device->events.destroy, keyboard->destroy, keyboard_destroy);
        wl_list_insert(&server->keyboards, &keyboard->link);
        wlr_seat_set_keyboard(server->seat, wlr_keyboard);
        server_update_seat_capabilities(server);
        focus_workspace_view(server);
        break;
    }
    case WLR_INPUT_DEVICE_POINTER:
        wlr_cursor_attach_input_device(server->cursor, device);
        server->pointer_present = true;
        wlr_cursor_set_xcursor(server->cursor, server->cursor_manager, "left_ptr");
        server_update_seat_capabilities(server);
        break;
    default:
        break;
    }
}

static void view_update_drag(NexwmServer *server)
{
    NexwmView *view = server->grabbed_view;
    if (!view || !view->mapped) return;
    if (server->cursor_mode == NEXWM_CURSOR_MOVE) {
        int x = (int)(server->cursor->x - server->grab_x);
        int y = (int)(server->cursor->y - server->grab_y);
        view->x = x;
        view->y = y;
        view->restore_x = x;
        view->restore_y = y;
        view->restore_valid = !view->maximized && !view->fullscreen;
        wlr_scene_node_set_position(&view->scene_tree->node, x, y);
    } else if (server->cursor_mode == NEXWM_CURSOR_RESIZE) {
        int dx = (int)(server->cursor->x - server->grab_x);
        int dy = (int)(server->cursor->y - server->grab_y);
        int width = server->grab_start_width;
        int height = server->grab_start_height;
        int x = server->grab_start_x;
        int y = server->grab_start_y;
        if (server->resize_edges & WLR_EDGE_LEFT) { width -= dx; x += dx; }
        if (server->resize_edges & WLR_EDGE_RIGHT) width += dx;
        if (server->resize_edges & WLR_EDGE_TOP) { height -= dy; y += dy; }
        if (server->resize_edges & WLR_EDGE_BOTTOM) height += dy;
        if (width < 80) { x -= 80 - width; width = 80; }
        if (height < 60) { y -= 60 - height; height = 60; }
        view->restore_valid = false;
        view_set_position_and_size(view, x, y, width, height);
    }
}

static struct wlr_scene_surface *scene_surface_at(NexwmServer *server,
                                                   double lx, double ly,
                                                   double *sx, double *sy,
                                                   struct wlr_scene_node **node_out)
{
#if NEXWM_WLROOTS_MINOR >= 21
    struct wlr_scene_node *node = wlr_scene_node_at(&server->scene->tree.node, lx, ly, sx, sy, NULL);
#else
    struct wlr_scene_node *node = wlr_scene_node_at(&server->scene->tree.node, lx, ly, sx, sy);
#endif
    if (node_out) *node_out = node;
    if (!node || node->type != WLR_SCENE_NODE_BUFFER) return NULL;
    return wlr_scene_surface_try_from_buffer(wlr_scene_buffer_from_node(node));
}

static NexwmView *view_from_scene_node(NexwmServer *server, struct wlr_scene_node *node)
{
    if (!node) return NULL;
    NexwmView *view;
    wl_list_for_each(view, &server->views, link) {
        for (struct wlr_scene_node *parent = node; parent;
             parent = parent->parent ? &parent->parent->node : NULL) {
            if (parent == &view->scene_tree->node) return view;
        }
    }
    return NULL;
}

static void cursor_process_motion(NexwmServer *server, uint32_t time_msec)
{
    if (server->cursor_mode != NEXWM_CURSOR_PASSTHROUGH) {
        view_update_drag(server);
        return;
    }
    double sx = 0, sy = 0;
    struct wlr_scene_surface *scene_surface = scene_surface_at(server,
        server->cursor->x, server->cursor->y, &sx, &sy, NULL);
    if (!scene_surface) {
        wlr_seat_pointer_notify_clear_focus(server->seat);
        return;
    }
    wlr_seat_pointer_notify_enter(server->seat, scene_surface->surface, sx, sy);
    wlr_seat_pointer_notify_motion(server->seat, time_msec, sx, sy);
    if (server->config.focus_mouse) {
        struct wlr_scene_node *node = NULL;
        scene_surface_at(server, server->cursor->x, server->cursor->y, &sx, &sy, &node);
        NexwmView *view = view_from_scene_node(server, node);
        if (view) view_focus(server, view);
    }
}

static void cursor_motion(struct wl_listener *listener, void *data)
{
    NexwmServer *server = wl_container_of(listener, server, cursor_motion);
    struct wlr_pointer_motion_event *event = data;
    wlr_cursor_move(server->cursor, &event->pointer->base, event->delta_x, event->delta_y);
    cursor_process_motion(server, event->time_msec);
}

static void cursor_motion_absolute(struct wl_listener *listener, void *data)
{
    NexwmServer *server = wl_container_of(listener, server, cursor_motion_absolute);
    struct wlr_pointer_motion_absolute_event *event = data;
    wlr_cursor_warp_absolute(server->cursor, &event->pointer->base, event->x, event->y);
    cursor_process_motion(server, event->time_msec);
}

static void cursor_button(struct wl_listener *listener, void *data)
{
    NexwmServer *server = wl_container_of(listener, server, cursor_button);
    struct wlr_pointer_button_event *event = data;
    if (event->state == WL_POINTER_BUTTON_STATE_PRESSED) {
        double sx = 0, sy = 0;
        struct wlr_scene_node *node = NULL;
        scene_surface_at(server, server->cursor->x, server->cursor->y, &sx, &sy, &node);
        NexwmView *view = view_from_scene_node(server, node);
        if (view && !server->config.focus_mouse) view_focus(server, view);
    }
    wlr_seat_pointer_notify_button(server->seat, event->time_msec,
                                    event->button, event->state);
    if (event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
        server->cursor_mode = NEXWM_CURSOR_PASSTHROUGH;
        server->grabbed_view = NULL;
    }
}

static void request_set_cursor(struct wl_listener *listener, void *data)
{
    NexwmServer *server = wl_container_of(listener, server, request_set_cursor);
    struct wlr_seat_pointer_request_set_cursor_event *event = data;
    if (event->seat_client == server->seat->pointer_state.focused_client)
        wlr_cursor_set_surface(server->cursor, event->surface,
                               event->hotspot_x, event->hotspot_y);
}

static void request_set_selection(struct wl_listener *listener, void *data)
{
    NexwmServer *server = wl_container_of(listener, server, request_set_selection);
    struct wlr_seat_request_set_selection_event *event = data;
    wlr_seat_set_selection(server->seat, event->source, event->serial);
}

static void start_interactive_move(NexwmView *view)
{
    NexwmServer *server = view->server;
    server->cursor_mode = NEXWM_CURSOR_MOVE;
    server->grabbed_view = view;
    server->grab_x = server->cursor->x - view->x;
    server->grab_y = server->cursor->y - view->y;
    view->restore_valid = false;
}

static void start_interactive_resize(NexwmView *view, uint32_t edges)
{
    NexwmServer *server = view->server;
    server->cursor_mode = NEXWM_CURSOR_RESIZE;
    server->grabbed_view = view;
    server->grab_x = server->cursor->x;
    server->grab_y = server->cursor->y;
    server->grab_start_x = view->x;
    server->grab_start_y = view->y;
    server->grab_start_width = view->width;
    server->grab_start_height = view->height;
    server->resize_edges = edges;
}

static void layer_new_popup(struct wl_listener *listener, void *data)
{
    NexwmLayerSurface *layer = wl_container_of(listener, layer, new_popup);
    struct wlr_xdg_popup *popup = data;
    if (!layer->scene) {
        wlr_xdg_popup_destroy(popup);
        return;
    }
    create_scene_popup(layer->server, popup, layer->scene->tree);
}

static void layer_map(struct wl_listener *listener, void *data)
{
    (void)data;
    NexwmLayerSurface *surface = wl_container_of(listener, surface, map);
    arrange_layers(surface->output);
    if (surface->wlr->current.keyboard_interactive !=
        ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE) {
        surface->server->keyboard_layer = surface;
        struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(surface->server->seat);
        if (keyboard) {
            wlr_seat_keyboard_notify_enter(surface->server->seat, surface->wlr->surface,
                                            keyboard->keycodes, keyboard->num_keycodes,
                                            &keyboard->modifiers);
        }
    }
    if (surface->output) wlr_output_schedule_frame(surface->output->wlr);
}

static void layer_unmap(struct wl_listener *listener, void *data)
{
    (void)data;
    NexwmLayerSurface *surface = wl_container_of(listener, surface, unmap);
    if (surface->server->keyboard_layer == surface) {
        surface->server->keyboard_layer = NULL;
        wlr_seat_keyboard_notify_clear_focus(surface->server->seat);
        focus_workspace_view(surface->server);
    }
    arrange_layers(surface->output);
    if (surface->output) wlr_output_schedule_frame(surface->output->wlr);
}

static void layer_destroy(struct wl_listener *listener, void *data)
{
    (void)data;
    NexwmLayerSurface *surface = wl_container_of(listener, surface, destroy);
    NexwmOutput *output = surface->output;
    if (surface->server->keyboard_layer == surface) {
        surface->server->keyboard_layer = NULL;
        wlr_seat_keyboard_notify_clear_focus(surface->server->seat);
        focus_workspace_view(surface->server);
    }
    wl_list_remove(&surface->map.link);
    wl_list_remove(&surface->unmap.link);
    wl_list_remove(&surface->destroy.link);
    wl_list_remove(&surface->new_popup.link);
    wl_list_remove(&surface->link);
    free(surface);
    arrange_layers(output);
}

static void server_new_layer_surface(struct wl_listener *listener, void *data)
{
    NexwmServer *server = wl_container_of(listener, server, new_layer_surface);
    struct wlr_layer_surface_v1 *wlr = data;
    NexwmOutput *output = NULL;
    if (wlr->output) {
        NexwmOutput *candidate;
        wl_list_for_each(candidate, &server->outputs, link)
            if (candidate->wlr == wlr->output) { output = candidate; break; }
    }
    if (!output) output = first_output(server);
    if (!output) {
        log_message("warning", "layer surface '%s' arrived before an output; waiting for an output",
                    wlr->namespace ? wlr->namespace : "unnamed");
        return;
    }
    wlr->output = output->wlr;
    int layer = (int)wlr->pending.layer;
    if (layer < ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND || layer > ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY)
        layer = ZWLR_LAYER_SHELL_V1_LAYER_TOP;

    NexwmLayerSurface *surface = calloc(1, sizeof *surface);
    if (!surface) {
        log_message("error", "out of memory while adding layer surface");
        wlr_layer_surface_v1_destroy(wlr);
        return;
    }
    surface->server = server;
    surface->wlr = wlr;
    surface->output = output;
    surface->scene = wlr_scene_layer_surface_v1_create(output->layer_trees[layer], wlr);
    if (!surface->scene) {
        log_message("error", "could not create the scene for layer surface '%s'",
                    wlr->namespace ? wlr->namespace : "unnamed");
        free(surface);
        wlr_layer_surface_v1_destroy(wlr);
        return;
    }
    NEXWM_ADD_LISTENER(&wlr->surface->events.map, surface->map, layer_map);
    NEXWM_ADD_LISTENER(&wlr->surface->events.unmap, surface->unmap, layer_unmap);
    NEXWM_ADD_LISTENER(&wlr->events.destroy, surface->destroy, layer_destroy);
    NEXWM_ADD_LISTENER(&wlr->events.new_popup, surface->new_popup, layer_new_popup);
    wl_list_insert(&server->layer_surfaces, &surface->link);
    arrange_layers(output);
}

static void view_commit(struct wl_listener *listener, void *data)
{
    (void)data;
    NexwmView *view = wl_container_of(listener, view, commit);
    if (view->xdg->base->initial_commit) {
        /* A toplevel must be configured after its first empty commit before it is allowed to map. */
        wlr_xdg_toplevel_set_size(view->xdg, 0, 0);
        return;
    }
    if (view->mapped) {
        int width = view->xdg->base->current.geometry.width;
        int height = view->xdg->base->current.geometry.height;
        if (width > 0) view->width = width;
        if (height > 0) view->height = height;
    }
}

static void view_map(struct wl_listener *listener, void *data)
{
    (void)data;
    NexwmView *view = wl_container_of(listener, view, map);
    NexwmServer *server = view->server;
    struct wlr_box area = server_workarea(server);
    view->mapped = true;
    view->minimized = false;
    view->workspace = server->workspace;
    view->output = first_output(server);
    view->width = view->xdg->base->current.geometry.width;
    view->height = view->xdg->base->current.geometry.height;
    if (view->width < 1) view->width = NEXWM_DEFAULT_WIDTH;
    if (view->height < 1) view->height = NEXWM_DEFAULT_HEIGHT;
    if (view->width > area.width) view->width = area.width;
    if (view->height > area.height) view->height = area.height;
    size_t count = 0;
    NexwmView *other;
    wl_list_for_each(other, &server->views, link) if (other->mapped) count++;
    int cascade = (int)(count % 10) * 28;
    view->x = area.x + 32 + cascade;
    view->y = area.y + 32 + cascade;
    if (view->x + view->width > area.x + area.width) view->x = area.x + area.width - view->width;
    if (view->y + view->height > area.y + area.height) view->y = area.y + area.height - view->height;
    view->restore_valid = false;
    wlr_scene_node_set_position(&view->scene_tree->node, view->x, view->y);
    wlr_scene_node_set_enabled(&view->scene_tree->node, true);
    wl_list_insert(&server->views, &view->link);
    if (view->foreign && view->output)
        wlr_foreign_toplevel_handle_v1_output_enter(view->foreign, view->output->wlr);
    view_sync_foreign_state(view);
    view_focus(server, view);
    log_message("info", "mapped '%s' on workspace %d",
                view->xdg->title ? view->xdg->title : "(untitled)", view->workspace + 1);
}

static void view_unmap(struct wl_listener *listener, void *data)
{
    (void)data;
    NexwmView *view = wl_container_of(listener, view, unmap);
    NexwmServer *server = view->server;
    if (!view->mapped) return;
    view->mapped = false;
    view->minimized = false;
    if (server->grabbed_view == view) {
        server->grabbed_view = NULL;
        server->cursor_mode = NEXWM_CURSOR_PASSTHROUGH;
    }
    if (view->output && view->foreign)
        wlr_foreign_toplevel_handle_v1_output_leave(view->foreign, view->output->wlr);
    view->output = NULL;
    wl_list_remove(&view->link);
    if (server->focused == view) {
        server->focused = NULL;
        wlr_seat_keyboard_notify_clear_focus(server->seat);
        focus_workspace_view(server);
    }
    wlr_scene_node_set_enabled(&view->scene_tree->node, false);
    view_sync_foreign_state(view);
}

static void view_destroy(struct wl_listener *listener, void *data)
{
    (void)data;
    NexwmView *view = wl_container_of(listener, view, destroy);
    if (view->mapped) view_unmap(&view->unmap, NULL);
    wl_list_remove(&view->map.link);
    wl_list_remove(&view->unmap.link);
    wl_list_remove(&view->destroy.link);
    wl_list_remove(&view->commit.link);
    wl_list_remove(&view->request_maximize.link);
    wl_list_remove(&view->request_fullscreen.link);
    wl_list_remove(&view->request_minimize.link);
    wl_list_remove(&view->request_move.link);
    wl_list_remove(&view->request_resize.link);
    wl_list_remove(&view->set_title.link);
    wl_list_remove(&view->set_app_id.link);
    if (view->foreign) {
        wl_list_remove(&view->foreign_request_activate.link);
        wl_list_remove(&view->foreign_request_maximize.link);
        wl_list_remove(&view->foreign_request_minimize.link);
        wl_list_remove(&view->foreign_request_fullscreen.link);
        wl_list_remove(&view->foreign_request_close.link);
        view->foreign->data = NULL;
        wlr_foreign_toplevel_handle_v1_destroy(view->foreign);
    }
    if (view->server->focused == view) {
        view->server->focused = NULL;
        wlr_seat_keyboard_notify_clear_focus(view->server->seat);
        focus_workspace_view(view->server);
    }
    free(view);
}

static void view_set_title(struct wl_listener *listener, void *data)
{
    (void)data;
    NexwmView *view = wl_container_of(listener, view, set_title);
    if (view->foreign) wlr_foreign_toplevel_handle_v1_set_title(view->foreign,
                                                                view->xdg->title ? view->xdg->title : "");
}

static void view_set_app_id(struct wl_listener *listener, void *data)
{
    (void)data;
    NexwmView *view = wl_container_of(listener, view, set_app_id);
    if (view->foreign) wlr_foreign_toplevel_handle_v1_set_app_id(view->foreign,
                                                                 view->xdg->app_id ? view->xdg->app_id : "");
}

static void view_request_maximize(struct wl_listener *listener, void *data)
{
    (void)data;
    NexwmView *view = wl_container_of(listener, view, request_maximize);
    view_set_maximized(view, view->xdg->requested.maximized);
}

static void view_request_fullscreen(struct wl_listener *listener, void *data)
{
    (void)data;
    NexwmView *view = wl_container_of(listener, view, request_fullscreen);
    view_set_fullscreen(view, view->xdg->requested.fullscreen);
}

static void view_request_minimize(struct wl_listener *listener, void *data)
{
    (void)data;
    NexwmView *view = wl_container_of(listener, view, request_minimize);
    view_minimize(view, true);
}

static void view_request_move(struct wl_listener *listener, void *data)
{
    NexwmView *view = wl_container_of(listener, view, request_move);
    (void)data;
    if (!view->mapped) return;
    view_focus(view->server, view);
    start_interactive_move(view);
}

static void view_request_resize(struct wl_listener *listener, void *data)
{
    NexwmView *view = wl_container_of(listener, view, request_resize);
    struct wlr_xdg_toplevel_resize_event *event = data;
    if (!view->mapped) return;
    view_focus(view->server, view);
    start_interactive_resize(view, event->edges);
}

static void foreign_request_activate(struct wl_listener *listener, void *data)
{
    (void)data;
    NexwmView *view = wl_container_of(listener, view, foreign_request_activate);
    if (!view->mapped) return;
    if (view->workspace != view->server->workspace) set_workspace(view->server, view->workspace);
    view_focus(view->server, view);
}

static void foreign_request_maximize(struct wl_listener *listener, void *data)
{
    struct wlr_foreign_toplevel_handle_v1_maximized_event *event = data;
    NexwmView *view = wl_container_of(listener, view, foreign_request_maximize);
    view_set_maximized(view, event->maximized);
}

static void foreign_request_minimize(struct wl_listener *listener, void *data)
{
    struct wlr_foreign_toplevel_handle_v1_minimized_event *event = data;
    NexwmView *view = wl_container_of(listener, view, foreign_request_minimize);
    view_minimize(view, event->minimized);
}

static void foreign_request_fullscreen(struct wl_listener *listener, void *data)
{
    struct wlr_foreign_toplevel_handle_v1_fullscreen_event *event = data;
    NexwmView *view = wl_container_of(listener, view, foreign_request_fullscreen);
    view_set_fullscreen(view, event->fullscreen);
}

static void foreign_request_close(struct wl_listener *listener, void *data)
{
    (void)data;
    NexwmView *view = wl_container_of(listener, view, foreign_request_close);
    wlr_xdg_toplevel_send_close(view->xdg);
}

static void server_new_toplevel(struct wl_listener *listener, void *data)
{
    NexwmServer *server = wl_container_of(listener, server, new_toplevel);
    struct wlr_xdg_toplevel *xdg = data;
    NexwmView *view = calloc(1, sizeof *view);
    if (!view) {
        log_message("error", "out of memory while adding a window");
        wlr_xdg_toplevel_send_close(xdg);
        return;
    }
    view->server = server;
    view->xdg = xdg;
    view->workspace = server->workspace;
    view->scene_tree = wlr_scene_xdg_surface_create(server->views_tree, xdg->base);
    if (!view->scene_tree) {
        log_message("error", "could not create the scene for a Wayland window");
        free(view);
        wlr_xdg_toplevel_send_close(xdg);
        return;
    }
    wlr_scene_node_set_enabled(&view->scene_tree->node, false);
    xdg->base->data = view->scene_tree;

    NEXWM_ADD_LISTENER(&xdg->base->surface->events.map, view->map, view_map);
    NEXWM_ADD_LISTENER(&xdg->base->surface->events.unmap, view->unmap, view_unmap);
    NEXWM_ADD_LISTENER(&xdg->base->events.destroy, view->destroy, view_destroy);
    NEXWM_ADD_LISTENER(&xdg->base->surface->events.commit, view->commit, view_commit);
    NEXWM_ADD_LISTENER(&xdg->events.request_maximize, view->request_maximize, view_request_maximize);
    NEXWM_ADD_LISTENER(&xdg->events.request_fullscreen, view->request_fullscreen, view_request_fullscreen);
    NEXWM_ADD_LISTENER(&xdg->events.request_minimize, view->request_minimize, view_request_minimize);
    NEXWM_ADD_LISTENER(&xdg->events.request_move, view->request_move, view_request_move);
    NEXWM_ADD_LISTENER(&xdg->events.request_resize, view->request_resize, view_request_resize);
    NEXWM_ADD_LISTENER(&xdg->events.set_title, view->set_title, view_set_title);
    NEXWM_ADD_LISTENER(&xdg->events.set_app_id, view->set_app_id, view_set_app_id);

    if (server->foreign_manager) {
        view->foreign = wlr_foreign_toplevel_handle_v1_create(server->foreign_manager);
        if (view->foreign) {
            view->foreign->data = view;
            wlr_foreign_toplevel_handle_v1_set_title(view->foreign, xdg->title ? xdg->title : "");
            wlr_foreign_toplevel_handle_v1_set_app_id(view->foreign, xdg->app_id ? xdg->app_id : "");
            NEXWM_ADD_LISTENER(&view->foreign->events.request_activate, view->foreign_request_activate, foreign_request_activate);
            NEXWM_ADD_LISTENER(&view->foreign->events.request_maximize, view->foreign_request_maximize, foreign_request_maximize);
            NEXWM_ADD_LISTENER(&view->foreign->events.request_minimize, view->foreign_request_minimize, foreign_request_minimize);
            NEXWM_ADD_LISTENER(&view->foreign->events.request_fullscreen, view->foreign_request_fullscreen, foreign_request_fullscreen);
            NEXWM_ADD_LISTENER(&view->foreign->events.request_close, view->foreign_request_close, foreign_request_close);
        }
    }
}

static void popup_destroy(struct wl_listener *listener, void *data)
{
    (void)data;
    NexwmPopup *popup = wl_container_of(listener, popup, destroy);
    wl_list_remove(&popup->new_popup.link);
    wl_list_remove(&popup->destroy.link);
    wl_list_remove(&popup->link);
    free(popup);
}

static void popup_new_popup(struct wl_listener *listener, void *data)
{
    NexwmPopup *parent = wl_container_of(listener, parent, new_popup);
    create_scene_popup(parent->server, data, parent->scene_tree);
}

static struct wlr_scene_tree *popup_root_tree(NexwmServer *server,
                                               struct wlr_xdg_popup *popup,
                                               struct wlr_scene_tree *fallback)
{
    struct wlr_surface *root_surface = popup->parent;
    for (;;) {
        struct wlr_xdg_surface *xdg = wlr_xdg_surface_try_from_wlr_surface(root_surface);
        if (xdg && xdg->role == WLR_XDG_SURFACE_ROLE_POPUP && xdg->popup) {
            root_surface = xdg->popup->parent;
            continue;
        }
        if (xdg && xdg->data) return xdg->data;
        NexwmLayerSurface *layer;
        wl_list_for_each(layer, &server->layer_surfaces, link) {
            if (layer->wlr->surface == root_surface && layer->scene)
                return layer->scene->tree;
        }
        return fallback;
    }
}

static void create_scene_popup(NexwmServer *server, struct wlr_xdg_popup *wlr_popup,
                               struct wlr_scene_tree *parent_tree)
{
    if (!wlr_popup || !parent_tree || wlr_popup->base->data) return;

    struct wlr_scene_tree *root_tree = popup_root_tree(server, wlr_popup, parent_tree);
#if NEXWM_WLROOTS_MINOR >= 21
    double root_x = 0, root_y = 0;
#else
    int root_x = 0, root_y = 0;
#endif
    if (root_tree) wlr_scene_node_coords(&root_tree->node, &root_x, &root_y);
    struct wlr_box constraint = server_workarea(server);
    constraint.x -= (int)root_x;
    constraint.y -= (int)root_y;
    wlr_xdg_popup_unconstrain_from_box(wlr_popup, &constraint);

    NexwmPopup *popup = calloc(1, sizeof *popup);
    if (!popup) {
        wl_resource_post_no_memory(wlr_popup->resource);
        wlr_xdg_popup_destroy(wlr_popup);
        return;
    }
    popup->server = server;
    popup->wlr = wlr_popup;
    popup->scene_tree = wlr_scene_xdg_surface_create(parent_tree, wlr_popup->base);
    if (!popup->scene_tree) {
        free(popup);
        wl_resource_post_no_memory(wlr_popup->resource);
        wlr_xdg_popup_destroy(wlr_popup);
        return;
    }
    wlr_popup->base->data = popup->scene_tree;
    wlr_popup->base->surface->data = popup->scene_tree;
    NEXWM_ADD_LISTENER(&wlr_popup->base->events.new_popup, popup->new_popup, popup_new_popup);
    NEXWM_ADD_LISTENER(&wlr_popup->events.destroy, popup->destroy, popup_destroy);
    wl_list_insert(&server->popups, &popup->link);
}

static void server_new_popup(struct wl_listener *listener, void *data)
{
    NexwmServer *server = wl_container_of(listener, server, new_popup);
    struct wlr_xdg_popup *popup = data;
    if (popup->base->data) return;
    struct wlr_scene_tree *parent_tree = NULL;
    struct wlr_xdg_surface *parent_xdg = wlr_xdg_surface_try_from_wlr_surface(popup->parent);
    if (parent_xdg && parent_xdg->data) parent_tree = parent_xdg->data;
    if (!parent_tree) {
        NexwmLayerSurface *layer;
        wl_list_for_each(layer, &server->layer_surfaces, link) {
            if (layer->wlr->surface == popup->parent && layer->scene) {
                parent_tree = layer->scene->tree;
                break;
            }
        }
    }
    if (!parent_tree) {
        log_message("warning", "a Wayland popup has no scene parent; dismissing it");
        wlr_xdg_popup_destroy(popup);
        return;
    }
    create_scene_popup(server, popup, parent_tree);
}

static void child_timer(struct wl_event_loop *loop, void *data)
{
    (void)loop;
    NexwmServer *server = data;
    pid_t pid;
    int status;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        if (pid == server->session_pid) {
            server->session_pid = 0;
            server->session_status = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
            log_message("info", "session command exited with status %d", server->session_status);
            wl_display_terminate(server->display);
        }
    }
}

static int child_timer_tick(void *data)
{
    child_timer(NULL, data);
    NexwmServer *server = data;
    return wl_event_source_timer_update(server->child_timer, NEXWM_CHILD_POLL_MS);
}

static int terminate_signal(int signal_number, void *data)
{
    (void)signal_number;
    NexwmServer *server = data;
    wl_display_terminate(server->display);
    return 0;
}

static pid_t start_session_command(const char *command)
{
    if (!command || !*command) return 0;
    pid_t pid = fork();
    if (pid < 0) {
        log_message("error", "cannot start the session command: %s", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGCHLD, SIG_DFL);
        execl("/bin/sh", "sh", "-c", command, (char *)NULL);
        _exit(127);
    }
    log_message("info", "started session command (pid %ld)", (long)pid);
    return pid;
}

static void stop_session_command(NexwmServer *server)
{
    if (server->session_pid <= 0) return;
    pid_t pid = server->session_pid;
    kill(pid, SIGTERM);
    for (int attempt = 0; attempt < 20; attempt++) {
        int status;
        pid_t result = waitpid(pid, &status, WNOHANG);
        if (result == pid) {
            server->session_pid = 0;
            return;
        }
        if (result < 0 && errno == ECHILD) {
            server->session_pid = 0;
            return;
        }
        struct timespec delay = { .tv_sec = 0, .tv_nsec = 50000000 };
        nanosleep(&delay, NULL);
    }
    kill(pid, SIGKILL);
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) { }
    server->session_pid = 0;
}

static bool server_init(NexwmServer *server)
{
    server->display = wl_display_create();
    if (!server->display) {
        log_message("error", "could not create the Wayland display");
        return false;
    }
    server->event_loop = wl_display_get_event_loop(server->display);

#if NEXWM_WLROOTS_MINOR >= 18
    server->backend = wlr_backend_autocreate(server->event_loop, NULL);
#else
    server->backend = wlr_backend_autocreate(server->display, &server->session);
#endif
    if (!server->backend) {
        log_message("error", "could not create a wlroots backend (is a seat or a nested Wayland/X11 display available?)");
        return false;
    }
    server->renderer = wlr_renderer_autocreate(server->backend);
    if (!server->renderer) {
        log_message("error", "could not create a wlroots renderer");
        return false;
    }
    if (!wlr_renderer_init_wl_display(server->renderer, server->display)) {
        log_message("error", "could not initialize the Wayland renderer protocols");
        return false;
    }
    server->allocator = wlr_allocator_autocreate(server->backend, server->renderer);
    if (!server->allocator) {
        log_message("error", "could not create a wlroots output allocator");
        return false;
    }
    if (!wlr_compositor_create(server->display, 6, server->renderer)) {
        log_message("error", "could not create the Wayland compositor global");
        return false;
    }
    if (!wlr_subcompositor_create(server->display)) {
        log_message("error", "could not create the Wayland subcompositor global");
        return false;
    }
    if (!wlr_data_device_manager_create(server->display)) {
        log_message("error", "could not create the Wayland data-device manager");
        return false;
    }

    server->scene = wlr_scene_create();
    server->output_layout = wlr_output_layout_create(server->display);
    server->cursor = wlr_cursor_create();
    server->cursor_manager = wlr_xcursor_manager_create(NULL, 24);
    server->seat = wlr_seat_create(server->display, "seat0");
    server->xdg_shell = wlr_xdg_shell_create(server->display, 6);
    server->layer_shell = wlr_layer_shell_v1_create(server->display, 4);
    server->foreign_manager = wlr_foreign_toplevel_manager_v1_create(server->display);
    if (!server->scene || !server->output_layout || !server->cursor || !server->cursor_manager ||
        !server->seat || !server->xdg_shell || !server->layer_shell || !server->foreign_manager) {
        log_message("error", "could not create the compositor's Wayland globals");
        return false;
    }

    server->scene_layers[ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND] =
        wlr_scene_tree_create(&server->scene->tree);
    server->scene_layers[ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM] =
        wlr_scene_tree_create(&server->scene->tree);
    server->views_tree = wlr_scene_tree_create(&server->scene->tree);
    server->scene_layers[ZWLR_LAYER_SHELL_V1_LAYER_TOP] =
        wlr_scene_tree_create(&server->scene->tree);
    server->scene_layers[ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY] =
        wlr_scene_tree_create(&server->scene->tree);
    for (int i = 0; i < NEXWM_LAYER_COUNT; i++) {
        if (!server->scene_layers[i]) {
            log_message("error", "could not create the compositor scene graph");
            return false;
        }
    }
    if (!server->views_tree) {
        log_message("error", "could not create the window scene graph");
        return false;
    }
    server->scene_output_layout = wlr_scene_attach_output_layout(server->scene, server->output_layout);
    if (!server->scene_output_layout) {
        log_message("error", "could not attach the output layout to the scene graph");
        return false;
    }

    wlr_cursor_attach_output_layout(server->cursor, server->output_layout);
    wlr_seat_set_capabilities(server->seat, 0);
    NEXWM_ADD_LISTENER(&server->backend->events.new_output, server->new_output, server_new_output);
    NEXWM_ADD_LISTENER(&server->backend->events.new_input, server->new_input, server_new_input);
    NEXWM_ADD_LISTENER(&server->xdg_shell->events.new_toplevel, server->new_toplevel, server_new_toplevel);
    NEXWM_ADD_LISTENER(&server->xdg_shell->events.new_popup, server->new_popup, server_new_popup);
    NEXWM_ADD_LISTENER(&server->layer_shell->events.new_surface, server->new_layer_surface, server_new_layer_surface);
    NEXWM_ADD_LISTENER(&server->cursor->events.motion, server->cursor_motion, cursor_motion);
    NEXWM_ADD_LISTENER(&server->cursor->events.motion_absolute, server->cursor_motion_absolute, cursor_motion_absolute);
    NEXWM_ADD_LISTENER(&server->cursor->events.button, server->cursor_button, cursor_button);
    NEXWM_ADD_LISTENER(&server->seat->events.request_set_cursor, server->request_set_cursor, request_set_cursor);
    NEXWM_ADD_LISTENER(&server->seat->events.request_set_selection, server->request_set_selection, request_set_selection);
    return true;
}

/* Take a listener of this program off the object it was put on. A listener that was never added (the compositor
 * failed before it got that far) is left alone: wl_list_remove on nothing is a crash, not a no-op. */
static void listener_remove(struct wl_listener *listener)
{
    if (listener->link.prev || listener->link.next) wl_list_remove(&listener->link);
}

static void server_finish(NexwmServer *server)
{
    stop_session_command(server);
    if (server->display) wl_display_destroy_clients(server->display);
    /* Every listener this program put on something of wlroots comes off before the objects are destroyed. The cursor
     * is the one that insists on it: wlr_cursor_destroy requires that nothing is listening to it any more ("Assertion
     * `wl_list_empty(&cur->events.motion.listener_list)' failed" — wlr_cursor.c), and a distribution builds wlroots
     * with its assertions on, so a compositor that leaves its listeners behind is ended by an abort (status 134)
     * instead of the clean exit its session asked for. The seat's listeners go before the backend that owns the seat
     * is destroyed, and the shell's before the display takes the shells down. */
    listener_remove(&server->cursor_motion);
    listener_remove(&server->cursor_motion_absolute);
    listener_remove(&server->cursor_button);
    listener_remove(&server->request_set_cursor);
    listener_remove(&server->request_set_selection);
    listener_remove(&server->new_toplevel);
    listener_remove(&server->new_popup);
    listener_remove(&server->new_layer_surface);
    if (server->backend) {
        wl_list_remove(&server->new_output.link);
        wl_list_remove(&server->new_input.link);
        wlr_backend_destroy(server->backend);
        server->backend = NULL;
    }
    if (server->child_timer) wl_event_source_remove(server->child_timer);
    if (server->sigint_source) wl_event_source_remove(server->sigint_source);
    if (server->sigterm_source) wl_event_source_remove(server->sigterm_source);
    if (server->cursor_manager) wlr_xcursor_manager_destroy(server->cursor_manager);
    if (server->cursor) wlr_cursor_destroy(server->cursor);
    if (server->output_layout) wlr_output_layout_destroy(server->output_layout);
    if (server->scene) wlr_scene_node_destroy(&server->scene->tree.node);
    if (server->allocator) wlr_allocator_destroy(server->allocator);
    if (server->renderer) wlr_renderer_destroy(server->renderer);
    if (server->display) wl_display_destroy(server->display);
    nexwm_config_free(&server->config);
}
int nexwm_wayland_run(const char *config_path, int replace, const char *session)
{
    if (replace) {
        log_message("error", "--replace is an X11-only option; a Wayland compositor cannot take over another compositor");
        return 3;
    }
    if (!getenv("XDG_RUNTIME_DIR") || !*getenv("XDG_RUNTIME_DIR")) {
        log_message("error", "XDG_RUNTIME_DIR is not set; start NexWM from a Wayland-capable login session");
        return 3;
    }

    NexwmServer server;
    memset(&server, 0, sizeof server);
    wl_list_init(&server.outputs);
    wl_list_init(&server.views);
    wl_list_init(&server.keyboards);
    wl_list_init(&server.layer_surfaces);
    wl_list_init(&server.popups);
    server.cursor_mode = NEXWM_CURSOR_PASSTHROUGH;
    server.workspace = 0;
    server.session_status = 0;
    wl_list_init(&server.new_output.link);
    wl_list_init(&server.new_input.link);
    wl_list_init(&server.new_toplevel.link);
    wl_list_init(&server.new_popup.link);
    wl_list_init(&server.new_layer_surface.link);
    wl_list_init(&server.cursor_motion.link);
    wl_list_init(&server.cursor_motion_absolute.link);
    wl_list_init(&server.cursor_button.link);
    wl_list_init(&server.request_set_cursor.link);
    wl_list_init(&server.request_set_selection.link);
    nexwm_config_defaults(&server.config);
    if (config_path) {
        char error[256];
        if (nexwm_config_load(&server.config, config_path, error, sizeof error) != 0) {
            log_message("error", "%s: %s", config_path, error);
            nexwm_config_free(&server.config);
            return 2;
        }
    }

    wlr_log_init(WLR_INFO, NULL);
    setenv("XDG_SESSION_TYPE", "wayland", 1);
    if (!getenv("XDG_CURRENT_DESKTOP")) setenv("XDG_CURRENT_DESKTOP", "HDE", 0);
    if (!server_init(&server)) {
        server.exit_code = 3;
        goto finish;
    }
    server.child_timer = wl_event_loop_add_timer(server.event_loop, child_timer_tick, &server);
    server.sigint_source = wl_event_loop_add_signal(server.event_loop, SIGINT, terminate_signal, &server);
    server.sigterm_source = wl_event_loop_add_signal(server.event_loop, SIGTERM, terminate_signal, &server);
    if (!server.child_timer || !server.sigint_source || !server.sigterm_source) {
        log_message("error", "could not install the compositor's event-loop sources");
        server.exit_code = 3;
        goto finish;
    }

    const char *socket = wl_display_add_socket_auto(server.display);
    if (!socket) {
        log_message("error", "could not create a Wayland socket in %s: %s",
                    getenv("XDG_RUNTIME_DIR"), strerror(errno));
        server.exit_code = 3;
        goto finish;
    }
    if (!wlr_backend_start(server.backend)) {
        log_message("error", "could not start the wlroots backend");
        server.exit_code = 3;
        goto finish;
    }
    setenv("WAYLAND_DISPLAY", socket, 1);
    server.session_pid = start_session_command(session);
    if (server.session_pid < 0) {
        server.exit_code = 3;
        goto finish;
    }
    if (server.child_timer) wl_event_source_timer_update(server.child_timer, NEXWM_CHILD_POLL_MS);

    NexwmOutput *output = first_output(&server);
    if (output) {
        log_message("info", "NexWM Wayland compositor on %s (%s, %dx%d)",
                    socket, NEXWM_NAME, output->full_area.width, output->full_area.height);
    } else {
        log_message("info", "NexWM Wayland compositor on %s (%s); waiting for an output",
                    socket, NEXWM_NAME);
    }
    log_message("info", "workspaces: %d; config: %s", server.config.desktops,
                config_path ? config_path : "defaults");
    server.running = true;
    wl_display_run(server.display);
    server.running = false;
    server.exit_code = server.session_status;

finish:
    server_finish(&server);
    return server.exit_code;
}

#endif /* NEXWM_HAVE_WLROOTS */
