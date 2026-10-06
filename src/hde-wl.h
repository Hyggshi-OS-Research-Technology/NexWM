/* hde-wl.h — the bits of HDE that differ on Wayland (the "HDE (Wayland)" session, labwc compositor).
 *
 * On X11 the panel, desktop and popups are ordinary windows with hints (DOCK, DESKTOP, struts) placed with
 * gtk_window_move(). Wayland has no global coordinates and no such hints: there they become layer-shell surfaces
 * (wlr-layer-shell, through gtk-layer-shell), anchored to screen edges with margins. Built without gtk-layer-shell
 * (libgtk-layer-shell-dev) these functions do nothing and report FALSE, and HDE runs on X11 only.
 */
#ifndef HDE_WL_H
#define HDE_WL_H

#include <gtk/gtk.h>

typedef enum { HDE_LAYER_BACKGROUND, HDE_LAYER_BOTTOM, HDE_LAYER_TOP, HDE_LAYER_OVERLAY } HdeLayer;
enum { HDE_EDGE_LEFT = 1, HDE_EDGE_RIGHT = 2, HDE_EDGE_TOP = 4, HDE_EDGE_BOTTOM = 8, HDE_EDGE_ALL = 15 };
typedef enum { HDE_KB_NONE, HDE_KB_EXCLUSIVE, HDE_KB_ON_DEMAND } HdeKeyboard;

gboolean hde_is_wayland(void);              /* the GDK display is a Wayland one */
gboolean hde_is_x11(void);
gboolean hde_wl_layer_available(void);      /* built with gtk-layer-shell and the compositor supports it */

/* Make a not yet realized window a layer surface. FALSE: not Wayland / no layer shell (the window stays normal). */
gboolean hde_wl_layer_init(GtkWindow *w, const char *name_space, HdeLayer layer, guint edges, HdeKeyboard kb);
gboolean hde_wl_is_layer(GtkWindow *w);
void hde_wl_layer_edges(GtkWindow *w, guint edges);
void hde_wl_layer_margins(GtkWindow *w, int left, int right, int top, int bottom);
void hde_wl_layer_exclusive(GtkWindow *w, int zone);   /* >0 reserve, 0 keep clear of others, -1 ignore others */
void hde_wl_layer_keyboard(GtkWindow *w, HdeKeyboard kb);
void hde_wl_layer_monitor(GtkWindow *w, GdkMonitor *m);
void hde_wl_layer_set_layer(GtkWindow *w, HdeLayer layer);

/* A window for popups that never take the focus (OSD, notifications): an override-redirect popup on X11, a layer
 * surface on Wayland (gtk-layer-shell works with toplevel windows only). */
GtkWidget *hde_popup_window_new(const char *name_space, HdeLayer layer, guint edges, HdeKeyboard kb);

/* The screen popups belong to: the primary one on X11, on Wayland the first one (or the one the panel is on). */
GdkMonitor *hde_main_monitor(void);
/* Place a popup at x,y (desktop coordinates, as on X11): gtk_window_move() on X11, margins of a layer surface
 * anchored top-left on Wayland. */
void hde_popup_move(GtkWindow *w, int x, int y);

#endif
