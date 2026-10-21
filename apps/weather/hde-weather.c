/* hde-weather.c — Hyggshi Weather: GTK3 desktop weather forecast & atmospheric monitor for HDE. */
#define _POSIX_C_SOURCE 200809L

#include "hde-build.h"
#include "hde-theme.h"
#include "hde-weather-core.h"
#include <cairo.h>
#include <ctype.h>
#include <gio/gio.h>
#include <gtk/gtk.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define APP_TITLE "Hyggshi Weather"
#define MAX_SAVED_CITIES 24

static HdeWeatherCity saved_cities[MAX_SAVED_CITIES];
static int saved_city_count = 0;
static int active_city_idx = 0;

static HdeWeatherForecast forecasts[MAX_SAVED_CITIES];
static HdeWeatherForecast active_fc;

/* Preferences: temp_fahrenheit (0=C, 1=F), wind_unit (0=km/h, 1=mph, 2=m/s), press_inhg (0=hPa, 1=inHg) */
static gboolean use_fahrenheit = FALSE;
static int wind_unit = 0;
static gboolean use_inhg = FALSE;
static gboolean anim_enabled = TRUE;
static double anim_phase = 0.0;
static guint anim_timer_id = 0;
static gboolean fetch_in_progress = FALSE;

static GtkCssProvider *app_css = NULL;
static GtkWidget *main_win = NULL;
static GtkWidget *city_list_box = NULL;
static GtkWidget *city_filter_entry = NULL;
static GtkWidget *status_spinner = NULL;
static GtkWidget *unit_temp_btn = NULL;
static GtkWidget *unit_wind_combo = NULL;
static GtkWidget *hero_area = NULL;
static GtkWidget *chart_area = NULL;
static GtkWidget *hourly_box = NULL;
static GtkWidget *daily_box = NULL;
static GtkWidget *row_temp_lbl[MAX_SAVED_CITIES];
static GtkWidget *row_sub_lbl[MAX_SAVED_CITIES];
static GtkWidget *row_icon_img[MAX_SAVED_CITIES];
static gboolean rebuilding_sidebar = FALSE;

/* Detail card labels & gauges */
static GtkWidget *lbl_uv_val, *lbl_uv_sub, *bar_uv;
static GtkWidget *lbl_wind_val, *lbl_wind_sub, *compass_area;
static GtkWidget *lbl_hum_val, *lbl_hum_sub, *bar_hum;
static GtkWidget *lbl_press_val, *lbl_press_sub, *bar_press;
static GtkWidget *lbl_vis_val, *lbl_vis_sub;
static GtkWidget *lbl_sun_val, *lbl_sun_sub;
static GtkWidget *lbl_precip_val, *lbl_precip_sub;
static GtkWidget *lbl_aqi_val, *lbl_aqi_sub, *bar_aqi;

static void refresh_all_views(void);
static void start_async_fetch_for_city(int idx);

/* --- Formatting helpers --- */

static void format_temp(double temp_c, char *buf, size_t bufsz, gboolean with_unit)
{
    double v = use_fahrenheit ? hde_weather_c_to_f(temp_c) : temp_c;
    int iv = (int)floor(v + 0.5);
    if (with_unit)
        snprintf(buf, bufsz, "%d°%s", iv, use_fahrenheit ? "F" : "C");
    else
        snprintf(buf, bufsz, "%d°", iv);
}

static void format_wind(double kmh, char *buf, size_t bufsz)
{
    if (wind_unit == 1)
        snprintf(buf, bufsz, "%.1f mph", hde_weather_kmh_to_mph(kmh));
    else if (wind_unit == 2)
        snprintf(buf, bufsz, "%.1f m/s", hde_weather_kmh_to_ms(kmh));
    else
        snprintf(buf, bufsz, "%.0f km/h", kmh);
}

static void format_pressure(double hpa, char *buf, size_t bufsz)
{
    if (use_inhg)
        snprintf(buf, bufsz, "%.2f inHg", hde_weather_hpa_to_inhg(hpa));
    else
        snprintf(buf, bufsz, "%.0f hPa", hpa);
}

/* --- Config load / save --- */

static char *config_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "hde", "weather.ini", NULL);
}

static void save_config(void)
{
    char *dir = g_build_filename(g_get_user_config_dir(), "hde", NULL);
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);

    GKeyFile *kf = g_key_file_new();
    g_key_file_set_boolean(kf, "weather", "fahrenheit", use_fahrenheit);
    g_key_file_set_integer(kf, "weather", "wind_unit", wind_unit);
    g_key_file_set_boolean(kf, "weather", "inhg", use_inhg);
    g_key_file_set_integer(kf, "weather", "active_index", active_city_idx);
    g_key_file_set_integer(kf, "weather", "city_count", saved_city_count);

    for (int i = 0; i < saved_city_count; i++) {
        char grp[32];
        snprintf(grp, sizeof(grp), "city_%d", i);
        g_key_file_set_string(kf, grp, "name", saved_cities[i].name);
        g_key_file_set_string(kf, grp, "country", saved_cities[i].country);
        g_key_file_set_double(kf, grp, "lat", saved_cities[i].lat);
        g_key_file_set_double(kf, grp, "lon", saved_cities[i].lon);
        g_key_file_set_double(kf, grp, "tz", saved_cities[i].utc_offset_hours);
    }

    char *path = config_path();
    g_key_file_save_to_file(kf, path, NULL);
    g_free(path);
    g_key_file_free(kf);
}

static void load_config(const char *initial_city_arg)
{
    int builtin_total = 0;
    const HdeWeatherCity *builtins = hde_weather_builtin_cities(&builtin_total);

    char *path = config_path();
    GKeyFile *kf = g_key_file_new();
    if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
        use_fahrenheit = g_key_file_get_boolean(kf, "weather", "fahrenheit", NULL);
        wind_unit = CLAMP(g_key_file_get_integer(kf, "weather", "wind_unit", NULL), 0, 2);
        use_inhg = g_key_file_get_boolean(kf, "weather", "inhg", NULL);
        active_city_idx = g_key_file_get_integer(kf, "weather", "active_index", NULL);
        int cnt = g_key_file_get_integer(kf, "weather", "city_count", NULL);
        for (int i = 0; i < cnt && saved_city_count < MAX_SAVED_CITIES; i++) {
            char grp[32];
            snprintf(grp, sizeof(grp), "city_%d", i);
            char *nm = g_key_file_get_string(kf, grp, "name", NULL);
            char *ct = g_key_file_get_string(kf, grp, "country", NULL);
            if (nm && *nm) {
                HdeWeatherCity c;
                memset(&c, 0, sizeof(c));
                snprintf(c.name, sizeof(c.name), "%s", nm);
                snprintf(c.country, sizeof(c.country), "%s", ct ? ct : "");
                c.lat = g_key_file_get_double(kf, grp, "lat", NULL);
                c.lon = g_key_file_get_double(kf, grp, "lon", NULL);
                c.utc_offset_hours = g_key_file_get_double(kf, grp, "tz", NULL);
                saved_cities[saved_city_count++] = c;
            }
            g_free(nm);
            g_free(ct);
        }
    }
    g_key_file_free(kf);
    g_free(path);

    if (saved_city_count == 0) {
        /* Default starter locations */
        static const int starter_indices[] = { 0, 1, 2, 4, 17, 25 };
        for (size_t i = 0; i < G_N_ELEMENTS(starter_indices); i++) {
            int idx = starter_indices[i];
            if (idx < builtin_total && saved_city_count < MAX_SAVED_CITIES)
                saved_cities[saved_city_count++] = builtins[idx];
        }
    }

    if (initial_city_arg && *initial_city_arg) {
        HdeWeatherCity found[4];
        if (hde_weather_search_builtin_cities(initial_city_arg, found, 4) > 0) {
            int existing = -1;
            for (int i = 0; i < saved_city_count; i++) {
                if (g_ascii_strcasecmp(saved_cities[i].name, found[0].name) == 0) {
                    existing = i;
                    break;
                }
            }
            if (existing >= 0) {
                active_city_idx = existing;
            } else if (saved_city_count < MAX_SAVED_CITIES) {
                saved_cities[saved_city_count] = found[0];
                active_city_idx = saved_city_count++;
            }
        }
    }

    active_city_idx = CLAMP(active_city_idx, 0, MAX(0, saved_city_count - 1));
    time_t now = time(NULL);
    for (int i = 0; i < saved_city_count; i++)
        hde_weather_generate_fallback(&saved_cities[i], now, &forecasts[i]);
    active_fc = forecasts[active_city_idx];
}

/* --- Asynchronous live Open-Meteo fetch --- */

typedef struct {
    int city_idx;
    HdeWeatherCity city;
} FetchTaskData;

static void refresh_forecast_views(void);

static void on_fetch_finished(GObject *source, GAsyncResult *res, gpointer user_data)
{
    GSubprocess *proc = G_SUBPROCESS(source);
    FetchTaskData *td = user_data;
    char *stdout_buf = NULL;

    if (g_subprocess_communicate_utf8_finish(proc, res, &stdout_buf, NULL, NULL)) {
        if (stdout_buf && *stdout_buf && td->city_idx >= 0 && td->city_idx < saved_city_count) {
            HdeWeatherForecast parsed;
            if (hde_weather_parse_open_meteo_json(stdout_buf, &td->city, &parsed) == 0) {
                forecasts[td->city_idx] = parsed;
                if (td->city_idx == active_city_idx)
                    active_fc = parsed;
                refresh_forecast_views();
            }
        }
    }
    g_free(stdout_buf);
    g_free(td);
    fetch_in_progress = FALSE;
    if (status_spinner)
        gtk_spinner_stop(GTK_SPINNER(status_spinner));
}

static void start_async_fetch_for_city(int idx)
{
    if (idx < 0 || idx >= saved_city_count || fetch_in_progress)
        return;

    char *curl_bin = g_find_program_in_path("curl");
    char *wget_bin = curl_bin ? NULL : g_find_program_in_path("wget");
    if (!curl_bin && !wget_bin)
        return;

    char url[512];
    snprintf(url, sizeof(url),
        "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
        "&current=temperature_2m,relative_humidity_2m,apparent_temperature,is_day,precipitation,"
        "weather_code,cloud_cover,surface_pressure,wind_speed_10m,wind_direction_10m,wind_gusts_10m"
        "&hourly=temperature_2m,precipitation_probability,weather_code,wind_speed_10m,is_day"
        "&daily=weather_code,temperature_2m_max,temperature_2m_min,sunrise,sunset,uv_index_max,"
        "precipitation_sum,precipitation_probability_max&timezone=auto&forecast_days=7",
        saved_cities[idx].lat, saved_cities[idx].lon);

    GError *err = NULL;
    GSubprocess *proc = NULL;
    if (curl_bin) {
        proc = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
                                &err, curl_bin, "-fsSL", "-m", "5", url, NULL);
    } else {
        proc = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
                                &err, wget_bin, "-qO-", "-T", "5", url, NULL);
    }
    g_free(curl_bin);
    g_free(wget_bin);

    if (!proc) {
        if (err) g_error_free(err);
        return;
    }

    fetch_in_progress = TRUE;
    if (status_spinner)
        gtk_spinner_start(GTK_SPINNER(status_spinner));

    FetchTaskData *td = g_new0(FetchTaskData, 1);
    td->city_idx = idx;
    td->city = saved_cities[idx];
    g_subprocess_communicate_utf8_async(proc, NULL, NULL, on_fetch_finished, td);
    g_object_unref(proc);
}

/* --- Custom Cairo Drawings --- */

static void draw_cloud(cairo_t *cr, double cx, double cy, double scale, double alpha)
{
    cairo_save(cr);
    cairo_translate(cr, cx, cy);
    cairo_scale(cr, scale, scale);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, alpha);
    cairo_arc(cr, -18, 4, 14, 0, 2 * G_PI);
    cairo_arc(cr, -4, -4, 18, 0, 2 * G_PI);
    cairo_arc(cr, 14, 2, 15, 0, 2 * G_PI);
    cairo_arc(cr, 26, 6, 11, 0, 2 * G_PI);
    cairo_fill(cr);
    cairo_restore(cr);
}

static gboolean on_hero_draw(GtkWidget *w, cairo_t *cr, gpointer user_data)
{
    (void)user_data;
    int width = gtk_widget_get_allocated_width(w);
    int height = gtk_widget_get_allocated_height(w);
    if (width <= 10 || height <= 10) return FALSE;

    HdeWeatherKind kind = hde_weather_wmo_kind(active_fc.wmo_code);
    int is_day = active_fc.is_day;

    /* Rounded clip */
    double r = 14.0;
    cairo_new_sub_path(cr);
    cairo_arc(cr, width - r, r, r, -G_PI / 2, 0);
    cairo_arc(cr, width - r, height - r, r, 0, G_PI / 2);
    cairo_arc(cr, r, height - r, r, G_PI / 2, G_PI);
    cairo_arc(cr, r, r, r, G_PI, 3 * G_PI / 2);
    cairo_close_path(cr);
    cairo_clip(cr);

    /* Sky gradient based on weather kind and day/night */
    cairo_pattern_t *sky = cairo_pattern_create_linear(0, 0, width * 0.4, height);
    if (!is_day) {
        cairo_pattern_add_color_stop_rgb(sky, 0.0, 0.08, 0.11, 0.24);
        cairo_pattern_add_color_stop_rgb(sky, 0.6, 0.14, 0.19, 0.36);
        cairo_pattern_add_color_stop_rgb(sky, 1.0, 0.20, 0.26, 0.44);
    } else if (kind == HDE_WEATHER_KIND_CLEAR) {
        cairo_pattern_add_color_stop_rgb(sky, 0.0, 0.12, 0.48, 0.88);
        cairo_pattern_add_color_stop_rgb(sky, 0.6, 0.24, 0.62, 0.95);
        cairo_pattern_add_color_stop_rgb(sky, 1.0, 0.42, 0.75, 0.98);
    } else if (kind == HDE_WEATHER_KIND_PARTLY_CLOUDY) {
        cairo_pattern_add_color_stop_rgb(sky, 0.0, 0.16, 0.44, 0.80);
        cairo_pattern_add_color_stop_rgb(sky, 1.0, 0.36, 0.64, 0.90);
    } else if (kind == HDE_WEATHER_KIND_RAIN) {
        cairo_pattern_add_color_stop_rgb(sky, 0.0, 0.16, 0.26, 0.38);
        cairo_pattern_add_color_stop_rgb(sky, 1.0, 0.26, 0.40, 0.54);
    } else if (kind == HDE_WEATHER_KIND_STORM) {
        cairo_pattern_add_color_stop_rgb(sky, 0.0, 0.14, 0.14, 0.24);
        cairo_pattern_add_color_stop_rgb(sky, 1.0, 0.26, 0.24, 0.38);
    } else if (kind == HDE_WEATHER_KIND_SNOW) {
        cairo_pattern_add_color_stop_rgb(sky, 0.0, 0.28, 0.44, 0.64);
        cairo_pattern_add_color_stop_rgb(sky, 1.0, 0.48, 0.64, 0.82);
    } else {
        cairo_pattern_add_color_stop_rgb(sky, 0.0, 0.28, 0.36, 0.46);
        cairo_pattern_add_color_stop_rgb(sky, 1.0, 0.42, 0.50, 0.60);
    }
    cairo_set_source(cr, sky);
    cairo_paint(cr);
    cairo_pattern_destroy(sky);

    /* Animated celestial & atmospheric elements on right side */
    double sun_x = width - 115.0;
    double sun_y = 68.0;

    if (!is_day) {
        /* Twinkling stars */
        for (int i = 0; i < 18; i++) {
            double sx = width * 0.45 + ((i * 73) % (int)(width * 0.52));
            double sy = 18.0 + ((i * 41) % (height - 40));
            double tw = 0.35 + 0.45 * fabs(sin(anim_phase * 1.5 + i));
            cairo_set_source_rgba(cr, 1.0, 1.0, 0.92, tw);
            cairo_arc(cr, sx, sy, (i % 3 == 0) ? 2.1 : 1.3, 0, 2 * G_PI);
            cairo_fill(cr);
        }
        /* Glowing crescent moon */
        cairo_set_source_rgba(cr, 0.98, 0.95, 0.72, 0.20);
        cairo_arc(cr, sun_x, sun_y, 44, 0, 2 * G_PI);
        cairo_fill(cr);
        cairo_set_source_rgb(cr, 0.99, 0.96, 0.76);
        cairo_arc(cr, sun_x, sun_y, 26, 0, 2 * G_PI);
        cairo_fill(cr);
    } else if (kind == HDE_WEATHER_KIND_CLEAR || kind == HDE_WEATHER_KIND_PARTLY_CLOUDY) {
        /* Sun glow + rotating rays */
        double pulse = 1.0 + 0.06 * sin(anim_phase * 2.0);
        cairo_pattern_t *glow = cairo_pattern_create_radial(sun_x, sun_y, 12, sun_x, sun_y, 68 * pulse);
        cairo_pattern_add_color_stop_rgba(glow, 0.0, 1.0, 0.95, 0.55, 0.85);
        cairo_pattern_add_color_stop_rgba(glow, 1.0, 1.0, 0.90, 0.35, 0.0);
        cairo_set_source(cr, glow);
        cairo_arc(cr, sun_x, sun_y, 68 * pulse, 0, 2 * G_PI);
        cairo_fill(cr);
        cairo_pattern_destroy(glow);

        cairo_save(cr);
        cairo_translate(cr, sun_x, sun_y);
        cairo_rotate(cr, anim_phase * 0.25);
        cairo_set_source_rgba(cr, 1.0, 0.95, 0.55, 0.55);
        cairo_set_line_width(cr, 3.0);
        for (int i = 0; i < 12; i++) {
            double ang = i * (G_PI / 6.0);
            cairo_move_to(cr, cos(ang) * 32.0, sin(ang) * 32.0);
            cairo_line_to(cr, cos(ang) * 44.0, sin(ang) * 44.0);
        }
        cairo_stroke(cr);
        cairo_restore(cr);

        cairo_set_source_rgb(cr, 1.0, 0.93, 0.42);
        cairo_arc(cr, sun_x, sun_y, 25, 0, 2 * G_PI);
        cairo_fill(cr);
    }

    /* Clouds */
    if (kind != HDE_WEATHER_KIND_CLEAR) {
        double drift = sin(anim_phase * 0.6) * 10.0;
        draw_cloud(cr, width - 145.0 + drift, 86.0, 1.35, 0.26);
        draw_cloud(cr, width - 90.0 - drift * 0.7, 74.0, 1.15, 0.42);
    } else {
        draw_cloud(cr, width - 170.0 + sin(anim_phase * 0.4) * 8.0, 115.0, 0.9, 0.14);
    }

    /* Rain drops or Snowflakes */
    if (kind == HDE_WEATHER_KIND_RAIN || kind == HDE_WEATHER_KIND_STORM) {
        cairo_set_source_rgba(cr, 0.75, 0.88, 1.0, 0.55);
        cairo_set_line_width(cr, 1.8);
        for (int i = 0; i < 22; i++) {
            double rx = width * 0.48 + ((i * 53) % (int)(width * 0.48));
            double ry = fmod((i * 29.0) + anim_phase * 95.0, (double)height);
            cairo_move_to(cr, rx, ry);
            cairo_line_to(cr, rx - 4.0, ry + 11.0);
        }
        cairo_stroke(cr);
    } else if (kind == HDE_WEATHER_KIND_SNOW) {
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.75);
        for (int i = 0; i < 20; i++) {
            double sx = width * 0.48 + ((i * 47) % (int)(width * 0.48)) + sin(anim_phase + i) * 5.0;
            double sy = fmod((i * 31.0) + anim_phase * 35.0, (double)height);
            cairo_arc(cr, sx, sy, 2.5, 0, 2 * G_PI);
            cairo_fill(cr);
        }
    }

    /* Typography on the left & bottom of hero card */
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);

    /* City & Country */
    char title[140];
    if (active_fc.city.country[0])
        snprintf(title, sizeof(title), "%s, %s", active_fc.city.name, active_fc.city.country);
    else
        snprintf(title, sizeof(title), "%s", active_fc.city.name);

    cairo_set_font_size(cr, 22.0);
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_move_to(cr, 24.0, 38.0);
    cairo_show_text(cr, title);

    /* Coordinates & status badge */
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 12.0);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.82);
    char meta[160];
    snprintf(meta, sizeof(meta), "%.2f°%s, %.2f°%s  •  %s (%s)",
             fabs(active_fc.city.lat), active_fc.city.lat >= 0 ? "N" : "S",
             fabs(active_fc.city.lon), active_fc.city.lon >= 0 ? "E" : "W",
             active_fc.is_live ? "Live Open-Meteo Data" : "Local Forecast Model",
             active_fc.updated_at);
    cairo_move_to(cr, 24.0, 58.0);
    cairo_show_text(cr, meta);

    /* Big current temperature */
    char tbuf[32];
    format_temp(active_fc.current_temp_c, tbuf, sizeof(tbuf), TRUE);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 56.0);
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_move_to(cr, 22.0, 126.0);
    cairo_show_text(cr, tbuf);

    /* Condition + High / Low + Feels Like */
    const char *cond = hde_weather_wmo_description(active_fc.wmo_code);
    cairo_set_font_size(cr, 16.0);
    cairo_move_to(cr, 24.0, 156.0);
    cairo_show_text(cr, cond);

    char fl_buf[32], hi_buf[32], lo_buf[32], sub_line[160];
    format_temp(active_fc.feels_like_c, fl_buf, sizeof(fl_buf), TRUE);
    double dmax = active_fc.daily_count > 0 ? active_fc.daily[0].temp_max_c : active_fc.current_temp_c + 3.0;
    double dmin = active_fc.daily_count > 0 ? active_fc.daily[0].temp_min_c : active_fc.current_temp_c - 4.0;
    format_temp(dmax, hi_buf, sizeof(hi_buf), FALSE);
    format_temp(dmin, lo_buf, sizeof(lo_buf), FALSE);
    snprintf(sub_line, sizeof(sub_line), "High %s  •  Low %s  •  Feels like %s  •  Humidity %d%%",
             hi_buf, lo_buf, fl_buf, active_fc.humidity_pct);

    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 13.0);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.90);
    cairo_move_to(cr, 24.0, 180.0);
    cairo_show_text(cr, sub_line);

    return FALSE;
}

static gboolean on_chart_draw(GtkWidget *w, cairo_t *cr, gpointer user_data)
{
    (void)user_data;
    int width = gtk_widget_get_allocated_width(w);
    int height = gtk_widget_get_allocated_height(w);
    int n = active_fc.hourly_count;
    if (width <= 40 || height <= 40 || n < 2)
        return FALSE;

    double pad_l = 28.0, pad_r = 28.0, pad_t = 28.0, pad_b = 28.0;
    double cw = width - pad_l - pad_r;
    double ch = height - pad_t - pad_b;

    double tmin = active_fc.hourly[0].temp_c;
    double tmax = active_fc.hourly[0].temp_c;
    for (int i = 1; i < n; i++) {
        if (active_fc.hourly[i].temp_c < tmin) tmin = active_fc.hourly[i].temp_c;
        if (active_fc.hourly[i].temp_c > tmax) tmax = active_fc.hourly[i].temp_c;
    }
    if (tmax - tmin < 2.0) {
        tmax += 1.0;
        tmin -= 1.0;
    }

    /* Subtle horizontal guide lines */
    cairo_set_source_rgba(cr, 0.5, 0.55, 0.65, 0.18);
    cairo_set_line_width(cr, 1.0);
    for (int g = 0; g <= 2; g++) {
        double gy = pad_t + ch * (g / 2.0);
        cairo_move_to(cr, pad_l, gy);
        cairo_line_to(cr, width - pad_r, gy);
    }
    cairo_stroke(cr);

    /* Precipitation probability bars along the bottom */
    for (int i = 0; i < n; i++) {
        double x = pad_l + (cw * i) / (n - 1);
        double pop = active_fc.hourly[i].pop_pct / 100.0;
        if (pop > 0.05) {
            double bh = pop * (ch * 0.38);
            cairo_set_source_rgba(cr, 0.21, 0.52, 0.89, 0.22);
            cairo_rectangle(cr, x - 5.0, pad_t + ch - bh, 10.0, bh);
            cairo_fill(cr);
        }
    }

    /* Area fill under temperature curve */
    cairo_pattern_t * grad = cairo_pattern_create_linear(0, pad_t, 0, pad_t + ch);
    cairo_pattern_add_color_stop_rgba(grad, 0.0, 0.96, 0.62, 0.18, 0.38);
    cairo_pattern_add_color_stop_rgba(grad, 1.0, 0.21, 0.52, 0.89, 0.03);

    for (int i = 0; i < n; i++) {
        double x = pad_l + (cw * i) / (n - 1);
        double norm = (active_fc.hourly[i].temp_c - tmin) / (tmax - tmin);
        double y = pad_t + ch * (1.0 - norm);
        if (i == 0) cairo_move_to(cr, x, y);
        else cairo_line_to(cr, x, y);
    }
    cairo_line_to(cr, pad_l + cw, pad_t + ch);
    cairo_line_to(cr, pad_l, pad_t + ch);
    cairo_close_path(cr);
    cairo_set_source(cr, grad);
    cairo_fill(cr);
    cairo_pattern_destroy(grad);

    /* Temperature curve line */
    cairo_set_source_rgb(cr, 0.95, 0.58, 0.14);
    cairo_set_line_width(cr, 2.6);
    for (int i = 0; i < n; i++) {
        double x = pad_l + (cw * i) / (n - 1);
        double norm = (active_fc.hourly[i].temp_c - tmin) / (tmax - tmin);
        double y = pad_t + ch * (1.0 - norm);
        if (i == 0) cairo_move_to(cr, x, y);
        else cairo_line_to(cr, x, y);
    }
    cairo_stroke(cr);

    /* Points and labels every 3 hours */
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    for (int i = 0; i < n; i += 3) {
        double x = pad_l + (cw * i) / (n - 1);
        double norm = (active_fc.hourly[i].temp_c - tmin) / (tmax - tmin);
        double y = pad_t + ch * (1.0 - norm);

        cairo_set_source_rgb(cr, 0.95, 0.58, 0.14);
        cairo_arc(cr, x, y, 3.8, 0, 2 * G_PI);
        cairo_fill(cr);

        char tbuf[16];
        format_temp(active_fc.hourly[i].temp_c, tbuf, sizeof(tbuf), FALSE);
        cairo_set_font_size(cr, 11.0);
        cairo_set_source_rgba(cr, 0.55, 0.60, 0.68, 0.95);
        cairo_move_to(cr, x - 10.0, y - 8.0);
        cairo_show_text(cr, tbuf);

        cairo_move_to(cr, x - 14.0, height - 8.0);
        cairo_show_text(cr, active_fc.hourly[i].label);
    }

    return FALSE;
}

typedef struct {
    double day_min_c;
    double day_max_c;
    double week_min_c;
    double week_max_c;
    gboolean is_today;
    double cur_temp_c;
} TempBarData;

static gboolean on_day_range_draw(GtkWidget *w, cairo_t *cr, gpointer user_data)
{
    TempBarData *d = user_data;
    if (!d) return FALSE;
    int width = gtk_widget_get_allocated_width(w);
    int height = gtk_widget_get_allocated_height(w);
    double bh = 7.0;
    double by = (height - bh) / 2.0;

    /* Track background */
    cairo_set_source_rgba(cr, 0.5, 0.55, 0.65, 0.20);
    cairo_rectangle(cr, 0, by, width, bh);
    cairo_fill(cr);

    double span = d->week_max_c - d->week_min_c;
    if (span < 1.0) span = 1.0;
    double x0 = CLAMP((d->day_min_c - d->week_min_c) / span, 0.0, 1.0) * width;
    double x1 = CLAMP((d->day_max_c - d->week_min_c) / span, 0.0, 1.0) * width;
    if (x1 - x0 < 8.0) x1 = MIN((double)width, x0 + 8.0);

    cairo_pattern_t *grad = cairo_pattern_create_linear(x0, 0, x1, 0);
    cairo_pattern_add_color_stop_rgb(grad, 0.0, 0.21, 0.58, 0.92);
    cairo_pattern_add_color_stop_rgb(grad, 0.5, 0.35, 0.78, 0.52);
    cairo_pattern_add_color_stop_rgb(grad, 1.0, 0.96, 0.58, 0.14);
    cairo_set_source(cr, grad);
    cairo_rectangle(cr, x0, by, x1 - x0, bh);
    cairo_fill(cr);
    cairo_pattern_destroy(grad);

    if (d->is_today) {
        double cx = CLAMP((d->cur_temp_c - d->week_min_c) / span, 0.0, 1.0) * width;
        cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
        cairo_arc(cr, cx, height / 2.0, 5.0, 0, 2 * G_PI);
        cairo_fill(cr);
        cairo_set_source_rgb(cr, 0.15, 0.20, 0.28);
        cairo_arc(cr, cx, height / 2.0, 2.5, 0, 2 * G_PI);
        cairo_fill(cr);
    }
    return FALSE;
}

static gboolean on_compass_draw(GtkWidget *w, cairo_t *cr, gpointer user_data)
{
    (void)user_data;
    int width = gtk_widget_get_allocated_width(w);
    int height = gtk_widget_get_allocated_height(w);
    double cx = width / 2.0;
    double cy = height / 2.0;
    double r = MIN(cx, cy) - 6.0;
    if (r <= 8.0) return FALSE;

    cairo_set_source_rgba(cr, 0.5, 0.55, 0.65, 0.28);
    cairo_set_line_width(cr, 2.0);
    cairo_arc(cr, cx, cy, r, 0, 2 * G_PI);
    cairo_stroke(cr);

    /* Needle pointing along wind_dir_deg */
    double ang = (active_fc.wind_dir_deg - 90.0) * (G_PI / 180.0);
    cairo_save(cr);
    cairo_translate(cr, cx, cy);
    cairo_rotate(cr, ang);

    cairo_set_source_rgb(cr, 0.21, 0.52, 0.89);
    cairo_move_to(cr, r * 0.78, 0);
    cairo_line_to(cr, -r * 0.45, -5.0);
    cairo_line_to(cr, -r * 0.25, 0);
    cairo_line_to(cr, -r * 0.45, 5.0);
    cairo_close_path(cr);
    cairo_fill(cr);
    cairo_restore(cr);

    return FALSE;
}

/* --- Rebuild Dynamic Lists & Views --- */

static void clear_container(GtkWidget *container)
{
    GList *children = gtk_container_get_children(GTK_CONTAINER(container));
    for (GList *l = children; l; l = l->next)
        gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(children);
}

static gboolean delete_city_idle_cb(gpointer user_data)
{
    int idx = GPOINTER_TO_INT(user_data);
    if (saved_city_count <= 1 || idx < 0 || idx >= saved_city_count)
        return G_SOURCE_REMOVE;
    for (int i = idx; i + 1 < saved_city_count; i++) {
        saved_cities[i] = saved_cities[i + 1];
        forecasts[i] = forecasts[i + 1];
    }
    saved_city_count--;
    if (active_city_idx >= saved_city_count)
        active_city_idx = saved_city_count - 1;
    active_fc = forecasts[active_city_idx];
    save_config();
    refresh_all_views();
    return G_SOURCE_REMOVE;
}

static void on_delete_city_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    g_idle_add(delete_city_idle_cb, user_data);
}

static void update_sidebar_labels_in_place(void)
{
    for (int i = 0; i < saved_city_count; i++) {
        if (row_temp_lbl[i]) {
            char tbuf[24];
            format_temp(forecasts[i].current_temp_c, tbuf, sizeof(tbuf), FALSE);
            gtk_label_set_text(GTK_LABEL(row_temp_lbl[i]), tbuf);
        }
        if (row_sub_lbl[i]) {
            gtk_label_set_text(GTK_LABEL(row_sub_lbl[i]), hde_weather_wmo_description(forecasts[i].wmo_code));
        }
        if (row_icon_img[i]) {
            const char *ic = hde_weather_wmo_icon(forecasts[i].wmo_code, forecasts[i].is_day);
            gtk_image_set_from_icon_name(GTK_IMAGE(row_icon_img[i]), ic, GTK_ICON_SIZE_LARGE_TOOLBAR);
        }
    }
}

static void rebuild_city_sidebar(void)
{
    if (!city_list_box) return;
    rebuilding_sidebar = TRUE;
    memset(row_temp_lbl, 0, sizeof(row_temp_lbl));
    memset(row_sub_lbl, 0, sizeof(row_sub_lbl));
    memset(row_icon_img, 0, sizeof(row_icon_img));
    clear_container(city_list_box);

    const char *filter = city_filter_entry ? gtk_entry_get_text(GTK_ENTRY(city_filter_entry)) : "";

    for (int i = 0; i < saved_city_count; i++) {
        if (filter && *filter) {
            if (!strstr(saved_cities[i].name, filter) &&
                g_ascii_strncasecmp(saved_cities[i].name, filter, strlen(filter)) != 0 &&
                g_ascii_strncasecmp(saved_cities[i].country, filter, strlen(filter)) != 0)
                continue;
        }

        GtkWidget *row = gtk_list_box_row_new();
        g_object_set_data(G_OBJECT(row), "city-idx", GINT_TO_POINTER(i));

        GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_container_set_border_width(GTK_CONTAINER(hb), 8);

        const char *icon_name = hde_weather_wmo_icon(forecasts[i].wmo_code, forecasts[i].is_day);
        GtkWidget *img = gtk_image_new_from_icon_name(icon_name, GTK_ICON_SIZE_LARGE_TOOLBAR);
        row_icon_img[i] = img;
        gtk_box_pack_start(GTK_BOX(hb), img, FALSE, FALSE, 0);

        GtkWidget *vb = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        GtkWidget *nm = gtk_label_new(saved_cities[i].name);
        gtk_style_context_add_class(gtk_widget_get_style_context(nm), "city-row-title");
        gtk_label_set_xalign(GTK_LABEL(nm), 0.0);
        gtk_label_set_ellipsize(GTK_LABEL(nm), PANGO_ELLIPSIZE_END);

        const char *cond = hde_weather_wmo_description(forecasts[i].wmo_code);
        GtkWidget *sub = gtk_label_new(cond);
        row_sub_lbl[i] = sub;
        gtk_style_context_add_class(gtk_widget_get_style_context(sub), "dim-label");
        gtk_label_set_xalign(GTK_LABEL(sub), 0.0);
        gtk_box_pack_start(GTK_BOX(vb), nm, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(vb), sub, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(hb), vb, TRUE, TRUE, 0);

        char tbuf[24];
        format_temp(forecasts[i].current_temp_c, tbuf, sizeof(tbuf), FALSE);
        GtkWidget *tlbl = gtk_label_new(tbuf);
        row_temp_lbl[i] = tlbl;
        gtk_style_context_add_class(gtk_widget_get_style_context(tlbl), "city-row-temp");
        gtk_box_pack_start(GTK_BOX(hb), tlbl, FALSE, FALSE, 2);

        if (saved_city_count > 1) {
            GtkWidget *del_btn = gtk_button_new_from_icon_name("window-close-symbolic", GTK_ICON_SIZE_MENU);
            gtk_button_set_relief(GTK_BUTTON(del_btn), GTK_RELIEF_NONE);
            gtk_widget_set_tooltip_text(del_btn, "Remove location");
            g_signal_connect(del_btn, "clicked", G_CALLBACK(on_delete_city_clicked), GINT_TO_POINTER(i));
            gtk_box_pack_start(GTK_BOX(hb), del_btn, FALSE, FALSE, 0);
        }

        gtk_container_add(GTK_CONTAINER(row), hb);
        gtk_list_box_insert(GTK_LIST_BOX(city_list_box), row, -1);
        if (i == active_city_idx)
            gtk_list_box_select_row(GTK_LIST_BOX(city_list_box), GTK_LIST_BOX_ROW(row));
    }
    gtk_widget_show_all(city_list_box);
    rebuilding_sidebar = FALSE;
}

static void rebuild_hourly_strip(void)
{
    if (!hourly_box) return;
    clear_container(hourly_box);

    for (int i = 0; i < active_fc.hourly_count; i++) {
        GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_style_context_add_class(gtk_widget_get_style_context(card), "weather-hour-card");
        gtk_widget_set_size_request(card, 72, -1);

        GtkWidget *tl = gtk_label_new(active_fc.hourly[i].label);
        gtk_style_context_add_class(gtk_widget_get_style_context(tl), "dim-label");

        const char *ic = hde_weather_wmo_icon(active_fc.hourly[i].wmo_code, active_fc.hourly[i].is_day);
        GtkWidget *img = gtk_image_new_from_icon_name(ic, GTK_ICON_SIZE_LARGE_TOOLBAR);

        char tbuf[24];
        format_temp(active_fc.hourly[i].temp_c, tbuf, sizeof(tbuf), FALSE);
        GtkWidget *temp_l = gtk_label_new(tbuf);
        gtk_style_context_add_class(gtk_widget_get_style_context(temp_l), "hour-temp-label");

        char pbuf[24];
        snprintf(pbuf, sizeof(pbuf), "%d%%", active_fc.hourly[i].pop_pct);
        GtkWidget *pop_l = gtk_label_new(pbuf);
        gtk_style_context_add_class(gtk_widget_get_style_context(pop_l), "rain-pill");

        gtk_box_pack_start(GTK_BOX(card), tl, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(card), img, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(card), temp_l, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(card), pop_l, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(hourly_box), card, FALSE, FALSE, 0);
    }
    gtk_widget_show_all(hourly_box);
}

static void rebuild_daily_list(void)
{
    if (!daily_box) return;
    clear_container(daily_box);

    double wmin = 100.0, wmax = -100.0;
    for (int i = 0; i < active_fc.daily_count; i++) {
        if (active_fc.daily[i].temp_min_c < wmin) wmin = active_fc.daily[i].temp_min_c;
        if (active_fc.daily[i].temp_max_c > wmax) wmax = active_fc.daily[i].temp_max_c;
    }

    for (int i = 0; i < active_fc.daily_count; i++) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        gtk_style_context_add_class(gtk_widget_get_style_context(row), "weather-day-row");

        /* Day & Date */
        GtkWidget *dbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
        gtk_widget_set_size_request(dbox, 90, -1);
        GtkWidget *dl = gtk_label_new(active_fc.daily[i].day_name);
        gtk_style_context_add_class(gtk_widget_get_style_context(dl), "city-row-title");
        gtk_label_set_xalign(GTK_LABEL(dl), 0.0);
        GtkWidget *dtl = gtk_label_new(active_fc.daily[i].date_label);
        gtk_style_context_add_class(gtk_widget_get_style_context(dtl), "dim-label");
        gtk_label_set_xalign(GTK_LABEL(dtl), 0.0);
        gtk_box_pack_start(GTK_BOX(dbox), dl, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(dbox), dtl, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(row), dbox, FALSE, FALSE, 0);

        /* Icon + Condition */
        const char *ic = hde_weather_wmo_icon(active_fc.daily[i].wmo_code, 1);
        GtkWidget *img = gtk_image_new_from_icon_name(ic, GTK_ICON_SIZE_LARGE_TOOLBAR);
        gtk_box_pack_start(GTK_BOX(row), img, FALSE, FALSE, 0);

        GtkWidget *cl = gtk_label_new(hde_weather_wmo_description(active_fc.daily[i].wmo_code));
        gtk_label_set_xalign(GTK_LABEL(cl), 0.0);
        gtk_widget_set_size_request(cl, 150, -1);
        gtk_box_pack_start(GTK_BOX(row), cl, FALSE, FALSE, 0);

        /* Precipitation probability */
        char pop_str[32];
        snprintf(pop_str, sizeof(pop_str), "%d%% rain", active_fc.daily[i].pop_pct);
        GtkWidget *pl = gtk_label_new(pop_str);
        gtk_style_context_add_class(gtk_widget_get_style_context(pl), "rain-pill");
        gtk_widget_set_size_request(pl, 74, -1);
        gtk_box_pack_start(GTK_BOX(row), pl, FALSE, FALSE, 0);

        /* Min temp + Range Bar + Max temp */
        char min_b[24], max_b[24];
        format_temp(active_fc.daily[i].temp_min_c, min_b, sizeof(min_b), FALSE);
        format_temp(active_fc.daily[i].temp_max_c, max_b, sizeof(max_b), FALSE);

        GtkWidget *min_l = gtk_label_new(min_b);
        gtk_style_context_add_class(gtk_widget_get_style_context(min_l), "dim-label");
        gtk_widget_set_size_request(min_l, 42, -1);
        gtk_label_set_xalign(GTK_LABEL(min_l), 1.0);
        gtk_box_pack_start(GTK_BOX(row), min_l, FALSE, FALSE, 0);

        GtkWidget *bar = gtk_drawing_area_new();
        gtk_widget_set_size_request(bar, 140, 20);
        TempBarData *tbd = g_new0(TempBarData, 1);
        tbd->day_min_c = active_fc.daily[i].temp_min_c;
        tbd->day_max_c = active_fc.daily[i].temp_max_c;
        tbd->week_min_c = wmin;
        tbd->week_max_c = wmax;
        tbd->is_today = (i == 0);
        tbd->cur_temp_c = active_fc.current_temp_c;
        g_object_set_data_full(G_OBJECT(bar), "tbd", tbd, g_free);
        g_signal_connect(bar, "draw", G_CALLBACK(on_day_range_draw), tbd);
        gtk_box_pack_start(GTK_BOX(row), bar, TRUE, TRUE, 0);

        GtkWidget *max_l = gtk_label_new(max_b);
        gtk_style_context_add_class(gtk_widget_get_style_context(max_l), "hour-temp-label");
        gtk_widget_set_size_request(max_l, 42, -1);
        gtk_label_set_xalign(GTK_LABEL(max_l), 0.0);
        gtk_box_pack_start(GTK_BOX(row), max_l, FALSE, FALSE, 0);

        gtk_box_pack_start(GTK_BOX(daily_box), row, FALSE, FALSE, 0);
    }
    gtk_widget_show_all(daily_box);
}

static void update_detail_cards(void)
{
    char buf[128], sub[160];

    /* 1. UV Index */
    snprintf(buf, sizeof(buf), "%.1f  (%s)", active_fc.uv_index, hde_weather_uv_category(active_fc.uv_index));
    gtk_label_set_text(GTK_LABEL(lbl_uv_val), buf);
    gtk_label_set_text(GTK_LABEL(lbl_uv_sub),
        active_fc.uv_index >= 6.0 ? "Sun protection recommended during midday hours."
                                  : "Low to moderate UV exposure right now.");
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(bar_uv), CLAMP(active_fc.uv_index / 12.0, 0.0, 1.0));

    /* 2. Wind */
    char w_spd[48], g_spd[48];
    format_wind(active_fc.wind_kmh, w_spd, sizeof(w_spd));
    format_wind(active_fc.wind_gust_kmh, g_spd, sizeof(g_spd));
    snprintf(buf, sizeof(buf), "%s  %s", w_spd, hde_weather_wind_compass(active_fc.wind_dir_deg));
    snprintf(sub, sizeof(sub), "Direction %d° (%s)  •  Gusts up to %s",
             active_fc.wind_dir_deg, hde_weather_wind_compass(active_fc.wind_dir_deg), g_spd);
    gtk_label_set_text(GTK_LABEL(lbl_wind_val), buf);
    gtk_label_set_text(GTK_LABEL(lbl_wind_sub), sub);
    if (compass_area) gtk_widget_queue_draw(compass_area);

    /* 3. Humidity & Dew Point */
    double dp_c = hde_weather_dew_point_c(active_fc.current_temp_c, active_fc.humidity_pct);
    char dp_str[32];
    format_temp(dp_c, dp_str, sizeof(dp_str), TRUE);
    snprintf(buf, sizeof(buf), "%d%%", active_fc.humidity_pct);
    snprintf(sub, sizeof(sub), "The dew point is %s right now.", dp_str);
    gtk_label_set_text(GTK_LABEL(lbl_hum_val), buf);
    gtk_label_set_text(GTK_LABEL(lbl_hum_sub), sub);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(bar_hum), CLAMP(active_fc.humidity_pct / 100.0, 0.0, 1.0));

    /* 4. Pressure */
    format_pressure(active_fc.pressure_hpa, buf, sizeof(buf));
    gtk_label_set_text(GTK_LABEL(lbl_press_val), buf);
    gtk_label_set_text(GTK_LABEL(lbl_press_sub),
        active_fc.pressure_hpa >= 1015.0 ? "High pressure system — generally fair skies."
        : (active_fc.pressure_hpa <= 1005.0 ? "Low pressure system — unsettled conditions."
                                            : "Normal standard sea-level atmospheric pressure."));
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(bar_press),
        CLAMP((active_fc.pressure_hpa - 980.0) / 60.0, 0.0, 1.0));

    /* 5. Visibility & Cloud Cover */
    snprintf(buf, sizeof(buf), "%.1f km", active_fc.visibility_km);
    snprintf(sub, sizeof(sub), "Cloud cover %d%%  •  %s",
             active_fc.cloud_cover_pct,
             active_fc.visibility_km >= 9.0 ? "Clear horizon visibility." : "Reduced visibility due to mist/rain.");
    gtk_label_set_text(GTK_LABEL(lbl_vis_val), buf);
    gtk_label_set_text(GTK_LABEL(lbl_vis_sub), sub);

    /* 6. Sunrise & Sunset */
    const char *srise = active_fc.daily_count > 0 ? active_fc.daily[0].sunrise : "06:00";
    const char *sset  = active_fc.daily_count > 0 ? active_fc.daily[0].sunset  : "18:00";
    snprintf(buf, sizeof(buf), "Sunrise %s", srise);
    snprintf(sub, sizeof(sub), "Sunset at %s local time.", sset);
    gtk_label_set_text(GTK_LABEL(lbl_sun_val), buf);
    gtk_label_set_text(GTK_LABEL(lbl_sun_sub), sub);

    /* 7. Precipitation */
    snprintf(buf, sizeof(buf), "%.1f mm", active_fc.precip_mm);
    int today_pop = active_fc.daily_count > 0 ? active_fc.daily[0].pop_pct : 20;
    snprintf(sub, sizeof(sub), "Daily precipitation chance: %d%%", today_pop);
    gtk_label_set_text(GTK_LABEL(lbl_precip_val), buf);
    gtk_label_set_text(GTK_LABEL(lbl_precip_sub), sub);

    /* 8. Air Quality */
    snprintf(buf, sizeof(buf), "AQI %d  (%s)", active_fc.aqi, hde_weather_aqi_category(active_fc.aqi));
    gtk_label_set_text(GTK_LABEL(lbl_aqi_val), buf);
    gtk_label_set_text(GTK_LABEL(lbl_aqi_sub),
        active_fc.aqi <= 50 ? "Air quality is satisfactory for outdoor activities."
                            : "Moderate air quality — sensitive groups should take care.");
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(bar_aqi), CLAMP(active_fc.aqi / 200.0, 0.0, 1.0));
}

static void refresh_forecast_views(void)
{
    if (unit_temp_btn)
        gtk_button_set_label(GTK_BUTTON(unit_temp_btn), use_fahrenheit ? "°F" : "°C");
    update_sidebar_labels_in_place();
    rebuild_hourly_strip();
    rebuild_daily_list();
    update_detail_cards();
    if (hero_area) gtk_widget_queue_draw(hero_area);
    if (chart_area) gtk_widget_queue_draw(chart_area);
}

static void refresh_all_views(void)
{
    rebuild_city_sidebar();
    refresh_forecast_views();
}

/* --- Add Location Dialog --- */

static void add_city_and_select(const HdeWeatherCity *c)
{
    if (!c || !c->name[0]) return;
    for (int i = 0; i < saved_city_count; i++) {
        if (g_ascii_strcasecmp(saved_cities[i].name, c->name) == 0) {
            active_city_idx = i;
            active_fc = forecasts[i];
            save_config();
            refresh_all_views();
            start_async_fetch_for_city(i);
            return;
        }
    }
    if (saved_city_count >= MAX_SAVED_CITIES) return;
    int idx = saved_city_count++;
    saved_cities[idx] = *c;
    hde_weather_generate_fallback(c, time(NULL), &forecasts[idx]);
    active_city_idx = idx;
    active_fc = forecasts[idx];
    save_config();
    refresh_all_views();
    start_async_fetch_for_city(idx);
}

typedef struct {
    GtkWidget *dialog;
    GtkWidget *search_entry;
    GtkWidget *list_box;
    HdeWeatherCity results[32];
    int result_count;
} AddCityDialogData;

static void populate_add_city_results(AddCityDialogData *d, const char *query)
{
    clear_container(d->list_box);
    d->result_count = hde_weather_search_builtin_cities(query, d->results, 32);

    for (int i = 0; i < d->result_count; i++) {
        GtkWidget *row = gtk_list_box_row_new();
        g_object_set_data(G_OBJECT(row), "res-idx", GINT_TO_POINTER(i));
        GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_container_set_border_width(GTK_CONTAINER(hb), 8);

        char label[160];
        snprintf(label, sizeof(label), "%s, %s", d->results[i].name, d->results[i].country);
        GtkWidget *nl = gtk_label_new(label);
        gtk_label_set_xalign(GTK_LABEL(nl), 0.0);
        gtk_box_pack_start(GTK_BOX(hb), nl, TRUE, TRUE, 0);

        char coords[64];
        snprintf(coords, sizeof(coords), "%.2f°, %.2f°", d->results[i].lat, d->results[i].lon);
        GtkWidget *cl = gtk_label_new(coords);
        gtk_style_context_add_class(gtk_widget_get_style_context(cl), "dim-label");
        gtk_box_pack_start(GTK_BOX(hb), cl, FALSE, FALSE, 0);

        gtk_container_add(GTK_CONTAINER(row), hb);
        gtk_list_box_insert(GTK_LIST_BOX(d->list_box), row, -1);
        if (i == 0)
            gtk_list_box_select_row(GTK_LIST_BOX(d->list_box), GTK_LIST_BOX_ROW(row));
    }
    gtk_widget_show_all(d->list_box);
}

static void on_add_search_changed(GtkEditable *ed, gpointer user_data)
{
    AddCityDialogData *d = user_data;
    populate_add_city_results(d, gtk_entry_get_text(GTK_ENTRY(ed)));
}

static void on_add_row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer user_data)
{
    AddCityDialogData *d = user_data;
    if (row) {
        gtk_list_box_select_row(box, row);
        gtk_dialog_response(GTK_DIALOG(d->dialog), GTK_RESPONSE_ACCEPT);
    }
}

static void on_add_city_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    (void)user_data;

    GtkWidget *dlg = gtk_dialog_new_with_buttons(
        "Add Location",
        GTK_WINDOW(main_win),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Add Selected", GTK_RESPONSE_ACCEPT,
        NULL);
    gtk_window_set_default_size(GTK_WINDOW(dlg), 460, 420);

    AddCityDialogData d;
    memset(&d, 0, sizeof(d));
    d.dialog = dlg;

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 12);

    d.search_entry = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(d.search_entry), "Search city or country (e.g. Tokyo, Vietnam, Paris)...");
    gtk_box_pack_start(GTK_BOX(vbox), d.search_entry, FALSE, FALSE, 0);

    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(sw, TRUE);

    d.list_box = gtk_list_box_new();
    gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(d.list_box), FALSE);
    gtk_container_add(GTK_CONTAINER(sw), d.list_box);
    gtk_box_pack_start(GTK_BOX(vbox), sw, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(content), vbox);

    populate_add_city_results(&d, "");
    g_signal_connect(d.search_entry, "changed", G_CALLBACK(on_add_search_changed), &d);
    g_signal_connect(d.list_box, "row-activated", G_CALLBACK(on_add_row_activated), &d);

    gtk_widget_show_all(dlg);
    HdeWeatherCity chosen;
    gboolean has_chosen = FALSE;
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        GtkListBoxRow *sel = gtk_list_box_get_selected_row(GTK_LIST_BOX(d.list_box));
        if (sel) {
            int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(sel), "res-idx"));
            if (idx >= 0 && idx < d.result_count) {
                chosen = d.results[idx];
                has_chosen = TRUE;
            }
        } else if (d.result_count > 0) {
            chosen = d.results[0];
            has_chosen = TRUE;
        }
    }
    gtk_widget_destroy(dlg);
    if (has_chosen)
        add_city_and_select(&chosen);
}

/* --- Signal Callbacks --- */

static void on_city_row_selected(GtkListBox *box, GtkListBoxRow *row, gpointer user_data)
{
    (void)box;
    (void)user_data;
    if (rebuilding_sidebar || !row)
        return;
    int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "city-idx"));
    if (idx >= 0 && idx < saved_city_count && idx != active_city_idx) {
        active_city_idx = idx;
        active_fc = forecasts[idx];
        save_config();
        refresh_forecast_views();
        start_async_fetch_for_city(idx);
    }
}

static void on_city_filter_changed(GtkEditable *ed, gpointer user_data)
{
    (void)ed;
    (void)user_data;
    rebuild_city_sidebar();
}

static void on_refresh_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    (void)user_data;
    hde_weather_generate_fallback(&saved_cities[active_city_idx], time(NULL), &forecasts[active_city_idx]);
    active_fc = forecasts[active_city_idx];
    refresh_forecast_views();
    start_async_fetch_for_city(active_city_idx);
}

static void on_temp_unit_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    (void)user_data;
    use_fahrenheit = !use_fahrenheit;
    save_config();
    refresh_all_views();
}

static void on_wind_unit_changed(GtkComboBox *combo, gpointer user_data)
{
    (void)user_data;
    wind_unit = CLAMP(gtk_combo_box_get_active(combo), 0, 2);
    save_config();
    refresh_all_views();
}

static gboolean on_anim_tick(gpointer user_data)
{
    (void)user_data;
    if (!anim_enabled) return G_SOURCE_CONTINUE;
    anim_phase += 0.05;
    if (hero_area && gtk_widget_get_mapped(hero_area))
        gtk_widget_queue_draw(hero_area);
    return G_SOURCE_CONTINUE;
}

static void show_about_dialog(void)
{
    const char *authors[] = { "Hyggshi OS Research Technology", NULL };
    gtk_show_about_dialog(GTK_WINDOW(main_win),
        "program-name", "Hyggshi Weather",
        "version", HDE_VERSION,
        "comments", "Desktop weather forecast, 24-hour temperature & precipitation chart, 7-day outlook, and atmospheric metrics for Hyggshi Desktop Environment.",
        "website", "https://github.com/Hyggshi-OS-Research-Technology/NexWM",
        "website-label", "NexWM / HDE Repository",
        "copyright", "Copyright (c) 2024-2026 Hyggshi OS Research Technology",
        "license-type", GTK_LICENSE_MIT_X11,
        "authors", authors,
        "logo-icon-name", "weather-clear",
        NULL);
}

static void on_about_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    (void)user_data;
    show_about_dialog();
}

static gboolean on_window_key_press(GtkWidget *w, GdkEventKey *ev, gpointer user_data)
{
    (void)w;
    (void)user_data;
    guint mods = ev->state & gtk_accelerator_get_default_mod_mask();
    if (ev->keyval == GDK_KEY_F1) {
        show_about_dialog();
        return TRUE;
    }
    if (ev->keyval == GDK_KEY_F5 || (mods == GDK_CONTROL_MASK && (ev->keyval == GDK_KEY_r || ev->keyval == GDK_KEY_R))) {
        on_refresh_clicked(NULL, NULL);
        return TRUE;
    }
    if (mods == GDK_CONTROL_MASK && (ev->keyval == GDK_KEY_q || ev->keyval == GDK_KEY_Q)) {
        gtk_widget_destroy(main_win);
        return TRUE;
    }
    return FALSE;
}

/* --- Helper to build Detail Card --- */

static GtkWidget *make_detail_card(const char *title, const char *icon_name,
                                   GtkWidget **val_lbl_out, GtkWidget **sub_lbl_out,
                                   GtkWidget *extra_widget)
{
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_style_context_add_class(gtk_widget_get_style_context(card), "weather-detail-card");
    gtk_widget_set_hexpand(card, TRUE);

    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *ic = gtk_image_new_from_icon_name(icon_name, GTK_ICON_SIZE_MENU);
    GtkWidget *tl = gtk_label_new(title);
    gtk_style_context_add_class(gtk_widget_get_style_context(tl), "detail-card-title");
    gtk_label_set_xalign(GTK_LABEL(tl), 0.0);
    gtk_box_pack_start(GTK_BOX(top), ic, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(top), tl, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(card), top, FALSE, FALSE, 0);

    GtkWidget *vl = gtk_label_new("—");
    gtk_style_context_add_class(gtk_widget_get_style_context(vl), "detail-card-value");
    gtk_label_set_xalign(GTK_LABEL(vl), 0.0);
    gtk_box_pack_start(GTK_BOX(card), vl, FALSE, FALSE, 0);

    if (extra_widget)
        gtk_box_pack_start(GTK_BOX(card), extra_widget, FALSE, FALSE, 2);

    GtkWidget *sl = gtk_label_new("");
    gtk_style_context_add_class(gtk_widget_get_style_context(sl), "dim-label");
    gtk_label_set_xalign(GTK_LABEL(sl), 0.0);
    gtk_label_set_line_wrap(GTK_LABEL(sl), TRUE);
    gtk_box_pack_start(GTK_BOX(card), sl, FALSE, FALSE, 0);

    if (val_lbl_out) *val_lbl_out = vl;
    if (sub_lbl_out) *sub_lbl_out = sl;
    return card;
}

static void apply_weather_css(void)
{
    if (!app_css) {
        app_css = gtk_css_provider_new();
        gtk_style_context_add_provider_for_screen(
            gdk_screen_get_default(), GTK_STYLE_PROVIDER(app_css),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
    const char *css =
        ".weather-sidebar { background: alpha(@theme_fg_color, 0.03); border-right: 1px solid alpha(@theme_fg_color, 0.10); }"
        ".city-row-title { font-weight: 700; font-size: 13.5px; }"
        ".city-row-temp { font-weight: 700; font-size: 18px; }"
        ".weather-section-card { background: alpha(@theme_fg_color, 0.04); border: 1px solid alpha(@theme_fg_color, 0.09); border-radius: 12px; padding: 14px; }"
        ".weather-section-title { font-weight: 700; font-size: 14px; opacity: 0.85; }"
        ".weather-hour-card { background: alpha(@theme_fg_color, 0.04); border: 1px solid alpha(@theme_fg_color, 0.08); border-radius: 10px; padding: 10px 8px; }"
        ".hour-temp-label { font-weight: 700; font-size: 14px; }"
        ".rain-pill { color: #3584e4; font-weight: 600; font-size: 11.5px; }"
        ".weather-day-row { padding: 8px 6px; border-bottom: 1px solid alpha(@theme_fg_color, 0.06); }"
        ".weather-detail-card { background: alpha(@theme_fg_color, 0.04); border: 1px solid alpha(@theme_fg_color, 0.09); border-radius: 12px; padding: 14px; }"
        ".detail-card-title { font-weight: 600; font-size: 12px; opacity: 0.72; }"
        ".detail-card-value { font-weight: 700; font-size: 20px; }";
    gtk_css_provider_load_from_data(app_css, css, -1, NULL);
}

static void on_theme_changed(gpointer user_data)
{
    (void)user_data;
    hde_theme_apply_process();
    HdeThemeInfo info;
    hde_theme_info_load(&info);
    anim_enabled = info.enable_animations;
    hde_theme_info_clear(&info);
}

int main(int argc, char **argv)
{
    const char *initial_city = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--version") == 0) {
            printf("hde-weather %s (%s)\n", HDE_RELEASE, HDE_VERSION);
            return 0;
        }
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: hde-weather [--city <name>] [--version]\n"
                   "Hyggshi Weather — GTK3 desktop weather forecast and atmospheric conditions.\n");
            return 0;
        }
        if (strcmp(argv[i], "--city") == 0 && i + 1 < argc) {
            initial_city = argv[++i];
        }
    }

    gtk_init(&argc, &argv);
    hde_theme_apply_process();
    hde_theme_watch(on_theme_changed, NULL);

    HdeThemeInfo ti;
    hde_theme_info_load(&ti);
    anim_enabled = ti.enable_animations;
    hde_theme_info_clear(&ti);

    apply_weather_css();
    load_config(initial_city);

    main_win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(main_win), APP_TITLE);
    gtk_window_set_default_size(GTK_WINDOW(main_win), 1040, 720);
    gtk_window_set_icon_name(GTK_WINDOW(main_win), "weather-clear");
    g_signal_connect(main_win, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    g_signal_connect(main_win, "key-press-event", G_CALLBACK(on_window_key_press), NULL);

    /* HeaderBar */
    GtkWidget *hbar = gtk_header_bar_new();
    gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(hbar), TRUE);
    gtk_header_bar_set_title(GTK_HEADER_BAR(hbar), APP_TITLE);
    gtk_header_bar_set_subtitle(GTK_HEADER_BAR(hbar), "Forecast & Atmospheric Conditions");

    GtkWidget *add_btn = gtk_button_new_with_label("+ Add Location");
    gtk_widget_set_tooltip_text(add_btn, "Add a city to saved locations");
    g_signal_connect(add_btn, "clicked", G_CALLBACK(on_add_city_clicked), NULL);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(hbar), add_btn);

    GtkWidget *ref_btn = gtk_button_new_from_icon_name("view-refresh-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(ref_btn, "Refresh weather forecast (Ctrl+R / F5)");
    g_signal_connect(ref_btn, "clicked", G_CALLBACK(on_refresh_clicked), NULL);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(hbar), ref_btn);

    status_spinner = gtk_spinner_new();
    gtk_header_bar_pack_start(GTK_HEADER_BAR(hbar), status_spinner);

    GtkWidget *about_btn = gtk_button_new_from_icon_name("help-about-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(about_btn, "About Hyggshi Weather (F1)");
    g_signal_connect(about_btn, "clicked", G_CALLBACK(on_about_clicked), NULL);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(hbar), about_btn);

    unit_wind_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(unit_wind_combo), "km/h");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(unit_wind_combo), "mph");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(unit_wind_combo), "m/s");
    gtk_combo_box_set_active(GTK_COMBO_BOX(unit_wind_combo), wind_unit);
    g_signal_connect(unit_wind_combo, "changed", G_CALLBACK(on_wind_unit_changed), NULL);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(hbar), unit_wind_combo);

    unit_temp_btn = gtk_button_new_with_label(use_fahrenheit ? "°F" : "°C");
    gtk_widget_set_tooltip_text(unit_temp_btn, "Toggle Celsius / Fahrenheit");
    g_signal_connect(unit_temp_btn, "clicked", G_CALLBACK(on_temp_unit_clicked), NULL);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(hbar), unit_temp_btn);

    gtk_window_set_titlebar(GTK_WINDOW(main_win), hbar);

    /* Main Horizontal Paned: Sidebar (Saved Cities) + Right Scrollable Forecast View */
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_set_position(GTK_PANED(paned), 265);

    /* Sidebar */
    GtkWidget *side_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_style_context_add_class(gtk_widget_get_style_context(side_box), "weather-sidebar");
    gtk_container_set_border_width(GTK_CONTAINER(side_box), 10);

    city_filter_entry = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(city_filter_entry), "Filter saved locations...");
    g_signal_connect(city_filter_entry, "changed", G_CALLBACK(on_city_filter_changed), NULL);
    gtk_box_pack_start(GTK_BOX(side_box), city_filter_entry, FALSE, FALSE, 0);

    GtkWidget *side_sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(side_sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(side_sw, TRUE);

    city_list_box = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(city_list_box), GTK_SELECTION_SINGLE);
    gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(city_list_box), FALSE);
    g_signal_connect(city_list_box, "row-selected", G_CALLBACK(on_city_row_selected), NULL);
    gtk_container_add(GTK_CONTAINER(side_sw), city_list_box);
    gtk_box_pack_start(GTK_BOX(side_box), side_sw, TRUE, TRUE, 0);

    gtk_paned_pack1(GTK_PANED(paned), side_box, FALSE, FALSE);

    /* Right Scrollable Forecast Content */
    GtkWidget *main_sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(main_sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

    GtkWidget *content_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_container_set_border_width(GTK_CONTAINER(content_box), 18);

    /* 1. Animated Cairo Hero Banner */
    hero_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(hero_area, -1, 204);
    g_signal_connect(hero_area, "draw", G_CALLBACK(on_hero_draw), NULL);
    gtk_box_pack_start(GTK_BOX(content_box), hero_area, FALSE, FALSE, 0);

    /* 2. 24-Hour Forecast Section (Temperature Chart + Scrollable Cards) */
    GtkWidget *hourly_sec = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_style_context_add_class(gtk_widget_get_style_context(hourly_sec), "weather-section-card");

    GtkWidget *h_title = gtk_label_new("24-Hour Forecast & Temperature Trend");
    gtk_style_context_add_class(gtk_widget_get_style_context(h_title), "weather-section-title");
    gtk_label_set_xalign(GTK_LABEL(h_title), 0.0);
    gtk_box_pack_start(GTK_BOX(hourly_sec), h_title, FALSE, FALSE, 0);

    chart_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(chart_area, -1, 130);
    g_signal_connect(chart_area, "draw", G_CALLBACK(on_chart_draw), NULL);
    gtk_box_pack_start(GTK_BOX(hourly_sec), chart_area, FALSE, FALSE, 0);

    GtkWidget *h_sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(h_sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
    hourly_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_container_add(GTK_CONTAINER(h_sw), hourly_box);
    gtk_box_pack_start(GTK_BOX(hourly_sec), h_sw, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(content_box), hourly_sec, FALSE, FALSE, 0);

    /* 3. 7-Day Forecast Section */
    GtkWidget *daily_sec = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_style_context_add_class(gtk_widget_get_style_context(daily_sec), "weather-section-card");

    GtkWidget *d_title = gtk_label_new("7-Day Outlook");
    gtk_style_context_add_class(gtk_widget_get_style_context(d_title), "weather-section-title");
    gtk_label_set_xalign(GTK_LABEL(d_title), 0.0);
    gtk_box_pack_start(GTK_BOX(daily_sec), d_title, FALSE, FALSE, 0);

    daily_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_box_pack_start(GTK_BOX(daily_sec), daily_box, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(content_box), daily_sec, FALSE, FALSE, 0);

    /* 4. Atmospheric Details Grid (4x2) */
    GtkWidget *grid_title = gtk_label_new("Atmospheric Conditions & Comfort");
    gtk_style_context_add_class(gtk_widget_get_style_context(grid_title), "weather-section-title");
    gtk_label_set_xalign(GTK_LABEL(grid_title), 0.0);
    gtk_box_pack_start(GTK_BOX(content_box), grid_title, FALSE, FALSE, 0);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 12);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_grid_set_column_homogeneous(GTK_GRID(grid), TRUE);

    bar_uv = gtk_progress_bar_new();
    compass_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(compass_area, 42, 42);
    g_signal_connect(compass_area, "draw", G_CALLBACK(on_compass_draw), NULL);
    bar_hum = gtk_progress_bar_new();
    bar_press = gtk_progress_bar_new();
    bar_aqi = gtk_progress_bar_new();

    gtk_grid_attach(GTK_GRID(grid),
        make_detail_card("UV INDEX", "weather-clear-symbolic", &lbl_uv_val, &lbl_uv_sub, bar_uv),
        0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid),
        make_detail_card("WIND & GUSTS", "weather-fog-symbolic", &lbl_wind_val, &lbl_wind_sub, compass_area),
        1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid),
        make_detail_card("HUMIDITY & DEW POINT", "weather-showers-symbolic", &lbl_hum_val, &lbl_hum_sub, bar_hum),
        2, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid),
        make_detail_card("PRESSURE", "preferences-system-time", &lbl_press_val, &lbl_press_sub, bar_press),
        3, 0, 1, 1);

    gtk_grid_attach(GTK_GRID(grid),
        make_detail_card("VISIBILITY & CLOUDS", "weather-overcast-symbolic", &lbl_vis_val, &lbl_vis_sub, NULL),
        0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid),
        make_detail_card("SUNRISE & SUNSET", "weather-clear-symbolic", &lbl_sun_val, &lbl_sun_sub, NULL),
        1, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid),
        make_detail_card("PRECIPITATION", "weather-showers-scattered-symbolic", &lbl_precip_val, &lbl_precip_sub, NULL),
        2, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid),
        make_detail_card("AIR QUALITY", "weather-few-clouds-symbolic", &lbl_aqi_val, &lbl_aqi_sub, bar_aqi),
        3, 1, 1, 1);

    gtk_box_pack_start(GTK_BOX(content_box), grid, FALSE, FALSE, 0);

    gtk_container_add(GTK_CONTAINER(main_sw), content_box);
    gtk_paned_pack2(GTK_PANED(paned), main_sw, TRUE, FALSE);
    gtk_container_add(GTK_CONTAINER(main_win), paned);

    refresh_all_views();
    gtk_widget_show_all(main_win);

    anim_timer_id = g_timeout_add(33, on_anim_tick, NULL);
    start_async_fetch_for_city(active_city_idx);

    gtk_main();

    if (anim_timer_id)
        g_source_remove(anim_timer_id);
    return 0;
}
