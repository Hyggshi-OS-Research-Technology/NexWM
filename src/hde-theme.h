/* hde-theme.h — Dark mode / theme shared by hde-panel, hde-desktop and hde-settings.
 *
 * Single source of configuration: ~/.config/hde/settings.ini, group [settings]
 *   theme_index          1 = Light, 2 = Dark (0/missing = not chosen: HDE leaves the GTK theme alone)
 *   gtk_theme            base theme chosen by the user (e.g. Adwaita, Yaru, Arc)
 *   gtk_theme_effective  theme actually used once the dark variant is picked (e.g. Adwaita-dark)
 *   icon_theme_name      icon theme (optional)
 *   font                 UI font (optional, e.g. "Sans 10")
 *   accent               accent color (#rrggbb)
 *
 * hde-settings writes these keys + ~/.config/gtk-3.0/settings.ini + GSettings; hde-xsettings publishes
 * them over XSETTINGS so that EVERY running GTK application switches theme immediately; HDE components
 * watch settings.ini with hde_theme_watch() to switch their own light/dark CSS.
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
    char *gtk_theme;        /* resolved theme, NULL = not managed */
    char *base_theme;       /* base theme (gtk_theme), may be NULL */
    char *icon_theme;       /* NULL = unchanged */
    char *font;             /* NULL = unchanged */
    char *accent;           /* never NULL */
} HdeThemeInfo;

char    *hde_settings_ini_path(void);
void     hde_theme_info_load(HdeThemeInfo *info);
void     hde_theme_info_clear(HdeThemeInfo *info);

/* Panel / OSD / notification popups use the dark look unless the user picked Light. */
gboolean hde_theme_shell_dark(void);

/* Apply the theme to the current GTK process itself (GtkSettings). */
void     hde_theme_apply_process(void);

typedef void (*HdeThemeChangedFunc)(gpointer user_data);
/* Watch settings.ini; on change: re-apply the theme to this process, then call func (may be NULL). */
void     hde_theme_watch(HdeThemeChangedFunc func, gpointer user_data);

/* --- used by hde-settings --- */
/* Effective theme name for a style + base theme; creates an Adwaita-dark shim if the system has none. */
char    *hde_theme_resolve_name(HdeStyle style, const char *base);
/* Current GTK theme according to ~/.config/gtk-3.0/settings.ini (or "Adwaita"). */
char    *hde_theme_current_gtk3(void);
/* Write ~/.config/gtk-3.0/settings.ini (+ gtk-4.0 for icons/font) and GSettings org.gnome.desktop.interface. */
void     hde_theme_write_system(const HdeThemeInfo *info);
/* Installed GTK3 themes (base themes only, -dark variants skipped) and icon themes; free with g_strfreev. */
gchar  **hde_theme_list_gtk_themes(void);
gchar  **hde_theme_list_icon_themes(void);
/* TRUE if the theme name looks like a dark variant (…-dark, …-Dark, …-darker). */
gboolean hde_theme_name_is_dark(const char *name);

#endif
