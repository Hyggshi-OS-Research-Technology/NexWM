/* hde-camera.c — Hyggshi Camera: GTK3 Webcam Photo & Video Capture for HDE. */
#define _POSIX_C_SOURCE 200809L

#include "hde-build.h"
#include "hde-theme.h"
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gtk/gtk.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define APP_TITLE "Hyggshi Camera"

typedef enum {
    FILTER_NORMAL = 0,
    FILTER_GRAYSCALE,
    FILTER_SEPIA,
    FILTER_WARM,
    FILTER_COOL,
    FILTER_INVERT
} CamFilter;

static GtkCssProvider *app_css;
static GtkWidget *main_win;
static GtkWidget *viewfinder;
static GtkWidget *device_combo;
static GtkWidget *res_combo;
static GtkWidget *timer_combo;
static GtkWidget *filter_combo;
static GtkWidget *mirror_toggle;
static GtkWidget *grid_toggle;
static GtkWidget *shutter_btn;
static GtkWidget *record_btn;
static GtkWidget *status_lbl;
static GtkWidget *thumb_img;

static GdkPixbuf *live_frame;
static char *last_saved_path;
static char *temp_frame_path;
static GPid capture_pid;
static gboolean capture_in_flight;

static gboolean is_recording;
static GPid record_pid;
static gint64 record_start_us;
static char *recording_path;

static int countdown_rem;
static guint countdown_timer_id;
static double flash_alpha;
static double anim_phase;

static void load_css(void)
{
    HdeThemeInfo ti;
    hde_theme_info_load(&ti);
    char *accent = hde_theme_accent_css(&ti);
    const char *ac = ti.accent ? ti.accent : "#3584e4";
    char *css = g_strdup_printf(
        ".cam-view { background-color: #0d0f14; border-radius: 12px; }"
        ".cam-bar { background-color: alpha(@theme_fg_color, 0.04); border-top: 1px solid alpha(@theme_fg_color, 0.10);"
        "  padding: 10px 14px; }"
        ".cam-shutter { border-radius: 24px; padding: 8px 22px; font-weight: 700; background-color: %s; color: #ffffff; }"
        ".cam-record-active { background-color: #e01b24; color: #ffffff; font-weight: 700; }"
        ".cam-status { font-size: 12px; opacity: 0.80; }"
        "%s",
        ac, accent);
    if (!app_css) {
        app_css = gtk_css_provider_new();
        GdkScreen *scr = gdk_screen_get_default();
        if (scr)
            gtk_style_context_add_provider_for_screen(scr, GTK_STYLE_PROVIDER(app_css),
                                                      GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
    gtk_css_provider_load_from_data(app_css, css, -1, NULL);
    g_free(css);
    g_free(accent);
    hde_theme_info_clear(&ti);
}

static void on_theme_changed(gpointer d)
{
    (void)d;
    load_css();
}

static char *camera_pictures_dir(void)
{
    const char *pic = g_get_user_special_dir(G_USER_DIRECTORY_PICTURES);
    char *base = (pic && *pic) ? g_strdup(pic) : g_build_filename(g_get_home_dir(), "Pictures", NULL);
    char *dir = g_build_filename(base, "Camera", NULL);
    g_mkdir_with_parents(dir, 0755);
    g_free(base);
    return dir;
}

static char *camera_videos_dir(void)
{
    const char *vid = g_get_user_special_dir(G_USER_DIRECTORY_VIDEOS);
    char *base = (vid && *vid) ? g_strdup(vid) : g_build_filename(g_get_home_dir(), "Videos", NULL);
    char *dir = g_build_filename(base, "Camera", NULL);
    g_mkdir_with_parents(dir, 0755);
    g_free(base);
    return dir;
}

static void populate_devices(void)
{
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(device_combo));
    int count = 0;
    for (int i = 0; i < 8; i++) {
        char dev[32], sys_name[128];
        g_snprintf(dev, sizeof dev, "/dev/video%d", i);
        if (!g_file_test(dev, G_FILE_TEST_EXISTS)) continue;
        g_snprintf(sys_name, sizeof sys_name, "/sys/class/video4linux/video%d/name", i);
        char *name = NULL;
        if (g_file_get_contents(sys_name, &name, NULL, NULL) && name)
            g_strstrip(name);
        char *label = g_strdup_printf("%s (%s)", (name && *name) ? name : "Camera", dev);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(device_combo), dev, label);
        g_free(label);
        g_free(name);
        count++;
    }
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(device_combo), "virtual",
                              count > 0 ? "Virtual Test Camera (Simulated)" : "Virtual Camera (No /dev/video* detected)");
    gtk_combo_box_set_active(GTK_COMBO_BOX(device_combo), 0);
}

static void selected_resolution(int *w_out, int *h_out)
{
    int idx = gtk_combo_box_get_active(GTK_COMBO_BOX(res_combo));
    if (idx == 1) { *w_out = 1920; *h_out = 1080; }
    else if (idx == 2) { *w_out = 640; *h_out = 480; }
    else { *w_out = 1280; *h_out = 720; }
}

static void apply_filter_to_surface(cairo_surface_t *surf, CamFilter filter, gboolean mirror)
{
    if (!surf || (filter == FILTER_NORMAL && !mirror)) return;
    cairo_surface_flush(surf);
    int w = cairo_image_surface_get_width(surf);
    int h = cairo_image_surface_get_height(surf);
    int stride = cairo_image_surface_get_stride(surf);
    unsigned char *data = cairo_image_surface_get_data(surf);
    if (!data) return;

    if (mirror) {
        for (int y = 0; y < h; y++) {
            guint32 *row = (guint32 *)(data + y * stride);
            for (int x = 0; x < w / 2; x++) {
                guint32 tmp = row[x];
                row[x] = row[w - 1 - x];
                row[w - 1 - x] = tmp;
            }
        }
    }
    if (filter == FILTER_NORMAL) {
        cairo_surface_mark_dirty(surf);
        return;
    }

    for (int y = 0; y < h; y++) {
        unsigned char *p = data + y * stride;
        for (int x = 0; x < w; x++, p += 4) {
            int b = p[0], g = p[1], r = p[2];
            int nr = r, ng = g, nb = b;
            switch (filter) {
            case FILTER_GRAYSCALE:
                nr = ng = nb = (r * 77 + g * 150 + b * 29) >> 8;
                break;
            case FILTER_SEPIA:
                nr = MIN(255, (r * 101 + g * 197 + b * 48) >> 8);
                ng = MIN(255, (r * 89 + g * 175 + b * 43) >> 8);
                nb = MIN(255, (r * 70 + g * 137 + b * 33) >> 8);
                break;
            case FILTER_WARM:
                nr = MIN(255, r + 24);
                ng = MIN(255, g + 8);
                nb = MAX(0, b - 18);
                break;
            case FILTER_COOL:
                nr = MAX(0, r - 18);
                ng = MIN(255, g + 6);
                nb = MIN(255, b + 26);
                break;
            case FILTER_INVERT:
                nr = 255 - r;
                ng = 255 - g;
                nb = 255 - b;
                break;
            default:
                break;
            }
            p[0] = (unsigned char)nb;
            p[1] = (unsigned char)ng;
            p[2] = (unsigned char)nr;
        }
    }
    cairo_surface_mark_dirty(surf);
}

/* Render either the hardware camera frame or the live animated synthetic scene onto a Cairo surface */
static cairo_surface_t *render_camera_frame(int w, int h, gboolean include_hud)
{
    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_t *cr = cairo_create(surf);

    if (live_frame) {
        int fw = gdk_pixbuf_get_width(live_frame);
        int fh = gdk_pixbuf_get_height(live_frame);
        cairo_save(cr);
        cairo_scale(cr, (double)w / MAX(1, fw), (double)h / MAX(1, fh));
        gdk_cairo_set_source_pixbuf(cr, live_frame, 0, 0);
        cairo_paint(cr);
        cairo_restore(cr);
    } else {
        /* Animated scenic studio test viewfinder */
        cairo_pattern_t *grad = cairo_pattern_create_linear(0, 0, w, h);
        cairo_pattern_add_color_stop_rgb(grad, 0.0, 0.08, 0.14, 0.26);
        cairo_pattern_add_color_stop_rgb(grad, 0.5, 0.12, 0.24, 0.38);
        cairo_pattern_add_color_stop_rgb(grad, 1.0, 0.18, 0.12, 0.28);
        cairo_set_source(cr, grad);
        cairo_paint(cr);
        cairo_pattern_destroy(grad);

        /* Soft moving orbs */
        for (int i = 0; i < 3; i++) {
            double ox = w * (0.25 + 0.25 * i) + cos(anim_phase + i * 1.7) * (w * 0.08);
            double oy = h * 0.45 + sin(anim_phase * 1.3 + i * 2.1) * (h * 0.12);
            double rad = MIN(w, h) * (0.14 + 0.03 * i);
            cairo_arc(cr, ox, oy, rad, 0, 2 * G_PI);
            if (i == 0) cairo_set_source_rgba(cr, 0.21, 0.52, 0.89, 0.38);
            else if (i == 1) cairo_set_source_rgba(cr, 0.18, 0.76, 0.49, 0.34);
            else cairo_set_source_rgba(cr, 0.90, 0.38, 0.00, 0.34);
            cairo_fill(cr);
        }

        /* SMPTE reference color strip at bottom-left */
        static const double bars[7][3] = {
            { 0.85, 0.85, 0.85 }, { 0.85, 0.85, 0.10 }, { 0.10, 0.85, 0.85 },
            { 0.10, 0.85, 0.10 }, { 0.85, 0.10, 0.85 }, { 0.85, 0.10, 0.10 }, { 0.10, 0.10, 0.85 }
        };
        double bw = 22.0, bh = 12.0, bx = 20.0, by = h - 34.0;
        for (int i = 0; i < 7; i++) {
            cairo_set_source_rgb(cr, bars[i][0], bars[i][1], bars[i][2]);
            cairo_rectangle(cr, bx + i * bw, by, bw, bh);
            cairo_fill(cr);
        }
    }

    CamFilter filter = (CamFilter)CLAMP(gtk_combo_box_get_active(GTK_COMBO_BOX(filter_combo)), 0, 5);
    gboolean mirror = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(mirror_toggle));
    apply_filter_to_surface(surf, filter, mirror);

    /* Timestamp watermark on captured frame / viewfinder */
    GDateTime *now = g_date_time_new_now_local();
    char *ts = g_date_time_format(now, "%Y-%m-%d %H:%M:%S");
    g_date_time_unref(now);
    cairo_select_font_face(cr, "monospace", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, MAX(13.0, h * 0.024));
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.78);
    cairo_move_to(cr, w - 210, h - 18);
    cairo_show_text(cr, ts);
    g_free(ts);

    if (include_hud) {
        if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(grid_toggle))) {
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.28);
            cairo_set_line_width(cr, 1.0);
            for (int i = 1; i <= 2; i++) {
                cairo_move_to(cr, (w * i) / 3.0, 0);
                cairo_line_to(cr, (w * i) / 3.0, h);
                cairo_move_to(cr, 0, (h * i) / 3.0);
                cairo_line_to(cr, w, (h * i) / 3.0);
            }
            cairo_stroke(cr);
        }
        if (is_recording) {
            gint64 sec = (g_get_monotonic_time() - record_start_us) / 1000000;
            char rbuf[64];
            g_snprintf(rbuf, sizeof rbuf, "REC  %02lld:%02lld", (long long)(sec / 60), (long long)(sec % 60));
            cairo_set_source_rgba(cr, 0.88, 0.11, 0.14, 0.92);
            cairo_arc(cr, 28, 28, 7, 0, 2 * G_PI);
            cairo_fill(cr);
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.95);
            cairo_set_font_size(cr, 15.0);
            cairo_move_to(cr, 42, 33);
            cairo_show_text(cr, rbuf);
        }
        if (countdown_rem > 0) {
            char cbuf[16];
            g_snprintf(cbuf, sizeof cbuf, "%d", countdown_rem);
            cairo_set_font_size(cr, MIN(w, h) * 0.22);
            cairo_text_extents_t ext;
            cairo_text_extents(cr, cbuf, &ext);
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.92);
            cairo_move_to(cr, w / 2.0 - ext.width / 2.0 - ext.x_bearing,
                          h / 2.0 - ext.height / 2.0 - ext.y_bearing);
            cairo_show_text(cr, cbuf);
        }
        if (flash_alpha > 0.01) {
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, flash_alpha);
            cairo_paint(cr);
        }
    }

    cairo_destroy(cr);
    return surf;
}

static gboolean on_viewfinder_draw(GtkWidget *w, cairo_t *cr, gpointer d)
{
    (void)d;
    int aw = gtk_widget_get_allocated_width(w);
    int ah = gtk_widget_get_allocated_height(w);
    if (aw < 10 || ah < 10) return FALSE;

    cairo_surface_t *surf = render_camera_frame(aw, ah, TRUE);
    cairo_set_source_surface(cr, surf, 0, 0);
    cairo_paint(cr);
    cairo_surface_destroy(surf);
    return FALSE;
}

static void on_v4l2_frame_done(GPid pid, gint status, gpointer d)
{
    (void)d;
    g_spawn_close_pid(pid);
    capture_in_flight = FALSE;
    if (status == 0 && temp_frame_path && g_file_test(temp_frame_path, G_FILE_TEST_EXISTS)) {
        GdkPixbuf *pb = gdk_pixbuf_new_from_file(temp_frame_path, NULL);
        if (pb) {
            g_clear_object(&live_frame);
            live_frame = pb;
        }
    }
}

static void poll_hardware_camera(void)
{
    if (capture_in_flight || is_recording) return;
    const char *dev = gtk_combo_box_get_active_id(GTK_COMBO_BOX(device_combo));
    if (!dev || !g_str_has_prefix(dev, "/dev/video")) {
        g_clear_object(&live_frame);
        return;
    }
    char *ff = g_find_program_in_path("ffmpeg");
    if (!ff) return;
    if (!temp_frame_path)
        temp_frame_path = g_strdup_printf("/tmp/hde-camera-preview-%d.jpg", (int)getpid());
    char *argv[] = {
        ff, (char *)"-nostdin", (char *)"-loglevel", (char *)"quiet",
        (char *)"-y", (char *)"-f", (char *)"v4l2",
        (char *)"-i", (char *)dev,
        (char *)"-frames:v", (char *)"1",
        temp_frame_path, NULL
    };
    if (g_spawn_async(NULL, argv, NULL, G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                      NULL, NULL, &capture_pid, NULL)) {
        capture_in_flight = TRUE;
        g_child_watch_add(capture_pid, on_v4l2_frame_done, NULL);
    }
    g_free(ff);
}

static void capture_photo_now(void)
{
    int rw = 1280, rh = 720;
    selected_resolution(&rw, &rh);
    cairo_surface_t *surf = render_camera_frame(rw, rh, FALSE);

    char *dir = camera_pictures_dir();
    GDateTime *now = g_date_time_new_now_local();
    char *stamp = g_date_time_format(now, "%Y%m%d_%H%M%S");
    g_date_time_unref(now);
    char *fname = g_strdup_printf("IMG_%s.png", stamp);
    char *full = g_build_filename(dir, fname, NULL);

    cairo_status_t st = cairo_surface_write_to_png(surf, full);
    cairo_surface_destroy(surf);

    if (st == CAIRO_STATUS_SUCCESS) {
        g_free(last_saved_path);
        last_saved_path = g_strdup(full);
        GdkPixbuf *thumb = gdk_pixbuf_new_from_file_at_scale(full, 48, 32, TRUE, NULL);
        if (thumb && thumb_img) {
            gtk_image_set_from_pixbuf(GTK_IMAGE(thumb_img), thumb);
            g_object_unref(thumb);
        }
        char *msg = g_strdup_printf("Saved photo: %s (%dx%d)", full, rw, rh);
        gtk_label_set_text(GTK_LABEL(status_lbl), msg);
        g_free(msg);
    } else {
        gtk_label_set_text(GTK_LABEL(status_lbl), "Failed to save photo.");
    }

    flash_alpha = 0.85;
    char *canb = g_find_program_in_path("canberra-gtk-play");
    if (canb) {
        char *argv[] = { canb, (char *)"-i", (char *)"camera-shutter", NULL };
        g_spawn_async(NULL, argv, NULL, G_SPAWN_DEFAULT, NULL, NULL, NULL, NULL);
        g_free(canb);
    }

    g_free(full);
    g_free(fname);
    g_free(stamp);
    g_free(dir);
}

static gboolean on_countdown_tick(gpointer d)
{
    (void)d;
    countdown_rem--;
    if (countdown_rem <= 0) {
        countdown_rem = 0;
        countdown_timer_id = 0;
        capture_photo_now();
        return G_SOURCE_REMOVE;
    }
    if (viewfinder) gtk_widget_queue_draw(viewfinder);
    return G_SOURCE_CONTINUE;
}

static void on_take_photo(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    if (countdown_timer_id) return;
    int t_idx = gtk_combo_box_get_active(GTK_COMBO_BOX(timer_combo));
    int secs = (t_idx == 1) ? 3 : (t_idx == 2) ? 5 : (t_idx == 3) ? 10 : 0;
    if (secs > 0) {
        countdown_rem = secs;
        countdown_timer_id = g_timeout_add(1000, on_countdown_tick, NULL);
        if (viewfinder) gtk_widget_queue_draw(viewfinder);
    } else {
        capture_photo_now();
    }
}

static void on_record_video(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    if (!is_recording) {
        char *dir = camera_videos_dir();
        GDateTime *now = g_date_time_new_now_local();
        char *stamp = g_date_time_format(now, "%Y%m%d_%H%M%S");
        g_date_time_unref(now);
        char *fname = g_strdup_printf("VID_%s.mp4", stamp);
        g_free(recording_path);
        recording_path = g_build_filename(dir, fname, NULL);
        g_free(fname);
        g_free(stamp);
        g_free(dir);

        const char *dev = gtk_combo_box_get_active_id(GTK_COMBO_BOX(device_combo));
        char *ff = g_find_program_in_path("ffmpeg");
        if (ff) {
            if (dev && g_str_has_prefix(dev, "/dev/video")) {
                char *argv[] = {
                    ff, (char *)"-nostdin", (char *)"-loglevel", (char *)"quiet",
                    (char *)"-y", (char *)"-f", (char *)"v4l2",
                    (char *)"-i", (char *)dev, recording_path, NULL
                };
                g_spawn_async(NULL, argv, NULL, G_SPAWN_DO_NOT_REAP_CHILD, NULL, NULL, &record_pid, NULL);
            } else {
                char *argv[] = {
                    ff, (char *)"-nostdin", (char *)"-loglevel", (char *)"quiet",
                    (char *)"-y", (char *)"-f", (char *)"lavfi",
                    (char *)"-i", (char *)"testsrc=size=1280x720:rate=25",
                    (char *)"-pix_fmt", (char *)"yuv420p", recording_path, NULL
                };
                g_spawn_async(NULL, argv, NULL, G_SPAWN_DO_NOT_REAP_CHILD, NULL, NULL, &record_pid, NULL);
            }
            g_free(ff);
        }
        is_recording = TRUE;
        record_start_us = g_get_monotonic_time();
        gtk_button_set_label(GTK_BUTTON(record_btn), "■ Stop Recording");
        gtk_style_context_add_class(gtk_widget_get_style_context(record_btn), "cam-record-active");
        gtk_label_set_text(GTK_LABEL(status_lbl), "Recording video…");
    } else {
        is_recording = FALSE;
        if (record_pid > 0) {
            kill(record_pid, SIGINT);
            g_spawn_close_pid(record_pid);
            record_pid = 0;
        }
        gtk_button_set_label(GTK_BUTTON(record_btn), "● Record Video");
        gtk_style_context_remove_class(gtk_widget_get_style_context(record_btn), "cam-record-active");
        if (recording_path) {
            g_free(last_saved_path);
            last_saved_path = g_strdup(recording_path);
            char *msg = g_strdup_printf("Saved video: %s", recording_path);
            gtk_label_set_text(GTK_LABEL(status_lbl), msg);
            g_free(msg);
        }
    }
}

static void on_open_last_media(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    if (!last_saved_path || !g_file_test(last_saved_path, G_FILE_TEST_EXISTS)) {
        char *dir = camera_pictures_dir();
        char *argv[] = { (char *)"hde-files", dir, NULL };
        g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
        g_free(dir);
        return;
    }
    char *argv[] = { (char *)"hde-media", last_saved_path, NULL };
    g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
}

static void on_open_folder(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    char *dir = camera_pictures_dir();
    char *argv[] = { (char *)"hde-files", dir, NULL };
    g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
    g_free(dir);
}

static gboolean on_cam_key(GtkWidget *w, GdkEventKey *ev, gpointer d)
{
    (void)w; (void)d;
    if (ev->keyval == GDK_KEY_space || ev->keyval == GDK_KEY_Return) {
        on_take_photo(NULL, NULL);
        return TRUE;
    }
    if (ev->keyval == GDK_KEY_r || ev->keyval == GDK_KEY_R) {
        on_record_video(NULL, NULL);
        return TRUE;
    }
    if (ev->keyval == GDK_KEY_Escape || ev->keyval == GDK_KEY_q) {
        gtk_widget_destroy(main_win);
        return TRUE;
    }
    return FALSE;
}

static gboolean on_anim_tick(gpointer d)
{
    (void)d;
    anim_phase += 0.045;
    if (flash_alpha > 0.0) {
        flash_alpha -= 0.12;
        if (flash_alpha < 0.0) flash_alpha = 0.0;
    }
    static int poll_div;
    if (++poll_div >= 8) {
        poll_div = 0;
        poll_hardware_camera();
    }
    if (viewfinder) gtk_widget_queue_draw(viewfinder);
    return G_SOURCE_CONTINUE;
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--version") || !strcmp(argv[i], "-v")) {
            printf("hde-camera (%s) %s %s\n", APP_TITLE, HDE_RELEASE, HDE_VERSION);
            return 0;
        }
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            printf("Usage: hde-camera\n"
                   "Hyggshi Camera — Photo and video capture application for HDE.\n"
                   "Keys: Space = Take Photo, R = Record Video, Q/Escape = Quit.\n");
            return 0;
        }
    }

    gtk_init(&argc, &argv);
    hde_theme_apply_process();
    load_css();
    hde_theme_watch(on_theme_changed, NULL);

    main_win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(main_win), APP_TITLE);
    gtk_window_set_icon_name(GTK_WINDOW(main_win), "camera-web");
    gtk_window_set_default_size(GTK_WINDOW(main_win), 860, 600);
    g_signal_connect(main_win, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    g_signal_connect(main_win, "key-press-event", G_CALLBACK(on_cam_key), NULL);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(main_win), root);

    /* Top Settings Bar */
    GtkWidget *top_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_style_context_add_class(gtk_widget_get_style_context(top_bar), "cam-bar");

    device_combo = gtk_combo_box_text_new();
    populate_devices();

    res_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(res_combo), "1280×720 (HD)");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(res_combo), "1920×1080 (Full HD)");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(res_combo), "640×480 (VGA)");
    gtk_combo_box_set_active(GTK_COMBO_BOX(res_combo), 0);

    timer_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(timer_combo), "Timer: Off");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(timer_combo), "Timer: 3s");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(timer_combo), "Timer: 5s");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(timer_combo), "Timer: 10s");
    gtk_combo_box_set_active(GTK_COMBO_BOX(timer_combo), 0);

    filter_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(filter_combo), "Filter: Normal");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(filter_combo), "Filter: Grayscale");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(filter_combo), "Filter: Sepia");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(filter_combo), "Filter: Warm");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(filter_combo), "Filter: Cool");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(filter_combo), "Filter: Invert");
    gtk_combo_box_set_active(GTK_COMBO_BOX(filter_combo), 0);

    mirror_toggle = gtk_toggle_button_new_with_label("Mirror");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(mirror_toggle), FALSE);

    grid_toggle = gtk_toggle_button_new_with_label("Grid");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(grid_toggle), TRUE);

    gtk_box_pack_start(GTK_BOX(top_bar), gtk_label_new("Camera:"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(top_bar), device_combo, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(top_bar), res_combo, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(top_bar), timer_combo, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(top_bar), filter_combo, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(top_bar), grid_toggle, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(top_bar), mirror_toggle, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root), top_bar, FALSE, FALSE, 0);

    /* Center Viewfinder */
    viewfinder = gtk_drawing_area_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(viewfinder), "cam-view");
    gtk_widget_set_margin_start(viewfinder, 14);
    gtk_widget_set_margin_end(viewfinder, 14);
    gtk_widget_set_margin_top(viewfinder, 10);
    gtk_widget_set_margin_bottom(viewfinder, 10);
    g_signal_connect(viewfinder, "draw", G_CALLBACK(on_viewfinder_draw), NULL);
    gtk_box_pack_start(GTK_BOX(root), viewfinder, TRUE, TRUE, 0);

    /* Bottom Action Bar */
    GtkWidget *bot_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_style_context_add_class(gtk_widget_get_style_context(bot_bar), "cam-bar");

    GtkWidget *preview_btn = gtk_button_new();
    thumb_img = gtk_image_new_from_icon_name("image-x-generic", GTK_ICON_SIZE_LARGE_TOOLBAR);
    gtk_container_add(GTK_CONTAINER(preview_btn), thumb_img);
    gtk_widget_set_tooltip_text(preview_btn, "Open last captured photo or video in Hyggshi Media");
    g_signal_connect(preview_btn, "clicked", G_CALLBACK(on_open_last_media), NULL);

    status_lbl = gtk_label_new("Ready — Press Space to take a photo or R to record video");
    gtk_label_set_xalign(GTK_LABEL(status_lbl), 0);
    gtk_label_set_ellipsize(GTK_LABEL(status_lbl), PANGO_ELLIPSIZE_MIDDLE);
    gtk_style_context_add_class(gtk_widget_get_style_context(status_lbl), "cam-status");

    shutter_btn = gtk_button_new_with_label("📷 Take Photo");
    gtk_style_context_add_class(gtk_widget_get_style_context(shutter_btn), "cam-shutter");
    g_signal_connect(shutter_btn, "clicked", G_CALLBACK(on_take_photo), NULL);

    record_btn = gtk_button_new_with_label("● Record Video");
    g_signal_connect(record_btn, "clicked", G_CALLBACK(on_record_video), NULL);

    GtkWidget *folder_btn = gtk_button_new_with_label("Open Folder");
    g_signal_connect(folder_btn, "clicked", G_CALLBACK(on_open_folder), NULL);

    gtk_box_pack_start(GTK_BOX(bot_bar), preview_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bot_bar), status_lbl, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(bot_bar), folder_btn, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(bot_bar), record_btn, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(bot_bar), shutter_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root), bot_bar, FALSE, FALSE, 0);

    gtk_widget_show_all(main_win);
    g_timeout_add(33, on_anim_tick, NULL);
    gtk_main();

    if (temp_frame_path) {
        unlink(temp_frame_path);
        g_free(temp_frame_path);
    }
    return 0;
}
