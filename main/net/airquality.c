#include "airquality.h"
#include "fetch.h"
#include "settings.h"
#include "app_data.h"
#include "wifi.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"

static const char *TAG = "aqi";

static const int64_t INTERVAL_US = 30LL * 60 * 1000000;   // 30 min
static int64_t s_next;
static int     s_backoff_min;

static bool do_fetch(void)
{
    char url[384];
    // Note the distinct host: air quality is not on the forecast endpoint.
    snprintf(url, sizeof(url),
        "https://air-quality-api.open-meteo.com/v1/air-quality"
        "?latitude=%.4f&longitude=%.4f&current=us_aqi,pm2_5&timezone=auto",
        g_settings.latitude, g_settings.longitude);

    char *body = fetch_json(url, 2048);
    if (!body) return false;

    cJSON *doc = cJSON_Parse(body);
    free(body);
    if (!doc) return false;

    cJSON *cur = cJSON_GetObjectItemCaseSensitive(doc, "current");
    cJSON *aqi = cur ? cJSON_GetObjectItemCaseSensitive(cur, "us_aqi") : NULL;
    if (!cJSON_IsNumber(aqi)) { cJSON_Delete(doc); return false; }

    g_data.aqi = (int)aqi->valuedouble;
    cJSON *pm = cJSON_GetObjectItemCaseSensitive(cur, "pm2_5");
    if (cJSON_IsNumber(pm)) g_data.pm25 = (int)lround(pm->valuedouble);

    g_data.aqiUpdatedAt = (uint32_t)time(NULL);
    g_data.aqiValid = true;
    cJSON_Delete(doc);

    ESP_LOGI(TAG, "US AQI %d  PM2.5 %d", g_data.aqi, g_data.pm25);
    return true;
}

void airquality_begin(void)
{
    // Offset past the weather fetch so the two never overlap at startup.
    s_next = 20LL * 1000000;
    s_backoff_min = 0;
}

void airquality_tick(void)
{
    int64_t now = esp_timer_get_time();
    if (now < s_next) return;
    if (!wifi_ready()) { s_next = now + 30LL * 1000000; return; }

    if (do_fetch()) {
        s_backoff_min = 0;
        s_next = now + INTERVAL_US;
    } else {
        s_backoff_min = s_backoff_min ? s_backoff_min * 2 : 1;
        if (s_backoff_min > 15) s_backoff_min = 15;
        s_next = now + (int64_t)s_backoff_min * 60 * 1000000;
        ESP_LOGW(TAG, "fetch failed, retry in %d min", s_backoff_min);
    }
}
