/* hde-tray: system tray for hde-panel
 *  - XEmbed (_NET_SYSTEM_TRAY_Sn): GtkStatusIcon and older apps
 *  - StatusNotifierItem (org.kde.StatusNotifierWatcher) + com.canonical.dbusmenu: Electron, Telegram, nm-applet...
 */
#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <gtk/gtkx.h>
#include <gio/gio.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "hde-tray.h"

#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

#define ICON_SIZE 18
#define MENU_ICON_SIZE 16
#define WATCHER_NAME "org.kde.StatusNotifierWatcher"
#define WATCHER_NAME_FD "org.freedesktop.StatusNotifierWatcher"
#define WATCHER_PATH "/StatusNotifierWatcher"
#define SNI_IFACE "org.kde.StatusNotifierItem"
#define DBUSMENU_IFACE "com.canonical.dbusmenu"

static GtkWidget *tray_box;
static GDBusConnection *bus;
static GHashTable *items;               /* key -> Item* */
static GDBusNodeInfo *watcher_info;
static gboolean is_watcher;             /* TRUE: hde-panel holds the watcher name */

/* =====================================================================
 *  XEmbed
 * ===================================================================== */
static Atom opcode_atom;

static void xembed_dock(Window w)
{
    GtkWidget *sock = gtk_socket_new();
    gtk_widget_set_size_request(sock, ICON_SIZE + 4, ICON_SIZE + 4);
    gtk_box_pack_start(GTK_BOX(tray_box), sock, FALSE, FALSE, 0);
    gtk_widget_show(sock);
    gtk_widget_realize(sock);
    gtk_socket_add_id(GTK_SOCKET(sock), w);   /* the default plug-removed handler destroys the socket */
}

static GdkFilterReturn xembed_filter(GdkXEvent *xev, GdkEvent *ev, gpointer data)
{
    XEvent *xe = xev;
    if (xe->type == ClientMessage && xe->xclient.message_type == opcode_atom &&
        xe->xclient.data.l[1] == 0 /* SYSTEM_TRAY_REQUEST_DOCK */) {
        xembed_dock((Window)xe->xclient.data.l[2]);
        return GDK_FILTER_REMOVE;
    }
    return GDK_FILTER_CONTINUE;
}

static void xembed_init(void)
{
    GdkDisplay *gd = gdk_display_get_default();
    if (!GDK_IS_X11_DISPLAY(gd)) return;
    Display *dpy = GDK_DISPLAY_XDISPLAY(gd);
    GdkScreen *scr = gdk_display_get_default_screen(gd);
    int sn = gdk_x11_screen_get_screen_number(scr);

    GtkWidget *owner = gtk_invisible_new();
    gtk_widget_realize(owner);
    GdkWindow *gw = gtk_widget_get_window(owner);
    Window xid = GDK_WINDOW_XID(gw);

    char name[64];
    g_snprintf(name, sizeof name, "_NET_SYSTEM_TRAY_S%d", sn);
    Atom sel = XInternAtom(dpy, name, False);
    opcode_atom = XInternAtom(dpy, "_NET_SYSTEM_TRAY_OPCODE", False);

    if (XGetSelectionOwner(dpy, sel) != None) {
        g_printerr("hde-panel: another system tray is already running; skipping XEmbed\n");
        return;
    }
    XSetSelectionOwner(dpy, sel, xid, CurrentTime);
    if (XGetSelectionOwner(dpy, sel) != xid) return;

    long orient = 0; /* ngang */
    XChangeProperty(dpy, xid, XInternAtom(dpy, "_NET_SYSTEM_TRAY_ORIENTATION", False),
                    XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&orient, 1);

    gdk_window_add_filter(gw, xembed_filter, NULL);

    XEvent ev = { 0 };
    ev.xclient.type = ClientMessage;
    ev.xclient.window = RootWindow(dpy, sn);
    ev.xclient.message_type = XInternAtom(dpy, "MANAGER", False);
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = CurrentTime;
    ev.xclient.data.l[1] = sel;
    ev.xclient.data.l[2] = xid;
    XSendEvent(dpy, RootWindow(dpy, sn), False, StructureNotifyMask, &ev);
    XFlush(dpy);
}

/* =====================================================================
 *  StatusNotifierItem
 * ===================================================================== */
typedef struct {
    int ref;
    char *key, *bus_name, *path, *menu_path;
    gboolean is_menu;
    int last_x, last_y;
    GdkEvent *press;                 /* last mouse event, used as the trigger for the popup menu */
    GtkWidget *button, *image;
    guint sub_id, watch_id;
    GCancellable *cancel;
} Item;

static Item *item_ref(Item *it) { it->ref++; return it; }

static void item_unref(Item *it)
{
    if (--it->ref > 0) return;
    if (it->press) gdk_event_free(it->press);
    g_free(it->key); g_free(it->bus_name); g_free(it->path); g_free(it->menu_path);
    g_object_unref(it->cancel);
    g_free(it);
}

static void add_theme_path(const char *dir)
{
    GtkIconTheme *th = gtk_icon_theme_get_default();
    gchar **paths = NULL; gint n = 0;
    gtk_icon_theme_get_search_path(th, &paths, &n);
    gboolean have = FALSE;
    for (gint i = 0; i < n; i++) if (!strcmp(paths[i], dir)) have = TRUE;
    g_strfreev(paths);
    if (!have) gtk_icon_theme_append_search_path(th, dir);
}

static void free_pixels(guchar *px, gpointer d) { g_free(px); }

static GdkPixbuf *pixbuf_from_pixmaps(GVariant *arr)
{
    GVariantIter it;
    gint32 w, h, bw = 0, bh = 0;
    GVariant *bytes, *best = NULL;
    g_variant_iter_init(&it, arr);
    while (g_variant_iter_next(&it, "(ii@ay)", &w, &h, &bytes)) {
        if (w > 0 && h > 0 && w > bw) {
            if (best) g_variant_unref(best);
            best = bytes; bw = w; bh = h;
        } else g_variant_unref(bytes);
    }
    if (!best) return NULL;

    gsize len = 0;
    const guchar *src = g_variant_get_fixed_array(best, &len, 1);
    GdkPixbuf *out = NULL;
    if (len >= (gsize)bw * bh * 4) {
        guchar *dst = g_malloc((gsize)bw * bh * 4);
        for (gsize i = 0; i < (gsize)bw * bh; i++) {      /* ARGB -> RGBA */
            dst[4*i]   = src[4*i+1];
            dst[4*i+1] = src[4*i+2];
            dst[4*i+2] = src[4*i+3];
            dst[4*i+3] = src[4*i];
        }
        GdkPixbuf *full = gdk_pixbuf_new_from_data(dst, GDK_COLORSPACE_RGB, TRUE, 8, bw, bh, bw * 4,
                                                   free_pixels, NULL);
        out = gdk_pixbuf_scale_simple(full, ICON_SIZE, ICON_SIZE, GDK_INTERP_HYPER);
        g_object_unref(full);
    }
    g_variant_unref(best);
    return out;
}

static void apply_props(Item *it, GVariant *r)
{
    GVariant *dict = NULL;
    const char *icon = NULL, *att = NULL, *status = NULL, *theme = NULL, *menu = NULL, *title = NULL;
    gboolean is_menu = FALSE;
    GVariant *pix = NULL;

    if (r) {
        g_variant_get(r, "(@a{sv})", &dict);
        g_variant_lookup(dict, "IconName", "&s", &icon);
        g_variant_lookup(dict, "AttentionIconName", "&s", &att);
        g_variant_lookup(dict, "Status", "&s", &status);
        g_variant_lookup(dict, "IconThemePath", "&s", &theme);
        g_variant_lookup(dict, "Menu", "&o", &menu);
        g_variant_lookup(dict, "Title", "&s", &title);
        g_variant_lookup(dict, "ItemIsMenu", "b", &is_menu);
        pix = g_variant_lookup_value(dict, "IconPixmap", G_VARIANT_TYPE("a(iiay)"));
    }

    if (theme && *theme) add_theme_path(theme);

    const char *name = icon;
    if (status && !strcmp(status, "NeedsAttention") && att && *att) name = att;

    GdkPixbuf *pb = NULL;
    if (name && *name) {
        if (g_path_is_absolute(name))
            pb = gdk_pixbuf_new_from_file_at_size(name, ICON_SIZE, ICON_SIZE, NULL);
        else
            pb = gtk_icon_theme_load_icon(gtk_icon_theme_get_default(), name, ICON_SIZE,
                                          GTK_ICON_LOOKUP_FORCE_SIZE, NULL);
    }
    if (!pb && pix) pb = pixbuf_from_pixmaps(pix);
    if (!pb) pb = gtk_icon_theme_load_icon(gtk_icon_theme_get_default(), "application-x-executable",
                                           ICON_SIZE, GTK_ICON_LOOKUP_FORCE_SIZE, NULL);
    if (pb) { gtk_image_set_from_pixbuf(GTK_IMAGE(it->image), pb); g_object_unref(pb); }

    g_free(it->menu_path);
    it->menu_path = (menu && strcmp(menu, "/") != 0 && *menu) ? g_strdup(menu) : NULL;
    it->is_menu = is_menu;
    gtk_widget_set_tooltip_text(it->button, (title && *title) ? title : NULL);
    gtk_widget_set_visible(it->button, !(status && !strcmp(status, "Passive")));

    if (pix) g_variant_unref(pix);
    if (dict) g_variant_unref(dict);
}

static void on_props(GObject *src, GAsyncResult *res, gpointer data)
{
    Item *it = data;
    GError *err = NULL;
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);
    if (!g_cancellable_is_cancelled(it->cancel)) apply_props(it, r);
    if (r) g_variant_unref(r);
    g_clear_error(&err);
    item_unref(it);
}

static void refresh(Item *it)
{
    g_dbus_connection_call(bus, it->bus_name, it->path, "org.freedesktop.DBus.Properties", "GetAll",
                           g_variant_new("(s)", SNI_IFACE), G_VARIANT_TYPE("(a{sv})"),
                           G_DBUS_CALL_FLAGS_NONE, 3000, it->cancel, on_props, item_ref(it));
}

static void on_item_signal(GDBusConnection *c, const gchar *sender, const gchar *path, const gchar *iface,
                           const gchar *sig, GVariant *params, gpointer data)
{
    if (g_str_has_prefix(sig, "New")) refresh(data);
}

/* ---------- menu (dbusmenu) ---------- */
/* icon-name (theme name or absolute path) or icon-data (PNG) of a menu item */
static GdkPixbuf *menu_icon(const char *name, GVariant *data)
{
    GdkPixbuf *pb = NULL;
    if (name && *name) {
        if (g_path_is_absolute(name))
            pb = gdk_pixbuf_new_from_file_at_size(name, MENU_ICON_SIZE, MENU_ICON_SIZE, NULL);
        else {
            GtkIconTheme *th = gtk_icon_theme_get_default();
            pb = gtk_icon_theme_load_icon(th, name, MENU_ICON_SIZE, GTK_ICON_LOOKUP_FORCE_SIZE, NULL);
            if (!pb && !g_str_has_suffix(name, "-symbolic")) {      /* the theme only has the symbolic variant */
                char *sym = g_strconcat(name, "-symbolic", NULL);
                pb = gtk_icon_theme_load_icon(th, sym, MENU_ICON_SIZE, GTK_ICON_LOOKUP_FORCE_SIZE, NULL);
                g_free(sym);
            }
        }
    }
    if (!pb && data) {
        gsize len = 0;
        const guchar *buf = g_variant_get_fixed_array(data, &len, 1);
        if (buf && len > 0) {
            GBytes *bytes = g_bytes_new(buf, len);
            GInputStream *st = g_memory_input_stream_new_from_bytes(bytes);
            pb = gdk_pixbuf_new_from_stream_at_scale(st, MENU_ICON_SIZE, MENU_ICON_SIZE, TRUE, NULL, NULL);
            g_object_unref(st);
            g_bytes_unref(bytes);
        }
    }
    return pb;
}

static GtkWidget *menu_item_with_icon(const char *label, gboolean check, GdkPixbuf *pb)
{
    GtkWidget *mi = check ? gtk_check_menu_item_new() : gtk_menu_item_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *img = gtk_image_new_from_pixbuf(pb);
    GtkWidget *lbl = gtk_label_new_with_mnemonic(label);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0);
    gtk_label_set_mnemonic_widget(GTK_LABEL(lbl), mi);
    gtk_box_pack_start(GTK_BOX(box), img, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), lbl, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(mi), box);
    return mi;
}
static void on_dbusmenu_activate(GtkMenuItem *mi, gpointer d)
{
    Item *it = g_object_get_data(G_OBJECT(mi), "item");
    gint32 id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(mi), "id"));
    if (!it || !it->menu_path) return;
    g_dbus_connection_call(bus, it->bus_name, it->menu_path, DBUSMENU_IFACE, "Event",
                           g_variant_new("(isvu)", id, "clicked", g_variant_new_int32(0), (guint32)time(NULL)),
                           NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

static void fill_menu(Item *it, GtkWidget *menu, GVariant *kids)
{
    GVariantIter iter;
    GVariant *v;
    g_variant_iter_init(&iter, kids);
    while ((v = g_variant_iter_next_value(&iter))) {
        GVariant *c = g_variant_get_variant(v);
        if (!g_variant_is_of_type(c, G_VARIANT_TYPE("(ia{sv}av)"))) {   /* the app sent data of the wrong type */
            g_variant_unref(c); g_variant_unref(v);
            continue;
        }
        gint32 id;
        GVariant *props, *sub;
        g_variant_get(c, "(i@a{sv}@av)", &id, &props, &sub);

        const char *type = NULL, *label = NULL, *toggle = NULL, *disp = NULL, *icon_name = NULL;
        gboolean visible = TRUE, enabled = TRUE;
        gint32 state = 0;
        g_variant_lookup(props, "type", "&s", &type);
        g_variant_lookup(props, "label", "&s", &label);
        g_variant_lookup(props, "toggle-type", "&s", &toggle);
        g_variant_lookup(props, "children-display", "&s", &disp);
        g_variant_lookup(props, "visible", "b", &visible);
        g_variant_lookup(props, "enabled", "b", &enabled);
        g_variant_lookup(props, "toggle-state", "i", &state);
        g_variant_lookup(props, "icon-name", "&s", &icon_name);

        if (visible) {
            GtkWidget *mi;
            if (type && !strcmp(type, "separator")) {
                mi = gtk_separator_menu_item_new();
            } else {
                const char *l = label ? label : "";
                gboolean is_check = toggle && *toggle;
                GVariant *idata = g_variant_lookup_value(props, "icon-data", G_VARIANT_TYPE("ay"));
                GdkPixbuf *ipb = menu_icon(icon_name, idata);
                if (idata) g_variant_unref(idata);
                if (ipb) {
                    mi = menu_item_with_icon(l, is_check, ipb);
                    g_object_unref(ipb);
                } else if (is_check) {
                    mi = gtk_check_menu_item_new_with_mnemonic(l);
                } else {
                    mi = gtk_menu_item_new_with_mnemonic(l);
                }
                if (is_check) {
                    if (!strcmp(toggle, "radio"))
                        gtk_check_menu_item_set_draw_as_radio(GTK_CHECK_MENU_ITEM(mi), TRUE);
                    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(mi), state == 1);
                }
                gtk_widget_set_sensitive(mi, enabled);
                if (disp && !strcmp(disp, "submenu")) {
                    GtkWidget *sm = gtk_menu_new();
                    fill_menu(it, sm, sub);
                    gtk_menu_item_set_submenu(GTK_MENU_ITEM(mi), sm);
                } else {
                    g_object_set_data_full(G_OBJECT(mi), "item", item_ref(it), (GDestroyNotify)item_unref);
                    g_object_set_data(G_OBJECT(mi), "id", GINT_TO_POINTER(id));
                    g_signal_connect(mi, "activate", G_CALLBACK(on_dbusmenu_activate), NULL);
                }
            }
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
        }
        g_variant_unref(props); g_variant_unref(sub);
        g_variant_unref(c); g_variant_unref(v);
    }
}

static void call_xy(Item *it, const char *method)
{
    g_dbus_connection_call(bus, it->bus_name, it->path, SNI_IFACE, method,
                           g_variant_new("(ii)", it->last_x, it->last_y),
                           NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

static gboolean destroy_idle(gpointer w) { gtk_widget_destroy(GTK_WIDGET(w)); return G_SOURCE_REMOVE; }
static void on_menu_done(GtkMenuShell *m, gpointer d) { g_idle_add(destroy_idle, m); }

static void on_layout(GObject *src, GAsyncResult *res, gpointer data)
{
    Item *it = data;
    GError *err = NULL;
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);
    gboolean shown = FALSE;

    if (r && gtk_widget_get_realized(it->button)) {
        guint32 rev;
        GVariant *layout;
        g_variant_get(r, "(u@(ia{sv}av))", &rev, &layout);
        gint32 id;
        GVariant *props, *kids;
        g_variant_get(layout, "(i@a{sv}@av)", &id, &props, &kids);

        GtkWidget *menu = gtk_menu_new();
        fill_menu(it, menu, kids);
        if (g_variant_n_children(kids) > 0) {
            gtk_widget_show_all(menu);
            g_signal_connect(menu, "selection-done", G_CALLBACK(on_menu_done), NULL);
            gtk_menu_popup_at_widget(GTK_MENU(menu), it->button, GDK_GRAVITY_NORTH_WEST,
                                     GDK_GRAVITY_SOUTH_WEST, it->press);
            shown = TRUE;
        } else gtk_widget_destroy(menu);

        g_variant_unref(props); g_variant_unref(kids);
        g_variant_unref(layout);
    }
    if (!shown && !r) call_xy(it, "ContextMenu");   /* the app draws its own menu */
    if (r) g_variant_unref(r);
    g_clear_error(&err);
    item_unref(it);
}

static void on_about(GObject *src, GAsyncResult *res, gpointer data)
{
    Item *it = data;
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, NULL);
    if (r) g_variant_unref(r);
    g_dbus_connection_call(bus, it->bus_name, it->menu_path, DBUSMENU_IFACE, "GetLayout",
                           g_variant_new("(ii@as)", 0, -1, g_variant_new_strv(NULL, 0)), G_VARIANT_TYPE("(u(ia{sv}av))"),
                           G_DBUS_CALL_FLAGS_NONE, 3000, NULL, on_layout, it);
}

static void show_menu(Item *it)
{
    if (!it->menu_path) { call_xy(it, "ContextMenu"); return; }
    g_dbus_connection_call(bus, it->bus_name, it->menu_path, DBUSMENU_IFACE, "AboutToShow",
                           g_variant_new("(i)", 0), NULL, G_DBUS_CALL_FLAGS_NONE, 1500, NULL,
                           on_about, item_ref(it));
}

static gboolean on_btn_press(GtkWidget *w, GdkEventButton *e, gpointer data)
{
    Item *it = data;
    it->last_x = (int)e->x_root;
    it->last_y = (int)e->y_root;
    if (it->press) gdk_event_free(it->press);
    it->press = gdk_event_copy((GdkEvent *)e);
    if (e->button == 1 && !it->is_menu) call_xy(it, "Activate");
    else if (e->button == 2) call_xy(it, "SecondaryActivate");
    else show_menu(it);
    return TRUE;
}

/* ---------- item lifecycle ---------- */
static void emit_watcher_signal(const char *sig, const char *key)
{
    if (!is_watcher) return;          /* client mode: the other watcher emits the signals itself */
    g_dbus_connection_emit_signal(bus, NULL, WATCHER_PATH, "org.kde.StatusNotifierWatcher", sig,
                                  key ? g_variant_new("(s)", key) : NULL, NULL);
}

static void item_remove(Item *it)
{
    emit_watcher_signal("StatusNotifierItemUnregistered", it->key);
    g_hash_table_remove(items, it->key);
    g_cancellable_cancel(it->cancel);
    if (it->sub_id)   g_dbus_connection_signal_unsubscribe(bus, it->sub_id);
    if (it->watch_id) g_bus_unwatch_name(it->watch_id);
    gtk_widget_destroy(it->button);
    item_unref(it);
}

static void on_name_vanished(GDBusConnection *c, const gchar *name, gpointer data)
{
    item_remove(data);
}

static void item_add(const char *bus_name, const char *path, const char *key)
{
    Item *it = g_new0(Item, 1);
    it->ref = 1;
    it->key = g_strdup(key);
    it->bus_name = g_strdup(bus_name);
    it->path = g_strdup(path);
    it->cancel = g_cancellable_new();

    it->button = gtk_button_new();
    gtk_button_set_relief(GTK_BUTTON(it->button), GTK_RELIEF_NONE);
    it->image = gtk_image_new_from_icon_name("application-x-executable", GTK_ICON_SIZE_MENU);
    gtk_image_set_pixel_size(GTK_IMAGE(it->image), ICON_SIZE);
    gtk_container_add(GTK_CONTAINER(it->button), it->image);
    g_signal_connect(it->button, "button-press-event", G_CALLBACK(on_btn_press), it);
    gtk_box_pack_start(GTK_BOX(tray_box), it->button, FALSE, FALSE, 0);
    gtk_widget_show_all(it->button);

    g_hash_table_insert(items, g_strdup(key), it);

    it->sub_id = g_dbus_connection_signal_subscribe(bus, bus_name, SNI_IFACE, NULL, path, NULL,
                                                    G_DBUS_SIGNAL_FLAGS_NONE, on_item_signal,
                                                    item_ref(it), (GDestroyNotify)item_unref);
    it->watch_id = g_bus_watch_name_on_connection(bus, bus_name, G_BUS_NAME_WATCHER_FLAGS_NONE,
                                                  NULL, on_name_vanished, it, NULL);
    refresh(it);
}

/* ---------- StatusNotifierWatcher ---------- */
static const char watcher_xml[] =
    "<node><interface name='org.kde.StatusNotifierWatcher'>"
    " <method name='RegisterStatusNotifierItem'><arg type='s' direction='in'/></method>"
    " <method name='RegisterStatusNotifierHost'><arg type='s' direction='in'/></method>"
    " <property name='RegisteredStatusNotifierItems' type='as' access='read'/>"
    " <property name='IsStatusNotifierHostRegistered' type='b' access='read'/>"
    " <property name='ProtocolVersion' type='i' access='read'/>"
    " <signal name='StatusNotifierItemRegistered'><arg type='s'/></signal>"
    " <signal name='StatusNotifierItemUnregistered'><arg type='s'/></signal>"
    " <signal name='StatusNotifierHostRegistered'/>"
    " <signal name='StatusNotifierHostUnregistered'/>"
    "</interface></node>";

static void register_item(const char *sender, const char *svc)
{
    char *bus_name, *path;
    const char *slash;
    if (svc[0] == '/' && !sender) return;   /* unknown sender app */
    if (svc[0] == '/') { bus_name = g_strdup(sender); path = g_strdup(svc); }
    else if ((slash = strchr(svc, '/'))) { bus_name = g_strndup(svc, slash - svc); path = g_strdup(slash); }
    else { bus_name = g_strdup(svc); path = g_strdup("/StatusNotifierItem"); }

    /* Items from the host's watcher may have invalid names -> GDBus prints a flood of GLib-GIO-CRITICAL. */
    if (!bus_name || !g_dbus_is_name(bus_name)) {
        g_free(bus_name); g_free(path);
        return;
    }
    char *key = g_strdup_printf("%s%s", bus_name, path);
    if (!g_hash_table_contains(items, key)) {
        item_add(bus_name, path, key);
        emit_watcher_signal("StatusNotifierItemRegistered", key);
    }
    g_free(bus_name); g_free(path); g_free(key);
}

static void watcher_method(GDBusConnection *c, const gchar *sender, const gchar *obj, const gchar *iface,
                           const gchar *method, GVariant *params, GDBusMethodInvocation *inv, gpointer ud)
{
    if (!strcmp(method, "RegisterStatusNotifierItem")) {
        const char *svc;
        g_variant_get(params, "(&s)", &svc);
        register_item(sender, svc);
    }
    g_dbus_method_invocation_return_value(inv, NULL);
}

static GVariant *watcher_get(GDBusConnection *c, const gchar *sender, const gchar *obj, const gchar *iface,
                             const gchar *prop, GError **err, gpointer ud)
{
    if (!strcmp(prop, "RegisteredStatusNotifierItems")) {
        GVariantBuilder b;
        GHashTableIter it;
        gpointer k;
        g_variant_builder_init(&b, G_VARIANT_TYPE("as"));
        g_hash_table_iter_init(&it, items);
        while (g_hash_table_iter_next(&it, &k, NULL)) g_variant_builder_add(&b, "s", (const char *)k);
        return g_variant_builder_end(&b);
    }
    if (!strcmp(prop, "IsStatusNotifierHostRegistered")) return g_variant_new_boolean(TRUE);
    if (!strcmp(prop, "ProtocolVersion")) return g_variant_new_int32(0);
    return NULL;
}

/* ---------- client mode: another watcher already exists (xfce4-panel, plasma, ...) ---------- */
static guint client_watch_id, host_own_id, client_sub_reg, client_sub_unreg;
static gboolean host_ready, client_registered;

static void client_stop(void)
{
    if (client_sub_reg)   g_dbus_connection_signal_unsubscribe(bus, client_sub_reg);
    if (client_sub_unreg) g_dbus_connection_signal_unsubscribe(bus, client_sub_unreg);
    client_sub_reg = client_sub_unreg = 0;
    client_registered = FALSE;
}

static void on_remote_registered(GDBusConnection *c, const gchar *sender, const gchar *path, const gchar *iface,
                                 const gchar *sig, GVariant *params, gpointer data)
{
    const char *svc = NULL;
    g_variant_get(params, "(&s)", &svc);
    if (svc) register_item(NULL, svc);
}

static void on_remote_unregistered(GDBusConnection *c, const gchar *sender, const gchar *path, const gchar *iface,
                                   const gchar *sig, GVariant *params, gpointer data)
{
    const char *svc = NULL;
    g_variant_get(params, "(&s)", &svc);
    if (!svc) return;
    /* normalize like register_item: "bus/path" or only "bus" */
    char *key = strchr(svc, '/') ? g_strdup(svc) : g_strdup_printf("%s/StatusNotifierItem", svc);
    Item *it = g_hash_table_lookup(items, key);
    if (it) item_remove(it);
    g_free(key);
}

static void on_remote_items(GObject *src, GAsyncResult *res, gpointer data)
{
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, NULL);
    if (!r) return;
    GVariant *boxed, *arr;
    g_variant_get(r, "(v)", &boxed);
    arr = boxed;
    GVariantIter iter;
    const char *svc;
    g_variant_iter_init(&iter, arr);
    while (g_variant_iter_next(&iter, "&s", &svc)) register_item(NULL, svc);
    g_variant_unref(boxed);
    g_variant_unref(r);
}

static void client_try_register(void)
{
    if (is_watcher || client_registered || !host_ready) return;
    client_registered = TRUE;

    client_sub_reg = g_dbus_connection_signal_subscribe(bus, WATCHER_NAME, WATCHER_NAME, "StatusNotifierItemRegistered",
                                                        WATCHER_PATH, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
                                                        on_remote_registered, NULL, NULL);
    client_sub_unreg = g_dbus_connection_signal_subscribe(bus, WATCHER_NAME, WATCHER_NAME, "StatusNotifierItemUnregistered",
                                                          WATCHER_PATH, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
                                                          on_remote_unregistered, NULL, NULL);

    char *host = g_strdup_printf("org.kde.StatusNotifierHost-%d", (int)getpid());
    g_dbus_connection_call(bus, WATCHER_NAME, WATCHER_PATH, WATCHER_NAME, "RegisterStatusNotifierHost",
                           g_variant_new("(s)", host), NULL, G_DBUS_CALL_FLAGS_NONE, 3000, NULL, NULL, NULL);
    g_free(host);

    g_dbus_connection_call(bus, WATCHER_NAME, WATCHER_PATH, "org.freedesktop.DBus.Properties", "Get",
                           g_variant_new("(ss)", WATCHER_NAME, "RegisteredStatusNotifierItems"),
                           G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 3000, NULL, on_remote_items, NULL);
}

static void on_host_acquired(GDBusConnection *c, const gchar *name, gpointer d)
{
    host_ready = TRUE;
    client_try_register();
}

static void on_watcher_appeared(GDBusConnection *c, const gchar *name, const gchar *owner, gpointer d)
{
    if (is_watcher || !g_strcmp0(owner, g_dbus_connection_get_unique_name(bus))) return;
    client_try_register();
}

static void on_watcher_vanished(GDBusConnection *c, const gchar *name, gpointer d)
{
    client_stop();      /* the items stay alive; if we become the watcher, the apps register again */
}

static void start_client(void)
{
    if (client_watch_id) return;
    g_printerr("hde-panel: another StatusNotifierWatcher is already running; switching to client mode\n");
    char *host = g_strdup_printf("org.kde.StatusNotifierHost-%d", (int)getpid());
    host_own_id = g_bus_own_name_on_connection(bus, host, G_BUS_NAME_OWNER_FLAGS_NONE,
                                               on_host_acquired, NULL, NULL, NULL);
    g_free(host);
    client_watch_id = g_bus_watch_name_on_connection(bus, WATCHER_NAME, G_BUS_NAME_WATCHER_FLAGS_NONE,
                                                     on_watcher_appeared, on_watcher_vanished, NULL, NULL);
}

/* ---------- competing for the watcher name ---------- */
static void on_name_acquired(GDBusConnection *c, const gchar *name, gpointer d)
{
    if (strcmp(name, WATCHER_NAME) != 0) return;
    /* GDBus queues for the name: if the old watcher exits, we take over and end up here */
    client_stop();
    is_watcher = TRUE;
    emit_watcher_signal("StatusNotifierHostRegistered", NULL);
}

static void on_name_lost(GDBusConnection *c, const gchar *name, gpointer d)
{
    if (strcmp(name, WATCHER_NAME) != 0) return;
    is_watcher = FALSE;
    start_client();
}

static void sni_init(void)
{
    bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    if (!bus) { g_printerr("hde-panel: no D-Bus session; skipping StatusNotifier\n"); return; }

    watcher_info = g_dbus_node_info_new_for_xml(watcher_xml, NULL);
    static const GDBusInterfaceVTable vt = { watcher_method, watcher_get, NULL, { 0 } };
    g_dbus_connection_register_object(bus, WATCHER_PATH, watcher_info->interfaces[0],
                                      &vt, NULL, NULL, NULL);
    g_bus_own_name_on_connection(bus, WATCHER_NAME, G_BUS_NAME_OWNER_FLAGS_NONE,
                                 on_name_acquired, on_name_lost, NULL, NULL);
    g_bus_own_name_on_connection(bus, WATCHER_NAME_FD, G_BUS_NAME_OWNER_FLAGS_NONE,
                                 NULL, NULL, NULL, NULL);
}

/* ===================================================================== */
GtkWidget *hde_tray_new(void)
{
    tray_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    items = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    xembed_init();
    sni_init();
    return tray_box;
}
