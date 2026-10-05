/* hde-theme.h — Dark mode / theme dùng chung cho hde-panel, hde-desktop và hde-settings.
 *
 * Nguồn cấu hình duy nhất: ~/.config/hde/settings.ini, nhóm [settings]
 *   theme_index          1 = Light, 2 = Dark (0/thiếu = chưa chọn: HDE không đụng vào theme GTK)
 *   gtk_theme            theme gốc người dùng chọn (vd. Adwaita, Yaru, Arc)
 *   gtk_theme_effective  tên theme thực dùng sau khi chọn biến thể tối (vd. Adwaita-dark)
 *   icon_theme_name      theme icon (tuỳ chọn)
 *   font                 font giao diện (tuỳ chọn, vd. "Sans 10")
 *   accent               màu nhấn (#rrggbb)
 *
 * hde-settings ghi các khoá này + ~/.config/gtk-3.0/settings.ini + GSettings; hde-xsettings phát
 * chúng qua XSETTINGS để MỌI ứng dụng GTK đang chạy đổi theme ngay; các thành phần HDE theo dõi
 * settings.ini bằng hde_theme_watch() để tự đổi CSS sáng/tối.
 */
#ifndef HDE_THEME_H
#define HDE_THEME_H

#include <gtk/gtk.h>

typedef enum {
    HDE_STYLE_DEFAULT = 0,
    HDE_STYLE_LIGHT = 1,
    HDE_STYLE_DARK = 2
} HdeStyle;

typedef struct {
    HdeStyle style;
    char *gtk_theme;        /* theme đã resolve, NULL = không quản lý */
    char *base_theme;       /* theme gốc (gtk_theme), có thể NULL */
    char *icon_theme;       /* NULL = không đổi */
    char *font;             /* NULL = không đổi */
    char *accent;           /* luôn khác NULL */
} HdeThemeInfo;

char    *hde_settings_ini_path(void);
void     hde_theme_info_load(HdeThemeInfo *info);
void     hde_theme_info_clear(HdeThemeInfo *info);

/* Panel / OSD / popup thông báo dùng giao diện tối, trừ khi người dùng chọn Light. */
gboolean hde_theme_shell_dark(void);

/* Áp theme cho chính tiến trình GTK hiện tại (GtkSettings). */
void     hde_theme_apply_process(void);

typedef void (*HdeThemeChangedFunc)(gpointer user_data);
/* Theo dõi settings.ini; khi đổi: áp lại theme cho tiến trình rồi gọi func (có thể NULL). */
void     hde_theme_watch(HdeThemeChangedFunc func, gpointer user_data);

/* --- dùng bởi hde-settings --- */
/* Tên theme thực dùng cho style + theme gốc; tạo shim Adwaita-dark nếu máy chưa có. */
char    *hde_theme_resolve_name(HdeStyle style, const char *base);
/* Theme GTK hiện tại theo ~/.config/gtk-3.0/settings.ini (hoặc "Adwaita"). */
char    *hde_theme_current_gtk3(void);
/* Ghi ~/.config/gtk-3.0/settings.ini (+ gtk-4.0 cho icon/font) và GSettings org.gnome.desktop.interface. */
void     hde_theme_write_system(const HdeThemeInfo *info);
/* Danh sách theme GTK3 (chỉ theme gốc, bỏ biến thể -dark) và theme icon đã cài; g_strfreev. */
gchar  **hde_theme_list_gtk_themes(void);
gchar  **hde_theme_list_icon_themes(void);
/* TRUE nếu tên theme trông như biến thể tối (…-dark, …-Dark, …-darker). */
gboolean hde_theme_name_is_dark(const char *name);

#endif
