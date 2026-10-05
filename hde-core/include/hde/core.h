#ifndef HDE_CORE_H
#define HDE_CORE_H
/* hde-core: khởi tạo backend (X11/Wayland) dùng chung cho hde-session và các tiện ích CLI. */
#include "hde/backend.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Khởi tạo settings + backend. Gọi nhiều lần cũng an toàn. Trả về 0 nếu thành công. */
int hde_core_init(HDEBackendType type);
/* Backend đang dùng, hoặc NULL nếu chưa init / init lỗi. */
HDEBackend *hde_core_backend(void);
void hde_core_shutdown(void);

#ifdef __cplusplus
}
#endif
#endif
