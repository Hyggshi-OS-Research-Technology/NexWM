/* hde-wl.c — see hde-wl.h */
#include "hde-wl.h"
#ifdef GDK_WINDOWING_WAYLAND
#include <gdk/gdkwayland.h>
#endif
#ifdef GDK_WINDOWING_X11
#include <gdk/gdkx.h>
#endif
#ifdef HAVE_GTK_LAYER_SHELL
#include <gtk-layer-shell.h>
#endif

gboolean hde_is_wayland(void)
{
#ifdef GDK_WINDOWING_WAYLAND
    GdkDisplay *d = gdk_display_get_default();
    return d && GDK_IS_WAYLAND_DISPLAY(d);
#else
    return FALSE;
#endif
}

gboolean hde_is_x11(void)
{
#ifdef GDK_WINDOWING_X11
    GdkDisplay *d = gdk_display_get_default();
    return d && GDK_IS_X11_DISPLAY(d);
#else
    return FALSE;
#endif
}

gboolean hde_wl_layer_available(void)
{
#ifdef HAVE_GTK_LAYER_SHELL
    return hde_is_wayland() && gtk_layer_is_supported();
#else
    return FALSE;
#endif
}

#ifdef HAVE_GTK_LAYER_SHELL
static GtkLayerShellLayer to_layer(HdeLayer l)
{
    switch (l) {
    case HDE_LAYER_BACKGROUND: return GTK_LAYER_SHELL_LAYER_BACKGROUND;
    case HDE_LAYER_BOTTOM: return GTK_LAYER_SHELL_LAYER_BOTTOM;
    case HDE_LAYER_OVERLAY: return GTK_LAYER_SHELL_LAYER_OVERLAY;
    default: return GTK_LAYER_SHELL_LAYER_TOP;
    }
}

static GtkLayerShellKeyboardMode to_kb(HdeKeyboard k)
{
    return k == HDE_KB_EXCLUSIVE ? GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE
         : k == HDE_KB_ON_DEMAND ? GTK_LAYER_SHELL_KEYBOARD_MODE_ON_DEMAND : GTK_LAYER_SHELL_KEYBOARD_MODE_NONE;
}
#endif

gboolean hde_wl_layer_init(GtkWindow *w, const char *name_space, HdeLayer layer, guint edges, HdeKeyboard kb)
{
#ifdef HAVE_GTK_LAYER_SHELL
    if (!hde_wl_layer_available()) return FALSE;
    if (!gtk_layer_is_layer_window(w)) gtk_layer_init_for_window(w);
    gtk_layer_set_namespace(w, name_space);
    gtk_layer_set_layer(w, to_layer(layer));
    hde_wl_layer_edges(w, edges);
    gtk_layer_set_keyboard_mode(w, to_kb(kb));
    return TRUE;
#else
    (void)w; (void)name_space; (void)layer; (void)edges; (void)kb;
    return FALSE;
#endif
}

gboolean hde_wl_is_layer(GtkWindow *w)
{
#ifdef HAVE_GTK_LAYER_SHELL
    return hde_is_wayland() && gtk_layer_is_layer_window(w);
#else
    (void)w;
    return FALSE;
#endif
}

void hde_wl_layer_edges(GtkWindow *w, guint edges)
{
#ifdef HAVE_GTK_LAYER_SHELL
    if (!hde_wl_is_layer(w)) return;
    gtk_layer_set_anchor(w, GTK_LAYER_SHELL_EDGE_LEFT, (edges & HDE_EDGE_LEFT) != 0);
    gtk_layer_set_anchor(w, GTK_LAYER_SHELL_EDGE_RIGHT, (edges & HDE_EDGE_RIGHT) != 0);
    gtk_layer_set_anchor(w, GTK_LAYER_SHELL_EDGE_TOP, (edges & HDE_EDGE_TOP) != 0);
    gtk_layer_set_anchor(w, GTK_LAYER_SHELL_EDGE_BOTTOM, (edges & HDE_EDGE_BOTTOM) != 0);
#else
    (void)w; (void)edges;
#endif
}

void hde_wl_layer_margins(GtkWindow *w, int left, int right, int top, int bottom)
{
#ifdef HAVE_GTK_LAYER_SHELL
    if (!hde_wl_is_layer(w)) return;
    gtk_layer_set_margin(w, GTK_LAYER_SHELL_EDGE_LEFT, left);
    gtk_layer_set_margin(w, GTK_LAYER_SHELL_EDGE_RIGHT, right);
    gtk_layer_set_margin(w, GTK_LAYER_SHELL_EDGE_TOP, top);
    gtk_layer_set_margin(w, GTK_LAYER_SHELL_EDGE_BOTTOM, bottom);
#else
    (void)w; (void)left; (void)right; (void)top; (void)bottom;
#endif
}

void hde_wl_layer_exclusive(GtkWindow *w, int zone)
{
#ifdef HAVE_GTK_LAYER_SHELL
    if (hde_wl_is_layer(w)) gtk_layer_set_exclusive_zone(w, zone);
#else
    (void)w; (void)zone;
#endif
}

void hde_wl_layer_keyboard(GtkWindow *w, HdeKeyboard kb)
{
#ifdef HAVE_GTK_LAYER_SHELL
    if (hde_wl_is_layer(w)) gtk_layer_set_keyboard_mode(w, to_kb(kb));
#else
    (void)w; (void)kb;
#endif
}

void hde_wl_layer_monitor(GtkWindow *w, GdkMonitor *m)
{
#ifdef HAVE_GTK_LAYER_SHELL
    if (hde_wl_is_layer(w)) gtk_layer_set_monitor(w, m);
#else
    (void)w; (void)m;
#endif
}

void hde_wl_layer_set_layer(GtkWindow *w, HdeLayer layer)
{
#ifdef HAVE_GTK_LAYER_SHELL
    if (hde_wl_is_layer(w)) gtk_layer_set_layer(w, to_layer(layer));
#else
    (void)w; (void)layer;
#endif
}

GtkWidget *hde_popup_window_new(const char *name_space, HdeLayer layer, guint edges, HdeKeyboard kb)
{
    if (hde_wl_layer_available()) {
        GtkWidget *w = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        gtk_window_set_decorated(GTK_WINDOW(w), FALSE);
        hde_wl_layer_init(GTK_WINDOW(w), name_space, layer, edges, kb);
        return w;
    }
    return gtk_window_new(GTK_WINDOW_POPUP);
}

GdkMonitor *hde_main_monitor(void)
{
    GdkDisplay *d = gdk_display_get_default();
    if (!d) return NULL;
    GdkMonitor *m = gdk_display_get_primary_monitor(d);
    return m ? m : gdk_display_get_monitor(d, 0);
}

void hde_popup_move(GtkWindow *w, int x, int y)
{
    if (hde_wl_is_layer(w)) {
        GdkRectangle g = { 0, 0, 0, 0 };
        GdkMonitor *m = hde_main_monitor();
        if (m) gdk_monitor_get_geometry(m, &g);
        hde_wl_layer_edges(w, HDE_EDGE_LEFT | HDE_EDGE_TOP);
        hde_wl_layer_exclusive(w, -1);
        hde_wl_layer_margins(w, MAX(0, x - g.x), 0, MAX(0, y - g.y), 0);
        return;
    }
    gtk_window_move(w, x, y);
}
