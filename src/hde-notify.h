#ifndef HDE_NOTIFY_H
#define HDE_NOTIFY_H
#include <gtk/gtk.h>

/* freedesktop notification daemon (org.freedesktop.Notifications 1.2), running inside hde-panel:
 * popups in the bottom-right corner, action buttons, images/icons, Do Not Disturb and sounds per ~/.config/hde/settings.ini
 * (dnd, notification_popups, notification_sounds). */
void hde_notify_init(void);

/* Bell button on the panel: notification history, Do Not Disturb toggle, clear all. */
GtkWidget *hde_notify_button_new(void);

#endif
