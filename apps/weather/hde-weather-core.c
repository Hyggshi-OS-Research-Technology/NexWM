/* hde-weather-core.c — Pure-C meteorological calculations, WMO code mapping, Open-Meteo JSON parser,
 * built-in world cities catalog, and offline deterministic forecast generator for Hyggshi Weather. */
#define _POSIX_C_SOURCE 200809L

#include "hde-weather-core.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *hde_weather_wmo_description(int wmo_code)
{
    switch (wmo_code) {
    case 0:  return "Clear Sky";
    case 1:  return "Mainly Clear";
    case 2:  return "Partly Cloudy";
    case 3:  return "Overcast";
    case 45: return "Foggy";
    case 48: return "Depositing Rime Fog";
    case 51: return "Light Drizzle";
    case 53: return "Moderate Drizzle";
    case 55: return "Dense Drizzle";
    case 56:
    case 57: return "Freezing Drizzle";
    case 61: return "Slight Rain";
    case 63: return "Moderate Rain";
    case 65: return "Heavy Rain";
    case 66:
    case 67: return "Freezing Rain";
    case 71: return "Slight Snow";
    case 73: return "Moderate Snow";
    case 75: return "Heavy Snow";
    case 77: return "Snow Grains";
    case 80: return "Light Rain Showers";
    case 81: return "Moderate Rain Showers";
    case 82: return "Violent Rain Showers";
    case 85: return "Light Snow Showers";
    case 86: return "Heavy Snow Showers";
    case 95: return "Thunderstorm";
    case 96:
    case 99: return "Thunderstorm with Hail";
    default: return "Partly Cloudy";
    }
}

HdeWeatherKind hde_weather_wmo_kind(int wmo_code)
{
    if (wmo_code == 0 || wmo_code == 1)
        return HDE_WEATHER_KIND_CLEAR;
    if (wmo_code == 2)
        return HDE_WEATHER_KIND_PARTLY_CLOUDY;
    if (wmo_code == 3)
        return HDE_WEATHER_KIND_OVERCAST;
    if (wmo_code == 45 || wmo_code == 48)
        return HDE_WEATHER_KIND_FOG;
    if ((wmo_code >= 51 && wmo_code <= 67) || (wmo_code >= 80 && wmo_code <= 82))
        return HDE_WEATHER_KIND_RAIN;
    if ((wmo_code >= 71 && wmo_code <= 77) || wmo_code == 85 || wmo_code == 86)
        return HDE_WEATHER_KIND_SNOW;
    if (wmo_code >= 95)
        return HDE_WEATHER_KIND_STORM;
    return HDE_WEATHER_KIND_PARTLY_CLOUDY;
}

const char *hde_weather_wmo_icon(int wmo_code, int is_day)
{
    switch (hde_weather_wmo_kind(wmo_code)) {
    case HDE_WEATHER_KIND_CLEAR:
        return is_day ? "weather-clear" : "weather-clear-night";
    case HDE_WEATHER_KIND_PARTLY_CLOUDY:
        return is_day ? "weather-few-clouds" : "weather-few-clouds-night";
    case HDE_WEATHER_KIND_OVERCAST:
        return "weather-overcast";
    case HDE_WEATHER_KIND_FOG:
        return "weather-fog";
    case HDE_WEATHER_KIND_RAIN:
        return (wmo_code == 51 || wmo_code == 61 || wmo_code == 80)
               ? "weather-showers-scattered" : "weather-showers";
    case HDE_WEATHER_KIND_SNOW:
        return "weather-snow";
    case HDE_WEATHER_KIND_STORM:
        return "weather-storm";
    default:
        return "weather-few-clouds";
    }
}

double hde_weather_c_to_f(double c)
{
    return c * 9.0 / 5.0 + 32.0;
}

double hde_weather_f_to_c(double f)
{
    return (f - 32.0) * 5.0 / 9.0;
}

double hde_weather_kmh_to_mph(double kmh)
{
    return kmh * 0.621371192;
}

double hde_weather_kmh_to_ms(double kmh)
{
    return kmh / 3.6;
}

double hde_weather_hpa_to_inhg(double hpa)
{
    return hpa * 0.029529983;
}

double hde_weather_dew_point_c(double temp_c, int humidity_pct)
{
    double rh = humidity_pct < 1 ? 1.0 : (humidity_pct > 100 ? 100.0 : (double)humidity_pct);
    const double a = 17.625;
    const double b = 243.04;
    double alpha = log(rh / 100.0) + (a * temp_c) / (b + temp_c);
    return (b * alpha) / (a - alpha);
}

double hde_weather_feels_like_c(double temp_c, int humidity_pct, double wind_kmh)
{
    /* Wind chill for cold & windy conditions (T <= 10 C and V >= 4.8 km/h) */
    if (temp_c <= 10.0 && wind_kmh >= 4.8) {
        double v_pow = pow(wind_kmh, 0.16);
        return 13.12 + 0.6215 * temp_c - 11.37 * v_pow + 0.3965 * temp_c * v_pow;
    }
    /* Rothfusz Heat Index for warm & humid conditions (T >= 26.7 C and RH >= 40%) */
    if (temp_c >= 26.7 && humidity_pct >= 40) {
        double tf = hde_weather_c_to_f(temp_c);
        double rh = (double)humidity_pct;
        double hi_f = -42.379
                    + 2.04901523 * tf
                    + 10.14333127 * rh
                    - 0.22475541 * tf * rh
                    - 0.00683783 * tf * tf
                    - 0.05481717 * rh * rh
                    + 0.00122874 * tf * tf * rh
                    + 0.00085282 * tf * rh * rh
                    - 0.00000199 * tf * tf * rh * rh;
        return hde_weather_f_to_c(hi_f);
    }
    return temp_c;
}

const char *hde_weather_wind_compass(int degrees)
{
    static const char *const dirs[8] = {
        "N", "NE", "E", "SE", "S", "SW", "W", "NW"
    };
    int norm = ((degrees % 360) + 360) % 360;
    int idx = ((norm + 22) / 45) % 8;
    return dirs[idx];
}

const char *hde_weather_uv_category(double uv)
{
    if (uv < 3.0)  return "Low";
    if (uv < 6.0)  return "Moderate";
    if (uv < 8.0)  return "High";
    if (uv < 11.0) return "Very High";
    return "Extreme";
}

const char *hde_weather_aqi_category(int aqi)
{
    if (aqi <= 50)  return "Good";
    if (aqi <= 100) return "Moderate";
    if (aqi <= 150) return "Sensitive";
    if (aqi <= 200) return "Unhealthy";
    if (aqi <= 300) return "Very Unhealthy";
    return "Hazardous";
}

static const HdeWeatherCity builtin_cities[] = {
    { "Ho Chi Minh City", "Vietnam",        10.8231, 106.6297,  7.0 },
    { "Hanoi",            "Vietnam",        21.0285, 105.8542,  7.0 },
    { "Da Nang",          "Vietnam",        16.0544, 108.2022,  7.0 },
    { "Can Tho",          "Vietnam",        10.0452, 105.7469,  7.0 },
    { "Tokyo",            "Japan",          35.6762, 139.6503,  9.0 },
    { "Osaka",            "Japan",          34.6937, 135.5023,  9.0 },
    { "Seoul",            "South Korea",    37.5665, 126.9780,  9.0 },
    { "Singapore",        "Singapore",       1.3521, 103.8198,  8.0 },
    { "Bangkok",          "Thailand",       13.7563, 100.5018,  7.0 },
    { "Taipei",           "Taiwan",         25.0330, 121.5654,  8.0 },
    { "Beijing",          "China",          39.9042, 116.4074,  8.0 },
    { "Shanghai",         "China",          31.2304, 121.4737,  8.0 },
    { "Sydney",           "Australia",     -33.8688, 151.2093, 10.0 },
    { "Melbourne",        "Australia",     -37.8136, 144.9631, 10.0 },
    { "Auckland",         "New Zealand",   -36.8485, 174.7633, 12.0 },
    { "Mumbai",           "India",          19.0760,  72.8777,  5.5 },
    { "Dubai",            "UAE",            25.2048,  55.2708,  4.0 },
    { "Cairo",            "Egypt",          30.0444,  31.2357,  2.0 },
    { "London",           "United Kingdom", 51.5074,  -0.1278,  0.0 },
    { "Paris",            "France",         48.8566,   2.3522,  1.0 },
    { "Berlin",           "Germany",        52.5200,  13.4050,  1.0 },
    { "Zurich",           "Switzerland",    47.3769,   8.5417,  1.0 },
    { "Madrid",           "Spain",          40.4168,  -3.7038,  1.0 },
    { "Rome",             "Italy",          41.9028,  12.4964,  1.0 },
    { "Amsterdam",        "Netherlands",    52.3676,   4.9041,  1.0 },
    { "Stockholm",        "Sweden",         59.3293,  18.0686,  1.0 },
    { "Moscow",           "Russia",         55.7558,  37.6173,  3.0 },
    { "New York",         "United States",  40.7128, -74.0060, -5.0 },
    { "San Francisco",    "United States",  37.7749,-122.4194, -8.0 },
    { "Chicago",          "United States",  41.8781, -87.6298, -6.0 },
    { "Toronto",          "Canada",         43.6532, -79.3832, -5.0 },
    { "Vancouver",        "Canada",         49.2827,-123.1207, -8.0 },
    { "Mexico City",      "Mexico",         19.4326, -99.1332, -6.0 },
    { "Sao Paulo",        "Brazil",        -23.5505, -46.6333, -3.0 },
    { "Buenos Aires",     "Argentina",     -34.6037, -58.3816, -3.0 }
};

const HdeWeatherCity *hde_weather_builtin_cities(int *count_out)
{
    if (count_out)
        *count_out = (int)(sizeof(builtin_cities) / sizeof(builtin_cities[0]));
    return builtin_cities;
}

static int contains_icase(const char *haystack, const char *needle)
{
    if (!needle || !*needle) return 1;
    if (!haystack) return 0;
    size_t nlen = strlen(needle);
    for (const char *p = haystack; *p; p++) {
        size_t i = 0;
        while (i < nlen && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i]))
            i++;
        if (i == nlen)
            return 1;
    }
    return 0;
}

int hde_weather_search_builtin_cities(const char *query, HdeWeatherCity *out, int max_out)
{
    if (!out || max_out <= 0) return 0;
    int total = 0;
    const HdeWeatherCity *all = hde_weather_builtin_cities(&total);
    int found = 0;
    for (int i = 0; i < total && found < max_out; i++) {
        if (contains_icase(all[i].name, query) || contains_icase(all[i].country, query)) {
            out[found++] = all[i];
        }
    }
    return found;
}

void hde_weather_generate_fallback(const HdeWeatherCity *city, time_t now_utc, HdeWeatherForecast *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (city) {
        out->city = *city;
    } else {
        out->city = builtin_cities[0];
    }
    out->is_live = 0;

    if (now_utc <= 0)
        now_utc = time(NULL);

    time_t local_ts = now_utc + (time_t)(out->city.utc_offset_hours * 3600.0);
    struct tm lt;
    gmtime_r(&local_ts, &lt);

    snprintf(out->updated_at, sizeof(out->updated_at), "%02d:%02d", lt.tm_hour, lt.tm_min);

    /* Compute baseline temperature from latitude and day-of-year */
    double abs_lat = fabs(out->city.lat);
    double annual_mean = 28.5 - 0.42 * abs_lat;
    /* Northern vs Southern hemisphere seasonal phase */
    double season_phase = ((lt.tm_yday - 195.0) / 365.0) * 2.0 * 3.14159265358979323846;
    if (out->city.lat < 0)
        season_phase += 3.14159265358979323846;
    double seasonal_amp = 2.0 + 0.22 * abs_lat;
    double day_mean = annual_mean + seasonal_amp * cos(season_phase);

    /* Deterministic city seed from name & coordinates */
    unsigned int seed = 2166136261u;
    for (const char *s = out->city.name; *s; s++)
        seed = (seed ^ (unsigned char)(*s)) * 16777619u;
    seed ^= (unsigned int)(lt.tm_yday * 131u);

    static const int sample_wmos[] = { 0, 1, 2, 2, 3, 61, 80, 95 };
    int base_wmo = sample_wmos[seed % 7];
    if (day_mean < 1.0 && (base_wmo == 61 || base_wmo == 80))
        base_wmo = 71;

    /* Diurnal cycle: coolest at 05:00, warmest at 14:30 */
    double diurnal_amp = 4.2;
    double hour_angle = ((lt.tm_hour + lt.tm_min / 60.0 - 14.5) / 24.0) * 2.0 * 3.14159265358979323846;
    out->current_temp_c = floor((day_mean + diurnal_amp * cos(hour_angle)) * 10.0 + 0.5) / 10.0;
    out->is_day = (lt.tm_hour >= 6 && lt.tm_hour < 18) ? 1 : 0;
    out->wmo_code = base_wmo;

    out->humidity_pct = 55 + (int)(seed % 30) - (int)(3.0 * cos(hour_angle));
    if (out->humidity_pct < 25) out->humidity_pct = 25;
    if (out->humidity_pct > 95) out->humidity_pct = 95;

    out->wind_kmh = 8.0 + (double)(seed % 16);
    out->wind_dir_deg = (int)((seed * 45u) % 360u);
    out->wind_gust_kmh = out->wind_kmh * 1.45;
    out->pressure_hpa = 1009.0 + (double)((int)(seed % 15) - 6);
    out->uv_index = out->is_day ? fmax(1.0, (9.5 - abs_lat * 0.08) * fmax(0.15, cos(hour_angle))) : 0.0;
    out->visibility_km = (base_wmo >= 61) ? 6.5 : 10.0;
    out->precip_mm = (base_wmo >= 61) ? 2.4 : 0.0;
    out->cloud_cover_pct = (base_wmo == 0) ? 8 : (base_wmo == 1 ? 22 : (base_wmo == 2 ? 48 : 82));
    out->aqi = 24 + (int)(seed % 48);
    out->feels_like_c = hde_weather_feels_like_c(out->current_temp_c, out->humidity_pct, out->wind_kmh);

    /* 24-hour forecast */
    out->hourly_count = 24;
    for (int i = 0; i < 24; i++) {
        int h = (lt.tm_hour + i) % 24;
        out->hourly[i].hour = h;
        if (i == 0)
            snprintf(out->hourly[i].label, sizeof(out->hourly[i].label), "Now");
        else
            snprintf(out->hourly[i].label, sizeof(out->hourly[i].label), "%02d:00", h);
        double ha = ((h - 14.5) / 24.0) * 2.0 * 3.14159265358979323846;
        out->hourly[i].temp_c = floor((day_mean + diurnal_amp * cos(ha)) * 10.0 + 0.5) / 10.0;
        out->hourly[i].is_day = (h >= 6 && h < 18) ? 1 : 0;
        int hwmo = sample_wmos[(seed + (unsigned int)(i / 3)) % 7];
        if (day_mean < 1.0 && (hwmo == 61 || hwmo == 80))
            hwmo = 71;
        out->hourly[i].wmo_code = (i == 0) ? out->wmo_code : hwmo;
        out->hourly[i].pop_pct = (out->hourly[i].wmo_code >= 61) ? (55 + (i * 7) % 35) : (int)((seed + (unsigned int)(i * 5)) % 25u);
        out->hourly[i].wind_kmh = out->wind_kmh + (double)((i % 5) - 2) * 1.2;
    }

    /* 7-day forecast */
    static const char *const wdays[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static const char *const months[12] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    out->daily_count = 7;
    for (int d = 0; d < 7; d++) {
        time_t d_ts = local_ts + (time_t)d * 86400;
        struct tm dt;
        gmtime_r(&d_ts, &dt);
        if (d == 0)
            snprintf(out->daily[d].day_name, sizeof(out->daily[d].day_name), "Today");
        else
            snprintf(out->daily[d].day_name, sizeof(out->daily[d].day_name), "%s", wdays[dt.tm_wday % 7]);
        snprintf(out->daily[d].date_label, sizeof(out->daily[d].date_label), "%s %d",
                 months[dt.tm_mon % 12], dt.tm_mday);

        double d_offset = sin((double)(d + (seed % 5)) * 1.1) * 1.8;
        out->daily[d].temp_min_c = floor((day_mean - diurnal_amp + d_offset) * 10.0 + 0.5) / 10.0;
        out->daily[d].temp_max_c = floor((day_mean + diurnal_amp + d_offset) * 10.0 + 0.5) / 10.0;
        int dwmo = (d == 0) ? out->wmo_code : sample_wmos[(seed + (unsigned int)d * 3u) % 8];
        if (day_mean < 1.0 && (dwmo == 61 || dwmo == 80))
            dwmo = 71;
        out->daily[d].wmo_code = dwmo;
        out->daily[d].pop_pct = (dwmo >= 61) ? (60 + (d * 9) % 35) : (int)((seed + (unsigned int)(d * 11)) % 30u);
        out->daily[d].uv_max = fmax(2.0, 9.5 - abs_lat * 0.09);
        out->daily[d].precip_mm = (dwmo >= 61) ? (2.5 + (double)(d % 4) * 2.1) : 0.0;
        snprintf(out->daily[d].sunrise, sizeof(out->daily[d].sunrise), "06:%02d", 2 + (int)(abs_lat / 4.0) % 25);
        snprintf(out->daily[d].sunset, sizeof(out->daily[d].sunset), "17:%02d", 45 + (int)(seed + d) % 14);
    }
}

/* --- Lightweight JSON extraction helpers for Open-Meteo responses --- */

static const char *find_json_section(const char *json, const char *section_name)
{
    if (!json || !section_name) return NULL;
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", section_name);
    const char *p = strstr(json, pat);
    while (p) {
        const char *after = p + strlen(pat);
        while (*after && isspace((unsigned char)*after)) after++;
        if (*after == ':') {
            after++;
            while (*after && isspace((unsigned char)*after)) after++;
            if (*after == '{' || *after == '[')
                return after;
        }
        p = strstr(p + 1, pat);
    }
    return NULL;
}

static int json_get_double_in_obj(const char *obj, const char *key, double *val_out)
{
    if (!obj || !key) return 0;
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    /* Scan until matching closing brace of obj */
    int depth = 0;
    const char *p = obj;
    while (*p) {
        if (*p == '{') depth++;
        else if (*p == '}') {
            depth--;
            if (depth <= 0) break;
        } else if (depth == 1 && strncmp(p, pat, strlen(pat)) == 0) {
            const char *v = p + strlen(pat);
            while (*v && (isspace((unsigned char)*v) || *v == ':')) v++;
            char *end = NULL;
            double d = strtod(v, &end);
            if (end && end != v) {
                if (val_out) *val_out = d;
                return 1;
            }
        }
        p++;
    }
    return 0;
}

static int json_get_double_array(const char *obj, const char *key, double *out, int max_out)
{
    if (!obj || !key || !out || max_out <= 0) return 0;
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(obj, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p && *p != '[') {
        if (*p == '}') return 0;
        p++;
    }
    if (*p != '[') return 0;
    p++;
    int count = 0;
    while (*p && *p != ']' && count < max_out) {
        while (*p && (isspace((unsigned char)*p) || *p == ',')) p++;
        if (*p == ']' || !*p) break;
        if (strncmp(p, "null", 4) == 0) {
            out[count++] = 0.0;
            p += 4;
            continue;
        }
        char *end = NULL;
        double d = strtod(p, &end);
        if (!end || end == p) break;
        out[count++] = d;
        p = end;
    }
    return count;
}

static int json_get_string_array(const char *obj, const char *key, char out[][32], int max_out)
{
    if (!obj || !key || !out || max_out <= 0) return 0;
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(obj, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p && *p != '[') {
        if (*p == '}') return 0;
        p++;
    }
    if (*p != '[') return 0;
    p++;
    int count = 0;
    while (*p && *p != ']' && count < max_out) {
        while (*p && *p != '"' && *p != ']') p++;
        if (*p != '"') break;
        p++;
        int i = 0;
        while (*p && *p != '"' && i < 31) {
            out[count][i++] = *p++;
        }
        out[count][i] = '\0';
        while (*p && *p != '"') p++;
        if (*p == '"') p++;
        count++;
    }
    return count;
}

int hde_weather_parse_open_meteo_json(const char *json, const HdeWeatherCity *city, HdeWeatherForecast *out)
{
    if (!json || !out) return -1;
    const char *cur = find_json_section(json, "current");
    if (!cur) return -1;

    double temp = 0.0;
    if (!json_get_double_in_obj(cur, "temperature_2m", &temp))
        return -1;

    /* Initialize with fallback first so any optional fields have sensible defaults */
    hde_weather_generate_fallback(city, time(NULL), out);
    out->is_live = 1;
    out->current_temp_c = temp;

    double v = 0.0;
    if (json_get_double_in_obj(cur, "relative_humidity_2m", &v))
        out->humidity_pct = (int)(v + 0.5);
    if (json_get_double_in_obj(cur, "apparent_temperature", &v))
        out->feels_like_c = v;
    else
        out->feels_like_c = hde_weather_feels_like_c(out->current_temp_c, out->humidity_pct, out->wind_kmh);
    if (json_get_double_in_obj(cur, "is_day", &v))
        out->is_day = (v >= 0.5) ? 1 : 0;
    if (json_get_double_in_obj(cur, "precipitation", &v))
        out->precip_mm = v;
    if (json_get_double_in_obj(cur, "weather_code", &v))
        out->wmo_code = (int)(v + 0.5);
    if (json_get_double_in_obj(cur, "cloud_cover", &v))
        out->cloud_cover_pct = (int)(v + 0.5);
    if (json_get_double_in_obj(cur, "surface_pressure", &v))
        out->pressure_hpa = v;
    if (json_get_double_in_obj(cur, "wind_speed_10m", &v))
        out->wind_kmh = v;
    if (json_get_double_in_obj(cur, "wind_direction_10m", &v))
        out->wind_dir_deg = (int)(v + 0.5);
    if (json_get_double_in_obj(cur, "wind_gusts_10m", &v))
        out->wind_gust_kmh = v;

    /* Parse hourly section */
    const char *hourly = find_json_section(json, "hourly");
    if (hourly) {
        double h_temp[24], h_pop[24], h_wmo[24], h_wind[24], h_day[24];
        char h_time[24][32];
        int n_temp = json_get_double_array(hourly, "temperature_2m", h_temp, 24);
        int n_pop  = json_get_double_array(hourly, "precipitation_probability", h_pop, 24);
        int n_wmo  = json_get_double_array(hourly, "weather_code", h_wmo, 24);
        int n_wind = json_get_double_array(hourly, "wind_speed_10m", h_wind, 24);
        int n_day  = json_get_double_array(hourly, "is_day", h_day, 24);
        int n_time = json_get_string_array(hourly, "time", h_time, 24);
        if (n_temp > 0) {
            out->hourly_count = n_temp;
            for (int i = 0; i < n_temp; i++) {
                out->hourly[i].temp_c = h_temp[i];
                if (i < n_pop)  out->hourly[i].pop_pct = (int)(h_pop[i] + 0.5);
                if (i < n_wmo)  out->hourly[i].wmo_code = (int)(h_wmo[i] + 0.5);
                if (i < n_wind) out->hourly[i].wind_kmh = h_wind[i];
                if (i < n_day)  out->hourly[i].is_day = (h_day[i] >= 0.5) ? 1 : 0;
                if (i < n_time) {
                    const char *tpart = strchr(h_time[i], 'T');
                    if (tpart && strlen(tpart + 1) >= 5) {
                        snprintf(out->hourly[i].label, sizeof(out->hourly[i].label), "%.5s", tpart + 1);
                        out->hourly[i].hour = atoi(tpart + 1);
                    }
                }
            }
        }
    }

    /* Parse daily section */
    const char *daily = find_json_section(json, "daily");
    if (daily) {
        double d_max[7], d_min[7], d_wmo[7], d_uv[7], d_prec[7], d_pop[7];
        char d_rise[7][32], d_set[7][32];
        int n_max  = json_get_double_array(daily, "temperature_2m_max", d_max, 7);
        int n_min  = json_get_double_array(daily, "temperature_2m_min", d_min, 7);
        int n_wmo  = json_get_double_array(daily, "weather_code", d_wmo, 7);
        int n_uv   = json_get_double_array(daily, "uv_index_max", d_uv, 7);
        int n_prec = json_get_double_array(daily, "precipitation_sum", d_prec, 7);
        int n_pop  = json_get_double_array(daily, "precipitation_probability_max", d_pop, 7);
        int n_rise = json_get_string_array(daily, "sunrise", d_rise, 7);
        int n_set  = json_get_string_array(daily, "sunset", d_set, 7);
        if (n_max > 0 && n_min > 0) {
            out->daily_count = n_max < n_min ? n_max : n_min;
            for (int i = 0; i < out->daily_count; i++) {
                out->daily[i].temp_max_c = d_max[i];
                out->daily[i].temp_min_c = d_min[i];
                if (i < n_wmo)  out->daily[i].wmo_code = (int)(d_wmo[i] + 0.5);
                if (i < n_uv)   out->daily[i].uv_max = d_uv[i];
                if (i < n_prec) out->daily[i].precip_mm = d_prec[i];
                if (i < n_pop)  out->daily[i].pop_pct = (int)(d_pop[i] + 0.5);
                if (i < n_rise) {
                    const char *t = strchr(d_rise[i], 'T');
                    if (t && strlen(t + 1) >= 5)
                        snprintf(out->daily[i].sunrise, sizeof(out->daily[i].sunrise), "%.5s", t + 1);
                }
                if (i < n_set) {
                    const char *t = strchr(d_set[i], 'T');
                    if (t && strlen(t + 1) >= 5)
                        snprintf(out->daily[i].sunset, sizeof(out->daily[i].sunset), "%.5s", t + 1);
                }
            }
            if (n_uv > 0 && out->is_day)
                out->uv_index = d_uv[0];
        }
    }
    return 0;
}

static int json_get_string_in_obj(const char *obj, const char *key, char *buf, size_t bufsz)
{
    if (!obj || !key || !buf || bufsz == 0) return 0;
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    int depth = 0;
    const char *p = obj;
    while (*p) {
        if (*p == '{') depth++;
        else if (*p == '}') {
            depth--;
            if (depth <= 0) break;
        } else if (depth == 1 && strncmp(p, pat, strlen(pat)) == 0) {
            const char *v = p + strlen(pat);
            while (*v && (isspace((unsigned char)*v) || *v == ':')) v++;
            if (*v == '"') {
                v++;
                size_t i = 0;
                while (*v && *v != '"' && i + 1 < bufsz)
                    buf[i++] = *v++;
                buf[i] = '\0';
                return 1;
            }
        }
        p++;
    }
    return 0;
}

int hde_weather_parse_geocoding_json(const char *json, HdeWeatherCity *out, int max_out)
{
    if (!json || !out || max_out <= 0) return 0;
    const char *arr = find_json_section(json, "results");
    if (!arr || *arr != '[') return 0;
    const char *p = arr + 1;
    int count = 0;
    while (*p && count < max_out) {
        while (*p && *p != '{' && *p != ']') p++;
        if (*p != '{') break;
        HdeWeatherCity c;
        memset(&c, 0, sizeof(c));
        c.utc_offset_hours = 0.0;
        if (json_get_string_in_obj(p, "name", c.name, sizeof(c.name)) &&
            json_get_double_in_obj(p, "latitude", &c.lat) &&
            json_get_double_in_obj(p, "longitude", &c.lon)) {
            if (!json_get_string_in_obj(p, "country", c.country, sizeof(c.country)))
                snprintf(c.country, sizeof(c.country), "World");
            c.utc_offset_hours = floor(c.lon / 15.0 + 0.5);
            out[count++] = c;
        }
        /* Advance past matching '}' of this result object */
        int depth = 0;
        while (*p) {
            if (*p == '{') depth++;
            else if (*p == '}') {
                depth--;
                if (depth <= 0) { p++; break; }
            }
            p++;
        }
    }
    return count;
}
