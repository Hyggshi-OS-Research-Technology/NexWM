#ifndef HDE_OSD_H
#define HDE_OSD_H
#include <gtk/gtk.h>

/* Khung OSD nhỏ ở giữa phía dưới màn hình (âm lượng / độ sáng / micro), tự ẩn sau ~1.5s.
 * percent < 0: không vẽ thanh mức, chỉ hiện icon + text. */
void hde_osd_show(const char *icon_name, int percent, const char *text);

/* Cửa sổ popup có góc bo tròn trong suốt khi WM có compositing (thêm lớp CSS "rounded").
 * Phải gọi trước khi cửa sổ được realize. */
void hde_popup_setup_alpha(GtkWidget *window);

#endif
