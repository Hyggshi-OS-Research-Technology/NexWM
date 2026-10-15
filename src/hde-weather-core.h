/* hde-weather-core.h — Pure-C meteorological calculations, WMO code mapping, Open-Meteo JSON parser,
 * built-in world cities catalog, and offline deterministic forecast generator for Hyggshi Weather. */
#ifndef HDE_WEATHER_CORE_H
#define HDE_WEATHER_CORE_H

#include <stddef.h>
#include <time.h>

typedef enum {
    HDE_WEATHER_KIND_CLEAR = 0,
    HDE_WEATHER_KIND_PARTLY_CLOUDY = 1,
    HDE_WEATHER_KIND_OVERCAST = 2,
    HDE_WEATHER_KIND_FOG = 3,
    HDE_WEATHER_KIND_RAIN = 4,
    HDE_WEATHER_KIND_SNOW = 5,
    HDE_WEATHER_KIND_STORM = 6
} HdeWeatherKind;

typedef struct {
    char name[64];
    char country[64];
    double lat;
    double lon;
    double utc_offset_hours;
} HdeWeatherCity;

typedef struct {
    int hour;           /* 0..23 local hour */
    char label[16];     /* e.g. "Now", "14:00" */
    double temp_c;
    int pop_pct;        /* 0..100 precipitation probability */
    double wind_kmh;
    int wmo_code;
    int is_day;
} HdeWeatherHour;

typedef struct {
    char day_name[16];  /* e.g. "Today", "Mon", "Tue" */
    char date_label[16];/* e.g. "Oct 14" */
    int wmo_code;
    double temp_min_c;
    double temp_max_c;
    int pop_pct;
    double uv_max;
    double precip_mm;
    char sunrise[16];   /* e.g. "06:04" */
    char sunset[16];    /* e.g. "17:56" */
} HdeWeatherDay;

typedef struct {
    HdeWeatherCity city;
    int is_live;
    char updated_at[32];

    double current_temp_c;
    double feels_like_c;
    int humidity_pct;
    double wind_kmh;
    int wind_dir_deg;
    double wind_gust_kmh;
    double pressure_hpa;
    double uv_index;
    double visibility_km;
    double precip_mm;
    int cloud_cover_pct;
    int aqi;
    int wmo_code;
    int is_day;

    HdeWeatherHour hourly[24];
    int hourly_count;

    HdeWeatherDay daily[7];
    int daily_count;
} HdeWeatherForecast;

/* WMO Weather interpretation codes (WW) */
const char    *hde_weather_wmo_description(int wmo_code);
const char    *hde_weather_wmo_icon(int wmo_code, int is_day);
HdeWeatherKind hde_weather_wmo_kind(int wmo_code);

/* Unit conversions */
double hde_weather_c_to_f(double c);
double hde_weather_f_to_c(double f);
double hde_weather_kmh_to_mph(double kmh);
double hde_weather_kmh_to_ms(double kmh);
double hde_weather_hpa_to_inhg(double hpa);

/* Meteorological formulas & categories */
double      hde_weather_dew_point_c(double temp_c, int humidity_pct);
double      hde_weather_feels_like_c(double temp_c, int humidity_pct, double wind_kmh);
const char *hde_weather_wind_compass(int degrees);
const char *hde_weather_uv_category(double uv);
const char *hde_weather_aqi_category(int aqi);

/* Built-in world cities catalog & search */
const HdeWeatherCity *hde_weather_builtin_cities(int *count_out);
int hde_weather_search_builtin_cities(const char *query, HdeWeatherCity *out, int max_out);

/* Deterministic offline forecast & Open-Meteo JSON parsers */
void hde_weather_generate_fallback(const HdeWeatherCity *city, time_t now_utc, HdeWeatherForecast *out);
int  hde_weather_parse_open_meteo_json(const char *json, const HdeWeatherCity *city, HdeWeatherForecast *out);
int  hde_weather_parse_geocoding_json(const char *json, HdeWeatherCity *out, int max_out);

#endif /* HDE_WEATHER_CORE_H */
