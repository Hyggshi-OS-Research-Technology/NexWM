#ifndef HDE_CORE_H
#define HDE_CORE_H
/* hde-core: backend initialization (X11/Wayland) shared by hde-session and the CLI tools. */
#include "hde/backend.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize settings + backend. Safe to call several times. Returns 0 on success. */
int hde_core_init(HDEBackendType type);
/* The backend in use, or NULL if not initialized / initialization failed. */
HDEBackend *hde_core_backend(void);
void hde_core_shutdown(void);

#ifdef __cplusplus
}
#endif
#endif
