#ifndef HDE_BACKEND_H
#define HDE_BACKEND_H
#ifdef __cplusplus
extern "C" {
#endif

typedef enum { HDE_BACKEND_AUTO=0, HDE_BACKEND_X11, HDE_BACKEND_WAYLAND } HDEBackendType;
typedef struct HDEBackend HDEBackend;
struct HDEBackend {
    HDEBackendType type;
    const char *name;
    int (*init)(HDEBackend *self);
    void (*shutdown)(HDEBackend *self);
    int (*launch)(HDEBackend *self, const char *command);
    int (*logout)(HDEBackend *self);
    int (*reboot)(HDEBackend *self);
    int (*poweroff)(HDEBackend *self);
    int (*suspend)(HDEBackend *self);
    int (*lock)(HDEBackend *self);
    void *private_data;
};

HDEBackend *hde_backend_create(HDEBackendType type);
void hde_backend_destroy(HDEBackend *backend);
HDEBackendType hde_backend_detect(void);

#ifdef __cplusplus
}
#endif
#endif
