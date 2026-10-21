/* hde-wallpaper.h — where the wallpaper comes from, shared by hde-desktop and Hyggshi Settings.
 * GLib only (no GTK, no display), so the rules can be checked by `make check-unit`.
 *
 * A wallpaper value may be:
 *   - a picture:                    /usr/share/backgrounds/warty-final-ubuntu.png
 *   - a shell-style pattern:        *.png in /usr/share/backgrounds     (the first match, sorted, is used)
 *   - GNOME's background file:      /usr/share/desktop-base/active-theme/wallpaper/gnome-background.xml
 *     (the <size width= height=> entry closest to the screen is picked, so the right variant is used)
 *
 * HDE keeps the source value in ~/.config/hde/desktop.ini:
 *
 *   [background-desktop]
 *   wallpaper=/usr/share/desktop-base/active-theme/wallpaper/gnome-background.xml
 *
 * The source (not the resolved picture) is stored there, so a theme update or a screen change is followed at the
 * next login. When no wallpaper was ever chosen, HDE writes that file itself and uses the system's active theme.
 */
#ifndef HDE_WALLPAPER_H
#define HDE_WALLPAPER_H

#include <glib.h>

/* ~/.config/hde/desktop.ini (or $HDE_DESKTOP_INI when set, for tests and for a session of one's own). */
char *hde_wallpaper_desktop_ini_path(void);

/* The raw `[background-desktop] wallpaper=` value, or NULL. Free with g_free(). */
char *hde_wallpaper_desktop_ini_get(const char *ini_path);

/* Write `[background-desktop] wallpaper=value` (the rest of the file is kept). No write when it is already that
 * value. Returns TRUE on success. The parent directory is created when needed. */
gboolean hde_wallpaper_desktop_ini_write(const char *ini_path, const char *value);

/* What the system offers when nothing was chosen: the active desktop-base theme's gnome-background.xml, else the
 * first picture of the user's background folder, then /usr/share/backgrounds, /usr/share/wallpapers and Xfce's
 * backdrops (as a glob such as *.png in /usr/share/backgrounds). Free with g_free(). */
char *hde_wallpaper_system_source(void);

/* Turn a value (picture, glob, XML) into the picture to show for a screen of width x height. Checks that the file
 * exists; returns NULL when nothing usable is there. Free with g_free(). */
char *hde_wallpaper_resolve(const char *value, int width, int height);

/* Best picture of a GNOME gnome-background.xml for a screen of width x height. Exposed for the unit test. */
char *hde_wallpaper_from_gnome_xml(const char *xml_path, int width, int height);

/* First regular file matching a shell-style pattern (sorted), or NULL. Exposed for the unit test. */
char *hde_wallpaper_expand_glob(const char *pattern);

#endif /* HDE_WALLPAPER_H */
