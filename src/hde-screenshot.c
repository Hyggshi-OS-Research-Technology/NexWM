/* hde-screenshot — HDE's built-in screenshot tool (GTK3 + Xlib), so PrtSc works without scrot or any
 * other external program.
 *
 *   hde-screenshot              the whole screen (all monitors)
 *   hde-screenshot --area       drag a rectangle on a frozen copy of the screen (Esc / right click = cancel)
 *   hde-screenshot --window     the active window, including its title bar
 *   options:  --delay N         wait N seconds first
 *             --file PATH       save to PATH instead of ~/Pictures/Screenshots/Screenshot_<date>_<time>.png
 *             --no-clipboard    do not copy the image to the clipboard
 *             --no-notify       do not show a notification
 *
 * hde-hotkeys runs it for Print / Shift+Print / Alt+Print (Settings > Keyboard can pick another tool).
 * The PNG is saved, copied to the clipboard and announced with a notification ("Open" / "Show in Folder").
 * X11 clipboards live in the program that owns them, so the process stays in the background while it owns
 * the clipboard (until another program copies something, at most 10 minutes; a clipboard manager may keep
 * the image afterwards). The saved path is printed on stdout. Exit status: 0 saved, 1 cancelled, 2 error.
 */
#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <glib/gstdio.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum { MODE_FULL, MODE_AREA, MODE_WINDOW } Mode;

static Mode mode = MODE_FULL;
static int opt_delay;
static char *opt_file;
static gboolean opt_no_clip, opt_no_notify;

static int exit_code = 2;
static GdkPixbuf *result;               /* the final image (also served on the clipboard) */
static char *saved_path;
static gboolean clip_active, notif_active, exiting;
static GDBusConnection *bus;
static guint32 notif_id;

/* ---------------------------------------------------------------- helpers */
static void quit_if_idle(void)
{
    if (!exiting && !clip_active && !notif_active) {
        exiting = TRUE;
        gtk_main_quit();
    }
}

static gboolean on_give_up(gpointer data)
{
    (void)data;
    if (!exiting) {
        exiting = TRUE;
        gtk_main_quit();
    }
    return G_SOURCE_REMOVE;
}

static char *home_relative(const char *path)
{
    const char *home = g_get_home_dir();
    size_t n = home ? strlen(home) : 0;
    if (n > 1 && !strncmp(path, home, n) && path[n] == '/') return g_strconcat("~", path + n, NULL);
    return g_strdup(path);
}

static void notify_user(const char *summary, const char *body, const char *image_path, gboolean with_actions);

static void fail(const char *what)
{
    g_printerr("hde-screenshot: %s\n", what);
    if (!opt_no_notify) notify_user("Screenshot failed", what, NULL, FALSE);
    exit_code = 2;
    quit_if_idle();
}

/* ---------------------------------------------------------------- capture */
static GdkPixbuf *grab_root(int x, int y, int w, int h)
{
    GdkWindow *root = gdk_get_default_root_window();
    int sw = gdk_window_get_width(root), sh = gdk_window_get_height(root);
    if (w < 0 || h < 0) { x = 0; y = 0; w = sw; h = sh; }
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > sw) w = sw - x;
    if (y + h > sh) h = sh - y;
    if (w <= 0 || h <= 0) return NULL;
    return gdk_pixbuf_get_from_window(root, x, y, w, h);
}

static gboolean get_prop(Display *dpy, Window w, const char *name, Atom type, unsigned char **data, unsigned long *n)
{
    Atom actual;
    int fmt;
    unsigned long after;
    *data = NULL;
    *n = 0;
    if (XGetWindowProperty(dpy, w, XInternAtom(dpy, name, False), 0, 64, False, type, &actual, &fmt, n, &after,
                           data) != Success || !*data)
        return FALSE;
    if (*n == 0 || fmt != 32) {
        XFree(*data);
        *data = NULL;
        return FALSE;
    }
    return TRUE;
}

static gboolean get_cardinals(Display *dpy, Window w, const char *name, long out[4])
{
    unsigned char *d;
    unsigned long n;
    if (!get_prop(dpy, w, name, XA_CARDINAL, &d, &n)) return FALSE;
    gboolean ok = n >= 4;
    if (ok) for (int i = 0; i < 4; i++) out[i] = ((long *)d)[i];
    XFree(d);
    return ok;
}

static gboolean has_window_type(Display *dpy, Window w, const char *type)
{
    unsigned char *d;
    unsigned long n;
    if (!get_prop(dpy, w, "_NET_WM_WINDOW_TYPE", XA_ATOM, &d, &n)) return FALSE;
    Atom want = XInternAtom(dpy, type, False);
    gboolean r = FALSE;
    for (unsigned long i = 0; i < n; i++)
        if (((Atom *)d)[i] == want) r = TRUE;
    XFree(d);
    return r;
}

/* Screen rectangle of the active window as the user sees it (title bar and borders included). */
static gboolean active_window_rect(GdkRectangle *r)
{
    GdkDisplay *gd = gdk_display_get_default();
    Display *dpy = GDK_DISPLAY_XDISPLAY(gd);
    Window root = DefaultRootWindow(dpy), w = None;
    unsigned char *d;
    unsigned long n;
    gboolean ok = FALSE;

    gdk_x11_display_error_trap_push(gd);
    if (get_prop(dpy, root, "_NET_ACTIVE_WINDOW", XA_WINDOW, &d, &n)) {
        w = (Window)((long *)d)[0];
        XFree(d);
    }
    if (w == None) {                                  /* window manager without EWMH: keyboard focus */
        int rev;
        XGetInputFocus(dpy, &w, &rev);
        if (w == PointerRoot) w = None;
    }
    if (w != None && w != root && !has_window_type(dpy, w, "_NET_WM_WINDOW_TYPE_DESKTOP") &&
        !has_window_type(dpy, w, "_NET_WM_WINDOW_TYPE_DOCK")) {
        /* the WM frame = the ancestor whose parent is the root window */
        Window frame = w;
        for (int depth = 0; depth < 32; depth++) {
            Window rr, parent = None, *kids = NULL;
            unsigned nk = 0;
            if (!XQueryTree(dpy, frame, &rr, &parent, &kids, &nk)) break;
            if (kids) XFree(kids);
            if (parent == None || parent == root) break;
            frame = parent;
        }
        XWindowAttributes ca, fa;
        int cx = 0, cy = 0;
        Window child;
        if (XGetWindowAttributes(dpy, w, &ca) && ca.map_state == IsViewable &&
            XTranslateCoordinates(dpy, w, root, 0, 0, &cx, &cy, &child)) {
            long ext[4] = { 0, 0, 0, 0 };
            if (get_cardinals(dpy, w, "_NET_FRAME_EXTENTS", ext) && (ext[0] || ext[1] || ext[2] || ext[3])) {
                /* visible decorations only (frames of Metacity/Mutter also contain invisible resize borders) */
                r->x = cx - (int)ext[0];
                r->y = cy - (int)ext[2];
                r->width = ca.width + (int)(ext[0] + ext[1]);
                r->height = ca.height + (int)(ext[2] + ext[3]);
            } else if (frame != w && XGetWindowAttributes(dpy, frame, &fa)) {
                r->x = fa.x;
                r->y = fa.y;
                r->width = fa.width + 2 * fa.border_width;
                r->height = fa.height + 2 * fa.border_width;
            } else {
                r->x = cx;
                r->y = cy;
                r->width = ca.width;
                r->height = ca.height;
                /* client-side decorations: leave out the invisible shadow around the window */
                if (get_cardinals(dpy, w, "_GTK_FRAME_EXTENTS", ext)) {
                    r->x += (int)ext[0];
                    r->y += (int)ext[2];
                    r->width -= (int)(ext[0] + ext[1]);
                    r->height -= (int)(ext[2] + ext[3]);
                }
            }
            ok = r->width > 0 && r->height > 0;
        }
    }
    gdk_x11_display_error_trap_pop_ignored(gd);
    return ok;
}

/* ---------------------------------------------------------------- clipboard */
static void clip_get(GtkClipboard *cb, GtkSelectionData *sel, guint info, gpointer data)
{
    (void)cb; (void)info;
    gtk_selection_data_set_pixbuf(sel, GDK_PIXBUF(data));
}

static void clip_clear(GtkClipboard *cb, gpointer data)
{
    (void)cb; (void)data;
    clip_active = FALSE;                              /* another program copied something */
    quit_if_idle();
}

static void copy_to_clipboard(GdkPixbuf *p)
{
    GtkTargetList *tl = gtk_target_list_new(NULL, 0);
    gtk_target_list_add_image_targets(tl, 0, TRUE);
    int n = 0;
    GtkTargetEntry *t = gtk_target_table_new_from_list(tl, &n);
    GtkClipboard *cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    if (n > 0 && gtk_clipboard_set_with_data(cb, t, n, clip_get, clip_clear, p)) {
        clip_active = TRUE;
        gtk_clipboard_set_can_store(cb, NULL, 0);     /* lets a clipboard manager keep it after we exit */
    }
    gtk_target_table_free(t, n);
    gtk_target_list_unref(tl);
}

/* ---------------------------------------------------------------- notification */
static void open_saved(gboolean folder)
{
    if (!saved_path) return;
    char *uri = g_filename_to_uri(saved_path, NULL, NULL);
    gboolean done = FALSE;
    if (folder && bus && uri) {
        /* file managers that implement org.freedesktop.FileManager1 also select the file */
        const char *uris[] = { uri, NULL };
        GVariant *r = g_dbus_connection_call_sync(bus, "org.freedesktop.FileManager1", "/org/freedesktop/FileManager1",
            "org.freedesktop.FileManager1", "ShowItems", g_variant_new("(^ass)", uris, ""), NULL,
            G_DBUS_CALL_FLAGS_NONE, 3000, NULL, NULL);
        if (r) { done = TRUE; g_variant_unref(r); }
    }
    if (!done) {
        char *target = folder ? g_path_get_dirname(saved_path) : g_strdup(saved_path);
        char *turi = g_filename_to_uri(target, NULL, NULL);
        if (!turi || !g_app_info_launch_default_for_uri(turi, NULL, NULL)) {
            char *argv[] = { "xdg-open", target, NULL };
            g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
        }
        g_free(turi);
        g_free(target);
    }
    g_free(uri);
}

static void on_notification_signal(GDBusConnection *c, const char *sender, const char *path, const char *iface,
                                   const char *signal, GVariant *params, gpointer data)
{
    (void)c; (void)sender; (void)path; (void)iface; (void)data;
    guint32 id = 0;
    if (!strcmp(signal, "ActionInvoked") && g_variant_is_of_type(params, G_VARIANT_TYPE("(us)"))) {
        const char *key = NULL;
        g_variant_get(params, "(u&s)", &id, &key);
        if (id == notif_id && key) open_saved(!strcmp(key, "folder"));
    } else if (!strcmp(signal, "NotificationClosed") && g_variant_is_of_type(params, G_VARIANT_TYPE("(uu)"))) {
        guint32 reason = 0;
        g_variant_get(params, "(uu)", &id, &reason);
        if (id == notif_id) {
            notif_active = FALSE;
            quit_if_idle();
        }
    }
}

static void notify_user(const char *summary, const char *body, const char *image_path, gboolean with_actions)
{
    if (!bus) bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    if (!bus) return;
    if (with_actions)
        g_dbus_connection_signal_subscribe(bus, NULL, "org.freedesktop.Notifications", NULL,
                                           "/org/freedesktop/Notifications", NULL, G_DBUS_SIGNAL_FLAGS_NONE,
                                           on_notification_signal, NULL, NULL);
    GVariantBuilder actions, hints;
    g_variant_builder_init(&actions, G_VARIANT_TYPE("as"));
    if (with_actions) {
        const char *a[] = { "default", "Open", "open", "Open", "folder", "Show in Folder" };
        for (guint i = 0; i < G_N_ELEMENTS(a); i++) g_variant_builder_add(&actions, "s", a[i]);
    }
    g_variant_builder_init(&hints, G_VARIANT_TYPE("a{sv}"));
    if (image_path) g_variant_builder_add(&hints, "{sv}", "image-path", g_variant_new_string(image_path));
    g_variant_builder_add(&hints, "{sv}", "sound-name", g_variant_new_string(image_path ? "screen-capture" : "dialog-warning"));
    g_variant_builder_add(&hints, "{sv}", "desktop-entry", g_variant_new_string("hde-screenshot"));
    GVariant *r = g_dbus_connection_call_sync(bus, "org.freedesktop.Notifications", "/org/freedesktop/Notifications",
        "org.freedesktop.Notifications", "Notify",
        g_variant_new("(susssasa{sv}i)", "Screenshot", (guint32)0, image_path ? "camera-photo" : "dialog-warning",
                      summary, body, &actions, &hints, -1),
        G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 3000, NULL, NULL);
    if (r) {
        g_variant_get(r, "(u)", &notif_id);
        g_variant_unref(r);
        if (with_actions) notif_active = TRUE;        /* stay alive for "Open" / "Show in Folder" */
        if (g_getenv("HDE_DEBUG")) g_printerr("hde-screenshot: notification %u: %s\n", notif_id, summary);
    } else if (g_getenv("HDE_DEBUG")) {
        g_printerr("hde-screenshot: no notification daemon answered\n");
    }
}

/* ---------------------------------------------------------------- saving */
static char *default_path(void)
{
    const char *pics = g_get_user_special_dir(G_USER_DIRECTORY_PICTURES);
    char *base = pics && strcmp(pics, g_get_home_dir()) ? g_strdup(pics) : g_build_filename(g_get_home_dir(), "Pictures", NULL);
    char *dir = g_build_filename(base, "Screenshots", NULL);
    g_free(base);
    if (g_mkdir_with_parents(dir, 0755) != 0) {
        g_free(dir);
        dir = g_strdup(g_get_home_dir());
    }
    GDateTime *now = g_date_time_new_now_local();
    char *stamp = g_date_time_format(now, "%Y-%m-%d_%H-%M-%S");
    g_date_time_unref(now);
    char *path = NULL;
    for (int i = 1; i < 1000 && !path; i++) {
        char *name = i == 1 ? g_strdup_printf("Screenshot_%s.png", stamp) : g_strdup_printf("Screenshot_%s-%d.png", stamp, i);
        path = g_build_filename(dir, name, NULL);
        g_free(name);
        if (g_file_test(path, G_FILE_TEST_EXISTS)) { g_free(path); path = NULL; }
    }
    g_free(stamp);
    g_free(dir);
    return path;
}

static void finish(GdkPixbuf *p)
{
    if (!p) { fail("Could not read the screen contents."); return; }
    result = p;
    char *path = opt_file ? g_strdup(opt_file) : default_path();
    GError *err = NULL;
    if (!path || !gdk_pixbuf_save(p, path, "png", &err, NULL)) {
        char *msg = g_strdup_printf("Could not save %s: %s", path ? path : "the picture", err ? err->message : "no file name");
        g_clear_error(&err);
        g_free(path);
        fail(msg);
        g_free(msg);
        return;
    }
    saved_path = path;
    exit_code = 0;
    printf("%s\n", path);
    fflush(stdout);
    if (!opt_no_clip) copy_to_clipboard(p);
    if (!opt_no_notify) {
        char *shown = home_relative(path);
        char *body = g_markup_printf_escaped("Saved as <b>%s</b>%s", shown,
                                             clip_active ? "\nand copied to the clipboard." : ".");
        notify_user("Screenshot taken", body, path, TRUE);
        g_free(body);
        g_free(shown);
    }
    if (clip_active || notif_active) g_timeout_add_seconds(600, on_give_up, NULL);
    quit_if_idle();
}

/* ---------------------------------------------------------------- area selection */
static struct {
    GtkWidget *win;
    GdkPixbuf *frozen;
    int sw, sh, tries;
    gboolean dragging, have;
    double x0, y0, x1, y1;
} area;

static void area_rect(GdkRectangle *r)
{
    double l = MIN(area.x0, area.x1), t = MIN(area.y0, area.y1);
    double rr = MAX(area.x0, area.x1), b = MAX(area.y0, area.y1);
    r->x = (int)floor(l);
    r->y = (int)floor(t);
    r->width = (int)ceil(rr) - r->x;
    r->height = (int)ceil(b) - r->y;
}

static void draw_badge(cairo_t *cr, const char *text, double x, double y, double max_x, double max_y)
{
    PangoLayout *l = pango_cairo_create_layout(cr);
    PangoFontDescription *fd = pango_font_description_from_string("Sans Bold 11");
    pango_layout_set_font_description(l, fd);
    pango_font_description_free(fd);
    pango_layout_set_text(l, text, -1);
    int tw, th;
    pango_layout_get_pixel_size(l, &tw, &th);
    double w = tw + 20, h = th + 10;
    x = CLAMP(x, 4, MAX(4, max_x - w - 4));
    y = CLAMP(y, 4, MAX(4, max_y - h - 4));
    double r = 6;
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -G_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, G_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, G_PI / 2, G_PI);
    cairo_arc(cr, x + r, y + r, r, G_PI, 3 * G_PI / 2);
    cairo_close_path(cr);
    cairo_set_source_rgba(cr, 0.07, 0.08, 0.10, 0.82);
    cairo_fill(cr);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_move_to(cr, x + 10, y + 5);
    pango_cairo_show_layout(cr, l);
    g_object_unref(l);
}

static gboolean area_draw(GtkWidget *w, cairo_t *cr, gpointer data)
{
    (void)w; (void)data;
    gdk_cairo_set_source_pixbuf(cr, area.frozen, 0, 0);
    cairo_paint(cr);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.45);
    if (!area.have) {
        cairo_paint(cr);
        GdkRectangle m = { 0, 0, area.sw, area.sh };
        GdkDisplay *d = gdk_display_get_default();
        GdkMonitor *mon = gdk_display_get_primary_monitor(d);
        if (!mon) mon = gdk_display_get_monitor(d, 0);
        if (mon) gdk_monitor_get_geometry(mon, &m);
        draw_badge(cr, "Drag to select an area  ·  Esc or right-click to cancel",
                   m.x + m.width / 2.0 - 230, m.y + m.height / 2.0 - 16, area.sw, area.sh);
        return TRUE;
    }
    GdkRectangle r;
    area_rect(&r);
    cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
    cairo_rectangle(cr, 0, 0, area.sw, area.sh);
    cairo_rectangle(cr, r.x, r.y, r.width, r.height);
    cairo_fill(cr);
    cairo_set_fill_rule(cr, CAIRO_FILL_RULE_WINDING);
    /* frame drawn outside the selection, so it never ends up in the picture */
    cairo_set_line_width(cr, 1);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.7);
    cairo_rectangle(cr, r.x - 2.5, r.y - 2.5, r.width + 5, r.height + 5);
    cairo_stroke(cr);
    cairo_set_line_width(cr, 2);
    cairo_set_source_rgb(cr, 0.33, 0.62, 1.0);
    cairo_rectangle(cr, r.x - 1, r.y - 1, r.width + 2, r.height + 2);
    cairo_stroke(cr);
    char txt[64];
    snprintf(txt, sizeof txt, "%d × %d", r.width, r.height);
    double by = r.y + r.height + 8;
    if (by + 30 > area.sh) by = r.y - 38;
    draw_badge(cr, txt, r.x, by, area.sw, area.sh);
    return TRUE;
}

static void area_close(void)
{
    gdk_seat_ungrab(gdk_display_get_default_seat(gdk_display_get_default()));
    if (area.win) gtk_widget_destroy(area.win);
    area.win = NULL;
    gdk_display_flush(gdk_display_get_default());
}

static void area_cancel(void)
{
    area_close();
    exit_code = 1;
    quit_if_idle();
}

static gboolean area_press(GtkWidget *w, GdkEventButton *e, gpointer data)
{
    (void)data;
    if (e->button == 3) { area_cancel(); return TRUE; }
    if (e->button != 1 || e->type != GDK_BUTTON_PRESS) return TRUE;
    area.dragging = TRUE;
    area.have = FALSE;
    area.x0 = area.x1 = e->x_root;
    area.y0 = area.y1 = e->y_root;
    gtk_widget_queue_draw(w);
    return TRUE;
}

static gboolean area_motion(GtkWidget *w, GdkEventMotion *e, gpointer data)
{
    (void)data;
    if (!area.dragging) return TRUE;
    area.x1 = e->x_root;
    area.y1 = e->y_root;
    area.have = fabs(area.x1 - area.x0) >= 1 || fabs(area.y1 - area.y0) >= 1;
    gtk_widget_queue_draw(w);
    return TRUE;
}

static gboolean area_release(GtkWidget *w, GdkEventButton *e, gpointer data)
{
    (void)data;
    if (e->button != 1 || !area.dragging) return TRUE;
    area.dragging = FALSE;
    area.x1 = e->x_root;
    area.y1 = e->y_root;
    GdkRectangle r;
    area_rect(&r);
    r.width = MIN(r.width, area.sw - r.x);
    r.height = MIN(r.height, area.sh - r.y);
    if (r.width < 3 || r.height < 3 || r.x < 0 || r.y < 0) {   /* just a click: keep waiting for a drag */
        area.have = FALSE;
        gtk_widget_queue_draw(w);
        return TRUE;
    }
    GdkPixbuf *sub = gdk_pixbuf_new_subpixbuf(area.frozen, r.x, r.y, r.width, r.height);
    GdkPixbuf *copy = gdk_pixbuf_copy(sub);
    g_object_unref(sub);
    area_close();
    finish(copy);
    return TRUE;
}

static gboolean area_key(GtkWidget *w, GdkEventKey *e, gpointer data)
{
    (void)w; (void)data;
    if (e->keyval == GDK_KEY_Escape) area_cancel();
    return TRUE;
}

/* While the Print key is still held, hde-hotkeys owns the keyboard (its passive grab): retry until free. */
static gboolean area_try_grab(gpointer data)
{
    (void)data;
    if (!area.win || !gtk_widget_get_window(area.win)) return G_SOURCE_REMOVE;
    GdkDisplay *d = gdk_display_get_default();
    GdkCursor *cur = gdk_cursor_new_from_name(d, "crosshair");
    GdkGrabStatus st = gdk_seat_grab(gdk_display_get_default_seat(d), gtk_widget_get_window(area.win),
                                     GDK_SEAT_CAPABILITY_ALL, FALSE, cur, NULL, NULL, NULL);
    if (cur) g_object_unref(cur);
    if (st == GDK_GRAB_SUCCESS) return G_SOURCE_REMOVE;
    if (++area.tries >= 80) {
        g_printerr("hde-screenshot: could not grab the mouse/keyboard (status %d); selecting without a grab\n", st);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static gboolean area_mapped(GtkWidget *w, GdkEvent *e, gpointer data)
{
    (void)w; (void)e; (void)data;
    if (area_try_grab(NULL) == G_SOURCE_CONTINUE) g_timeout_add(50, area_try_grab, NULL);
    return FALSE;
}

static void area_begin(void)
{
    GdkDisplay *gd = gdk_display_get_default();
    Display *dpy = GDK_DISPLAY_XDISPLAY(gd);
    /* one area selection at a time (e.g. Shift+Print pressed twice) */
    Atom lock = XInternAtom(dpy, "_HDE_SCREENSHOT_AREA", False);
    if (XGetSelectionOwner(dpy, lock) != None) {
        g_printerr("hde-screenshot: an area selection is already running\n");
        exit_code = 1;
        quit_if_idle();
        return;
    }
    area.frozen = grab_root(0, 0, -1, -1);             /* freeze first: menus and tooltips stay in the picture */
    if (!area.frozen) { fail("Could not read the screen contents."); return; }
    area.sw = gdk_pixbuf_get_width(area.frozen);
    area.sh = gdk_pixbuf_get_height(area.frozen);

    area.win = gtk_window_new(GTK_WINDOW_POPUP);       /* override-redirect: above everything, not managed */
    gtk_window_set_title(GTK_WINDOW(area.win), "hde-screenshot area");
    gtk_widget_set_app_paintable(area.win, TRUE);
    gtk_window_move(GTK_WINDOW(area.win), 0, 0);
    gtk_widget_set_size_request(area.win, area.sw, area.sh);
    gtk_window_set_default_size(GTK_WINDOW(area.win), area.sw, area.sh);
    gtk_widget_add_events(area.win, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK |
                                    GDK_KEY_PRESS_MASK | GDK_STRUCTURE_MASK);
    g_signal_connect(area.win, "draw", G_CALLBACK(area_draw), NULL);
    g_signal_connect(area.win, "button-press-event", G_CALLBACK(area_press), NULL);
    g_signal_connect(area.win, "motion-notify-event", G_CALLBACK(area_motion), NULL);
    g_signal_connect(area.win, "button-release-event", G_CALLBACK(area_release), NULL);
    g_signal_connect(area.win, "key-press-event", G_CALLBACK(area_key), NULL);
    g_signal_connect(area.win, "map-event", G_CALLBACK(area_mapped), NULL);
    gtk_widget_realize(area.win);
    GdkCursor *cur = gdk_cursor_new_from_name(gd, "crosshair");
    if (cur) {
        gdk_window_set_cursor(gtk_widget_get_window(area.win), cur);
        g_object_unref(cur);
    }
    XSetSelectionOwner(dpy, lock, GDK_WINDOW_XID(gtk_widget_get_window(area.win)), CurrentTime);
    gtk_widget_show(area.win);
}

/* ---------------------------------------------------------------- main */
static gboolean start_capture(gpointer data)
{
    (void)data;
    GdkRectangle r;
    switch (mode) {
    case MODE_AREA:
        area_begin();
        break;
    case MODE_WINDOW:
        /* no usable active window (e.g. the desktop has the focus): take the whole screen */
        finish(active_window_rect(&r) ? grab_root(r.x, r.y, r.width, r.height) : grab_root(0, 0, -1, -1));
        break;
    default:
        finish(grab_root(0, 0, -1, -1));
        break;
    }
    return G_SOURCE_REMOVE;
}

static void usage(FILE *f)
{
    fprintf(f,
            "Usage: hde-screenshot [--area | --window] [--delay N] [--file PATH] [--no-clipboard] [--no-notify]\n"
            "  (no option)    the whole screen\n"
            "  -a, --area     drag a rectangle (Esc or right click cancels)\n"
            "  -w, --window   the active window\n"
            "  -d, --delay N  wait N seconds first\n"
            "  -f, --file P   save to P (default: ~/Pictures/Screenshots/Screenshot_<date>_<time>.png)\n"
            "  --no-clipboard, --no-notify\n");
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-a") || !strcmp(a, "--area")) mode = MODE_AREA;
        else if (!strcmp(a, "-w") || !strcmp(a, "--window")) mode = MODE_WINDOW;
        else if (!strcmp(a, "--full") || !strcmp(a, "--screen")) mode = MODE_FULL;
        else if ((!strcmp(a, "-d") || !strcmp(a, "--delay")) && i + 1 < argc) opt_delay = CLAMP(atoi(argv[++i]), 0, 3600);
        else if (!strncmp(a, "--delay=", 8)) opt_delay = CLAMP(atoi(a + 8), 0, 3600);
        else if ((!strcmp(a, "-f") || !strcmp(a, "--file")) && i + 1 < argc) opt_file = argv[++i];
        else if (!strncmp(a, "--file=", 7)) opt_file = (char *)a + 7;
        else if (!strcmp(a, "--no-clipboard")) opt_no_clip = TRUE;
        else if (!strcmp(a, "--no-notify")) opt_no_notify = TRUE;
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(stdout); return 0; }
        else { fprintf(stderr, "hde-screenshot: unknown option %s\n", a); usage(stderr); return 2; }
    }
    gdk_set_allowed_backends("x11");
    if (!gtk_init_check(&argc, &argv)) {
        fprintf(stderr, "hde-screenshot: cannot open the X display (DISPLAY=%s)\n", g_getenv("DISPLAY") ? g_getenv("DISPLAY") : "unset");
        return 2;
    }
    g_set_application_name("Screenshot");
    if (opt_delay > 0) g_timeout_add_seconds(opt_delay, start_capture, NULL);
    else g_idle_add(start_capture, NULL);
    gtk_main();
    exiting = TRUE;
    if (clip_active) gtk_clipboard_store(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD));
    g_clear_object(&result);
    g_clear_object(&area.frozen);
    g_free(saved_path);
    return exit_code;
}
