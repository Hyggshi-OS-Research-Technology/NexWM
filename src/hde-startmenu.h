/* hde-startmenu.h — the Start menu of hde-panel in two layouts (Settings > Start Menu, menu_style=):
 *
 *   modern   like Linux Mint's Cinnamon menu: on the left your picture and name, the places (Home, Documents, ...),
 *            your favorite apps and the lock / log out / power buttons; on the right a search box over the
 *            categories and the apps of the category under the mouse, each with its description.
 *   kickoff  like KDE Plasma's Kickoff: your picture, name and the search box on top, categories on the left with
 *            Favorites as a grid of tiles, the "Applications" and "Places" tabs and Sleep / Restart / Shut Down below.
 *
 * (menu_style=classic is the old drop-down GtkMenu, which stays in hde-panel.c.)
 * Typing searches at once (names, descriptions, keywords and commands, accents optional); Up/Down/Left/Right, Enter
 * and Esc work as expected; right-click an app: Add to / Remove from Favorites, Pin to Panel, Add to Desktop.
 * On X11 it is a popup window holding the keyboard and mouse like a menu; on Wayland a layer-shell surface.
 */
#ifndef HDE_STARTMENU_H
#define HDE_STARTMENU_H

#include <gtk/gtk.h>
#include "hde-panel-config.h"

/* What the menu cannot do itself (in hde-panel.c). power(): 0 = the Session / Power dialog, 1 log out, 2 restart,
 * 3 shut down, 4 suspend, 5 lock (2 and 3 ask first). */
typedef struct {
    void (*power)(int action);
    void (*open_settings)(const char *page);
} HdeMenuActions;

void     hde_startmenu_init(const HdeMenuActions *actions, gboolean debug);
/* Settings changed: rebuilt (if needed) before it opens the next time. */
void     hde_startmenu_set_config(const HdePanelConfig *cfg);
/* anchor: the Start button (may be hidden), panel: the panel window. initial_text: typed already (may be NULL). */
void     hde_startmenu_show(GtkWidget *anchor, GtkWidget *panel, guint32 time, const char *initial_text);
void     hde_startmenu_hide(void);
gboolean hde_startmenu_visible(void);
void     hde_startmenu_toggle(GtkWidget *anchor, GtkWidget *panel, guint32 time);
/* CSS of the menu in the panel's colours (added to the panel's style sheet). */
char    *hde_startmenu_css(gboolean dark, const char *accent);

#endif
