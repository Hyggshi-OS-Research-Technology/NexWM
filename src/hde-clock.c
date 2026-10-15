/* hde-clock.c — Hyggshi Clock: World Clock, Alarms, Stopwatch, and Timer for HDE (GTK3). */
#define _POSIX_C_SOURCE 200809L

#include "hde-build.h"
#include "hde-theme.h"
#include <gtk/gtk.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define APP_TITLE "Hyggshi Clock"

typedef struct {
    char *city;
    char *tz_id;
    GtkWidget *row;
    GtkWidget *time_lbl;
    GtkWidget *sub_lbl;
} WorldZone;

typedef struct {
    int hour;
    int minute;
    char *label;
    int repeat_mode; /* 0 = Every day, 1 = Weekdays, 2 = Weekends, 3 = Once */
    gboolean enabled;
    int last_fired_yday;
} AlarmItem;

static GtkCssProvider *app_css;
static GtkWidget *main_win;
static GtkWidget *stack;

/* World Clock widgets */
static GtkWidget *analog_area;
static GtkWidget *local_time_lbl;
static GtkWidget *local_date_lbl;
static GtkWidget *local_tz_lbl;
static GtkWidget *zones_list;
static GtkWidget *zone_combo;
static GPtrArray *world_zones; /* WorldZone* */

/* Alarms widgets */
static GPtrArray *alarms; /* AlarmItem* */
static GtkWidget *alarms_list;
static GtkWidget *alarm_hour_spin;
static GtkWidget *alarm_min_spin;
static GtkWidget *alarm_label_entry;
static GtkWidget *alarm_repeat_combo;
static GtkWidget *alarm_banner_revealer;
static GtkWidget *alarm_banner_label;

/* Stopwatch state & widgets */
static gboolean sw_running;
static gint64 sw_start_us;
static gint64 sw_accum_us;
static gint64 sw_last_lap_us;
static int sw_lap_count;
static GtkWidget *sw_display_lbl;
static GtkWidget *sw_start_btn;
static GtkWidget *sw_lap_btn;
static GtkWidget *sw_reset_btn;
static GtkListStore *sw_lap_store;

/* Timer state & widgets */
static gboolean tmr_running;
static gint64 tmr_total_ms;
static gint64 tmr_remain_ms;
static gint64 tmr_last_tick_us;
static GtkWidget *tmr_ring_area;
static GtkWidget *tmr_hour_spin;
static GtkWidget *tmr_min_spin;
static GtkWidget *tmr_sec_spin;
static GtkWidget *tmr_name_entry;
static GtkWidget *tmr_start_btn;
static GtkWidget *tmr_reset_btn;

static const struct {
    const char *city;
    const char *tz;
} preset_zones[] = {
    { "UTC (Coordinated Universal Time)", "UTC" },
    { "New York (United States)", "America/New_York" },
    { "Los Angeles (United States)", "America/Los_Angeles" },
    { "Chicago (United States)", "America/Chicago" },
    { "Toronto (Canada)", "America/Toronto" },
    { "Sao Paulo (Brazil)", "America/Sao_Paulo" },
    { "London (United Kingdom)", "Europe/London" },
    { "Paris (France)", "Europe/Paris" },
    { "Berlin (Germany)", "Europe/Berlin" },
    { "Madrid (Spain)", "Europe/Madrid" },
    { "Rome (Italy)", "Europe/Rome" },
    { "Moscow (Russia)", "Europe/Moscow" },
    { "Cairo (Egypt)", "Africa/Cairo" },
    { "Dubai (UAE)", "Asia/Dubai" },
    { "Mumbai / New Delhi (India)", "Asia/Kolkata" },
    { "Bangkok (Thailand)", "Asia/Bangkok" },
    { "Ho Chi Minh City (Vietnam)", "Asia/Ho_Chi_Minh" },
    { "Singapore", "Asia/Singapore" },
    { "Hong Kong", "Asia/Hong_Kong" },
    { "Beijing / Shanghai (China)", "Asia/Shanghai" },
    { "Seoul (South Korea)", "Asia/Seoul" },
    { "Tokyo (Japan)", "Asia/Tokyo" },
    { "Sydney (Australia)", "Australia/Sydney" },
    { "Auckland (New Zealand)", "Pacific/Auckland" }
};

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
    char *css = g_strdup_printf(
        ".clock-card { background-color: alpha(@theme_fg_color, 0.04); border: 1px solid alpha(@theme_fg_color, 0.10);"
        "  border-radius: 12px; padding: 14px; }"
        ".clock-big-time { font-family: monospace; font-size: 38px; font-weight: 700; }"
        ".clock-sw-time { font-family: monospace; font-size: 46px; font-weight: 700; }"
        ".clock-city-title { font-size: 15px; font-weight: 600; }"
        ".clock-city-time { font-family: monospace; font-size: 22px; font-weight: 700; color: %s; }"
        ".clock-sub { font-size: 12px; opacity: 0.72; }"
        ".clock-alarm-time { font-family: monospace; font-size: 24px; font-weight: 700; }"
        ".clock-banner { background-color: alpha(%s, 0.15); border: 1px solid %s; border-radius: 10px; padding: 10px 14px; }"
        "%s",
        ac, ac, ac, accent);
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

/* ---------------------------------------------------------------- Persistence */
static void save_state(void)
{
    GKeyFile *kf = g_key_file_new();
    if (world_zones) {
        GPtrArray *cities = g_ptr_array_new();
        GPtrArray *tzs = g_ptr_array_new();
        for (guint i = 0; i < world_zones->len; i++) {
            WorldZone *z = g_ptr_array_index(world_zones, i);
            g_ptr_array_add(cities, z->city);
            g_ptr_array_add(tzs, z->tz_id);
        }
        g_key_file_set_string_list(kf, "world", "cities", (const gchar *const *)cities->pdata, cities->len);
        g_key_file_set_string_list(kf, "world", "tzs", (const gchar *const *)tzs->pdata, tzs->len);
        g_ptr_array_free(cities, TRUE);
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

/* ---------------------------------------------------------------- World Clock */
static void world_zone_free(gpointer p)
{
    WorldZone *z = p;
    if (!z) return;
    g_free(z->city);
    g_free(z->tz_id);
    g_free(z);
}

static gboolean draw_analog_clock(GtkWidget *w, cairo_t *cr, gpointer d)
{
    (void)d;
    int width = gtk_widget_get_allocated_width(w);
    int height = gtk_widget_get_allocated_height(w);
    double cx = width / 2.0;
    double cy = height / 2.0;
    double radius = MIN(width, height) / 2.0 - 12.0;
    if (radius < 20) return FALSE;

    HdeThemeInfo ti;
    hde_theme_info_load(&ti);
    GdkRGBA accent = { 0.21, 0.52, 0.89, 1.0 };
    if (ti.accent) gdk_rgba_parse(&accent, ti.accent);
    gboolean dark = hde_theme_shell_dark();
    hde_theme_info_clear(&ti);

    double fg = dark ? 0.92 : 0.15;

    /* Dial background */
    cairo_arc(cr, cx, cy, radius, 0, 2 * G_PI);
    cairo_set_source_rgba(cr, fg, fg, fg, 0.05);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, fg, fg, fg, 0.22);
    cairo_set_line_width(cr, 2.2);
    cairo_stroke(cr);

    /* Hour and minute ticks */
    for (int i = 0; i < 60; i++) {
        double angle = i * G_PI / 30.0 - G_PI / 2.0;
        gboolean is_hour = (i % 5) == 0;
        double r_outer = radius - 6.0;
        double r_inner = r_outer - (is_hour ? 10.0 : 4.0);
        cairo_set_source_rgba(cr, fg, fg, fg, is_hour ? 0.75 : 0.28);
        cairo_set_line_width(cr, is_hour ? 2.2 : 1.0);
        cairo_move_to(cr, cx + cos(angle) * r_inner, cy + sin(angle) * r_inner);
        cairo_line_to(cr, cx + cos(angle) * r_outer, cy + sin(angle) * r_outer);
        cairo_stroke(cr);
    }

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
    cairo_set_source_rgba(cr, fg, fg, fg, 0.90);
    cairo_set_line_width(cr, 4.2);
    cairo_move_to(cr, cx, cy);
    cairo_line_to(cr, cx + cos(h_angle) * (radius * 0.52), cy + sin(h_angle) * (radius * 0.52));
    cairo_stroke(cr);

    /* Minute hand */
    cairo_set_source_rgba(cr, fg, fg, fg, 0.85);
    cairo_set_line_width(cr, 3.0);
    cairo_move_to(cr, cx, cy);
    cairo_line_to(cr, cx + cos(m_angle) * (radius * 0.74), cy + sin(m_angle) * (radius * 0.74));
    cairo_stroke(cr);

    /* Second hand */
    cairo_set_source_rgba(cr, accent.red, accent.green, accent.blue, 0.95);
    cairo_set_line_width(cr, 1.8);
    cairo_move_to(cr, cx - cos(s_angle) * (radius * 0.16), cy - sin(s_angle) * (radius * 0.16));
    cairo_line_to(cr, cx + cos(s_angle) * (radius * 0.82), cy + sin(s_angle) * (radius * 0.82));
    cairo_stroke(cr);

    /* Center cap */
    cairo_arc(cr, cx, cy, 4.5, 0, 2 * G_PI);
    cairo_set_source_rgba(cr, accent.red, accent.green, accent.blue, 1.0);
    cairo_fill(cr);

    return FALSE;
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
    char *tz_str = g_strdup_printf("Local Time (%s, UTC%+03d:%02d)", tz_abbr ? tz_abbr : "Local", off_hr, off_mn);

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
        int zdoy = g_date_time_get_day_of_year(zt);
        const char *day_rel = (zdoy == local_doy) ? "Today"
                            : (zdoy > local_doy || (local_doy > 360 && zdoy == 1)) ? "Tomorrow" : "Yesterday";
        GTimeSpan diff = (g_date_time_get_utc_offset(zt) - local_off) / G_TIME_SPAN_HOUR;
        char *sub = diff == 0
            ? g_strdup_printf("%s · Same time as local (%s)", day_rel, z->tz_id)
            : g_strdup_printf("%s · %+ld hr from local (%s)", day_rel, (long)diff, z->tz_id);

        if (z->time_lbl) gtk_label_set_text(GTK_LABEL(z->time_lbl), zt_str);
        if (z->sub_lbl) gtk_label_set_text(GTK_LABEL(z->sub_lbl), sub);
        g_free(zt_str);
        g_free(sub);
        g_date_time_unref(zt);
    }
    g_date_time_unref(local_now);
    if (analog_area) gtk_widget_queue_draw(analog_area);
}

static gboolean remove_zone_idle_cb(gpointer d)
{
    WorldZone *z = d;
    if (!z || !world_zones) return G_SOURCE_REMOVE;
    if (z->row) gtk_widget_destroy(z->row);
    g_ptr_array_remove(world_zones, z);
    save_state();
    return G_SOURCE_REMOVE;
}

static void on_remove_zone(GtkButton *b, gpointer d)
{
    (void)b;
    g_idle_add(remove_zone_idle_cb, d);
}

static void add_world_zone_row(const char *city, const char *tz_id, gboolean persist)
{
    if (!city || !tz_id) return;
    for (guint i = 0; i < world_zones->len; i++) {
        WorldZone *existing = g_ptr_array_index(world_zones, i);
        if (!g_strcmp0(existing->tz_id, tz_id) && !g_strcmp0(existing->city, city))
            return;
    }
    WorldZone *z = g_new0(WorldZone, 1);
    z->city = g_strdup(city);
    z->tz_id = g_strdup(tz_id);

    GtkWidget *row = gtk_list_box_row_new();
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
    GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_start(hb, 12);
    gtk_widget_set_margin_end(hb, 12);
    gtk_widget_set_margin_top(hb, 10);
    gtk_widget_set_margin_bottom(hb, 10);

    GtkWidget *vb = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    GtkWidget *cl = gtk_label_new(city);
    gtk_label_set_xalign(GTK_LABEL(cl), 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(cl), "clock-city-title");
    z->sub_lbl = gtk_label_new(tz_id);
    gtk_label_set_xalign(GTK_LABEL(z->sub_lbl), 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(z->sub_lbl), "clock-sub");
    gtk_box_pack_start(GTK_BOX(vb), cl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vb), z->sub_lbl, FALSE, FALSE, 0);

    z->time_lbl = gtk_label_new("--:--:--");
    gtk_style_context_add_class(gtk_widget_get_style_context(z->time_lbl), "clock-city-time");

    GtkWidget *del_btn = gtk_button_new_from_icon_name("list-remove-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_button_set_relief(GTK_BUTTON(del_btn), GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text(del_btn, "Remove time zone");
    g_signal_connect(del_btn, "clicked", G_CALLBACK(on_remove_zone), z);

    gtk_box_pack_start(GTK_BOX(hb), vb, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(hb), del_btn, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(hb), z->time_lbl, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(row), hb);
    gtk_widget_show_all(row);
    gtk_list_box_insert(GTK_LIST_BOX(zones_list), row, -1);

    z->row = row;
    g_ptr_array_add(world_zones, z);
    update_world_zones_ui();
    if (persist) save_state();
}

static void on_add_zone_clicked(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    int idx = gtk_combo_box_get_active(GTK_COMBO_BOX(zone_combo));
    if (idx < 0 || idx >= (int)G_N_ELEMENTS(preset_zones)) return;
    add_world_zone_row(preset_zones[idx].city, preset_zones[idx].tz, TRUE);
}

static GtkWidget *build_world_page(void)
{
    GtkWidget *paned = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 18);
    gtk_container_set_border_width(GTK_CONTAINER(paned), 18);

    /* Left: Analog & Local Clock Card */
    GtkWidget *left_card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_style_context_add_class(gtk_widget_get_style_context(left_card), "clock-card");
    gtk_widget_set_size_request(left_card, 270, -1);

    analog_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(analog_area, 210, 210);
    gtk_widget_set_vexpand(analog_area, TRUE);
    g_signal_connect(analog_area, "draw", G_CALLBACK(draw_analog_clock), NULL);
    gtk_box_pack_start(GTK_BOX(left_card), analog_area, TRUE, TRUE, 0);

    local_time_lbl = gtk_label_new("00:00:00");
    gtk_style_context_add_class(gtk_widget_get_style_context(local_time_lbl), "clock-big-time");
    local_date_lbl = gtk_label_new("");
    gtk_style_context_add_class(gtk_widget_get_style_context(local_date_lbl), "clock-city-title");
    local_tz_lbl = gtk_label_new("");
    gtk_style_context_add_class(gtk_widget_get_style_context(local_tz_lbl), "clock-sub");

    gtk_box_pack_start(GTK_BOX(left_card), local_time_lbl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(left_card), local_date_lbl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(left_card), local_tz_lbl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(paned), left_card, FALSE, FALSE, 0);

    /* Right: World Clocks list + Add bar */
    GtkWidget *right_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    GtkWidget *add_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    zone_combo = gtk_combo_box_text_new();
    for (guint i = 0; i < G_N_ELEMENTS(preset_zones); i++)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(zone_combo), preset_zones[i].city);
    gtk_combo_box_set_active(GTK_COMBO_BOX(zone_combo), 0);
    gtk_widget_set_hexpand(zone_combo, TRUE);

    GtkWidget *add_btn = gtk_button_new_with_label("+ Add World Clock");
    gtk_style_context_add_class(gtk_widget_get_style_context(add_btn), "suggested-action");
    g_signal_connect(add_btn, "clicked", G_CALLBACK(on_add_zone_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(add_row), zone_combo, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(add_row), add_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(right_box), add_row, FALSE, FALSE, 0);

    GtkWidget *sc = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sc), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sc), GTK_SHADOW_IN);
    zones_list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(zones_list), GTK_SELECTION_NONE);
    gtk_container_add(GTK_CONTAINER(sc), zones_list);
    gtk_box_pack_start(GTK_BOX(right_box), sc, TRUE, TRUE, 0);

    gtk_box_pack_start(GTK_BOX(paned), right_box, TRUE, TRUE, 0);
    return paned;
}

/* ---------------------------------------------------------------- Alarms */
static void alarm_free(gpointer p)
{
    AlarmItem *a = p;
    if (!a) return;
    g_free(a->label);
    g_free(a);
}

static void rebuild_alarms_list(void);

static void on_alarm_toggled(GObject *sw, GParamSpec *ps, gpointer d)
{
    (void)ps;
    AlarmItem *a = d;
    if (!a) return;
    a->enabled = gtk_switch_get_active(GTK_SWITCH(sw));
    save_state();
}

static gboolean delete_alarm_idle_cb(gpointer d)
{
    AlarmItem *a = d;
    if (!a || !alarms) return G_SOURCE_REMOVE;
    g_ptr_array_remove(alarms, a);
    rebuild_alarms_list();
    save_state();
    return G_SOURCE_REMOVE;
}

static void on_alarm_delete(GtkButton *b, gpointer d)
{
    (void)b;
    g_idle_add(delete_alarm_idle_cb, d);
}

static void rebuild_alarms_list(void)
{
    if (!alarms_list) return;
    GList *children = gtk_container_get_children(GTK_CONTAINER(alarms_list));
    for (GList *l = children; l; l = l->next)
        gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(children);

    static const char *const rep_names[] = { "Every day", "Weekdays (Mon–Fri)", "Weekends (Sat–Sun)", "Once" };
    for (guint i = 0; alarms && i < alarms->len; i++) {
        AlarmItem *a = g_ptr_array_index(alarms, i);
        GtkWidget *row = gtk_list_box_row_new();
        gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
        GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
        gtk_widget_set_margin_start(hb, 14);
        gtk_widget_set_margin_end(hb, 14);
        gtk_widget_set_margin_top(hb, 10);
        gtk_widget_set_margin_bottom(hb, 10);

        char tbuf[16];
        g_snprintf(tbuf, sizeof tbuf, "%02d:%02d", a->hour, a->minute);
        GtkWidget *tl = gtk_label_new(tbuf);
        gtk_style_context_add_class(gtk_widget_get_style_context(tl), "clock-alarm-time");

        GtkWidget *vb = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        GtkWidget *nl = gtk_label_new(a->label && *a->label ? a->label : "Alarm");
        gtk_label_set_xalign(GTK_LABEL(nl), 0);
        gtk_style_context_add_class(gtk_widget_get_style_context(nl), "clock-city-title");
        GtkWidget *rl = gtk_label_new(rep_names[CLAMP(a->repeat_mode, 0, 3)]);
        gtk_label_set_xalign(GTK_LABEL(rl), 0);
        gtk_style_context_add_class(gtk_widget_get_style_context(rl), "clock-sub");
        gtk_box_pack_start(GTK_BOX(vb), nl, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(vb), rl, FALSE, FALSE, 0);

        GtkWidget *sw = gtk_switch_new();
        gtk_widget_set_valign(sw, GTK_ALIGN_CENTER);
        gtk_switch_set_active(GTK_SWITCH(sw), a->enabled);
        g_signal_connect(sw, "notify::active", G_CALLBACK(on_alarm_toggled), a);

        GtkWidget *del = gtk_button_new_from_icon_name("user-trash-symbolic", GTK_ICON_SIZE_BUTTON);
        gtk_button_set_relief(GTK_BUTTON(del), GTK_RELIEF_NONE);
        gtk_widget_set_tooltip_text(del, "Delete alarm");
        g_signal_connect(del, "clicked", G_CALLBACK(on_alarm_delete), a);

        gtk_box_pack_start(GTK_BOX(hb), tl, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(hb), vb, TRUE, TRUE, 0);
        gtk_box_pack_end(GTK_BOX(hb), del, FALSE, FALSE, 0);
        gtk_box_pack_end(GTK_BOX(hb), sw, FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(row), hb);
        gtk_list_box_insert(GTK_LIST_BOX(alarms_list), row, -1);
    }
    gtk_widget_show_all(alarms_list);
}

static void on_add_alarm_clicked(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    AlarmItem *a = g_new0(AlarmItem, 1);
    a->hour = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(alarm_hour_spin));
    a->minute = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(alarm_min_spin));
    const char *txt = gtk_entry_get_text(GTK_ENTRY(alarm_label_entry));
    a->label = g_strdup(txt && *txt ? txt : "Alarm");
    a->repeat_mode = CLAMP(gtk_combo_box_get_active(GTK_COMBO_BOX(alarm_repeat_combo)), 0, 3);
    a->enabled = TRUE;
    a->last_fired_yday = -1;
    g_ptr_array_add(alarms, a);
    gtk_entry_set_text(GTK_ENTRY(alarm_label_entry), "");
    rebuild_alarms_list();
    save_state();
}

static void on_dismiss_alarm_banner(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    gtk_revealer_set_reveal_child(GTK_REVEALER(alarm_banner_revealer), FALSE);
}

static GtkWidget *build_alarms_page(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_container_set_border_width(GTK_CONTAINER(box), 18);

    alarm_banner_revealer = gtk_revealer_new();
    GtkWidget *banner = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_style_context_add_class(gtk_widget_get_style_context(banner), "clock-banner");
    alarm_banner_label = gtk_label_new("Alarm ringing!");
    gtk_label_set_xalign(GTK_LABEL(alarm_banner_label), 0);
    GtkWidget *dismiss = gtk_button_new_with_label("Dismiss");
    g_signal_connect(dismiss, "clicked", G_CALLBACK(on_dismiss_alarm_banner), NULL);
    gtk_box_pack_start(GTK_BOX(banner), alarm_banner_label, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(banner), dismiss, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(alarm_banner_revealer), banner);
    gtk_box_pack_start(GTK_BOX(box), alarm_banner_revealer, FALSE, FALSE, 0);

    /* Add Alarm Bar */
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_style_context_add_class(gtk_widget_get_style_context(card), "clock-card");

    alarm_hour_spin = gtk_spin_button_new_with_range(0, 23, 1);
    gtk_spin_button_set_wrap(GTK_SPIN_BUTTON(alarm_hour_spin), TRUE);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(alarm_hour_spin), 7);

    alarm_min_spin = gtk_spin_button_new_with_range(0, 59, 1);
    gtk_spin_button_set_wrap(GTK_SPIN_BUTTON(alarm_min_spin), TRUE);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(alarm_min_spin), 30);

    alarm_label_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(alarm_label_entry), "Alarm label (e.g. Morning workout)");
    gtk_widget_set_hexpand(alarm_label_entry, TRUE);

    alarm_repeat_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(alarm_repeat_combo), "Every day");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(alarm_repeat_combo), "Weekdays");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(alarm_repeat_combo), "Weekends");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(alarm_repeat_combo), "Once");
    gtk_combo_box_set_active(GTK_COMBO_BOX(alarm_repeat_combo), 0);

    GtkWidget *add_btn = gtk_button_new_with_label("+ Add Alarm");
    gtk_style_context_add_class(gtk_widget_get_style_context(add_btn), "suggested-action");
    g_signal_connect(add_btn, "clicked", G_CALLBACK(on_add_alarm_clicked), NULL);

    gtk_box_pack_start(GTK_BOX(card), gtk_label_new("Time:"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(card), alarm_hour_spin, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(card), gtk_label_new(":"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(card), alarm_min_spin, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(card), alarm_label_entry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(card), alarm_repeat_combo, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(card), add_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), card, FALSE, FALSE, 0);

    GtkWidget *sc = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sc), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sc), GTK_SHADOW_IN);
    alarms_list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(alarms_list), GTK_SELECTION_NONE);
    gtk_container_add(GTK_CONTAINER(sc), alarms_list);
    gtk_box_pack_start(GTK_BOX(box), sc, TRUE, TRUE, 0);

    return box;
}

/* ---------------------------------------------------------------- Stopwatch */
static void format_sw_us(gint64 us, char *buf, size_t n)
{
    if (us < 0) us = 0;
    gint64 cs = (us / 10000) % 100;
    gint64 total_sec = us / 1000000;
    gint64 s = total_sec % 60;
    gint64 m = (total_sec / 60) % 60;
    gint64 h = total_sec / 3600;
    if (h > 0)
        g_snprintf(buf, n, "%02lld:%02lld:%02lld.%02lld", (long long)h, (long long)m, (long long)s, (long long)cs);
    else
        g_snprintf(buf, n, "%02lld:%02lld.%02lld", (long long)m, (long long)s, (long long)cs);
}

static gint64 sw_elapsed_now(void)
{
    return sw_accum_us + (sw_running ? (g_get_monotonic_time() - sw_start_us) : 0);
}

static void on_sw_start_pause(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    if (!sw_running) {
        sw_start_us = g_get_monotonic_time();
        sw_running = TRUE;
        gtk_button_set_label(GTK_BUTTON(sw_start_btn), "Pause");
        gtk_widget_set_sensitive(sw_lap_btn, TRUE);
        gtk_widget_set_sensitive(sw_reset_btn, TRUE);
    } else {
        sw_accum_us += g_get_monotonic_time() - sw_start_us;
        sw_running = FALSE;
        gtk_button_set_label(GTK_BUTTON(sw_start_btn), "Resume");
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
    g_snprintf(num_s, sizeof num_s, "#%d", sw_lap_count);
    format_sw_us(split, split_s, sizeof split_s);
    format_sw_us(total, total_s, sizeof total_s);

    GtkTreeIter it;
    gtk_list_store_prepend(sw_lap_store, &it);
    gtk_list_store_set(sw_lap_store, &it, 0, num_s, 1, split_s, 2, total_s, -1);
}

static void on_sw_reset(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    sw_running = FALSE;
    sw_accum_us = 0;
    sw_last_lap_us = 0;
    sw_lap_count = 0;
    gtk_button_set_label(GTK_BUTTON(sw_start_btn), "Start");
    gtk_label_set_text(GTK_LABEL(sw_display_lbl), "00:00.00");
    gtk_widget_set_sensitive(sw_lap_btn, FALSE);
    gtk_widget_set_sensitive(sw_reset_btn, FALSE);
    gtk_list_store_clear(sw_lap_store);
}

static GtkWidget *build_stopwatch_page(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_container_set_border_width(GTK_CONTAINER(box), 18);

    GtkWidget *top_card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_style_context_add_class(gtk_widget_get_style_context(top_card), "clock-card");

    sw_display_lbl = gtk_label_new("00:00.00");
    gtk_style_context_add_class(gtk_widget_get_style_context(sw_display_lbl), "clock-sw-time");
    gtk_box_pack_start(GTK_BOX(top_card), sw_display_lbl, FALSE, FALSE, 6);

    GtkWidget *btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_halign(btns, GTK_ALIGN_CENTER);
    sw_start_btn = gtk_button_new_with_label("Start");
    gtk_widget_set_size_request(sw_start_btn, 110, 36);
    gtk_style_context_add_class(gtk_widget_get_style_context(sw_start_btn), "suggested-action");
    sw_lap_btn = gtk_button_new_with_label("Lap");
    gtk_widget_set_size_request(sw_lap_btn, 90, 36);
    gtk_widget_set_sensitive(sw_lap_btn, FALSE);
    sw_reset_btn = gtk_button_new_with_label("Reset");
    gtk_widget_set_size_request(sw_reset_btn, 90, 36);
    gtk_widget_set_sensitive(sw_reset_btn, FALSE);

    g_signal_connect(sw_start_btn, "clicked", G_CALLBACK(on_sw_start_pause), NULL);
    g_signal_connect(sw_lap_btn, "clicked", G_CALLBACK(on_sw_lap), NULL);
    g_signal_connect(sw_reset_btn, "clicked", G_CALLBACK(on_sw_reset), NULL);

    gtk_box_pack_start(GTK_BOX(btns), sw_start_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(btns), sw_lap_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(btns), sw_reset_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(top_card), btns, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), top_card, FALSE, FALSE, 0);

    sw_lap_store = gtk_list_store_new(3, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    GtkWidget *tv = gtk_tree_view_new_with_model(GTK_TREE_MODEL(sw_lap_store));
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(tv), TRUE);
    const char *cols[] = { "Lap", "Split Time", "Total Time" };
    for (int i = 0; i < 3; i++) {
        GtkCellRenderer *r = gtk_cell_renderer_text_new();
        GtkTreeViewColumn *c = gtk_tree_view_column_new_with_attributes(cols[i], r, "text", i, NULL);
        gtk_tree_view_column_set_expand(c, TRUE);
        gtk_tree_view_append_column(GTK_TREE_VIEW(tv), c);
    }
    GtkWidget *sc = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sc), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sc), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(sc), tv);
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
    double radius = MIN(width, height) / 2.0 - 14.0;
    if (radius < 24) return FALSE;

    HdeThemeInfo ti;
    hde_theme_info_load(&ti);
    GdkRGBA accent = { 0.21, 0.52, 0.89, 1.0 };
    if (ti.accent) gdk_rgba_parse(&accent, ti.accent);
    gboolean dark = hde_theme_shell_dark();
    hde_theme_info_clear(&ti);

    double fg = dark ? 0.92 : 0.15;

    /* Background track */
    cairo_set_line_width(cr, 10.0);
    cairo_set_source_rgba(cr, fg, fg, fg, 0.12);
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
    cairo_set_font_size(cr, 28.0);
    cairo_text_extents_t ext;
    cairo_text_extents(cr, buf, &ext);
    cairo_set_source_rgba(cr, fg, fg, fg, 0.95);
    cairo_move_to(cr, cx - ext.width / 2.0 - ext.x_bearing, cy - ext.height / 2.0 - ext.y_bearing);
    cairo_show_text(cr, buf);

    return FALSE;
}

static void on_tmr_spin_changed(GtkSpinButton *s, gpointer d)
{
    (void)s; (void)d;
    if (!tmr_running && tmr_total_ms == 0 && tmr_ring_area)
        gtk_widget_queue_draw(tmr_ring_area);
}

static void set_timer_preset_sec(int total_sec)
{
    tmr_running = FALSE;
    tmr_total_ms = (gint64)total_sec * 1000;
    tmr_remain_ms = tmr_total_ms;
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(tmr_hour_spin), total_sec / 3600);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(tmr_min_spin), (total_sec / 60) % 60);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(tmr_sec_spin), total_sec % 60);
    gtk_button_set_label(GTK_BUTTON(tmr_start_btn), "Start");
    if (tmr_ring_area) gtk_widget_queue_draw(tmr_ring_area);
}

static void on_tmr_preset_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    int sec = GPOINTER_TO_INT(d);
    set_timer_preset_sec(sec);
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
        }
        if (tmr_remain_ms <= 0) return;
        tmr_last_tick_us = g_get_monotonic_time();
        tmr_running = TRUE;
        gtk_button_set_label(GTK_BUTTON(tmr_start_btn), "Pause");
    } else {
        tmr_running = FALSE;
        gtk_button_set_label(GTK_BUTTON(tmr_start_btn), "Resume");
    }
    if (tmr_ring_area) gtk_widget_queue_draw(tmr_ring_area);
}

static void on_tmr_plus_one(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    tmr_total_ms += 60000;
    tmr_remain_ms += 60000;
    if (tmr_ring_area) gtk_widget_queue_draw(tmr_ring_area);
}

static void on_tmr_reset(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    tmr_running = FALSE;
    tmr_total_ms = 0;
    tmr_remain_ms = 0;
    gtk_button_set_label(GTK_BUTTON(tmr_start_btn), "Start");
    if (tmr_ring_area) gtk_widget_queue_draw(tmr_ring_area);
}

static GtkWidget *build_timer_page(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 18);
    gtk_container_set_border_width(GTK_CONTAINER(box), 18);

    /* Left: Circular countdown ring */
    GtkWidget *left_card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_style_context_add_class(gtk_widget_get_style_context(left_card), "clock-card");
    gtk_widget_set_size_request(left_card, 270, -1);

    tmr_ring_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(tmr_ring_area, 220, 220);
    gtk_widget_set_vexpand(tmr_ring_area, TRUE);
    g_signal_connect(tmr_ring_area, "draw", G_CALLBACK(draw_timer_ring), NULL);
    gtk_box_pack_start(GTK_BOX(left_card), tmr_ring_area, TRUE, TRUE, 0);

    GtkWidget *act_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(act_row, GTK_ALIGN_CENTER);
    tmr_start_btn = gtk_button_new_with_label("Start");
    gtk_style_context_add_class(gtk_widget_get_style_context(tmr_start_btn), "suggested-action");
    GtkWidget *plus_btn = gtk_button_new_with_label("+1 min");
    tmr_reset_btn = gtk_button_new_with_label("Reset");
    g_signal_connect(tmr_start_btn, "clicked", G_CALLBACK(on_tmr_start_pause), NULL);
    g_signal_connect(plus_btn, "clicked", G_CALLBACK(on_tmr_plus_one), NULL);
    g_signal_connect(tmr_reset_btn, "clicked", G_CALLBACK(on_tmr_reset), NULL);
    gtk_box_pack_start(GTK_BOX(act_row), tmr_start_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(act_row), plus_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(act_row), tmr_reset_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(left_card), act_row, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), left_card, FALSE, FALSE, 0);

    /* Right: Duration spinners & quick presets */
    GtkWidget *right_card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_style_context_add_class(gtk_widget_get_style_context(right_card), "clock-card");

    GtkWidget *spin_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    tmr_hour_spin = gtk_spin_button_new_with_range(0, 99, 1);
    tmr_min_spin = gtk_spin_button_new_with_range(0, 59, 1);
    tmr_sec_spin = gtk_spin_button_new_with_range(0, 59, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(tmr_min_spin), 5);
    g_signal_connect(tmr_hour_spin, "value-changed", G_CALLBACK(on_tmr_spin_changed), NULL);
    g_signal_connect(tmr_min_spin, "value-changed", G_CALLBACK(on_tmr_spin_changed), NULL);
    g_signal_connect(tmr_sec_spin, "value-changed", G_CALLBACK(on_tmr_spin_changed), NULL);

    gtk_box_pack_start(GTK_BOX(spin_row), gtk_label_new("Hours:"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(spin_row), tmr_hour_spin, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(spin_row), gtk_label_new("Min:"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(spin_row), tmr_min_spin, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(spin_row), gtk_label_new("Sec:"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(spin_row), tmr_sec_spin, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(right_card), spin_row, FALSE, FALSE, 0);

    tmr_name_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(tmr_name_entry), "Timer label (e.g. Tea, Pomodoro, Pasta)");
    gtk_box_pack_start(GTK_BOX(right_card), tmr_name_entry, FALSE, FALSE, 0);

    GtkWidget *pl = gtk_label_new("Quick Presets");
    gtk_label_set_xalign(GTK_LABEL(pl), 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(pl), "clock-city-title");
    gtk_box_pack_start(GTK_BOX(right_card), pl, FALSE, FALSE, 0);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_homogeneous(GTK_GRID(grid), TRUE);
    static const struct { const char *lbl; int sec; } presets[] = {
        { "1 min", 60 }, { "3 min", 180 }, { "5 min", 300 }, { "10 min", 600 },
        { "15 min", 900 }, { "25 min (Pomodoro)", 1500 }, { "30 min", 1800 }, { "1 hour", 3600 }
    };
    for (guint i = 0; i < G_N_ELEMENTS(presets); i++) {
        GtkWidget *b = gtk_button_new_with_label(presets[i].lbl);
        g_signal_connect(b, "clicked", G_CALLBACK(on_tmr_preset_clicked), GINT_TO_POINTER(presets[i].sec));
        gtk_grid_attach(GTK_GRID(grid), b, (int)(i % 4), (int)(i / 4), 1, 1);
    }
    gtk_box_pack_start(GTK_BOX(right_card), grid, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), right_card, TRUE, TRUE, 0);
    return box;
}

/* ---------------------------------------------------------------- Master Tick */
static void check_alarms_tick(void)
{
    if (!alarms || alarms->len == 0) return;
    time_t now_t = time(NULL);
    struct tm lt;
    if (!localtime_r(&now_t, &lt)) return;

    for (guint i = 0; i < alarms->len; i++) {
        AlarmItem *a = g_ptr_array_index(alarms, i);
        if (!a->enabled) continue;
        if (a->last_fired_yday == lt.tm_yday && lt.tm_sec > 2) continue;
        if (a->hour != lt.tm_hour || a->minute != lt.tm_min || lt.tm_sec > 1) continue;

        /* Check weekday/weekend constraint */
        gboolean weekend = (lt.tm_wday == 0 || lt.tm_wday == 6);
        if (a->repeat_mode == 1 && weekend) continue;
        if (a->repeat_mode == 2 && !weekend) continue;

        a->last_fired_yday = lt.tm_yday;
        char msg[256];
        g_snprintf(msg, sizeof msg, "%s (%02d:%02d)", a->label ? a->label : "Alarm", a->hour, a->minute);
        notify_event("Alarm", msg);
        if (alarm_banner_label && alarm_banner_revealer) {
            gtk_label_set_text(GTK_LABEL(alarm_banner_label), msg);
            gtk_revealer_set_reveal_child(GTK_REVEALER(alarm_banner_revealer), TRUE);
        }
        if (a->repeat_mode == 3) {
            a->enabled = FALSE;
            rebuild_alarms_list();
            save_state();
        }
    }
}

static gboolean on_fast_tick(gpointer d)
{
    (void)d;
    if (sw_running && sw_display_lbl) {
        char buf[32];
        format_sw_us(sw_elapsed_now(), buf, sizeof buf);
        gtk_label_set_text(GTK_LABEL(sw_display_lbl), buf);
    }
    if (tmr_running) {
        gint64 now_us = g_get_monotonic_time();
        gint64 dt_ms = (now_us - tmr_last_tick_us) / 1000;
        if (dt_ms > 0) {
            tmr_last_tick_us = now_us;
            tmr_remain_ms -= dt_ms;
            if (tmr_remain_ms <= 0) {
                tmr_remain_ms = 0;
                tmr_running = FALSE;
                gtk_button_set_label(GTK_BUTTON(tmr_start_btn), "Start");
                const char *lbl = gtk_entry_get_text(GTK_ENTRY(tmr_name_entry));
                notify_event("Timer Finished", (lbl && *lbl) ? lbl : "Your countdown timer has finished.");
            }
            if (tmr_ring_area) gtk_widget_queue_draw(tmr_ring_area);
        }
    }
    return G_SOURCE_CONTINUE;
}

static gboolean on_second_tick(gpointer d)
{
    (void)d;
    update_world_zones_ui();
    check_alarms_tick();
    return G_SOURCE_CONTINUE;
}

static void load_saved_state(void)
{
    GKeyFile *kf = g_key_file_new();
    char *path = clock_ini_path();
    gboolean ok = g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL);
    g_free(path);

    gsize nc = 0, nt = 0;
    char **cities = ok ? g_key_file_get_string_list(kf, "world", "cities", &nc, NULL) : NULL;
    char **tzs = ok ? g_key_file_get_string_list(kf, "world", "tzs", &nt, NULL) : NULL;
    if (cities && tzs && nc > 0 && nc == nt) {
        for (gsize i = 0; i < nc; i++)
            add_world_zone_row(cities[i], tzs[i], FALSE);
    } else {
        add_world_zone_row("UTC (Coordinated Universal Time)", "UTC", FALSE);
        add_world_zone_row("New York (United States)", "America/New_York", FALSE);
        add_world_zone_row("London (United Kingdom)", "Europe/London", FALSE);
        add_world_zone_row("Ho Chi Minh City (Vietnam)", "Asia/Ho_Chi_Minh", FALSE);
        add_world_zone_row("Tokyo (Japan)", "Asia/Tokyo", FALSE);
    }
    g_strfreev(cities);
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
        a->repeat_mode = CLAMP(g_key_file_get_integer(kf, grp, "repeat", NULL), 0, 3);
        a->enabled = g_key_file_get_boolean(kf, grp, "enabled", NULL);
        a->last_fired_yday = -1;
        g_ptr_array_add(alarms, a);
    }
    rebuild_alarms_list();
    g_key_file_free(kf);
}

static void show_about_dialog(void)
{
    const char *authors[] = { "Hyggshi OS Research Technology", NULL };
    gtk_show_about_dialog(GTK_WINDOW(main_win),
        "program-name", "Hyggshi Clock",
        "version", HDE_VERSION,
        "comments", "World Clock, Alarms, Stopwatch, and Countdown Timer for Hyggshi Desktop Environment.",
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
    return FALSE;
}

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
    gtk_window_set_default_size(GTK_WINDOW(main_win), 760, 480);
    g_signal_connect(main_win, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    g_signal_connect(main_win, "key-press-event", G_CALLBACK(on_clock_key), NULL);

    stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(stack), GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT_RIGHT);

    GtkWidget *hbar = gtk_header_bar_new();
    gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(hbar), TRUE);
    gtk_header_bar_set_title(GTK_HEADER_BAR(hbar), APP_TITLE);
    GtkWidget *switcher = gtk_stack_switcher_new();
    gtk_stack_switcher_set_stack(GTK_STACK_SWITCHER(switcher), GTK_STACK(stack));
    gtk_header_bar_set_custom_title(GTK_HEADER_BAR(hbar), switcher);

    GtkWidget *about_btn = gtk_button_new_from_icon_name("help-about-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(about_btn, "About Hyggshi Clock (F1)");
    g_signal_connect(about_btn, "clicked", G_CALLBACK(on_about_clicked), NULL);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(hbar), about_btn);

    gtk_window_set_titlebar(GTK_WINDOW(main_win), hbar);

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
    g_timeout_add(40, on_fast_tick, NULL);

    gtk_main();
    return 0;
}
