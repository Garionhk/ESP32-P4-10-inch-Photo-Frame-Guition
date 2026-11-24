#include "weather.h"
#include "fetch.h"
#include "settings.h"
#include "app_data.h"
#include "wifi.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"

static const char *TAG = "weather";

static const int64_t INTERVAL_US = 15LL * 60 * 1000000;   // 15 min
static int64_t s_next;
static int     s_backoff_min;

static double num(cJSON *o, const char *k, double dflt)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? v->valuedouble : dflt;
}

static bool do_fetch(void)
{
    char url[512];
    // timezone=auto is what makes the `daily` bucket the LOCAL calendar day
    // rather than a UTC one -- without it today's high/low straddle midnight.
    snprintf(url, sizeof(url),
        "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
        "&current=temperature_2m,apparent_temperature,weather_code,cloud_cover,"
        "relative_humidity_2m,wind_speed_10m,surface_pressure,uv_index"
        "&daily=temperature_2m_max,temperature_2m_min&forecast_days=1&timezone=auto",
        g_settings.latitude, g_settings.longitude);

    char *body = fetch_json(url, 4096);
    if (!body) return false;

    cJSON *doc = cJSON_Parse(body);
    free(body);
    if (!doc) { ESP_LOGW(TAG, "unparseable response"); return false; }

    cJSON *cur = cJSON_GetObjectItemCaseSensitive(doc, "current");
    if (!cJSON_IsObject(cur)) { cJSON_Delete(doc); return false; }

    // Each field falls back to its existing value, so a response missing one
    // key does not blank the display.
    g_data.tempC         = num(cur, "temperature_2m",       g_data.tempC);
    g_data.feelsLikeC    = num(cur, "apparent_temperature", g_data.feelsLikeC);
    g_data.weatherCode   = (int)num(cur, "weather_code",    g_data.weatherCode);
    g_data.cloudCoverPct = (int)num(cur, "cloud_cover",     g_data.cloudCoverPct);
    g_data.humidityPct   = (int)num(cur, "relative_humidity_2m", g_data.humidityPct);
    g_data.windKph       = num(cur, "wind_speed_10m",       g_data.windKph);

    double p = num(cur, "surface_pressure", g_data.pressureHpa);
    if (g_data.pressureHpa > 0) g_data.pressureTrend = p - g_data.pressureHpa;
    g_data.pressureHpa = p;

    g_data.uvIndex = num(cur, "uv_index", g_data.uvIndex);
    g_data.uvUpdatedAt = (uint32_t)time(NULL);
    g_data.uvValid = true;

    // Today's high/low ride along on the same request, so they cost no extra
    // fetch. Tracked by their own flag: a response without the daily block
    // leaves the row absent rather than showing a stray 0 degrees.
    cJSON *daily = cJSON_GetObjectItemCaseSensitive(doc, "daily");
    if (cJSON_IsObject(daily)) {
        cJSON *hi = cJSON_GetObjectItemCaseSensitive(daily, "temperature_2m_max");
        cJSON *lo = cJSON_GetObjectItemCaseSensitive(daily, "temperature_2m_min");
        if (cJSON_IsArray(hi) && cJSON_IsArray(lo) &&
            cJSON_GetArraySize(hi) > 0 && cJSON_GetArraySize(lo) > 0) {
            g_data.tempMaxC = cJSON_GetArrayItem(hi, 0)->valuedouble;
            g_data.tempMinC = cJSON_GetArrayItem(lo, 0)->valuedouble;
            g_data.dailyValid = true;
        }
    }

    g_data.weatherUpdatedAt = (uint32_t)time(NULL);
    g_data.weatherValid = true;
    cJSON_Delete(doc);

    ESP_LOGI(TAG, "%.1fC feels %.1f  hi/lo %.1f/%.1f  code %d  hum %d%%  wind %.1f",
             g_data.tempC, g_data.feelsLikeC, g_data.tempMaxC, g_data.tempMinC,
             g_data.weatherCode, g_data.humidityPct, g_data.windKph);
    return true;
}

void weather_begin(void) { s_next = 0; s_backoff_min = 0; }

void weather_tick(void)
{
    int64_t now = esp_timer_get_time();
    if (now < s_next) return;
    if (!wifi_ready()) { s_next = now + 30LL * 1000000; return; }

    if (do_fetch()) {
        s_backoff_min = 0;
        s_next = now + INTERVAL_US;
    } else {
        // 1 -> 2 -> 4 -> 8 -> 15 min, capped. A server having a bad afternoon
        // should not be hammered, and last-good data is still on screen.
        s_backoff_min = s_backoff_min ? s_backoff_min * 2 : 1;
        if (s_backoff_min > 15) s_backoff_min = 15;
        s_next = now + (int64_t)s_backoff_min * 60 * 1000000;
        ESP_LOGW(TAG, "fetch failed, retry in %d min", s_backoff_min);
    }
}
