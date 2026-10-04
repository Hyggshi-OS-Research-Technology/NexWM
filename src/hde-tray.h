#ifndef HDE_TRAY_H
#define HDE_TRAY_H
#include <gtk/gtk.h>

/* Trả về hộp chứa các icon tray (XEmbed + StatusNotifierItem) */
GtkWidget *hde_tray_new(void);

#endif
