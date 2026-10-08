/* nexwm-wayland-probe.c — verify the Wayland globals NexWM publishes on a headless test output. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>

struct probe {
    bool compositor;
    bool subcompositor;
    bool shm;
    bool output;
    bool seat;
    bool xdg_shell;
    bool layer_shell;
    bool foreign_toplevel;
};

static void global(void *data, struct wl_registry *registry, uint32_t name,
                   const char *interface, uint32_t version)
{
    (void)registry;
    (void)name;
    (void)version;
    struct probe *probe = data;
    if (!strcmp(interface, "wl_compositor")) probe->compositor = true;
    else if (!strcmp(interface, "wl_subcompositor")) probe->subcompositor = true;
    else if (!strcmp(interface, "wl_shm")) probe->shm = true;
    else if (!strcmp(interface, "wl_output")) probe->output = true;
    else if (!strcmp(interface, "wl_seat")) probe->seat = true;
    else if (!strcmp(interface, "xdg_wm_base")) probe->xdg_shell = true;
    else if (!strcmp(interface, "zwlr_layer_shell_v1")) probe->layer_shell = true;
    else if (!strcmp(interface, "zwlr_foreign_toplevel_manager_v1")) probe->foreign_toplevel = true;
}

static void global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = global,
    .global_remove = global_remove,
};

int main(void)
{
    struct wl_display *display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "nexwm-wayland-probe: cannot connect to %s\n",
                getenv("WAYLAND_DISPLAY") ? getenv("WAYLAND_DISPLAY") : "(default socket)");
        return 1;
    }
    struct probe probe = {0};
    struct wl_registry *registry = wl_display_get_registry(display);
    if (!registry || wl_registry_add_listener(registry, &registry_listener, &probe) < 0 ||
        wl_display_roundtrip(display) < 0) {
        fprintf(stderr, "nexwm-wayland-probe: could not read the compositor registry\n");
        if (registry) wl_registry_destroy(registry);
        wl_display_disconnect(display);
        return 1;
    }
    wl_registry_destroy(registry);
    wl_display_disconnect(display);

    printf("wl_compositor=%d wl_subcompositor=%d wl_shm=%d wl_output=%d wl_seat=%d xdg_wm_base=%d "
           "zwlr_layer_shell_v1=%d zwlr_foreign_toplevel_manager_v1=%d\n",
           probe.compositor, probe.subcompositor, probe.shm, probe.output, probe.seat, probe.xdg_shell,
           probe.layer_shell, probe.foreign_toplevel);
    return probe.compositor && probe.subcompositor && probe.shm && probe.output && probe.seat && probe.xdg_shell &&
           probe.layer_shell && probe.foreign_toplevel ? 0 : 2;
}
