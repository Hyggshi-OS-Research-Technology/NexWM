/* hde-x11taskbar.h — the taskbar of the panel on X11: one button per window, or one per application when they
 * are put together (Settings > Panel). libwnck says which windows there are and what they are called; the
 * buttons are ours, so that resting the pointer on one can open the preview (src/hde-peek.h) — libwnck's own
 * tasklist keeps its buttons to itself.
 *
 * Wayland has src/hde-wltaskbar.c instead (wlr-foreign-toplevel-management).
 */
#ifndef HDE_X11TASKBAR_H
#define HDE_X11TASKBAR_H

#include <gtk/gtk.h>

/* NULL when there is no window list to ask (a session without libwnck's screen). */
GtkWidget *hde_x11_taskbar_new(void);
/* Window titles next to the icons (Settings > Panel: "Window titles on the taskbar"). */
void hde_x11_taskbar_set_labels(GtkWidget *bar, gboolean labels);
/* 0: one button per window, 1: one per application when the bar runs out of room, 2: always one per application. */
void hde_x11_taskbar_set_grouping(GtkWidget *bar, int mode);
/* The preview on hover: off (the old tooltips) or on, and how long the pointer rests before it opens. */
void hde_x11_taskbar_set_preview(GtkWidget *bar, gboolean on, int delay_ms);

#endif /* HDE_X11TASKBAR_H */
