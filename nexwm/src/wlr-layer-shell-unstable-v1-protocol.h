/*
 * Compatibility fallback for wlroots development packages that ship
 * wlr/types/wlr_layer_shell_v1.h without its generated protocol header.
 * That public wlroots header uses only these enum declarations. Their values
 * are defined by wlr-layer-shell-unstable-v1.xml.
 *
 * The actual generated header is preferred when the distro installs it; the
 * Makefile puts wlroots' pkg-config include paths before nexwm/src.
 */
#ifndef NEXWM_WLR_LAYER_SHELL_PROTOCOL_FALLBACK_H
#define NEXWM_WLR_LAYER_SHELL_PROTOCOL_FALLBACK_H

enum zwlr_layer_shell_v1_layer {
    ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND = 0,
    ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM = 1,
    ZWLR_LAYER_SHELL_V1_LAYER_TOP = 2,
    ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY = 3,
};

enum zwlr_layer_surface_v1_keyboard_interactivity {
    ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE = 0,
    ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE = 1,
    ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND = 2,
};

#endif
