/* hde-desktop: wallpaper and desktop icons from ~/Desktop
 *
 * Icons: click = select (a visible frame, in the accent color), Ctrl+click = add/remove, drag on the empty
 * desktop = rubber-band selection, double-click / Enter = open. Right-click on an icon opens the icon's own
 * menu (Open, Open With, Open in Terminal, Cut, Copy, Rename, Move to Trash, Properties) for the clicked icon,
 * or for the whole selection when the icon is part of it; right-click on the empty desktop opens the desktop menu.
 * Keys: Delete / Shift+Delete, F2, Enter, Alt+Enter, Ctrl+A / C / X / V, Esc, Menu / Shift+F10.
 * Cut and Copy use the same clipboard format as GNOME/Xfce file managers (x-special/gnome-copied-files).
 */
#include <gtk/gtk.h>
#include <gio/gdesktopappinfo.h>
#include <glib/gstdio.h>
#include "hde-theme.h"
#include "hde-commands.h"
#include "hde-panel-config.h"
#include "hde-wl.h"
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <math.h>

#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
static int panel_top_px, panel_bottom_px = HDE_PANEL_SIZE_DEFAULT;   /* the panel (Settings > Panel): keep icons clear */
#define CELL_W 100
#define CELL_H 108
#define MARGIN 16
#define TOP0 (MARGIN + panel_top_px)          /* first row of icons */

static GtkWidget *win, *fixed;
static GdkPixbuf *wallpaper;
static char *wallpaper_path;
static int wallpaper_mode;          /* 0 Fill, 1 Fit, 2 Stretch, 3 Center (Settings > Appearance) */
static GdkRectangle mon;
static GdkRGBA accent = { 0.208, 0.518, 0.894, 1.0 };   /* #3584e4; replaced by the accent color of Settings */
static gboolean debug_on;                               /* HDE_DEBUG=1: log menus to stderr (session.log) */
static GHashTable *cut_paths;                           /* paths cut to the clipboard: drawn faded */
static GPtrArray *reselect_paths;                       /* select these after the next reload (rename, paste) */
static guint reload_id;

#define DBG(...) do { if (debug_on) { g_printerr("hde-desktop: "); g_printerr(__VA_ARGS__); g_printerr("\n"); } } while (0)

/* Mouse icon dragging and marquee selection */
static GtkWidget *drag_icon = NULL;
static gboolean dragging_icon = FALSE;
static gdouble drag_start_x = 0, drag_start_y = 0;
static gint drag_orig_x = 0, drag_orig_y = 0;
static gint drag_last_x = 0, drag_last_y = 0;
static gboolean selecting = FALSE;
static gdouble select_start_x = 0, select_start_y = 0;
static gdouble select_cur_x = 0, select_cur_y = 0;

typedef enum {
    SORT_NAME,
    SORT_NAME_DESC,
    SORT_MTIME,
    SORT_TYPE,
    SORT_SIZE
} SortMode;

static SortMode sort_mode = SORT_NAME;
static gboolean keep_arranged = FALSE;
static gint cmp_file_paths(gconstpointer a, gconstpointer b);
static void reload_icons(void);
static void queue_reload(void);
static void m_arrange_icons(GtkMenuItem *i, gpointer d);
static void popup_icon_menu(GtkWidget *icon, const GdkEvent *trigger);

/* ---------- configuration ---------- */
static char *config_file(void)
{
    return g_build_filename(g_get_user_config_dir(), "hde", "config.ini", NULL);
}

static GPtrArray *extra_bg;                             /* Wayland: wallpaper-only surfaces on the other screens */
static void redraw_all(void);

static void load_wallpaper(const char *path)
{
    if (path && wallpaper_path && !strcmp(path, wallpaper_path) && wallpaper) {
        if (win) gtk_widget_queue_draw(win);
        return;
    }
    g_clear_object(&wallpaper);
    g_free(wallpaper_path);
    wallpaper_path = path ? g_strdup(path) : NULL;
    if (path) {
        GError *e = NULL;
        wallpaper = gdk_pixbuf_new_from_file(path, &e);
        if (!wallpaper) {
            g_printerr("hde-desktop: cannot load wallpaper %s: %s\n", path, e ? e->message : "?");
            g_clear_error(&e);
        }
    }
    redraw_all();
}

static void load_config(void)
{
    GKeyFile *kf = g_key_file_new();
    char *cf = config_file();
    char *wp = NULL;
    wallpaper_mode = 0;
    if (g_key_file_load_from_file(kf, cf, G_KEY_FILE_NONE, NULL)) {
        wp = g_key_file_get_string(kf, "desktop", "wallpaper", NULL);
        wallpaper_mode = CLAMP(g_key_file_get_integer(kf, "desktop", "wallpaper_mode", NULL), 0, 3);
    }
    if (!wp) {
        /* The old Settings only saved the wallpaper in settings.ini */
        GKeyFile *sk = g_key_file_new();
        char *sp = hde_settings_ini_path();
        if (g_key_file_load_from_file(sk, sp, G_KEY_FILE_NONE, NULL))
            wp = g_key_file_get_string(sk, "settings", "wallpaper", NULL);
        g_free(sp);
        g_key_file_free(sk);
    }
    if (wp && *wp) load_wallpaper(wp);
    else if (win) gtk_widget_queue_draw(win);
    g_free(wp);
    g_key_file_free(kf);
    g_free(cf);
}

static void save_config(void)
{
    GKeyFile *kf = g_key_file_new();
    char *cf = config_file();
    g_key_file_load_from_file(kf, cf, G_KEY_FILE_NONE, NULL);
    if (wallpaper_path) g_key_file_set_string(kf, "desktop", "wallpaper", wallpaper_path);
    char *dir = g_path_get_dirname(cf);
    g_mkdir_with_parents(dir, 0755);
    g_key_file_save_to_file(kf, cf, NULL);
    g_free(dir); g_free(cf);
    g_key_file_free(kf);
}

/* ---------- wallpaper rendering ---------- */
/* the wallpaper in the rectangle of one screen (X, Y, W, H) */
static void paint_wallpaper(cairo_t *cr, double X, double Y, double W, double H)
{
    cairo_save(cr);
    cairo_rectangle(cr, X, Y, W, H);
    cairo_clip(cr);
    cairo_translate(cr, X, Y);
    if (wallpaper) {
        double pw = gdk_pixbuf_get_width(wallpaper), ph = gdk_pixbuf_get_height(wallpaper);
        double sx, sy;
        switch (wallpaper_mode) {
        case 1:  sx = sy = MIN(W / pw, H / ph); break;          /* Fit: the whole image is visible */
        case 2:  sx = W / pw; sy = H / ph; break;                /* Stretch */
        case 3:  sx = sy = 1.0; break;                           /* Center: keep the original size */
        default: sx = sy = MAX(W / pw, H / ph); break;           /* Fill: cover the screen, crop the excess */
        }
        if (wallpaper_mode == 1 || wallpaper_mode == 3) {
            cairo_set_source_rgb(cr, 0.10, 0.12, 0.16);
            cairo_paint(cr);
        }
        cairo_save(cr);
        cairo_translate(cr, (W - pw * sx) / 2, (H - ph * sy) / 2);
        cairo_scale(cr, sx, sy);
        gdk_cairo_set_source_pixbuf(cr, wallpaper, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
        cairo_paint(cr);
        cairo_restore(cr);
    } else {
        cairo_pattern_t *g = cairo_pattern_create_linear(0, 0, W, H);
        cairo_pattern_add_color_stop_rgb(g, 0.0, 0.10, 0.18, 0.36);
        cairo_pattern_add_color_stop_rgb(g, 1.0, 0.22, 0.42, 0.62);
        cairo_set_source(cr, g);
        cairo_paint(cr);
        cairo_pattern_destroy(g);
    }
    cairo_restore(cr);
}

/* one wallpaper per screen (with Extend, each screen shows the whole picture instead of a half) */
static gboolean on_draw(GtkWidget *w, cairo_t *cr, gpointer data)
{
    (void)data;
    int W = gtk_widget_get_allocated_width(w);
    int H = gtk_widget_get_allocated_height(w);
    GdkDisplay *d = gdk_display_get_default();
    int n = gdk_display_get_n_monitors(d);
    if (n <= 1 || hde_wl_is_layer(GTK_WINDOW(win))) {        /* Wayland: this surface is one screen */
        paint_wallpaper(cr, 0, 0, W, H);
        return FALSE;   /* keep drawing the child icons */
    }
    cairo_set_source_rgb(cr, 0.10, 0.12, 0.16);     /* parts of the desktop no screen shows */
    cairo_paint(cr);
    for (int i = 0; i < n; i++) {
        GdkRectangle r;
        gdk_monitor_get_geometry(gdk_display_get_monitor(d, i), &r);
        paint_wallpaper(cr, r.x, r.y, r.width, r.height);
    }
    return FALSE;
}

/* ---------- icons ---------- */
/* Launchers of Type=Link (web / file shortcuts): GDesktopAppInfo only handles Type=Application.
 * Returns the URL (and optionally Name / Icon), or NULL if path is not such a launcher. */
static char *desktop_link_url(const char *path, char **name, char **icon)
{
    GKeyFile *kf = g_key_file_new();
    char *url = NULL;
    if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
        char *type = g_key_file_get_string(kf, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_TYPE, NULL);
        if (!g_strcmp0(type, G_KEY_FILE_DESKTOP_TYPE_LINK)) {
            url = g_key_file_get_string(kf, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_URL, NULL);
            if (name) *name = g_key_file_get_locale_string(kf, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_NAME, NULL, NULL);
            if (icon) *icon = g_key_file_get_string(kf, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_ICON, NULL);
        }
        g_free(type);
    }
    g_key_file_free(kf);
    return url;
}

static void activate_target(const char *path)
{
    GError *err = NULL;
    if (g_str_has_suffix(path, ".desktop")) {
        GDesktopAppInfo *a = g_desktop_app_info_new_from_filename(path);
        if (a) {
            if (!g_app_info_launch(G_APP_INFO(a), NULL, NULL, &err)) {
                g_printerr("hde-desktop: cannot launch %s: %s\n", path, err->message);
                g_clear_error(&err);
            }
            g_object_unref(a);
            return;
        }
        char *url = desktop_link_url(path, NULL, NULL);
        if (url && *url) {
            if (!g_app_info_launch_default_for_uri(url, NULL, &err)) {
                g_printerr("hde-desktop: cannot open %s: %s\n", url, err->message);
                g_clear_error(&err);
                char *argv[] = { "xdg-open", url, NULL };
                g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
            }
            g_free(url);
            return;
        }
        g_free(url);
    }
    char *uri = g_filename_to_uri(path, NULL, NULL);
    if (!uri) return;
    if (!g_app_info_launch_default_for_uri(uri, NULL, &err)) {
        /* No default handler (minimal ISO): try xdg-open / a file manager. */
        g_printerr("hde-desktop: no default handler for %s: %s\n", uri, err->message);
        g_clear_error(&err);
        const char *openers[] = { "xdg-open", "thunar", "pcmanfm", "nautilus", "dolphin", NULL };
        for (int i = 0; openers[i]; i++) {
            char *prog = g_find_program_in_path(openers[i]);
            if (!prog) continue;
            char *argv[] = { prog, (char *)path, NULL };
            gboolean ok = g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
            g_free(prog);
            if (ok) break;
        }
    }
    g_free(uri);
}

/* ---------- selection ---------- */
static gboolean icon_is_selected(GtkWidget *w)
{
    return gtk_style_context_has_class(gtk_widget_get_style_context(w), "selected");
}

static void icon_set_selected(GtkWidget *w, gboolean on)
{
    GtkStyleContext *c = gtk_widget_get_style_context(w);
    if (on == gtk_style_context_has_class(c, "selected")) return;
    if (on) gtk_style_context_add_class(c, "selected");
    else gtk_style_context_remove_class(c, "selected");
    gtk_widget_queue_draw(w);
}

/* Selected icons in stacking order (g_list_free). */
static GList *selected_icons(void)
{
    GList *out = NULL, *children = fixed ? gtk_container_get_children(GTK_CONTAINER(fixed)) : NULL;
    for (GList *l = children; l; l = l->next)
        if (icon_is_selected(l->data)) out = g_list_prepend(out, l->data);
    g_list_free(children);
    return g_list_reverse(out);
}

/* Select only ev (NULL = clear the selection). */
static void select_icon(GtkWidget *ev)
{
    GList *children = fixed ? gtk_container_get_children(GTK_CONTAINER(fixed)) : NULL;
    for (GList *l = children; l; l = l->next) icon_set_selected(l->data, l->data == ev);
    g_list_free(children);
}

static void rounded_rect(cairo_t *cr, double x, double y, double w, double h, double r)
{
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -G_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, G_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, G_PI / 2, G_PI);
    cairo_arc(cr, x + r, y + r, r, G_PI, 3 * G_PI / 2);
    cairo_close_path(cr);
}

/* Hover / selection frame of an icon. A GtkEventBox without its own window never paints CSS backgrounds or
 * borders, so the frame is drawn here, before (= under) the icon and its label. */
static gboolean draw_icon_frame(GtkWidget *w, cairo_t *cr, gpointer d)
{
    (void)d;
    gboolean sel = icon_is_selected(w);
    gboolean hover = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(w), "hover"));
    if (!sel && !hover) return FALSE;
    double W = gtk_widget_get_allocated_width(w), H = gtk_widget_get_allocated_height(w);
    cairo_save(cr);
    if (sel) {
        /* a dark outline keeps the frame visible on light wallpapers, the accent fill and light edge on dark ones */
        rounded_rect(cr, 0.5, 0.5, W - 1, H - 1, 9);
        cairo_set_source_rgba(cr, 0, 0, 0, 0.45);
        cairo_set_line_width(cr, 1);
        cairo_stroke(cr);
        rounded_rect(cr, 2, 2, W - 4, H - 4, 8);
        cairo_set_source_rgba(cr, accent.red, accent.green, accent.blue, hover ? 0.62 : 0.50);
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, accent.red * 0.4 + 0.6, accent.green * 0.4 + 0.6, accent.blue * 0.4 + 0.6, 0.95);
        cairo_set_line_width(cr, 2);
        cairo_stroke(cr);
    } else {
        rounded_rect(cr, 1.5, 1.5, W - 3, H - 3, 8);
        cairo_set_source_rgba(cr, 1, 1, 1, 0.16);
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, 1, 1, 1, 0.42);
        cairo_set_line_width(cr, 1);
        cairo_stroke(cr);
    }
    cairo_restore(cr);
    return FALSE;   /* GTK then draws the icon and the label on top */
}

static gboolean on_icon_crossing(GtkWidget *w, GdkEventCrossing *e, gpointer d)
{
    (void)d;
    if (e->detail == GDK_NOTIFY_INFERIOR) return FALSE;
    g_object_set_data(G_OBJECT(w), "hover", GINT_TO_POINTER(e->type == GDK_ENTER_NOTIFY));
    gtk_widget_queue_draw(w);
    return FALSE;
}

static void save_icon_position(GtkWidget *ev)
{
    const char *path = g_object_get_data(G_OBJECT(ev), "target");
    if (!path || !fixed) return;
    gint x = 0, y = 0;
    gtk_container_child_get(GTK_CONTAINER(fixed), ev, "x", &x, "y", &y, NULL);

    GKeyFile *kf = g_key_file_new();
    char *cf = config_file();
    g_key_file_load_from_file(kf, cf, G_KEY_FILE_NONE, NULL);
    char *key = g_compute_checksum_for_string(G_CHECKSUM_MD5, path, -1);
    g_key_file_set_integer(kf, "icon-positions", key, x);
    /* store y next to x using a second group/key pair */
    char *ykey = g_strdup_printf("%s_y", key);
    g_key_file_set_integer(kf, "icon-positions", ykey, y);
    char *dir = g_path_get_dirname(cf);
    g_mkdir_with_parents(dir, 0755);
    g_key_file_save_to_file(kf, cf, NULL);
    g_free(dir); g_free(ykey); g_free(key); g_free(cf);
    g_key_file_free(kf);
}

/* Detect double-clicks ourselves instead of relying only on GDK_2BUTTON_PRESS: GDK drops a
 * double-click if the pointer moves >5px, or under Xephyr/touchpads, so icons felt "unclickable". */
static GtkWidget *last_press_icon = NULL;
static guint32    last_press_time = 0;
static gdouble    last_press_x = 0, last_press_y = 0;
static guint32    last_activate_time = 0;

static void launch_icon(GtkWidget *ev, guint32 t)
{
    const char *path = g_object_get_data(G_OBJECT(ev), "target");
    last_activate_time = t;
    last_press_icon = NULL;
    drag_icon = NULL;
    dragging_icon = FALSE;
    if (path) activate_target(path);
}

static gboolean on_icon_press(GtkWidget *ev, GdkEventButton *e, gpointer data)
{
    (void)data;
    if (e->button == 3) {
        /* The icon's own menu — never the desktop menu. Right-click on an icon that is part of the selection
         * acts on the whole selection; on any other icon it selects just that icon. */
        if (e->type == GDK_BUTTON_PRESS) {
            if (!icon_is_selected(ev)) select_icon(ev);
            popup_icon_menu(ev, (GdkEvent *)e);
        }
        return TRUE;
    }
    if (e->button != 1) return FALSE;
    if (e->type == GDK_3BUTTON_PRESS) return TRUE;
    if ((e->state & GDK_CONTROL_MASK) && e->type == GDK_BUTTON_PRESS) {   /* Ctrl+click: add / remove one icon */
        icon_set_selected(ev, !icon_is_selected(ev));
        last_press_icon = NULL;
        return TRUE;
    }
    select_icon(ev);

    if (e->type == GDK_2BUTTON_PRESS) {
        /* Already opened by the self-detection branch on the 2nd press: ignore. */
        if (e->time != last_activate_time && e->time - last_activate_time > 50)
            launch_icon(ev, e->time);
        return TRUE;
    }

    guint dbl_time = 400;
    g_object_get(gtk_settings_get_default(), "gtk-double-click-time", &dbl_time, NULL);
    if (last_press_icon == ev && (e->time - last_press_time) <= dbl_time &&
        fabs(e->x_root - last_press_x) <= 16 && fabs(e->y_root - last_press_y) <= 16) {
        launch_icon(ev, e->time);
        return TRUE;
    }
    last_press_icon = ev;
    last_press_time = e->time;
    last_press_x = e->x_root;
    last_press_y = e->y_root;
    drag_icon = ev;
    dragging_icon = FALSE;
    drag_start_x = e->x_root;
    drag_start_y = e->y_root;
    gtk_container_child_get(GTK_CONTAINER(fixed), ev, "x", &drag_orig_x, "y", &drag_orig_y, NULL);
    drag_last_x = drag_orig_x;
    drag_last_y = drag_orig_y;
    return TRUE;
}

static void snap_icon_position(gint *x, gint *y)
{
    gint max_x = MAX(MARGIN, gtk_widget_get_allocated_width(fixed) - CELL_W);
    gint max_y = MAX(TOP0, gtk_widget_get_allocated_height(fixed) - panel_bottom_px - CELL_H);
    gint sx = MARGIN + (gint)round(((*x - MARGIN) / (gdouble)CELL_W)) * CELL_W;
    gint sy = TOP0 + (gint)round(((*y - TOP0) / (gdouble)CELL_H)) * CELL_H;
    sx = MAX(MARGIN, MIN(sx, max_x));
    sy = MAX(TOP0, MIN(sy, max_y));
    *x = sx;
    *y = sy;
}

/* Return TRUE when another desktop icon already occupies this grid cell.
 * The dragged icon itself is ignored, so an icon can stay in its own cell. */
static gboolean icon_cell_occupied(gint x, gint y, GtkWidget *ignore)
{
    if (!fixed) return FALSE;

    GList *children = gtk_container_get_children(GTK_CONTAINER(fixed));
    gboolean occupied = FALSE;

    for (GList *l = children; l; l = l->next) {
        GtkWidget *child = GTK_WIDGET(l->data);
        if (child == ignore) continue;

        gint cx = 0, cy = 0;
        gtk_container_child_get(GTK_CONTAINER(fixed), child,
                                "x", &cx, "y", &cy, NULL);

        if (cx == x && cy == y) {
            occupied = TRUE;
            break;
        }
    }

    g_list_free(children);
    return occupied;
}

/* Find the nearest free grid cell.  This is used when an old config contains
 * duplicate positions or when an icon is dropped onto an occupied cell. */
static gboolean find_free_cell(gint wanted_x, gint wanted_y,
                               GtkWidget *ignore, gint *out_x, gint *out_y)
{
    if (!fixed || !out_x || !out_y) return FALSE;

    gint max_x = MAX(MARGIN, gtk_widget_get_allocated_width(fixed) - CELL_W);
    gint max_y = MAX(TOP0, gtk_widget_get_allocated_height(fixed) - panel_bottom_px - CELL_H);
    gint cols = MAX(1, ((max_x - MARGIN) / CELL_W) + 1);
    gint rows = MAX(1, ((max_y - TOP0) / CELL_H) + 1);

    gint wx = wanted_x;
    gint wy = wanted_y;
    snap_icon_position(&wx, &wy);

    /* Search in expanding Manhattan rings around the requested cell. */
    gint target_col = (wx - MARGIN) / CELL_W;
    gint target_row = (wy - TOP0) / CELL_H;
    gint max_radius = MAX(cols, rows);

    for (gint radius = 0; radius <= max_radius; ++radius) {
        for (gint row = MAX(0, target_row - radius);
             row <= MIN(rows - 1, target_row + radius); ++row) {
            for (gint col = MAX(0, target_col - radius);
                 col <= MIN(cols - 1, target_col + radius); ++col) {
                if (MAX(abs(col - target_col), abs(row - target_row)) != radius)
                    continue;

                gint x = MARGIN + col * CELL_W;
                gint y = TOP0 + row * CELL_H;
                if (!icon_cell_occupied(x, y, ignore)) {
                    *out_x = x;
                    *out_y = y;
                    return TRUE;
                }
            }
        }
    }

    return FALSE;
}

static gboolean on_icon_motion(GtkWidget *ev, GdkEventMotion *e, gpointer data)
{
    (void)data;
    if (drag_icon != ev || !(e->state & GDK_BUTTON1_MASK)) return FALSE;
    gdouble dx = e->x_root - drag_start_x;
    gdouble dy = e->y_root - drag_start_y;
    if (!dragging_icon && (fabs(dx) > 4.0 || fabs(dy) > 4.0)) dragging_icon = TRUE;
    if (!dragging_icon) return TRUE;

    gint nx = drag_orig_x + (gint)dx;
    gint ny = drag_orig_y + (gint)dy;
    nx = MAX(MARGIN, MIN(nx, gtk_widget_get_allocated_width(fixed) - CELL_W));
    ny = MAX(TOP0, MIN(ny, gtk_widget_get_allocated_height(fixed) - panel_bottom_px - CELL_H));
    snap_icon_position(&nx, &ny);

    /* Never place two icons in the same grid cell.  While dragging, an
     * occupied target is simply rejected and the icon stays at its last
     * valid position instead of being painted over another icon. */
    if (!icon_cell_occupied(nx, ny, ev)) {
        gtk_fixed_move(GTK_FIXED(fixed), ev, nx, ny);
        drag_last_x = nx;
        drag_last_y = ny;
    }
    return TRUE;
}

static gboolean on_icon_release(GtkWidget *ev, GdkEventButton *e, gpointer data)
{
    (void)data;
    if (e->button != 1 || drag_icon != ev) return FALSE;
    if (dragging_icon) {
        keep_arranged = FALSE;

        /* Final safety check: a stale/old config or resize must never leave
         * the icon stacked on another icon. */
        gint x = drag_last_x, y = drag_last_y;
        if (icon_cell_occupied(x, y, ev)) {
            if (find_free_cell(x, y, ev, &x, &y))
                gtk_fixed_move(GTK_FIXED(fixed), ev, x, y);
        }
        save_icon_position(ev);
    }
    drag_icon = NULL;
    dragging_icon = FALSE;
    return TRUE;
}

static GtkWidget *make_icon(const char *name, GIcon *icon, const char *path)
{
    GtkWidget *ev = gtk_event_box_new();
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(ev), FALSE);
    gtk_widget_add_events(ev, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK |
                              GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK);
    gtk_style_context_add_class(gtk_widget_get_style_context(ev), "desk-icon");
    g_object_set_data_full(G_OBJECT(ev), "target", g_strdup(path), g_free);
    gtk_widget_set_tooltip_text(ev, name);                 /* long names are cut to two lines */
    if (cut_paths && g_hash_table_contains(cut_paths, path)) gtk_widget_set_opacity(ev, 0.5);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_size_request(box, CELL_W - 8, CELL_H - 8);
    GtkWidget *img = gtk_image_new_from_gicon(icon, GTK_ICON_SIZE_DIALOG);
    gtk_image_set_pixel_size(GTK_IMAGE(img), 48);
    GtkWidget *lbl = gtk_label_new(name);
    gtk_label_set_line_wrap(GTK_LABEL(lbl), TRUE);
    gtk_label_set_line_wrap_mode(GTK_LABEL(lbl), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
    gtk_label_set_lines(GTK_LABEL(lbl), 2);
    gtk_label_set_max_width_chars(GTK_LABEL(lbl), 12);
    gtk_label_set_justify(GTK_LABEL(lbl), GTK_JUSTIFY_CENTER);
    gtk_box_pack_start(GTK_BOX(box), img, FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(box), lbl, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(ev), box);
    g_signal_connect(ev, "draw", G_CALLBACK(draw_icon_frame), NULL);
    g_signal_connect(ev, "enter-notify-event", G_CALLBACK(on_icon_crossing), NULL);
    g_signal_connect(ev, "leave-notify-event", G_CALLBACK(on_icon_crossing), NULL);
    g_signal_connect(ev, "button-press-event", G_CALLBACK(on_icon_press), NULL);
    g_signal_connect(ev, "motion-notify-event", G_CALLBACK(on_icon_motion), NULL);
    g_signal_connect(ev, "button-release-event", G_CALLBACK(on_icon_release), NULL);
    return ev;
}

static void destroy_child(GtkWidget *w, gpointer d) { gtk_widget_destroy(w); }

static gboolean restore_icon_position(GtkWidget *w, const char *path)
{
    if (!path || !fixed) return FALSE;
    GKeyFile *kf = g_key_file_new();
    char *cf = config_file();
    gboolean ok = g_key_file_load_from_file(kf, cf, G_KEY_FILE_NONE, NULL);
    gboolean found = FALSE;
    if (ok) {
        char *key = g_compute_checksum_for_string(G_CHECKSUM_MD5, path, -1);
        char *ykey = g_strdup_printf("%s_y", key);
        GError *err = NULL;
        gint x = g_key_file_get_integer(kf, "icon-positions", key, &err);
        if (!err) {
            gint y = g_key_file_get_integer(kf, "icon-positions", ykey, &err);
            if (!err) {
                snap_icon_position(&x, &y);
                if (icon_cell_occupied(x, y, w)) {
                    gint fx = x, fy = y;
                    if (find_free_cell(x, y, w, &fx, &fy)) {
                        x = fx;
                        y = fy;
                    } else {
                        g_clear_error(&err);
                        g_free(ykey);
                        g_free(key);
                        g_free(cf);
                        g_key_file_free(kf);
                        return FALSE;
                    }
                }
                gtk_fixed_put(GTK_FIXED(fixed), w, x, y);
                found = TRUE;
            }
        }
        g_clear_error(&err);
        g_free(ykey); g_free(key);
    }
    g_free(cf); g_key_file_free(kf);
    return found;
}

static void reload_icons(void)
{
    /* keep the selection across reloads (by path); after Rename / Paste select the new items instead */
    GHashTable *keep = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    if (reselect_paths && reselect_paths->len) {
        for (guint i = 0; i < reselect_paths->len; i++) g_hash_table_add(keep, g_strdup(g_ptr_array_index(reselect_paths, i)));
        g_ptr_array_set_size(reselect_paths, 0);
    } else {
        GList *sel = selected_icons();
        for (GList *l = sel; l; l = l->next) {
            const char *p = g_object_get_data(G_OBJECT(l->data), "target");
            if (p) g_hash_table_add(keep, g_strdup(p));
        }
        g_list_free(sel);
    }
    if (reload_id) { g_source_remove(reload_id); reload_id = 0; }
    drag_icon = NULL;
    dragging_icon = FALSE;
    last_press_icon = NULL;
    gtk_container_foreach(GTK_CONTAINER(fixed), destroy_child, NULL);

    gint available_h = MAX(CELL_H, mon.height - panel_top_px - panel_bottom_px - 2 * MARGIN);
    int rows = MAX(1, available_h / CELL_H);
    int idx = 0;

    #define PLACE(w) do { \
        const char *_pp = g_object_get_data(G_OBJECT(w), "target"); \
        if (keep_arranged || !restore_icon_position((w), _pp)) { \
            gint _px = mon.x + MARGIN + (idx / rows) * CELL_W; \
            gint _py = mon.y + TOP0 + (idx % rows) * CELL_H; \
            gint _fx = _px, _fy = _py; \
            if (!find_free_cell(_px, _py, (w), &_fx, &_fy)) { \
                _fx = _px; _fy = _py; \
            } \
            gtk_fixed_put(GTK_FIXED(fixed), (w), _fx, _fy); \
        } \
        if (_pp && g_hash_table_contains(keep, _pp)) icon_set_selected((w), TRUE); \
        idx++; \
    } while (0)

    /* The Home folder is always present */
    GIcon *home_icon = g_themed_icon_new("user-home");
    GtkWidget *home = make_icon("Home", home_icon, g_get_home_dir());
    g_object_unref(home_icon);
    g_object_set_data(G_OBJECT(home), "home", GINT_TO_POINTER(1));   /* no Cut / Rename / Trash for Home */
    PLACE(home);

    const char *ddir = g_get_user_special_dir(G_USER_DIRECTORY_DESKTOP);
    GDir *d = ddir ? g_dir_open(ddir, 0, NULL) : NULL;
    if (d) {
        GList *names = NULL;
        const char *fn;
        while ((fn = g_dir_read_name(d)))
            if (fn[0] != '.') names = g_list_prepend(names, g_strdup(fn));
        g_dir_close(d);
        names = g_list_sort(names, cmp_file_paths);

        for (GList *l = names; l; l = l->next) {
            char *path = g_build_filename(ddir, l->data, NULL);
            GIcon *icon = NULL;
            char *label = g_strdup(l->data);

            if (g_str_has_suffix(path, ".desktop")) {
                GDesktopAppInfo *a = g_desktop_app_info_new_from_filename(path);
                if (a) {
                    icon = g_object_ref(g_app_info_get_icon(G_APP_INFO(a)) ?
                                        g_app_info_get_icon(G_APP_INFO(a)) : g_themed_icon_new("application-x-executable"));
                    g_free(label);
                    label = g_strdup(g_app_info_get_display_name(G_APP_INFO(a)));
                    g_object_unref(a);
                } else {
                    char *lname = NULL, *licon = NULL;
                    char *url = desktop_link_url(path, &lname, &licon);
                    if (url) {
                        if (lname && *lname) { g_free(label); label = g_strdup(lname); }
                        if (licon && *licon) {
                            if (g_path_is_absolute(licon)) {
                                GFile *lf = g_file_new_for_path(licon);
                                icon = g_file_icon_new(lf);
                                g_object_unref(lf);
                            } else {
                                icon = g_themed_icon_new_with_default_fallbacks(licon);
                            }
                        } else {
                            icon = g_themed_icon_new(g_str_has_prefix(url, "http") ? "web-browser" : "emblem-symbolic-link");
                        }
                    }
                    g_free(url); g_free(lname); g_free(licon);
                }
            }
            if (!icon) {
                GFile *f = g_file_new_for_path(path);
                GFileInfo *fi = g_file_query_info(f, "standard::icon", 0, NULL, NULL);
                if (fi && g_file_info_get_icon(fi)) icon = g_object_ref(g_file_info_get_icon(fi));
                g_clear_object(&fi);
                g_object_unref(f);
            }
            if (!icon) icon = g_themed_icon_new("text-x-generic");

            GtkWidget *w = make_icon(label, icon, path);
            PLACE(w);
            g_object_unref(icon); g_free(label); g_free(path);
        }
        g_list_free_full(names, g_free);
    }
    #undef PLACE
    g_hash_table_unref(keep);
    gtk_widget_show_all(fixed);
}

/* File changes come in bursts (created + changed + attributes...): reload once, and never in the middle of a drag. */
static gboolean reload_idle(gpointer d)
{
    (void)d;
    if (dragging_icon || selecting) return G_SOURCE_CONTINUE;
    reload_id = 0;
    reload_icons();
    return G_SOURCE_REMOVE;
}

static void queue_reload(void)
{
    if (reload_id) g_source_remove(reload_id);
    reload_id = g_timeout_add(150, reload_idle, NULL);
}

/* ---------- context menu desktop ---------- */
static void launch_uri(const char *uri)
{
    if (!uri) return;
    g_app_info_launch_default_for_uri(uri, NULL, NULL);
}

static gboolean spawn_program(const char *program, GError **error)
{
    if (!program || !*program) return FALSE;

    char *argv[] = { (char *)program, NULL };
    return g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, error);
}

/*
 * Settings is part of NexDE itself, so do not depend only on PATH.
 * When hde-desktop is launched from a display manager/session, PATH can
 * differ from the user's interactive shell. That made `Settings` appear
 * to work only sometimes when hde-settings was still in ./build.
 */
static void launch_hyggshi_settings(GtkMenuItem *item, gpointer data)
{
    (void)item;
    (void)data;

    GPtrArray *paths = g_ptr_array_new_with_free_func(g_free);
    const char *path_env = g_getenv("PATH");
    (void)path_env;

    /* 1. Normal installed binary. */
    char *found = g_find_program_in_path("hde-settings");
    if (found) g_ptr_array_add(paths, found);

    /* 2. Sibling of the running hde-desktop binary.
     *    e.g. /home/me/nexDE/build/hde-desktop -> hde-settings */
    char *self = g_file_read_link("/proc/self/exe", NULL);
    if (self) {
        char *dir = g_path_get_dirname(self);
        char *candidate = g_build_filename(dir, "hde-settings", NULL);
        g_ptr_array_add(paths, candidate);
        g_free(dir);
        g_free(self);
    }

    /* 3. Common installation locations, useful when PATH is incomplete. */
    g_ptr_array_add(paths, g_strdup("/usr/local/bin/hde-settings"));
    g_ptr_array_add(paths, g_strdup("/usr/bin/hde-settings"));

    gboolean started = FALSE;
    const char *started_path = NULL;

    for (guint i = 0; i < paths->len; i++) {
        const char *candidate = g_ptr_array_index(paths, i);
        if (!g_file_test(candidate, G_FILE_TEST_IS_EXECUTABLE)) continue;

        GError *err = NULL;
        if (spawn_program(candidate, &err)) {
            started = TRUE;
            started_path = candidate;
            g_clear_error(&err);
            break;
        }
        g_clear_error(&err);
    }

    if (!started) {
        GtkWidget *parent = win ? win : NULL;
        GtkWidget *dialog = gtk_message_dialog_new(
            parent ? GTK_WINDOW(parent) : NULL,
            GTK_DIALOG_MODAL,
            GTK_MESSAGE_ERROR,
            GTK_BUTTONS_CLOSE,
            "Cannot open Hyggshi Settings");
        gtk_message_dialog_format_secondary_text(
            GTK_MESSAGE_DIALOG(dialog),
            "hde-settings was not found or could not be started.\n\n"
            "Build it with `make` or install NexDE with `sudo make install`.\n"
            "Expected: ./build/hde-settings or /usr/local/bin/hde-settings");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
    } else {
        (void)started_path;
    }

    g_ptr_array_free(paths, TRUE);
}

/* Terminal (same list as Ctrl+Alt+T) whose working directory is dir. */
static void open_terminal_in(const char *dir)
{
    char *argv[] = { "/bin/sh", "-c", HDE_SH_TERMINAL, NULL };
    GError *err = NULL;
    if (!g_spawn_async(dir && g_file_test(dir, G_FILE_TEST_IS_DIR) ? dir : NULL, argv, NULL, G_SPAWN_DEFAULT,
                       NULL, NULL, NULL, &err)) {
        g_printerr("hde-desktop: cannot start a terminal: %s\n", err->message);
        g_clear_error(&err);
    }
}

static void m_terminal(GtkMenuItem *i, gpointer d)
{
    (void)i; (void)d;
    open_terminal_in(g_get_user_special_dir(G_USER_DIRECTORY_DESKTOP));
}

static void m_wallpaper(GtkMenuItem *i, gpointer d)
{
    (void)i; (void)d;
    GtkWidget *dlg = gtk_file_chooser_dialog_new("Choose Background", NULL, GTK_FILE_CHOOSER_ACTION_OPEN,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Select", GTK_RESPONSE_ACCEPT, NULL);
    GtkFileFilter *ff = gtk_file_filter_new();
    gtk_file_filter_set_name(ff, "Images");
    gtk_file_filter_add_pixbuf_formats(ff);
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), ff);
    const char *pictures = g_get_user_special_dir(G_USER_DIRECTORY_PICTURES);
    if (pictures) gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dlg), pictures);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char *f = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        load_wallpaper(f);
        save_config();
        g_free(f);
    }
    gtk_widget_destroy(dlg);
}

static void m_new_folder(GtkMenuItem *i, gpointer d)
{
    (void)i; (void)d;
    const char *ddir = g_get_user_special_dir(G_USER_DIRECTORY_DESKTOP);
    if (!ddir) return;

    GtkWidget *dlg = gtk_dialog_new_with_buttons("New Folder", NULL, GTK_DIALOG_MODAL,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Create", GTK_RESPONSE_ACCEPT, NULL);
    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(box), 14);
    gtk_box_pack_start(GTK_BOX(content), gtk_label_new("Folder name:"), FALSE, FALSE, 0);
    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(entry), "New Folder");
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    gtk_box_pack_start(GTK_BOX(content), entry, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(content), box, FALSE, FALSE, 0);
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_ACCEPT);
    gtk_widget_show_all(dlg);

    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        const char *name = gtk_entry_get_text(GTK_ENTRY(entry));
        if (name && *name) {
            char *path = g_build_filename(ddir, name, NULL);
            if (g_mkdir(path, 0755) != 0) {
                GtkWidget *err = gtk_message_dialog_new(GTK_WINDOW(win), GTK_DIALOG_MODAL,
                    GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE,
                    "Could not create folder:\n%s", g_strerror(errno));
                gtk_dialog_run(GTK_DIALOG(err));
                gtk_widget_destroy(err);
            } else {
                reload_icons();
            }
            g_free(path);
        }
    }
    gtk_widget_destroy(dlg);
}

static void m_show_desktop_files(GtkMenuItem *i, gpointer d)
{
    (void)i; (void)d;
    const char *ddir = g_get_user_special_dir(G_USER_DIRECTORY_DESKTOP);
    if (!ddir) return;
    char *uri = g_filename_to_uri(ddir, NULL, NULL);
    launch_uri(uri);
    g_free(uri);
}

static void m_refresh(GtkMenuItem *i, gpointer d)
{
    (void)i; (void)d;
    reload_icons();
}

static void m_select_all(GtkMenuItem *i, gpointer d)
{
    (void)i; (void)d;
    GList *children = gtk_container_get_children(GTK_CONTAINER(fixed));
    for (GList *l = children; l; l = l->next) icon_set_selected(l->data, TRUE);
    g_list_free(children);
}

static gboolean clipboard_has_files(void)
{
    GtkClipboard *cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    gint n = 0;
    GdkAtom *targets = NULL;
    gboolean found = FALSE;
    if (gtk_clipboard_wait_for_targets(cb, &targets, &n)) {
        for (gint i = 0; i < n && !found; i++) {
            gchar *name = gdk_atom_name(targets[i]);
            if (name && (!g_ascii_strcasecmp(name, "text/uri-list") || !strcmp(name, "x-special/gnome-copied-files")))
                found = TRUE;
            g_free(name);
        }
        g_free(targets);
    }
    return found;
}

/* ---------- file operations (icon menu, keyboard, Paste) ---------- */
static gboolean is_dir(const char *p) { return g_file_test(p, G_FILE_TEST_IS_DIR); }
static gboolean is_launcher(const char *p) { return g_str_has_suffix(p, ".desktop") && !is_dir(p); }
static gboolean path_exists(const char *p) { GStatBuf st; return g_lstat(p, &st) == 0; }

static void message(GtkMessageType type, const char *primary, const char *secondary)
{
    GtkWidget *d = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL, type, GTK_BUTTONS_CLOSE, "%s", primary);
    if (secondary && *secondary) gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), "%s", secondary);
    gtk_window_set_position(GTK_WINDOW(d), GTK_WIN_POS_CENTER);
    gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
}

static gboolean confirm(const char *primary, const char *secondary, const char *ok_label)
{
    GtkWidget *d = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING, GTK_BUTTONS_NONE, "%s", primary);
    if (secondary && *secondary) gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), "%s", secondary);
    gtk_dialog_add_buttons(GTK_DIALOG(d), "_Cancel", GTK_RESPONSE_CANCEL, ok_label, GTK_RESPONSE_ACCEPT, NULL);
    GtkWidget *ok = gtk_dialog_get_widget_for_response(GTK_DIALOG(d), GTK_RESPONSE_ACCEPT);
    if (ok) gtk_style_context_add_class(gtk_widget_get_style_context(ok), "destructive-action");
    gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_CANCEL);
    gtk_window_set_position(GTK_WINDOW(d), GTK_WIN_POS_CENTER);
    gboolean yes = gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_ACCEPT;
    gtk_widget_destroy(d);
    return yes;
}

/* Saved icon position (config.ini [icon-positions], md5 of the path): follow a rename, forget a deleted file. */
static void move_icon_position(const char *old_path, const char *new_path)
{
    GKeyFile *kf = g_key_file_new();
    char *cf = config_file();
    if (g_key_file_load_from_file(kf, cf, G_KEY_FILE_KEEP_COMMENTS, NULL)) {
        char *ok = g_compute_checksum_for_string(G_CHECKSUM_MD5, old_path, -1);
        char *oky = g_strdup_printf("%s_y", ok);
        GError *e = NULL;
        gint x = g_key_file_get_integer(kf, "icon-positions", ok, &e);
        gint y = e ? 0 : g_key_file_get_integer(kf, "icon-positions", oky, &e);
        if (!e) {
            g_key_file_remove_key(kf, "icon-positions", ok, NULL);
            g_key_file_remove_key(kf, "icon-positions", oky, NULL);
            if (new_path) {
                char *nk = g_compute_checksum_for_string(G_CHECKSUM_MD5, new_path, -1);
                char *nky = g_strdup_printf("%s_y", nk);
                g_key_file_set_integer(kf, "icon-positions", nk, x);
                g_key_file_set_integer(kf, "icon-positions", nky, y);
                g_free(nky);
                g_free(nk);
            }
            g_key_file_save_to_file(kf, cf, NULL);
        }
        g_clear_error(&e);
        g_free(oky);
        g_free(ok);
    }
    g_free(cf);
    g_key_file_free(kf);
}

static gboolean delete_recursive(GFile *f, GError **err)
{
    /* never follow a symlink into its target: only real folders are emptied first */
    if (g_file_query_file_type(f, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL) == G_FILE_TYPE_DIRECTORY) {
        GFileEnumerator *en = g_file_enumerate_children(f, G_FILE_ATTRIBUTE_STANDARD_NAME,
                                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, err);
        if (!en) return FALSE;
        GFileInfo *fi;
        gboolean ok = TRUE;
        while (ok && (fi = g_file_enumerator_next_file(en, NULL, err))) {
            GFile *c = g_file_get_child(f, g_file_info_get_name(fi));
            ok = delete_recursive(c, err);
            g_object_unref(c);
            g_object_unref(fi);
        }
        g_object_unref(en);
        if (!ok || (err && *err)) return FALSE;
    }
    return g_file_delete(f, NULL, err);
}

static gboolean copy_recursive(GFile *src, GFile *dst, GError **err)
{
    if (g_file_query_file_type(src, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL) != G_FILE_TYPE_DIRECTORY)
        return g_file_copy(src, dst, G_FILE_COPY_NOFOLLOW_SYMLINKS | G_FILE_COPY_ALL_METADATA, NULL, NULL, NULL, err);
    if (!g_file_make_directory(dst, NULL, err)) return FALSE;
    GFileEnumerator *en = g_file_enumerate_children(src, G_FILE_ATTRIBUTE_STANDARD_NAME,
                                                    G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, err);
    if (!en) return FALSE;
    GFileInfo *fi;
    gboolean ok = TRUE;
    while (ok && (fi = g_file_enumerator_next_file(en, NULL, err))) {
        GFile *s = g_file_get_child(src, g_file_info_get_name(fi));
        GFile *d = g_file_get_child(dst, g_file_info_get_name(fi));
        ok = copy_recursive(s, d, err);
        g_object_unref(s);
        g_object_unref(d);
        g_object_unref(fi);
    }
    g_object_unref(en);
    return ok && !(err && *err);
}

/* dir/name, or "name (copy).ext", "name (copy 2).ext", ... when that name is taken. */
static char *unique_path(const char *dir, const char *name, gboolean folder)
{
    char *path = g_build_filename(dir, name, NULL);
    if (!path_exists(path)) return path;
    g_free(path);
    const char *dot = folder ? NULL : strrchr(name, '.');
    if (dot == name) dot = NULL;                               /* ".bashrc" has no extension */
    char *stem = dot ? g_strndup(name, dot - name) : g_strdup(name);
    const char *ext = dot ? dot : "";
    path = NULL;
    for (int i = 1; i < 10000 && !path; i++) {
        char *n = i == 1 ? g_strdup_printf("%s (copy)%s", stem, ext) : g_strdup_printf("%s (copy %d)%s", stem, i, ext);
        path = g_build_filename(dir, n, NULL);
        g_free(n);
        if (path_exists(path)) { g_free(path); path = NULL; }
    }
    g_free(stem);
    return path;
}

static void set_cut_paths(GPtrArray *paths)
{
    if (!cut_paths) cut_paths = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    g_hash_table_remove_all(cut_paths);
    for (guint i = 0; paths && i < paths->len; i++) g_hash_table_add(cut_paths, g_strdup(g_ptr_array_index(paths, i)));
    GList *children = fixed ? gtk_container_get_children(GTK_CONTAINER(fixed)) : NULL;
    for (GList *l = children; l; l = l->next) {
        const char *p = g_object_get_data(G_OBJECT(l->data), "target");
        gtk_widget_set_opacity(l->data, p && g_hash_table_contains(cut_paths, p) ? 0.5 : 1.0);
    }
    g_list_free(children);
}

/* Cut / Copy: the format of GNOME / Xfce / MATE file managers, plus a URI list and plain paths for other apps. */
typedef struct { gboolean cut; char **uris; } ClipFiles;
enum { CLIP_GNOME, CLIP_URIS, CLIP_TEXT };

static void clip_files_get(GtkClipboard *cb, GtkSelectionData *sd, guint info, gpointer data)
{
    (void)cb;
    ClipFiles *cf = data;
    GString *s = g_string_new(NULL);
    if (info == CLIP_GNOME) {
        g_string_append(s, cf->cut ? "cut" : "copy");
        for (int i = 0; cf->uris[i]; i++) g_string_append_printf(s, "\n%s", cf->uris[i]);
        gtk_selection_data_set(sd, gtk_selection_data_get_target(sd), 8, (const guchar *)s->str, (gint)s->len);
    } else if (info == CLIP_URIS) {
        gtk_selection_data_set_uris(sd, cf->uris);
    } else {
        for (int i = 0; cf->uris[i]; i++) {
            char *p = g_filename_from_uri(cf->uris[i], NULL, NULL);
            if (i) g_string_append_c(s, '\n');
            g_string_append(s, p ? p : cf->uris[i]);
            g_free(p);
        }
        gtk_selection_data_set_text(sd, s->str, -1);
    }
    g_string_free(s, TRUE);
}

static void clip_files_clear(GtkClipboard *cb, gpointer data)
{
    (void)cb;
    ClipFiles *cf = data;
    if (cf->cut) set_cut_paths(NULL);                          /* another copy replaced ours: un-fade */
    g_strfreev(cf->uris);
    g_free(cf);
}

static void clipboard_set_files(GPtrArray *paths, gboolean cut)
{
    static GtkTargetEntry targets[] = {
        { "x-special/gnome-copied-files", 0, CLIP_GNOME },
        { "text/uri-list", 0, CLIP_URIS },
        { "UTF8_STRING", 0, CLIP_TEXT },
        { "text/plain;charset=utf-8", 0, CLIP_TEXT },
        { "text/plain", 0, CLIP_TEXT },
        { "STRING", 0, CLIP_TEXT },
    };
    if (!paths || !paths->len) return;
    ClipFiles *cf = g_new0(ClipFiles, 1);
    cf->cut = cut;
    cf->uris = g_new0(char *, paths->len + 1);
    for (guint i = 0; i < paths->len; i++) cf->uris[i] = g_filename_to_uri(g_ptr_array_index(paths, i), NULL, NULL);
    GtkClipboard *cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    if (!gtk_clipboard_set_with_data(cb, targets, G_N_ELEMENTS(targets), clip_files_get, clip_files_clear, cf)) {
        g_strfreev(cf->uris);
        g_free(cf);
        return;
    }
    set_cut_paths(cut ? paths : NULL);
    DBG("%s %u item(s) to the clipboard", cut ? "cut" : "copied", paths->len);
}

/* Paste files from the clipboard onto the desktop: copies (folders included), or moves after Cut. A name that
 * is already taken becomes "name (copy)". The pasted items are selected afterwards. */
static void paste_files(void)
{
    const char *ddir = g_get_user_special_dir(G_USER_DIRECTORY_DESKTOP);
    if (!ddir) return;
    GtkClipboard *cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    gboolean cut = FALSE;
    gchar **uris = NULL;
    GtkSelectionData *sd = gtk_clipboard_wait_for_contents(cb, gdk_atom_intern_static_string("x-special/gnome-copied-files"));
    if (sd) {
        const guchar *data = gtk_selection_data_get_data(sd);
        gint len = gtk_selection_data_get_length(sd);
        if (data && len > 0) {
            char *txt = g_strndup((const char *)data, len);
            char **lines = g_strsplit(txt, "\n", -1);
            if (lines[0]) {
                cut = !strcmp(g_strstrip(lines[0]), "cut");
                GPtrArray *a = g_ptr_array_new();
                for (int i = 1; lines[i]; i++) {
                    char *l = g_strstrip(lines[i]);
                    if (*l) g_ptr_array_add(a, g_strdup(l));
                }
                g_ptr_array_add(a, NULL);
                uris = (gchar **)g_ptr_array_free(a, FALSE);
            }
            g_strfreev(lines);
            g_free(txt);
        }
        gtk_selection_data_free(sd);
    }
    if (!uris || !uris[0]) {
        g_strfreev(uris);
        uris = gtk_clipboard_wait_for_uris(cb);
        cut = FALSE;
    }
    if (!uris) return;

    if (!reselect_paths) reselect_paths = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_set_size(reselect_paths, 0);
    GFile *dest_dir = g_file_new_for_path(ddir);
    GString *errors = g_string_new(NULL);
    guint done = 0;
    for (gint i = 0; uris[i]; i++) {
        GFile *src = g_file_new_for_uri(uris[i]);
        char *name = g_file_get_basename(src);
        GFile *parent = g_file_get_parent(src);
        char *src_path = g_file_get_path(src);
        if (!name || !parent || !strcmp(name, "/")) goto next;
        if (g_file_equal(src, dest_dir) || g_file_has_prefix(dest_dir, src)) {
            g_string_append_printf(errors, "%s: a folder cannot be pasted into itself\n", name);
            goto next;
        }
        gboolean same_dir = g_file_equal(parent, dest_dir);
        if (cut && same_dir) {                                 /* cut + paste in the same place: nothing to do */
            if (src_path) g_ptr_array_add(reselect_paths, g_strdup(src_path));
            done++;
            goto next;
        }
        gboolean folder = g_file_query_file_type(src, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL) == G_FILE_TYPE_DIRECTORY;
        char *dst_path = unique_path(ddir, name, folder);
        if (!dst_path) goto next;
        GFile *dst = g_file_new_for_path(dst_path);
        GError *e = NULL;
        gboolean ok;
        if (cut) {
            ok = g_file_move(src, dst, G_FILE_COPY_NOFOLLOW_SYMLINKS | G_FILE_COPY_ALL_METADATA, NULL, NULL, NULL, &e);
            if (!ok && g_error_matches(e, G_IO_ERROR, G_IO_ERROR_WOULD_RECURSE)) {   /* folder on another disk */
                g_clear_error(&e);
                ok = copy_recursive(src, dst, &e) && delete_recursive(src, &e);
            }
        } else {
            ok = copy_recursive(src, dst, &e);
        }
        if (ok) {
            g_ptr_array_add(reselect_paths, g_strdup(dst_path));
            done++;
        } else {
            g_string_append_printf(errors, "%s: %s\n", name, e ? e->message : "failed");
        }
        g_clear_error(&e);
        g_object_unref(dst);
        g_free(dst_path);
next:
        g_free(src_path);
        g_clear_object(&parent);
        g_free(name);
        g_object_unref(src);
    }
    DBG("pasted %u item(s) (%s)", done, cut ? "move" : "copy");
    if (cut && done) {
        /* our own Cut: the files have moved, so the clipboard must not paste them a second time */
        if (cut_paths && g_hash_table_size(cut_paths)) gtk_clipboard_clear(cb);
        set_cut_paths(NULL);
    }
    g_object_unref(dest_dir);
    g_strfreev(uris);
    if (errors->len) message(GTK_MESSAGE_ERROR, "Some items could not be pasted", errors->str);
    g_string_free(errors, TRUE);
    queue_reload();
}

static void m_paste(GtkMenuItem *i, gpointer d)
{
    (void)i; (void)d;
    paste_files();
}

static void rename_path(const char *path)
{
    gboolean launcher = is_launcher(path);
    GKeyFile *kf = NULL;
    char *old_name = NULL;
    if (launcher) {
        kf = g_key_file_new();
        if (g_key_file_load_from_file(kf, path, G_KEY_FILE_KEEP_COMMENTS | G_KEY_FILE_KEEP_TRANSLATIONS, NULL))
            old_name = g_key_file_get_locale_string(kf, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_NAME, NULL, NULL);
        if (!old_name) { g_key_file_free(kf); kf = NULL; launcher = FALSE; }
    }
    if (!old_name) old_name = g_filename_display_basename(path);

    GtkWidget *dlg = gtk_dialog_new_with_buttons("Rename", NULL, GTK_DIALOG_MODAL,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Rename", GTK_RESPONSE_ACCEPT, NULL);
    gtk_window_set_position(GTK_WINDOW(dlg), GTK_WIN_POS_CENTER);
    gtk_window_set_default_size(GTK_WINDOW(dlg), 360, -1);
    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    gtk_container_set_border_width(GTK_CONTAINER(content), 12);
    gtk_box_set_spacing(GTK_BOX(content), 8);
    GtkWidget *lbl = gtk_label_new(launcher ? "New name of the launcher:" : is_dir(path) ? "New folder name:" : "New file name:");
    gtk_label_set_xalign(GTK_LABEL(lbl), 0);
    gtk_box_pack_start(GTK_BOX(content), lbl, FALSE, FALSE, 0);
    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(entry), old_name);
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    gtk_box_pack_start(GTK_BOX(content), entry, FALSE, FALSE, 0);
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_ACCEPT);
    gtk_widget_show_all(dlg);
    /* like file managers: the name is selected without its extension */
    const char *dot = launcher || is_dir(path) ? NULL : strrchr(old_name, '.');
    gtk_widget_grab_focus(entry);
    gtk_editable_select_region(GTK_EDITABLE(entry), 0, dot && dot != old_name ? (gint)g_utf8_pointer_to_offset(old_name, dot) : -1);

    while (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char *name = g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(entry))));
        if (!*name || !strcmp(name, old_name)) { g_free(name); break; }
        if (launcher) {
            /* a launcher shows its Name=, not its file name: change that (translations would hide the new name) */
            gchar **keys = g_key_file_get_keys(kf, G_KEY_FILE_DESKTOP_GROUP, NULL, NULL);
            for (int i = 0; keys && keys[i]; i++)
                if (g_str_has_prefix(keys[i], "Name[")) g_key_file_remove_key(kf, G_KEY_FILE_DESKTOP_GROUP, keys[i], NULL);
            g_strfreev(keys);
            g_key_file_set_string(kf, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_NAME, name);
            GError *e = NULL;
            gboolean ok = g_key_file_save_to_file(kf, path, &e);
            if (!ok) message(GTK_MESSAGE_ERROR, "Could not rename the launcher", e ? e->message : NULL);
            g_clear_error(&e);
            g_free(name);
            if (ok) break;
            continue;
        }
        if (strchr(name, '/')) {
            message(GTK_MESSAGE_ERROR, "A name cannot contain “/”", "Please choose another name.");
            g_free(name);
            continue;
        }
        GFile *f = g_file_new_for_path(path);
        GError *e = NULL;
        GFile *nf = g_file_set_display_name(f, name, NULL, &e);
        g_object_unref(f);
        if (!nf) {
            message(GTK_MESSAGE_ERROR, "Could not rename", e ? e->message : NULL);
            g_clear_error(&e);
            g_free(name);
            continue;                                           /* let the user pick another name */
        }
        char *np = g_file_get_path(nf);
        g_object_unref(nf);
        if (np) {
            move_icon_position(path, np);
            if (!reselect_paths) reselect_paths = g_ptr_array_new_with_free_func(g_free);
            g_ptr_array_set_size(reselect_paths, 0);
            g_ptr_array_add(reselect_paths, np);
            DBG("renamed %s -> %s", path, np);
        }
        g_free(name);
        break;
    }
    gtk_widget_destroy(dlg);
    if (kf) g_key_file_free(kf);
    g_free(old_name);
    queue_reload();
}

static void delete_paths(GPtrArray *paths, gboolean ask)
{
    if (!paths || !paths->len) return;
    if (ask) {
        char *base = g_filename_display_basename(g_ptr_array_index(paths, 0));
        char *q = paths->len == 1 ? g_strdup_printf("Permanently delete “%s”?", base)
                                  : g_strdup_printf("Permanently delete %u items?", paths->len);
        gboolean yes = confirm(q, "Deleted items are not moved to the Trash and cannot be restored.", "_Delete");
        g_free(q);
        g_free(base);
        if (!yes) return;
    }
    GString *errors = g_string_new(NULL);
    for (guint i = 0; i < paths->len; i++) {
        const char *p = g_ptr_array_index(paths, i);
        GFile *f = g_file_new_for_path(p);
        GError *e = NULL;
        if (delete_recursive(f, &e)) move_icon_position(p, NULL);
        else g_string_append_printf(errors, "%s: %s\n", p, e ? e->message : "failed");
        g_clear_error(&e);
        g_object_unref(f);
    }
    if (errors->len) message(GTK_MESSAGE_ERROR, "Some items could not be deleted", errors->str);
    g_string_free(errors, TRUE);
    queue_reload();
}

static void trash_paths(GPtrArray *paths)
{
    if (!paths || !paths->len) return;
    GPtrArray *no_trash = g_ptr_array_new_with_free_func(g_free);
    GString *errors = g_string_new(NULL);
    guint trashed = 0;
    for (guint i = 0; i < paths->len; i++) {
        const char *p = g_ptr_array_index(paths, i);
        GFile *f = g_file_new_for_path(p);
        GError *e = NULL;
        if (g_file_trash(f, NULL, &e)) { move_icon_position(p, NULL); trashed++; }
        else if (g_error_matches(e, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED)) g_ptr_array_add(no_trash, g_strdup(p));
        else g_string_append_printf(errors, "%s: %s\n", p, e ? e->message : "failed");
        g_clear_error(&e);
        g_object_unref(f);
    }
    DBG("moved %u item(s) to the Trash", trashed);
    if (errors->len) message(GTK_MESSAGE_ERROR, "Some items could not be moved to the Trash", errors->str);
    if (no_trash->len &&
        confirm(no_trash->len == 1 ? "This item cannot be moved to the Trash. Delete it permanently?"
                                   : "These items cannot be moved to the Trash. Delete them permanently?",
                "There is no Trash on this disk.", "_Delete"))
        delete_paths(no_trash, FALSE);
    g_ptr_array_unref(no_trash);
    g_string_free(errors, TRUE);
    queue_reload();
}

static void open_paths(GPtrArray *paths)
{
    for (guint i = 0; paths && i < paths->len; i++) activate_target(g_ptr_array_index(paths, i));
}

static void launch_app_for_paths(GAppInfo *app, GPtrArray *paths)
{
    GList *files = NULL;
    for (guint i = paths->len; i-- > 0;) files = g_list_prepend(files, g_file_new_for_path(g_ptr_array_index(paths, i)));
    GdkAppLaunchContext *ctx = gdk_display_get_app_launch_context(gdk_display_get_default());
    GError *e = NULL;
    if (!g_app_info_launch(app, files, G_APP_LAUNCH_CONTEXT(ctx), &e)) {
        char *msg = g_strdup_printf("Could not open with %s", g_app_info_get_display_name(app));
        message(GTK_MESSAGE_ERROR, msg, e ? e->message : NULL);
        g_free(msg);
    }
    g_clear_error(&e);
    g_object_unref(ctx);
    g_list_free_full(files, g_object_unref);
}

static void add_size(GFile *f, guint64 *bytes, guint *count, int depth)
{
    GFileInfo *fi = g_file_query_info(f, G_FILE_ATTRIBUTE_STANDARD_TYPE "," G_FILE_ATTRIBUTE_STANDARD_SIZE,
                                      G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, NULL);
    if (!fi) return;
    if (g_file_info_get_file_type(fi) == G_FILE_TYPE_DIRECTORY && depth < 32 && *count < 20000) {
        GFileEnumerator *en = g_file_enumerate_children(f, G_FILE_ATTRIBUTE_STANDARD_NAME, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, NULL);
        GFileInfo *ci;
        while (en && *count < 20000 && (ci = g_file_enumerator_next_file(en, NULL, NULL))) {
            GFile *c = g_file_get_child(f, g_file_info_get_name(ci));
            (*count)++;
            add_size(c, bytes, count, depth + 1);
            g_object_unref(c);
            g_object_unref(ci);
        }
        g_clear_object(&en);
    } else {
        *bytes += g_file_info_get_size(fi);
    }
    g_object_unref(fi);
}

static void prop_row(GtkWidget *grid, int row, const char *key, const char *value)
{
    if (!value || !*value) return;
    GtkWidget *k = gtk_label_new(key);
    gtk_label_set_xalign(GTK_LABEL(k), 1);
    gtk_style_context_add_class(gtk_widget_get_style_context(k), "dim-label");
    GtkWidget *v = gtk_label_new(value);
    gtk_label_set_xalign(GTK_LABEL(v), 0);
    gtk_label_set_selectable(GTK_LABEL(v), TRUE);
    gtk_label_set_line_wrap(GTK_LABEL(v), TRUE);
    gtk_label_set_line_wrap_mode(GTK_LABEL(v), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_width_chars(GTK_LABEL(v), 34);          /* without a minimum width the window gets very tall */
    gtk_label_set_max_width_chars(GTK_LABEL(v), 46);
    gtk_widget_set_hexpand(v, TRUE);
    gtk_grid_attach(GTK_GRID(grid), k, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), v, 1, row, 1, 1);
}

/* Properties window (not modal: several can stay open). */
static void show_properties(GPtrArray *paths)
{
    if (!paths || !paths->len) return;
    const char *first = g_ptr_array_index(paths, 0);
    char *base = paths->len == 1 ? g_filename_display_basename(first) : NULL;
    char *title = base ? g_strdup_printf("%s — Properties", base) : g_strdup_printf("%u Items — Properties", paths->len);
    GtkWidget *dlg = gtk_dialog_new_with_buttons(title, NULL, 0, "_Close", GTK_RESPONSE_CLOSE, NULL);
    g_free(title);
    gtk_window_set_position(GTK_WINDOW(dlg), GTK_WIN_POS_CENTER);
    gtk_window_set_resizable(GTK_WINDOW(dlg), FALSE);     /* exactly as big as its contents */
    g_signal_connect(dlg, "response", G_CALLBACK(gtk_widget_destroy), NULL);
    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    gtk_container_set_border_width(GTK_CONTAINER(content), 14);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 14);
    gtk_box_pack_start(GTK_BOX(content), grid, TRUE, TRUE, 0);
    int row = 0;

    if (paths->len == 1) {
        GFile *f = g_file_new_for_path(first);
        GFileInfo *fi = g_file_query_info(f, "standard::*,time::modified,access::*,unix::mode", G_FILE_QUERY_INFO_NONE, NULL, NULL);
        gboolean launcher = is_launcher(first);
        GDesktopAppInfo *app = launcher ? g_desktop_app_info_new_from_filename(first) : NULL;
        char *link_name = NULL, *link_icon = NULL;
        char *link_url = launcher && !app ? desktop_link_url(first, &link_name, &link_icon) : NULL;
        GIcon *icon = app && g_app_info_get_icon(G_APP_INFO(app)) ? g_app_info_get_icon(G_APP_INFO(app))
                      : fi ? g_file_info_get_icon(fi) : NULL;
        GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        GtkWidget *img = link_icon && *link_icon && !g_path_is_absolute(link_icon)
                         ? gtk_image_new_from_icon_name(link_icon, GTK_ICON_SIZE_DIALOG)
                         : icon ? gtk_image_new_from_gicon(icon, GTK_ICON_SIZE_DIALOG)
                                : gtk_image_new_from_icon_name("text-x-generic", GTK_ICON_SIZE_DIALOG);
        gtk_image_set_pixel_size(GTK_IMAGE(img), 48);
        GtkWidget *name = gtk_label_new(NULL);
        char *m = g_markup_printf_escaped("<b><big>%s</big></b>", app ? g_app_info_get_display_name(G_APP_INFO(app))
                                                               : link_name && *link_name ? link_name : base);
        gtk_label_set_markup(GTK_LABEL(name), m);
        g_free(m);
        gtk_label_set_selectable(GTK_LABEL(name), TRUE);
        gtk_label_set_line_wrap(GTK_LABEL(name), TRUE);
        gtk_label_set_line_wrap_mode(GTK_LABEL(name), PANGO_WRAP_WORD_CHAR);
        gtk_label_set_width_chars(GTK_LABEL(name), 24);
        gtk_label_set_max_width_chars(GTK_LABEL(name), 40);
        gtk_label_set_xalign(GTK_LABEL(name), 0);
        gtk_box_pack_start(GTK_BOX(head), img, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(head), name, TRUE, TRUE, 0);
        gtk_grid_attach(GTK_GRID(grid), head, 0, row++, 2, 1);
        gtk_grid_attach(GTK_GRID(grid), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), 0, row++, 2, 1);

        if (fi) {
            const char *ct = g_file_info_get_content_type(fi);
            char *desc = ct ? g_content_type_get_description(ct) : NULL;
            char *type = desc ? g_strdup_printf("%s (%s)", desc, ct) : g_strdup(ct);
            prop_row(grid, row++, "Type:", type);
            g_free(type);
            g_free(desc);
        }
        char *dir = g_path_get_dirname(first);
        prop_row(grid, row++, "Location:", dir);
        g_free(dir);
        if (fi && g_file_info_get_is_symlink(fi)) prop_row(grid, row++, "Link to:", g_file_info_get_symlink_target(fi));
        if (fi) {
            guint64 bytes = 0;
            guint count = 0;
            char *sz;
            if (g_file_info_get_file_type(fi) == G_FILE_TYPE_DIRECTORY) {
                add_size(f, &bytes, &count, 0);
                char *h = g_format_size(bytes);
                sz = g_strdup_printf("%u item%s, %s%s", count, count == 1 ? "" : "s", h, count >= 20000 ? " (or more)" : "");
                g_free(h);
            } else {
                char *h = g_format_size_full(g_file_info_get_size(fi), G_FORMAT_SIZE_LONG_FORMAT);
                sz = g_strdup(h);
                g_free(h);
            }
            prop_row(grid, row++, launcher ? "File size:" : "Size:", sz);
            g_free(sz);
            GDateTime *dt = g_file_info_get_modification_date_time(fi);
            if (dt) {
                GDateTime *local = g_date_time_to_local(dt);
                char *ds = g_date_time_format(local, "%A, %d %B %Y, %H:%M");
                prop_row(grid, row++, "Modified:", ds);
                g_free(ds);
                g_date_time_unref(local);
                g_date_time_unref(dt);
            }
            gboolean r = !g_file_info_has_attribute(fi, G_FILE_ATTRIBUTE_ACCESS_CAN_READ) || g_file_info_get_attribute_boolean(fi, G_FILE_ATTRIBUTE_ACCESS_CAN_READ);
            gboolean w = !g_file_info_has_attribute(fi, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE) || g_file_info_get_attribute_boolean(fi, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE);
            char mode[16] = "";
            if (g_file_info_has_attribute(fi, G_FILE_ATTRIBUTE_UNIX_MODE))
                g_snprintf(mode, sizeof mode, " (%03o)", g_file_info_get_attribute_uint32(fi, G_FILE_ATTRIBUTE_UNIX_MODE) & 0777);
            char *acc = g_strdup_printf("%s%s", r && w ? "Read and write" : r ? "Read only" : "No access", mode);
            prop_row(grid, row++, "Access:", acc);
            g_free(acc);
        }
        if (app) {
            prop_row(grid, row++, "Command:", g_app_info_get_commandline(G_APP_INFO(app)));
            prop_row(grid, row++, "Comment:", g_app_info_get_description(G_APP_INFO(app)));
        }
        prop_row(grid, row++, "Address:", link_url);
        g_free(link_url);
        g_free(link_name);
        g_free(link_icon);
        g_clear_object(&app);
        g_clear_object(&fi);
        g_object_unref(f);
    } else {
        guint files = 0, folders = 0, count = 0;
        guint64 bytes = 0;
        for (guint i = 0; i < paths->len; i++) {
            GFile *f = g_file_new_for_path(g_ptr_array_index(paths, i));
            if (g_file_query_file_type(f, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL) == G_FILE_TYPE_DIRECTORY) folders++;
            else files++;
            add_size(f, &bytes, &count, 0);
            g_object_unref(f);
        }
        GtkWidget *name = gtk_label_new(NULL);
        char *m = g_markup_printf_escaped("<b><big>%u items selected</big></b>", paths->len);
        gtk_label_set_markup(GTK_LABEL(name), m);
        g_free(m);
        gtk_label_set_xalign(GTK_LABEL(name), 0);
        gtk_grid_attach(GTK_GRID(grid), name, 0, row++, 2, 1);
        char *what = g_strdup_printf("%u file%s, %u folder%s", files, files == 1 ? "" : "s", folders, folders == 1 ? "" : "s");
        prop_row(grid, row++, "Contains:", what);
        g_free(what);
        char *h = g_format_size(bytes);
        prop_row(grid, row++, "Total size:", h);
        g_free(h);
        char *dir = g_path_get_dirname(first);
        prop_row(grid, row++, "Location:", dir);
        g_free(dir);
    }
    g_free(base);
    gtk_widget_show_all(dlg);
    gtk_window_present(GTK_WINDOW(dlg));
}

/* ---------- icon context menu ---------- */
static GPtrArray *item_paths(GtkMenuItem *i) { return g_object_get_data(G_OBJECT(i), "paths"); }
static void m_open(GtkMenuItem *i, gpointer d) { (void)d; open_paths(item_paths(i)); }
static void m_cut(GtkMenuItem *i, gpointer d) { (void)d; clipboard_set_files(item_paths(i), TRUE); }
static void m_copy(GtkMenuItem *i, gpointer d) { (void)d; clipboard_set_files(item_paths(i), FALSE); }
static void m_trash(GtkMenuItem *i, gpointer d) { (void)d; trash_paths(item_paths(i)); }
static void m_properties(GtkMenuItem *i, gpointer d) { (void)d; show_properties(item_paths(i)); }

static void m_rename(GtkMenuItem *i, gpointer d)
{
    (void)d;
    GPtrArray *p = item_paths(i);
    if (p && p->len == 1) rename_path(g_ptr_array_index(p, 0));
}

static void m_open_terminal_here(GtkMenuItem *i, gpointer d)
{
    (void)d;
    GPtrArray *p = item_paths(i);
    if (p && p->len) open_terminal_in(g_ptr_array_index(p, 0));
}

static void m_open_with_app(GtkMenuItem *i, gpointer d)
{
    (void)d;
    GAppInfo *app = g_object_get_data(G_OBJECT(i), "app");
    GPtrArray *p = item_paths(i);
    if (app && p) launch_app_for_paths(app, p);
}

static void m_open_with_other(GtkMenuItem *i, gpointer d)
{
    (void)d;
    GPtrArray *p = item_paths(i);
    if (!p || !p->len) return;
    GFile *f = g_file_new_for_path(g_ptr_array_index(p, 0));
    GtkWidget *dlg = gtk_app_chooser_dialog_new(NULL, GTK_DIALOG_MODAL, f);
    gtk_window_set_title(GTK_WINDOW(dlg), "Open With");
    gtk_app_chooser_widget_set_show_all(GTK_APP_CHOOSER_WIDGET(gtk_app_chooser_dialog_get_widget(GTK_APP_CHOOSER_DIALOG(dlg))), TRUE);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK) {
        GAppInfo *app = gtk_app_chooser_get_app_info(GTK_APP_CHOOSER(dlg));
        if (app) {
            launch_app_for_paths(app, p);
            g_object_unref(app);
        }
    }
    gtk_widget_destroy(dlg);
    g_object_unref(f);
}

static void set_item_paths(GtkWidget *item, GPtrArray *paths)
{
    if (paths) g_object_set_data_full(G_OBJECT(item), "paths", g_ptr_array_ref(paths), (GDestroyNotify)g_ptr_array_unref);
}

/* Menu item with a mnemonic and, as a hint, the keyboard shortcut that does the same on the desktop. */
static GtkWidget *menu_add(GtkWidget *menu, const char *mnemonic, guint key, GdkModifierType mods, GCallback cb, GPtrArray *paths)
{
    GtkWidget *it = gtk_menu_item_new_with_mnemonic(mnemonic);
    GtkWidget *child = gtk_bin_get_child(GTK_BIN(it));
    if (key && GTK_IS_ACCEL_LABEL(child)) gtk_accel_label_set_accel(GTK_ACCEL_LABEL(child), key, mods);
    set_item_paths(it, paths);
    if (cb) g_signal_connect(it, "activate", cb, NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), it);
    return it;
}

static GtkWidget *app_item(GAppInfo *app, GPtrArray *paths, gboolean is_default)
{
    GtkWidget *it = gtk_menu_item_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GIcon *ic = g_app_info_get_icon(app);
    GtkWidget *img = ic ? gtk_image_new_from_gicon(ic, GTK_ICON_SIZE_MENU) : gtk_image_new_from_icon_name("application-x-executable", GTK_ICON_SIZE_MENU);
    gtk_image_set_pixel_size(GTK_IMAGE(img), 16);
    char *text = is_default ? g_strdup_printf("%s (default)", g_app_info_get_display_name(app)) : g_strdup(g_app_info_get_display_name(app));
    GtkWidget *lbl = gtk_label_new(text);
    g_free(text);
    gtk_box_pack_start(GTK_BOX(box), img, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), lbl, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(it), box);
    g_object_set_data_full(G_OBJECT(it), "app", g_object_ref(app), g_object_unref);
    set_item_paths(it, paths);
    g_signal_connect(it, "activate", G_CALLBACK(m_open_with_app), NULL);
    return it;
}

/* "Open With" submenu: the default application first, then the other applications registered for this type. */
static GtkWidget *open_with_menu(GPtrArray *paths, const char *ctype)
{
    GtkWidget *sub = gtk_menu_new();
    GAppInfo *def = ctype ? g_app_info_get_default_for_type(ctype, FALSE) : NULL;
    GList *apps = ctype ? g_app_info_get_all_for_type(ctype) : NULL;
    int n = 0;
    if (def) { gtk_menu_shell_append(GTK_MENU_SHELL(sub), app_item(def, paths, TRUE)); n++; }
    for (GList *l = apps; l && n < 16; l = l->next) {
        if (def && g_app_info_equal(l->data, def)) continue;
        gtk_menu_shell_append(GTK_MENU_SHELL(sub), app_item(l->data, paths, FALSE));
        n++;
    }
    if (n) gtk_menu_shell_append(GTK_MENU_SHELL(sub), gtk_separator_menu_item_new());
    menu_add(sub, "_Other Application…", 0, 0, G_CALLBACK(m_open_with_other), paths);
    g_list_free_full(apps, g_object_unref);
    g_clear_object(&def);
    return sub;
}

static void log_menu(const char *what, GtkWidget *menu)
{
    if (!debug_on) return;
    GString *s = g_string_new(NULL);
    GList *items = gtk_container_get_children(GTK_CONTAINER(menu));
    for (GList *l = items; l; l = l->next) {
        if (GTK_IS_SEPARATOR_MENU_ITEM(l->data)) continue;
        const char *label = gtk_menu_item_get_label(GTK_MENU_ITEM(l->data));
        if (!label) continue;
        if (s->len) g_string_append(s, " | ");
        gboolean tag = FALSE;                                   /* drop mnemonics and markup */
        for (const char *c = label; *c; c++) {
            if (*c == '<') tag = TRUE;
            else if (*c == '>') tag = FALSE;
            else if (!tag && *c != '_') g_string_append_c(s, *c);
        }
    }
    g_list_free(items);
    DBG("%s: %s", what, s->str);
    g_string_free(s, TRUE);
}

/* The menu of a desktop icon (or of all selected icons). The paths are copied into the menu items, so the
 * actions still work when the desktop reloads its icons while the menu or a dialog is open. */
static void popup_icon_menu(GtkWidget *icon, const GdkEvent *trigger)
{
    GList *sel = selected_icons();
    if (!sel) sel = g_list_append(NULL, icon);
    GPtrArray *all = g_ptr_array_new_with_free_func(g_free);     /* every selected item */
    GPtrArray *ops = g_ptr_array_new_with_free_func(g_free);     /* without Home: Cut / Copy / Rename / Trash */
    for (GList *l = sel; l; l = l->next) {
        const char *p = g_object_get_data(G_OBJECT(l->data), "target");
        if (!p) continue;
        g_ptr_array_add(all, g_strdup(p));
        if (!g_object_get_data(G_OBJECT(l->data), "home")) g_ptr_array_add(ops, g_strdup(p));
    }
    g_list_free(sel);
    if (!all->len) { g_ptr_array_unref(all); g_ptr_array_unref(ops); return; }
    const char *first = g_ptr_array_index(all, 0);
    gboolean single = all->len == 1, folder = single && is_dir(first), launcher = single && is_launcher(first);

    GtkWidget *menu = gtk_menu_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(menu), "hde-icon-menu");
    GtkWidget *it = menu_add(menu, "_Open", GDK_KEY_Return, 0, G_CALLBACK(m_open), all);
    gtk_label_set_markup_with_mnemonic(GTK_LABEL(gtk_bin_get_child(GTK_BIN(it))), "<b>_Open</b>");
    if (single && !launcher) {
        GFile *f = g_file_new_for_path(first);
        GFileInfo *fi = g_file_query_info(f, G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE, G_FILE_QUERY_INFO_NONE, NULL, NULL);
        const char *ct = fi ? g_file_info_get_content_type(fi) : NULL;
        GtkWidget *ow = gtk_menu_item_new_with_mnemonic("Open _With");
        gtk_menu_item_set_submenu(GTK_MENU_ITEM(ow), open_with_menu(all, ct ? ct : (folder ? "inode/directory" : "application/octet-stream")));
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), ow);
        g_clear_object(&fi);
        g_object_unref(f);
    }
    if (folder) menu_add(menu, "Open in _Terminal", 0, 0, G_CALLBACK(m_open_terminal_here), all);
    if (ops->len) {
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
        menu_add(menu, "C_ut", GDK_KEY_x, GDK_CONTROL_MASK, G_CALLBACK(m_cut), ops);
        menu_add(menu, "_Copy", GDK_KEY_c, GDK_CONTROL_MASK, G_CALLBACK(m_copy), ops);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
        if (single) menu_add(menu, "_Rename…", GDK_KEY_F2, 0, G_CALLBACK(m_rename), ops);
        menu_add(menu, "_Move to Trash", GDK_KEY_Delete, 0, G_CALLBACK(m_trash), ops);
    }
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    menu_add(menu, "_Properties", GDK_KEY_Return, GDK_MOD1_MASK, G_CALLBACK(m_properties), all);

    char *what = single ? g_strdup_printf("icon menu for %s", first) : g_strdup_printf("icon menu for %u items", all->len);
    log_menu(what, menu);
    g_free(what);
    gtk_widget_show_all(menu);
    g_signal_connect(menu, "selection-done", G_CALLBACK(gtk_widget_destroy), NULL);
    if (trigger && trigger->type == GDK_BUTTON_PRESS)
        gtk_menu_popup_at_pointer(GTK_MENU(menu), trigger);
    else
        gtk_menu_popup_at_widget(GTK_MENU(menu), icon, GDK_GRAVITY_CENTER, GDK_GRAVITY_NORTH_WEST, trigger);
    g_ptr_array_unref(all);
    g_ptr_array_unref(ops);
}

static void m_arrange_icons(GtkMenuItem *i, gpointer d)
{
    (void)i; (void)d;
    keep_arranged = TRUE;
    GKeyFile *kf = g_key_file_new();
    char *cf = config_file();
    g_key_file_load_from_file(kf, cf, G_KEY_FILE_NONE, NULL);
    g_key_file_remove_group(kf, "icon-positions", NULL);
    char *dir = g_path_get_dirname(cf);
    g_mkdir_with_parents(dir, 0755);
    g_key_file_save_to_file(kf, cf, NULL);
    g_free(dir); g_free(cf); g_key_file_free(kf);
    reload_icons();
}

static void set_sort_mode(GtkMenuItem *i, gpointer d)
{
    (void)d;
    sort_mode = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(i), "sort-mode"));
    reload_icons();
}

static void m_keep_arranged(GtkMenuItem *i, gpointer d)
{
    (void)d;
    keep_arranged = gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(i));
    if (keep_arranged) m_arrange_icons(NULL, NULL);
    else reload_icons();
}

static gint cmp_file_paths(gconstpointer a, gconstpointer b)
{
    const char *pa = a, *pb = b;
    if (sort_mode == SORT_NAME_DESC) return -g_utf8_collate(pa, pb);
    if (sort_mode == SORT_TYPE) {
        const char *ea = strrchr(pa, '.');
        const char *eb = strrchr(pb, '.');
        int c = g_ascii_strcasecmp(ea ? ea : "", eb ? eb : "");
        return c ? c : g_utf8_collate(pa, pb);
    }
    if (sort_mode == SORT_MTIME || sort_mode == SORT_SIZE) {
        GStatBuf sa, sb;
        gboolean oka = g_stat(pa, &sa) == 0, okb = g_stat(pb, &sb) == 0;
        if (oka && okb) {
            if (sort_mode == SORT_MTIME) {
                if (sa.st_mtime != sb.st_mtime) return sa.st_mtime > sb.st_mtime ? -1 : 1;
            } else {
                if (sa.st_size != sb.st_size) return sa.st_size > sb.st_size ? -1 : 1;
            }
        }
    }
    return g_utf8_collate(pa, pb);
}

static void on_settings_sort_changed(GtkComboBox *c, gpointer d)
{
    (void)d;
    sort_mode = (SortMode)gtk_combo_box_get_active(c);
    if (keep_arranged) reload_icons();
}

static void m_settings(GtkMenuItem *i, gpointer d)
{
    (void)i; (void)d;
    GtkWidget *dlg = gtk_dialog_new_with_buttons("HDE Desktop Settings", GTK_WINDOW(win),
        GTK_DIALOG_MODAL, "_Close", GTK_RESPONSE_CLOSE, NULL);
    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    gtk_container_set_border_width(GTK_CONTAINER(content), 16);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 12);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 18);
    gtk_box_pack_start(GTK_BOX(content), grid, TRUE, TRUE, 0);

    GtkWidget *title = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(title), "<b>Desktop</b>");
    gtk_widget_set_halign(title, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), title, 0, 0, 2, 1);

    GtkWidget *arr = gtk_check_button_new_with_label("Keep icons arranged");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(arr), keep_arranged);
    g_signal_connect(arr, "toggled", G_CALLBACK(m_keep_arranged), NULL);
    gtk_grid_attach(GTK_GRID(grid), arr, 0, 1, 2, 1);

    GtkWidget *sort_label = gtk_label_new("Default sorting:");
    gtk_widget_set_halign(sort_label, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), sort_label, 0, 2, 1, 1);
    GtkWidget *sort = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(sort), "Name");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(sort), "Name descending");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(sort), "Modified time");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(sort), "Type");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(sort), "Size");
    gtk_combo_box_set_active(GTK_COMBO_BOX(sort), (gint)sort_mode);
    g_signal_connect(sort, "changed", G_CALLBACK(on_settings_sort_changed), NULL);
    gtk_grid_attach(GTK_GRID(grid), sort, 1, 2, 1, 1);

    GtkWidget *wall = gtk_button_new_with_label("Change Background…");
    g_signal_connect(wall, "clicked", G_CALLBACK(m_wallpaper), NULL);
    gtk_grid_attach(GTK_GRID(grid), wall, 0, 3, 2, 1);

    gtk_widget_show_all(dlg);
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
}

static void m_hyggshi_settings(GtkMenuItem *i, gpointer d)
{
    launch_hyggshi_settings(i, d);
}

static void m_display_settings(GtkMenuItem *i, gpointer d)
{
    (void)i; (void)d;
    const char *commands[] = { "gnome-control-center", "xfce4-display-settings",
                              "lxqt-config-monitor", "arandr", NULL };
    for (int k = 0; commands[k]; k++) {
        char *p = g_find_program_in_path(commands[k]);
        if (!p) continue;
        char *cmd = NULL;
        if (!strcmp(commands[k], "gnome-control-center"))
            cmd = g_strdup_printf("%s display", p);
        else
            cmd = g_strdup(p);
        GError *err = NULL;
        if (g_spawn_command_line_async(cmd, &err)) {
            g_free(cmd); g_free(p); g_clear_error(&err); return;
        }
        g_clear_error(&err);
        g_free(cmd); g_free(p);
    }
}

/* "About HDE": the About window (logo of the system, HDE version, credits; "System details…" opens Settings > About) */
static void m_about(GtkMenuItem *i, gpointer d)
{
    (void)i; (void)d;
    char *self = g_file_read_link("/proc/self/exe", NULL);
    char *dir = self ? g_path_get_dirname(self) : NULL;
    char *sib = dir ? g_build_filename(dir, "hde-settings", NULL) : NULL;
    char *prog = sib && g_file_test(sib, G_FILE_TEST_IS_EXECUTABLE) ? g_strdup(sib) : g_find_program_in_path("hde-settings");
    if (prog) {
        char *argv[] = { prog, (char *)"--about-window", NULL };
        GError *e = NULL;
        if (!g_spawn_async(NULL, argv, NULL, 0, NULL, NULL, NULL, &e)) {
            g_printerr("hde-desktop: cannot start %s: %s\n", prog, e ? e->message : "?");
            g_clear_error(&e);
        }
    } else {
        g_printerr("hde-desktop: hde-settings not found\n");
    }
    g_free(prog); g_free(sib); g_free(dir); g_free(self);
}

/* ---------- screens changed (F8, a monitor plugged in or out): cover the whole desktop again ---------- */
static guint screens_id;

/* Wayland: a layer surface covers one screen only; the others get a surface that just shows the wallpaper */
static gboolean extra_draw(GtkWidget *w, cairo_t *cr, gpointer d)
{
    (void)d;
    paint_wallpaper(cr, 0, 0, gtk_widget_get_allocated_width(w), gtk_widget_get_allocated_height(w));
    return TRUE;
}

static void update_extra_wallpapers(GdkMonitor *main_mon)
{
    if (!win || !hde_wl_is_layer(GTK_WINDOW(win))) return;
    if (extra_bg) g_ptr_array_free(extra_bg, TRUE);
    extra_bg = g_ptr_array_new_with_free_func((GDestroyNotify)gtk_widget_destroy);
    GdkDisplay *dpy = gdk_display_get_default();
    for (int i = 0; i < gdk_display_get_n_monitors(dpy); i++) {
        GdkMonitor *m = gdk_display_get_monitor(dpy, i);
        if (m == main_mon) continue;
        GtkWidget *w = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        if (!hde_wl_layer_init(GTK_WINDOW(w), "hde-wallpaper", HDE_LAYER_BACKGROUND, HDE_EDGE_ALL, HDE_KB_NONE)) {
            gtk_widget_destroy(w);
            continue;
        }
        hde_wl_layer_exclusive(GTK_WINDOW(w), -1);
        hde_wl_layer_monitor(GTK_WINDOW(w), m);
        gtk_widget_set_app_paintable(w, TRUE);
        g_signal_connect(w, "draw", G_CALLBACK(extra_draw), NULL);
        gtk_widget_show(w);
        g_ptr_array_add(extra_bg, w);
    }
    DBG("Wayland: wallpaper on %u more screen(s)", extra_bg->len);
}

static void redraw_all(void)
{
    if (win) gtk_widget_queue_draw(win);
    for (guint i = 0; extra_bg && i < extra_bg->len; i++) gtk_widget_queue_draw(extra_bg->pdata[i]);
}

static gboolean desktop_place(gpointer d)
{
    (void)d;
    screens_id = 0;
    if (!win) return G_SOURCE_REMOVE;
    if (hde_wl_is_layer(GTK_WINDOW(win))) {
        GdkMonitor *m = hde_main_monitor();
        if (m) gdk_monitor_get_geometry(m, &mon);
        mon.x = mon.y = 0;                               /* the surface's own coordinates */
        hde_wl_layer_monitor(GTK_WINDOW(win), m);
        gtk_widget_set_size_request(win, mon.width, mon.height);
        update_extra_wallpapers(m);
        reload_icons();
        redraw_all();
        fprintf(stderr, "hde-desktop: screens changed: desktop %dx%d (Wayland layer shell, %d screen(s))\n", mon.width,
                mon.height, gdk_display_get_n_monitors(gdk_display_get_default()));
        return G_SOURCE_REMOVE;
    }
    GdkDisplay *dpy = gdk_display_get_default();
    GdkMonitor *m = gdk_display_get_primary_monitor(dpy);
    if (!m) m = gdk_display_get_monitor(dpy, 0);
    if (m) gdk_monitor_get_geometry(m, &mon);
    GdkScreen *scr = gdk_display_get_default_screen(dpy);
    int sw = gdk_screen_get_width(scr), sh = gdk_screen_get_height(scr);
    gtk_widget_set_size_request(win, sw, sh);
    gtk_window_resize(GTK_WINDOW(win), sw, sh);
    gtk_window_move(GTK_WINDOW(win), 0, 0);
    reload_icons();
    gtk_widget_queue_draw(win);
    fprintf(stderr, "hde-desktop: screens changed: desktop %dx%d, icons on the primary screen %dx%d+%d+%d\n",
            sw, sh, mon.width, mon.height, mon.x, mon.y);
    return G_SOURCE_REMOVE;
}

static void on_screens_changed(GdkScreen *s, gpointer d)
{
    (void)s; (void)d;
    if (screens_id) g_source_remove(screens_id);
    screens_id = g_timeout_add(250, desktop_place, NULL);
}

static GtkWidget *make_sort_item(GtkWidget *submenu, GSList **group, const char *label, SortMode mode)
{
    GtkWidget *it = gtk_radio_menu_item_new_with_label(*group, label);
    *group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(it));
    if ((int)mode == (int)sort_mode) gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(it), TRUE);
    g_object_set_data(G_OBJECT(it), "sort-mode", GINT_TO_POINTER(mode));
    g_signal_connect(it, "activate", G_CALLBACK(set_sort_mode), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(submenu), it);
    return it;
}

static void popup_desktop_menu(const GdkEvent *e)
{
    GtkWidget *menu = gtk_menu_new();
    GtkWidget *item;

    item = gtk_menu_item_new_with_mnemonic("_New Folder");
    g_signal_connect(item, "activate", G_CALLBACK(m_new_folder), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

    item = gtk_menu_item_new_with_mnemonic("_Paste");
    if (GTK_IS_ACCEL_LABEL(gtk_bin_get_child(GTK_BIN(item))))
        gtk_accel_label_set_accel(GTK_ACCEL_LABEL(gtk_bin_get_child(GTK_BIN(item))), GDK_KEY_v, GDK_CONTROL_MASK);
    gtk_widget_set_sensitive(item, clipboard_has_files());
    g_signal_connect(item, "activate", G_CALLBACK(m_paste), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

    item = gtk_menu_item_new_with_mnemonic("Select _All");
    if (GTK_IS_ACCEL_LABEL(gtk_bin_get_child(GTK_BIN(item))))
        gtk_accel_label_set_accel(GTK_ACCEL_LABEL(gtk_bin_get_child(GTK_BIN(item))), GDK_KEY_a, GDK_CONTROL_MASK);
    g_signal_connect(item, "activate", G_CALLBACK(m_select_all), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

    item = gtk_menu_item_new_with_label("Arrange Icons");
    g_signal_connect(item, "activate", G_CALLBACK(m_arrange_icons), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

    GtkWidget *arrange_by = gtk_menu_item_new_with_label("Arrange By…");
    GtkWidget *sort_menu = gtk_menu_new();
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(arrange_by), sort_menu);
    GtkWidget *keep = gtk_check_menu_item_new_with_label("Keep Arranged…");
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(keep), keep_arranged);
    g_signal_connect(keep, "toggled", G_CALLBACK(m_keep_arranged), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(sort_menu), keep);
    gtk_menu_shell_append(GTK_MENU_SHELL(sort_menu), gtk_separator_menu_item_new());
    GSList *sort_group = NULL;
    make_sort_item(sort_menu, &sort_group, "Sort by Name", SORT_NAME);
    make_sort_item(sort_menu, &sort_group, "Sort by Name Descending", SORT_NAME_DESC);
    make_sort_item(sort_menu, &sort_group, "Sort by Modified Time", SORT_MTIME);
    make_sort_item(sort_menu, &sort_group, "Sort by Type", SORT_TYPE);
    make_sort_item(sort_menu, &sort_group, "Sort by Size", SORT_SIZE);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), arrange_by);

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

    item = gtk_menu_item_new_with_label("Show Desktop in Files");
    g_signal_connect(item, "activate", G_CALLBACK(m_show_desktop_files), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

    item = gtk_menu_item_new_with_label("Open in Terminal");
    g_signal_connect(item, "activate", G_CALLBACK(m_terminal), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

    item = gtk_menu_item_new_with_label("Change Background…");
    g_signal_connect(item, "activate", G_CALLBACK(m_wallpaper), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

    item = gtk_menu_item_new_with_label("Desktop Icons Settings");
    g_signal_connect(item, "activate", G_CALLBACK(m_settings), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

    item = gtk_menu_item_new_with_label("Display Settings");
    g_signal_connect(item, "activate", G_CALLBACK(m_display_settings), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

    item = gtk_menu_item_new_with_label("Settings");
    g_signal_connect(item, "activate", G_CALLBACK(m_hyggshi_settings), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

    item = gtk_menu_item_new_with_label("About HDE");
    g_signal_connect(item, "activate", G_CALLBACK(m_about), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

    item = gtk_menu_item_new_with_label("Refresh");
    g_signal_connect(item, "activate", G_CALLBACK(m_refresh), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

    log_menu("desktop menu", menu);
    gtk_widget_show_all(menu);
    g_signal_connect(menu, "selection-done", G_CALLBACK(gtk_widget_destroy), NULL);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), e);
}

static void clear_icon_selection(void)
{
    select_icon(NULL);
}

static gboolean point_in_selection(GtkWidget *w, gdouble sx, gdouble sy, gdouble ex, gdouble ey)
{
    gint x = 0, y = 0;
    gtk_container_child_get(GTK_CONTAINER(fixed), w, "x", &x, "y", &y, NULL);
    gint ww = gtk_widget_get_allocated_width(w), hh = gtk_widget_get_allocated_height(w);
    gdouble l = MIN(sx, ex), r = MAX(sx, ex), t = MIN(sy, ey), b = MAX(sy, ey);
    return (x < r && x + ww > l && y < b && y + hh > t);
}

static gboolean on_fixed_draw(GtkWidget *w, cairo_t *cr, gpointer d)
{
    (void)d;
    if (!selecting) return FALSE;
    gdouble x = MIN(select_start_x, select_cur_x);
    gdouble y = MIN(select_start_y, select_cur_y);
    gdouble ww = fabs(select_cur_x - select_start_x);
    gdouble hh = fabs(select_cur_y - select_start_y);
    cairo_save(cr);
    cairo_set_source_rgba(cr, 0.25, 0.50, 1.0, 0.22);
    cairo_rectangle(cr, x, y, ww, hh);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 0.35, 0.60, 1.0, 0.85);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);
    cairo_restore(cr);
    return FALSE;
}

static gboolean select_additive;            /* Ctrl held when the rubber band started */

static void update_box_selection(void)
{
    GList *children = gtk_container_get_children(GTK_CONTAINER(fixed));
    for (GList *l = children; l; l = l->next) {
        GtkWidget *child = GTK_WIDGET(l->data);
        gboolean in = point_in_selection(child, select_start_x, select_start_y, select_cur_x, select_cur_y);
        gboolean kept = select_additive && g_object_get_data(G_OBJECT(child), "kept");
        icon_set_selected(child, in || kept);
    }
    g_list_free(children);
}

/* Rubber-band selection. GtkFixed has no window of its own, so these run on the desktop window (on_bg_press,
 * motion and release handlers of win); the coordinates are the same because the GtkFixed fills the window. */
static gboolean on_fixed_press(GtkWidget *w, GdkEventButton *e, gpointer d)
{
    (void)w; (void)d;
    if (e->button != 1 || e->type != GDK_BUTTON_PRESS) return FALSE;
    select_additive = (e->state & GDK_CONTROL_MASK) != 0;          /* Ctrl+drag adds to the selection */
    if (!select_additive) clear_icon_selection();
    GList *children = gtk_container_get_children(GTK_CONTAINER(fixed));
    for (GList *l = children; l; l = l->next)
        g_object_set_data(G_OBJECT(l->data), "kept", GINT_TO_POINTER(icon_is_selected(l->data)));
    g_list_free(children);
    selecting = TRUE;
    select_start_x = select_cur_x = e->x;
    select_start_y = select_cur_y = e->y;
    gtk_widget_queue_draw(fixed);
    return TRUE;
}

static gboolean on_fixed_motion(GtkWidget *w, GdkEventMotion *e, gpointer d)
{
    (void)w; (void)d;
    if (!selecting || !(e->state & GDK_BUTTON1_MASK)) return FALSE;
    select_cur_x = e->x;
    select_cur_y = e->y;
    update_box_selection();
    gtk_widget_queue_draw(fixed);
    return TRUE;
}

static gboolean on_fixed_release(GtkWidget *w, GdkEventButton *e, gpointer d)
{
    (void)w; (void)d;
    if (e->button != 1 || !selecting) return FALSE;
    select_cur_x = e->x;
    select_cur_y = e->y;
    update_box_selection();
    selecting = FALSE;
    gtk_widget_queue_draw(fixed);
    return TRUE;
}

static gboolean on_bg_press(GtkWidget *w, GdkEventButton *e, gpointer d)
{
    (void)w; (void)d;
    if (e->type != GDK_BUTTON_PRESS) return FALSE;
    if (e->button == 1) return on_fixed_press(w, e, d);       /* empty desktop: clear + start a rubber band */
    if (e->button == 3) {                       /* empty desktop (icons handle their own right-click) */
        select_icon(NULL);
        popup_desktop_menu((GdkEvent *)e);
        return TRUE;
    }
    return FALSE;
}

/* Keyboard on the desktop (it has the focus after a click on it). */
static gboolean on_desktop_key(GtkWidget *w, GdkEventKey *e, gpointer d)
{
    (void)w; (void)d;
    GdkModifierType mods = e->state & gtk_accelerator_get_default_mod_mask();
    GList *sel = selected_icons();
    GPtrArray *all = g_ptr_array_new_with_free_func(g_free), *ops = g_ptr_array_new_with_free_func(g_free);
    for (GList *l = sel; l; l = l->next) {
        const char *p = g_object_get_data(G_OBJECT(l->data), "target");
        if (!p) continue;
        g_ptr_array_add(all, g_strdup(p));
        if (!g_object_get_data(G_OBJECT(l->data), "home")) g_ptr_array_add(ops, g_strdup(p));
    }
    GtkWidget *first = sel ? sel->data : NULL;
    g_list_free(sel);
    gboolean ctrl = mods == GDK_CONTROL_MASK, handled = TRUE;
    switch (e->keyval) {
    case GDK_KEY_Return: case GDK_KEY_KP_Enter: case GDK_KEY_ISO_Enter:
        if (mods == GDK_MOD1_MASK) show_properties(all); else if (!mods) open_paths(all); else handled = FALSE;
        break;
    case GDK_KEY_Delete: case GDK_KEY_KP_Delete:
        if (mods == GDK_SHIFT_MASK) delete_paths(ops, TRUE); else if (!mods) trash_paths(ops); else handled = FALSE;
        break;
    case GDK_KEY_F2:
        if (ops->len == 1 && all->len == 1) rename_path(g_ptr_array_index(ops, 0));
        break;
    case GDK_KEY_Escape:
        select_icon(NULL);
        break;
    case GDK_KEY_Menu: case GDK_KEY_F10:
        if (e->keyval == GDK_KEY_F10 && mods != GDK_SHIFT_MASK) { handled = FALSE; break; }
        if (first) popup_icon_menu(first, (GdkEvent *)e);
        else popup_desktop_menu((GdkEvent *)e);
        break;
    case GDK_KEY_a: case GDK_KEY_A:
        if (ctrl) m_select_all(NULL, NULL); else handled = FALSE;
        break;
    case GDK_KEY_c: case GDK_KEY_C: case GDK_KEY_Insert:
        if (ctrl) clipboard_set_files(ops, FALSE); else handled = FALSE;
        break;
    case GDK_KEY_x: case GDK_KEY_X:
        if (ctrl) clipboard_set_files(ops, TRUE); else handled = FALSE;
        break;
    case GDK_KEY_v: case GDK_KEY_V:
        if (ctrl) paste_files(); else handled = FALSE;
        break;
    default:
        handled = FALSE;
    }
    g_ptr_array_unref(all);
    g_ptr_array_unref(ops);
    return handled;
}

static void on_dir_changed(GFileMonitor *m, GFile *f, GFile *o, GFileMonitorEvent ev, gpointer d)
{
    (void)m; (void)f; (void)o; (void)ev; (void)d;
    queue_reload();
}

/* Accent color of Settings > Appearance for the selection frame. */
static void load_accent(void)
{
    HdeThemeInfo ti;
    hde_theme_info_load(&ti);
    GdkRGBA c;
    if (ti.accent && gdk_rgba_parse(&c, ti.accent)) accent = c;
    hde_theme_info_clear(&ti);
    if (fixed) gtk_widget_queue_draw(fixed);
}

static void on_theme_changed(gpointer d)
{
    (void)d;
    load_accent();
    int t = 0, b = 0;                     /* Settings > Panel: moved to the other edge, or another height */
    hde_panel_reserved(&t, &b);
    if (t != panel_top_px || b != panel_bottom_px) {
        panel_top_px = t;
        panel_bottom_px = b;
        DBG("panel now takes %d px at the top, %d px at the bottom: icons rearranged", t, b);
        reload_icons();
    }
}

/* Settings > Appearance writes the wallpaper to config.ini: reload as soon as the file changes. */
static guint config_reload_id;
static gboolean config_reload(gpointer d)
{
    (void)d;
    config_reload_id = 0;
    load_config();
    return G_SOURCE_REMOVE;
}

static void on_config_changed(GFileMonitor *m, GFile *f, GFile *o, GFileMonitorEvent ev, gpointer d)
{
    (void)m; (void)f; (void)o; (void)d;
    if (ev == G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED) return;
    if (!config_reload_id) config_reload_id = g_timeout_add(300, config_reload, NULL);
}

static void load_css(void)
{
    /* the hover / selection frame itself is drawn by draw_icon_frame() (event boxes ignore CSS backgrounds) */
    const char *css =
        ".desk-icon label, .desk-icon label:backdrop { color: white; text-shadow: 1px 1px 2px black, 0 0 4px black; }"
        /* symbolic icons use the text color: keep them bright even when the desktop is unfocused (:backdrop) */
        ".desk-icon image, .desk-icon image:backdrop { color: #eef1f6; -gtk-icon-shadow: 0 1px 3px rgba(0,0,0,0.65); }"
        ".desk-icon.selected label, .desk-icon.selected label:backdrop { color: #ffffff; text-shadow: 0 1px 2px rgba(0,0,0,0.9); }";
    GtkCssProvider *p = gtk_css_provider_new();
    gtk_css_provider_load_from_data(p, css, -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(p), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(p);
}

int main(int argc, char **argv)
{
    gtk_init(&argc, &argv);
    debug_on = g_getenv("HDE_DEBUG") != NULL;
    hde_panel_reserved(&panel_top_px, &panel_bottom_px);
    hde_theme_apply_process();          /* right-click menu / dialogs follow Dark mode */
    hde_theme_watch(on_theme_changed, NULL);
    load_accent();
    load_css();

    GdkDisplay *dpy = gdk_display_get_default();
    GdkMonitor *m = gdk_display_get_primary_monitor(dpy);
    if (!m) m = gdk_display_get_monitor(dpy, 0);
    gdk_monitor_get_geometry(m, &mon);

    GdkScreen *scr = gdk_display_get_default_screen(dpy);
    int sw = gdk_screen_get_width(scr), sh = gdk_screen_get_height(scr);

    win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), "hde-desktop");
    if (hde_wl_layer_init(GTK_WINDOW(win), "hde-desktop", HDE_LAYER_BACKGROUND, HDE_EDGE_ALL, HDE_KB_ON_DEMAND)) {
        /* Wayland: the background layer of the main screen (under the panel too); keys when clicked */
        hde_wl_layer_exclusive(GTK_WINDOW(win), -1);
        hde_wl_layer_monitor(GTK_WINDOW(win), m);
        mon.x = mon.y = 0;
        gtk_widget_set_size_request(win, mon.width, mon.height);
    } else {
        gtk_window_set_type_hint(GTK_WINDOW(win), GDK_WINDOW_TYPE_HINT_DESKTOP);
        gtk_window_set_decorated(GTK_WINDOW(win), FALSE);
        gtk_window_set_skip_taskbar_hint(GTK_WINDOW(win), TRUE);
        gtk_window_set_skip_pager_hint(GTK_WINDOW(win), TRUE);
        gtk_window_stick(GTK_WINDOW(win));
        gtk_window_set_keep_below(GTK_WINDOW(win), TRUE);   /* never on top of the panel / other windows */
        gtk_widget_set_size_request(win, sw, sh);
        gtk_window_set_default_size(GTK_WINDOW(win), sw, sh);
        gtk_window_move(GTK_WINDOW(win), 0, 0);
    }
    gtk_widget_set_app_paintable(win, TRUE);
    gtk_widget_add_events(win, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK | GDK_KEY_PRESS_MASK);
    g_signal_connect(win, "draw", G_CALLBACK(on_draw), NULL);
    g_signal_connect(win, "button-press-event", G_CALLBACK(on_bg_press), NULL);
    g_signal_connect(win, "motion-notify-event", G_CALLBACK(on_fixed_motion), NULL);
    g_signal_connect(win, "button-release-event", G_CALLBACK(on_fixed_release), NULL);
    g_signal_connect(win, "key-press-event", G_CALLBACK(on_desktop_key), NULL);
    g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    fixed = gtk_fixed_new();
    g_signal_connect(fixed, "destroy", G_CALLBACK(gtk_widget_destroyed), &fixed);
    g_signal_connect_after(fixed, "draw", G_CALLBACK(on_fixed_draw), NULL);   /* rubber band on top of the icons */
    gtk_container_add(GTK_CONTAINER(win), fixed);

    load_config();
    reload_icons();
    {
        char *cf = config_file();
        char *dir = g_path_get_dirname(cf);
        g_mkdir_with_parents(dir, 0755);
        GFile *f = g_file_new_for_path(cf);
        GFileMonitor *cm = g_file_monitor_file(f, G_FILE_MONITOR_NONE, NULL, NULL);
        if (cm) g_signal_connect(cm, "changed", G_CALLBACK(on_config_changed), NULL);
        g_object_unref(f);
        g_free(dir);
        g_free(cf);
    }

    const char *ddir = g_get_user_special_dir(G_USER_DIRECTORY_DESKTOP);
    if (ddir) {
        GFile *f = g_file_new_for_path(ddir);
        GFileMonitor *fm = g_file_monitor_directory(f, G_FILE_MONITOR_NONE, NULL, NULL);
        if (fm) g_signal_connect(fm, "changed", G_CALLBACK(on_dir_changed), NULL);
        g_object_unref(f);
    }

    gtk_widget_show_all(win);
    if (hde_wl_is_layer(GTK_WINDOW(win))) {
        update_extra_wallpapers(m);
        fprintf(stderr, "hde-desktop: Wayland: desktop on the background layer, %dx%d\n", mon.width, mon.height);
    } else gdk_window_lower(gtk_widget_get_window(win));  /* even without a WM, or when the WM ignores the DESKTOP hint */
    g_signal_connect(scr, "monitors-changed", G_CALLBACK(on_screens_changed), NULL);
    g_signal_connect(scr, "size-changed", G_CALLBACK(on_screens_changed), NULL);
    gtk_main();
    return 0;
}
