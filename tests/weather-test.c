/* weather-test.c — Unit tests for Hyggshi Weather pure-C core (hde-weather-core.c). */
#include "hde-weather-core.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int passed = 0;
static int failed = 0;

static void check(int cond, const char *msg)
{
    if (cond) {
        printf("PASS: weather: %s\n", msg);
        passed++;
    } else {
        printf("FAIL: weather: %s\n", msg);
        failed++;
    }
}

int main(void)
{
    /* 1. WMO weather code descriptions, kinds, and icons */
    check(strcmp(hde_weather_wmo_description(0), "Clear Sky") == 0, "WMO 0 is 'Clear Sky'");
    check(strcmp(hde_weather_wmo_description(2), "Partly Cloudy") == 0, "WMO 2 is 'Partly Cloudy'");
    check(strcmp(hde_weather_wmo_description(65), "Heavy Rain") == 0, "WMO 65 is 'Heavy Rain'");
    check(strcmp(hde_weather_wmo_description(95), "Thunderstorm") == 0, "WMO 95 is 'Thunderstorm'");
    check(strcmp(hde_weather_wmo_icon(0, 1), "weather-clear") == 0, "WMO 0 daytime icon is weather-clear");
    check(strcmp(hde_weather_wmo_icon(0, 0), "weather-clear-night") == 0, "WMO 0 nighttime icon is weather-clear-night");
    check(hde_weather_wmo_kind(75) == HDE_WEATHER_KIND_SNOW, "WMO 75 is snow kind");
    check(hde_weather_wmo_kind(96) == HDE_WEATHER_KIND_STORM, "WMO 96 is storm kind");

    /* 2. Unit conversions */
    check(fabs(hde_weather_c_to_f(0.0) - 32.0) < 1e-6, "0 C == 32 F");
    check(fabs(hde_weather_c_to_f(100.0) - 212.0) < 1e-6, "100 C == 212 F");
    check(fabs(hde_weather_f_to_c(68.0) - 20.0) < 1e-6, "68 F == 20 C");
    check(fabs(hde_weather_kmh_to_ms(36.0) - 10.0) < 1e-6, "36 km/h == 10 m/s");
    check(fabs(hde_weather_kmh_to_mph(100.0) - 62.1371) < 1e-2, "100 km/h == 62.14 mph");
    check(fabs(hde_weather_hpa_to_inhg(1013.25) - 29.92) < 1e-2, "1013.25 hPa == 29.92 inHg");

    /* 3. Meteorological formulas & categories */
    double dp = hde_weather_dew_point_c(30.0, 70);
    check(dp > 23.5 && dp < 24.5, "dew point at 30 C & 70% RH is ~24 C");
    double fl_hot = hde_weather_feels_like_c(32.0, 75, 10.0);
    check(fl_hot > 38.0, "heat index at 32 C & 75% RH feels hotter than 38 C");
    double fl_cold = hde_weather_feels_like_c(0.0, 50, 30.0);
    check(fl_cold < -4.0, "wind chill at 0 C & 30 km/h feels colder than -4 C");
    check(strcmp(hde_weather_wind_compass(0), "N") == 0, "0 deg is N");
    check(strcmp(hde_weather_wind_compass(90), "E") == 0, "90 deg is E");
    check(strcmp(hde_weather_wind_compass(225), "SW") == 0, "225 deg is SW");
    check(strcmp(hde_weather_uv_category(1.5), "Low") == 0, "UV 1.5 is Low");
    check(strcmp(hde_weather_uv_category(7.0), "High") == 0, "UV 7.0 is High");
    check(strcmp(hde_weather_uv_category(11.5), "Extreme") == 0, "UV 11.5 is Extreme");
    check(strcmp(hde_weather_aqi_category(35), "Good") == 0, "AQI 35 is Good");

    /* 4. Built-in cities search */
    int total = 0;
    const HdeWeatherCity *all = hde_weather_builtin_cities(&total);
    check(all != NULL && total >= 25, "built-in world cities catalog has >= 25 cities");
    HdeWeatherCity matches[8];
    int n_vn = hde_weather_search_builtin_cities("Vietnam", matches, 8);
    check(n_vn >= 3, "searching 'Vietnam' finds Ho Chi Minh City, Hanoi, Da Nang");

    /* 5. Offline deterministic forecast generator */
    HdeWeatherForecast fc;
    hde_weather_generate_fallback(&all[0], 1728885600, &fc);
    check(fc.hourly_count == 24, "fallback forecast generates 24 hourly entries");
    check(fc.daily_count == 7, "fallback forecast generates 7 daily entries");
    check(fc.current_temp_c > 15.0 && fc.current_temp_c < 42.0, "Ho Chi Minh City fallback temp is realistic");

    /* 6. Open-Meteo JSON parser */
    const char *sample_json =
        "{"
        "\"current\":{\"temperature_2m\":29.4,\"relative_humidity_2m\":78,\"apparent_temperature\":34.1,"
        "\"is_day\":1,\"precipitation\":1.2,\"weather_code\":61,\"cloud_cover\":65,"
        "\"surface_pressure\":1008.5,\"wind_speed_10m\":14.2,\"wind_direction_10m\":135,\"wind_gusts_10m\":22.0},"
        "\"hourly\":{\"time\":[\"2026-10-14T12:00\",\"2026-10-14T13:00\"],"
        "\"temperature_2m\":[29.4,30.1],\"precipitation_probability\":[40,60],"
        "\"weather_code\":[61,80],\"wind_speed_10m\":[14.2,15.5],\"is_day\":[1,1]},"
        "\"daily\":{\"time\":[\"2026-10-14\",\"2026-10-15\"],"
        "\"weather_code\":[61,2],\"temperature_2m_max\":[32.5,33.0],\"temperature_2m_min\":[24.8,25.1],"
        "\"sunrise\":[\"2026-10-14T05:48\",\"2026-10-15T05:48\"],"
        "\"sunset\":[\"2026-10-14T17:42\",\"2026-10-15T17:41\"],"
        "\"uv_index_max\":[8.2,9.0],\"precipitation_sum\":[4.5,0.0],\"precipitation_probability_max\":[75,20]}"
        "}";
    HdeWeatherForecast parsed;
    int rc = hde_weather_parse_open_meteo_json(sample_json, &all[0], &parsed);
    check(rc == 0 && parsed.is_live == 1, "Open-Meteo JSON parses cleanly with is_live=1");
    check(fabs(parsed.current_temp_c - 29.4) < 1e-4, "parsed current_temp_c == 29.4");
    check(parsed.humidity_pct == 78, "parsed humidity_pct == 78");
    check(parsed.wmo_code == 61, "parsed wmo_code == 61");
    check(parsed.hourly_count == 2 && strcmp(parsed.hourly[1].label, "13:00") == 0, "parsed hourly time label is 13:00");
    check(parsed.daily_count == 2 && fabs(parsed.daily[0].temp_max_c - 32.5) < 1e-4, "parsed daily max temp == 32.5");
    check(strcmp(parsed.daily[0].sunrise, "05:48") == 0, "parsed sunrise == 05:48");

    /* 7. Geocoding JSON parser */
    const char *geo_json =
        "{\"results\":["
        "{\"name\":\"Da Lat\",\"latitude\":11.9404,\"longitude\":108.4583,\"country\":\"Vietnam\"}"
        "]}";
    HdeWeatherCity geo_out[4];
    int n_geo = hde_weather_parse_geocoding_json(geo_json, geo_out, 4);
    check(n_geo == 1 && strcmp(geo_out[0].name, "Da Lat") == 0, "Open-Meteo geocoding JSON parses Da Lat");

    printf("\nweather-test: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
