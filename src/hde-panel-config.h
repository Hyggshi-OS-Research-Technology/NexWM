/* hde-panel-config.h — how the panel and the Start menu look, shared by hde-panel, hde-desktop (icons stay clear of
 * the panel), the popups (OSD, notifications, calendar) and hde-settings (Settings > Panel, Settings > Start Menu).
 * GLib only. Everything lives in ~/.config/hde/settings.ini, group [settings]; the panel follows changes live.
 *
 *   panel_position=bottom|top           panel_size=34 (24-64 px)          panel_opacity=100 (40-100 %)
 *   panel_show_menu / _desktop / _run / _launchers / _taskbar / _workspaces / _tray / _status / _notifications /
 *   _clock = true|false                 panel_taskbar_labels=true         panel_taskbar_group=auto|never|always
 *   clock_24h=true  clock_show_date=true  clock_show_seconds=false
 *   panel_launchers=firefox-esr.desktop;org.gnome.Terminal.desktop;      (pinned apps, desktop file ids)
 *   panel_applets=cpu;mem;                 (extensions; each one is a group [applet:ID], see HdeApplet)
 *   menu_style=modern|kickoff|classic      (modern: like Linux Mint's Cinnamon menu; kickoff: like KDE Plasma;
 *                                           classic: the old drop-down menu)
 *   menu_show_sidebar / _places / _favorites / _recent / _descriptions = true|false   menu_hover_switch=true
 *   menu_icon_size=32 (24|32|48)           menu_size=normal|compact|large
 *   menu_favorites=firefox-esr.desktop;... (missing: a sensible default list, see hde_menu_default_favorites)
 *   menu_button_label=Menu                 menu_button_icon=menu|os|hde|none|<icon name>
 */
#ifndef HDE_PANEL_CONFIG_H
#define HDE_PANEL_CONFIG_H

#include <glib.h>

#define HDE_PANEL_SIZE_DEFAULT 34
#define HDE_PANEL_SIZE_MIN 24
#define HDE_PANEL_SIZE_MAX 64

typedef enum { HDE_MENU_MODERN, HDE_MENU_KICKOFF, HDE_MENU_CLASSIC } HdeMenuStyle;

typedef struct {
    gboolean top;                   /* panel at the top of the screen (default: bottom) */
    int size;                       /* height in pixels */
    int opacity;                    /* percent; below 100 needs a compositing window manager */
    gboolean show_menu, show_desktop, show_run, show_launchers, show_taskbar, show_workspaces, show_tray,
             show_status, show_notifications, show_clock;
    gboolean taskbar_labels;        /* window titles next to the icons (FALSE: icons only) */
    int taskbar_group;              /* 0 never, 1 when space runs out, 2 always */
    gboolean clock_24h, clock_date, clock_seconds;
    char *menu_label;               /* text of the Start button ("" = icon only) */
    char *menu_icon;                /* "menu" (the ☰ sign), "os" (logo of the system), "hde", "none", or an icon name */
    char **launchers;               /* pinned apps (desktop file ids), never NULL */
    char **applets;                 /* extension ids, never NULL */

    HdeMenuStyle menu_style;
    gboolean menu_sidebar, menu_places, menu_favorites, menu_recent, menu_descriptions, menu_hover;
    int menu_icon_size;             /* 24, 32 or 48 */
    int menu_size;                  /* 0 compact, 1 normal, 2 large */
    char **favorites;               /* NULL: not chosen yet (use hde_menu_default_favorites) */
} HdePanelConfig;

void hde_panel_config_load(HdePanelConfig *c);
void hde_panel_config_clear(HdePanelConfig *c);
/* Space the panel takes at the top / bottom of the primary screen (0 for the other edge). */
void hde_panel_reserved(int *top, int *bottom);
const char *hde_menu_style_id(HdeMenuStyle s);          /* "modern", "kickoff", "classic" */
HdeMenuStyle hde_menu_style_from_id(const char *id);

/* Panel extensions ("applets"): small live items next to the tray.
 *   [applet:ID]  type=cpu|memory|command|separator   label=CPU   command=...   interval=5   click=...
 * cpu / memory are built in (no tool needed); command shows the first line a command prints (like Xfce's Generic
 * Monitor) and runs click= when clicked. */
typedef struct {
    char *id, *type, *label, *command, *click;
    int interval;                   /* seconds */
} HdeApplet;

int  hde_applets_load(HdeApplet **out);                 /* returns how many (free with hde_applets_free) */
void hde_applets_free(HdeApplet *a, int n);
/* Add an extension (new [applet:ID] group + panel_applets) or replace the one with the same id; NULL id = new one.
 * Returns the id (g_free). */
char *hde_applet_save(const HdeApplet *a);
void  hde_applet_remove(const char *id);

/* Lists of desktop file ids (pinned apps, favorites): read / write one key of [settings] (atomically). */
char   **hde_cfg_get_list(const char *key);             /* never NULL (g_strfreev) */
gboolean hde_cfg_has_key(const char *key);
void     hde_cfg_set_list(const char *key, const char *const *list);
gboolean hde_cfg_list_contains(const char *key, const char *id);
void     hde_cfg_list_add(const char *key, const char *id);        /* at the end, once */
void     hde_cfg_list_remove(const char *key, const char *id);
void     hde_cfg_list_move(const char *key, const char *id, int delta);
void     hde_cfg_set_string(const char *key, const char *value);   /* NULL removes the key */
void     hde_cfg_set_bool(const char *key, gboolean value);

/* Default favorites of the Start menu: web browser, files, terminal, settings, software center, text editor (the
 * ones installed). Returns desktop file ids (g_strfreev). Needs GIO (it is in libgio, always there with GTK). */
char **hde_menu_default_favorites(void);
/* Favorites in use: menu_favorites, or the defaults while the user has not changed them. */
char **hde_menu_favorites(void);

#endif
