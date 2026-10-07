#ifndef HDE_STATUS_H
#define HDE_STATUS_H
#include <gtk/gtk.h>
#include "hde-run.h"                /* hde_open_settings() used to live here */

/* Status area of the panel: Fcitx, Wi-Fi, Bluetooth, volume, battery.
 * Items without the matching hardware/service hide themselves. */
GtkWidget *hde_status_new(void);

/* What the clicks open (hde-panel.c): the Control Center at a page (HdeCcPage) next to the icon, the battery panel.
 * Without them: the old actions (settings pages, mute). */
typedef struct {
    void (*control_center)(GtkWidget *anchor, int page);
    void (*battery)(GtkWidget *anchor);
} HdeStatusActions;
void hde_status_set_actions(const HdeStatusActions *actions);

/* Refresh now (after changing the volume, toggling Wi-Fi, ...). */
void hde_status_refresh(void);

/* Read the current volume level / microphone state and show the OSD. */
void hde_status_osd_volume(void);
void hde_status_osd_mic(void);

#endif
