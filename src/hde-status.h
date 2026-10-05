#ifndef HDE_STATUS_H
#define HDE_STATUS_H
#include <gtk/gtk.h>

/* Khu vực trạng thái trên panel: Fcitx, Wi-Fi, Bluetooth, âm lượng, pin.
 * Mục nào không có phần cứng/dịch vụ tương ứng sẽ tự ẩn. */
GtkWidget *hde_status_new(void);

/* Cập nhật ngay (sau khi đổi âm lượng, bật/tắt Wi-Fi, ...). */
void hde_status_refresh(void);

/* Đọc mức âm lượng / trạng thái micro hiện tại rồi hiện OSD. */
void hde_status_osd_volume(void);
void hde_status_osd_mic(void);

/* Mở hde-settings (bản cạnh hde-panel trước, rồi tới PATH); page = NULL hoặc id trang. */
void hde_open_settings(const char *page);

#endif
