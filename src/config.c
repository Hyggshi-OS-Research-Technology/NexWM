/*
 * config.c - Configuration parser for NexWM
 * Phase 1: Default configuration only
 */

#include "config.h"
#include "log.h"
#include <string.h>
#include <stdlib.h>

nex_config_t g_config;

void nex_config_default(void)
{
    memset(&g_config, 0, sizeof(nex_config_t));

    g_config.modkey = XCB_MOD_MASK_4;
    strncpy(g_config.terminal, "nex-terminal", sizeof(g_config.terminal) - 1);
    strncpy(g_config.launcher, "nex-launcher", sizeof(g_config.launcher) - 1);
    g_config.border_width = 2;
    g_config.border_focus = 0x5b8dd9;
    g_config.border_normal = 0x444444;
    g_config.gaps = 8;
    g_config.focus_follows_mouse = 1;
    g_config.workspace_count = 9;
    g_config.default_layout = NEX_LAYOUT_TILED;
    g_config.keybind_count = 0;
    g_config.rule_count = 0;

    NEX_INFO("Loaded default configuration");
}

int nex_config_load(const char *path)
{
    (void)path;
    nex_config_default();
    NEX_INFO("Configuration file loading not yet implemented (Phase 1)");
    return 0;
}
