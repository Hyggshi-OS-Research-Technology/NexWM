#ifndef HDE_TRAY_H
#define HDE_TRAY_H
#include <gtk/gtk.h>

/* Returns the box holding the tray icons (XEmbed + StatusNotifierItem) */
GtkWidget *hde_tray_new(void);

#endif
