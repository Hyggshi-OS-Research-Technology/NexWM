#ifndef HDE_NOTIFY_H
#define HDE_NOTIFY_H
#include <gtk/gtk.h>

/* Trình nền thông báo theo chuẩn freedesktop (org.freedesktop.Notifications 1.2), chạy trong hde-panel:
 * popup góc dưới-phải, nút hành động, ảnh/icon, Do Not Disturb và âm báo theo ~/.config/hde/settings.ini
 * (dnd, notification_popups, notification_sounds). */
void hde_notify_init(void);

/* Nút chuông trên panel: lịch sử thông báo, bật/tắt Do Not Disturb, xoá tất cả. */
GtkWidget *hde_notify_button_new(void);

#endif
