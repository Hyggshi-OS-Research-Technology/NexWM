/* hde-flyout.h — the pop-up panels of hde-panel (Control Center, battery): a frame next to the status area that closes
 * when you click elsewhere or press Esc, and takes the keyboard while it is open (Wi-Fi passwords, arrow keys).
 *
 * X11: a popup window that holds the pointer and the keyboard like a menu (no window manager involved, so it works
 * with every one of them). Wayland: a transparent layer-shell surface over the screen (minus the panel) with the frame
 * in its corner, keyboard exclusive while it is open.
 * The frame is right-aligned with the button that opened it, on the panel's side of the screen.
 */
#ifndef HDE_FLYOUT_H
#define HDE_FLYOUT_H

#include <gtk/gtk.h>

typedef struct _HdeFlyout HdeFlyout;

/* name: layer-shell namespace / window title / log prefix ("control center"); css_class: style class of the frame. */
HdeFlyout *hde_flyout_new(const char *name, const char *css_class);
void       hde_flyout_set_child(HdeFlyout *f, GtkWidget *child);
/* Opens it next to anchor (a widget on the panel; NULL or hidden: the right end of the panel). */
void       hde_flyout_show(HdeFlyout *f, GtkWidget *anchor, GtkWidget *panel);
void       hde_flyout_hide(HdeFlyout *f);
gboolean   hde_flyout_visible(HdeFlyout *f);
/* Called after it was hidden (click outside, Esc, hde_flyout_hide). */
void       hde_flyout_on_hide(HdeFlyout *f, void (*cb)(gpointer), gpointer data);
/* Esc: cb returns TRUE if it used the key (e.g. back from a sub-page), else the flyout closes. */
void       hde_flyout_on_escape(HdeFlyout *f, gboolean (*cb)(gpointer), gpointer data);
/* Where the frame is now, in screen coordinates (Wayland: relative to the screen it is on). */
void       hde_flyout_geometry(HdeFlyout *f, GdkRectangle *r);
/* The height there is for it between the panel and the other edge of the screen (minus margins). */
int        hde_flyout_max_height(HdeFlyout *f, GtkWidget *panel);
/* Give a widget inside the keyboard focus (also right after showing it). */
void       hde_flyout_focus(HdeFlyout *f, GtkWidget *w);
/* Keyboard and pointer: grabbed (X11) / exclusive (Wayland), for the logs. */
const char *hde_flyout_input_state(HdeFlyout *f);

/* ---- the panel's right-click menus ---- */
/* A menu item with an icon that is always shown (GtkImageMenuItem hides it when gtk-menu-images is off). icon: names
 * separated by '|', the first one the icon theme has is used; NULL: no icon (the label stays aligned). */
GtkWidget *hde_menu_item(const char *icon, const char *label);
/* The same as a check item / radio item (group: the previous radio item of the same group, or NULL; image: an icon
 * in front of the label, may be NULL). */
GtkWidget *hde_menu_check_item(const char *label, gboolean active);
GtkWidget *hde_menu_radio_item(GtkWidget *group_member, GtkWidget *image, const char *label, gboolean active);
/* An image for a menu item from icon names separated by '|' */
GtkWidget *hde_menu_icon(const char *icon);
/* A submenu item with an icon; returns the submenu to fill. */
GtkWidget *hde_menu_submenu(GtkWidget *menu, const char *icon, const char *label);
/* Pops the menu up at the pointer (event: the click, may be NULL) and destroys it once it is closed. */
void       hde_menu_popup(GtkWidget *menu, GtkWidget *widget, const GdkEvent *event);

#endif
