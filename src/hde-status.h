#ifndef HDE_STATUS_H
#define HDE_STATUS_H
#include <gtk/gtk.h>

/* Khu vực trạng thái trên panel: Fcitx, Wi-Fi, Bluetooth, âm lượng, pin.
 * Mục nào không có phần cứng/dịch vụ tương ứng sẽ tự ẩn. */
GtkWidget *hde_status_new(void);

#endif
