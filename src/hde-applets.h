/* hde-applets.h — what Settings > Panel adds to the panel besides the fixed items:
 *   pinned apps (panel_launchers): one button per app; right-click: a new window, app actions and recent files,
 *     then Unpin / Move left / Move right
 *   extensions (panel_applets, see HdeApplet in hde-panel-config.h): CPU and memory use (built in, no tool needed),
 *   the output of a command refreshed every few seconds (click runs another command), separators. */
#ifndef HDE_APPLETS_H
#define HDE_APPLETS_H

#include <gtk/gtk.h>

/* Start an application from its .desktop id (what the pinned apps, the taskbar's menu and the Start menu share). */
void       hde_launch_app(const char *desktop_id);
/* Add the application's new-window command, .desktop actions and recent files to an open taskbar menu. */
gboolean   hde_app_menu_append(GtkMenuShell *menu, const char *desktop_id);
GtkWidget *hde_launchers_new(void);
/* rebuild the buttons for these desktop file ids (icon_size: pixels) */
void       hde_launchers_update(GtkWidget *box, char **ids, int icon_size);
/* the pinned apps were changed by someone else (the menu of a taskbar button): read them again and build the
 * box from them at once */
void       hde_launchers_refresh(void);

GtkWidget *hde_applets_new(void);
/* rebuild from settings.ini (panel_applets + [applet:ID] groups) */
void       hde_applets_update(GtkWidget *box);

void       hde_applets_set_debug(gboolean on);
/* The battery saver: the extensions refresh 3 times less often while it is on. */
void       hde_applets_set_slow(gboolean slow);

#endif
