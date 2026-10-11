/* hde-switch.c — see hde-switch.h: the window that Alt+Tab opens.
 *
 * It is a popup like the Control Center's and the taskbar's preview (src/hde-flyout.c, src/hde-peek.c): a frame
 * with the windows of this session in it, one picture each with the name under it, the one that is picked marked.
 * It takes no keyboard: hde-hotkeys holds Alt and Tab (it grabs the keyboard while the switcher is open) and says
 * over the panel's command channel (src/hde-ipc.h) which way to move and when to stop.
 *
 * The pictures are read once, when it opens, and kept until it closes: a window that is picked is the window as
 * it looked when Alt was pressed, not as it looks after every press of Tab.
 */

#include "hde-switch.h"

#include "hde-osd.h"                 /* hde_popup_setup_alpha: rounded corners where the screen is composited */
#include "hde-panel-config.h"        /* hde_panel_reserved: how much of the screen the panel takes */
#include "hde-switch-core.h"
#include "hde-wl.h"                  /* hde_popup_window_new / hde_popup_move / hde_main_monitor */

#include <string.h>

/* the most windows the switcher shows: there is no screen on which more pictures could be told apart */
#define MAX_WINDOWS 24
/* below this a picture is not worth drawing (hde_switch_layout says so): the icon and the name are shown */
#define THUMB_MIN HDE_SWITCH_THUMB_MIN
/* the icon of an application where there is no picture, and the room it is given */
#define ICON_PX 48
#define NO_PIC_W 160
#define NO_PIC_H 90

typedef struct {
    char *title;                     /* ours */
    GdkPixbuf *icon;                 /* ours: the taskbar's, kept alive while it is shown */
    GdkPixbuf *picture;              /* ours: read from the screen when the switcher opened */
    gpointer handle;
    GtkWidget *box, *image, *label;  /* the frame of this window in the grid */
} Item;

static GtkWidget *win;               /* the popup, kept between two presses of Alt+Tab */
static GtkWidget *content;           /* the box inside it */
static Item items[MAX_WINDOWS];
static int n_items;
static int sel = -1;                 /* which window is picked */
static guint32 key_ts;               /* the time of the key press that opened it */
static int (*list_windows)(HdeSwitchItem *out, int max);
static const HdeSwitchOps *ops;
static gpointer ops_data;

static void mark(void);                /* which window is picked */

static gboolean debug_on(void) { return g_getenv("HDE_DEBUG") != NULL; }
#define DBG(...) do { if (debug_on()) { g_printerr("hde-panel: switcher: " __VA_ARGS__); g_printerr("\n"); } } while (0)

/* ---------------------------------------------------------------- the popup window */

static void ensure_window(void)
{
    if (win) return;
    win = hde_popup_window_new("hde-switch", HDE_LAYER_OVERLAY, HDE_EDGE_ALL, HDE_KB_NONE);
    gtk_window_set_title(GTK_WINDOW(win), "hde-switch");
    gtk_window_set_resizable(GTK_WINDOW(win), FALSE);
    gtk_window_set_accept_focus(GTK_WINDOW(win), FALSE);
    hde_popup_setup_alpha(win);
    gtk_style_context_add_class(gtk_widget_get_style_context(win), "hde-switch");
    content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, HDE_SWITCH_GAP);
    gtk_container_set_border_width(GTK_CONTAINER(content), HDE_SWITCH_PAD);
    gtk_container_add(GTK_CONTAINER(win), content);
}

/* ---------------------------------------------------------------- the list of windows */

static void items_free(void)
{
    for (int i = 0; i < n_items; i++) {
        g_free(items[i].title);
        if (items[i].icon) g_object_unref(items[i].icon);
        if (items[i].picture) g_object_unref(items[i].picture);
    }
    memset(items, 0, sizeof items);
    n_items = 0;
    sel = -1;
}

/* read the list once, and a picture of every window in it (that is what takes the time, so only on opening) */
static void items_fill(void)
{
    items_free();
    if (!list_windows) return;
    HdeSwitchItem list[MAX_WINDOWS];
    int n = list_windows(list, MAX_WINDOWS);
    if (n > MAX_WINDOWS) n = MAX_WINDOWS;
    for (int i = 0; i < n; i++) {
        items[i].title = g_strdup(list[i].title && *list[i].title ? list[i].title : " ");
        items[i].icon = list[i].icon ? g_object_ref(list[i].icon) : NULL;
        items[i].handle = list[i].handle;
        items[i].picture = ops && ops->grab ? ops->grab(list[i].handle, ops_data) : NULL;
    }
    n_items = n;
    DBG("%d window(s)%s", n, n ? "" : ": nothing to switch to");
}

/* ---------------------------------------------------------------- one window in it */

static gboolean on_item_enter(GtkWidget *w, GdkEventCrossing *e, gpointer d)
{
    (void)w; (void)e;
    int i = GPOINTER_TO_INT(d);
    if (i < 0 || i >= n_items || i == sel) return FALSE;
    sel = i;                                   /* the mark follows the pointer, as it does in Windows */
    mark();
    DBG("the pointer picks %d (%s)", i, items[i].title);
    return FALSE;
}

static gboolean on_item_press(GtkWidget *w, GdkEventButton *e, gpointer d)
{
    (void)w;
    int i = GPOINTER_TO_INT(d);
    if (e->type != GDK_BUTTON_PRESS || e->button != 1 || i < 0 || i >= n_items) return FALSE;
    sel = i;
    hde_switch_end(FALSE, key_ts);
    return TRUE;
}

/* the picture, or what stands in for it: the icon of the application where a window has none to give (it is
 * minimized, or the screen is too small to draw pictures at all) */
static GtkWidget *picture(Item *it, int w, int h, gboolean with_pictures)
{
    GtkWidget *img;
    GdkPixbuf *pix = with_pictures ? it->picture : NULL;
    if (pix && gdk_pixbuf_get_width(pix) > 0 && gdk_pixbuf_get_height(pix) > 0) {
        int tw = 0, th = 0;
        hde_peek_thumb_size(gdk_pixbuf_get_width(pix), gdk_pixbuf_get_height(pix), w, h, &tw, &th);
        GdkPixbuf *small = (tw > 0 && th > 0) ? gdk_pixbuf_scale_simple(pix, tw, th, GDK_INTERP_BILINEAR) : NULL;
        img = gtk_image_new_from_pixbuf(small ? small : pix);
        if (small) g_object_unref(small);
    } else if (it->icon) {
        img = gtk_image_new_from_pixbuf(it->icon);
        gtk_image_set_pixel_size(GTK_IMAGE(img), ICON_PX);
        gtk_widget_set_size_request(img, NO_PIC_W, NO_PIC_H);
    } else {
        img = gtk_image_new_from_icon_name("application-x-executable", GTK_ICON_SIZE_DIALOG);
        gtk_image_set_pixel_size(GTK_IMAGE(img), ICON_PX);
        gtk_widget_set_size_request(img, NO_PIC_W, NO_PIC_H);
    }
    gtk_style_context_add_class(gtk_widget_get_style_context(img), "switch-picture");
    return img;
}

/* the room there is: the screen the panel is on, without the panel itself */
static void work_area(HdeRect *work)
{
    GdkRectangle mon = { 0, 0, 1024, 768 };
    GdkMonitor *m = hde_main_monitor();
    if (m) gdk_monitor_get_geometry(m, &mon);
    int top = 0, bottom = 0;
    hde_panel_reserved(&top, &bottom);
    work->x = mon.x;
    work->y = mon.y + top;
    work->w = mon.width;
    work->h = mon.height - top - bottom;
    if (work->h < 100) { work->y = mon.y; work->h = mon.height; }
}

static void build(void)
{
    GList *ch = gtk_container_get_children(GTK_CONTAINER(content));
    for (GList *l = ch; l; l = l->next) gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(ch);

    HdeRect work;
    work_area(&work);
    HdeSwitchLayout l;
    hde_switch_layout(n_items, work.w, work.h, &l);
    gboolean with_pictures = l.thumb_w >= THUMB_MIN;

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), HDE_SWITCH_GAP);
    gtk_grid_set_column_spacing(GTK_GRID(grid), HDE_SWITCH_GAP);
    for (int i = 0; i < n_items; i++) {
        Item *it = &items[i];
        it->box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        gtk_style_context_add_class(gtk_widget_get_style_context(it->box), "switch-item");
        it->image = picture(it, l.thumb_w, l.thumb_h, with_pictures);
        it->label = gtk_label_new(it->title);
        gtk_label_set_ellipsize(GTK_LABEL(it->label), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(it->label), with_pictures ? l.thumb_w / 8 : 30);
        gtk_style_context_add_class(gtk_widget_get_style_context(it->label), "switch-name");
        gtk_box_pack_start(GTK_BOX(it->box), it->image, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(it->box), it->label, FALSE, FALSE, 0);
        if (with_pictures) gtk_widget_set_size_request(it->image, l.thumb_w, l.thumb_h);

        GtkWidget *ev = gtk_event_box_new();
        gtk_widget_add_events(ev, GDK_BUTTON_PRESS_MASK | GDK_ENTER_NOTIFY_MASK);
        gtk_container_add(GTK_CONTAINER(ev), it->box);
        g_signal_connect(ev, "enter-notify-event", G_CALLBACK(on_item_enter), GINT_TO_POINTER(i));
        g_signal_connect(ev, "button-press-event", G_CALLBACK(on_item_press), GINT_TO_POINTER(i));
        gtk_grid_attach(GTK_GRID(grid), ev, i % l.cols, i / l.cols, 1, 1);
    }
    gtk_container_add(GTK_CONTAINER(content), grid);
    gtk_widget_show_all(content);
}

/* ---------------------------------------------------------------- where it goes, and what is picked */

static void place(void)
{
    HdeRect work;
    work_area(&work);
    GtkRequisition nat = { 0, 0 };
    gtk_widget_get_preferred_size(win, NULL, &nat);
    int x = 0, y = 0;
    hde_switch_place(&work, nat.width, nat.height, &x, &y);
    hde_popup_move(GTK_WINDOW(win), x, y);
    if (debug_on())
        g_printerr("hde-panel: switcher: %d window(s), %dx%d at %d,%d, %d picked (%s)\n", n_items, nat.width,
                   nat.height, x, y, sel, sel >= 0 && sel < n_items ? items[sel].title : "-");
}

static void mark(void)
{
    for (int i = 0; i < n_items; i++) {
        GtkStyleContext *sc = gtk_widget_get_style_context(items[i].box);
        if (i == sel) gtk_style_context_add_class(sc, "picked");
        else gtk_style_context_remove_class(sc, "picked");
    }
}

/* ---------------------------------------------------------------- what the panel and hde-hotkeys call */

void hde_switch_set_provider(int (*list)(HdeSwitchItem *out, int max), const HdeSwitchOps *o, gpointer data)
{
    list_windows = list;
    ops = o;
    ops_data = data;
}

void hde_switch_step(int step, guint32 timestamp)
{
    key_ts = timestamp;
    if (hde_switch_visible()) {
        sel = hde_switch_move(sel, n_items, step);
        mark();
        if (debug_on())
            g_printerr("hde-panel: switcher: %s: %d picked (%s)\n", step >= 0 ? "Tab" : "Shift+Tab", sel,
                       sel >= 0 && sel < n_items ? items[sel].title : "-");
        return;
    }
    ensure_window();
    items_fill();
    if (!n_items) { DBG("no windows to switch between"); return; }
    sel = hde_switch_start(n_items, step);
    if (sel < 0) sel = 0;                    /* one window: it is the only one there is */
    build();
    place();
    mark();
    gtk_widget_show(win);
    if (!hde_wl_is_layer(GTK_WINDOW(win)) && gtk_widget_get_window(win))
        gdk_window_raise(gtk_widget_get_window(win));
    DBG("shown");
}

void hde_switch_end(gboolean cancel, guint32 timestamp)
{
    if (!hde_switch_visible()) return;
    if (!cancel && sel >= 0 && sel < n_items && ops && ops->activate) {
        DBG("%s (%s)", cancel ? "cancelled" : "chosen", items[sel].title);
        ops->activate(items[sel].handle, ops_data, timestamp ? timestamp : key_ts);
    } else if (cancel) DBG("cancelled");
    hde_switch_hide();
}

void hde_switch_hide(void)
{
    if (win && gtk_widget_get_visible(win)) {
        gtk_widget_hide(win);
        DBG("hidden");
    }
    items_free();
}

gboolean hde_switch_visible(void)
{
    return win != NULL && gtk_widget_get_visible(win);
}
