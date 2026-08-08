/*
 * nex-settings.h - NexSettings header
 * GUI Settings control panel for NexWM / Hyggshi OS
 */

#ifndef NEX_SETTINGS_H
#define NEX_SETTINGS_H

typedef struct {
    int gaps;
    int workspace_count;
    char default_layout[32];
    int border_width;
    unsigned int border_focus;
    unsigned int border_normal;
} nex_settings_t;

int nex_settings_load(nex_settings_t *s, const char *path);
int nex_settings_save(const nex_settings_t *s, const char *path);
int nex_settings_apply_ipc(const nex_settings_t *s);

#endif
