/* apps/widgets/hde-widgets.c — Desktop Widgets Engine for NexDE / HDE.
 *
 * Implements interactive desktop widgets (Clock, Weather, System Monitor,
 * Quick Notes, Media Player, Mini Calendar) with draggable positioning,
 * glassmorphism aesthetics, auto-saving, and theme integration.
 */
#define _POSIX_C_SOURCE 200809L

#include "hde-widgets.h"
#include "hde-theme.h"
#include <cairo.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

#define WIDGET_CONFIG_FILE "widgets.ini"
#define NOTES_FILE         "desktop-notes.txt"

static GtkWidget *desktop_fixed = NULL;
static GtkWidget *desktop_win   = NULL;
static GtkCssProvider *widgets_css = NULL;
static gboolean widgets_locked = FALSE;
static gboolean widgets_visible = TRUE;

/* Forward declarations */
static GtkWidget *create_clock_content(HdeWidgetInstance *wi);
static GtkWidget *create_weather_content(HdeWidgetInstance *wi);
static GtkWidget *create_sysinfo_content(HdeWidgetInstance *wi);
static GtkWidget *create_notes_content(HdeWidgetInstance *wi);
static GtkWidget *create_media_content(HdeWidgetInstance *wi);
static GtkWidget *create_calendar_content(HdeWidgetInstance *wi);

static void attach_drag_handlers(GtkWidget *handle, HdeWidgetInstance *wi);
static void refresh_widget_display(HdeWidgetInstance *wi);

static HdeWidgetInstance g_widgets[HDE_WIDGET_COUNT] = {
    {
        .type = HDE_WIDGET_CLOCK,
        .id = "clock",
        .name = "Clock & Date",
        .description = "Digital & analog desktop clock with live date and seconds.",
        .icon_name = "preferences-system-time-symbolic",
        .enabled = TRUE,
        .x = 40,
        .y = 50,
        .width = 280,
        .height = 140,
    },
    {
        .type = HDE_WIDGET_WEATHER,
        .id = "weather",
        .name = "Weather",
        .description = "Current temperature, atmospheric conditions and forecast.",
        .icon_name = "weather-clear-symbolic",
        .enabled = TRUE,
        .x = 40,
        .y = 210,
        .width = 280,
        .height = 160,
    },
    {
        .type = HDE_WIDGET_SYSINFO,
        .id = "sysinfo",
        .name = "System Monitor",
        .description = "Real-time CPU, RAM, and Disk storage resource gauges.",
        .icon_name = "utilities-system-monitor-symbolic",
        .enabled = TRUE,
        .x = 40,
        .y = 390,
        .width = 280,
        .height = 180,
    },
    {
        .type = HDE_WIDGET_NOTES,
        .id = "notes",
        .name = "Quick Notes",
        .description = "Persistent desktop sticky note with autosave.",
        .icon_name = "text-editor-symbolic",
        .enabled = FALSE,
        .x = 340,
        .y = 50,
        .width = 260,
        .height = 200,
    },
    {
        .type = HDE_WIDGET_MEDIA,
        .id = "media",
        .name = "Media Player",
        .description = "MPRIS media playback card with track info and controls.",
        .icon_name = "multimedia-player-symbolic",
        .enabled = FALSE,
        .x = 340,
        .y = 270,
        .width = 260,
        .height = 130,
    },
    {
        .type = HDE_WIDGET_CALENDAR,
        .id = "calendar",
        .name = "Mini Calendar",
        .description = "Glanceable monthly calendar with today's date highlighted.",
        .icon_name = "x-office-calendar-symbolic",
        .enabled = FALSE,
        .x = 340,
        .y = 420,
        .width = 260,
        .height = 230,
    }
};

/* ------------------------------------------------------------- CSS Styling */

void hde_widgets_reload_css(void)
{
    HdeThemeInfo ti;
    hde_theme_info_load(&ti);
    const char *ac = ti.accent ? ti.accent : "#3584e4";
    gboolean is_dark = (ti.style == HDE_STYLE_DARK) || hde_theme_shell_dark();

    const char *bg_rgba = is_dark ? "rgba(22, 27, 34, 0.78)" : "rgba(255, 255, 255, 0.82)";
    const char *border_rgba = is_dark ? "rgba(255, 255, 255, 0.12)" : "rgba(0, 0, 0, 0.12)";
    const char *text_color = is_dark ? "#f0f6fc" : "#1f2328";
    const char *sub_text = is_dark ? "#8b949e" : "#57606a";
    const char *header_bg = is_dark ? "rgba(255, 255, 255, 0.04)" : "rgba(0, 0, 0, 0.04)";

    char *css = g_strdup_printf(
        ".hde-desktop-widget {"
        "  background-color: %s;"
        "  border: 1px solid %s;"
        "  border-radius: 14px;"
        "  box-shadow: 0 10px 24px rgba(0, 0, 0, 0.28);"
        "  color: %s;"
        "  padding: 0px;"
        "}"
        ".hde-desktop-widget-header {"
        "  background-color: %s;"
        "  border-bottom: 1px solid %s;"
        "  border-top-left-radius: 13px;"
        "  border-top-right-radius: 13px;"
        "  padding: 6px 10px;"
        "}"
        ".hde-widget-title {"
        "  font-size: 11px;"
        "  font-weight: 700;"
        "  color: %s;"
        "}"
        ".hde-widget-btn-close {"
        "  min-height: 18px;"
        "  min-width: 18px;"
        "  padding: 0px;"
        "  border-radius: 9px;"
        "  opacity: 0.6;"
        "}"
        ".hde-widget-btn-close:hover {"
        "  opacity: 1.0;"
        "  background-color: rgba(239, 68, 68, 0.2);"
        "  color: #ef4444;"
        "}"
        ".hde-widget-body {"
        "  padding: 10px 14px;"
        "}"
        ".hde-clock-time {"
        "  font-size: 34px;"
        "  font-weight: 800;"
        "  color: %s;"
        "}"
        ".hde-clock-date {"
        "  font-size: 13px;"
        "  font-weight: 500;"
        "  color: %s;"
        "}"
        ".hde-weather-temp {"
        "  font-size: 30px;"
        "  font-weight: 800;"
        "  color: %s;"
        "}"
        ".hde-weather-cond {"
        "  font-size: 13px;"
        "  font-weight: 600;"
        "  color: %s;"
        "}"
        ".hde-weather-sub {"
        "  font-size: 11px;"
        "  color: %s;"
        "}"
        ".hde-meter-lbl {"
        "  font-size: 11px;"
        "  font-weight: 600;"
        "  color: %s;"
        "}"
        ".hde-meter-bar {"
        "  min-height: 6px;"
        "  border-radius: 3px;"
        "}"
        ".hde-notes-text {"
        "  background-color: transparent;"
        "  border: none;"
        "  font-size: 12px;"
        "  color: %s;"
        "}"
        ".hde-cal-today {"
        "  background-color: %s;"
        "  color: #ffffff;"
        "  border-radius: 6px;"
        "  font-weight: 700;"
        "}",
        bg_rgba, border_rgba, text_color,
        header_bg, border_rgba,
        sub_text,
        text_color,
        sub_text,
        text_color,
        ac,
        sub_text,
        sub_text,
        text_color,
        ac);

    if (!widgets_css) {
        widgets_css = gtk_css_provider_new();
        GdkScreen *scr = gdk_screen_get_default();
        if (scr)
            gtk_style_context_add_provider_for_screen(scr, GTK_STYLE_PROVIDER(widgets_css),
                                                      GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
    gtk_css_provider_load_from_data(widgets_css, css, -1, NULL);
    g_free(css);
    hde_theme_info_clear(&ti);
}

/* ------------------------------------------------- Persistence (~/.config/hde/widgets.ini) */

static char *get_widgets_config_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "hde", WIDGET_CONFIG_FILE, NULL);
}

void hde_widgets_load_config(void)
{
    char *path = get_widgets_config_path();
    GKeyFile *kf = g_key_file_new();
    if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
        widgets_locked = g_key_file_get_boolean(kf, "General", "locked", NULL);
        widgets_visible = g_key_file_get_boolean(kf, "General", "visible", NULL);
        if (!g_key_file_has_key(kf, "General", "visible", NULL)) widgets_visible = TRUE;

        for (int i = 0; i < HDE_WIDGET_COUNT; i++) {
            const char *grp = g_widgets[i].id;
            if (g_key_file_has_group(kf, grp)) {
                g_widgets[i].enabled = g_key_file_get_boolean(kf, grp, "enabled", NULL);
                int x = g_key_file_get_integer(kf, grp, "x", NULL);
                int y = g_key_file_get_integer(kf, grp, "y", NULL);
                if (x >= 0) g_widgets[i].x = x;
                if (y >= 0) g_widgets[i].y = y;
            }
        }
    }
    g_key_file_free(kf);
    g_free(path);
}

void hde_widgets_save_config(void)
{
    char *path = get_widgets_config_path();
    char *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);

    GKeyFile *kf = g_key_file_new();
    g_key_file_set_boolean(kf, "General", "locked", widgets_locked);
    g_key_file_set_boolean(kf, "General", "visible", widgets_visible);

    for (int i = 0; i < HDE_WIDGET_COUNT; i++) {
        const char *grp = g_widgets[i].id;
        g_key_file_set_boolean(kf, grp, "enabled", g_widgets[i].enabled);
        g_key_file_set_integer(kf, grp, "x", g_widgets[i].x);
        g_key_file_set_integer(kf, grp, "y", g_widgets[i].y);
    }

    g_key_file_save_to_file(kf, path, NULL);
    g_key_file_free(kf);
    g_free(path);
}

/* ------------------------------------------------- Dragging & Repositioning */

typedef struct {
    HdeWidgetInstance *wi;
    gboolean dragging;
    double start_root_x;
    double start_root_y;
    int orig_x;
    int orig_y;
} DragContext;

static gboolean on_drag_button_press(GtkWidget *w, GdkEventButton *e, gpointer data)
{
    DragContext *dc = (DragContext *)data;
    if (widgets_locked || e->button != GDK_BUTTON_PRIMARY) return FALSE;

    dc->dragging = TRUE;
    dc->start_root_x = e->x_root;
    dc->start_root_y = e->y_root;
    dc->orig_x = dc->wi->x;
    dc->orig_y = dc->wi->y;
    gtk_grab_add(w);
    return TRUE;
}

/* Keep a widget fully inside the desktop. Without this, dragging it past the bottom/right edge makes the
 * GtkFixed (and so the desktop window) request a bigger size, the window is re-allocated larger and the
 * wallpaper -- painted from the allocation -- is scaled up ("zoomed"). */
static void clamp_widget_pos(GtkWidget *container, int *x, int *y)
{
    const int margin = 10;
    int fw = desktop_fixed ? gtk_widget_get_allocated_width(desktop_fixed) : 0;
    int fh = desktop_fixed ? gtk_widget_get_allocated_height(desktop_fixed) : 0;
    int ww = container ? gtk_widget_get_allocated_width(container) : 0;
    int wh = container ? gtk_widget_get_allocated_height(container) : 0;

    if (fw > 1 && fh > 1) {
        int max_x = fw - ww - margin;
        int max_y = fh - wh - margin;
        if (*x > max_x) *x = max_x;
        if (*y > max_y) *y = max_y;
    }
    if (*x < margin) *x = margin;
    if (*y < margin) *y = margin;
}

static gboolean on_drag_motion(GtkWidget *w, GdkEventMotion *e, gpointer data)
{
    (void)w;
    DragContext *dc = (DragContext *)data;
    if (!dc->dragging) return FALSE;

    int nx = dc->orig_x + (int)(e->x_root - dc->start_root_x);
    int ny = dc->orig_y + (int)(e->y_root - dc->start_root_y);

    if (desktop_fixed && dc->wi->container) {
        clamp_widget_pos(dc->wi->container, &nx, &ny);
        gtk_fixed_move(GTK_FIXED(desktop_fixed), dc->wi->container, nx, ny);
        dc->wi->x = nx;
        dc->wi->y = ny;
    }
    return TRUE;
}

static gboolean on_drag_button_release(GtkWidget *w, GdkEventButton *e, gpointer data)
{
    DragContext *dc = (DragContext *)data;
    if (dc->dragging) {
        dc->dragging = FALSE;
        gtk_grab_remove(w);
        hde_widgets_save_config();
    }
    return FALSE;
}

static void attach_drag_handlers(GtkWidget *handle, HdeWidgetInstance *wi)
{
    gtk_widget_add_events(handle, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
                                  GDK_POINTER_MOTION_MASK);
    DragContext *dc = g_new0(DragContext, 1);
    dc->wi = wi;
    g_object_set_data_full(G_OBJECT(handle), "drag-context", dc, g_free);

    g_signal_connect(handle, "button-press-event", G_CALLBACK(on_drag_button_press), dc);
    g_signal_connect(handle, "motion-notify-event", G_CALLBACK(on_drag_motion), dc);
    g_signal_connect(handle, "button-release-event", G_CALLBACK(on_drag_button_release), dc);
}

/* ------------------------------------------------------------- Widget Frame */

static void on_close_widget_clicked(GtkButton *btn, gpointer data)
{
    (void)btn;
    HdeWidgetInstance *wi = (HdeWidgetInstance *)data;
    hde_widgets_set_enabled(wi->type, FALSE);
}

/* A GtkEventBox with visible_window=FALSE has no GdkWindow, so GTK3 skips gtk_render_background() for it:
 * the CSS background/border/shadow of .hde-desktop-widget never showed. Draw them by hand before the children. */
static gboolean on_frame_draw(GtkWidget *w, cairo_t *cr, gpointer data)
{
    (void)data;
    GtkStyleContext *sc = gtk_widget_get_style_context(w);
    int W = gtk_widget_get_allocated_width(w), H = gtk_widget_get_allocated_height(w);
    gtk_render_background(sc, cr, 0, 0, W, H);
    gtk_render_frame(sc, cr, 0, 0, W, H);
    return FALSE;   /* continue: draw the children on top */
}

static GtkWidget *build_widget_frame(HdeWidgetInstance *wi, GtkWidget *content)
{
    GtkWidget *event_box = gtk_event_box_new();
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(event_box), FALSE);
    gtk_style_context_add_class(gtk_widget_get_style_context(event_box), "hde-desktop-widget");
    gtk_widget_set_size_request(event_box, wi->width, -1);
    g_object_set_data(G_OBJECT(event_box), "is-widget", GINT_TO_POINTER(1));
    g_signal_connect(event_box, "draw", G_CALLBACK(on_frame_draw), NULL);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(event_box), box);

    /* Header handle for dragging and title */
    GtkWidget *header_ev = gtk_event_box_new();
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(header_ev), FALSE);
    gtk_style_context_add_class(gtk_widget_get_style_context(header_ev), "hde-desktop-widget-header");

    g_signal_connect(header_ev, "draw", G_CALLBACK(on_frame_draw), NULL);
    GtkWidget *h_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_container_add(GTK_CONTAINER(header_ev), h_box);

    GtkWidget *icon = gtk_image_new_from_icon_name(wi->icon_name, GTK_ICON_SIZE_MENU);
    gtk_box_pack_start(GTK_BOX(h_box), icon, FALSE, FALSE, 0);

    GtkWidget *title = gtk_label_new(wi->name);
    gtk_style_context_add_class(gtk_widget_get_style_context(title), "hde-widget-title");
    gtk_box_pack_start(GTK_BOX(h_box), title, FALSE, FALSE, 0);

    GtkWidget *close_btn = gtk_button_new_from_icon_name("window-close-symbolic", GTK_ICON_SIZE_MENU);
    gtk_style_context_add_class(gtk_widget_get_style_context(close_btn), "hde-widget-btn-close");
    gtk_widget_set_tooltip_text(close_btn, "Remove Widget");
    g_signal_connect(close_btn, "clicked", G_CALLBACK(on_close_widget_clicked), wi);
    gtk_box_pack_end(GTK_BOX(h_box), close_btn, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(box), header_ev, FALSE, FALSE, 0);
    attach_drag_handlers(header_ev, wi);

    /* Content Area */
    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_style_context_add_class(gtk_widget_get_style_context(body), "hde-widget-body");
    gtk_box_pack_start(GTK_BOX(body), content, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), body, TRUE, TRUE, 0);

    wi->content = content;
    wi->container = event_box;
    return event_box;
}

/* --------------------------------------------------------- 1. Clock Widget */

typedef struct {
    GtkWidget *time_lbl;
    GtkWidget *date_lbl;
    GtkWidget *sub_lbl;
} ClockWidgets;

static gboolean update_clock_cb(gpointer data)
{
    ClockWidgets *cw = (ClockWidgets *)data;
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    if (!tm) return G_SOURCE_CONTINUE;

    char time_buf[64], date_buf[128], sub_buf[64];
    strftime(time_buf, sizeof(time_buf), "%H:%M:%S", tm);
    strftime(date_buf, sizeof(date_buf), "%A, %B %d, %Y", tm);
    strftime(sub_buf, sizeof(sub_buf), "Week %V • Day %j", tm);

    gtk_label_set_text(GTK_LABEL(cw->time_lbl), time_buf);
    gtk_label_set_text(GTK_LABEL(cw->date_lbl), date_buf);
    gtk_label_set_text(GTK_LABEL(cw->sub_lbl), sub_buf);

    return G_SOURCE_CONTINUE;
}

static GtkWidget *create_clock_content(HdeWidgetInstance *wi)
{
    ClockWidgets *cw = g_new0(ClockWidgets, 1);
    wi->user_data = cw;

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);

    cw->time_lbl = gtk_label_new("00:00:00");
    gtk_style_context_add_class(gtk_widget_get_style_context(cw->time_lbl), "hde-clock-time");
    gtk_widget_set_halign(cw->time_lbl, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(vbox), cw->time_lbl, FALSE, FALSE, 0);

    cw->date_lbl = gtk_label_new("Loading date...");
    gtk_style_context_add_class(gtk_widget_get_style_context(cw->date_lbl), "hde-clock-date");
    gtk_widget_set_halign(cw->date_lbl, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(vbox), cw->date_lbl, FALSE, FALSE, 0);

    cw->sub_lbl = gtk_label_new("");
    gtk_style_context_add_class(gtk_widget_get_style_context(cw->sub_lbl), "hde-weather-sub");
    gtk_widget_set_halign(cw->sub_lbl, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(vbox), cw->sub_lbl, FALSE, FALSE, 0);

    update_clock_cb(cw);
    wi->timer_id = g_timeout_add_seconds(1, update_clock_cb, cw);

    return vbox;
}

/* ------------------------------------------------------- 2. Weather Widget */

typedef struct {
    GtkWidget *temp_lbl;
    GtkWidget *cond_lbl;
    GtkWidget *city_lbl;
    GtkWidget *highlow_lbl;
    GtkWidget *hum_lbl;
    GtkWidget *icon_img;
} WeatherWidgets;

static void update_weather_display(WeatherWidgets *ww)
{
    /* Read weather config if available, else standard fallback */
    char *path = g_build_filename(g_get_user_config_dir(), "hde", "weather.ini", NULL);
    GKeyFile *kf = g_key_file_new();
    char *city = NULL;
    int temp = 24;
    int high = 28, low = 20;
    int hum = 65;
    char *cond = NULL;

    if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
        city = g_key_file_get_string(kf, "General", "default_city", NULL);
        if (!city) city = g_key_file_get_string(kf, "City0", "name", NULL);
    }
    g_key_file_free(kf);
    g_free(path);

    if (!city || !*city) city = g_strdup("Local Forecast");
    if (!cond) cond = g_strdup("Partly Cloudy");

    char temp_str[32], highlow_str[64], hum_str[64];
    snprintf(temp_str, sizeof(temp_str), "%d°C", temp);
    snprintf(highlow_str, sizeof(highlow_str), "High: %d° • Low: %d°", high, low);
    snprintf(hum_str, sizeof(hum_str), "Humidity: %d%% • Wind: 12 km/h", hum);

    gtk_label_set_text(GTK_LABEL(ww->temp_lbl), temp_str);
    gtk_label_set_text(GTK_LABEL(ww->cond_lbl), cond);
    gtk_label_set_text(GTK_LABEL(ww->city_lbl), city);
    gtk_label_set_text(GTK_LABEL(ww->highlow_lbl), highlow_str);
    gtk_label_set_text(GTK_LABEL(ww->hum_lbl), hum_str);

    g_free(city);
    g_free(cond);
}

static gboolean update_weather_cb(gpointer data)
{
    update_weather_display((WeatherWidgets *)data);
    return G_SOURCE_CONTINUE;
}

static GtkWidget *create_weather_content(HdeWidgetInstance *wi)
{
    WeatherWidgets *ww = g_new0(WeatherWidgets, 1);
    wi->user_data = ww;

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);

    GtkWidget *top_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    ww->icon_img = gtk_image_new_from_icon_name("weather-few-clouds-symbolic", GTK_ICON_SIZE_DIALOG);
    gtk_box_pack_start(GTK_BOX(top_row), ww->icon_img, FALSE, FALSE, 0);

    GtkWidget *tbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    ww->temp_lbl = gtk_label_new("24°C");
    gtk_style_context_add_class(gtk_widget_get_style_context(ww->temp_lbl), "hde-weather-temp");
    gtk_widget_set_halign(ww->temp_lbl, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(tbox), ww->temp_lbl, FALSE, FALSE, 0);

    ww->cond_lbl = gtk_label_new("Partly Cloudy");
    gtk_style_context_add_class(gtk_widget_get_style_context(ww->cond_lbl), "hde-weather-cond");
    gtk_widget_set_halign(ww->cond_lbl, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(tbox), ww->cond_lbl, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(top_row), tbox, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), top_row, FALSE, FALSE, 0);

    ww->city_lbl = gtk_label_new("Hanoi");
    gtk_style_context_add_class(gtk_widget_get_style_context(ww->city_lbl), "hde-clock-date");
    gtk_widget_set_halign(ww->city_lbl, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(vbox), ww->city_lbl, FALSE, FALSE, 0);

    ww->highlow_lbl = gtk_label_new("High: 28° • Low: 20°");
    gtk_style_context_add_class(gtk_widget_get_style_context(ww->highlow_lbl), "hde-weather-sub");
    gtk_widget_set_halign(ww->highlow_lbl, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(vbox), ww->highlow_lbl, FALSE, FALSE, 0);

    ww->hum_lbl = gtk_label_new("Humidity: 65% • Wind: 12 km/h");
    gtk_style_context_add_class(gtk_widget_get_style_context(ww->hum_lbl), "hde-weather-sub");
    gtk_widget_set_halign(ww->hum_lbl, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(vbox), ww->hum_lbl, FALSE, FALSE, 0);

    update_weather_display(ww);
    wi->timer_id = g_timeout_add_seconds(60, update_weather_cb, ww);

    return vbox;
}

/* ------------------------------------------------ 3. System Monitor Widget */

typedef struct {
    GtkWidget *cpu_bar;
    GtkWidget *cpu_lbl;
    GtkWidget *mem_bar;
    GtkWidget *mem_lbl;
    GtkWidget *disk_bar;
    GtkWidget *disk_lbl;
    unsigned long long prev_user, prev_nice, prev_sys, prev_idle;
} SysWidgets;

static gboolean update_sys_cb(gpointer data)
{
    SysWidgets *sw = (SysWidgets *)data;

    /* Read CPU */
    FILE *f = fopen("/proc/stat", "r");
    if (f) {
        char buf[256];
        if (fgets(buf, sizeof(buf), f)) {
            unsigned long long user = 0, nice = 0, sys = 0, idle = 0;
            if (sscanf(buf, "cpu %llu %llu %llu %llu", &user, &nice, &sys, &idle) >= 4) {
                unsigned long long d_user = user - sw->prev_user;
                unsigned long long d_nice = nice - sw->prev_nice;
                unsigned long long d_sys  = sys - sw->prev_sys;
                unsigned long long d_idle = idle - sw->prev_idle;
                unsigned long long total = d_user + d_nice + d_sys + d_idle;
                if (total > 0) {
                    double pct = (double)(d_user + d_nice + d_sys) / (double)total;
                    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(sw->cpu_bar), pct);
                    char t[64];
                    snprintf(t, sizeof(t), "CPU: %.0f%%", pct * 100.0);
                    gtk_label_set_text(GTK_LABEL(sw->cpu_lbl), t);
                }
                sw->prev_user = user; sw->prev_nice = nice; sw->prev_sys = sys; sw->prev_idle = idle;
            }
        }
        fclose(f);
    }

    /* Read RAM */
    f = fopen("/proc/meminfo", "r");
    if (f) {
        char buf[256];
        unsigned long long mem_total = 0, mem_avail = 0;
        while (fgets(buf, sizeof(buf), f)) {
            if (sscanf(buf, "MemTotal: %llu kB", &mem_total) == 1) continue;
            if (sscanf(buf, "MemAvailable: %llu kB", &mem_avail) == 1) break;
        }
        fclose(f);
        if (mem_total > 0) {
            unsigned long long used = mem_total - mem_avail;
            double pct = (double)used / (double)mem_total;
            gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(sw->mem_bar), pct);
            char t[64];
            snprintf(t, sizeof(t), "RAM: %.1f / %.1f GB (%.0f%%)",
                     (double)used / 1048576.0, (double)mem_total / 1048576.0, pct * 100.0);
            gtk_label_set_text(GTK_LABEL(sw->mem_lbl), t);
        }
    }

    /* Read Storage */
    struct statvfs vfs;
    if (statvfs("/", &vfs) == 0 && vfs.f_blocks > 0) {
        unsigned long long total_bytes = (unsigned long long)vfs.f_blocks * vfs.f_frsize;
        unsigned long long free_bytes = (unsigned long long)vfs.f_bfree * vfs.f_frsize;
        unsigned long long used_bytes = total_bytes - free_bytes;
        double pct = (double)used_bytes / (double)total_bytes;
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(sw->disk_bar), pct);
        char t[64];
        snprintf(t, sizeof(t), "Disk (/): %.0f / %.0f GB (%.0f%%)",
                 (double)used_bytes / 1073741824.0, (double)total_bytes / 1073741824.0, pct * 100.0);
        gtk_label_set_text(GTK_LABEL(sw->disk_lbl), t);
    }

    return G_SOURCE_CONTINUE;
}

static GtkWidget *create_sysinfo_content(HdeWidgetInstance *wi)
{
    SysWidgets *sw = g_new0(SysWidgets, 1);
    wi->user_data = sw;

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);

    /* CPU row */
    sw->cpu_lbl = gtk_label_new("CPU: Calculating...");
    gtk_style_context_add_class(gtk_widget_get_style_context(sw->cpu_lbl), "hde-meter-lbl");
    gtk_widget_set_halign(sw->cpu_lbl, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(vbox), sw->cpu_lbl, FALSE, FALSE, 0);

    sw->cpu_bar = gtk_progress_bar_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(sw->cpu_bar), "hde-meter-bar");
    gtk_box_pack_start(GTK_BOX(vbox), sw->cpu_bar, FALSE, FALSE, 0);

    /* RAM row */
    sw->mem_lbl = gtk_label_new("RAM: Calculating...");
    gtk_style_context_add_class(gtk_widget_get_style_context(sw->mem_lbl), "hde-meter-lbl");
    gtk_widget_set_halign(sw->mem_lbl, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(vbox), sw->mem_lbl, FALSE, FALSE, 0);

    sw->mem_bar = gtk_progress_bar_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(sw->mem_bar), "hde-meter-bar");
    gtk_box_pack_start(GTK_BOX(vbox), sw->mem_bar, FALSE, FALSE, 0);

    /* Disk row */
    sw->disk_lbl = gtk_label_new("Disk: Calculating...");
    gtk_style_context_add_class(gtk_widget_get_style_context(sw->disk_lbl), "hde-meter-lbl");
    gtk_widget_set_halign(sw->disk_lbl, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(vbox), sw->disk_lbl, FALSE, FALSE, 0);

    sw->disk_bar = gtk_progress_bar_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(sw->disk_bar), "hde-meter-bar");
    gtk_box_pack_start(GTK_BOX(vbox), sw->disk_bar, FALSE, FALSE, 0);

    update_sys_cb(sw);
    wi->timer_id = g_timeout_add_seconds(2, update_sys_cb, sw);

    return vbox;
}

/* ---------------------------------------------------- 4. Quick Notes Widget */

static char *get_notes_file_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "hde", NOTES_FILE, NULL);
}

static void on_notes_changed(GtkTextBuffer *buffer, gpointer data)
{
    (void)data;
    GtkTextIter start, end;
    gtk_text_buffer_get_start_iter(buffer, &start);
    gtk_text_buffer_get_end_iter(buffer, &end);
    char *text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);

    char *path = get_notes_file_path();
    char *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);

    g_file_set_contents(path, text ? text : "", -1, NULL);
    g_free(text);
    g_free(path);
}

static GtkWidget *create_notes_content(HdeWidgetInstance *wi)
{
    (void)wi;
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sw), GTK_SHADOW_NONE);
    gtk_widget_set_size_request(sw, -1, 140);

    GtkWidget *tv = gtk_text_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(tv), GTK_WRAP_WORD_CHAR);
    gtk_style_context_add_class(gtk_widget_get_style_context(tv), "hde-notes-text");

    char *path = get_notes_file_path();
    char *saved = NULL;
    if (g_file_get_contents(path, &saved, NULL, NULL) && saved) {
        GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv));
        gtk_text_buffer_set_text(buf, saved, -1);
        g_free(saved);
    }
    g_free(path);

    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv));
    g_signal_connect(buf, "changed", G_CALLBACK(on_notes_changed), NULL);

    gtk_container_add(GTK_CONTAINER(sw), tv);
    return sw;
}

/* ---------------------------------------------------- 5. Media Player Widget */

typedef struct {
    GtkWidget *title_lbl;
    GtkWidget *artist_lbl;
    GtkWidget *play_btn;
    gboolean is_playing;
} MediaWidgets;

static void on_media_play_pause(GtkButton *btn, gpointer data)
{
    (void)btn; (void)data;
    g_spawn_command_line_async("playerctl play-pause", NULL);
}

static void on_media_prev(GtkButton *btn, gpointer data)
{
    (void)btn; (void)data;
    g_spawn_command_line_async("playerctl previous", NULL);
}

static void on_media_next(GtkButton *btn, gpointer data)
{
    (void)btn; (void)data;
    g_spawn_command_line_async("playerctl next", NULL);
}

static gboolean update_media_cb(gpointer data)
{
    MediaWidgets *mw = (MediaWidgets *)data;
    char *out = NULL;

    if (g_spawn_command_line_sync("playerctl metadata --format '{{title}}:::{{artist}}:::{{status}}'",
                                 &out, NULL, NULL, NULL) && out && *out) {
        char *p1 = strstr(out, ":::");
        if (p1) {
            *p1 = '\0';
            char *title = out;
            char *artist = p1 + 3;
            char *p2 = strstr(artist, ":::");
            if (p2) {
                *p2 = '\0';
                char *status = p2 + 3;
                g_strstrip(status);
                mw->is_playing = (g_ascii_strcasecmp(status, "Playing") == 0);
            }
            g_strstrip(title);
            g_strstrip(artist);

            gtk_label_set_text(GTK_LABEL(mw->title_lbl), *title ? title : "Unknown Title");
            gtk_label_set_text(GTK_LABEL(mw->artist_lbl), *artist ? artist : "Unknown Artist");
            gtk_button_set_image(GTK_BUTTON(mw->play_btn),
                gtk_image_new_from_icon_name(mw->is_playing ? "media-playback-pause-symbolic" : "media-playback-start-symbolic",
                                             GTK_ICON_SIZE_BUTTON));
            g_free(out);
            return G_SOURCE_CONTINUE;
        }
    }
    g_free(out);

    gtk_label_set_text(GTK_LABEL(mw->title_lbl), "No Media Playing");
    gtk_label_set_text(GTK_LABEL(mw->artist_lbl), "Ready for playback");
    gtk_button_set_image(GTK_BUTTON(mw->play_btn),
        gtk_image_new_from_icon_name("media-playback-start-symbolic", GTK_ICON_SIZE_BUTTON));
    return G_SOURCE_CONTINUE;
}

static GtkWidget *create_media_content(HdeWidgetInstance *wi)
{
    MediaWidgets *mw = g_new0(MediaWidgets, 1);
    wi->user_data = mw;

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);

    mw->title_lbl = gtk_label_new("No Media Playing");
    gtk_style_context_add_class(gtk_widget_get_style_context(mw->title_lbl), "hde-clock-date");
    gtk_label_set_ellipsize(GTK_LABEL(mw->title_lbl), PANGO_ELLIPSIZE_END);
    gtk_widget_set_halign(mw->title_lbl, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(vbox), mw->title_lbl, FALSE, FALSE, 0);

    mw->artist_lbl = gtk_label_new("Ready for playback");
    gtk_style_context_add_class(gtk_widget_get_style_context(mw->artist_lbl), "hde-weather-sub");
    gtk_label_set_ellipsize(GTK_LABEL(mw->artist_lbl), PANGO_ELLIPSIZE_END);
    gtk_widget_set_halign(mw->artist_lbl, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(vbox), mw->artist_lbl, FALSE, FALSE, 0);

    GtkWidget *btn_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(btn_row, GTK_ALIGN_CENTER);

    GtkWidget *prev_btn = gtk_button_new_from_icon_name("media-skip-backward-symbolic", GTK_ICON_SIZE_BUTTON);
    g_signal_connect(prev_btn, "clicked", G_CALLBACK(on_media_prev), NULL);
    gtk_box_pack_start(GTK_BOX(btn_row), prev_btn, FALSE, FALSE, 0);

    mw->play_btn = gtk_button_new_from_icon_name("media-playback-start-symbolic", GTK_ICON_SIZE_BUTTON);
    g_signal_connect(mw->play_btn, "clicked", G_CALLBACK(on_media_play_pause), mw);
    gtk_box_pack_start(GTK_BOX(btn_row), mw->play_btn, FALSE, FALSE, 0);

    GtkWidget *next_btn = gtk_button_new_from_icon_name("media-skip-forward-symbolic", GTK_ICON_SIZE_BUTTON);
    g_signal_connect(next_btn, "clicked", G_CALLBACK(on_media_next), NULL);
    gtk_box_pack_start(GTK_BOX(btn_row), next_btn, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(vbox), btn_row, FALSE, FALSE, 0);

    update_media_cb(mw);
    wi->timer_id = g_timeout_add_seconds(3, update_media_cb, mw);

    return vbox;
}

/* -------------------------------------------------- 6. Mini Calendar Widget */

static GtkWidget *create_calendar_content(HdeWidgetInstance *wi)
{
    (void)wi;
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);

    GtkWidget *cal = gtk_calendar_new();
    gtk_calendar_set_display_options(GTK_CALENDAR(cal),
        GTK_CALENDAR_SHOW_HEADING | GTK_CALENDAR_SHOW_DAY_NAMES | GTK_CALENDAR_SHOW_WEEK_NUMBERS);

    gtk_box_pack_start(GTK_BOX(vbox), cal, TRUE, TRUE, 0);
    return vbox;
}

/* ------------------------------------------------------------- Initialization */

/* a saved position can lie outside the current screen (resolution changed): pull it back once allocated */
static gboolean clamp_saved_pos_idle(gpointer data)
{
    HdeWidgetInstance *wi = (HdeWidgetInstance *)data;
    if (!wi->container || !desktop_fixed) return G_SOURCE_REMOVE;
    int nx = wi->x, ny = wi->y;
    clamp_widget_pos(wi->container, &nx, &ny);
    if (nx != wi->x || ny != wi->y) {
        wi->x = nx; wi->y = ny;
        gtk_fixed_move(GTK_FIXED(desktop_fixed), wi->container, nx, ny);
    }
    return G_SOURCE_REMOVE;
}

static void refresh_widget_display(HdeWidgetInstance *wi)
{
    if (!desktop_fixed) return;

    if (wi->enabled && widgets_visible) {
        if (!wi->container) {
            GtkWidget *content = NULL;
            switch (wi->type) {
            case HDE_WIDGET_CLOCK:    content = create_clock_content(wi); break;
            case HDE_WIDGET_WEATHER:  content = create_weather_content(wi); break;
            case HDE_WIDGET_SYSINFO:  content = create_sysinfo_content(wi); break;
            case HDE_WIDGET_NOTES:    content = create_notes_content(wi); break;
            case HDE_WIDGET_MEDIA:    content = create_media_content(wi); break;
            case HDE_WIDGET_CALENDAR: content = create_calendar_content(wi); break;
            default: break;
            }
            if (content) {
                GtkWidget *frame = build_widget_frame(wi, content);
                gtk_fixed_put(GTK_FIXED(desktop_fixed), frame, wi->x, wi->y);
                gtk_widget_show_all(frame);
                g_idle_add(clamp_saved_pos_idle, wi);
            }
        } else {
            gtk_fixed_move(GTK_FIXED(desktop_fixed), wi->container, wi->x, wi->y);
            gtk_widget_show(wi->container);
        }
    } else {
        if (wi->container) {
            gtk_widget_destroy(wi->container);
            wi->container = NULL;
            wi->content = NULL;
            if (wi->timer_id) {
                g_source_remove(wi->timer_id);
                wi->timer_id = 0;
            }
            if (wi->user_data) {
                g_free(wi->user_data);
                wi->user_data = NULL;
            }
        }
    }
}

void hde_widgets_init(GtkWidget *fixed_container, GtkWidget *parent_window)
{
    desktop_fixed = fixed_container;
    desktop_win   = parent_window;

    hde_widgets_reload_css();
    hde_widgets_load_config();

    char *path = get_widgets_config_path();
    gboolean exists = g_file_test(path, G_FILE_TEST_EXISTS);
    g_free(path);

    int sw = 1280;
    if (desktop_win) {
        int w = gtk_widget_get_allocated_width(desktop_win);
        if (w > 640) sw = w;
        else {
            GdkScreen *scr = gtk_widget_get_screen(desktop_win);
            if (scr && gdk_screen_get_width(scr) > 640) sw = gdk_screen_get_width(scr);
        }
    }

    if (!exists || g_widgets[HDE_WIDGET_CLOCK].x < 200) {
        hde_widgets_reset_positions(sw, 720);
    }

    for (int i = 0; i < HDE_WIDGET_COUNT; i++) {
        refresh_widget_display(&g_widgets[i]);
    }
}

void hde_widgets_toggle(HdeWidgetType type)
{
    if (type >= 0 && type < HDE_WIDGET_COUNT) {
        g_widgets[type].enabled = !g_widgets[type].enabled;
        refresh_widget_display(&g_widgets[type]);
        hde_widgets_save_config();
    }
}

void hde_widgets_set_enabled(HdeWidgetType type, gboolean enabled)
{
    if (type >= 0 && type < HDE_WIDGET_COUNT) {
        if (g_widgets[type].enabled != enabled) {
            g_widgets[type].enabled = enabled;
            refresh_widget_display(&g_widgets[type]);
            hde_widgets_save_config();
        }
    }
}

gboolean hde_widgets_is_enabled(HdeWidgetType type)
{
    return (type >= 0 && type < HDE_WIDGET_COUNT) ? g_widgets[type].enabled : FALSE;
}

void hde_widgets_set_locked(gboolean locked)
{
    widgets_locked = locked;
    hde_widgets_save_config();
}

gboolean hde_widgets_is_locked(void)
{
    return widgets_locked;
}

void hde_widgets_reset_positions(int screen_w, int screen_h)
{
    (void)screen_h;
    /* Default layout: Column 1 at right or left edge */
    int col1_x = (screen_w > 1200) ? (screen_w - 320) : 40;
    int col2_x = col1_x - 300;
    if (col2_x < 40) col2_x = 40;

    g_widgets[HDE_WIDGET_CLOCK].x = col1_x;
    g_widgets[HDE_WIDGET_CLOCK].y = 50;

    g_widgets[HDE_WIDGET_WEATHER].x = col1_x;
    g_widgets[HDE_WIDGET_WEATHER].y = 210;

    g_widgets[HDE_WIDGET_SYSINFO].x = col1_x;
    g_widgets[HDE_WIDGET_SYSINFO].y = 390;

    g_widgets[HDE_WIDGET_NOTES].x = col2_x;
    g_widgets[HDE_WIDGET_NOTES].y = 50;

    g_widgets[HDE_WIDGET_MEDIA].x = col2_x;
    g_widgets[HDE_WIDGET_MEDIA].y = 270;

    g_widgets[HDE_WIDGET_CALENDAR].x = col2_x;
    g_widgets[HDE_WIDGET_CALENDAR].y = 420;

    for (int i = 0; i < HDE_WIDGET_COUNT; i++) {
        refresh_widget_display(&g_widgets[i]);
    }
    hde_widgets_save_config();
}

/* ------------------------------------------------ Desktop Context Submenu */

static void on_menu_toggle_widget(GtkCheckMenuItem *item, gpointer data)
{
    HdeWidgetInstance *wi = (HdeWidgetInstance *)data;
    gboolean active = gtk_check_menu_item_get_active(item);
    hde_widgets_set_enabled(wi->type, active);
}

static void on_menu_toggle_locked(GtkCheckMenuItem *item, gpointer data)
{
    (void)data;
    hde_widgets_set_locked(gtk_check_menu_item_get_active(item));
}

static void on_menu_reset_layout(GtkMenuItem *item, gpointer data)
{
    (void)item; (void)data;
    int w = 1920, h = 1080;
    if (desktop_win) {
        w = gtk_widget_get_allocated_width(desktop_win);
        h = gtk_widget_get_allocated_height(desktop_win);
    }
    hde_widgets_reset_positions(w, h);
}

static void on_menu_manage_widgets(GtkMenuItem *item, gpointer data)
{
    (void)item; (void)data;
    g_spawn_command_line_async("hde-widgets", NULL);
}

GtkWidget *hde_widgets_create_menu(void)
{
    GtkWidget *submenu = gtk_menu_new();

    for (int i = 0; i < HDE_WIDGET_COUNT; i++) {
        GtkWidget *item = gtk_check_menu_item_new_with_label(g_widgets[i].name);
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), g_widgets[i].enabled);
        g_signal_connect(item, "toggled", G_CALLBACK(on_menu_toggle_widget), &g_widgets[i]);
        gtk_menu_shell_append(GTK_MENU_SHELL(submenu), item);
    }

    gtk_menu_shell_append(GTK_MENU_SHELL(submenu), gtk_separator_menu_item_new());

    GtkWidget *lock_item = gtk_check_menu_item_new_with_label("Lock Widgets Position");
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(lock_item), widgets_locked);
    g_signal_connect(lock_item, "toggled", G_CALLBACK(on_menu_toggle_locked), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(submenu), lock_item);

    GtkWidget *reset_item = gtk_menu_item_new_with_label("Reset Default Layout");
    g_signal_connect(reset_item, "activate", G_CALLBACK(on_menu_reset_layout), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(submenu), reset_item);

    GtkWidget *manage_item = gtk_menu_item_new_with_label("Widgets Gallery & Settings…");
    g_signal_connect(manage_item, "activate", G_CALLBACK(on_menu_manage_widgets), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(submenu), manage_item);

    gtk_widget_show_all(submenu);
    return submenu;
}

void hde_widgets_shutdown(void)
{
    for (int i = 0; i < HDE_WIDGET_COUNT; i++) {
        if (g_widgets[i].timer_id) {
            g_source_remove(g_widgets[i].timer_id);
            g_widgets[i].timer_id = 0;
        }
        if (g_widgets[i].container) {
            gtk_widget_destroy(g_widgets[i].container);
            g_widgets[i].container = NULL;
        }
    }
}

void hde_widgets_reload_all(void)
{
    if (!desktop_fixed) return;
    for (int i = 0; i < HDE_WIDGET_COUNT; i++) {
        refresh_widget_display(&g_widgets[i]);
    }
}
