/* hde-switch.h — the window switcher: hold Alt and press Tab, and the windows line up in the middle of the
 * screen with a picture of each of them and its name under it; let Alt go and the one that is picked comes to
 * the front (Windows 11 shows the same).
 *
 * The popup is here (src/hde-switch.c, part of hde-panel). The sums behind it — which window is picked, how the
 * windows are laid out, how big the popup is, where it stands — are in src/hde-switch-core.[ch] and are tested
 * without a screen by tests/switch-test.c. The list of windows and the way to read a window's picture and to
 * bring it up come from the taskbar: src/hde-x11taskbar.c on X11.
 *
 * Wayland is not covered here: there the compositor's own switcher does this (labwc's, with previews, which
 * hde-settings --wayland-config asks for), because no program may read what another window shows.
 */
#ifndef HDE_SWITCH_H
#define HDE_SWITCH_H

#include <gtk/gtk.h>

/* what the switcher needs from the taskbar: one window in the list, and what can be done with it */
typedef struct {
    const char *title;      /* the name of the window (borrowed: the taskbar keeps it) */
    GdkPixbuf *icon;        /* the icon of the application, may be NULL (borrowed as well) */
    gpointer handle;        /* what the taskbar needs to find the window again */
} HdeSwitchItem;

typedef struct {
    /* a picture of the window, at its own size (NULL: none — a minimized window on X11) */
    GdkPixbuf *(*grab)(gpointer handle, gpointer data);
    /* bring it up. `timestamp` is the time of the key press that asked for it: a window manager only listens to
     * a request that carries the time of a real key press. */
    void (*activate)(gpointer handle, gpointer data, guint32 timestamp);
} HdeSwitchOps;

/* Where the windows come from (called once, before the switcher is built). */
void hde_switch_set_provider(int (*list)(HdeSwitchItem *out, int max), const HdeSwitchOps *ops, gpointer data);

/* Alt+Tab was pressed (step 1), or Alt+Shift+Tab (step -1). `timestamp`: the time of that key press. */
void hde_switch_step(int step, guint32 timestamp);
/* Alt was let go: bring the window that is picked up (cancel: leave everything as it is). */
void hde_switch_end(gboolean cancel, guint32 timestamp);
gboolean hde_switch_visible(void);
/* Close it and forget the windows (the panel is going away, or the windows changed under it). */
void hde_switch_hide(void);

#endif /* HDE_SWITCH_H */
