/* hde-flyout.c — see hde-flyout.h (the window part is the same as the Start menu's, hde-startmenu.c) */
#include "hde-flyout.h"
#include "hde-wl.h"
#include "hde-osd.h"
#include "hde-panel-config.h"

#define GAP 6                       /* between the panel and the frame, and from the screen edge */

struct _HdeFlyout {
    char *name;
    GtkWidget *win, *frame, *child, *anchor, *panel, *focus;
    gboolean grabbed, panel_top;
    int grab_tries, placed_w, placed_h;
    guint grab_retry_id;
    GdkRectangle geo;               /* where the frame is */
    void (*hide_cb)(gpointer);
    gpointer hide_data;
    gboolean (*esc_cb)(gpointer);
    gpointer esc_data;
};

static gboolean debug_on(void) { return g_getenv("HDE_DEBUG") != NULL; }

static gboolean transparent_draw(GtkWidget *w, cairo_t *cr, gpointer d)
{
    (void)w; (void)d;
    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_restore(cr);
    return FALSE;
}

static gboolean point_in_frame(HdeFlyout *f, int rx, int ry)
{
    GdkWindow *fw = gtk_widget_get_window(f->frame);
    if (!fw) return FALSE;
    int ox = 0, oy = 0;
    gdk_window_get_origin(fw, &ox, &oy);
    GtkAllocation a;
    gtk_widget_get_allocation(f->frame, &a);
    if (gtk_widget_get_has_window(f->frame)) { a.x = 0; a.y = 0; }
    return rx >= ox + a.x && ry >= oy + a.y && rx < ox + a.x + a.width && ry < oy + a.y + a.height;
}

/* a click outside closes it (X11: we hold the pointer; Wayland: the transparent rest of the screen) */
static gboolean on_button(GtkWidget *w, GdkEventButton *e, gpointer d)
{
    HdeFlyout *f = d;
    if (e->type != GDK_BUTTON_PRESS) return FALSE;
    GdkWindow *top = gdk_window_get_toplevel(e->window);
    gboolean ours = top == gtk_widget_get_window(w);
    if (!ours || !point_in_frame(f, (int)e->x_root, (int)e->y_root)) {
        if (debug_on()) g_printerr("hde-panel: %s: click outside: closed\n", f->name);
        hde_flyout_hide(f);
        return TRUE;
    }
    return FALSE;
}

/* after the widgets: an entry or a list gets the keys first */
static gboolean on_key_after(GtkWidget *w, GdkEventKey *e, gpointer d)
{
    (void)w;
    HdeFlyout *f = d;
    if (e->keyval != GDK_KEY_Escape) return FALSE;
    if (f->esc_cb && f->esc_cb(f->esc_data)) return TRUE;
    hde_flyout_hide(f);
    return TRUE;
}

static void on_size_allocate(GtkWidget *w, GdkRectangle *a, gpointer d);

HdeFlyout *hde_flyout_new(const char *name, const char *css_class)
{
    HdeFlyout *f = g_new0(HdeFlyout, 1);
    f->name = g_strdup(name);
    gboolean wl = hde_wl_layer_available();
    f->win = gtk_window_new(wl ? GTK_WINDOW_TOPLEVEL : GTK_WINDOW_POPUP);
    gtk_window_set_title(GTK_WINDOW(f->win), name);
    gtk_window_set_type_hint(GTK_WINDOW(f->win), GDK_WINDOW_TYPE_HINT_POPUP_MENU);
    gtk_window_set_resizable(GTK_WINDOW(f->win), FALSE);
    gtk_widget_add_events(f->win, GDK_BUTTON_PRESS_MASK | GDK_KEY_PRESS_MASK);
    g_signal_connect(f->win, "button-press-event", G_CALLBACK(on_button), f);
    g_signal_connect_after(f->win, "key-press-event", G_CALLBACK(on_key_after), f);
    g_signal_connect(f->win, "delete-event", G_CALLBACK(gtk_widget_hide_on_delete), NULL);
    g_signal_connect(f->win, "size-allocate", G_CALLBACK(on_size_allocate), f);
    if (wl) {
        char *ns = g_strdup_printf("hde-%s", name);
        for (char *p = ns; *p; p++) if (*p == ' ') *p = '-';
        hde_wl_layer_init(GTK_WINDOW(f->win), ns, HDE_LAYER_TOP, HDE_EDGE_ALL, HDE_KB_EXCLUSIVE);
        g_free(ns);
        hde_wl_layer_exclusive(GTK_WINDOW(f->win), 0);
        GdkVisual *v = gdk_screen_get_rgba_visual(gtk_widget_get_screen(f->win));
        if (v) gtk_widget_set_visual(f->win, v);
        gtk_widget_set_app_paintable(f->win, TRUE);
        g_signal_connect(f->win, "draw", G_CALLBACK(transparent_draw), NULL);
        f->frame = gtk_event_box_new();
        gtk_widget_set_halign(f->frame, GTK_ALIGN_END);
        gtk_widget_set_valign(f->frame, GTK_ALIGN_END);
        gtk_style_context_add_class(gtk_widget_get_style_context(f->frame), "rounded");
        gtk_container_add(GTK_CONTAINER(f->win), f->frame);
        gtk_widget_show(f->frame);
    } else {
        hde_popup_setup_alpha(f->win);
        f->frame = f->win;
    }
    gtk_style_context_add_class(gtk_widget_get_style_context(f->frame), "hde-flyout");
    if (css_class) gtk_style_context_add_class(gtk_widget_get_style_context(f->frame), css_class);
    return f;
}

void hde_flyout_set_child(HdeFlyout *f, GtkWidget *child)
{
    if (f->child) gtk_container_remove(GTK_CONTAINER(f->frame), f->child);
    f->child = child;
    gtk_container_add(GTK_CONTAINER(f->frame), child);
    gtk_widget_show(child);
}

void hde_flyout_on_hide(HdeFlyout *f, void (*cb)(gpointer), gpointer data) { f->hide_cb = cb; f->hide_data = data; }
void hde_flyout_on_escape(HdeFlyout *f, gboolean (*cb)(gpointer), gpointer data) { f->esc_cb = cb; f->esc_data = data; }
gboolean hde_flyout_visible(HdeFlyout *f) { return f && f->win && gtk_widget_get_visible(f->win); }
void hde_flyout_geometry(HdeFlyout *f, GdkRectangle *r) { *r = f->geo; }

const char *hde_flyout_input_state(HdeFlyout *f)
{
    if (hde_wl_is_layer(GTK_WINDOW(f->win))) return "layer shell, keyboard exclusive";
    return f->grabbed ? "keyboard and pointer grabbed" : "not grabbed yet";
}

/* ---------------------------------------------------------------- X11: hold the keyboard and the pointer */
static void send_focus(HdeFlyout *f, gboolean in)
{
    GdkWindow *gw = gtk_widget_get_window(f->win);
    if (!gw) return;
    GdkSeat *seat = gdk_display_get_default_seat(gtk_widget_get_display(f->win));
    GdkEvent *fe = gdk_event_new(GDK_FOCUS_CHANGE);
    fe->focus_change.type = GDK_FOCUS_CHANGE;
    fe->focus_change.window = g_object_ref(gw);
    fe->focus_change.in = in;
    if (seat && gdk_seat_get_keyboard(seat)) gdk_event_set_device(fe, gdk_seat_get_keyboard(seat));
    gtk_widget_send_focus_change(f->win, fe);
    gdk_event_free(fe);
}

static gboolean try_grab(HdeFlyout *f)
{
    GdkWindow *gw = gtk_widget_get_window(f->win);
    if (!gw) return FALSE;
    GdkSeat *seat = gdk_display_get_default_seat(gtk_widget_get_display(f->win));
    GdkGrabStatus st = gdk_seat_grab(seat, gw, GDK_SEAT_CAPABILITY_ALL, TRUE, NULL, NULL, NULL, NULL);
    if (st != GDK_GRAB_SUCCESS) return FALSE;
    f->grabbed = TRUE;
    gtk_grab_add(f->win);
    send_focus(f, TRUE);
    if (f->focus) gtk_widget_grab_focus(f->focus);
    return TRUE;
}

static gboolean grab_retry(gpointer d)
{
    HdeFlyout *f = d;
    if (!hde_flyout_visible(f) || f->grabbed) { f->grab_retry_id = 0; return G_SOURCE_REMOVE; }
    if (try_grab(f) || ++f->grab_tries >= 12) {
        if (!f->grabbed && debug_on()) g_printerr("hde-panel: %s: could not grab the keyboard and the pointer\n", f->name);
        f->grab_retry_id = 0;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

/* ---------------------------------------------------------------- placement */
static GdkMonitor *panel_monitor(GtkWidget *panel)
{
    GdkWindow *pw = panel ? gtk_widget_get_window(panel) : NULL;
    GdkMonitor *m = pw ? gdk_display_get_monitor_at_window(gdk_display_get_default(), pw) : NULL;
    return m ? m : hde_main_monitor();
}

int hde_flyout_max_height(HdeFlyout *f, GtkWidget *panel)
{
    (void)f;
    GdkRectangle mon = { 0, 0, 1024, 768 };
    GdkMonitor *m = panel_monitor(panel);
    if (m) gdk_monitor_get_geometry(m, &mon);
    int t = 0, b = 0;
    hde_panel_reserved(&t, &b);
    return MAX(240, mon.height - t - b - 3 * GAP);
}

/* where the frame goes for a size w x h; resize: also give the window that size (X11) */
static void place_sized(HdeFlyout *f, int w, int h, gboolean resize)
{
    GdkRectangle mon = { 0, 0, 1024, 768 };
    GdkMonitor *m = panel_monitor(f->panel);
    if (m) gdk_monitor_get_geometry(m, &mon);
    GdkWindow *pw = f->panel ? gtk_widget_get_window(f->panel) : NULL;
    int px = mon.x, py = mon.y + mon.height, ph = 0;
    gboolean layer = hde_wl_is_layer(GTK_WINDOW(f->win));
    if (layer) {
        /* Wayland: every surface has its own coordinates (origin 0,0): ask the panel's settings instead */
        int t = 0, b = 0;
        hde_panel_reserved(&t, &b);
        f->panel_top = t > 0;
        py = f->panel_top ? mon.y : mon.y + mon.height - b;
        ph = f->panel_top ? t : b;
    } else if (pw) {
        gdk_window_get_origin(pw, &px, &py);
        ph = gdk_window_get_height(pw);
        f->panel_top = py < mon.y + mon.height / 2;
    } else f->panel_top = FALSE;
    /* right edge of the anchor, in screen coordinates (the panel spans the whole width of its screen) */
    int right = mon.x + mon.width - GAP;
    if (f->anchor && f->panel && gtk_widget_get_visible(f->anchor) && gtk_widget_get_realized(f->anchor)) {
        int ax = 0, ay = 0;
        if (gtk_widget_translate_coordinates(f->anchor, f->panel, 0, 0, &ax, &ay))
            right = (layer ? mon.x : px) + ax + gtk_widget_get_allocated_width(f->anchor);
    }
    int x = CLAMP(right - w, mon.x + GAP, mon.x + mon.width - w - GAP);
    int y = f->panel_top ? py + ph + GAP : py - h - GAP;
    if (!pw && !layer) y = mon.y + mon.height - h - 40;
    if (layer) {
        hde_wl_layer_monitor(GTK_WINDOW(f->win), m);
        gtk_widget_set_valign(f->frame, f->panel_top ? GTK_ALIGN_START : GTK_ALIGN_END);
        gtk_widget_set_margin_end(f->frame, MAX(0, mon.x + mon.width - (x + w)));
        gtk_widget_set_margin_top(f->frame, f->panel_top ? GAP : 0);
        gtk_widget_set_margin_bottom(f->frame, f->panel_top ? 0 : GAP);
    } else {
        gtk_window_move(GTK_WINDOW(f->win), x, y);
        if (resize) gtk_window_resize(GTK_WINDOW(f->win), w, h);
    }
    f->geo.x = x; f->geo.y = y; f->geo.width = w; f->geo.height = h;
    f->placed_w = w;
    f->placed_h = h;
}

static void place(HdeFlyout *f)
{
    GtkRequisition nat;
    gtk_widget_get_preferred_size(f->frame == f->win ? f->win : f->frame, NULL, &nat);
    place_sized(f, nat.width, nat.height, TRUE);
}

/* X11: the content grew or shrank (a page, notifications): keep the frame next to the panel, at the size it got
 * (only moved, so that this never feeds back into another resize) */
static void on_size_allocate(GtkWidget *w, GdkRectangle *a, gpointer d)
{
    (void)w;
    HdeFlyout *f = d;
    if (!hde_flyout_visible(f) || hde_wl_is_layer(GTK_WINDOW(f->win))) return;
    if (a->width == f->placed_w && a->height == f->placed_h) return;
    place_sized(f, a->width, a->height, FALSE);
}

void hde_flyout_show(HdeFlyout *f, GtkWidget *anchor, GtkWidget *panel)
{
    f->anchor = anchor;
    f->panel = panel;
    place(f);
    gtk_widget_show(f->win);
    if (hde_wl_is_layer(GTK_WINDOW(f->win))) {
        gtk_window_present(GTK_WINDOW(f->win));
        if (f->focus) gtk_widget_grab_focus(f->focus);
    } else {
        gdk_window_raise(gtk_widget_get_window(f->win));
        f->grab_tries = 0;
        if (!try_grab(f) && !f->grab_retry_id) f->grab_retry_id = g_timeout_add(80, grab_retry, f);
    }
}

void hde_flyout_hide(HdeFlyout *f)
{
    if (!hde_flyout_visible(f)) return;
    if (f->grab_retry_id) { g_source_remove(f->grab_retry_id); f->grab_retry_id = 0; }
    if (f->grabbed) {
        gtk_grab_remove(f->win);
        gdk_seat_ungrab(gdk_display_get_default_seat(gtk_widget_get_display(f->win)));
        f->grabbed = FALSE;
        send_focus(f, FALSE);
    }
    gtk_widget_hide(f->win);
    if (debug_on()) g_printerr("hde-panel: %s: hidden\n", f->name);
    if (f->hide_cb) f->hide_cb(f->hide_data);
}

void hde_flyout_focus(HdeFlyout *f, GtkWidget *w)
{
    f->focus = w;
    if (w && hde_flyout_visible(f)) gtk_widget_grab_focus(w);
}

/* ---------------------------------------------------------------- right-click menus */
static const char *pick_icon(const char *spec, char *buf, size_t len)
{
    char **names = g_strsplit(spec, "|", -1);
    GtkIconTheme *t = gtk_icon_theme_get_default();
    const char *pick = names[0];
    for (int i = 0; names[i]; i++)
        if (gtk_icon_theme_has_icon(t, names[i])) { pick = names[i]; break; }
    g_strlcpy(buf, pick ? pick : "", len);
    g_strfreev(names);
    return buf;
}

GtkWidget *hde_menu_item(const char *icon, const char *label)
{
    GtkWidget *it = gtk_menu_item_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *img;
    if (icon) {
        char buf[128];
        img = gtk_image_new_from_icon_name(pick_icon(icon, buf, sizeof buf), GTK_ICON_SIZE_MENU);
    } else img = gtk_image_new();
    gtk_image_set_pixel_size(GTK_IMAGE(img), 16);
    gtk_widget_set_size_request(img, 16, 16);
    GtkWidget *l = gtk_label_new_with_mnemonic(label);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_box_pack_start(GTK_BOX(box), img, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), l, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(it), box);
    gtk_style_context_add_class(gtk_widget_get_style_context(it), "hde-icon-item");
    return it;
}

GtkWidget *hde_menu_check_item(const char *label, gboolean active)
{
    GtkWidget *it = gtk_check_menu_item_new_with_mnemonic(label);
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(it), active);
    return it;
}

GtkWidget *hde_menu_icon(const char *icon)
{
    char buf[128];
    GtkWidget *img = gtk_image_new_from_icon_name(pick_icon(icon, buf, sizeof buf), GTK_ICON_SIZE_MENU);
    gtk_image_set_pixel_size(GTK_IMAGE(img), 16);
    return img;
}

GtkWidget *hde_menu_radio_item(GtkWidget *group_member, GtkWidget *image, const char *label, gboolean active)
{
    GtkWidget *it = group_member ? gtk_radio_menu_item_new_from_widget(GTK_RADIO_MENU_ITEM(group_member))
                                 : gtk_radio_menu_item_new(NULL);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    if (image) gtk_box_pack_start(GTK_BOX(box), image, FALSE, FALSE, 0);
    GtkWidget *l = gtk_label_new_with_mnemonic(label);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_box_pack_start(GTK_BOX(box), l, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(it), box);
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(it), active);
    return it;
}

GtkWidget *hde_menu_submenu(GtkWidget *menu, const char *icon, const char *label)
{
    GtkWidget *it = hde_menu_item(icon, label);
    GtkWidget *sub = gtk_menu_new();
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(it), sub);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), it);
    return sub;
}

static gboolean destroy_menu_idle(gpointer m)
{
    gtk_widget_destroy(GTK_WIDGET(m));
    return G_SOURCE_REMOVE;
}

static void on_menu_closed(GtkMenuShell *m, gpointer d)
{
    (void)d;
    g_idle_add(destroy_menu_idle, m);           /* after the activated item has run */
}

void hde_menu_popup(GtkWidget *menu, GtkWidget *widget, const GdkEvent *event)
{
    gtk_style_context_add_class(gtk_widget_get_style_context(menu), "hde-panel-menu");
    g_signal_connect(menu, "deactivate", G_CALLBACK(on_menu_closed), NULL);
    gtk_widget_show_all(menu);
    if (widget && !gtk_menu_get_attach_widget(GTK_MENU(menu))) gtk_menu_attach_to_widget(GTK_MENU(menu), widget, NULL);
    if (event) gtk_menu_popup_at_pointer(GTK_MENU(menu), event);
    else if (widget) gtk_menu_popup_at_widget(GTK_MENU(menu), widget, GDK_GRAVITY_NORTH_WEST, GDK_GRAVITY_SOUTH_WEST, NULL);
    else gtk_menu_popup_at_pointer(GTK_MENU(menu), NULL);
}
