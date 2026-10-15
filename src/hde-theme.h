/* hde-theme.h — Dark mode / theme shared by hde-panel, hde-desktop and hde-settings.
 *
 * Single source of configuration: ~/.config/hde/settings.ini, group [settings]
 *   theme_index          1 = Light, 2 = Dark (0/missing = not chosen: HDE leaves the GTK theme alone)
 *   gtk_theme            base theme chosen by the user (e.g. Adwaita, Yaru, Arc)
 *   gtk_theme_effective  theme actually used once the dark variant is picked (e.g. Adwaita-dark)
 *   icon_theme_name      icon theme (optional)
 *   font                 UI font (optional, e.g. "Sans 10")
 *   accent               accent color (#rrggbb), or "auto" / missing = Automatic: the selection colour of the GTK
 *                        theme (Yaru: orange, Adwaita: blue), so that HDE's highlights match what the theme draws
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
    char *accent;           /* never NULL: #rrggbb */
    gboolean accent_auto;   /* accent = the colour of the GTK theme ("Automatic", the default) */
    gboolean enable_animations; /* TRUE by default: desktop, window & GTK widget animations */
    int animation_speed;    /* 0 = Off, 1 = Fast (100ms), 2 = Normal (160ms), 3 = Slow (260ms) */
} HdeThemeInfo;

char    *hde_settings_ini_path(void);
void     hde_theme_info_load(HdeThemeInfo *info);
void     hde_theme_info_clear(HdeThemeInfo *info);
gboolean hde_theme_animations_enabled(void);
int      hde_theme_animation_duration_ms(void);

/* The selection colour of the GTK theme in use (theme_selected_bg_color) as #rrggbb, made dark enough for white text
 * on it; NULL when GTK has no display. hde_theme_info_load() uses it for the Automatic accent once the process has
 * called hde_theme_apply_process() (before that, and in programs without a display: #3584e4). */
char    *hde_theme_accent_from_gtk(void);
/* CSS giving the standard GTK widgets (switches, sliders, progress bars, selected rows, suggested buttons) the accent
 * colour chosen in Settings instead of the theme's own; "" for the Automatic accent (the theme's colour already).
 * Free with g_free. */
char    *hde_theme_accent_css(const HdeThemeInfo *info);

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

#define HDE_BUILTIN_ICON_THEME "Hyggshi-Icons"

/* TRUE if the OS has at least one system icon theme installed (e.g. Adwaita, Papirus, breeze, Yaru, Mint-Y). */
gboolean    hde_theme_has_system_icon_theme(void);
/* TRUE if a specific icon theme name is installed on the system or user icon directories. */
gboolean    hde_theme_icon_theme_exists(const char *name);
/* Best available icon theme on this OS (prefers Adwaita, Papirus, Yaru, breeze, or falls back to Hyggshi-Icons); free with g_free. */
char       *hde_theme_best_icon_theme(void);
/* Ensure the built-in Hyggshi-Icons SVG icon theme is installed in $XDG_DATA_HOME/icons/Hyggshi-Icons; returns its name. */
const char *hde_theme_ensure_icon_theme(void);
/* Automatically provision fallback icons and install an OS icon theme package (e.g. "adwaita-icon-theme",
 * "papirus-icon-theme", "breeze-icon-theme") when the OS lacks an icon theme or when requested by the user. */
gboolean    hde_theme_auto_install_icon_theme(const char *pkg, gboolean interactive, char **status_out);

#endif
