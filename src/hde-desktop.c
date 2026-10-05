/* hde-desktop: wallpaper and desktop icons from ~/Desktop */
#include <gtk/gtk.h>
#include <gio/gdesktopappinfo.h>
#include <glib/gstdio.h>
#include "hde-theme.h"
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <math.h>

#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#define PANEL_HEIGHT 34
#define CELL_W 100
#define CELL_H 108
#define MARGIN 16

static GtkWidget *win, *fixed;
static GdkPixbuf *wallpaper;
static char *wallpaper_path;
static int wallpaper_mode;          /* 0 Fill, 1 Fit, 2 Stretch, 3 Center (Settings > Appearance) */
static GtkWidget *selected;
static GdkRectangle mon;

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
static void m_arrange_icons(GtkMenuItem *i, gpointer d);

/* ---------- configuration ---------- */
static char *config_file(void)
{
    return g_build_filename(g_get_user_config_dir(), "hde", "config.ini", NULL);
}

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
    if (win) gtk_widget_queue_draw(win);
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
        /* Bản Settings cũ chỉ lưu wallpaper vào settings.ini */
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
static gboolean on_draw(GtkWidget *w, cairo_t *cr, gpointer data)
{
    int W = gtk_widget_get_allocated_width(w);
    int H = gtk_widget_get_allocated_height(w);

    if (wallpaper) {
        double pw = gdk_pixbuf_get_width(wallpaper), ph = gdk_pixbuf_get_height(wallpaper);
        double sx, sy;
        switch (wallpaper_mode) {
        case 1:  sx = sy = MIN(W / pw, H / ph); break;          /* Fit: thấy trọn ảnh */
        case 2:  sx = W / pw; sy = H / ph; break;                /* Stretch */
        case 3:  sx = sy = 1.0; break;                           /* Center: giữ kích thước gốc */
        default: sx = sy = MAX(W / pw, H / ph); break;           /* Fill: phủ kín, cắt phần thừa */
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
    return FALSE;   /* tiếp tục vẽ các icon con */
}

/* ---------- icons ---------- */
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
    }
    char *uri = g_filename_to_uri(path, NULL, NULL);
    if (!uri) return;
    if (!g_app_info_launch_default_for_uri(uri, NULL, &err)) {
        /* Không có handler mặc định (ISO tối giản): thử xdg-open / file manager. */
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

static void select_icon(GtkWidget *ev)
{
    GList *children = fixed ? gtk_container_get_children(GTK_CONTAINER(fixed)) : NULL;
    for (GList *l = children; l; l = l->next)
        gtk_style_context_remove_class(gtk_widget_get_style_context(GTK_WIDGET(l->data)), "selected");
    g_list_free(children);
    selected = ev;
    if (ev) gtk_style_context_add_class(gtk_widget_get_style_context(ev), "selected");
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

/* Tự nhận diện double-click thay vì chỉ dựa vào GDK_2BUTTON_PRESS: GDK bỏ qua
 * double-click nếu chuột lệch >5px hoặc dưới Xephyr/touchpad nên icon "không bấm được". */
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
    if (e->button != 1) return FALSE;
    if (e->type == GDK_3BUTTON_PRESS) return TRUE;
    select_icon(ev);

    if (e->type == GDK_2BUTTON_PRESS) {
        /* Đã mở bởi nhánh tự nhận diện ở lần nhấn thứ 2 thì bỏ qua. */
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
    gint max_y = MAX(MARGIN, gtk_widget_get_allocated_height(fixed) - PANEL_HEIGHT - CELL_H);
    gint sx = MARGIN + (gint)round(((*x - MARGIN) / (gdouble)CELL_W)) * CELL_W;
    gint sy = MARGIN + (gint)round(((*y - MARGIN) / (gdouble)CELL_H)) * CELL_H;
    sx = MAX(MARGIN, MIN(sx, max_x));
    sy = MAX(MARGIN, MIN(sy, max_y));
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
    gint max_y = MAX(MARGIN, gtk_widget_get_allocated_height(fixed) - PANEL_HEIGHT - CELL_H);
    gint cols = MAX(1, ((max_x - MARGIN) / CELL_W) + 1);
    gint rows = MAX(1, ((max_y - MARGIN) / CELL_H) + 1);

    gint wx = wanted_x;
    gint wy = wanted_y;
    snap_icon_position(&wx, &wy);

    /* Search in expanding Manhattan rings around the requested cell. */
    gint target_col = (wx - MARGIN) / CELL_W;
    gint target_row = (wy - MARGIN) / CELL_H;
    gint max_radius = MAX(cols, rows);

    for (gint radius = 0; radius <= max_radius; ++radius) {
        for (gint row = MAX(0, target_row - radius);
             row <= MIN(rows - 1, target_row + radius); ++row) {
            for (gint col = MAX(0, target_col - radius);
                 col <= MIN(cols - 1, target_col + radius); ++col) {
                if (MAX(abs(col - target_col), abs(row - target_row)) != radius)
                    continue;

                gint x = MARGIN + col * CELL_W;
                gint y = MARGIN + row * CELL_H;
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
    ny = MAX(MARGIN, MIN(ny, gtk_widget_get_allocated_height(fixed) - PANEL_HEIGHT - CELL_H));
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
    gtk_widget_add_events(ev, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK);
    gtk_style_context_add_class(gtk_widget_get_style_context(ev), "desk-icon");
    g_object_set_data_full(G_OBJECT(ev), "target", g_strdup(path), g_free);

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
    selected = NULL;
    gtk_container_foreach(GTK_CONTAINER(fixed), destroy_child, NULL);

    gint available_h = MAX(CELL_H, mon.height - PANEL_HEIGHT - 2 * MARGIN);
    int rows = MAX(1, available_h / CELL_H);
    int idx = 0;

    #define PLACE(w) do { \
        const char *_pp = g_object_get_data(G_OBJECT(w), "target"); \
        if (keep_arranged || !restore_icon_position((w), _pp)) { \
            gint _px = MARGIN + (idx / rows) * CELL_W; \
            gint _py = MARGIN + (idx % rows) * CELL_H; \
            gint _fx = _px, _fy = _py; \
            if (!find_free_cell(_px, _py, (w), &_fx, &_fy)) { \
                _fx = _px; _fy = _py; \
            } \
            gtk_fixed_put(GTK_FIXED(fixed), (w), _fx, _fy); \
        } \
        idx++; \
    } while (0)

    /* Thư mục Home luôn có */
    GIcon *home_icon = g_themed_icon_new("user-home");
    GtkWidget *home = make_icon("Home", home_icon, g_get_home_dir());
    g_object_unref(home_icon);
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
    gtk_widget_show_all(fixed);
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

static void launch_command_candidates(const char *const *commands)
{
    for (int k = 0; commands[k]; k++) {
        char *p = g_find_program_in_path(commands[k]);
        if (!p) continue;

        GError *err = NULL;
        if (spawn_program(p, &err)) {
            g_free(p);
            g_clear_error(&err);
            return;
        }

        g_clear_error(&err);
        g_free(p);
    }
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

static void m_terminal(GtkMenuItem *i, gpointer d)
{
    (void)i; (void)d;
    const char *terms[] = { "x-terminal-emulator", "xfce4-terminal", "lxterminal",
                            "gnome-terminal", "konsole", "mate-terminal", "xterm", NULL };
    launch_command_candidates(terms);
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
    selected = NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(fixed));
    for (GList *l = children; l; l = l->next) {
        GtkStyleContext *ctx = gtk_widget_get_style_context(GTK_WIDGET(l->data));
        gtk_style_context_add_class(ctx, "selected");
    }
    g_list_free(children);
}

static gboolean clipboard_has_files(void)
{
    GtkClipboard *cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    gint n = 0;
    GdkAtom *targets = NULL;
    gboolean found = FALSE;
    if (gtk_clipboard_wait_for_targets(cb, &targets, &n)) {
        for (gint i = 0; i < n; i++) {
            gchar *name = gdk_atom_name(targets[i]);
            if (name && !g_ascii_strcasecmp(name, "text/uri-list")) {
                found = TRUE;
                g_free(name);
                break;
            }
            g_free(name);
        }
        g_free(targets);
    }
    return found;
}

static void m_paste(GtkMenuItem *i, gpointer d)
{
    (void)i; (void)d;
    const char *ddir = g_get_user_special_dir(G_USER_DIRECTORY_DESKTOP);
    if (!ddir) return;
    GtkClipboard *cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    gchar **uris = gtk_clipboard_wait_for_uris(cb);
    if (!uris) return;

    for (gint i = 0; uris[i]; i++) {
        GFile *src = g_file_new_for_uri(uris[i]);
        char *name = g_file_get_basename(src);
        if (!name || !*name) { g_clear_object(&src); g_free(name); continue; }
        char *dest_path = g_build_filename(ddir, name, NULL);
        GFile *dst = g_file_new_for_path(dest_path);
        GError *err = NULL;
        if (!g_file_copy(src, dst, G_FILE_COPY_NONE, NULL, NULL, NULL, &err)) {
            g_clear_error(&err);
        }
        g_object_unref(src);
        g_object_unref(dst);
        g_free(dest_path);
        g_free(name);
    }
    g_strfreev(uris);
    reload_icons();
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

static void m_about(GtkMenuItem *i, gpointer d)
{
    (void)i; (void)d;
    const gchar *authors[] = { "HyggshiOSDeveloper", NULL };
    gtk_show_about_dialog(GTK_WINDOW(win),
        "program-name", "Hyggshi Desktop Environment",
        "version", "1.0",
        "comments", "A lightweight GTK3 desktop environment for Hyggshi OS.",
        "copyright", "Copyright © 2026 HyggshiOSDeveloper",
        "authors", authors,
        "website", "https://github.com/HyggshiOSDeveloper/Hyggshi-OS-project-center",
        "license", "MIT License",
        NULL);
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

static void popup_desktop_menu(GdkEventButton *e)
{
    GtkWidget *menu = gtk_menu_new();
    GtkWidget *item;

    item = gtk_menu_item_new_with_label("New Folder");
    g_signal_connect(item, "activate", G_CALLBACK(m_new_folder), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

    item = gtk_menu_item_new_with_label("Paste");
    gtk_widget_set_sensitive(item, clipboard_has_files());
    g_signal_connect(item, "activate", G_CALLBACK(m_paste), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

    item = gtk_menu_item_new_with_label("Select All");
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

    gtk_widget_show_all(menu);
    g_signal_connect(menu, "selection-done", G_CALLBACK(gtk_widget_destroy), NULL);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)e);
}

static void clear_icon_selection(void)
{
    GList *children = gtk_container_get_children(GTK_CONTAINER(fixed));
    for (GList *l = children; l; l = l->next)
        gtk_style_context_remove_class(gtk_widget_get_style_context(GTK_WIDGET(l->data)), "selected");
    g_list_free(children);
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

static void update_box_selection(void)
{
    GList *children = gtk_container_get_children(GTK_CONTAINER(fixed));
    for (GList *l = children; l; l = l->next) {
        GtkWidget *child = GTK_WIDGET(l->data);
        GtkStyleContext *ctx = gtk_widget_get_style_context(child);
        if (point_in_selection(child, select_start_x, select_start_y, select_cur_x, select_cur_y))
            gtk_style_context_add_class(ctx, "selected");
        else
            gtk_style_context_remove_class(ctx, "selected");
    }
    g_list_free(children);
}

static gboolean on_fixed_press(GtkWidget *w, GdkEventButton *e, gpointer d)
{
    (void)w; (void)d;
    if (e->button != 1 || e->type != GDK_BUTTON_PRESS) return FALSE;
    clear_icon_selection();
    selected = NULL;
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
    if (e->button == 1) { select_icon(NULL); return FALSE; }
    if (e->button == 3) {
        popup_desktop_menu(e);
        return TRUE;
    }
    return FALSE;
}

static void on_dir_changed(GFileMonitor *m, GFile *f, GFile *o, GFileMonitorEvent ev, gpointer d)
{
    reload_icons();
}

/* Settings > Appearance ghi hình nền vào config.ini: nạp lại ngay khi file đổi. */
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
    const char *css =
        ".desk-icon { border: 1px solid transparent; border-radius: 8px; padding: 2px; }"
        ".desk-icon label { color: white; text-shadow: 1px 1px 2px black, 0 0 4px black; }"
        ".desk-icon.selected { background: rgba(61,111,217,0.52); border: 2px solid rgba(125,175,255,0.95); border-radius: 8px; box-shadow: 0 0 0 1px rgba(20,50,100,0.65), 0 2px 8px rgba(0,0,0,0.28); }"
        ".desk-icon.selected label { color: #ffffff; font-weight: 600; }"
        ".desk-icon:hover { background: rgba(255,255,255,0.12); border: 1px solid rgba(255,255,255,0.25); border-radius: 8px; }";
    GtkCssProvider *p = gtk_css_provider_new();
    gtk_css_provider_load_from_data(p, css, -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(p), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(p);
}

int main(int argc, char **argv)
{
    gtk_init(&argc, &argv);
    hde_theme_apply_process();          /* menu chuột phải / hộp thoại theo Dark mode */
    hde_theme_watch(NULL, NULL);
    load_css();

    GdkDisplay *dpy = gdk_display_get_default();
    GdkMonitor *m = gdk_display_get_primary_monitor(dpy);
    if (!m) m = gdk_display_get_monitor(dpy, 0);
    gdk_monitor_get_geometry(m, &mon);

    GdkScreen *scr = gdk_display_get_default_screen(dpy);
    int sw = gdk_screen_get_width(scr), sh = gdk_screen_get_height(scr);

    win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), "hde-desktop");
    gtk_window_set_type_hint(GTK_WINDOW(win), GDK_WINDOW_TYPE_HINT_DESKTOP);
    gtk_window_set_decorated(GTK_WINDOW(win), FALSE);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(win), TRUE);
    gtk_window_set_skip_pager_hint(GTK_WINDOW(win), TRUE);
    gtk_window_stick(GTK_WINDOW(win));
    gtk_window_set_keep_below(GTK_WINDOW(win), TRUE);   /* không bao giờ đè lên panel/cửa sổ khác */
    gtk_widget_set_size_request(win, sw, sh);
    gtk_window_set_default_size(GTK_WINDOW(win), sw, sh);
    gtk_window_move(GTK_WINDOW(win), 0, 0);
    gtk_widget_set_app_paintable(win, TRUE);
    gtk_widget_add_events(win, GDK_BUTTON_PRESS_MASK);
    g_signal_connect(win, "draw", G_CALLBACK(on_draw), NULL);
    g_signal_connect(win, "button-press-event", G_CALLBACK(on_bg_press), NULL);
    g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    fixed = gtk_fixed_new();
    gtk_widget_add_events(fixed, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK);
    g_signal_connect(fixed, "button-press-event", G_CALLBACK(on_fixed_press), NULL);
    g_signal_connect(fixed, "motion-notify-event", G_CALLBACK(on_fixed_motion), NULL);
    g_signal_connect(fixed, "button-release-event", G_CALLBACK(on_fixed_release), NULL);
    g_signal_connect_after(fixed, "draw", G_CALLBACK(on_fixed_draw), NULL);
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
    gdk_window_lower(gtk_widget_get_window(win));         /* kể cả khi không có WM hoặc WM bỏ qua DESKTOP hint */
    gtk_main();
    return 0;
}
