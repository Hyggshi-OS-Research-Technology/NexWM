/* hde-screenshot — HDE's built-in screenshot tool (GTK3 + Xlib), so PrtSc works without scrot or any
 * other external program.
 *
 *   hde-screenshot              the whole screen (all monitors)
 *   hde-screenshot --area       drag a rectangle on a frozen copy of the screen (Esc / right click = cancel)
 *   hde-screenshot --window     the active window, including its title bar
 *   hde-screenshot --ui         the Screenshot window (Start menu > Screenshot): choose whole screen / window / area
 *                               and a delay, then see the picture with Copy, Save As, Open and Show in Folder
 *   options:  --delay N         wait N seconds first
 *             --file PATH       save to PATH instead of ~/Pictures/Screenshots/Screenshot_<date>_<time>.png
 *             --clipboard       only copy the image to the clipboard, do not save a file
 *             --no-clipboard    do not copy the image to the clipboard
 *             --no-notify       do not show a notification
 *             --pointer, --no-pointer   with / without the mouse pointer in the picture (default: settings.ini
 *                               screenshot_pointer, off; the Screenshot window has a check box for it)
 *
 * hde-hotkeys runs it for Print / Shift+Print / Alt+Print (Settings > Keyboard can pick another tool) and, with
 * --clipboard, for Ctrl+Print / Ctrl+Shift+Print / Ctrl+Alt+Print.
 * The PNG is saved, copied to the clipboard and announced with a notification ("Open" / "Show in Folder").
 * X11 clipboards live in the program that owns them, so the process stays in the background while it owns
 * the clipboard (until another program copies something, at most 10 minutes; a clipboard manager may keep
 * the image afterwards). The saved path is printed on stdout. Exit status: 0 saved (or copied), 1 cancelled, 2 error.
 *
 * In the "HDE (Wayland)" session programs cannot read the screen through GTK: grim takes the picture (wlr-screencopy,
 * labwc has it) and slurp lets you drag the area (for --window too: Wayland does not tell where the active window is).
 */
#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#ifdef HAVE_XFIXES
#include <X11/extensions/Xfixes.h>
#endif
#include <glib/gstdio.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum { MODE_FULL, MODE_AREA, MODE_WINDOW } Mode;
static gboolean wayland_mode;           /* the Wayland session: grim (+ slurp) */

static Mode mode = MODE_FULL;
static int opt_delay;
static char *opt_file;
static gboolean opt_no_clip, opt_no_notify, opt_clip_only, opt_ui;
static int opt_pointer = -1;            /* -1: settings.ini screenshot_pointer */

static int exit_code = 2;
static GdkPixbuf *result;               /* the final image (also served on the clipboard) */
static char *saved_path;
static gboolean clip_active, notif_active, exiting;
static GDBusConnection *bus;
static guint32 notif_id;

static gboolean ui_visible(void);
static void finish(GdkPixbuf *p);
static void ui_show_result(GdkPixbuf *p, const char *path);
static void ui_show_error(const char *what);
static void ui_show_start(void);

/* ---------------------------------------------------------------- helpers */
static void quit_if_idle(void)
{
    if (!exiting && !clip_active && !notif_active && !ui_visible()) {
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

static void notify_user(const char *summary, const char *body, const char *image_path, GdkPixbuf *image,
                        gboolean with_actions);

static void fail(const char *what)
{
    g_printerr("hde-screenshot: %s\n", what);
    if (opt_ui) { ui_show_error(what); return; }
    if (!opt_no_notify) notify_user("Screenshot failed", what, NULL, NULL, FALSE);
    exit_code = 2;
    quit_if_idle();
}

/* ---------------------------------------------------------------- settings.ini */
static char *settings_path(void) { return g_build_filename(g_get_user_config_dir(), "hde", "settings.ini", NULL); }

static gboolean setting_bool(const char *key, gboolean def)
{
    char *path = settings_path();
    GKeyFile *kf = g_key_file_new();
    gboolean v = def;
    if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL) && g_key_file_has_key(kf, "settings", key, NULL))
        v = g_key_file_get_boolean(kf, "settings", key, NULL);
    g_key_file_free(kf);
    g_free(path);
    return v;
}

static void setting_set_bool(const char *key, gboolean v)
{
    char *path = settings_path(), *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0700);
    GKeyFile *kf = g_key_file_new();
    g_key_file_load_from_file(kf, path, G_KEY_FILE_KEEP_COMMENTS, NULL);
    if (!g_key_file_has_key(kf, "settings", key, NULL) || g_key_file_get_boolean(kf, "settings", key, NULL) != v) {
        g_key_file_set_boolean(kf, "settings", key, v);
        g_key_file_save_to_file(kf, path, NULL);
    }
    g_key_file_free(kf);
    g_free(dir);
    g_free(path);
}

static gboolean want_pointer(void) { return opt_pointer >= 0 ? opt_pointer : setting_bool("screenshot_pointer", FALSE); }

/* ---------------------------------------------------------------- capture */
/* The mouse pointer onto a picture of the screen whose top left corner is at ox, oy (XFixes: the X server draws the
 * pointer on top, it is never in what programs read from the screen). */
static void add_pointer(GdkPixbuf *pb, int ox, int oy)
{
#ifdef HAVE_XFIXES
    GdkDisplay *gd = gdk_display_get_default();
    if (!pb || !GDK_IS_X11_DISPLAY(gd)) return;
    Display *dpy = GDK_DISPLAY_XDISPLAY(gd);
    int evb, erb;
    if (!XFixesQueryExtension(dpy, &evb, &erb)) return;
    XFixesCursorImage *ci = XFixesGetCursorImage(dpy);
    if (!ci) return;
    int x0 = ci->x - ci->xhot - ox, y0 = ci->y - ci->yhot - oy;
    int w = gdk_pixbuf_get_width(pb), h = gdk_pixbuf_get_height(pb), nc = gdk_pixbuf_get_n_channels(pb);
    int rs = gdk_pixbuf_get_rowstride(pb);
    guchar *px = gdk_pixbuf_get_pixels(pb);
    int drawn = 0;
    for (int j = 0; j < ci->height; j++)
        for (int i = 0; i < ci->width; i++) {
            int x = x0 + i, y = y0 + j;
            if (x < 0 || y < 0 || x >= w || y >= h) continue;
            unsigned long v = ci->pixels[j * ci->width + i];  /* premultiplied ARGB in the low 32 bits of a long */
            unsigned a = (v >> 24) & 0xff;
            if (!a) continue;
            guchar *d = px + y * rs + x * nc;
            unsigned src[3] = { (v >> 16) & 0xff, (v >> 8) & 0xff, v & 0xff };
            for (int c = 0; c < 3; c++) d[c] = (guchar)MIN(255u, src[c] + d[c] * (255 - a) / 255);
            if (nc == 4) d[3] = (guchar)MIN(255u, a + d[3] * (255 - a) / 255);
            drawn++;
        }
    g_printerr("hde-screenshot: pointer at %d,%d (%dx%d) %s\n", ci->x, ci->y, ci->width, ci->height,
               drawn ? "drawn into the picture" : "outside the picture");
    XFree(ci);
#else
    (void)pb; (void)ox; (void)oy;
    g_printerr("hde-screenshot: built without libxfixes-dev: no mouse pointer in the picture\n");
#endif
}

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

/* the whole screen or a window, with the pointer when wanted */
static GdkPixbuf *grab_shot(int x, int y, int w, int h)
{
    GdkPixbuf *pb = grab_root(x, y, w, h);
    if (pb && want_pointer()) add_pointer(pb, w < 0 || h < 0 ? 0 : MAX(0, x), w < 0 || h < 0 ? 0 : MAX(0, y));
    return pb;
}

/* ---------------------------------------------------------------- Wayland: grim, slurp */
static GdkPixbuf *grim_capture(const char *geometry, GError **err)
{
    char *grim = g_find_program_in_path("grim");
    if (!grim) {
        g_set_error_literal(err, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                            "Screenshots on Wayland need grim: sudo apt install grim");
        return NULL;
    }
    /* -c: with the pointer */
    const char *argv[8];
    int k = 0;
    argv[k++] = grim;
    if (want_pointer()) argv[k++] = "-c";
    argv[k++] = "-t";
    argv[k++] = "png";
    if (geometry) {
        argv[k++] = "-g";
        argv[k++] = geometry;
    }
    argv[k++] = "-";
    argv[k] = NULL;
    char *out = NULL;
    gsize len = 0;
    int status = 0;
    GdkPixbuf *pb = NULL;
    GError *e = NULL;
    GSubprocess *sp = g_subprocess_newv(argv, G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE, &e);
    GBytes *ob = NULL, *eb = NULL;
    if (sp && g_subprocess_communicate(sp, NULL, NULL, &ob, &eb, &e)) {
        status = g_subprocess_get_exit_status(sp);
        out = ob ? g_bytes_unref_to_data(ob, &len) : NULL;
        ob = NULL;
        if (status == 0 && out && len) {
            GdkPixbufLoader *ld = gdk_pixbuf_loader_new_with_type("png", NULL);
            if (gdk_pixbuf_loader_write(ld, (const guchar *)out, len, &e) && gdk_pixbuf_loader_close(ld, &e)) {
                pb = gdk_pixbuf_loader_get_pixbuf(ld);
                if (pb) g_object_ref(pb);
            } else gdk_pixbuf_loader_close(ld, NULL);
            g_object_unref(ld);
        } else if (!e) {
            gsize el = 0;
            char *msg = eb ? g_bytes_unref_to_data(eb, &el) : NULL;
            eb = NULL;
            g_set_error(&e, G_IO_ERROR, G_IO_ERROR_FAILED, "grim failed: %s", msg && *msg ? g_strstrip(msg) : "?");
            g_free(msg);
        }
    }
    if (eb) g_bytes_unref(eb);
    if (ob) g_bytes_unref(ob);
    g_free(out);
    g_clear_object(&sp);
    g_free(grim);
    if (e) g_propagate_error(err, e);
    return pb;
}

/* slurp: drag a rectangle; returns "x,y wxh" or NULL when cancelled (Esc) */
static char *slurp_geometry(GError **err)
{
    char *slurp = g_find_program_in_path("slurp");
    if (!slurp) {
        g_set_error_literal(err, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                            "Choosing an area on Wayland needs slurp: sudo apt install slurp");
        return NULL;
    }
    char *out = NULL;
    int status = 1;
    const char *argv[] = { slurp, "-d", NULL };
    g_spawn_sync(NULL, (char **)argv, NULL, G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, &out, NULL, &status, NULL);
    g_free(slurp);
    if (status != 0 || !out || !*g_strstrip(out)) { g_free(out); return NULL; }
    return out;
}

static void wayland_capture(void)
{
    GError *e = NULL;
    char *geo = NULL;
    if (mode != MODE_FULL) {
        geo = slurp_geometry(&e);
        if (!geo && !e) {                       /* cancelled */
            g_printerr("hde-screenshot: area: cancelled\n");
            exit_code = 1;
            if (ui_visible()) ui_show_start();
            quit_if_idle();
            return;
        }
    }
    GdkPixbuf *pb = e ? NULL : grim_capture(geo, &e);
    g_free(geo);
    if (!pb) {
        fail(e ? e->message : "Could not read the screen contents.");
        g_clear_error(&e);
        return;
    }
    finish(pb);
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
    (void)cb;
    g_object_unref(data);
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
    g_object_ref(p);
    if (n > 0 && gtk_clipboard_set_with_data(cb, t, n, clip_get, clip_clear, p)) {
        clip_active = TRUE;
        gtk_clipboard_set_can_store(cb, NULL, 0);     /* lets a clipboard manager keep it after we exit */
    } else {
        g_object_unref(p);
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

/* A small copy of the picture as the notification's image-data hint (for a picture that is not in a file). */
static GVariant *image_data_hint(GdkPixbuf *src)
{
    int w = gdk_pixbuf_get_width(src), h = gdk_pixbuf_get_height(src);
    double s = MIN(1.0, MIN(320.0 / w, 200.0 / h));
    GdkPixbuf *t = gdk_pixbuf_scale_simple(src, MAX(1, (int)(w * s)), MAX(1, (int)(h * s)), GDK_INTERP_BILINEAR);
    if (!t) return NULL;
    GVariant *v = g_variant_new("(iiibii@ay)", gdk_pixbuf_get_width(t), gdk_pixbuf_get_height(t),
                                gdk_pixbuf_get_rowstride(t), gdk_pixbuf_get_has_alpha(t),
                                gdk_pixbuf_get_bits_per_sample(t), gdk_pixbuf_get_n_channels(t),
                                g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, gdk_pixbuf_read_pixels(t),
                                                          gdk_pixbuf_get_byte_length(t), 1));
    g_object_unref(t);
    return v;
}

static void notify_user(const char *summary, const char *body, const char *image_path, GdkPixbuf *image,
                        gboolean with_actions)
{
    gboolean ok = image_path || image;
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
    GVariant *img = image ? image_data_hint(image) : NULL;
    if (img) g_variant_builder_add(&hints, "{sv}", "image-data", img);
    g_variant_builder_add(&hints, "{sv}", "sound-name", g_variant_new_string(ok ? "screen-capture" : "dialog-warning"));
    g_variant_builder_add(&hints, "{sv}", "desktop-entry", g_variant_new_string("hde-screenshot"));
    GVariant *r = g_dbus_connection_call_sync(bus, "org.freedesktop.Notifications", "/org/freedesktop/Notifications",
        "org.freedesktop.Notifications", "Notify",
        g_variant_new("(susssasa{sv}i)", "Screenshot", (guint32)0, ok ? "camera-photo" : "dialog-warning",
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
    g_clear_object(&result);
    g_clear_pointer(&saved_path, g_free);
    result = p;
    if (opt_clip_only) {                              /* Ctrl+Print & co.: clipboard only, no file */
        copy_to_clipboard(p);
        if (!clip_active) { fail("Could not copy the picture to the clipboard."); return; }
        exit_code = 0;
        if (!opt_no_notify)
            notify_user("Screenshot copied", "Copied to the clipboard (not saved to a file). Paste it with Ctrl+V.",
                        NULL, p, FALSE);
        g_timeout_add_seconds(600, on_give_up, NULL);
        quit_if_idle();
        return;
    }
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
    if (opt_ui) { ui_show_result(p, path); return; }
    if (!opt_no_notify) {
        char *dir = g_path_get_dirname(path);
        char *shown = home_relative(dir);
        char *body = g_markup_printf_escaped("Saved in %s%s", shown, clip_active ? " and copied to the clipboard." : ".");
        notify_user("Screenshot taken", body, path, NULL, TRUE);
        g_free(body);
        g_free(shown);
        g_free(dir);
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
    if (opt_ui) { ui_show_start(); return; }
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
    if (wayland_mode) { wayland_capture(); return G_SOURCE_REMOVE; }
    switch (mode) {
    case MODE_AREA:
        area_begin();
        break;
    case MODE_WINDOW:
        /* no usable active window (e.g. the desktop has the focus): take the whole screen */
        finish(active_window_rect(&r) ? grab_shot(r.x, r.y, r.width, r.height) : grab_shot(0, 0, -1, -1));
        break;
    default:
        finish(grab_shot(0, 0, -1, -1));
        break;
    }
    return G_SOURCE_REMOVE;
}

/* ---------------------------------------------------------------- the Screenshot window (--ui) */
static struct {
    GtkWidget *win, *stack, *modes[3], *delay, *pointer, *take, *preview, *saved, *error;
    gboolean closed;                    /* the user closed the window (it is also hidden while taking a picture) */
} ui;

static gboolean ui_visible(void) { return opt_ui && ui.win && !ui.closed; }

static void ui_present(void)
{
    ui.closed = FALSE;
    gtk_widget_show(ui.win);
    gtk_window_present(GTK_WINDOW(ui.win));
}

static void ui_show_start(void)
{
    gtk_stack_set_visible_child_name(GTK_STACK(ui.stack), "start");
    gtk_widget_hide(ui.error);
    ui_present();
    gtk_widget_grab_focus(ui.take);
}

static void ui_show_error(const char *what)
{
    gtk_stack_set_visible_child_name(GTK_STACK(ui.stack), "start");
    gtk_label_set_text(GTK_LABEL(ui.error), what);
    gtk_widget_show(ui.error);
    ui_present();
}

static void ui_show_result(GdkPixbuf *p, const char *path)
{
    int w = gdk_pixbuf_get_width(p), h = gdk_pixbuf_get_height(p);
    double sc = MIN(1.0, MIN(560.0 / w, 320.0 / h));
    GdkPixbuf *t = gdk_pixbuf_scale_simple(p, MAX(1, (int)(w * sc)), MAX(1, (int)(h * sc)), GDK_INTERP_BILINEAR);
    gtk_image_set_from_pixbuf(GTK_IMAGE(ui.preview), t);
    if (t) g_object_unref(t);
    char *shown = home_relative(path);
    char *msg = g_strdup_printf("%d × %d — saved as %s%s", w, h, shown, clip_active ? " and copied to the clipboard" : "");
    gtk_label_set_text(GTK_LABEL(ui.saved), msg);
    g_free(msg);
    g_free(shown);
    gtk_stack_set_visible_child_name(GTK_STACK(ui.stack), "result");
    ui_present();
    g_printerr("hde-screenshot: window: saved %s (%dx%d)\n", path, w, h);
}

static gboolean ui_capture(gpointer data)
{
    (void)data;
    start_capture(NULL);
    return G_SOURCE_REMOVE;
}

static void on_ui_take(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    mode = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ui.modes[1])) ? MODE_WINDOW
         : gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ui.modes[2])) ? MODE_AREA : MODE_FULL;
    int delay = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(ui.delay));
    gboolean ptr = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ui.pointer));
    opt_pointer = ptr;
    setting_set_bool("screenshot_pointer", ptr);           /* remembered, also for PrtSc */
    g_printerr("hde-screenshot: window: taking %s in %d s%s\n", mode == MODE_AREA ? "an area" : mode == MODE_WINDOW ?
               "the active window" : "the whole screen", delay, ptr && mode != MODE_AREA ? ", with the pointer" : "");
    gtk_widget_hide(ui.win);                  /* not in the picture */
    gdk_display_flush(gdk_display_get_default());
    g_timeout_add(MAX(delay * 1000, mode == MODE_WINDOW ? 600 : 400), ui_capture, NULL);
}

static void on_ui_new(GtkButton *b, gpointer d) { (void)b; (void)d; ui_show_start(); }
static void on_ui_copy(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    if (result) copy_to_clipboard(result);
    gtk_label_set_text(GTK_LABEL(ui.saved), clip_active ? "Copied to the clipboard. Paste it with Ctrl+V." : "Could not copy.");
}
static void on_ui_open(GtkButton *b, gpointer d) { (void)b; (void)d; open_saved(FALSE); }
static void on_ui_folder(GtkButton *b, gpointer d) { (void)b; (void)d; open_saved(TRUE); }

static void on_ui_save_as(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    if (!result) return;
    GtkWidget *dlg = gtk_file_chooser_dialog_new("Save Screenshot As", GTK_WINDOW(ui.win), GTK_FILE_CHOOSER_ACTION_SAVE,
                                                 "_Cancel", GTK_RESPONSE_CANCEL, "_Save", GTK_RESPONSE_ACCEPT, NULL);
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dlg), TRUE);
    if (saved_path) {
        char *dir = g_path_get_dirname(saved_path), *base = g_path_get_basename(saved_path);
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dlg), dir);
        gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dlg), base);
        g_free(dir);
        g_free(base);
    }
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char *f = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        const char *type = g_str_has_suffix(f, ".jpg") || g_str_has_suffix(f, ".jpeg") ? "jpeg" : "png";
        GError *e = NULL;
        if (gdk_pixbuf_save(result, f, type, &e, NULL)) {
            g_free(saved_path);
            saved_path = g_strdup(f);
            char *shown = home_relative(f), *m = g_strdup_printf("Saved as %s", shown);
            gtk_label_set_text(GTK_LABEL(ui.saved), m);
            g_free(m);
            g_free(shown);
        } else {
            gtk_label_set_text(GTK_LABEL(ui.saved), e ? e->message : "Could not save.");
            g_clear_error(&e);
        }
        g_free(f);
    }
    gtk_widget_destroy(dlg);
}

static gboolean on_ui_delete(GtkWidget *w, GdkEvent *e, gpointer d)
{
    (void)e; (void)d;
    gtk_widget_hide(w);
    ui.closed = TRUE;
    /* the picture stays on the clipboard while it is ours (at most 10 minutes), like after PrtSc */
    if (clip_active) g_timeout_add_seconds(600, on_give_up, NULL);
    quit_if_idle();
    return TRUE;
}

static gboolean on_ui_key(GtkWidget *w, GdkEventKey *e, gpointer d)
{
    if (e->keyval == GDK_KEY_Escape || ((e->state & GDK_CONTROL_MASK) && (e->keyval == GDK_KEY_w || e->keyval == GDK_KEY_q))) {
        on_ui_delete(w, NULL, d);
        return TRUE;
    }
    return FALSE;
}

static GtkWidget *ui_mode_button(GtkWidget *group, const char *icon, const char *label)
{
    GtkWidget *b = gtk_radio_button_new_from_widget(group ? GTK_RADIO_BUTTON(group) : NULL);
    gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(b), FALSE);
    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(v), 8);
    GtkWidget *im = gtk_image_new_from_icon_name(icon, GTK_ICON_SIZE_DIALOG);
    gtk_image_set_pixel_size(GTK_IMAGE(im), 40);
    gtk_box_pack_start(GTK_BOX(v), im, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v), gtk_label_new_with_mnemonic(label), FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(b), v);
    return b;
}

static GtkWidget *ui_button(const char *label, GCallback cb)
{
    GtkWidget *b = gtk_button_new_with_mnemonic(label);
    g_signal_connect(b, "clicked", cb, NULL);
    return b;
}

static void ui_build(void)
{
    ui.win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(ui.win), "Screenshot");
    gtk_window_set_icon_name(GTK_WINDOW(ui.win), "applets-screenshooter");
    gtk_window_set_default_size(GTK_WINDOW(ui.win), 600, -1);
    gtk_window_set_position(GTK_WINDOW(ui.win), GTK_WIN_POS_CENTER);
    g_signal_connect(ui.win, "delete-event", G_CALLBACK(on_ui_delete), NULL);
    g_signal_connect(ui.win, "key-press-event", G_CALLBACK(on_ui_key), NULL);
    ui.stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(ui.stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_stack_set_vhomogeneous(GTK_STACK(ui.stack), FALSE);    /* the start page as small as it is */
    gtk_container_add(GTK_CONTAINER(ui.win), ui.stack);

    GtkWidget *start = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_container_set_border_width(GTK_CONTAINER(start), 22);
    GtkWidget *title = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(title), "<span size=\"x-large\" weight=\"bold\">Take a screenshot</span>");
    gtk_widget_set_halign(title, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(start), title, FALSE, FALSE, 0);
    GtkWidget *modes = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_box_set_homogeneous(GTK_BOX(modes), TRUE);
    ui.modes[0] = ui_mode_button(NULL, "video-display", "_Whole screen");
    ui.modes[1] = ui_mode_button(ui.modes[0], "window-new", "Active _window");
    ui.modes[2] = ui_mode_button(ui.modes[0], "edit-select-all", "Select an _area");
    for (int i = 0; i < 3; i++) gtk_box_pack_start(GTK_BOX(modes), ui.modes[i], TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(start), modes, FALSE, FALSE, 0);
    GtkWidget *drow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *dl = gtk_label_new_with_mnemonic("_Delay (seconds):");
    ui.delay = gtk_spin_button_new_with_range(0, 30, 1);
    gtk_label_set_mnemonic_widget(GTK_LABEL(dl), ui.delay);
    gtk_box_pack_start(GTK_BOX(drow), dl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(drow), ui.delay, FALSE, FALSE, 0);
    ui.pointer = gtk_check_button_new_with_mnemonic("Show the mouse _pointer");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ui.pointer), want_pointer());
    gtk_widget_set_tooltip_text(ui.pointer, "For the whole screen and a window (also for the Print key)");
    gtk_widget_set_margin_start(ui.pointer, 12);
    gtk_box_pack_start(GTK_BOX(drow), ui.pointer, FALSE, FALSE, 0);
    ui.take = gtk_button_new_with_mnemonic("_Take Screenshot");
    gtk_style_context_add_class(gtk_widget_get_style_context(ui.take), "suggested-action");
    gtk_widget_set_can_default(ui.take, TRUE);
    g_signal_connect(ui.take, "clicked", G_CALLBACK(on_ui_take), NULL);
    gtk_box_pack_end(GTK_BOX(drow), ui.take, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(start), drow, FALSE, FALSE, 0);
    ui.error = gtk_label_new("");
    gtk_label_set_line_wrap(GTK_LABEL(ui.error), TRUE);
    gtk_label_set_xalign(GTK_LABEL(ui.error), 0);
    gtk_widget_set_no_show_all(ui.error, TRUE);
    gtk_box_pack_start(GTK_BOX(start), ui.error, FALSE, FALSE, 0);
    GtkWidget *hint = gtk_label_new("Keys: Print = whole screen · Alt+Print = window · Shift+Print = area · hold Ctrl as "
                                    "well to only copy it. Pictures go to Pictures/Screenshots.");
    gtk_label_set_line_wrap(GTK_LABEL(hint), TRUE);
    gtk_label_set_xalign(GTK_LABEL(hint), 0);
    gtk_widget_set_opacity(hint, 0.7);
    gtk_box_pack_start(GTK_BOX(start), hint, FALSE, FALSE, 0);
    gtk_stack_add_named(GTK_STACK(ui.stack), start, "start");

    GtkWidget *res = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(res), 18);
    ui.preview = gtk_image_new();
    gtk_widget_set_size_request(ui.preview, -1, 200);
    gtk_box_pack_start(GTK_BOX(res), ui.preview, TRUE, TRUE, 0);
    ui.saved = gtk_label_new("");
    gtk_label_set_line_wrap(GTK_LABEL(ui.saved), TRUE);
    gtk_label_set_selectable(GTK_LABEL(ui.saved), TRUE);
    gtk_widget_set_can_focus(ui.saved, FALSE);
    gtk_box_pack_start(GTK_BOX(res), ui.saved, FALSE, FALSE, 0);
    GtkWidget *acts = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(acts, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(acts), ui_button("_Copy", G_CALLBACK(on_ui_copy)), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(acts), ui_button("Save _As…", G_CALLBACK(on_ui_save_as)), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(acts), ui_button("_Open", G_CALLBACK(on_ui_open)), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(acts), ui_button("Show in _Folder", G_CALLBACK(on_ui_folder)), FALSE, FALSE, 0);
    GtkWidget *again = ui_button("_New Screenshot", G_CALLBACK(on_ui_new));
    gtk_style_context_add_class(gtk_widget_get_style_context(again), "suggested-action");
    gtk_box_pack_start(GTK_BOX(acts), again, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(res), acts, FALSE, FALSE, 0);
    gtk_stack_add_named(GTK_STACK(ui.stack), res, "result");
    gtk_widget_show_all(ui.stack);
    gtk_widget_grab_default(ui.take);
}

static void usage(FILE *f)
{
    fprintf(f,
            "Usage: hde-screenshot [--area | --window] [--delay N] [--file PATH | --clipboard] [--pointer] [--no-clipboard]\n"
            "                      [--no-notify]\n"
            "  (no option)      the whole screen\n"
            "  -a, --area       drag a rectangle (Esc or right click cancels)\n"
            "  -w, --window     the active window\n"
            "  -d, --delay N    wait N seconds first\n"
            "  -f, --file P     save to P (default: ~/Pictures/Screenshots/Screenshot_<date>_<time>.png)\n"
            "  -c, --clipboard  only copy to the clipboard, save no file\n"
            "  -i, --ui         the Screenshot window (mode, delay, then Copy / Save As / Open / Show in Folder)\n"
            "  -p, --pointer    with the mouse pointer (--no-pointer: without; default: screenshot_pointer in settings.ini)\n"
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
        else if (!strcmp(a, "-c") || !strcmp(a, "--clipboard")) opt_clip_only = TRUE;
        else if (!strcmp(a, "--no-clipboard")) opt_no_clip = TRUE;
        else if (!strcmp(a, "--no-notify")) opt_no_notify = TRUE;
        else if (!strcmp(a, "-p") || !strcmp(a, "--pointer")) opt_pointer = 1;
        else if (!strcmp(a, "--no-pointer")) opt_pointer = 0;
        else if (!strcmp(a, "-i") || !strcmp(a, "--ui") || !strcmp(a, "--interactive")) opt_ui = TRUE;
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(stdout); return 0; }
        else { fprintf(stderr, "hde-screenshot: unknown option %s\n", a); usage(stderr); return 2; }
    }
    if (opt_clip_only && (opt_no_clip || opt_file)) {
        fprintf(stderr, "hde-screenshot: --clipboard cannot be combined with --no-clipboard or --file\n");
        return 2;
    }
    const char *wl = g_getenv("WAYLAND_DISPLAY");
    wayland_mode = wl && *wl && !g_strcmp0(g_getenv("XDG_SESSION_TYPE"), "wayland");
    gdk_set_allowed_backends(wayland_mode ? "wayland,x11" : "x11");
    if (!gtk_init_check(&argc, &argv)) {
        fprintf(stderr, "hde-screenshot: cannot open the X display (DISPLAY=%s)\n", g_getenv("DISPLAY") ? g_getenv("DISPLAY") : "unset");
        return 2;
    }
    g_set_application_name("Screenshot");
    if (opt_ui) {
        if (opt_clip_only || opt_file) {
            fprintf(stderr, "hde-screenshot: --ui cannot be combined with --clipboard or --file\n");
            return 2;
        }
        ui_build();
        ui_show_start();
        g_printerr("hde-screenshot: window: shown\n");
    } else if (opt_delay > 0) g_timeout_add_seconds(opt_delay, start_capture, NULL);
    else g_idle_add(start_capture, NULL);
    gtk_main();
    exiting = TRUE;
    if (clip_active) gtk_clipboard_store(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD));
    g_clear_object(&result);
    g_clear_object(&area.frozen);
    g_free(saved_path);
    return exit_code;
}
