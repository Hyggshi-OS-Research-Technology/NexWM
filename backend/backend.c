#include "hde/backend.h"
#include <stdlib.h>
#include <string.h>
extern HDEBackend *hde_backend_x11_create(void); extern HDEBackend *hde_backend_wayland_create(void);
/* The Wayland backend is still only a placeholder; desktop/panel are both GTK/X11.
 * Priority: HDE_BACKEND=x11|wayland  >  DISPLAY set -> X11 (Xephyr/XWayland)  >  only WAYLAND_DISPLAY -> Wayland.
 * Previously a WAYLAND_DISPLAY on the host was enough to pick Wayland, even when running inside Xephyr (DISPLAY=:2). */
HDEBackendType hde_backend_detect(void){
    const char *forced=getenv("HDE_BACKEND");
    if(forced&&!strcmp(forced,"x11"))return HDE_BACKEND_X11;
    if(forced&&!strcmp(forced,"wayland"))return HDE_BACKEND_WAYLAND;
    const char *d=getenv("DISPLAY"); if(d&&*d)return HDE_BACKEND_X11;
    const char *w=getenv("WAYLAND_DISPLAY"); if(w&&*w)return HDE_BACKEND_WAYLAND;
    return HDE_BACKEND_X11;
}
HDEBackend *hde_backend_create(HDEBackendType type){if(type==HDE_BACKEND_AUTO)type=hde_backend_detect(); if(type==HDE_BACKEND_WAYLAND)return hde_backend_wayland_create(); return hde_backend_x11_create();}
void hde_backend_destroy(HDEBackend*b){free(b);}
