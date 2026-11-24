#include "settings.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "mbedtls/sha256.h"
#include "esp_random.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "settings";
static const char *NS  = "framecfg";

settings_t g_settings = {
    .latitude   = 51.4779f,
    .longitude  = -0.0015f,
    .tz         = "UTC0",
    .imperial   = false,
    .landscape  = false,
    // All four overlay items on by default: that exercises the two-page
    // rotation, and an empty strip on a fresh device looks broken.
    .ov_items   = 0x0F,
    .ov_opacity = 255,
    .theme      = 0,          // Ember orange, the design's default
    .ov_page_ms = 10000,
    .dwell_s    = 20,
    .immich_cache_mb = 64,
    .source_mode = 0,          // SD only until Immich is configured
    .sd_weight   = 50,
    .provisioned = false,
};

bool settings_check_password(const char *password)
{
    if (!g_settings.web_pw_set) return true;      // nothing set: never lock out
    if (!password) return false;

    uint8_t want[32];
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, g_settings.web_salt, sizeof(g_settings.web_salt));
    mbedtls_sha256_update(&ctx, (const uint8_t *)password, strlen(password));
    mbedtls_sha256_finish(&ctx, want);
    mbedtls_sha256_free(&ctx);

    // Constant-time compare: a timing-varying memcmp on a hash is a small
    // thing to get right and an irritating one to explain later.
    uint8_t diff = 0;
    for (int i = 0; i < 32; i++) diff |= want[i] ^ g_settings.web_hash[i];
    return diff == 0;
}

void settings_set_password(const char *password)
{
    if (!password || !password[0]) {
        g_settings.web_pw_set = false;
        memset(g_settings.web_hash, 0, sizeof(g_settings.web_hash));
        return;
    }
    esp_fill_random(g_settings.web_salt, sizeof(g_settings.web_salt));

    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, g_settings.web_salt, sizeof(g_settings.web_salt));
    mbedtls_sha256_update(&ctx, (const uint8_t *)password, strlen(password));
    mbedtls_sha256_finish(&ctx, g_settings.web_hash);
    mbedtls_sha256_free(&ctx);

    g_settings.web_pw_set = true;
    ESP_LOGI(TAG, "web password set");
}

void settings_begin(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "UNPROVISIONED (defaults; location is Greenwich on purpose)");
        return;
    }

    uint8_t ok = 0;
    if (nvs_get_u8(h, "ok", &ok) == ESP_OK && ok) {
        size_t n = SETTINGS_STR_MAX;
        nvs_get_str(h, "ssid", g_settings.wifi_ssid, &n);  n = SETTINGS_STR_MAX;
        nvs_get_str(h, "pass", g_settings.wifi_pass, &n);  n = SETTINGS_STR_MAX;
        nvs_get_str(h, "tz",   g_settings.tz,        &n);
        size_t sz = sizeof(float);
        nvs_get_blob(h, "lat", &g_settings.latitude,  &sz); sz = sizeof(float);
        nvs_get_blob(h, "lon", &g_settings.longitude, &sz);
        uint8_t b = 0;
        if (nvs_get_u8(h, "imp", &b) == ESP_OK) g_settings.imperial  = b;
        if (nvs_get_u8(h, "land", &b) == ESP_OK) g_settings.landscape = b;
        if (nvs_get_u8(h, "rot180", &b) == ESP_OK) g_settings.rotate180 = b;
        if (nvs_get_u8(h, "ovit", &b) == ESP_OK) g_settings.ov_items   = b;
        if (nvs_get_u8(h, "ovop", &b) == ESP_OK) g_settings.ov_opacity = b;
        if (nvs_get_u8(h, "thm",  &b) == ESP_OK) g_settings.theme      = b;
        n = SETTINGS_URL_MAX;    nvs_get_str(h, "imurl", g_settings.immich_url,    &n);
        n = SETTINGS_KEY_MAX;    nvs_get_str(h, "imkey", g_settings.immich_key,    &n);
        n = SETTINGS_ALBUMS_MAX; nvs_get_str(h, "imalb", g_settings.immich_albums, &n);
        n = SETTINGS_CA_MAX;     nvs_get_str(h, "imca",  g_settings.immich_ca,     &n);
        if (nvs_get_u8(h, "imcmb", &b) == ESP_OK) g_settings.immich_cache_mb = b;
        if (nvs_get_u8(h, "srcm",  &b) == ESP_OK) g_settings.source_mode     = b;
        if (nvs_get_u8(h, "sdw",   &b) == ESP_OK) g_settings.sd_weight       = b;
        uint16_t w16;
        if (nvs_get_u16(h, "ovms", &w16) == ESP_OK) g_settings.ov_page_ms = w16;
        if (nvs_get_u16(h, "dwell", &w16) == ESP_OK) g_settings.dwell_s = w16;

        size_t sl = sizeof(g_settings.web_salt), hl = sizeof(g_settings.web_hash);
        if (nvs_get_blob(h, "wsalt", g_settings.web_salt, &sl) == ESP_OK &&
            nvs_get_blob(h, "whash", g_settings.web_hash, &hl) == ESP_OK) {
            uint8_t pset = 0;
            nvs_get_u8(h, "wpset", &pset);
            g_settings.web_pw_set = pset != 0;
        }
        g_settings.provisioned = true;
    }
    nvs_close(h);

    // The SSID is printed because a router broadcasts it anyway and "connected
    // to the wrong network" is otherwise painful to diagnose. The password
    // never is: serial logs end up pasted into issue reports.
    ESP_LOGI(TAG, "%s  ssid \"%s\"  %.4f,%.4f  tz %s  %s",
             g_settings.provisioned ? "loaded" : "UNPROVISIONED (defaults)",
             g_settings.wifi_ssid, g_settings.latitude, g_settings.longitude,
             g_settings.tz, g_settings.imperial ? "imperial" : "metric");
    ESP_LOGI(TAG, "orientation: %s%s",
             g_settings.landscape ? "landscape" : "portrait",
             g_settings.rotate180 ? " + 180" : "");
    ESP_LOGI(TAG, "overlay mask 0x%X, page %u ms, web password %s",
             g_settings.ov_items, g_settings.ov_page_ms,
             g_settings.web_pw_set ? "SET" : "not set");
    // The API key is never printed: serial logs end up pasted into issue
    // reports, exactly as with the WiFi password.
    ESP_LOGI(TAG, "source mode %u, Immich %s%s",
             g_settings.source_mode,
             g_settings.immich_url[0] ? g_settings.immich_url : "not configured",
             g_settings.immich_key[0] ? " (key set)" : "");

    setenv("TZ", g_settings.tz, 1);
    tzset();
}

bool settings_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return false;
    nvs_set_str(h, "ssid", g_settings.wifi_ssid);
    nvs_set_str(h, "pass", g_settings.wifi_pass);
    nvs_set_str(h, "tz",   g_settings.tz);
    nvs_set_blob(h, "lat", &g_settings.latitude,  sizeof(float));
    nvs_set_blob(h, "lon", &g_settings.longitude, sizeof(float));
    nvs_set_u8(h, "imp",  g_settings.imperial);
    nvs_set_u8(h, "land", g_settings.landscape);
    nvs_set_u8(h, "rot180", g_settings.rotate180);
    nvs_set_u8(h, "ovit", g_settings.ov_items);
    nvs_set_u8(h, "ovop", g_settings.ov_opacity);
    nvs_set_u8(h, "thm",  g_settings.theme);
    nvs_set_str(h, "imurl", g_settings.immich_url);
    nvs_set_str(h, "imkey", g_settings.immich_key);
    nvs_set_str(h, "imalb", g_settings.immich_albums);
    nvs_set_str(h, "imca",  g_settings.immich_ca);
    nvs_set_u8(h, "imcmb", g_settings.immich_cache_mb);
    nvs_set_u8(h, "srcm",  g_settings.source_mode);
    nvs_set_u8(h, "sdw",   g_settings.sd_weight);
    nvs_set_u16(h, "ovms", g_settings.ov_page_ms);
    nvs_set_u16(h, "dwell", g_settings.dwell_s);
    nvs_set_blob(h, "wsalt", g_settings.web_salt, sizeof(g_settings.web_salt));
    nvs_set_blob(h, "whash", g_settings.web_hash, sizeof(g_settings.web_hash));
    nvs_set_u8(h, "wpset", g_settings.web_pw_set);
    nvs_set_u8(h, "ok", 1);      // written last: a half-written record must
    esp_err_t err = nvs_commit(h);   // never read back as valid
    nvs_close(h);
    g_settings.provisioned = true;
    return err == ESP_OK;
}
