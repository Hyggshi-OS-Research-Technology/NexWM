/* hde-settings.h — phần dùng chung giữa các trang của Hyggshi Settings. */
#ifndef HDE_SETTINGS_APP_H
#define HDE_SETTINGS_APP_H

#include <gtk/gtk.h>

#define CONFIG_GROUP "settings"

/* ---- cấu hình ~/.config/hde/settings.ini ----
 * Mỗi lần ghi đều đọc lại file trước, nên không đè mất thay đổi do hde-panel (vd. Do Not Disturb)
 * hay tiến trình khác ghi trong lúc Settings đang mở. */
gboolean cfg_get_bool(const char *key, gboolean fallback);
int      cfg_get_int(const char *key, int fallback);
double   cfg_get_double(const char *key, double fallback);
char    *cfg_get_string(const char *key, const char *fallback);   /* g_free */
gboolean cfg_has_key(const char *key);
void     cfg_set_bool(const char *key, gboolean value);
void     cfg_set_int(const char *key, int value);
void     cfg_set_double(const char *key, double value);
void     cfg_set_string(const char *key, const char *value);
/* Ghi nhiều khoá trong một lần (một lần ghi file -> các tiến trình theo dõi chỉ nạp lại một lần). */
GKeyFile *cfg_begin(void);
void      cfg_commit(GKeyFile *kf);     /* lưu + giải phóng */

/* ---- giao diện ---- */
GtkWidget *settings_window(void);
void       settings_status(const char *fmt, ...) G_GNUC_PRINTF(1, 2);
GtkWidget *page_base(void);
GtkWidget *section(const char *title);
GtkWidget *row_box(const char *title, const char *description, GtkWidget *control);
GtkWidget *info_label(const char *text);
GtkWidget *card_new(void);                 /* GtkListBox dạng thẻ, không chọn hàng */
void       card_clear(GtkWidget *card);
GtkWidget *card_placeholder(const char *text);
GtkWidget *icon_button(const char *icon_name, const char *tooltip);
void       message_dialog(GtkMessageType type, const char *title, const char *detail);
gboolean   have_program(const char *name);
void       launch_candidates(const char *const *commands);

/* ---- chạy lệnh bất đồng bộ (không bao giờ chặn giao diện) ---- */
typedef void (*SettingsRunCb)(gboolean ok, int exit_status, const char *out, const char *err, gpointer data);
/* stdin_text != NULL: ghi vào stdin của tiến trình (vd. mật khẩu cho `nmcli --ask`) */
void run_argv_async(const char *const *argv, const char *stdin_text, int timeout_sec, SettingsRunCb cb, gpointer data);
void run_shell_async(const char *script, SettingsRunCb cb, gpointer data);

/* ---- các trang ---- */
GtkWidget *page_network_new(void);
GtkWidget *page_bluetooth_new(void);
GtkWidget *page_appearance_new(void);
GtkWidget *page_windows_new(void);
GtkWidget *page_keyboard_new(void);
GtkWidget *page_sound_new(void);

/* Áp các thiết lập cần chạy lại mỗi lần đăng nhập (bàn phím, lặp phím, tắt màn hình, ...).
 * Gọi bởi `hde-settings --apply` (hde-session chạy khi khởi động) và khi người dùng đổi giá trị. */
void apply_keyboard_settings(void);
/* Đổi Light/Dark cho toàn hệ thống (dùng bởi trang Appearance và `hde-settings --style dark|light`). */
void appearance_apply_style(gboolean dark);
void apply_power_settings(void);
void apply_input_settings(void);

#endif
