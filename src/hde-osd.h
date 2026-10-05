#ifndef HDE_OSD_H
#define HDE_OSD_H
#include <gtk/gtk.h>

/* Small OSD box at the bottom center of the screen (volume / brightness / microphone), hides itself after ~1.5s.
 * percent < 0: no level bar, only icon + text. */
void hde_osd_show(const char *icon_name, int percent, const char *text);

/* Popup window with transparent rounded corners when the WM composites (adds the CSS class "rounded").
 * Must be called before the window is realized. */
void hde_popup_setup_alpha(GtkWidget *window);

#endif
