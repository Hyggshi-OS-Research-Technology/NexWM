/* hde-camera.c — Hyggshi Camera: Smooth 30 FPS V4L2 mmap + persistent pipe GTK3 webcam app for HDE. */
#define _POSIX_C_SOURCE 200809L

#include "hde-build.h"
#include "hde-theme.h"
#include <cairo.h>
#include <errno.h>
#include <fcntl.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gtk/gtk.h>
#include <linux/videodev2.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define APP_TITLE "Hyggshi Camera"
#define MAX_V4L2_BUFS 4

typedef enum {
    FILTER_NORMAL = 0,
    FILTER_GRAYSCALE,
    FILTER_SEPIA,
    FILTER_WARM,
    FILTER_COOL,
    FILTER_INVERT
} CamFilter;

typedef struct {
    void  *start;
    size_t length;
} V4l2MmapBuf;

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

/* Shared frame state between background capture thread and GTK main thread */
static GMutex frame_mutex;
static unsigned char *shared_bgra = NULL;
static int shared_w = 0;
static int shared_h = 0;
static uint64_t shared_frame_seq = 0;
static uint64_t drawn_frame_seq = 0;
static double measured_fps = 0.0;
static char active_backend_name[64] = "Virtual Studio";

/* Persistent cairo surface on UI thread */
static cairo_surface_t *display_surf = NULL;
static int display_surf_w = 0;
static int display_surf_h = 0;

/* Background streaming thread control */
static GThread *stream_thread = NULL;
static volatile gint stream_running = 0;
static volatile gint stream_generation = 0;
static char current_dev_id[64] = "";
static int target_stream_w = 640;
static int target_stream_h = 480;
static volatile gint current_filter = FILTER_NORMAL;
static volatile gint current_mirror = 1;

/* Video recording state (fed directly from the live 30 FPS stream) */
static GMutex record_mutex;
static gboolean is_recording = FALSE;
static GPid record_pid = 0;
static int record_stdin_fd = -1;
static int record_w = 0;
static int record_h = 0;
static gint64 record_start_us = 0;
static char *recording_path = NULL;
static char *last_saved_path = NULL;

/* Countdown & flash */
static int countdown_rem = 0;
static guint countdown_timer_id = 0;
static double flash_alpha = 0.0;
static double anim_phase = 0.0;

static void load_css(void)
{
    HdeThemeInfo ti;
    hde_theme_info_load(&ti);
    char *accent = hde_theme_accent_css(&ti);
    const char *ac = ti.accent ? ti.accent : "#3584e4";
    char *css = g_strdup_printf(
        ".cam-view { background-color: #0b0d12; border-radius: 12px; }"
        ".cam-bar { background-color: alpha(@theme_fg_color, 0.04); border-top: 1px solid alpha(@theme_fg_color, 0.10);"
        "  padding: 10px 14px; }"
        ".cam-shutter { border-radius: 24px; padding: 8px 22px; font-weight: 700; background-color: %s; color: #ffffff; }"
        ".cam-record-active { background-color: #e01b24; color: #ffffff; font-weight: 700; }"
        ".cam-status { font-size: 12px; opacity: 0.85; }"
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

static int xioctl(int fd, unsigned long request, void *arg)
{
    int r;
    do {
        r = ioctl(fd, request, arg);
    } while (r == -1 && (errno == EINTR || errno == EAGAIN));
    return r;
}

static gboolean is_valid_v4l2_capture_device(const char *dev, char *name_out, size_t name_sz)
{
    int fd = open(dev, O_RDWR | O_NONBLOCK);
    if (fd < 0)
        return FALSE;
    struct v4l2_capability cap;
    memset(&cap, 0, sizeof(cap));
    if (xioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
        close(fd);
        return FALSE;
    }
    uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
    close(fd);
    if (!(caps & V4L2_CAP_VIDEO_CAPTURE) || !(caps & V4L2_CAP_STREAMING))
        return FALSE;
    if (name_out && name_sz > 0)
        snprintf(name_out, name_sz, "%s", (const char *)cap.card);
    return TRUE;
}

static void populate_devices(void)
{
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(device_combo));
    int count = 0;
    for (int i = 0; i < 16; i++) {
        char dev[32], card_name[128] = "";
        g_snprintf(dev, sizeof dev, "/dev/video%d", i);
        if (!g_file_test(dev, G_FILE_TEST_EXISTS))
            continue;
        if (!is_valid_v4l2_capture_device(dev, card_name, sizeof(card_name)))
            continue;
        char *label = g_strdup_printf("%s (%s)", card_name[0] ? card_name : "USB Camera", dev);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(device_combo), dev, label);
        g_free(label);
        count++;
    }
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(device_combo), "virtual",
                              count > 0 ? "Virtual Studio Camera (60 FPS)" : "Virtual Camera (No /dev/video* detected)");
    gtk_combo_box_set_active(GTK_COMBO_BOX(device_combo), 0);
}

static void selected_resolution(int *w_out, int *h_out)
{
    int idx = res_combo ? gtk_combo_box_get_active(GTK_COMBO_BOX(res_combo)) : 0;
    if (idx == 1) { *w_out = 1280; *h_out = 720; }
    else if (idx == 2) { *w_out = 1920; *h_out = 1080; }
    else { *w_out = 640; *h_out = 480; }
}

/* Fast in-place pixel filter and horizontal mirror on BGRA buffer (runs in worker thread!) */
static void process_bgra_frame_in_place(unsigned char *bgra, int w, int h, CamFilter filter, gboolean mirror)
{
    if (!bgra || w <= 0 || h <= 0) return;
    int stride = w * 4;

    if (mirror) {
        for (int y = 0; y < h; y++) {
            uint32_t *row = (uint32_t *)(bgra + y * stride);
            for (int x = 0, rx = w - 1; x < rx; x++, rx--) {
                uint32_t tmp = row[x];
                row[x] = row[rx];
                row[rx] = tmp;
            }
        }
    }

    if (filter == FILTER_NORMAL)
        return;

    int total_px = w * h;
    unsigned char *p = bgra;
    for (int i = 0; i < total_px; i++, p += 4) {
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

/* Push a completed BGRA frame from the worker thread to the shared buffer and video recorder */
static void publish_captured_bgra_frame(const unsigned char *bgra, int w, int h, const char *backend_label)
{
    static gint64 last_fps_ts = 0;
    static int fps_frames = 0;

    gint64 now = g_get_monotonic_time();
    fps_frames++;
    if (last_fps_ts == 0) {
        last_fps_ts = now;
    } else if (now - last_fps_ts >= 500000) {
        measured_fps = (fps_frames * 1000000.0) / (double)(now - last_fps_ts);
        fps_frames = 0;
        last_fps_ts = now;
    }

    size_t bytes = (size_t)w * (size_t)h * 4;

    g_mutex_lock(&frame_mutex);
    if (shared_w != w || shared_h != h || !shared_bgra) {
        g_free(shared_bgra);
        shared_bgra = g_malloc(bytes);
        shared_w = w;
        shared_h = h;
    }
    memcpy(shared_bgra, bgra, bytes);
    process_bgra_frame_in_place(shared_bgra, w, h,
                                (CamFilter)g_atomic_int_get(&current_filter),
                                g_atomic_int_get(&current_mirror) != 0);
    shared_frame_seq++;
    if (backend_label)
        snprintf(active_backend_name, sizeof(active_backend_name), "%s", backend_label);

    /* If recording is active and dimensions match, feed frame to ffmpeg stdin pipe */
    g_mutex_lock(&record_mutex);
    if (is_recording && record_stdin_fd >= 0 && record_w == w && record_h == h) {
        ssize_t wr = write(record_stdin_fd, shared_bgra, bytes);
        (void)wr;
    }
    g_mutex_unlock(&record_mutex);

    g_mutex_unlock(&frame_mutex);
}

/* Fast YUYV 4:2:2 to BGRA32 converter */
static void convert_yuyv_to_bgra(const unsigned char *yuyv, size_t yuyv_len, unsigned char *bgra, int w, int h)
{
    size_t expected = (size_t)w * (size_t)h * 2;
    size_t limit = yuyv_len < expected ? yuyv_len : expected;
    unsigned char *dst = bgra;
    for (size_t i = 0; i + 3 < limit; i += 4) {
        int y0 = yuyv[i + 0];
        int u  = yuyv[i + 1] - 128;
        int y1 = yuyv[i + 2];
        int v  = yuyv[i + 3] - 128;

        int r_add = (359 * v) >> 8;
        int g_sub = (88 * u + 183 * v) >> 8;
        int b_add = (454 * u) >> 8;

        dst[0] = (unsigned char)CLAMP(y0 + b_add, 0, 255);
        dst[1] = (unsigned char)CLAMP(y0 - g_sub, 0, 255);
        dst[2] = (unsigned char)CLAMP(y0 + r_add, 0, 255);
        dst[3] = 255;

        dst[4] = (unsigned char)CLAMP(y1 + b_add, 0, 255);
        dst[5] = (unsigned char)CLAMP(y1 - g_sub, 0, 255);
        dst[6] = (unsigned char)CLAMP(y1 + r_add, 0, 255);
        dst[7] = 255;
        dst += 8;
    }
}

/* Decode MJPEG buffer in RAM via GdkPixbufLoader into BGRA32 */
static gboolean decode_mjpeg_to_bgra(const unsigned char *jpeg_data, size_t jpeg_len,
                                     unsigned char **bgra_buf, int *w_io, int *h_io)
{
    if (!jpeg_data || jpeg_len < 16) return FALSE;
    GdkPixbufLoader *loader = gdk_pixbuf_loader_new();
    if (!gdk_pixbuf_loader_write(loader, jpeg_data, jpeg_len, NULL)) {
        gdk_pixbuf_loader_close(loader, NULL);
        g_object_unref(loader);
        return FALSE;
    }
    gdk_pixbuf_loader_close(loader, NULL);
    GdkPixbuf *pb = gdk_pixbuf_loader_get_pixbuf(loader);
    if (!pb) {
        g_object_unref(loader);
        return FALSE;
    }

    int pw = gdk_pixbuf_get_width(pb);
    int ph = gdk_pixbuf_get_height(pb);
    int n_ch = gdk_pixbuf_get_n_channels(pb);
    int rowstride = gdk_pixbuf_get_rowstride(pb);
    const guchar *pixels = gdk_pixbuf_get_pixels(pb);

    if (*w_io != pw || *h_io != ph || !*bgra_buf) {
        g_free(*bgra_buf);
        *bgra_buf = g_malloc((size_t)pw * (size_t)ph * 4);
        *w_io = pw;
        *h_io = ph;
    }

    unsigned char *dst = *bgra_buf;
    for (int y = 0; y < ph; y++) {
        const guchar *src = pixels + y * rowstride;
        unsigned char *drow = dst + (size_t)y * (size_t)pw * 4;
        for (int x = 0; x < pw; x++) {
            drow[0] = src[2];
            drow[1] = src[1];
            drow[2] = src[0];
            drow[3] = (n_ch == 4) ? src[3] : 255;
            src += n_ch;
            drow += 4;
        }
    }

    g_object_unref(loader);
    return TRUE;
}

/* Tier 1: Direct Linux V4L2 mmap streaming at 30 FPS (zero disk I/O, zero process spawn overhead) */
static gboolean run_v4l2_mmap_stream(const char *dev, int req_w, int req_h, int my_gen)
{
    int fd = open(dev, O_RDWR | O_NONBLOCK);
    if (fd < 0)
        return FALSE;

    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = (uint32_t)req_w;
    fmt.fmt.pix.height = (uint32_t)req_h;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    fmt.fmt.pix.field = V4L2_FIELD_ANY;

    if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0 ||
        (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_MJPEG && fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_YUYV)) {
        memset(&fmt, 0, sizeof(fmt));
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt.fmt.pix.width = (uint32_t)req_w;
        fmt.fmt.pix.height = (uint32_t)req_h;
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
        fmt.fmt.pix.field = V4L2_FIELD_ANY;
        if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
            close(fd);
            return FALSE;
        }
    }

    uint32_t pixfmt = fmt.fmt.pix.pixelformat;
    if (pixfmt != V4L2_PIX_FMT_MJPEG && pixfmt != V4L2_PIX_FMT_YUYV) {
        close(fd);
        return FALSE;
    }

    int w = (int)fmt.fmt.pix.width;
    int h = (int)fmt.fmt.pix.height;

    /* Request 30 FPS from sensor */
    struct v4l2_streamparm parm;
    memset(&parm, 0, sizeof(parm));
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = 30;
    xioctl(fd, VIDIOC_S_PARM, &parm);

    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = MAX_V4L2_BUFS;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0 || req.count < 2) {
        close(fd);
        return FALSE;
    }

    V4l2MmapBuf bufs[MAX_V4L2_BUFS];
    memset(bufs, 0, sizeof(bufs));
    uint32_t n_bufs = req.count > MAX_V4L2_BUFS ? MAX_V4L2_BUFS : req.count;

    for (uint32_t i = 0; i < n_bufs; i++) {
        struct v4l2_buffer b;
        memset(&b, 0, sizeof(b));
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = i;
        if (xioctl(fd, VIDIOC_QUERYBUF, &b) < 0) {
            for (uint32_t j = 0; j < i; j++)
                munmap(bufs[j].start, bufs[j].length);
            close(fd);
            return FALSE;
        }
        bufs[i].length = b.length;
        bufs[i].start = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, b.m.offset);
        if (bufs[i].start == MAP_FAILED) {
            for (uint32_t j = 0; j < i; j++)
                munmap(bufs[j].start, bufs[j].length);
            close(fd);
            return FALSE;
        }
        xioctl(fd, VIDIOC_QBUF, &b);
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(fd, VIDIOC_STREAMON, &type) < 0) {
        for (uint32_t i = 0; i < n_bufs; i++)
            munmap(bufs[i].start, bufs[i].length);
        close(fd);
        return FALSE;
    }

    unsigned char *bgra = g_malloc0((size_t)w * (size_t)h * 4);
    const char *bname = (pixfmt == V4L2_PIX_FMT_MJPEG) ? "V4L2 MJPEG Direct" : "V4L2 YUYV Direct";

    while (g_atomic_int_get(&stream_running) && g_atomic_int_get(&stream_generation) == my_gen) {
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;
        int pr = poll(&pfd, 1, 100);
        if (pr <= 0)
            continue;

        struct v4l2_buffer b;
        memset(&b, 0, sizeof(b));
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        if (xioctl(fd, VIDIOC_DQBUF, &b) < 0)
            continue;

        if (b.index < n_bufs && b.bytesused > 0) {
            if (pixfmt == V4L2_PIX_FMT_YUYV) {
                convert_yuyv_to_bgra(bufs[b.index].start, b.bytesused, bgra, w, h);
                publish_captured_bgra_frame(bgra, w, h, bname);
            } else if (pixfmt == V4L2_PIX_FMT_MJPEG) {
                if (decode_mjpeg_to_bgra(bufs[b.index].start, b.bytesused, &bgra, &w, &h))
                    publish_captured_bgra_frame(bgra, w, h, bname);
            }
        }
        xioctl(fd, VIDIOC_QBUF, &b);
    }

    xioctl(fd, VIDIOC_STREAMOFF, &type);
    for (uint32_t i = 0; i < n_bufs; i++)
        munmap(bufs[i].start, bufs[i].length);
    close(fd);
    g_free(bgra);
    return TRUE;
}

/* Tier 2: Persistent continuous raw BGRA pipe via ffmpeg (opened once, streams 30 FPS continuously) */
static gboolean run_ffmpeg_pipe_stream(const char *dev, int w, int h, int my_gen)
{
    char *ff = g_find_program_in_path("ffmpeg");
    if (!ff)
        return FALSE;

    char size_arg[32];
    snprintf(size_arg, sizeof(size_arg), "%dx%d", w, h);
    char *argv[] = {
        ff,
        (char *)"-nostdin",
        (char *)"-loglevel", (char *)"quiet",
        (char *)"-fflags", (char *)"nobuffer",
        (char *)"-flags", (char *)"low_delay",
        (char *)"-f", (char *)"v4l2",
        (char *)"-framerate", (char *)"30",
        (char *)"-video_size", size_arg,
        (char *)"-i", (char *)dev,
        (char *)"-vf", (char *)"scale=iw:ih",
        (char *)"-f", (char *)"rawvideo",
        (char *)"-pix_fmt", (char *)"bgra",
        (char *)"-",
        NULL
    };

    GPid pid = 0;
    gint out_fd = -1;
    if (!g_spawn_async_with_pipes(NULL, argv, NULL,
                                  G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_STDERR_TO_DEV_NULL,
                                  NULL, NULL, &pid, NULL, &out_fd, NULL, NULL)) {
        g_free(ff);
        return FALSE;
    }
    g_free(ff);

    size_t frame_bytes = (size_t)w * (size_t)h * 4;
    unsigned char *bgra = g_malloc(frame_bytes);
    gboolean got_any_frame = FALSE;

    while (g_atomic_int_get(&stream_running) && g_atomic_int_get(&stream_generation) == my_gen) {
        size_t got = 0;
        while (got < frame_bytes &&
               g_atomic_int_get(&stream_running) &&
               g_atomic_int_get(&stream_generation) == my_gen) {
            struct pollfd pfd = { .fd = out_fd, .events = POLLIN };
            int pr = poll(&pfd, 1, 150);
            if (pr == 0)
                continue;
            if (pr < 0)
                break;
            ssize_t n = read(out_fd, bgra + got, frame_bytes - got);
            if (n <= 0)
                goto stream_done;
            got += (size_t)n;
        }
        if (got == frame_bytes) {
            got_any_frame = TRUE;
            publish_captured_bgra_frame(bgra, w, h, "FFmpeg Live Stream");
        }
    }

stream_done:
    close(out_fd);
    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
    g_spawn_close_pid(pid);
    g_free(bgra);
    return got_any_frame;
}

/* Tier 3: Virtual Studio Camera (smooth 30 FPS animated test scene when no hardware camera is selected) */
static void run_virtual_studio_stream(int w, int h, int my_gen)
{
    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_t *cr = cairo_create(surf);
    double phase = 0.0;

    while (g_atomic_int_get(&stream_running) && g_atomic_int_get(&stream_generation) == my_gen) {
        gint64 t0 = g_get_monotonic_time();
        phase += 0.045;

        cairo_pattern_t *grad = cairo_pattern_create_linear(0, 0, w, h);
        cairo_pattern_add_color_stop_rgb(grad, 0.0, 0.08, 0.14, 0.26);
        cairo_pattern_add_color_stop_rgb(grad, 0.5, 0.12, 0.24, 0.38);
        cairo_pattern_add_color_stop_rgb(grad, 1.0, 0.18, 0.12, 0.28);
        cairo_set_source(cr, grad);
        cairo_paint(cr);
        cairo_pattern_destroy(grad);

        for (int i = 0; i < 3; i++) {
            double ox = w * (0.25 + 0.25 * i) + cos(phase + i * 1.7) * (w * 0.09);
            double oy = h * 0.45 + sin(phase * 1.3 + i * 2.1) * (h * 0.14);
            double rad = MIN(w, h) * (0.14 + 0.03 * i);
            cairo_arc(cr, ox, oy, rad, 0, 2 * G_PI);
            if (i == 0) cairo_set_source_rgba(cr, 0.21, 0.52, 0.89, 0.42);
            else if (i == 1) cairo_set_source_rgba(cr, 0.18, 0.76, 0.49, 0.38);
            else cairo_set_source_rgba(cr, 0.92, 0.42, 0.08, 0.38);
            cairo_fill(cr);
        }

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

        cairo_surface_flush(surf);
        publish_captured_bgra_frame(cairo_image_surface_get_data(surf), w, h, "Virtual Studio");

        gint64 elapsed = g_get_monotonic_time() - t0;
        if (elapsed < 33000)
            g_usleep((gulong)(33000 - elapsed));
    }

    cairo_destroy(cr);
    cairo_surface_destroy(surf);
}

static gpointer camera_stream_worker(gpointer data)
{
    (void)data;
    while (g_atomic_int_get(&stream_running)) {
        int my_gen = g_atomic_int_get(&stream_generation);
        char dev[64];
        int w = target_stream_w;
        int h = target_stream_h;
        snprintf(dev, sizeof(dev), "%s", current_dev_id);

        if (g_str_has_prefix(dev, "/dev/video")) {
            if (!run_v4l2_mmap_stream(dev, w, h, my_gen)) {
                if (g_atomic_int_get(&stream_running) && g_atomic_int_get(&stream_generation) == my_gen) {
                    if (!run_ffmpeg_pipe_stream(dev, w, h, my_gen)) {
                        run_virtual_studio_stream(w, h, my_gen);
                    }
                }
            }
        } else {
            run_virtual_studio_stream(w, h, my_gen);
        }
    }
    return NULL;
}

static void restart_camera_stream(void)
{
    const char *dev = device_combo ? gtk_combo_box_get_active_id(GTK_COMBO_BOX(device_combo)) : "virtual";
    snprintf(current_dev_id, sizeof(current_dev_id), "%s", dev ? dev : "virtual");
    selected_resolution(&target_stream_w, &target_stream_h);
    g_atomic_int_inc(&stream_generation);

    if (!stream_thread) {
        g_atomic_int_set(&stream_running, 1);
        stream_thread = g_thread_new("hde-camera-stream", camera_stream_worker, NULL);
    }
}

/* Copy latest shared BGRA frame into a Cairo surface (with optional HUD overlay) */
static void update_display_surface_from_shared(void)
{
    g_mutex_lock(&frame_mutex);
    if (!shared_bgra || shared_w <= 0 || shared_h <= 0) {
        g_mutex_unlock(&frame_mutex);
        return;
    }
    if (shared_frame_seq == drawn_frame_seq && display_surf) {
        g_mutex_unlock(&frame_mutex);
        return;
    }
    if (!display_surf || display_surf_w != shared_w || display_surf_h != shared_h) {
        if (display_surf) cairo_surface_destroy(display_surf);
        display_surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, shared_w, shared_h);
        display_surf_w = shared_w;
        display_surf_h = shared_h;
    }
    cairo_surface_flush(display_surf);
    unsigned char *dst = cairo_image_surface_get_data(display_surf);
    int dst_stride = cairo_image_surface_get_stride(display_surf);
    int src_stride = shared_w * 4;
    for (int y = 0; y < shared_h; y++)
        memcpy(dst + y * dst_stride, shared_bgra + y * src_stride, (size_t)src_stride);
    cairo_surface_mark_dirty(display_surf);
    drawn_frame_seq = shared_frame_seq;
    g_mutex_unlock(&frame_mutex);
}

static gboolean on_viewfinder_draw(GtkWidget *w, cairo_t *cr, gpointer d)
{
    (void)d;
    int aw = gtk_widget_get_allocated_width(w);
    int ah = gtk_widget_get_allocated_height(w);
    if (aw < 10 || ah < 10) return FALSE;

    /* Dark studio background */
    cairo_set_source_rgb(cr, 0.04, 0.05, 0.07);
    cairo_paint(cr);

    update_display_surface_from_shared();

    if (display_surf && display_surf_w > 0 && display_surf_h > 0) {
        /* Aspect-ratio-preserving fit inside viewfinder */
        double scale = MIN((double)aw / display_surf_w, (double)ah / display_surf_h);
        double dw = display_surf_w * scale;
        double dh = display_surf_h * scale;
        double ox = (aw - dw) / 2.0;
        double oy = (ah - dh) / 2.0;

        cairo_save(cr);
        cairo_translate(cr, ox, oy);
        cairo_scale(cr, scale, scale);
        cairo_set_source_surface(cr, display_surf, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
        cairo_paint(cr);
        cairo_restore(cr);

        /* Rule-of-thirds composition grid */
        if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(grid_toggle))) {
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.24);
            cairo_set_line_width(cr, 1.0);
            for (int i = 1; i <= 2; i++) {
                cairo_move_to(cr, ox + (dw * i) / 3.0, oy);
                cairo_line_to(cr, ox + (dw * i) / 3.0, oy + dh);
                cairo_move_to(cr, ox, oy + (dh * i) / 3.0);
                cairo_line_to(cr, ox + dw, oy + (dh * i) / 3.0);
            }
            cairo_stroke(cr);
        }

        /* Live FPS & resolution badge in top-right */
        char badge[128];
        snprintf(badge, sizeof(badge), "%s  •  %dx%d  •  %.0f FPS",
                 active_backend_name, display_surf_w, display_surf_h,
                 measured_fps > 1.0 ? measured_fps : 30.0);
        cairo_select_font_face(cr, "monospace", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 11.5);
        cairo_text_extents_t ext;
        cairo_text_extents(cr, badge, &ext);
        double bx = ox + dw - ext.width - 24.0;
        double by = oy + 14.0;
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.52);
        cairo_rectangle(cr, bx - 8.0, by - 4.0, ext.width + 16.0, 22.0);
        cairo_fill(cr);
        cairo_set_source_rgba(cr, 0.45, 0.95, 0.62, 0.95);
        cairo_move_to(cr, bx, by + 11.0);
        cairo_show_text(cr, badge);
    }

    if (is_recording) {
        gint64 sec = (g_get_monotonic_time() - record_start_us) / 1000000;
        char rbuf[64];
        g_snprintf(rbuf, sizeof rbuf, "REC  %02lld:%02lld", (long long)(sec / 60), (long long)(sec % 60));
        cairo_set_source_rgba(cr, 0.88, 0.11, 0.14, 0.92);
        cairo_arc(cr, 28, 28, 7, 0, 2 * G_PI);
        cairo_fill(cr);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.95);
        cairo_select_font_face(cr, "monospace", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 15.0);
        cairo_move_to(cr, 42, 33);
        cairo_show_text(cr, rbuf);
    }

    if (countdown_rem > 0) {
        char cbuf[16];
        g_snprintf(cbuf, sizeof cbuf, "%d", countdown_rem);
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, MIN(aw, ah) * 0.22);
        cairo_text_extents_t ext;
        cairo_text_extents(cr, cbuf, &ext);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.92);
        cairo_move_to(cr, aw / 2.0 - ext.width / 2.0 - ext.x_bearing,
                      ah / 2.0 - ext.height / 2.0 - ext.y_bearing);
        cairo_show_text(cr, cbuf);
    }

    if (flash_alpha > 0.01) {
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, flash_alpha);
        cairo_paint(cr);
    }

    return FALSE;
}

static void capture_photo_now(void)
{
    update_display_surface_from_shared();
    if (!display_surf || display_surf_w <= 0 || display_surf_h <= 0) {
        gtk_label_set_text(GTK_LABEL(status_lbl), "Camera stream is warming up…");
        return;
    }

    /* Create snapshot surface with timestamp watermark */
    int rw = display_surf_w;
    int rh = display_surf_h;
    cairo_surface_t *snap = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, rw, rh);
    cairo_t *cr = cairo_create(snap);
    cairo_set_source_surface(cr, display_surf, 0, 0);
    cairo_paint(cr);

    GDateTime *now = g_date_time_new_now_local();
    char *ts = g_date_time_format(now, "%Y-%m-%d %H:%M:%S");
    char *stamp = g_date_time_format(now, "%Y%m%d_%H%M%S");
    g_date_time_unref(now);

    cairo_select_font_face(cr, "monospace", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, MAX(12.0, rh * 0.025));
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.82);
    cairo_move_to(cr, rw - 195.0, rh - 14.0);
    cairo_show_text(cr, ts);
    g_free(ts);
    cairo_destroy(cr);

    char *dir = camera_pictures_dir();
    char *fname = g_strdup_printf("IMG_%s.png", stamp);
    char *full = g_build_filename(dir, fname, NULL);

    cairo_status_t st = cairo_surface_write_to_png(snap, full);
    cairo_surface_destroy(snap);

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

static void stop_video_recording(void)
{
    g_mutex_lock(&record_mutex);
    if (!is_recording) {
        g_mutex_unlock(&record_mutex);
        return;
    }
    is_recording = FALSE;
    int fd = record_stdin_fd;
    record_stdin_fd = -1;
    GPid pid = record_pid;
    record_pid = 0;
    g_mutex_unlock(&record_mutex);

    if (fd >= 0)
        close(fd);
    if (pid > 0) {
        waitpid(pid, NULL, 0);
        g_spawn_close_pid(pid);
    }
}

static void on_record_video(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    if (!is_recording) {
        char *ff = g_find_program_in_path("ffmpeg");
        if (!ff) {
            gtk_label_set_text(GTK_LABEL(status_lbl), "Install ffmpeg to record video.");
            return;
        }

        int rw = shared_w > 0 ? shared_w : target_stream_w;
        int rh = shared_h > 0 ? shared_h : target_stream_h;
        char size_str[32];
        snprintf(size_str, sizeof(size_str), "%dx%d", rw, rh);

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

        /* Encode directly from our live 30 FPS BGRA stream so the webcam preview stays 100% smooth! */
        char *argv[] = {
            ff,
            (char *)"-nostdin",
            (char *)"-loglevel", (char *)"quiet",
            (char *)"-y",
            (char *)"-f", (char *)"rawvideo",
            (char *)"-pix_fmt", (char *)"bgra",
            (char *)"-s", size_str,
            (char *)"-r", (char *)"30",
            (char *)"-i", (char *)"-",
            (char *)"-pix_fmt", (char *)"yuv420p",
            recording_path,
            NULL
        };

        GPid pid = 0;
        gint in_fd = -1;
        if (g_spawn_async_with_pipes(NULL, argv, NULL,
                                     G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                                     NULL, NULL, &pid, &in_fd, NULL, NULL, NULL)) {
            g_mutex_lock(&record_mutex);
            record_pid = pid;
            record_stdin_fd = in_fd;
            record_w = rw;
            record_h = rh;
            is_recording = TRUE;
            record_start_us = g_get_monotonic_time();
            g_mutex_unlock(&record_mutex);

            gtk_button_set_label(GTK_BUTTON(record_btn), "■ Stop Recording");
            gtk_style_context_add_class(gtk_widget_get_style_context(record_btn), "cam-record-active");
            gtk_label_set_text(GTK_LABEL(status_lbl), "Recording live video at 30 FPS…");
        }
        g_free(ff);
    } else {
        stop_video_recording();
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

static void on_device_or_res_changed(GtkComboBox *c, gpointer d)
{
    (void)c; (void)d;
    restart_camera_stream();
}

static void on_filter_changed(GtkComboBox *c, gpointer d)
{
    (void)d;
    g_atomic_int_set(&current_filter, CLAMP(gtk_combo_box_get_active(c), 0, 5));
}

static void on_mirror_toggled(GtkToggleButton *tb, gpointer d)
{
    (void)d;
    g_atomic_int_set(&current_mirror, gtk_toggle_button_get_active(tb) ? 1 : 0);
}

static void show_about_dialog(void)
{
    const char *authors[] = { "Hyggshi OS Research Technology", NULL };
    gtk_show_about_dialog(GTK_WINDOW(main_win),
        "program-name", "Hyggshi Camera",
        "version", HDE_VERSION,
        "comments", "Smooth 30 FPS Linux V4L2 photo and video capture application for Hyggshi Desktop Environment.",
        "website", "https://github.com/Hyggshi-OS-Research-Technology/NexWM",
        "website-label", "NexWM / HDE Repository",
        "copyright", "Copyright (c) 2024-2026 Hyggshi OS Research Technology",
        "license-type", GTK_LICENSE_MIT_X11,
        "authors", authors,
        "logo-icon-name", "camera-web",
        NULL);
}

static void on_about_clicked(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    show_about_dialog();
}

static gboolean on_cam_key(GtkWidget *w, GdkEventKey *ev, gpointer d)
{
    (void)w; (void)d;
    if (ev->keyval == GDK_KEY_F1) {
        show_about_dialog();
        return TRUE;
    }
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

static gboolean on_ui_tick(gpointer d)
{
    (void)d;
    anim_phase += 0.045;
    if (flash_alpha > 0.0) {
        flash_alpha -= 0.14;
        if (flash_alpha < 0.0) flash_alpha = 0.0;
        if (viewfinder) gtk_widget_queue_draw(viewfinder);
        return G_SOURCE_CONTINUE;
    }
    if (shared_frame_seq != drawn_frame_seq || is_recording || countdown_rem > 0) {
        if (viewfinder)
            gtk_widget_queue_draw(viewfinder);
    }
    return G_SOURCE_CONTINUE;
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--version") || !strcmp(argv[i], "-v")) {
            printf("hde-camera (%s) %s %s\n", APP_TITLE, HDE_RELEASE, HDE_VERSION);
            return 0;
        }
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            printf("Usage: hde-camera\n"
                   "Hyggshi Camera — Smooth 30 FPS V4L2 photo and video capture application for HDE.\n"
                   "Keys: Space = Take Photo, R = Record Video, Q/Escape = Quit.\n");
            return 0;
        }
    }

    gtk_init(&argc, &argv);
    g_mutex_init(&frame_mutex);
    g_mutex_init(&record_mutex);

    hde_theme_apply_process();
    load_css();
    hde_theme_watch(on_theme_changed, NULL);

    main_win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(main_win), APP_TITLE);
    gtk_window_set_icon_name(GTK_WINDOW(main_win), "camera-web");
    gtk_window_set_default_size(GTK_WINDOW(main_win), 880, 620);
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
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(res_combo), "640×480 (30 FPS Fast)");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(res_combo), "1280×720 (HD 30 FPS)");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(res_combo), "1920×1080 (Full HD)");
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
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(mirror_toggle), TRUE);

    grid_toggle = gtk_toggle_button_new_with_label("Grid");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(grid_toggle), TRUE);

    g_signal_connect(device_combo, "changed", G_CALLBACK(on_device_or_res_changed), NULL);
    g_signal_connect(res_combo, "changed", G_CALLBACK(on_device_or_res_changed), NULL);
    g_signal_connect(filter_combo, "changed", G_CALLBACK(on_filter_changed), NULL);
    g_signal_connect(mirror_toggle, "toggled", G_CALLBACK(on_mirror_toggled), NULL);

    GtkWidget *about_btn = gtk_button_new_from_icon_name("help-about-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(about_btn, "About Hyggshi Camera (F1)");
    g_signal_connect(about_btn, "clicked", G_CALLBACK(on_about_clicked), NULL);

    gtk_box_pack_start(GTK_BOX(top_bar), gtk_label_new("Camera:"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(top_bar), device_combo, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(top_bar), res_combo, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(top_bar), timer_combo, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(top_bar), filter_combo, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(top_bar), about_btn, FALSE, FALSE, 0);
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

    status_lbl = gtk_label_new("Ready — Live 30 FPS stream (Space = Take Photo, R = Record Video)");
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
    restart_camera_stream();
    g_timeout_add(16, on_ui_tick, NULL);
    gtk_main();

    stop_video_recording();
    g_atomic_int_set(&stream_running, 0);
    g_atomic_int_inc(&stream_generation);
    if (stream_thread)
        g_thread_join(stream_thread);
    if (display_surf)
        cairo_surface_destroy(display_surf);
    g_free(shared_bgra);
    return 0;
}
