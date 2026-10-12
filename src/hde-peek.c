/* hde-peek.c — see hde-peek.h: the preview the taskbar shows when the pointer rests on one of its buttons.
 *
 * A popup window above (or below, for a panel at the top of the screen) the button, with a picture of every
 * window of the application in a grid, the title under each of them and a × to close it. It never takes the
 * keyboard: the pointer alone drives it — rest on the button, it opens; move away, it closes; move into it, it
 * stays and the pictures can be clicked.
 */
#include "hde-peek.h"

#include "hde-osd.h"                 /* hde_popup_setup_alpha: rounded corners where the screen is composited */
#include "hde-panel-config.h"        /* hde_panel_reserved: how much of the screen the panel takes */
#include "hde-peek-core.h"
#include "hde-wl.h"

#include <string.h>

#define HIDE_GRACE_MS 150            /* how long the preview waits before it closes once the pointer has gone */
#define PEEK_PAD 8                   /* inside the frame, and between the title and the pictures */
#define PEEK_GAP 8                   /* between the pictures */
#define ICON_PX 48                   /* the icon of the application where there is no picture to show */
#define NO_PIC_W 160                 /* the room a picture is given when there is none (a minimized window) */
#define NO_PIC_H 90
/* the box a picture is drawn in: the whole thing for one window, a smaller one when there are several */
#define GROUP_W 200
#define GROUP_H 112

typedef struct {
    char *title;
    GdkPixbuf *icon;                 /* ours: unref'd when the popup is rebuilt */
    gpointer handle;
} Item;

static GtkWidget *win;               /* the popup, kept between hovers */
static GtkWidget *content;           /* the box inside it */
static GtkWidget *anchor;            /* the taskbar button the preview belongs to */
static const HdePeekOps *ops;
static gpointer ops_data;
static GList *items;                 /* Item*, in the order they are shown */
static char *group_title;
static guint show_id, hide_id;

static gboolean debug_on(void) { return g_getenv("HDE_DEBUG") != NULL; }
/* what it does, for the session log (HDE_DEBUG=1): the test rests the pointer on a button and waits for the
 * "shown" line, so every step that can keep it from appearing says so */
#define DBG(...) do { if (debug_on()) { g_printerr("hde-panel: peek: " __VA_ARGS__); g_printerr("\n"); } } while (0)

/* ---------------------------------------------------------------- the list of windows */

static void items_free(void)
{
    for (GList *l = items; l; l = l->next) {
        Item *it = l->data;
        g_free(it->title);
        if (it->icon) g_object_unref(it->icon);
        g_free(it);
    }
    g_list_free(items);
    items = NULL;
}

/* the button the preview belongs to was destroyed (the window it stood for is gone): nothing left to point at */
/* the tooltip of the button would stay over the preview (the pointer is still on the button): mute it, as the
 * Control Center and the battery panel do with theirs (src/hde-flyout.c) */
static GtkWidget *muted_tip;

static void mute_tip(GtkWidget *w)
{
    if (muted_tip) {
        gtk_widget_set_has_tooltip(muted_tip, TRUE);
        g_object_remove_weak_pointer(G_OBJECT(muted_tip), (gpointer *)&muted_tip);
        muted_tip = NULL;
    }
    if (w && gtk_widget_get_has_tooltip(w)) {
        gtk_widget_set_has_tooltip(w, FALSE);
        muted_tip = w;
        g_object_add_weak_pointer(G_OBJECT(w), (gpointer *)&muted_tip);
        gtk_tooltip_trigger_tooltip_query(gtk_widget_get_display(w));
    }
}

static void on_anchor_gone(gpointer data, GObject *was)
{
    (void)data; (void)was;
    anchor = NULL;
    hde_peek_hide();
}

static void set_anchor(GtkWidget *a)
{
    if (anchor) g_object_weak_unref(G_OBJECT(anchor), on_anchor_gone, NULL);
    anchor = a;
    if (anchor) g_object_weak_ref(G_OBJECT(anchor), on_anchor_gone, NULL);
}

/* ---------------------------------------------------------------- the popup window */

static gboolean on_win_enter(GtkWidget *w, GdkEventCrossing *e, gpointer d)
{
    (void)w; (void)e; (void)d;
    if (hide_id) { g_source_remove(hide_id); hide_id = 0; }     /* the pointer is inside: stay */
    return FALSE;
}

static gboolean on_win_leave(GtkWidget *w, GdkEventCrossing *e, gpointer d)
{
    (void)w; (void)e; (void)d;
    hde_peek_leave();
    return FALSE;
}

static void ensure_window(void)
{
    if (win) return;
    win = hde_popup_window_new("hde-peek", HDE_LAYER_OVERLAY, HDE_EDGE_ALL, HDE_KB_NONE);
    gtk_window_set_title(GTK_WINDOW(win), "hde-peek");
    gtk_window_set_type_hint(GTK_WINDOW(win), GDK_WINDOW_TYPE_HINT_POPUP_MENU);
    gtk_window_set_resizable(GTK_WINDOW(win), FALSE);
    gtk_window_set_accept_focus(GTK_WINDOW(win), FALSE);
    hde_popup_setup_alpha(win);
    gtk_style_context_add_class(gtk_widget_get_style_context(win), "hde-peek");
    gtk_widget_add_events(win, GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK);
    g_signal_connect(win, "enter-notify-event", G_CALLBACK(on_win_enter), NULL);
    g_signal_connect(win, "leave-notify-event", G_CALLBACK(on_win_leave), NULL);
    g_signal_connect(win, "destroy", G_CALLBACK(gtk_widget_destroyed), &win);
    content = gtk_box_new(GTK_ORIENTATION_VERTICAL, PEEK_GAP);
    gtk_container_set_border_width(GTK_CONTAINER(content), PEEK_PAD);
    gtk_container_add(GTK_CONTAINER(win), content);
}

/* ---------------------------------------------------------------- one window in the preview */

static gboolean on_item_enter(GtkWidget *w, GdkEventCrossing *e, gpointer d)
{
    (void)w; (void)e;
    gtk_style_context_add_class(gtk_widget_get_style_context(GTK_WIDGET(d)), "hover");
    return FALSE;
}

static gboolean on_item_leave(GtkWidget *w, GdkEventCrossing *e, gpointer d)
{
    (void)w; (void)e;
    gtk_style_context_remove_class(gtk_widget_get_style_context(GTK_WIDGET(d)), "hover");
    return FALSE;
}

static void on_thumb_press(GtkWidget *w, GdkEventButton *e, gpointer d)
{
    (void)w;
    Item *it = d;
    if (e->type != GDK_BUTTON_PRESS || e->button != 1) return;
    if (ops && ops->activate) ops->activate(it->handle, ops_data);
    hde_peek_hide();
}

static void on_close_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    Item *it = d;
    if (ops && ops->close) ops->close(it->handle, ops_data);
    hde_peek_hide();
}

/* the picture itself, or what stands in for it: the icon of the application where the window has none to give
 * (it is minimized, or this is Wayland, where no program may read another window's picture) */
static GtkWidget *picture(Item *it, int max_w, int max_h)
{
    GdkPixbuf *pix = ops && ops->grab ? ops->grab(it->handle, ops_data) : NULL;
    GtkWidget *img;
    if (pix && gdk_pixbuf_get_width(pix) > 0 && gdk_pixbuf_get_height(pix) > 0) {
        int tw = 0, th = 0;
        hde_peek_thumb_size(gdk_pixbuf_get_width(pix), gdk_pixbuf_get_height(pix), max_w, max_h, &tw, &th);
        GdkPixbuf *small = (tw > 0 && th > 0) ? gdk_pixbuf_scale_simple(pix, tw, th, GDK_INTERP_BILINEAR) : NULL;
        img = gtk_image_new_from_pixbuf(small ? small : pix);
        if (small) g_object_unref(small);
        g_object_unref(pix);
    } else {
        if (pix) g_object_unref(pix);
        if (it->icon) img = gtk_image_new_from_pixbuf(it->icon);
        else img = gtk_image_new_from_icon_name("application-x-executable", GTK_ICON_SIZE_DIALOG);
        gtk_image_set_pixel_size(GTK_IMAGE(img), ICON_PX);
        gtk_widget_set_size_request(img, NO_PIC_W, NO_PIC_H);
    }
    return img;
}

static GtkWidget *build_item(Item *it, int max_w, int max_h)
{
    GtkWidget *thumb = gtk_event_box_new();
    gtk_widget_add_events(thumb, GDK_BUTTON_PRESS_MASK | GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK);
    GtkWidget *img = picture(it, max_w, max_h);
    gtk_style_context_add_class(gtk_widget_get_style_context(img), "peek-thumb");
    gtk_container_add(GTK_CONTAINER(thumb), img);

    GtkWidget *ov = gtk_overlay_new();
    gtk_container_add(GTK_CONTAINER(ov), thumb);
    if (ops && ops->close) {
        GtkWidget *cl = gtk_button_new();
        gtk_container_add(GTK_CONTAINER(cl), gtk_image_new_from_icon_name("window-close-symbolic", GTK_ICON_SIZE_MENU));
        gtk_button_set_relief(GTK_BUTTON(cl), GTK_RELIEF_NONE);
        gtk_widget_set_can_focus(cl, FALSE);
        gtk_widget_set_tooltip_text(cl, "Close");
        gtk_widget_set_halign(cl, GTK_ALIGN_END);
        gtk_widget_set_valign(cl, GTK_ALIGN_START);
        gtk_widget_set_margin_top(cl, 2);
        gtk_widget_set_margin_end(cl, 2);
        gtk_style_context_add_class(gtk_widget_get_style_context(cl), "peek-close");
        g_signal_connect(cl, "clicked", G_CALLBACK(on_close_clicked), it);
        gtk_overlay_add_overlay(GTK_OVERLAY(ov), cl);
    }

    GtkWidget *lbl = gtk_label_new(it->title && *it->title ? it->title : " ");
    gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(lbl), max_w / 8);
    gtk_style_context_add_class(gtk_widget_get_style_context(lbl), "peek-title");

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_style_context_add_class(gtk_widget_get_style_context(vbox), "peek-item");
    gtk_box_pack_start(GTK_BOX(vbox), ov, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), lbl, FALSE, FALSE, 0);
    g_signal_connect(thumb, "button-press-event", G_CALLBACK(on_thumb_press), it);
    g_signal_connect(thumb, "enter-notify-event", G_CALLBACK(on_item_enter), vbox);
    g_signal_connect(thumb, "leave-notify-event", G_CALLBACK(on_item_leave), vbox);
    return vbox;
}

/* ---------------------------------------------------------------- filling and placing */

static void rebuild(void)
{
    GList *ch = gtk_container_get_children(GTK_CONTAINER(content));
    for (GList *l = ch; l; l = l->next) gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(ch);

    int n = g_list_length(items);
    int max_w = n > 1 ? GROUP_W : HDE_PEEK_THUMB_MAX_W;
    int max_h = n > 1 ? GROUP_H : HDE_PEEK_THUMB_MAX_H;

    if (n > 1 && group_title && *group_title) {
        GtkWidget *h = gtk_label_new(group_title);
        gtk_label_set_ellipsize(GTK_LABEL(h), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(h), 40);
        gtk_widget_set_halign(h, GTK_ALIGN_START);
        gtk_style_context_add_class(gtk_widget_get_style_context(h), "peek-group");
        gtk_box_pack_start(GTK_BOX(content), h, FALSE, FALSE, 0);
    }

    int cols = 1, rows = 1;
    hde_peek_grid(n, HDE_PEEK_PER_ROW, &cols, &rows);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), PEEK_GAP);
    gtk_grid_set_column_spacing(GTK_GRID(grid), PEEK_GAP);
    int i = 0;
    for (GList *l = items; l; l = l->next, i++)
        gtk_grid_attach(GTK_GRID(grid), build_item((Item *)l->data, max_w, max_h), i % cols, i / cols, 1, 1);
    gtk_box_pack_start(GTK_BOX(content), grid, FALSE, FALSE, 0);
    gtk_widget_show_all(content);
}

/* above the button, centred on it, and always on the screen: see hde_peek_popup_position */
static void place(void)
{
    GdkRectangle mon = { 0, 0, 1024, 768 };
    GdkMonitor *m = NULL;
    gboolean layer = FALSE, panel_top = FALSE;
    int px = mon.x, py = mon.y, ph = 0, ax = 0, ay = 0;

    GtkWidget *top = anchor ? gtk_widget_get_toplevel(anchor) : NULL;
    if (top && !gtk_widget_is_toplevel(top)) top = NULL;
    GdkWindow *tw = top ? gtk_widget_get_window(top) : NULL;
    if (tw) {
        m = gdk_display_get_monitor_at_window(gtk_widget_get_display(top), tw);
        layer = hde_wl_is_layer(GTK_WINDOW(top));
    }
    if (!m) m = hde_main_monitor();
    if (m) gdk_monitor_get_geometry(m, &mon);

    if (layer) {
        /* Wayland: every surface has its own coordinates, so the panel's place comes from its own settings */
        int t = 0, b = 0;
        hde_panel_reserved(&t, &b);
        panel_top = t > 0;
        py = panel_top ? mon.y : mon.y + mon.height - b;
        ph = panel_top ? t : b;
    } else if (tw) {
        gdk_window_get_origin(tw, &px, &py);
        ph = gdk_window_get_height(tw);
        panel_top = py < mon.y + mon.height / 2;
    }
    if (anchor && top) gtk_widget_translate_coordinates(anchor, top, 0, 0, &ax, &ay);

    HdeRect a = { (layer ? mon.x : px) + ax, py + ay, 0, 0 };
    if (anchor) {
        a.w = gtk_widget_get_allocated_width(anchor);
        a.h = gtk_widget_get_allocated_height(anchor);
    }
    HdeRect work = { mon.x, mon.y, mon.width, mon.height };
    if (panel_top) { work.y += ph; work.h -= ph; } else work.h -= ph;
    if (work.h < 1) { work.y = mon.y; work.h = mon.height; }

    GtkRequisition nat = { 0, 0 };
    gtk_widget_get_preferred_size(win, NULL, &nat);
    int x = 0, y = 0;
    hde_peek_popup_position(&a, &work, nat.width, nat.height, panel_top, &x, &y);
    hde_popup_move(GTK_WINDOW(win), x, y);
    if (debug_on())
        g_printerr("hde-panel: peek: %d window(s), %dx%d at %d,%d (button %d,%d %dx%d, panel %s)\n",
                   g_list_length(items), nat.width, nat.height, x, y, a.x, a.y, a.w, a.h,
                   panel_top ? "top" : "bottom");
}

static gboolean show_now(gpointer d)
{
    (void)d;
    show_id = 0;
    if (!win || !anchor || !items) { DBG("not shown: no popup or no window(s) left"); return G_SOURCE_REMOVE; }
    if (!gtk_widget_get_visible(anchor)) { DBG("not shown: the button went away while we waited"); return G_SOURCE_REMOVE; }
    rebuild();
    place();
    mute_tip(anchor);
    gtk_widget_show(win);
    if (!hde_wl_is_layer(GTK_WINDOW(win)) && gtk_widget_get_window(win))
        gdk_window_raise(gtk_widget_get_window(win));
    if (debug_on()) g_printerr("hde-panel: peek: shown\n");
    return G_SOURCE_REMOVE;
}

static gboolean hide_now(gpointer d)
{
    (void)d;
    hide_id = 0;
    hde_peek_hide();
    return G_SOURCE_REMOVE;
}

/* ---------------------------------------------------------------- what the taskbar calls */

void hde_peek_hover(GtkWidget *a, const char *gtitle, const HdePeekItem *its, int n,
                    const HdePeekOps *o, gpointer data, int delay_ms)
{
    if (!a || !its || n <= 0 || !o) { hde_peek_leave(); return; }
    ensure_window();
    if (hide_id) { g_source_remove(hide_id); hide_id = 0; }
    if (show_id) { g_source_remove(show_id); show_id = 0; }

    gboolean already = hde_peek_visible();
    items_free();
    set_anchor(a);
    ops = o;
    ops_data = data;
    g_free(group_title);
    group_title = g_strdup(gtitle ? gtitle : "");
    for (int i = 0; i < n; i++) {
        Item *it = g_new0(Item, 1);
        it->title = g_strdup(its[i].title ? its[i].title : "");
        it->icon = its[i].icon ? g_object_ref(its[i].icon) : NULL;
        it->handle = its[i].handle;
        items = g_list_append(items, it);
    }
    DBG("hover: %d window(s), %s in %d ms", n, already ? "a preview is open: at once" : "waited for", delay_ms);
    /* already open for another button: show the new windows at once, as Windows does */
    if (already || delay_ms <= 0) show_now(NULL);
    else show_id = g_timeout_add(delay_ms, show_now, NULL);
}

void hde_peek_leave(void)
{
    if (show_id) { g_source_remove(show_id); show_id = 0; DBG("left before it opened"); }
    if (!hde_peek_visible()) { items_free(); return; }
    if (!hide_id) { hide_id = g_timeout_add(HIDE_GRACE_MS, hide_now, NULL); DBG("left: closing in %d ms", HIDE_GRACE_MS); }
}

void hde_peek_hide(void)
{
    if (show_id) { g_source_remove(show_id); show_id = 0; }
    if (hide_id) { g_source_remove(hide_id); hide_id = 0; }
    gboolean was = win != NULL && gtk_widget_get_visible(win);
    if (was) gtk_widget_hide(win);
    mute_tip(NULL);
    items_free();
    if (was && debug_on()) g_printerr("hde-panel: peek: hidden\n");
}

gboolean hde_peek_visible(void)
{
    return win != NULL && gtk_widget_get_visible(win);
}
