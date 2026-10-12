/* hde-wltaskbar.h — the taskbar of hde-panel on Wayland, through wlr-foreign-toplevel-management (labwc, sway,
 * Wayfire, ...; libwnck only works on X11). One button per window with the app's icon and title: click to bring it
 * forward (or minimize it if it is in front), middle-click to close, right-click: Minimize / Maximize / Close.
 * Resting the pointer on a button opens the preview (src/hde-peek.h): on Wayland there is no picture of the
 * window — no program may read what another window shows — so it is the icon and the title that are shown. */
#ifndef HDE_WLTASKBAR_H
#define HDE_WLTASKBAR_H

#include <gtk/gtk.h>

/* NULL if the compositor does not offer the protocol */
GtkWidget *hde_wl_taskbar_new(void);
void       hde_wl_taskbar_set_labels(GtkWidget *taskbar, gboolean labels);
/* The preview on hover: off (the old tooltips) or on, and how long the pointer rests before it opens. */
void       hde_wl_taskbar_set_preview(GtkWidget *taskbar, gboolean on, int delay_ms);
/* minimize every window; again: bring back the ones minimized that way (Show Desktop, Super+D) */
void       hde_wl_taskbar_show_desktop(void);

#endif
