/* hde-osinfo.h — which system HDE runs on (/etc/os-release) and its logo, for Settings > About, the About window
 * and the Start button.
 *
 * The logo of ID= (ubuntu, debian, hyggshios, linuxmint, ...) is looked up in this order:
 *   1. the icon named by LOGO= (Ubuntu: ubuntu-logo, Hyggshi OS: distributor-logo, Fedora: fedora-logo-icon)
 *   2. icons the system or the icon theme installs: distributor-logo-<id>, <id>-logo, <id>-logo-icon,
 *      start-here-<id>, emblem-<id>; then /usr/share/pixmaps/<LOGO>.png, <id>-logo.png / .svg
 *   3. HDE's own copy: $(PREFIX)/share/hde/logos/<id>.svg (data/logos, drawn by HDE itself: no SVG library needed)
 *      and the Hyggshi OS logo, drawn in code
 *   4. the same for each entry of ID_LIKE (a derivative without a logo of its own shows its base's logo)
 *   5. a round badge with the first letter of NAME, in the colour of ANSI_COLOR
 * HDE_OS_RELEASE=<file> reads another os-release file (tests, screenshots of other systems).
 */
#ifndef HDE_OSINFO_H
#define HDE_OSINFO_H

#include <gtk/gtk.h>

typedef struct {
    char *id, *id_like, *name, *pretty_name, *version, *version_id, *codename, *variant, *build_id;
    char *home_url, *support_url, *bug_url, *logo, *ansi_color;
    char *base_codename;            /* HYGGSHI_BASE_CODENAME, UBUNTU_CODENAME or DEBIAN_CODENAME */
    char *source;                   /* the file that was read (NULL: none found) */
} HdeOsInfo;

void  hde_os_info_load(HdeOsInfo *os);
void  hde_os_info_clear(HdeOsInfo *os);
/* "Hyggshi OS 1.0 "Sen Vàng"", "Ubuntu 24.04.1 LTS", "Debian GNU/Linux 13 (trixie)" (g_free) */
char *hde_os_title(const HdeOsInfo *os);
/* "Debian 13 (trixie)", "Ubuntu 24.04 LTS (noble)" for a derivative, NULL otherwise. base_id: "debian" (g_free) */
char *hde_os_base_text(const HdeOsInfo *os, char **base_id);

/* The logo, size x size logical pixels (scale: HiDPI factor). how (optional): where it came from, for logs:
 * "icon theme: distributor-logo", "HDE: data/logos/debian.svg", "built in: hyggshios", "badge: H" (g_free). */
cairo_surface_t *hde_os_logo_surface(const HdeOsInfo *os, int size, int scale, gboolean dark, char **how);
/* The logo of one distribution id (steps 2-3 and 5): the "Based on" logo. */
cairo_surface_t *hde_distro_logo_surface(const char *id, const char *name, int size, int scale, gboolean dark,
                                         char **how);
/* HDE's own logo (rounded square in the accent colour, a window and an H). */
void hde_draw_hde_logo(cairo_t *cr, double x, double y, double size, const GdkRGBA *accent);
cairo_surface_t *hde_hde_logo_surface(int size, int scale, const char *accent);

/* A file of HDE's data directory ($(PREFIX)/share/hde, or data/ next to a fresh build), NULL if missing (g_free). */
char *hde_data_file(const char *rel);

#endif
