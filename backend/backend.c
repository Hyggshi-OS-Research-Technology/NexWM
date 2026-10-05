#include "hde/backend.h"
#include <stdlib.h>
#include <string.h>
extern HDEBackend *hde_backend_x11_create(void); extern HDEBackend *hde_backend_wayland_create(void);
/* Backend Wayland hiện mới là placeholder, còn desktop/panel đều là GTK/X11.
 * Ưu tiên: HDE_BACKEND=x11|wayland  >  có DISPLAY thì X11 (Xephyr/XWayland)  >  chỉ có WAYLAND_DISPLAY thì Wayland.
 * Trước đây chỉ cần host có WAYLAND_DISPLAY là chọn Wayland, dù đang chạy trong Xephyr (DISPLAY=:2). */
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
