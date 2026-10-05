#ifndef HDE_SEARCH_H
#define HDE_SEARCH_H
#include <gtk/gtk.h>

/* Ô tìm ứng dụng (mở từ Start menu: gõ phím bất kỳ, mục "Search", hoặc `hde-panel --search`).
 * Tìm theo tên / tên chung / từ khoá / lệnh (g_desktop_app_info_search), Enter để mở,
 * gõ một lệnh có trong PATH để chạy trực tiếp. time = timestamp X (0 = lấy từ server). */
void hde_search_show(const char *initial_text, guint32 time);
gboolean hde_search_visible(void);
void hde_search_hide(void);

/* Kích hoạt (focus) cửa sổ theo kiểu pager (_NET_ACTIVE_WINDOW source=2) — WM luôn chấp nhận. */
void hde_window_force_activate(GtkWidget *window, guint32 time);

#endif
