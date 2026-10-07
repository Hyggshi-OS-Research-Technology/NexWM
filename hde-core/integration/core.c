#include "hde/core.h"
#include "hde/settings.h"
#include <stdio.h>

static HDEBackend *g_backend;

int hde_core_init(HDEBackendType type)
{
    if (g_backend) return 0;
    hde_settings_init();
    g_backend = hde_backend_create(type);
    if (!g_backend) {
        fprintf(stderr, "hde-core: cannot create backend\n");
        return -1;
    }
    if (g_backend->init && g_backend->init(g_backend) != 0) {
        fprintf(stderr, "hde-core: backend '%s' init failed\n", g_backend->name ? g_backend->name : "?");
        hde_backend_destroy(g_backend);
        g_backend = NULL;
        return -1;
    }
    return 0;
}

HDEBackend *hde_core_backend(void)
{
    return g_backend;
}

void hde_core_shutdown(void)
{
    if (!g_backend) return;
    if (g_backend->shutdown) g_backend->shutdown(g_backend);
    hde_backend_destroy(g_backend);
    g_backend = NULL;
    hde_settings_shutdown();
}
