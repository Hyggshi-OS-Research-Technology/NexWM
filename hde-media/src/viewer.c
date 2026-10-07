/* viewer.c — the window that shows the pictures (the first part of hde-media; the player and the recorder come next).
 *
 * One window per set of pictures: the picture in a drawing area, a toolbar, a status line, and the keys everybody
 * expects from a picture viewer (Left/Right, +/-, 0/1, r/R, s, i, f/F11, Escape, q). The picture is drawn with cairo,
 * the zoom and the rotation live in the state of media.h (gallery.c), so what is on the screen follows exactly what
 * tests/media-test.c checks.
 *
 *   Left / Right / PageUp / PageDown   previous / next picture (wraps around)
 *   Home / End                          the first / the last picture
 *   + / -                               zoom in / out (the ladder of gallery.c)
 *   0 / 1                               fit the window / 100 % (the size on disk)
 *   r / R                               rotate a quarter turn right / left
 *   s                                   start / stop the slideshow
 *   Space                               the next picture (the slideshow keeps running)
 *   i                                   more about the picture (folder, date, size)
 *   f or F11                            full screen (hide the toolbar), Escape leaves it (again: closes)
 *   Ctrl+O                              open a picture (the file chooser)
 *   q or Ctrl+W                         close the window
 *   The arrow keys, Home and End stop a running slideshow: browsing is not watching.
 *   The mouse wheel zooms, a double click goes full screen, and dragging moves a picture that is bigger than the
 *   window. Dropping a picture on the window opens it.
 *
 * The program logs what it does on stdout with the "hde-media: " prefix (showing 3/12: pic3.png (800x600, fit 62%),
 * zoom 125%, rotated right (now 90°), slideshow on (5 s per picture)): the test in tests/media-test.sh reads those.
 */
#include "viewer.h"     /* the part of this file main.c sees (and the opaque type) */
#include "media.h"      /* the state, and MEDIA_TITLE */

#include <gtk/gtk.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

struct _HdeMediaViewer {
    GtkApplication *app;
    GtkWidget *window, *toolbar, *tools, *scrolled, *area, *status, *statusbar, *slideshow_button;
    HdeMediaView view;
    GdkPixbuf *loaded;          /* the file as it is on disk */
    GdkPixbuf *shown;           /* after rotation and zoom: what the drawing area paints */
    int   shown_w, shown_h;     /* the size of `shown`, for the status line and the first paint */
    int   info;                 /* the "i" key: the long status line */
    int   fullscreen;
    guint tick_id;
    char  error[256];           /* something to say in the status line (a file that cannot be read, ...); a copy: the
                                 * GError it comes from is freed right after */
    int   fit_vw, fit_vh;       /* the size of the window area the current "fit" was computed for (0 = not computed
                                 * yet), so a resize that does not change it does not re-scale anything */
    int   pan_x, pan_y;         /* the mouse position while dragging a zoomed picture */
    int   panning;
    int   alive;                /* the window is still there (main.c asks before handing it a new picture) */
};

static void viewer_refresh(HdeMediaViewer *w);
static void viewer_apply_fullscreen(HdeMediaViewer *w);

/* ------------------------------------------------------------------ messages */

static void viewer_media_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("hde-media: ", stdout);
    vprintf(fmt, ap);
    fputc('\n', stdout);
    fflush(stdout);
    va_end(ap);
}

/* ------------------------------------------------------------------ the picture on the screen */

static GdkPixbuf *viewer_rotated_pixbuf(const HdeMediaViewer *w)
{
    switch (w->view.rotation) {
    case 90:  return gdk_pixbuf_rotate_simple(w->loaded, GDK_PIXBUF_ROTATE_CLOCKWISE);
    case 180: return gdk_pixbuf_rotate_simple(w->loaded, GDK_PIXBUF_ROTATE_UPSIDEDOWN);
    case 270: return gdk_pixbuf_rotate_simple(w->loaded, GDK_PIXBUF_ROTATE_COUNTERCLOCKWISE);
    default:  return (GdkPixbuf *)g_object_ref(w->loaded);
    }
}

/* The picture that fits the window: `zoom` is a percentage (100 = as it is on disk), 0 = fit. It never blows a small
 * picture up to fill the window (that is what 100 % is for). */
static void viewer_update_status(HdeMediaViewer *w)
{
    const char *cur = hde_media_view_current(&w->view);
    char text[1024];
    size_t n = w->view.list.n;
    long i = w->view.index;

    const char *name = cur ? hde_media_basename(cur) : "(no pictures)";
    snprintf(text, sizeof text, "%ld / %zu · %s · %d×%d", i + 1, n, name, w->shown_w, w->shown_h);
    if (w->view.zoom <= 0.0) {
        int percent = w->shown_w > 0 && w->loaded ? (int)(100.0 * w->shown_w / gdk_pixbuf_get_width(w->loaded)) : 100;
        snprintf(text + strlen(text), sizeof text - strlen(text), " · fit %d %%", percent);
    } else {
        snprintf(text + strlen(text), sizeof text - strlen(text), " · %d %%", (int)(w->view.zoom * 100.0 + 0.5));
    }
    if (w->view.rotation) snprintf(text + strlen(text), sizeof text - strlen(text), " · %d°", w->view.rotation);
    if (w->view.slideshow)
        snprintf(text + strlen(text), sizeof text - strlen(text), " · slideshow %.0f s", w->view.interval);

    if (w->info && cur) {
        const HdeMediaItem *it = &w->view.list.items[i];
        char human[32];
        hde_media_human_size(human, sizeof human, it->size);
        GDateTime *dt = g_date_time_new_from_unix_local((gint64)it->mtime);
        char *when = dt ? g_date_time_format(dt, "%Y-%m-%d %H:%M") : NULL;
        char *dir = hde_media_view_dir(&w->view);
        snprintf(text + strlen(text), sizeof text - strlen(text), "\n%s · %s%s%s%s", dir ? dir : "", human,
                 when ? " · " : "", when ? when : "", w->view.recursive ? " · sub-folders" : "");
        free(dir);
        if (dt) g_date_time_unref(dt);
        g_free(when);
    }
    if (w->error[0]) snprintf(text + strlen(text), sizeof text - strlen(text), "  —  %s", w->error);
    gtk_label_set_text(GTK_LABEL(w->status), text);

    char *title = g_strdup_printf("%s (%ld/%zu) — %s", name, i + 1, n, MEDIA_TITLE);
    gtk_window_set_title(GTK_WINDOW(w->window), title);
    g_free(title);
}

static void viewer_refresh(HdeMediaViewer *w)
{
    const char *cur = hde_media_view_current(&w->view);
    GError *err = NULL;

    if (w->shown) { g_object_unref(w->shown); w->shown = NULL; }
    if (w->loaded) { g_object_unref(w->loaded); w->loaded = NULL; }
    w->error[0] = 0;
    w->shown_w = w->shown_h = 0;

    if (!cur) {
        snprintf(w->error, sizeof w->error, "no pictures");
        gtk_widget_set_size_request(w->area, 1, 1);
        viewer_update_status(w);
        gtk_widget_queue_draw(w->area);
        return;
    }

    w->loaded = gdk_pixbuf_new_from_file(cur, &err);
    if (!w->loaded) {
        snprintf(w->error, sizeof w->error, "%s", err && err->message ? err->message : "this file cannot be read");
        viewer_media_log("cannot read %s: %s", cur, w->error);
        gtk_widget_set_size_request(w->area, 1, 1);
        if (err) g_error_free(err);
        viewer_update_status(w);
        gtk_widget_queue_draw(w->area);
        return;
    }

    GdkPixbuf *rot = viewer_rotated_pixbuf(w);
    int rw = gdk_pixbuf_get_width(rot), rh = gdk_pixbuf_get_height(rot);

    /* how big should it be drawn? */
    double scale;
    if (w->view.zoom <= 0.0) {
        int vw = gtk_widget_get_allocated_width(w->scrolled);
        int vh = gtk_widget_get_allocated_height(w->scrolled);
        if (vw < 50 || vh < 50) {
            vw = 1100; vh = 700;                                 /* before the first layout: the default window */
            w->fit_vw = w->fit_vh = 0;                           /* so the real size fits it again afterwards */
        } else {
            w->fit_vw = vw; w->fit_vh = vh;
        }
        double sx = (double)vw / (double)rw, sy = (double)vh / (double)rh;
        scale = sx < sy ? sx : sy;
        if (scale > 1.0) scale = 1.0;                            /* fit never enlarges */
    } else {
        w->fit_vw = w->fit_vh = 0;
        scale = w->view.zoom;
    }
    int tw = (int)(rw * scale + 0.5), th = (int)(rh * scale + 0.5);
    if (tw < 1) tw = 1;
    if (th < 1) th = 1;

    if (tw == rw && th == rh) {
        w->shown = (GdkPixbuf *)g_object_ref(rot);
    } else {
        w->shown = gdk_pixbuf_scale_simple(rot, tw, th, GDK_INTERP_BILINEAR);
    }
    g_object_unref(rot);
    if (!w->shown) {                                             /* out of memory for this size */
        w->shown = (GdkPixbuf *)g_object_ref(w->loaded);
        tw = gdk_pixbuf_get_width(w->loaded);
        th = gdk_pixbuf_get_height(w->loaded);
    }
    w->shown_w = tw;
    w->shown_h = th;
    gtk_widget_set_size_request(w->area, tw, th);
    gtk_widget_set_halign(w->area, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(w->area, GTK_ALIGN_CENTER);

    /* the line the tests read (and that tells the user what is on the screen):
     *   hde-media: showing 3/12: img10.png (640x480, fit 62 %)
     *   hde-media: showing 4/12: pic4.jpg (2048x1536, 25 %, rotated right 90°) */
    char where[64];
    if (w->view.zoom <= 0.0)
        snprintf(where, sizeof where, "fit %d %%", (int)(100.0 * tw / (double)gdk_pixbuf_get_width(w->loaded) + 0.5));
    else
        snprintf(where, sizeof where, "%d %%", (int)(w->view.zoom * 100.0 + 0.5));
    const char *rot = w->view.rotation == 90  ? ", rotated right 90°"
                    : w->view.rotation == 180 ? ", upside down"
                    : w->view.rotation == 270 ? ", rotated left 90°" : "";
    viewer_media_log("showing %ld/%zu: %s (%dx%d, %s%s)", w->view.index + 1, w->view.list.n, hde_media_basename(cur),
                     gdk_pixbuf_get_width(w->loaded), gdk_pixbuf_get_height(w->loaded), where, rot);
    viewer_update_status(w);
    gtk_widget_queue_draw(w->area);
}

static gboolean viewer_on_draw(GtkWidget *widget, cairo_t *cr, gpointer data)
{
    HdeMediaViewer *w = data;

    /* the empty space around a picture that is smaller than the window: the background of the theme */
    GtkAllocation a;
    gtk_widget_get_allocation(widget, &a);
    gtk_render_background(gtk_widget_get_style_context(widget), cr, 0, 0, a.width, a.height);

    if (w->shown) {
        gdk_cairo_set_source_pixbuf(cr, w->shown, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
        cairo_paint(cr);
    }
    return TRUE;
}

/* ------------------------------------------------------------------ what the keys call */

static void viewer_step(HdeMediaViewer *w, int delta)
{
    if (hde_media_view_step(&w->view, delta) == 0) viewer_refresh(w);
}

static void viewer_goto_index(HdeMediaViewer *w, long index)
{
    if (hde_media_view_goto(&w->view, index) == 0) viewer_refresh(w);
}

static void viewer_zoom(HdeMediaViewer *w, int direction)
{
    double z = hde_media_view_zoom(&w->view, direction);
    if (z <= 0.0) viewer_media_log("zoom fit");
    else viewer_media_log("zoom %d %%", (int)(z * 100.0 + 0.5));
    viewer_refresh(w);
}

static void viewer_zoom_to(HdeMediaViewer *w, double z)
{
    hde_media_view_set_zoom(&w->view, z);
    if (z > 0.0) viewer_media_log("zoom %d %%", (int)(z * 100.0 + 0.5));
    else viewer_media_log("zoom fit");
    viewer_refresh(w);
}

static void viewer_rotate_by(HdeMediaViewer *w, int degrees)
{
    int angle = hde_media_view_rotate(&w->view, degrees);
    viewer_media_log("rotated %s (now %d°)", degrees > 0 ? "right" : "left", angle);
    viewer_refresh(w);
}

static gboolean viewer_on_tick(gpointer data)
{
    HdeMediaViewer *w = data;
    if (hde_media_view_tick(&w->view, g_get_monotonic_time() / 1000)) {
        viewer_media_log("slideshow: next picture");
        viewer_step(w, 1);
    }
    return G_SOURCE_CONTINUE;
}

static void viewer_toggle_slideshow(HdeMediaViewer *w)
{
    int on = !w->view.slideshow;
    hde_media_view_slideshow(&w->view, on, g_get_monotonic_time() / 1000);
    gtk_button_set_label(GTK_BUTTON(w->slideshow_button), on ? "Stop slideshow" : "Slideshow");
    if (on) viewer_media_log("slideshow on (%d s per picture)", (int)(w->view.interval + 0.5));
    else viewer_media_log("slideshow off");
    viewer_update_status(w);
}

static void viewer_apply_fullscreen(HdeMediaViewer *w)
{
    if (w->fullscreen) {
        gtk_widget_hide(w->toolbar);
        gtk_widget_hide(w->statusbar);
        gtk_window_fullscreen(GTK_WINDOW(w->window));
    } else {
        gtk_window_unfullscreen(GTK_WINDOW(w->window));
        gtk_widget_show(w->toolbar);
        gtk_widget_show(w->statusbar);
    }
    if (w->fullscreen) viewer_media_log("full screen on");
    else viewer_media_log("full screen off");
}

static void viewer_toggle_fullscreen(HdeMediaViewer *w)
{
    w->fullscreen = !w->fullscreen;
    viewer_apply_fullscreen(w);
}

static void viewer_toggle_info(HdeMediaViewer *w)
{
    w->info = !w->info;
    viewer_update_status(w);
}

/* a picture (or a folder) dropped on the window, or chosen in the file chooser */
static int viewer_open_path(HdeMediaViewer *w, const char *path)
{
    if (hde_media_view_open(&w->view, path) != 0) {
        viewer_media_log("cannot show %s (not a picture, or an empty folder)", path);
        snprintf(w->error, sizeof w->error, "this file cannot be shown");
        viewer_update_status(w);
        return -1;
    }
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
        viewer_media_log("opened %zu pictures in %s (sorted by %s%s)", w->view.list.n, path,
                  hde_media_sort_name(w->view.sort), w->view.recursive ? ", sub-folders too" : "");
    else
        viewer_media_log("opened %s with %zu pictures of its folder (sorted by %s)", path, w->view.list.n,
                  hde_media_sort_name(w->view.sort));
    viewer_refresh(w);
    return 0;
}

static void viewer_open_many(HdeMediaViewer *w, char *const *paths, int n)
{
    if (hde_media_view_open_many(&w->view, paths, n) != 0) {
        viewer_media_log("none of the %d files given is a picture", n);
        snprintf(w->error, sizeof w->error, "none of these files is a picture");
        viewer_update_status(w);
        return;
    }
    viewer_media_log("opened %d files given on the command line (%zu of them pictures)", n, w->view.list.n);
    viewer_refresh(w);
}

static void viewer_on_open_clicked(GtkWidget *button, gpointer data)
{
    HdeMediaViewer *w = data;
    (void)button;
    GtkWidget *dlg = gtk_file_chooser_dialog_new("Open a picture", GTK_WINDOW(w->window), GTK_FILE_CHOOSER_ACTION_OPEN,
                                                 "_Cancel", GTK_RESPONSE_CANCEL, "_Open", GTK_RESPONSE_ACCEPT, NULL);
    GtkFileFilter *f = gtk_file_filter_new();
    gtk_file_filter_set_name(f, "Pictures");
    gtk_file_filter_add_pixbuf_formats(f);
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), f);
    GtkFileFilter *all = gtk_file_filter_new();
    gtk_file_filter_set_name(all, "All files");
    gtk_file_filter_add_pattern(all, "*");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), all);
    char *dir = hde_media_view_dir(&w->view);
    if (dir) gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dlg), dir);
    free(dir);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        if (path) {
            viewer_open_path(w, path);
            g_free(path);
        }
    }
    gtk_widget_destroy(dlg);
}

static gboolean viewer_on_key(GtkWidget *widget, GdkEventKey *ev, gpointer data)
{
    HdeMediaViewer *w = data;
    (void)widget;
    guint k = ev->keyval;
    gboolean ctrl = (ev->state & GDK_CONTROL_MASK) != 0;

    if (ctrl && (k == GDK_KEY_o || k == GDK_KEY_O)) { viewer_on_open_clicked(NULL, w); return TRUE; }
    if (ctrl && (k == GDK_KEY_w || k == GDK_KEY_q || k == GDK_KEY_Q)) { gtk_widget_destroy(w->window); return TRUE; }

    switch (k) {
    case GDK_KEY_Left: case GDK_KEY_Up: case GDK_KEY_Page_Up:
        if (w->view.slideshow) viewer_toggle_slideshow(w);
        viewer_step(w, -1);
        return TRUE;
    case GDK_KEY_Right: case GDK_KEY_Down: case GDK_KEY_Page_Down:
        if (w->view.slideshow) viewer_toggle_slideshow(w);
        viewer_step(w, 1);
        return TRUE;
    case GDK_KEY_space:
        viewer_step(w, 1);            /* on, and the slideshow stays on: Space is "go on" */
        return TRUE;
    case GDK_KEY_Home:
        if (w->view.slideshow) viewer_toggle_slideshow(w);
        viewer_goto_index(w, 0);
        return TRUE;
    case GDK_KEY_End:
        if (w->view.slideshow) viewer_toggle_slideshow(w);
        viewer_goto_index(w, (long)w->view.list.n - 1);
        return TRUE;
    case GDK_KEY_plus: case GDK_KEY_equal: case GDK_KEY_KP_Add: viewer_zoom(w, 1); return TRUE;
    case GDK_KEY_minus: case GDK_KEY_underscore: case GDK_KEY_KP_Subtract: viewer_zoom(w, -1); return TRUE;
    case GDK_KEY_0: case GDK_KEY_KP_0: viewer_zoom_to(w, HDE_MEDIA_ZOOM_FIT); return TRUE;
    case GDK_KEY_1: case GDK_KEY_KP_1: viewer_zoom_to(w, 1.0); return TRUE;
    case GDK_KEY_r: viewer_rotate_by(w, 90); return TRUE;
    case GDK_KEY_R: viewer_rotate_by(w, -90); return TRUE;
    case GDK_KEY_s: case GDK_KEY_S: viewer_toggle_slideshow(w); return TRUE;
    case GDK_KEY_i: case GDK_KEY_I: viewer_toggle_info(w); return TRUE;
    case GDK_KEY_f: case GDK_KEY_F: case GDK_KEY_F11: viewer_toggle_fullscreen(w); return TRUE;
    case GDK_KEY_Escape:
        if (w->fullscreen) { w->fullscreen = 0; viewer_apply_fullscreen(w); }
        else gtk_widget_destroy(w->window);
        return TRUE;
    case GDK_KEY_q: case GDK_KEY_Q: gtk_widget_destroy(w->window); return TRUE;
    default:
        return FALSE;
    }
}

/* dragging a picture that is bigger than the window moves it (the scrollbars come from the scrolled window) */
static gboolean viewer_on_button_press(GtkWidget *widget, GdkEventButton *ev, gpointer data)
{
    HdeMediaViewer *w = data;
    (void)widget;
    if (ev->type == GDK_2BUTTON_PRESS && ev->button == 1) {
        viewer_toggle_fullscreen(w);
        return TRUE;
    }
    if (ev->button == 1) {
        w->panning = 1;
        w->pan_x = (int)ev->x_root;
        w->pan_y = (int)ev->y_root;
        return TRUE;
    }
    return FALSE;
}

static gboolean viewer_on_button_release(GtkWidget *widget, GdkEventButton *ev, gpointer data)
{
    HdeMediaViewer *w = data;
    (void)widget;
    if (ev->button == 1) w->panning = 0;
    return FALSE;
}

static gboolean viewer_on_motion(GtkWidget *widget, GdkEventMotion *ev, gpointer data)
{
    HdeMediaViewer *w = data;
    (void)widget;
    if (!w->panning) return FALSE;
    GtkAdjustment *ha = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(w->scrolled));
    GtkAdjustment *va = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(w->scrolled));
    int dx = (int)ev->x_root - w->pan_x, dy = (int)ev->y_root - w->pan_y;
    w->pan_x = (int)ev->x_root;
    w->pan_y = (int)ev->y_root;
    if (ha) gtk_adjustment_set_value(ha, gtk_adjustment_get_value(ha) - dx);
    if (va) gtk_adjustment_set_value(va, gtk_adjustment_get_value(va) - dy);
    return TRUE;
}

static gboolean viewer_on_scroll(GtkWidget *widget, GdkEventScroll *ev, gpointer data)
{
    HdeMediaViewer *w = data;
    (void)widget;
    if (ev->direction == GDK_SCROLL_UP || ev->direction == GDK_SCROLL_LEFT) viewer_zoom(w, 1);
    else if (ev->direction == GDK_SCROLL_DOWN || ev->direction == GDK_SCROLL_RIGHT) viewer_zoom(w, -1);
    return TRUE;
}

static void viewer_on_drag_data(GtkWidget *widget, GdkDragContext *ctx, gint x, gint y, GtkSelectionData *sel, guint info,
                         guint time, gpointer data)
{
    HdeMediaViewer *w = data;
    (void)widget; (void)ctx; (void)x; (void)y; (void)info; (void)time;
    char **uris = gtk_selection_data_get_uris(sel);
    if (uris) {
        if (uris[0]) {
            char *path = g_filename_from_uri(uris[0], NULL, NULL);
            if (path) {
                viewer_open_path(w, path);
                g_free(path);
            }
        }
        g_strfreev(uris);
    }
    gtk_drag_finish(ctx, TRUE, FALSE, time);
}

/* the window was given a new size (or the first one): with "fit" the picture follows it */
static void viewer_on_size_allocate(GtkWidget *widget, GdkRectangle *alloc, gpointer data)
{
    HdeMediaViewer *w = data;
    (void)widget; (void)alloc;
    if (!w->loaded) return;
    int vw = gtk_widget_get_allocated_width(w->scrolled);
    int vh = gtk_widget_get_allocated_height(w->scrolled);
    if (w->view.zoom > 0.0) return;                       /* an explicit zoom does not follow the window */
    if (vw == w->fit_vw && vh == w->fit_vh) return;       /* the picture already fits this size */
    viewer_refresh(w);
}

static void viewer_on_destroy(GtkWidget *widget, gpointer data)
{
    HdeMediaViewer *w = data;
    (void)widget;
    w->alive = 0;                     /* main.c asks: a new hde-media makes a new window */
    if (w->tick_id) { g_source_remove(w->tick_id); w->tick_id = 0; }
    if (w->shown) { g_object_unref(w->shown); w->shown = NULL; }
    if (w->loaded) { g_object_unref(w->loaded); w->loaded = NULL; }
    hde_media_view_free(&w->view);
    if (w->window) { gtk_widget_destroy(w->window); w->window = NULL; }
}

/* ------------------------------------------------------------------ the window */

static GtkWidget *viewer_tool_button(const char *label, const char *tip, GCallback cb, HdeMediaViewer *w)
{
    GtkWidget *b = gtk_button_new_with_label(label);
    gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text(b, tip);
    g_signal_connect(b, "clicked", cb, w);
    gtk_box_pack_start(GTK_BOX(w->tools), b, FALSE, FALSE, 0);
    return b;
}

static void viewer_on_prev(GtkWidget *b, gpointer d) { (void)b; viewer_step(d, -1); }
static void viewer_on_next(GtkWidget *b, gpointer d) { (void)b; viewer_step(d, 1); }
static void viewer_on_fit(GtkWidget *b, gpointer d) { (void)b; viewer_zoom_to(d, HDE_MEDIA_ZOOM_FIT); }
static void viewer_on_actual(GtkWidget *b, gpointer d) { (void)b; viewer_zoom_to(d, 1.0); }
static void viewer_on_zoom_in(GtkWidget *b, gpointer d) { (void)b; viewer_zoom(d, 1); }
static void viewer_on_zoom_out(GtkWidget *b, gpointer d) { (void)b; viewer_zoom(d, -1); }
static void viewer_on_rot_right(GtkWidget *b, gpointer d) { (void)b; viewer_rotate_by(d, 90); }
static void viewer_on_rot_left(GtkWidget *b, gpointer d) { (void)b; viewer_rotate_by(d, -90); }
static void viewer_on_slideshow(GtkWidget *b, gpointer d) { (void)b; viewer_toggle_slideshow(d); }
static void viewer_on_fullscreen(GtkWidget *b, gpointer d) { (void)b; viewer_toggle_fullscreen(d); }
static void viewer_on_info(GtkWidget *b, gpointer d) { (void)b; viewer_toggle_info(d); }
static void viewer_on_close(GtkWidget *b, gpointer d) { (void)b; gtk_widget_destroy(((HdeMediaViewer *)d)->window); }

static void viewer_build_window(HdeMediaViewer *w)
{
    w->alive = 1;
    w->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    /* WM_CLASS comes from the program name (g_set_prgname("hde-media") in main.c), which is also what the menu
     * entry's StartupWMClass names: setting it here would be deprecated in GTK 3.24 */
    gtk_window_set_default_size(GTK_WINDOW(w->window), 1100, 720);
    gtk_window_set_icon_name(GTK_WINDOW(w->window), "image-x-generic");
    gtk_application_add_window(w->app, GTK_WINDOW(w->window));

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(w->window), box);

    w->toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(w->toolbar), GTK_STYLE_CLASS_TOOLBAR);
    gtk_style_context_add_class(gtk_widget_get_style_context(w->toolbar), "hde-media-toolbar");
    w->tools = w->toolbar;
    viewer_tool_button("Open", "Open a picture (Ctrl+O)", G_CALLBACK(viewer_on_open_clicked), w);
    viewer_tool_button("◀", "Previous picture (Left)", G_CALLBACK(viewer_on_prev), w);
    viewer_tool_button("▶", "Next picture (Right)", G_CALLBACK(viewer_on_next), w);
    viewer_tool_button("Fit", "Fit the window (0)", G_CALLBACK(viewer_on_fit), w);
    viewer_tool_button("100 %", "As it is on disk (1)", G_CALLBACK(viewer_on_actual), w);
    viewer_tool_button("−", "Zoom out (-)", G_CALLBACK(viewer_on_zoom_out), w);
    viewer_tool_button("+", "Zoom in (+)", G_CALLBACK(viewer_on_zoom_in), w);
    viewer_tool_button("↺", "Rotate left (Shift+R)", G_CALLBACK(viewer_on_rot_left), w);
    viewer_tool_button("↻", "Rotate right (r)", G_CALLBACK(viewer_on_rot_right), w);
    w->slideshow_button = viewer_tool_button("Slideshow", "Start / stop the slideshow (s)", G_CALLBACK(viewer_on_slideshow), w);
    viewer_tool_button("Full screen", "Full screen (f / F11)", G_CALLBACK(viewer_on_fullscreen), w);
    viewer_tool_button("i", "More about the picture (i)", G_CALLBACK(viewer_on_info), w);
    viewer_tool_button("Close", "Close the window (q)", G_CALLBACK(viewer_on_close), w);
    gtk_box_pack_start(GTK_BOX(box), w->toolbar, FALSE, FALSE, 0);

    w->scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(w->scrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_hexpand(w->scrolled, TRUE);
    gtk_widget_set_vexpand(w->scrolled, TRUE);
    w->area = gtk_drawing_area_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(w->area), "hde-media-view");
    gtk_widget_add_events(w->area, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK |
                                     GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK);
    gtk_container_add(GTK_CONTAINER(w->scrolled), w->area);
    gtk_box_pack_start(GTK_BOX(box), w->scrolled, TRUE, TRUE, 0);

    w->status = gtk_label_new("");
    gtk_style_context_add_class(gtk_widget_get_style_context(w->status), "hde-media-status");
    gtk_label_set_xalign(GTK_LABEL(w->status), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(w->status), PANGO_ELLIPSIZE_END);
    gtk_label_set_lines(GTK_LABEL(w->status), 2);
    gtk_widget_set_margin_start(w->status, 8);
    gtk_widget_set_margin_end(w->status, 8);
    gtk_widget_set_margin_top(w->status, 3);
    gtk_widget_set_margin_bottom(w->status, 3);
    w->statusbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(w->statusbar), "hde-media-statusbar");
    gtk_box_pack_start(GTK_BOX(w->statusbar), w->status, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), w->statusbar, FALSE, FALSE, 0);

    /* the pictures can be dropped on the window */
    GtkTargetEntry targets[] = { { (gchar *)"text/uri-list", 0, 0 } };
    gtk_drag_dest_set(w->window, GTK_DEST_DEFAULT_ALL, targets, 1, GDK_ACTION_COPY);

    g_signal_connect(w->window, "destroy", G_CALLBACK(viewer_on_destroy), w);
    g_signal_connect(w->window, "key-press-event", G_CALLBACK(viewer_on_key), w);
    g_signal_connect(w->window, "drag-data-received", G_CALLBACK(viewer_on_drag_data), w);
    g_signal_connect(w->area, "draw", G_CALLBACK(viewer_on_draw), w);
    g_signal_connect(w->area, "button-press-event", G_CALLBACK(viewer_on_button_press), w);
    g_signal_connect(w->area, "button-release-event", G_CALLBACK(viewer_on_button_release), w);
    g_signal_connect(w->area, "motion-notify-event", G_CALLBACK(viewer_on_motion), w);
    g_signal_connect(w->area, "scroll-event", G_CALLBACK(viewer_on_scroll), w);
    g_signal_connect(w->scrolled, "size-allocate", G_CALLBACK(viewer_on_size_allocate), w);

    if (w->view.slideshow) gtk_button_set_label(GTK_BUTTON(w->slideshow_button), "Stop slideshow");
    w->tick_id = g_timeout_add(250, viewer_on_tick, w);       /* four times a second: the slideshow is exact enough */
}

/* ------------------------------------------------------------------ the API of viewer.h */

HdeMediaViewer *hde_media_viewer_new(GtkApplication *app, const char *path, char *const *paths, int n_paths,
                                     HdeMediaSort sort, int recursive, double interval, int slideshow, int fullscreen)
{
    HdeMediaViewer *w = g_new0(HdeMediaViewer, 1);
    w->app = app;
    hde_media_view_init(&w->view, sort, recursive, interval);
    w->view.slideshow = slideshow ? 1 : 0;
    w->fullscreen = fullscreen ? 1 : 0;

    if (n_paths > 1) {
        hde_media_view_open_many(&w->view, paths, n_paths);
    } else if (path) {
        hde_media_view_open(&w->view, path);
    } else {
        const char *home = g_get_home_dir();
        char *pictures = g_build_filename(home, "Pictures", NULL);
        const char *start = g_file_test(pictures, G_FILE_TEST_IS_DIR) ? pictures : home;
        hde_media_view_open(&w->view, start);
        g_free(pictures);
    }
    viewer_build_window(w);
    viewer_refresh(w);
    gtk_widget_show_all(w->window);
    if (w->fullscreen) viewer_apply_fullscreen(w);
    gtk_widget_grab_focus(w->window);
    return w;
}

/* ------------------------------------------------------------------ the window as main.c sees it */

int hde_media_viewer_has_pictures(HdeMediaViewer *v)
{
    return v && v->view.list.n > 0;
}

int hde_media_viewer_alive(HdeMediaViewer *v)
{
    return v && v->alive;
}

GtkWidget *hde_media_viewer_widget(HdeMediaViewer *v)
{
    return v ? v->window : NULL;
}

void hde_media_viewer_open(HdeMediaViewer *v, const char *path, char *const *paths, int n_paths)
{
    if (!v || !v->alive) return;
    if (n_paths > 1) viewer_open_many(v, paths, n_paths);
    else if (path) viewer_open_path(v, path);
    else return;
    gtk_window_present(GTK_WINDOW(v->window));
}

void hde_media_viewer_present(HdeMediaViewer *v)
{
    if (v && v->alive) gtk_window_present(GTK_WINDOW(v->window));
}

void hde_media_viewer_free(HdeMediaViewer *v)
{
    if (!v) return;
    if (v->tick_id) { g_source_remove(v->tick_id); v->tick_id = 0; }
    if (v->shown) { g_object_unref(v->shown); v->shown = NULL; }
    if (v->loaded) { g_object_unref(v->loaded); v->loaded = NULL; }
    if (v->alive) hde_media_view_free(&v->view);
    g_free(v);
}
