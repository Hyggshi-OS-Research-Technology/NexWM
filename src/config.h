/*
 * config.h - Configuration structures for NexWM
 */

#ifndef NEXWM_CONFIG_H
#define NEXWM_CONFIG_H

#include <stdint.h>
#include <xcb/xcb.h>

#define NEX_MAX_KEYBINDS 128
#define NEX_MAX_RULES 64
#define NEX_MAX_WORKSPACES 16
#define NEX_MAX_MONITORS 8

typedef enum {
    NEX_LAYOUT_FLOATING = 0,
    NEX_LAYOUT_TILED = 1,
    NEX_LAYOUT_MONOCLE = 2
} nex_layout_t;

typedef struct {
    uint16_t mod;
    xcb_keysym_t keysym;
    void (*action)(void *arg);
    void *arg;
} nex_keybind_t;

typedef struct {
    char class[64];
    char instance[64];
    char title[128];
    int workspace;
    int floating;
    int fullscreen;
} nex_rule_t;

typedef struct {
    uint16_t modkey;
    char terminal[128];
    char launcher[128];
    uint32_t border_width;
    uint32_t border_focus;
    uint32_t border_normal;
    uint32_t gaps;
    int focus_follows_mouse;
    int workspace_count;
    nex_layout_t default_layout;

    nex_keybind_t keybinds[NEX_MAX_KEYBINDS];
    int keybind_count;

    nex_rule_t rules[NEX_MAX_RULES];
    int rule_count;
} nex_config_t;

extern nex_config_t g_config;

int nex_config_load(const char *path);
void nex_config_default(void);

#endif
