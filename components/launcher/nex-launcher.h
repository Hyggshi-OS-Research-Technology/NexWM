/*
 * nex-launcher.h - NexLauncher header
 * Application launcher for NexWM
 */

#ifndef NEX_LAUNCHER_H
#define NEX_LAUNCHER_H

#define NEX_LAUNCHER_MAX_APPS  1024
#define NEX_LAUNCHER_MAX_PATH   512

typedef struct {
    char name[128];       /* Application display name */
    char exec[512];       /* Exec= value from .desktop */
    char icon[256];       /* Icon= value (name or path) */
    char categories[256]; /* Categories= value */
    char comment[256];    /* Comment= for tooltip */
    int  terminal;        /* Terminal=true */
} nex_app_t;

typedef struct {
    nex_app_t apps[NEX_LAUNCHER_MAX_APPS];
    int       count;
} nex_app_list_t;

int  nex_launcher_scan(nex_app_list_t *list);
void nex_launcher_filter(const nex_app_list_t *in, const char *query,
                         nex_app_list_t *out);
int  nex_launcher_launch(const nex_app_t *app);

#endif
