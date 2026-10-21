/* hde-clock.c — Modern, Feature-rich Clock for Hyggshi Desktop Environment (GTK3).
 *
 * Features:
 *   - World Clock: High-DPI analog dial, digital local station, searchable global cities,
 *     day/night indicators, time difference tags, and customizable city cards.
 *   - Alarms: Multiple alarms, repeat modes (daily, weekdays, weekends, custom days),
 *     next-trigger countdowns, interactive ringing banner, sound alerts, and notifications.
 *   - Stopwatch: Millisecond precision, lap tracking with fastest/slowest lap highlighting,
 *     keyboard shortcuts (Space/L/R), and clipboard export.
 *   - Timer: Smooth Cairo radial countdown gauge, one-click preset chips (Pomodoro, 5m, 15m, etc.),
 *     quick +1m/+5m adjustments, sound alarm, and repeat.
 *
 * Language & Code: English.
 */
#define _POSIX_C_SOURCE 200809L

#include "hde-build.h"
#include "hde-theme.h"
#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define APP_TITLE "Hyggshi Clock"
#define MAX_LAPS 500

/* ---------------------------------------------------------------- Data Structures */
typedef struct {
    char *city;
    char *country;
    char *tz_id;
    GtkWidget *card;
    GtkWidget *time_lbl;
    GtkWidget *sub_lbl;
    GtkWidget *diff_lbl;
    GtkWidget *day_icon;
} WorldZone;

typedef struct {
    int hour;
    int minute;
    char *label;
    int repeat_mode; /* 0=Every day, 1=Weekdays, 2=Weekends, 3=Once, 4=Custom */
    gboolean days_active[7]; /* Mon=0..Sun=6 */
    gboolean enabled;
    int last_fired_yday;
    GtkWidget *row_widget;
    GtkWidget *time_lbl;
    GtkWidget *meta_lbl;
    GtkWidget *switch_w;
} AlarmItem;

typedef struct {
    int lap_num;
    gint64 split_us;
    gint64 total_us;
} LapRecord;

/* ---------------------------------------------------------------- Global State */
static GtkCssProvider *app_css = NULL;
static GtkWidget *main_win = NULL;
static GtkWidget *stack = NULL;
static GtkWidget *header_bar = NULL;
static GtkWidget *stack_switcher = NULL;

/* World Clock */
static GtkWidget *analog_area = NULL;
static GtkWidget *local_time_lbl = NULL;
static GtkWidget *local_date_lbl = NULL;
static GtkWidget *local_tz_lbl = NULL;
static GtkWidget *zones_flowbox = NULL;
static GPtrArray *world_zones = NULL; /* WorldZone* */

/* Alarms */
static GPtrArray *alarms = NULL; /* AlarmItem* */
static GtkWidget *alarms_listbox = NULL;
static GtkWidget *alarm_add_revealer = NULL;
static GtkWidget *alarm_hour_spin = NULL;
static GtkWidget *alarm_min_spin = NULL;
static GtkWidget *alarm_label_entry = NULL;
static GtkWidget *alarm_repeat_combo = NULL;
static GtkWidget *alarm_day_btns[7];
static GtkWidget *alarm_ringing_revealer = NULL;
static GtkWidget *alarm_ringing_lbl = NULL;
static AlarmItem *current_ringing_alarm = NULL;

/* Stopwatch */
static gboolean sw_running = FALSE;
static gint64 sw_start_us = 0;
static gint64 sw_accum_us = 0;
static gint64 sw_last_lap_us = 0;
static int sw_lap_count = 0;
static GtkWidget *sw_display_lbl = NULL;
static GtkWidget *sw_split_lbl = NULL;
static GtkWidget *sw_start_btn = NULL;
static GtkWidget *sw_lap_btn = NULL;
static GtkWidget *sw_reset_btn = NULL;
static GtkWidget *sw_copy_btn = NULL;
static GtkListStore *sw_lap_store = NULL;
static GtkWidget *sw_lap_treeview = NULL;
static GArray *sw_laps = NULL; /* LapRecord */

/* Timer */
static gboolean tmr_running = FALSE;
static gint64 tmr_total_ms = 0;
static gint64 tmr_remain_ms = 0;
static gint64 tmr_last_tick_us = 0;
static char *tmr_title = NULL;
static GtkWidget *tmr_ring_area = NULL;
static GtkWidget *tmr_hour_spin = NULL;
static GtkWidget *tmr_min_spin = NULL;
static GtkWidget *tmr_sec_spin = NULL;
static GtkWidget *tmr_name_entry = NULL;
static GtkWidget *tmr_start_btn = NULL;
static GtkWidget *tmr_reset_btn = NULL;
static GtkWidget *tmr_ringing_revealer = NULL;
static GtkWidget *tmr_ringing_lbl = NULL;

/* Comprehensive global cities dataset */
static const struct {
    const char *city;
    const char *country;
    const char *tz;
} city_db[] = {
    { "UTC", "Coordinated Universal Time", "UTC" },
    { "London", "United Kingdom", "Europe/London" },
    { "Paris", "France", "Europe/Paris" },
    { "Berlin", "Germany", "Europe/Berlin" },
    { "Madrid", "Spain", "Europe/Madrid" },
    { "Rome", "Italy", "Europe/Rome" },
    { "Amsterdam", "Netherlands", "Europe/Amsterdam" },
    { "Stockholm", "Sweden", "Europe/Stockholm" },
    { "Vienna", "Austria", "Europe/Vienna" },
    { "Athens", "Greece", "Europe/Athens" },
    { "Helsinki", "Finland", "Europe/Helsinki" },
    { "Kyiv", "Ukraine", "Europe/Kyiv" },
    { "Moscow", "Russia", "Europe/Moscow" },
    { "Istanbul", "Turkey", "Europe/Istanbul" },
    { "Dubai", "United Arab Emirates", "Asia/Dubai" },
    { "Riyadh", "Saudi Arabia", "Asia/Riyadh" },
    { "Cairo", "Egypt", "Africa/Cairo" },
    { "Johannesburg", "South Africa", "Africa/Johannesburg" },
    { "Nairobi", "Kenya", "Africa/Nairobi" },
    { "Mumbai", "India", "Asia/Kolkata" },
    { "New Delhi", "India", "Asia/Kolkata" },
    { "Dhaka", "Bangladesh", "Asia/Dhaka" },
    { "Bangkok", "Thailand", "Asia/Bangkok" },
    { "Ho Chi Minh City", "Vietnam", "Asia/Ho_Chi_Minh" },
    { "Hanoi", "Vietnam", "Asia/Ho_Chi_Minh" },
    { "Singapore", "Singapore", "Asia/Singapore" },
    { "Kuala Lumpur", "Malaysia", "Asia/Kuala_Lumpur" },
    { "Jakarta", "Indonesia", "Asia/Jakarta" },
    { "Hong Kong", "China", "Asia/Hong_Kong" },
    { "Taipei", "Taiwan", "Asia/Taipei" },
    { "Beijing", "China", "Asia/Shanghai" },
    { "Shanghai", "China", "Asia/Shanghai" },
    { "Seoul", "South Korea", "Asia/Seoul" },
    { "Tokyo", "Japan", "Asia/Tokyo" },
    { "Sydney", "Australia", "Australia/Sydney" },
    { "Melbourne", "Australia", "Australia/Melbourne" },
    { "Auckland", "New Zealand", "Pacific/Auckland" },
    { "Honolulu", "United States", "Pacific/Honolulu" },
    { "Anchorage", "United States", "America/Anchorage" },
    { "Vancouver", "Canada", "America/Vancouver" },
    { "Seattle", "United States", "America/Los_Angeles" },
    { "San Francisco", "United States", "America/Los_Angeles" },
    { "Los Angeles", "United States", "America/Los_Angeles" },
    { "Denver", "United States", "America/Denver" },
    { "Chicago", "United States", "America/Chicago" },
    { "Houston", "United States", "America/Chicago" },
    { "Toronto", "Canada", "America/Toronto" },
    { "New York", "United States", "America/New_York" },
    { "Boston", "United States", "America/New_York" },
    { "Mexico City", "Mexico", "America/Mexico_City" },
    { "Bogota", "Colombia", "America/Bogota" },
    { "Lima", "Peru", "America/Lima" },
    { "Santiago", "Chile", "America/Santiago" },
    { "Buenos Aires", "Argentina", "America/Argentina/Buenos_Aires" },
    { "Sao Paulo", "Brazil", "America/Sao_Paulo" }
};

/* ---------------------------------------------------------------- Forward Declarations */
static void save_state(void);
static void update_world_zones_ui(void);
static void rebuild_alarms_list(void);

/* ---------------------------------------------------------------- CSS & Theming */
static char *clock_ini_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "hde", "clock.ini", NULL);
}

static void load_css(void)
{
    HdeThemeInfo ti;
    hde_theme_info_load(&ti);
    char *accent = hde_theme_accent_css(&ti);
    const char *ac = ti.accent ? ti.accent : "#3584e4";
    gboolean dark = hde_theme_shell_dark();

    const char *card_bg = dark ? "rgba(255, 255, 255, 0.05)" : "rgba(0, 0, 0, 0.04)";
    const char *card_border = dark ? "rgba(255, 255, 255, 0.10)" : "rgba(0, 0, 0, 0.08)";
    const char *badge_bg = dark ? "rgba(255, 255, 255, 0.08)" : "rgba(0, 0, 0, 0.06)";

    char *css = g_strdup_printf(
        ".clock-card {"
        "  background-color: %s;"
        "  border: 1px solid %s;"
        "  border-radius: 14px;"
        "  padding: 16px;"
        "}"
        ".clock-card-city {"
        "  background-color: %s;"
        "  border: 1px solid %s;"
        "  border-radius: 12px;"
        "  padding: 14px 18px;"
        "  min-width: 240px;"
        "}"
        ".clock-card-city:hover {"
        "  background-color: %s;"
        "}"
        ".clock-hero-time {"
        "  font-family: monospace, sans-serif;"
        "  font-size: 38px;"
        "  font-weight: 800;"
        "  letter-spacing: 0.5px;"
        "}"
        ".clock-sw-time {"
        "  font-family: monospace, sans-serif;"
        "  font-size: 52px;"
        "  font-weight: 800;"
        "  color: %s;"
        "}"
        ".clock-city-name {"
        "  font-size: 16px;"
        "  font-weight: 700;"
        "}"
        ".clock-city-time {"
        "  font-family: monospace, sans-serif;"
        "  font-size: 26px;"
        "  font-weight: 700;"
        "  color: %s;"
        "}"
        ".clock-sub {"
        "  font-size: 12px;"
        "  opacity: 0.75;"
        "}"
        ".clock-tag {"
        "  background-color: %s;"
        "  border-radius: 6px;"
        "  padding: 2px 8px;"
        "  font-size: 11px;"
        "  font-weight: 600;"
        "}"
        ".clock-alarm-time {"
        "  font-family: monospace, sans-serif;"
        "  font-size: 28px;"
        "  font-weight: 700;"
        "}"
        ".clock-alert-banner {"
        "  background-color: alpha(%s, 0.20);"
        "  border: 1px solid %s;"
        "  border-radius: 12px;"
        "  padding: 12px 18px;"
        "}"
        ".clock-chip-btn {"
        "  border-radius: 18px;"
        "  padding: 4px 14px;"
        "  font-size: 13px;"
        "  font-weight: 600;"
        "}"
        ".clock-fastest {"
        "  color: #2ec27e;"
        "  font-weight: 700;"
        "}"
        ".clock-slowest {"
        "  color: #e01b24;"
        "  font-weight: 700;"
        "}"
        "%s",
        card_bg, card_border,
        card_bg, card_border,
        dark ? "rgba(255, 255, 255, 0.08)" : "rgba(0, 0, 0, 0.07)",
        ac,
        ac,
        badge_bg,
        ac, ac,
        accent);

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
    if (analog_area) gtk_widget_queue_draw(analog_area);
    if (tmr_ring_area) gtk_widget_queue_draw(tmr_ring_area);
}

/* Sound and System Notifications */
static void notify_event(const char *summary, const char *body)
{
    GdkDisplay *dpy = gdk_display_get_default();
    if (dpy) gdk_display_beep(dpy);

    char *ns = g_find_program_in_path("notify-send");
    if (ns) {
        char *argv[] = {
            ns,
            (char *)"--app-name=Hyggshi Clock",
            (char *)"--icon=preferences-system-time",
            (char *)"--urgency=critical",
            (char *)"--",
            (char *)summary,
            (char *)body,
            NULL
        };
        g_spawn_async(NULL, argv, NULL, G_SPAWN_DEFAULT, NULL, NULL, NULL, NULL);
        g_free(ns);
    }

    char *canb = g_find_program_in_path("canberra-gtk-play");
    if (canb) {
        char *argv[] = { canb, (char *)"-i", (char *)"alarm-clock-elapsed", NULL };
        g_spawn_async(NULL, argv, NULL, G_SPAWN_DEFAULT, NULL, NULL, NULL, NULL);
        g_free(canb);
    }
}

/* ---------------------------------------------------------------- World Clock: Analog Canvas */
static gboolean draw_analog_clock(GtkWidget *w, cairo_t *cr, gpointer d)
{
    (void)d;
    int width = gtk_widget_get_allocated_width(w);
    int height = gtk_widget_get_allocated_height(w);
    double cx = width / 2.0;
    double cy = height / 2.0;
    double radius = MIN(width, height) / 2.0 - 12.0;
    if (radius < 24) return FALSE;

    HdeThemeInfo ti;
    hde_theme_info_load(&ti);
    GdkRGBA accent = { 0.21, 0.52, 0.89, 1.0 };
    if (ti.accent) gdk_rgba_parse(&accent, ti.accent);
    gboolean dark = hde_theme_shell_dark();
    hde_theme_info_clear(&ti);

    double fg = dark ? 0.95 : 0.12;

    /* Bezel ring & dial face */
    cairo_pattern_t *pat = cairo_pattern_create_radial(cx - radius * 0.2, cy - radius * 0.2, radius * 0.1, cx, cy, radius);
    if (dark) {
        cairo_pattern_add_color_stop_rgba(pat, 0.0, 0.18, 0.20, 0.24, 0.9);
        cairo_pattern_add_color_stop_rgba(pat, 1.0, 0.10, 0.11, 0.13, 0.95);
    } else {
        cairo_pattern_add_color_stop_rgba(pat, 0.0, 1.0, 1.0, 1.0, 0.95);
        cairo_pattern_add_color_stop_rgba(pat, 1.0, 0.92, 0.93, 0.95, 0.9);
    }
    cairo_arc(cr, cx, cy, radius, 0, 2 * G_PI);
    cairo_set_source(cr, pat);
    cairo_fill_preserve(cr);
    cairo_pattern_destroy(pat);

    /* Bezel border */
    cairo_set_source_rgba(cr, fg, fg, fg, 0.18);
    cairo_set_line_width(cr, 2.4);
    cairo_stroke(cr);

    /* Minute and hour tick marks */
    for (int i = 0; i < 60; i++) {
        double angle = i * G_PI / 30.0 - G_PI / 2.0;
        gboolean is_hour = (i % 5) == 0;
        double r_out = radius - 8.0;
        double r_in = r_out - (is_hour ? 10.0 : 4.0);

        cairo_set_source_rgba(cr, fg, fg, fg, is_hour ? 0.85 : 0.25);
        cairo_set_line_width(cr, is_hour ? 2.6 : 1.2);
        cairo_move_to(cr, cx + cos(angle) * r_in, cy + sin(angle) * r_in);
        cairo_line_to(cr, cx + cos(angle) * r_out, cy + sin(angle) * r_out);
        cairo_stroke(cr);
    }

    /* Current local time */
    GDateTime *now = g_date_time_new_now_local();
    int hr = g_date_time_get_hour(now) % 12;
    int mn = g_date_time_get_minute(now);
    double sc = g_date_time_get_seconds(now);
    g_date_time_unref(now);

    double h_angle = ((hr + mn / 60.0 + sc / 3600.0) / 12.0) * 2.0 * G_PI - G_PI / 2.0;
    double m_angle = ((mn + sc / 60.0) / 60.0) * 2.0 * G_PI - G_PI / 2.0;
    double s_angle = (sc / 60.0) * 2.0 * G_PI - G_PI / 2.0;

    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);

    /* Hour hand */
    cairo_set_source_rgba(cr, fg, fg, fg, 0.92);
    cairo_set_line_width(cr, 4.8);
    cairo_move_to(cr, cx, cy);
    cairo_line_to(cr, cx + cos(h_angle) * (radius * 0.52), cy + sin(h_angle) * (radius * 0.52));
    cairo_stroke(cr);

    /* Minute hand */
    cairo_set_source_rgba(cr, fg, fg, fg, 0.82);
    cairo_set_line_width(cr, 3.2);
    cairo_move_to(cr, cx, cy);
    cairo_line_to(cr, cx + cos(m_angle) * (radius * 0.76), cy + sin(m_angle) * (radius * 0.76));
    cairo_stroke(cr);

    /* Second hand */
    cairo_set_source_rgba(cr, accent.red, accent.green, accent.blue, 0.95);
    cairo_set_line_width(cr, 1.8);
    cairo_move_to(cr, cx - cos(s_angle) * (radius * 0.18), cy - sin(s_angle) * (radius * 0.18));
    cairo_line_to(cr, cx + cos(s_angle) * (radius * 0.84), cy + sin(s_angle) * (radius * 0.84));
    cairo_stroke(cr);

    /* Second hand counterweight & center cap */
    cairo_arc(cr, cx - cos(s_angle) * (radius * 0.18), cy - sin(s_angle) * (radius * 0.18), 3.0, 0, 2 * G_PI);
    cairo_fill(cr);

    cairo_arc(cr, cx, cy, 5.0, 0, 2 * G_PI);
    cairo_fill(cr);

    cairo_arc(cr, cx, cy, 2.0, 0, 2 * G_PI);
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_fill(cr);

    return FALSE;
}

/* ---------------------------------------------------------------- World Clock Management */
static void world_zone_free(gpointer p)
{
    WorldZone *z = p;
    if (!z) return;
    g_free(z->city);
    g_free(z->country);
    g_free(z->tz_id);
    g_free(z);
}

static void on_remove_zone(GtkButton *b, gpointer d)
{
    (void)b;
    WorldZone *z = d;
    if (!z || !world_zones) return;
    if (z->card) gtk_widget_destroy(z->card);
    g_ptr_array_remove(world_zones, z);
    save_state();
}

static void add_world_zone_card(const char *city, const char *country, const char *tz_id, gboolean persist)
{
    if (!city || !tz_id) return;
    for (guint i = 0; i < world_zones->len; i++) {
        WorldZone *existing = g_ptr_array_index(world_zones, i);
        if (!g_strcmp0(existing->tz_id, tz_id) && !g_strcmp0(existing->city, city))
            return;
    }

    WorldZone *z = g_new0(WorldZone, 1);
    z->city = g_strdup(city);
    z->country = country ? g_strdup(country) : g_strdup("");
    z->tz_id = g_strdup(tz_id);

    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_style_context_add_class(gtk_widget_get_style_context(card), "clock-card-city");
    z->card = card;

    /* Top row: City title + Country + Delete icon */
    GtkWidget *top_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *title_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);

    GtkWidget *cl = gtk_label_new(city);
    gtk_label_set_xalign(GTK_LABEL(cl), 0.0);
    gtk_style_context_add_class(gtk_widget_get_style_context(cl), "clock-city-name");

    GtkWidget *country_lbl = gtk_label_new(country && *country ? country : tz_id);
    gtk_label_set_xalign(GTK_LABEL(country_lbl), 0.0);
    gtk_style_context_add_class(gtk_widget_get_style_context(country_lbl), "clock-sub");

    gtk_box_pack_start(GTK_BOX(title_box), cl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(title_box), country_lbl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(top_row), title_box, TRUE, TRUE, 0);

    GtkWidget *del_btn = gtk_button_new_from_icon_name("window-close-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_button_set_relief(GTK_BUTTON(del_btn), GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text(del_btn, "Remove this city");
    g_signal_connect(del_btn, "clicked", G_CALLBACK(on_remove_zone), z);
    gtk_box_pack_end(GTK_BOX(top_row), del_btn, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(card), top_row, FALSE, FALSE, 0);

    /* Center row: Large digital time + Day/Night Icon */
    GtkWidget *time_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    z->time_lbl = gtk_label_new("--:--:--");
    gtk_label_set_xalign(GTK_LABEL(z->time_lbl), 0.0);
    gtk_style_context_add_class(gtk_widget_get_style_context(z->time_lbl), "clock-city-time");

    z->day_icon = gtk_image_new_from_icon_name("weather-clear-symbolic", GTK_ICON_SIZE_LARGE_TOOLBAR);
    gtk_box_pack_start(GTK_BOX(time_row), z->time_lbl, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(time_row), z->day_icon, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(card), time_row, FALSE, FALSE, 2);

    /* Bottom row: Time difference tag + relative day */
    GtkWidget *meta_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    z->diff_lbl = gtk_label_new("0 hrs");
    gtk_style_context_add_class(gtk_widget_get_style_context(z->diff_lbl), "clock-tag");

    z->sub_lbl = gtk_label_new("Today · UTC offset");
    gtk_label_set_xalign(GTK_LABEL(z->sub_lbl), 0.0);
    gtk_style_context_add_class(gtk_widget_get_style_context(z->sub_lbl), "clock-sub");

    gtk_box_pack_start(GTK_BOX(meta_row), z->diff_lbl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(meta_row), z->sub_lbl, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(card), meta_row, FALSE, FALSE, 0);

    gtk_widget_show_all(card);
    gtk_container_add(GTK_CONTAINER(zones_flowbox), card);

    g_ptr_array_add(world_zones, z);
    update_world_zones_ui();
    if (persist) save_state();
}

static void update_world_zones_ui(void)
{
    GDateTime *local_now = g_date_time_new_now_local();
    char *t_str = g_date_time_format(local_now, "%H:%M:%S");
    char *d_str = g_date_time_format(local_now, "%A, %B %e, %Y");
    const char *tz_abbr = g_date_time_get_timezone_abbreviation(local_now);
    GTimeSpan local_off = g_date_time_get_utc_offset(local_now);
    int off_hr = (int)(local_off / G_TIME_SPAN_HOUR);
    int off_mn = abs((int)((local_off % G_TIME_SPAN_HOUR) / G_TIME_SPAN_MINUTE));
    char *tz_str = g_strdup_printf("Local Station: %s (UTC%+03d:%02d)", tz_abbr ? tz_abbr : "Local", off_hr, off_mn);

    if (local_time_lbl) gtk_label_set_text(GTK_LABEL(local_time_lbl), t_str);
    if (local_date_lbl) gtk_label_set_text(GTK_LABEL(local_date_lbl), d_str);
    if (local_tz_lbl) gtk_label_set_text(GTK_LABEL(local_tz_lbl), tz_str);
    g_free(t_str);
    g_free(d_str);
    g_free(tz_str);

    int local_doy = g_date_time_get_day_of_year(local_now);
    for (guint i = 0; world_zones && i < world_zones->len; i++) {
        WorldZone *z = g_ptr_array_index(world_zones, i);
        GTimeZone *tz = g_time_zone_new_identifier(z->tz_id);
        if (!tz) tz = g_time_zone_new_utc();
        GDateTime *zt = g_date_time_to_timezone(local_now, tz);
        g_time_zone_unref(tz);
        if (!zt) continue;

        char *zt_str = g_date_time_format(zt, "%H:%M:%S");
        int hour = g_date_time_get_hour(zt);
        gboolean is_day = (hour >= 6 && hour < 18);

        int zdoy = g_date_time_get_day_of_year(zt);
        const char *day_rel = (zdoy == local_doy) ? "Today"
                            : (zdoy > local_doy || (local_doy > 360 && zdoy == 1)) ? "Tomorrow" : "Yesterday";

        GTimeSpan diff_span = g_date_time_get_utc_offset(zt) - local_off;
        int diff_hours = (int)(diff_span / G_TIME_SPAN_HOUR);
        int diff_mins = abs((int)((diff_span % G_TIME_SPAN_HOUR) / G_TIME_SPAN_MINUTE));

        char diff_buf[32];
        if (diff_hours == 0 && diff_mins == 0) {
            g_snprintf(diff_buf, sizeof diff_buf, "Same time");
        } else if (diff_mins == 0) {
            g_snprintf(diff_buf, sizeof diff_buf, "%+d hrs", diff_hours);
        } else {
            g_snprintf(diff_buf, sizeof diff_buf, "%+d:%02d hrs", diff_hours, diff_mins);
        }

        char *sub = g_strdup_printf("%s · %s", day_rel, z->tz_id);

        if (z->time_lbl) gtk_label_set_text(GTK_LABEL(z->time_lbl), zt_str);
        if (z->diff_lbl) gtk_label_set_text(GTK_LABEL(z->diff_lbl), diff_buf);
        if (z->sub_lbl) gtk_label_set_text(GTK_LABEL(z->sub_lbl), sub);
        if (z->day_icon) {
            gtk_image_set_from_icon_name(GTK_IMAGE(z->day_icon),
                                         is_day ? "weather-clear-symbolic" : "weather-clear-night-symbolic",
                                         GTK_ICON_SIZE_LARGE_TOOLBAR);
        }

        g_free(zt_str);
        g_free(sub);
        g_date_time_unref(zt);
    }

    g_date_time_unref(local_now);
    if (analog_area) gtk_widget_queue_draw(analog_area);
}

/* Add City Dialog */
static void on_city_dialog_response(GtkDialog *dialog, gint response_id, gpointer d)
{
    GtkTreeView *tree = GTK_TREE_VIEW(d);
    if (response_id == GTK_RESPONSE_ACCEPT) {
        GtkTreeSelection *sel = gtk_tree_view_get_selection(tree);
        GtkTreeModel *model = NULL;
        GtkTreeIter iter;
        if (gtk_tree_selection_get_selected(sel, &model, &iter)) {
            char *city = NULL, *country = NULL, *tz = NULL;
            gtk_tree_model_get(model, &iter, 0, &city, 1, &country, 2, &tz, -1);
            add_world_zone_card(city, country, tz, TRUE);
            g_free(city);
            g_free(country);
            g_free(tz);
        }
    }
    gtk_widget_destroy(GTK_WIDGET(dialog));
}

static void on_filter_entry_changed(GtkSearchEntry *entry, gpointer d)
{
    GtkTreeModelFilter *filter = GTK_TREE_MODEL_FILTER(d);
    gtk_tree_model_filter_refilter(filter);
}

static gboolean city_filter_func(GtkTreeModel *model, GtkTreeIter *iter, gpointer data)
{
    GtkSearchEntry *entry = GTK_SEARCH_ENTRY(data);
    const char *query = gtk_entry_get_text(GTK_ENTRY(entry));
    if (!query || !*query) return TRUE;

    char *city = NULL, *country = NULL, *tz = NULL;
    gtk_tree_model_get(model, iter, 0, &city, 1, &country, 2, &tz, -1);

    char *q_lower = g_utf8_strdown(query, -1);
    char *c_lower = city ? g_utf8_strdown(city, -1) : g_strdup("");
    char *co_lower = country ? g_utf8_strdown(country, -1) : g_strdup("");
    char *tz_lower = tz ? g_utf8_strdown(tz, -1) : g_strdup("");

    gboolean match = (strstr(c_lower, q_lower) != NULL ||
                      strstr(co_lower, q_lower) != NULL ||
                      strstr(tz_lower, q_lower) != NULL);

    g_free(q_lower);
    g_free(c_lower);
    g_free(co_lower);
    g_free(tz_lower);
    g_free(city);
    g_free(country);
    g_free(tz);

    return match;
}

static void show_add_city_dialog(void)
{
    GtkWidget *dlg = gtk_dialog_new_with_buttons("Add World City",
                                                 GTK_WINDOW(main_win),
                                                 GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                                 "Cancel", GTK_RESPONSE_CANCEL,
                                                 "Add City", GTK_RESPONSE_ACCEPT,
                                                 NULL);
    gtk_window_set_default_size(GTK_WINDOW(dlg), 440, 480);
    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    gtk_container_set_border_width(GTK_CONTAINER(content), 12);

    GtkWidget *search = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(search), "Search city, country or timezone...");
    gtk_box_pack_start(GTK_BOX(content), search, FALSE, FALSE, 6);

    GtkListStore *store = gtk_list_store_new(3, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    for (size_t i = 0; i < G_N_ELEMENTS(city_db); i++) {
        GtkTreeIter it;
        gtk_list_store_append(store, &it);
        gtk_list_store_set(store, &it, 0, city_db[i].city, 1, city_db[i].country, 2, city_db[i].tz, -1);
    }

    GtkTreeModelFilter *filter = GTK_TREE_MODEL_FILTER(gtk_tree_model_filter_new(GTK_TREE_MODEL(store), NULL));
    gtk_tree_model_filter_set_visible_func(filter, city_filter_func, search, NULL);
    g_signal_connect(search, "search-changed", G_CALLBACK(on_filter_entry_changed), filter);

    GtkWidget *tv = gtk_tree_view_new_with_model(GTK_TREE_MODEL(filter));
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(tv), TRUE);

    GtkCellRenderer *r_city = gtk_cell_renderer_text_new();
    g_object_set(r_city, "weight", PANGO_WEIGHT_BOLD, NULL);
    GtkTreeViewColumn *col_city = gtk_tree_view_column_new_with_attributes("City", r_city, "text", 0, NULL);
    gtk_tree_view_column_set_expand(col_city, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tv), col_city);

    GtkCellRenderer *r_co = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *col_co = gtk_tree_view_column_new_with_attributes("Country / Timezone", r_co, "text", 1, NULL);
    gtk_tree_view_column_set_expand(col_co, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tv), col_co);

    GtkWidget *sc = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sc), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(sc), tv);
    gtk_box_pack_start(GTK_BOX(content), sc, TRUE, TRUE, 6);

    g_signal_connect(dlg, "response", G_CALLBACK(on_city_dialog_response), tv);
    gtk_widget_show_all(dlg);
}

static GtkWidget *build_world_page(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 18);
    gtk_container_set_border_width(GTK_CONTAINER(box), 18);

    /* Left: Local Station Card */
    GtkWidget *left_card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_style_context_add_class(gtk_widget_get_style_context(left_card), "clock-card");
    gtk_widget_set_size_request(left_card, 280, -1);

    analog_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(analog_area, 220, 220);
    gtk_widget_set_vexpand(analog_area, TRUE);
    g_signal_connect(analog_area, "draw", G_CALLBACK(draw_analog_clock), NULL);
    gtk_box_pack_start(GTK_BOX(left_card), analog_area, TRUE, TRUE, 0);

    local_time_lbl = gtk_label_new("00:00:00");
    gtk_style_context_add_class(gtk_widget_get_style_context(local_time_lbl), "clock-hero-time");
    local_date_lbl = gtk_label_new("");
    gtk_style_context_add_class(gtk_widget_get_style_context(local_date_lbl), "clock-sub");
    local_tz_lbl = gtk_label_new("");
    gtk_style_context_add_class(gtk_widget_get_style_context(local_tz_lbl), "clock-tag");

    gtk_box_pack_start(GTK_BOX(left_card), local_time_lbl, FALSE, FALSE, 2);
    gtk_box_pack_start(GTK_BOX(left_card), local_date_lbl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(left_card), local_tz_lbl, FALSE, FALSE, 6);

    gtk_box_pack_start(GTK_BOX(box), left_card, FALSE, FALSE, 0);

    /* Right: World Zones Flow Layout */
    GtkWidget *right_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_hexpand(right_box, TRUE);

    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *head_lbl = gtk_label_new("Global Time Stations");
    gtk_style_context_add_class(gtk_widget_get_style_context(head_lbl), "clock-city-name");
    GtkWidget *add_btn = gtk_button_new_with_label("+ Add City");
    gtk_style_context_add_class(gtk_widget_get_style_context(add_btn), "suggested-action");
    g_signal_connect(add_btn, "clicked", G_CALLBACK(show_add_city_dialog), NULL);

    gtk_box_pack_start(GTK_BOX(bar), head_lbl, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(bar), add_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(right_box), bar, FALSE, FALSE, 0);

    zones_flowbox = gtk_flow_box_new();
    gtk_widget_set_valign(zones_flowbox, GTK_ALIGN_START);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(zones_flowbox), 4);
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(zones_flowbox), GTK_SELECTION_NONE);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(zones_flowbox), 12);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(zones_flowbox), 12);

    GtkWidget *sc = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sc), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(sc), zones_flowbox);
    gtk_box_pack_start(GTK_BOX(right_box), sc, TRUE, TRUE, 0);

    gtk_box_pack_start(GTK_BOX(box), right_box, TRUE, TRUE, 0);
    return box;
}

/* ---------------------------------------------------------------- Alarms Management */
static void alarm_free(gpointer p)
{
    AlarmItem *a = p;
    if (!a) return;
    g_free(a->label);
    g_free(a);
}

static void on_alarm_toggle(GtkSwitch *sw, gboolean state, gpointer d)
{
    AlarmItem *a = d;
    if (!a) return;
    a->enabled = state;
    save_state();
}

static void on_delete_alarm(GtkButton *b, gpointer d)
{
    (void)b;
    AlarmItem *a = d;
    if (!a || !alarms) return;
    if (a->row_widget) gtk_widget_destroy(a->row_widget);
    g_ptr_array_remove(alarms, a);
    save_state();
}

static void on_alarm_snooze(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    if (alarm_ringing_revealer)
        gtk_revealer_set_reveal_child(GTK_REVEALER(alarm_ringing_revealer), FALSE);

    /* Snooze current alarm by 10 minutes */
    if (current_ringing_alarm) {
        GDateTime *now = g_date_time_new_now_local();
        GDateTime *snooze = g_date_time_add_minutes(now, 10);
        current_ringing_alarm->hour = g_date_time_get_hour(snooze);
        current_ringing_alarm->minute = g_date_time_get_minute(snooze);
        current_ringing_alarm->enabled = TRUE;
        current_ringing_alarm->last_fired_yday = -1;
        g_date_time_unref(now);
        g_date_time_unref(snooze);
        rebuild_alarms_list();
        save_state();
    }
}

static void on_alarm_dismiss(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    if (alarm_ringing_revealer)
        gtk_revealer_set_reveal_child(GTK_REVEALER(alarm_ringing_revealer), FALSE);
}

static void on_repeat_combo_changed(GtkComboBox *combo, gpointer d)
{
    (void)d;
    int mode = gtk_combo_box_get_active(combo);
    gboolean is_custom = (mode == 4);
    for (int i = 0; i < 7; i++) {
        if (alarm_day_btns[i])
            gtk_widget_set_sensitive(alarm_day_btns[i], is_custom);
    }
}

static void on_toggle_alarm_revealer(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    gboolean rev = gtk_revealer_get_reveal_child(GTK_REVEALER(alarm_add_revealer));
    gtk_revealer_set_reveal_child(GTK_REVEALER(alarm_add_revealer), !rev);
}

static void on_save_new_alarm(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    AlarmItem *a = g_new0(AlarmItem, 1);
    a->hour = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(alarm_hour_spin));
    a->minute = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(alarm_min_spin));
    const char *lbl = gtk_entry_get_text(GTK_ENTRY(alarm_label_entry));
    a->label = (lbl && *lbl) ? g_strdup(lbl) : g_strdup("Alarm");
    a->repeat_mode = gtk_combo_box_get_active(GTK_COMBO_BOX(alarm_repeat_combo));
    a->enabled = TRUE;
    a->last_fired_yday = -1;

    for (int i = 0; i < 7; i++) {
        a->days_active[i] = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(alarm_day_btns[i]));
    }

    g_ptr_array_add(alarms, a);
    gtk_revealer_set_reveal_child(GTK_REVEALER(alarm_add_revealer), FALSE);
    rebuild_alarms_list();
    save_state();
}

static void rebuild_alarms_list(void)
{
    if (!alarms_listbox) return;

    /* Clear listbox children */
    GList *children = gtk_container_get_children(GTK_CONTAINER(alarms_listbox));
    for (GList *c = children; c != NULL; c = c->next)
        gtk_widget_destroy(GTK_WIDGET(c->data));
    g_list_free(children);

    const char *day_names[] = { "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun" };

    for (guint i = 0; i < alarms->len; i++) {
        AlarmItem *a = g_ptr_array_index(alarms, i);

        GtkWidget *row = gtk_list_box_row_new();
        gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
        a->row_widget = row;

        GtkWidget *card = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
        gtk_style_context_add_class(gtk_widget_get_style_context(card), "clock-card");
        gtk_widget_set_margin_start(card, 8);
        gtk_widget_set_margin_end(card, 8);
        gtk_widget_set_margin_top(card, 4);
        gtk_widget_set_margin_bottom(card, 4);

        /* Left: Time and description */
        GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        char t_str[32];
        g_snprintf(t_str, sizeof t_str, "%02d:%02d", a->hour, a->minute);
        a->time_lbl = gtk_label_new(t_str);
        gtk_label_set_xalign(GTK_LABEL(a->time_lbl), 0.0);
        gtk_style_context_add_class(gtk_widget_get_style_context(a->time_lbl), "clock-alarm-time");

        GString *meta = g_string_new(a->label ? a->label : "Alarm");
        g_string_append(meta, " · ");
        if (a->repeat_mode == 0) g_string_append(meta, "Every day");
        else if (a->repeat_mode == 1) g_string_append(meta, "Weekdays (Mon-Fri)");
        else if (a->repeat_mode == 2) g_string_append(meta, "Weekends (Sat-Sun)");
        else if (a->repeat_mode == 3) g_string_append(meta, "Once");
        else {
            g_string_append(meta, "Days: ");
            gboolean first = TRUE;
            for (int d = 0; d < 7; d++) {
                if (a->days_active[d]) {
                    if (!first) g_string_append(meta, ", ");
                    g_string_append(meta, day_names[d]);
                    first = FALSE;
                }
            }
        }

        a->meta_lbl = gtk_label_new(meta->str);
        g_string_free(meta, TRUE);
        gtk_label_set_xalign(GTK_LABEL(a->meta_lbl), 0.0);
        gtk_style_context_add_class(gtk_widget_get_style_context(a->meta_lbl), "clock-sub");

        gtk_box_pack_start(GTK_BOX(vbox), a->time_lbl, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(vbox), a->meta_lbl, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(card), vbox, TRUE, TRUE, 0);

        /* Right: Switch and Delete */
        GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        a->switch_w = gtk_switch_new();
        gtk_switch_set_active(GTK_SWITCH(a->switch_w), a->enabled);
        gtk_widget_set_valign(a->switch_w, GTK_ALIGN_CENTER);
        g_signal_connect(a->switch_w, "state-set", G_CALLBACK(on_alarm_toggle), a);

        GtkWidget *del_btn = gtk_button_new_from_icon_name("user-trash-symbolic", GTK_ICON_SIZE_BUTTON);
        gtk_button_set_relief(GTK_BUTTON(del_btn), GTK_RELIEF_NONE);
        gtk_widget_set_tooltip_text(del_btn, "Delete Alarm");
        g_signal_connect(del_btn, "clicked", G_CALLBACK(on_delete_alarm), a);

        gtk_box_pack_start(GTK_BOX(actions), a->switch_w, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(actions), del_btn, FALSE, FALSE, 0);
        gtk_box_pack_end(GTK_BOX(card), actions, FALSE, FALSE, 0);

        gtk_container_add(GTK_CONTAINER(row), card);
        gtk_widget_show_all(row);
        gtk_list_box_insert(GTK_LIST_BOX(alarms_listbox), row, -1);
    }
}

static void check_alarms_tick(void)
{
    GDateTime *now = g_date_time_new_now_local();
    int cur_hr = g_date_time_get_hour(now);
    int cur_mn = g_date_time_get_minute(now);
    int cur_dow = g_date_time_get_day_of_week(now); /* 1=Mon .. 7=Sun */
    int cur_yday = g_date_time_get_day_of_year(now);

    for (guint i = 0; i < alarms->len; i++) {
        AlarmItem *a = g_ptr_array_index(alarms, i);
        if (!a->enabled) continue;
        if (a->hour == cur_hr && a->minute == cur_mn) {
            if (a->last_fired_yday == cur_yday) continue;

            gboolean fire = FALSE;
            if (a->repeat_mode == 0) {
                fire = TRUE; /* Every day */
            } else if (a->repeat_mode == 1) {
                fire = (cur_dow >= 1 && cur_dow <= 5); /* Weekdays */
            } else if (a->repeat_mode == 2) {
                fire = (cur_dow == 6 || cur_dow == 7); /* Weekends */
            } else if (a->repeat_mode == 3) {
                fire = TRUE; /* Once */
                a->enabled = FALSE;
                if (a->switch_w) gtk_switch_set_active(GTK_SWITCH(a->switch_w), FALSE);
            } else if (a->repeat_mode == 4) {
                int day_idx = cur_dow - 1; /* 0..6 */
                if (day_idx >= 0 && day_idx < 7 && a->days_active[day_idx])
                    fire = TRUE;
            }

            if (fire) {
                a->last_fired_yday = cur_yday;
                current_ringing_alarm = a;
                char msg[128];
                g_snprintf(msg, sizeof msg, "Alarm Ringing: %s (%02d:%02d)",
                           a->label ? a->label : "Alarm", a->hour, a->minute);
                notify_event("Alarm Alert", msg);

                if (alarm_ringing_revealer && alarm_ringing_lbl) {
                    gtk_label_set_text(GTK_LABEL(alarm_ringing_lbl), msg);
                    gtk_revealer_set_reveal_child(GTK_REVEALER(alarm_ringing_revealer), TRUE);
                }
            }
        }
    }
    g_date_time_unref(now);
}

static GtkWidget *build_alarms_page(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(box), 16);

    /* Alarm Ringing Banner */
    alarm_ringing_revealer = gtk_revealer_new();
    GtkWidget *banner = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_style_context_add_class(gtk_widget_get_style_context(banner), "clock-alert-banner");

    GtkWidget *ring_icon = gtk_image_new_from_icon_name("alarm-symbolic", GTK_ICON_SIZE_BUTTON);
    alarm_ringing_lbl = gtk_label_new("Alarm Alert!");
    gtk_style_context_add_class(gtk_widget_get_style_context(alarm_ringing_lbl), "clock-city-name");

    GtkWidget *snooze_btn = gtk_button_new_with_label("Snooze (+10m)");
    GtkWidget *dismiss_btn = gtk_button_new_with_label("Dismiss");
    gtk_style_context_add_class(gtk_widget_get_style_context(dismiss_btn), "suggested-action");

    g_signal_connect(snooze_btn, "clicked", G_CALLBACK(on_alarm_snooze), NULL);
    g_signal_connect(dismiss_btn, "clicked", G_CALLBACK(on_alarm_dismiss), NULL);

    gtk_box_pack_start(GTK_BOX(banner), ring_icon, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(banner), alarm_ringing_lbl, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(banner), dismiss_btn, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(banner), snooze_btn, FALSE, FALSE, 0);

    gtk_container_add(GTK_CONTAINER(alarm_ringing_revealer), banner);
    gtk_box_pack_start(GTK_BOX(box), alarm_ringing_revealer, FALSE, FALSE, 0);

    /* Top bar: Title + Add Alarm toggle */
    GtkWidget *top_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *title_lbl = gtk_label_new("Configured Alarms");
    gtk_style_context_add_class(gtk_widget_get_style_context(title_lbl), "clock-city-name");

    GtkWidget *new_btn = gtk_button_new_with_label("+ New Alarm");
    gtk_style_context_add_class(gtk_widget_get_style_context(new_btn), "suggested-action");
    g_signal_connect(new_btn, "clicked", G_CALLBACK(on_toggle_alarm_revealer), NULL);

    gtk_box_pack_start(GTK_BOX(top_bar), title_lbl, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(top_bar), new_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), top_bar, FALSE, FALSE, 0);

    /* Add Alarm Form Revealer */
    alarm_add_revealer = gtk_revealer_new();
    GtkWidget *add_card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_style_context_add_class(gtk_widget_get_style_context(add_card), "clock-card");

    GtkWidget *row1 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *time_lbl_prompt = gtk_label_new("Time (24h):");
    alarm_hour_spin = gtk_spin_button_new_with_range(0, 23, 1);
    GtkWidget *colon_lbl = gtk_label_new(":");
    alarm_min_spin = gtk_spin_button_new_with_range(0, 59, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(alarm_hour_spin), 7);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(alarm_min_spin), 30);

    alarm_label_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(alarm_label_entry), "Alarm label (e.g. Wake up, Meeting)");
    gtk_widget_set_hexpand(alarm_label_entry, TRUE);

    gtk_box_pack_start(GTK_BOX(row1), time_lbl_prompt, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row1), alarm_hour_spin, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row1), colon_lbl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row1), alarm_min_spin, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row1), alarm_label_entry, TRUE, TRUE, 8);
    gtk_box_pack_start(GTK_BOX(add_card), row1, FALSE, FALSE, 0);

    /* Row 2: Repeat mode & custom day chips */
    GtkWidget *row2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *rep_lbl = gtk_label_new("Repeat:");
    alarm_repeat_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(alarm_repeat_combo), "Every day");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(alarm_repeat_combo), "Weekdays (Mon-Fri)");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(alarm_repeat_combo), "Weekends (Sat-Sun)");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(alarm_repeat_combo), "Once");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(alarm_repeat_combo), "Custom Days");
    gtk_combo_box_set_active(GTK_COMBO_BOX(alarm_repeat_combo), 0);
    g_signal_connect(alarm_repeat_combo, "changed", G_CALLBACK(on_repeat_combo_changed), NULL);

    gtk_box_pack_start(GTK_BOX(row2), rep_lbl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row2), alarm_repeat_combo, FALSE, FALSE, 0);

    const char *days_short[] = { "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun" };
    for (int i = 0; i < 7; i++) {
        alarm_day_btns[i] = gtk_toggle_button_new_with_label(days_short[i]);
        gtk_widget_set_sensitive(alarm_day_btns[i], FALSE);
        gtk_box_pack_start(GTK_BOX(row2), alarm_day_btns[i], FALSE, FALSE, 0);
    }

    GtkWidget *save_btn = gtk_button_new_with_label("Save Alarm");
    gtk_style_context_add_class(gtk_widget_get_style_context(save_btn), "suggested-action");
    g_signal_connect(save_btn, "clicked", G_CALLBACK(on_save_new_alarm), NULL);
    gtk_box_pack_end(GTK_BOX(row2), save_btn, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(add_card), row2, FALSE, FALSE, 0);

    gtk_container_add(GTK_CONTAINER(alarm_add_revealer), add_card);
    gtk_box_pack_start(GTK_BOX(box), alarm_add_revealer, FALSE, FALSE, 0);

    /* Alarms Scrolled List */
    alarms_listbox = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(alarms_listbox), GTK_SELECTION_NONE);

    GtkWidget *sc = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sc), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(sc), alarms_listbox);
    gtk_box_pack_start(GTK_BOX(box), sc, TRUE, TRUE, 0);

    return box;
}

/* ---------------------------------------------------------------- Stopwatch */
static gint64 sw_elapsed_now(void)
{
    gint64 total = sw_accum_us;
    if (sw_running)
        total += (g_get_monotonic_time() - sw_start_us);
    return total;
}

static void format_sw_us(gint64 us, char *buf, size_t sz)
{
    gint64 total_ms = us / 1000;
    int ms = (int)((total_ms % 1000) / 10); /* 00..99 */
    int s = (int)((total_ms / 1000) % 60);
    int m = (int)((total_ms / 60000) % 60);
    int h = (int)(total_ms / 3600000);
    if (h > 0)
        g_snprintf(buf, sz, "%02d:%02d:%02d.%02d", h, m, s, ms);
    else
        g_snprintf(buf, sz, "%02d:%02d.%02d", m, s, ms);
}

static void refresh_sw_lap_colors(void)
{
    if (!sw_laps || sw_laps->len < 2) return;

    /* Find fastest and slowest split times */
    gint64 min_split = G_MAXINT64;
    gint64 max_split = 0;
    int min_idx = -1;
    int max_idx = -1;

    for (guint i = 0; i < sw_laps->len; i++) {
        LapRecord *r = &g_array_index(sw_laps, LapRecord, i);
        if (r->split_us < min_split) { min_split = r->split_us; min_idx = (int)i; }
        if (r->split_us > max_split) { max_split = r->split_us; max_idx = (int)i; }
    }

    /* Iterate store and update note column */
    GtkTreeIter iter;
    if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(sw_lap_store), &iter)) {
        int rev_idx = (int)sw_laps->len - 1;
        do {
            if (rev_idx == min_idx) {
                gtk_list_store_set(sw_lap_store, &iter, 3, "Fastest", 4, "#2ec27e", -1);
            } else if (rev_idx == max_idx) {
                gtk_list_store_set(sw_lap_store, &iter, 3, "Slowest", 4, "#e01b24", -1);
            } else {
                gtk_list_store_set(sw_lap_store, &iter, 3, "—", 4, NULL, -1);
            }
            rev_idx--;
        } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(sw_lap_store), &iter));
    }
}

static void on_sw_start_pause(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    if (!sw_running) {
        sw_start_us = g_get_monotonic_time();
        sw_running = TRUE;
        gtk_button_set_label(GTK_BUTTON(sw_start_btn), "Pause");
        gtk_style_context_remove_class(gtk_widget_get_style_context(sw_start_btn), "suggested-action");
        gtk_widget_set_sensitive(sw_lap_btn, TRUE);
        gtk_widget_set_sensitive(sw_reset_btn, FALSE);
    } else {
        sw_accum_us += (g_get_monotonic_time() - sw_start_us);
        sw_running = FALSE;
        gtk_button_set_label(GTK_BUTTON(sw_start_btn), "Resume");
        gtk_style_context_add_class(gtk_widget_get_style_context(sw_start_btn), "suggested-action");
        gtk_widget_set_sensitive(sw_lap_btn, FALSE);
        gtk_widget_set_sensitive(sw_reset_btn, TRUE);
    }
}

static void on_sw_lap(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    gint64 total = sw_elapsed_now();
    gint64 split = total - sw_last_lap_us;
    sw_last_lap_us = total;
    sw_lap_count++;

    char split_s[32], total_s[32], num_s[16];
    g_snprintf(num_s, sizeof num_s, "#%02d", sw_lap_count);
    format_sw_us(split, split_s, sizeof split_s);
    format_sw_us(total, total_s, sizeof total_s);

    LapRecord rec = { sw_lap_count, split, total };
    g_array_append_val(sw_laps, rec);

    GtkTreeIter it;
    gtk_list_store_prepend(sw_lap_store, &it);
    gtk_list_store_set(sw_lap_store, &it, 0, num_s, 1, split_s, 2, total_s, 3, "—", 4, NULL, -1);

    refresh_sw_lap_colors();
    gtk_widget_set_sensitive(sw_copy_btn, TRUE);
}

static void on_sw_reset(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    sw_running = FALSE;
    sw_accum_us = 0;
    sw_last_lap_us = 0;
    sw_lap_count = 0;
    gtk_button_set_label(GTK_BUTTON(sw_start_btn), "Start");
    gtk_style_context_add_class(gtk_widget_get_style_context(sw_start_btn), "suggested-action");
    gtk_label_set_text(GTK_LABEL(sw_display_lbl), "00:00.00");
    gtk_label_set_text(GTK_LABEL(sw_split_lbl), "Lap: 00:00.00");
    gtk_widget_set_sensitive(sw_lap_btn, FALSE);
    gtk_widget_set_sensitive(sw_reset_btn, FALSE);
    gtk_widget_set_sensitive(sw_copy_btn, FALSE);
    gtk_list_store_clear(sw_lap_store);
    g_array_set_size(sw_laps, 0);
}

static void on_sw_copy_laps(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    if (!sw_laps || sw_laps->len == 0) return;
    GString *out = g_string_new("Lap,Split Time,Total Time\n");
    for (guint i = 0; i < sw_laps->len; i++) {
        LapRecord *r = &g_array_index(sw_laps, LapRecord, i);
        char s1[32], s2[32];
        format_sw_us(r->split_us, s1, sizeof s1);
        format_sw_us(r->total_us, s2, sizeof s2);
        g_string_append_printf(out, "%d,%s,%s\n", r->lap_num, s1, s2);
    }
    GtkClipboard *cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    if (cb) gtk_clipboard_set_text(cb, out->str, -1);
    g_string_free(out, TRUE);
}

static GtkWidget *build_stopwatch_page(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_container_set_border_width(GTK_CONTAINER(box), 20);

    /* Display Card */
    GtkWidget *top_card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_style_context_add_class(gtk_widget_get_style_context(top_card), "clock-card");

    sw_display_lbl = gtk_label_new("00:00.00");
    gtk_style_context_add_class(gtk_widget_get_style_context(sw_display_lbl), "clock-sw-time");
    gtk_box_pack_start(GTK_BOX(top_card), sw_display_lbl, FALSE, FALSE, 4);

    sw_split_lbl = gtk_label_new("Lap: 00:00.00");
    gtk_style_context_add_class(gtk_widget_get_style_context(sw_split_lbl), "clock-sub");
    gtk_box_pack_start(GTK_BOX(top_card), sw_split_lbl, FALSE, FALSE, 0);

    GtkWidget *btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_halign(btns, GTK_ALIGN_CENTER);

    sw_start_btn = gtk_button_new_with_label("Start");
    gtk_widget_set_size_request(sw_start_btn, 110, 38);
    gtk_style_context_add_class(gtk_widget_get_style_context(sw_start_btn), "suggested-action");

    sw_lap_btn = gtk_button_new_with_label("Lap");
    gtk_widget_set_size_request(sw_lap_btn, 96, 38);
    gtk_widget_set_sensitive(sw_lap_btn, FALSE);

    sw_reset_btn = gtk_button_new_with_label("Reset");
    gtk_widget_set_size_request(sw_reset_btn, 96, 38);
    gtk_widget_set_sensitive(sw_reset_btn, FALSE);

    sw_copy_btn = gtk_button_new_with_label("Copy Laps");
    gtk_widget_set_size_request(sw_copy_btn, 100, 38);
    gtk_widget_set_sensitive(sw_copy_btn, FALSE);

    g_signal_connect(sw_start_btn, "clicked", G_CALLBACK(on_sw_start_pause), NULL);
    g_signal_connect(sw_lap_btn, "clicked", G_CALLBACK(on_sw_lap), NULL);
    g_signal_connect(sw_reset_btn, "clicked", G_CALLBACK(on_sw_reset), NULL);
    g_signal_connect(sw_copy_btn, "clicked", G_CALLBACK(on_sw_copy_laps), NULL);

    gtk_box_pack_start(GTK_BOX(btns), sw_start_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(btns), sw_lap_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(btns), sw_reset_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(btns), sw_copy_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(top_card), btns, FALSE, FALSE, 4);

    gtk_box_pack_start(GTK_BOX(box), top_card, FALSE, FALSE, 0);

    /* Laps Table */
    sw_laps = g_array_new(FALSE, FALSE, sizeof(LapRecord));
    sw_lap_store = gtk_list_store_new(5, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    sw_lap_treeview = gtk_tree_view_new_with_model(GTK_TREE_MODEL(sw_lap_store));
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(sw_lap_treeview), TRUE);

    const char *col_titles[] = { "Lap", "Split Time", "Total Time", "Remark" };
    for (int i = 0; i < 4; i++) {
        GtkCellRenderer *r = gtk_cell_renderer_text_new();
        if (i == 3) {
            GtkTreeViewColumn *c = gtk_tree_view_column_new_with_attributes(col_titles[i], r, "text", i, "foreground", 4, NULL);
            gtk_tree_view_column_set_expand(c, TRUE);
            gtk_tree_view_append_column(GTK_TREE_VIEW(sw_lap_treeview), c);
        } else {
            GtkTreeViewColumn *c = gtk_tree_view_column_new_with_attributes(col_titles[i], r, "text", i, NULL);
            gtk_tree_view_column_set_expand(c, TRUE);
            gtk_tree_view_append_column(GTK_TREE_VIEW(sw_lap_treeview), c);
        }
    }

    GtkWidget *sc = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sc), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(sc), sw_lap_treeview);
    gtk_box_pack_start(GTK_BOX(box), sc, TRUE, TRUE, 0);

    return box;
}

/* ---------------------------------------------------------------- Timer */
static gboolean draw_timer_ring(GtkWidget *w, cairo_t *cr, gpointer d)
{
    (void)d;
    int width = gtk_widget_get_allocated_width(w);
    int height = gtk_widget_get_allocated_height(w);
    double cx = width / 2.0;
    double cy = height / 2.0;
    double radius = MIN(width, height) / 2.0 - 18.0;
    if (radius < 30) return FALSE;

    HdeThemeInfo ti;
    hde_theme_info_load(&ti);
    GdkRGBA accent = { 0.21, 0.52, 0.89, 1.0 };
    if (ti.accent) gdk_rgba_parse(&accent, ti.accent);
    gboolean dark = hde_theme_shell_dark();
    hde_theme_info_clear(&ti);

    double fg = dark ? 0.95 : 0.12;

    /* Background track ring */
    cairo_set_line_width(cr, 12.0);
    cairo_set_source_rgba(cr, fg, fg, fg, 0.10);
    cairo_arc(cr, cx, cy, radius, 0, 2 * G_PI);
    cairo_stroke(cr);

    /* Progress arc */
    double frac = (tmr_total_ms > 0) ? CLAMP((double)tmr_remain_ms / (double)tmr_total_ms, 0.0, 1.0) : 1.0;
    if (frac > 0.001) {
        cairo_set_source_rgba(cr, accent.red, accent.green, accent.blue, 0.95);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_arc(cr, cx, cy, radius, -G_PI / 2.0, -G_PI / 2.0 + frac * 2.0 * G_PI);
        cairo_stroke(cr);
    }

    /* Digital time readout inside ring */
    gint64 rem_s = (tmr_remain_ms + 999) / 1000;
    if (tmr_total_ms == 0 && !tmr_running) {
        int h = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(tmr_hour_spin));
        int m = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(tmr_min_spin));
        int s = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(tmr_sec_spin));
        rem_s = h * 3600 + m * 60 + s;
    }

    char buf[32];
    g_snprintf(buf, sizeof buf, "%02lld:%02lld:%02lld",
               (long long)(rem_s / 3600), (long long)((rem_s / 60) % 60), (long long)(rem_s % 60));

    cairo_select_font_face(cr, "monospace", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 32.0);
    cairo_text_extents_t ext;
    cairo_text_extents(cr, buf, &ext);
    cairo_set_source_rgba(cr, fg, fg, fg, 0.95);
    cairo_move_to(cr, cx - ext.width / 2.0 - ext.x_bearing, cy - ext.height / 2.0 - ext.y_bearing);
    cairo_show_text(cr, buf);

    /* Timer Title below digital readout */
    const char *lbl = tmr_title && *tmr_title ? tmr_title : "Countdown Timer";
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 13.0);
    cairo_text_extents_t ext2;
    cairo_text_extents(cr, lbl, &ext2);
    cairo_set_source_rgba(cr, fg, fg, fg, 0.65);
    cairo_move_to(cr, cx - ext2.width / 2.0 - ext2.x_bearing, cy + ext.height + 12.0);
    cairo_show_text(cr, lbl);

    return FALSE;
}

static void set_timer_preset_sec(int total_sec, const char *label)
{
    tmr_running = FALSE;
    tmr_total_ms = (gint64)total_sec * 1000;
    tmr_remain_ms = tmr_total_ms;
    g_free(tmr_title);
    tmr_title = g_strdup(label);

    gtk_spin_button_set_value(GTK_SPIN_BUTTON(tmr_hour_spin), total_sec / 3600);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(tmr_min_spin), (total_sec / 60) % 60);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(tmr_sec_spin), total_sec % 60);
    gtk_entry_set_text(GTK_ENTRY(tmr_name_entry), label);
    gtk_button_set_label(GTK_BUTTON(tmr_start_btn), "Start");
    gtk_style_context_add_class(gtk_widget_get_style_context(tmr_start_btn), "suggested-action");

    if (tmr_ring_area) gtk_widget_queue_draw(tmr_ring_area);
}

static void on_tmr_preset_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    int sec = GPOINTER_TO_INT(d);
    char buf[32];
    if (sec >= 3600) g_snprintf(buf, sizeof buf, "%d Hour Timer", sec / 3600);
    else g_snprintf(buf, sizeof buf, "%d Min Timer", sec / 60);
    set_timer_preset_sec(sec, buf);
}

static void on_tmr_start_pause(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    if (!tmr_running) {
        if (tmr_remain_ms <= 0) {
            int h = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(tmr_hour_spin));
            int m = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(tmr_min_spin));
            int s = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(tmr_sec_spin));
            tmr_total_ms = ((gint64)h * 3600 + (gint64)m * 60 + s) * 1000;
            tmr_remain_ms = tmr_total_ms;
            const char *name = gtk_entry_get_text(GTK_ENTRY(tmr_name_entry));
            g_free(tmr_title);
            tmr_title = (name && *name) ? g_strdup(name) : g_strdup("Timer");
        }
        if (tmr_remain_ms <= 0) return;
        tmr_last_tick_us = g_get_monotonic_time();
        tmr_running = TRUE;
        gtk_button_set_label(GTK_BUTTON(tmr_start_btn), "Pause");
        gtk_style_context_remove_class(gtk_widget_get_style_context(tmr_start_btn), "suggested-action");
    } else {
        tmr_running = FALSE;
        gtk_button_set_label(GTK_BUTTON(tmr_start_btn), "Resume");
        gtk_style_context_add_class(gtk_widget_get_style_context(tmr_start_btn), "suggested-action");
    }
    if (tmr_ring_area) gtk_widget_queue_draw(tmr_ring_area);
}

static void on_tmr_add_minutes(GtkButton *b, gpointer d)
{
    (void)b;
    int mins = GPOINTER_TO_INT(d);
    tmr_total_ms += (gint64)mins * 60000;
    tmr_remain_ms += (gint64)mins * 60000;
    if (tmr_ring_area) gtk_widget_queue_draw(tmr_ring_area);
}

static void on_tmr_reset(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    tmr_running = FALSE;
    tmr_remain_ms = tmr_total_ms;
    gtk_button_set_label(GTK_BUTTON(tmr_start_btn), "Start");
    gtk_style_context_add_class(gtk_widget_get_style_context(tmr_start_btn), "suggested-action");
    if (tmr_ring_area) gtk_widget_queue_draw(tmr_ring_area);
}

static void on_tmr_dismiss(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    if (tmr_ringing_revealer)
        gtk_revealer_set_reveal_child(GTK_REVEALER(tmr_ringing_revealer), FALSE);
}

static void on_tmr_repeat(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    if (tmr_ringing_revealer)
        gtk_revealer_set_reveal_child(GTK_REVEALER(tmr_ringing_revealer), FALSE);
    tmr_remain_ms = tmr_total_ms;
    tmr_last_tick_us = g_get_monotonic_time();
    tmr_running = TRUE;
    gtk_button_set_label(GTK_BUTTON(tmr_start_btn), "Pause");
    if (tmr_ring_area) gtk_widget_queue_draw(tmr_ring_area);
}

static GtkWidget *build_timer_page(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_container_set_border_width(GTK_CONTAINER(box), 20);

    /* Ringing Alert Banner */
    tmr_ringing_revealer = gtk_revealer_new();
    GtkWidget *banner = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_style_context_add_class(gtk_widget_get_style_context(banner), "clock-alert-banner");

    GtkWidget *bell = gtk_image_new_from_icon_name("alarm-symbolic", GTK_ICON_SIZE_BUTTON);
    tmr_ringing_lbl = gtk_label_new("Timer finished!");
    gtk_style_context_add_class(gtk_widget_get_style_context(tmr_ringing_lbl), "clock-city-name");

    GtkWidget *repeat_btn = gtk_button_new_with_label("Repeat");
    GtkWidget *dismiss_btn = gtk_button_new_with_label("Dismiss");
    gtk_style_context_add_class(gtk_widget_get_style_context(dismiss_btn), "suggested-action");

    g_signal_connect(repeat_btn, "clicked", G_CALLBACK(on_tmr_repeat), NULL);
    g_signal_connect(dismiss_btn, "clicked", G_CALLBACK(on_tmr_dismiss), NULL);

    gtk_box_pack_start(GTK_BOX(banner), bell, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(banner), tmr_ringing_lbl, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(banner), dismiss_btn, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(banner), repeat_btn, FALSE, FALSE, 0);

    gtk_container_add(GTK_CONTAINER(tmr_ringing_revealer), banner);
    gtk_box_pack_start(GTK_BOX(box), tmr_ringing_revealer, FALSE, FALSE, 0);

    /* Main Card */
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 24);
    gtk_style_context_add_class(gtk_widget_get_style_context(card), "clock-card");

    /* Left: Cairo Ring Area */
    GtkWidget *ring_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    tmr_ring_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(tmr_ring_area, 260, 260);
    g_signal_connect(tmr_ring_area, "draw", G_CALLBACK(draw_timer_ring), NULL);
    gtk_box_pack_start(GTK_BOX(ring_box), tmr_ring_area, TRUE, TRUE, 0);

    /* Under ring: +1m and +5m quick adjustments */
    GtkWidget *adj_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_halign(adj_box, GTK_ALIGN_CENTER);
    GtkWidget *p1_btn = gtk_button_new_with_label("+1 Min");
    GtkWidget *p5_btn = gtk_button_new_with_label("+5 Min");
    g_signal_connect(p1_btn, "clicked", G_CALLBACK(on_tmr_add_minutes), GINT_TO_POINTER(1));
    g_signal_connect(p5_btn, "clicked", G_CALLBACK(on_tmr_add_minutes), GINT_TO_POINTER(5));
    gtk_box_pack_start(GTK_BOX(adj_box), p1_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(adj_box), p5_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ring_box), adj_box, FALSE, FALSE, 4);

    gtk_box_pack_start(GTK_BOX(card), ring_box, FALSE, FALSE, 0);

    /* Right: Controls, Presets, Custom Input */
    GtkWidget *ctrl_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_widget_set_hexpand(ctrl_box, TRUE);

    GtkWidget *preset_title = gtk_label_new("Quick Timer Presets");
    gtk_label_set_xalign(GTK_LABEL(preset_title), 0.0);
    gtk_style_context_add_class(gtk_widget_get_style_context(preset_title), "clock-city-name");
    gtk_box_pack_start(GTK_BOX(ctrl_box), preset_title, FALSE, FALSE, 0);

    GtkWidget *presets_flow = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(presets_flow), GTK_SELECTION_NONE);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(presets_flow), 4);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(presets_flow), 8);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(presets_flow), 8);

    const struct { const char *name; int sec; } p_list[] = {
        { "1 Min", 60 },
        { "3 Min", 180 },
        { "5 Min", 300 },
        { "10 Min", 600 },
        { "15 Min", 900 },
        { "25 Min (Pomodoro)", 1500 },
        { "30 Min", 1800 },
        { "1 Hour", 3600 }
    };

    for (size_t i = 0; i < G_N_ELEMENTS(p_list); i++) {
        GtkWidget *chip = gtk_button_new_with_label(p_list[i].name);
        gtk_style_context_add_class(gtk_widget_get_style_context(chip), "clock-chip-btn");
        g_signal_connect(chip, "clicked", G_CALLBACK(on_tmr_preset_clicked), GINT_TO_POINTER(p_list[i].sec));
        gtk_container_add(GTK_CONTAINER(presets_flow), chip);
    }
    gtk_box_pack_start(GTK_BOX(ctrl_box), presets_flow, FALSE, FALSE, 0);

    /* Custom Timer Section */
    GtkWidget *custom_title = gtk_label_new("Custom Timer");
    gtk_label_set_xalign(GTK_LABEL(custom_title), 0.0);
    gtk_style_context_add_class(gtk_widget_get_style_context(custom_title), "clock-city-name");
    gtk_box_pack_start(GTK_BOX(ctrl_box), custom_title, FALSE, FALSE, 2);

    GtkWidget *spin_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    tmr_hour_spin = gtk_spin_button_new_with_range(0, 99, 1);
    tmr_min_spin = gtk_spin_button_new_with_range(0, 59, 1);
    tmr_sec_spin = gtk_spin_button_new_with_range(0, 59, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(tmr_min_spin), 5);

    gtk_box_pack_start(GTK_BOX(spin_row), tmr_hour_spin, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(spin_row), gtk_label_new("hr"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(spin_row), tmr_min_spin, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(spin_row), gtk_label_new("min"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(spin_row), tmr_sec_spin, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(spin_row), gtk_label_new("sec"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ctrl_box), spin_row, FALSE, FALSE, 0);

    tmr_name_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(tmr_name_entry), "Timer label (e.g. Baking, Reading)");
    gtk_box_pack_start(GTK_BOX(ctrl_box), tmr_name_entry, FALSE, FALSE, 0);

    /* Primary Action Buttons */
    GtkWidget *action_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    tmr_start_btn = gtk_button_new_with_label("Start");
    gtk_widget_set_size_request(tmr_start_btn, 120, 38);
    gtk_style_context_add_class(gtk_widget_get_style_context(tmr_start_btn), "suggested-action");

    tmr_reset_btn = gtk_button_new_with_label("Reset");
    gtk_widget_set_size_request(tmr_reset_btn, 100, 38);

    g_signal_connect(tmr_start_btn, "clicked", G_CALLBACK(on_tmr_start_pause), NULL);
    g_signal_connect(tmr_reset_btn, "clicked", G_CALLBACK(on_tmr_reset), NULL);

    gtk_box_pack_start(GTK_BOX(action_row), tmr_start_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(action_row), tmr_reset_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ctrl_box), action_row, FALSE, FALSE, 6);

    gtk_box_pack_start(GTK_BOX(card), ctrl_box, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), card, FALSE, FALSE, 0);

    return box;
}

/* ---------------------------------------------------------------- Fast & Slow Ticks */
static gboolean on_second_tick(gpointer d)
{
    (void)d;
    update_world_zones_ui();
    check_alarms_tick();
    return G_SOURCE_CONTINUE;
}

static gboolean on_fast_tick(gpointer d)
{
    (void)d;
    /* Stopwatch update */
    if (sw_running && sw_display_lbl) {
        gint64 cur = sw_elapsed_now();
        char buf[32];
        format_sw_us(cur, buf, sizeof buf);
        gtk_label_set_text(GTK_LABEL(sw_display_lbl), buf);

        if (sw_split_lbl) {
            char split_b[32];
            format_sw_us(cur - sw_last_lap_us, split_b, sizeof split_b);
            char lap_msg[48];
            g_snprintf(lap_msg, sizeof lap_msg, "Current Lap: %s", split_b);
            gtk_label_set_text(GTK_LABEL(sw_split_lbl), lap_msg);
        }
    }

    /* Timer countdown */
    if (tmr_running) {
        gint64 now_us = g_get_monotonic_time();
        gint64 delta_ms = (now_us - tmr_last_tick_us) / 1000;
        tmr_last_tick_us = now_us;
        tmr_remain_ms -= delta_ms;

        if (tmr_remain_ms <= 0) {
            tmr_remain_ms = 0;
            tmr_running = FALSE;
            gtk_button_set_label(GTK_BUTTON(tmr_start_btn), "Start");
            gtk_style_context_add_class(gtk_widget_get_style_context(tmr_start_btn), "suggested-action");

            char msg[128];
            g_snprintf(msg, sizeof msg, "Timer completed: %s", tmr_title ? tmr_title : "Timer");
            notify_event("Timer Finished", msg);

            if (tmr_ringing_revealer && tmr_ringing_lbl) {
                gtk_label_set_text(GTK_LABEL(tmr_ringing_lbl), msg);
                gtk_revealer_set_reveal_child(GTK_REVEALER(tmr_ringing_revealer), TRUE);
            }
        }
        if (tmr_ring_area) gtk_widget_queue_draw(tmr_ring_area);
    }

    return G_SOURCE_CONTINUE;
}

/* ---------------------------------------------------------------- Persistence */
static void save_state(void)
{
    GKeyFile *kf = g_key_file_new();
    if (world_zones) {
        GPtrArray *cities = g_ptr_array_new();
        GPtrArray *countries = g_ptr_array_new();
        GPtrArray *tzs = g_ptr_array_new();
        for (guint i = 0; i < world_zones->len; i++) {
            WorldZone *z = g_ptr_array_index(world_zones, i);
            g_ptr_array_add(cities, z->city);
            g_ptr_array_add(countries, z->country ? z->country : "");
            g_ptr_array_add(tzs, z->tz_id);
        }
        g_key_file_set_string_list(kf, "world", "cities", (const gchar *const *)cities->pdata, cities->len);
        g_key_file_set_string_list(kf, "world", "countries", (const gchar *const *)countries->pdata, countries->len);
        g_key_file_set_string_list(kf, "world", "tzs", (const gchar *const *)tzs->pdata, tzs->len);
        g_ptr_array_free(cities, TRUE);
        g_ptr_array_free(countries, TRUE);
        g_ptr_array_free(tzs, TRUE);
    }

    if (alarms) {
        g_key_file_set_integer(kf, "alarms", "count", (int)alarms->len);
        for (guint i = 0; i < alarms->len; i++) {
            AlarmItem *a = g_ptr_array_index(alarms, i);
            char grp[32];
            g_snprintf(grp, sizeof grp, "alarm_%u", i);
            g_key_file_set_integer(kf, grp, "hour", a->hour);
            g_key_file_set_integer(kf, grp, "minute", a->minute);
            g_key_file_set_string(kf, grp, "label", a->label ? a->label : "Alarm");
            g_key_file_set_integer(kf, grp, "repeat", a->repeat_mode);
            g_key_file_set_boolean(kf, grp, "enabled", a->enabled);
            for (int d = 0; d < 7; d++) {
                char k[16];
                g_snprintf(k, sizeof k, "day_%d", d);
                g_key_file_set_boolean(kf, grp, k, a->days_active[d]);
            }
        }
    }

    char *path = clock_ini_path();
    char *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0755);
    g_key_file_save_to_file(kf, path, NULL);
    g_free(dir);
    g_free(path);
    g_key_file_free(kf);
}

static void load_saved_state(void)
{
    GKeyFile *kf = g_key_file_new();
    char *path = clock_ini_path();
    gboolean ok = g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL);
    g_free(path);

    gsize nc = 0, nco = 0, nt = 0;
    char **cities = ok ? g_key_file_get_string_list(kf, "world", "cities", &nc, NULL) : NULL;
    char **countries = ok ? g_key_file_get_string_list(kf, "world", "countries", &nco, NULL) : NULL;
    char **tzs = ok ? g_key_file_get_string_list(kf, "world", "tzs", &nt, NULL) : NULL;

    if (cities && tzs && nc > 0 && nc == nt) {
        for (gsize i = 0; i < nc; i++) {
            const char *co = (countries && i < nco) ? countries[i] : "";
            add_world_zone_card(cities[i], co, tzs[i], FALSE);
        }
    } else {
        add_world_zone_card("London", "United Kingdom", "Europe/London", FALSE);
        add_world_zone_card("New York", "United States", "America/New_York", FALSE);
        add_world_zone_card("Tokyo", "Japan", "Asia/Tokyo", FALSE);
        add_world_zone_card("Ho Chi Minh City", "Vietnam", "Asia/Ho_Chi_Minh", FALSE);
    }
    g_strfreev(cities);
    g_strfreev(countries);
    g_strfreev(tzs);

    int n_alarms = ok ? g_key_file_get_integer(kf, "alarms", "count", NULL) : 0;
    for (int i = 0; i < n_alarms; i++) {
        char grp[32];
        g_snprintf(grp, sizeof grp, "alarm_%d", i);
        if (!g_key_file_has_group(kf, grp)) continue;
        AlarmItem *a = g_new0(AlarmItem, 1);
        a->hour = CLAMP(g_key_file_get_integer(kf, grp, "hour", NULL), 0, 23);
        a->minute = CLAMP(g_key_file_get_integer(kf, grp, "minute", NULL), 0, 59);
        a->label = g_key_file_get_string(kf, grp, "label", NULL);
        if (!a->label) a->label = g_strdup("Alarm");
        a->repeat_mode = CLAMP(g_key_file_get_integer(kf, grp, "repeat", NULL), 0, 4);
        a->enabled = g_key_file_get_boolean(kf, grp, "enabled", NULL);
        a->last_fired_yday = -1;
        for (int d = 0; d < 7; d++) {
            char k[16];
            g_snprintf(k, sizeof k, "day_%d", d);
            a->days_active[d] = g_key_file_get_boolean(kf, grp, k, NULL);
        }
        g_ptr_array_add(alarms, a);
    }
    rebuild_alarms_list();
    g_key_file_free(kf);
}

/* ---------------------------------------------------------------- Dialogs & Events */
static void show_about_dialog(void)
{
    const char *authors[] = { "Hyggshi OS Research Technology", NULL };
    gtk_show_about_dialog(GTK_WINDOW(main_win),
        "program-name", "Hyggshi Clock",
        "version", HDE_VERSION,
        "comments", "Modern World Clock, Alarms, Stopwatch, and Countdown Timer for HDE.",
        "website", "https://github.com/Hyggshi-OS-Research-Technology/NexWM",
        "website-label", "NexWM / HDE Repository",
        "copyright", "Copyright (c) 2024-2026 Hyggshi OS Research Technology",
        "license-type", GTK_LICENSE_MIT_X11,
        "authors", authors,
        "logo-icon-name", "preferences-system-time",
        NULL);
}

static void on_about_clicked(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    show_about_dialog();
}

static gboolean on_clock_key(GtkWidget *w, GdkEventKey *ev, gpointer d)
{
    (void)w; (void)d;
    if (ev->keyval == GDK_KEY_F1) {
        show_about_dialog();
        return TRUE;
    }
    const char *visible = gtk_stack_get_visible_child_name(GTK_STACK(stack));
    if (visible && !strcmp(visible, "stopwatch")) {
        if (ev->keyval == GDK_KEY_space) {
            on_sw_start_pause(NULL, NULL);
            return TRUE;
        } else if (ev->keyval == GDK_KEY_l || ev->keyval == GDK_KEY_L) {
            if (sw_running) on_sw_lap(NULL, NULL);
            return TRUE;
        } else if (ev->keyval == GDK_KEY_r || ev->keyval == GDK_KEY_R) {
            if (!sw_running && sw_accum_us > 0) on_sw_reset(NULL, NULL);
            return TRUE;
        }
    }
    return FALSE;
}

/* ---------------------------------------------------------------- Main Entry Point */
int main(int argc, char **argv)
{
    const char *start_page = "world";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--version") || !strcmp(argv[i], "-v")) {
            printf("hde-clock (%s) %s %s\n", APP_TITLE, HDE_RELEASE, HDE_VERSION);
            return 0;
        }
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            printf("Usage: hde-clock [--world | --alarms | --stopwatch | --timer]\n"
                   "Hyggshi Clock — World Clock, Alarms, Stopwatch, and Timer for HDE.\n");
            return 0;
        }
        if (!strcmp(argv[i], "--world")) start_page = "world";
        else if (!strcmp(argv[i], "--alarms")) start_page = "alarms";
        else if (!strcmp(argv[i], "--stopwatch")) start_page = "stopwatch";
        else if (!strcmp(argv[i], "--timer")) start_page = "timer";
    }

    gtk_init(&argc, &argv);
    hde_theme_apply_process();
    load_css();
    hde_theme_watch(on_theme_changed, NULL);

    world_zones = g_ptr_array_new_with_free_func(world_zone_free);
    alarms = g_ptr_array_new_with_free_func(alarm_free);

    main_win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(main_win), APP_TITLE);
    gtk_window_set_icon_name(GTK_WINDOW(main_win), "preferences-system-time");
    gtk_window_set_default_size(GTK_WINDOW(main_win), 840, 520);
    g_signal_connect(main_win, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    g_signal_connect(main_win, "key-press-event", G_CALLBACK(on_clock_key), NULL);

    stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(stack), GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT_RIGHT);

    header_bar = gtk_header_bar_new();
    gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(header_bar), TRUE);
    gtk_header_bar_set_title(GTK_HEADER_BAR(header_bar), APP_TITLE);

    stack_switcher = gtk_stack_switcher_new();
    gtk_stack_switcher_set_stack(GTK_STACK_SWITCHER(stack_switcher), GTK_STACK(stack));
    gtk_header_bar_set_custom_title(GTK_HEADER_BAR(header_bar), stack_switcher);

    GtkWidget *about_btn = gtk_button_new_from_icon_name("help-about-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(about_btn, "About Hyggshi Clock (F1)");
    g_signal_connect(about_btn, "clicked", G_CALLBACK(on_about_clicked), NULL);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header_bar), about_btn);

    gtk_window_set_titlebar(GTK_WINDOW(main_win), header_bar);

    gtk_stack_add_titled(GTK_STACK(stack), build_world_page(), "world", "World Clock");
    gtk_stack_add_titled(GTK_STACK(stack), build_alarms_page(), "alarms", "Alarms");
    gtk_stack_add_titled(GTK_STACK(stack), build_stopwatch_page(), "stopwatch", "Stopwatch");
    gtk_stack_add_titled(GTK_STACK(stack), build_timer_page(), "timer", "Timer");

    gtk_container_add(GTK_CONTAINER(main_win), stack);
    load_saved_state();
    update_world_zones_ui();

    gtk_widget_show_all(main_win);
    gtk_stack_set_visible_child_name(GTK_STACK(stack), start_page);

    g_timeout_add(1000, on_second_tick, NULL);
    g_timeout_add(35, on_fast_tick, NULL);

    gtk_main();
    return 0;
}
