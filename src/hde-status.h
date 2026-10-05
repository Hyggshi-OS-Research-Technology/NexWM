#ifndef HDE_STATUS_H
#define HDE_STATUS_H
#include <gtk/gtk.h>

/* Status area of the panel: Fcitx, Wi-Fi, Bluetooth, volume, battery.
 * Items without the matching hardware/service hide themselves. */
GtkWidget *hde_status_new(void);

/* Refresh now (after changing the volume, toggling Wi-Fi, ...). */
void hde_status_refresh(void);

/* Read the current volume level / microphone state and show the OSD. */
void hde_status_osd_volume(void);
void hde_status_osd_mic(void);

/* Open hde-settings (the copy next to hde-panel first, then PATH); page = NULL or a page id. */
void hde_open_settings(const char *page);

#endif
