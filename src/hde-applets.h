/* hde-applets.h — what Settings > Panel adds to the panel besides the fixed items:
 *   pinned apps (panel_launchers): one button per app, right-click: Unpin / Move left / Move right
 *   extensions (panel_applets, see HdeApplet in hde-panel-config.h): CPU and memory use (built in, no tool needed),
 *   the output of a command refreshed every few seconds (click runs another command), separators. */
#ifndef HDE_APPLETS_H
#define HDE_APPLETS_H

#include <gtk/gtk.h>

GtkWidget *hde_launchers_new(void);
/* rebuild the buttons for these desktop file ids (icon_size: pixels) */
void       hde_launchers_update(GtkWidget *box, char **ids, int icon_size);

GtkWidget *hde_applets_new(void);
/* rebuild from settings.ini (panel_applets + [applet:ID] groups) */
void       hde_applets_update(GtkWidget *box);

void       hde_applets_set_debug(gboolean on);

#endif
