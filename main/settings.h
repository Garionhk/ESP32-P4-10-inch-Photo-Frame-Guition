// ===========================================================================
// settings.h -- runtime user configuration
// ===========================================================================
// Carried over from the CYD project unchanged in spirit: nothing user-specific
// is compiled in. These live in NVS and are set from the web page, because
// re-flashing to change a setting is a poor repair path for something hanging
// on a wall -- and because a binary built from a header full of secrets
// carries those secrets.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define SETTINGS_STR_MAX    64
#define SETTINGS_URL_MAX    128
#define SETTINGS_KEY_MAX    96
#define SETTINGS_ALBUMS_MAX 256
// A PEM fits comfortably; anything larger is a chain, and only the root is
// needed to trust a self-signed Immich.
#define SETTINGS_CA_MAX     2048

typedef struct {
    char    wifi_ssid[SETTINGS_STR_MAX];
    char    wifi_pass[SETTINGS_STR_MAX];
    // Greenwich Observatory: a placeholder that is obviously not the user's,
    // so an unconfigured device shows visibly wrong data rather than plausible
    // data for someone else's city.
    float   latitude;
    float   longitude;
    char    tz[SETTINGS_STR_MAX];      // POSIX TZ string, not an IANA name
    bool    imperial;
    bool    landscape;
    // Mounted the other way up. Affects the photo fit pass, which end of the
    // panel the info strip sits at, and the sense of a swipe.
    bool    rotate180;

    // Overlay: one bit per item, in OV_* order (clock, weather, sun, aqi).
    // A mask rather than four bools so the packing rule has something to
    // count, and so adding a fifth item later does not change the NVS layout.
    uint8_t  ov_items;
    uint8_t  theme;           // index into THEMES[]
    uint8_t  ov_opacity;      // 0-255, scales the bar's authored alpha
    uint16_t ov_page_ms;      // (unused: pages now follow the photo)
    uint16_t dwell_s;         // seconds a photo stays on screen

    // Web UI password, stored SALTED AND HASHED -- never in the clear. The
    // page can upload photos and will hold an Immich key, so a readable
    // password in NVS would be a poor trade for a few lines of code.
    uint8_t  web_salt[8];
    uint8_t  web_hash[32];    // SHA-256(salt || password)
    bool     web_pw_set;

    // ---- Immich ----
    char    immich_url[SETTINGS_URL_MAX];       // http(s)://host:2283
    char    immich_key[SETTINGS_KEY_MAX];       // x-api-key
    char    immich_albums[SETTINGS_ALBUMS_MAX]; // comma-separated album ids
    char    immich_ca[SETTINGS_CA_MAX];         // optional PEM for a private CA
    uint8_t immich_cache_mb;                    // LRU cap on the SD cache
    uint8_t source_mode;                        // source_mode_t
    uint8_t sd_weight;                          // 0-100, share of SD in BOTH mode

    bool    provisioned;
} settings_t;

// Returns true when `password` matches the stored hash. Always true when no
// password has been set, so a fresh device is not locked out of itself.
bool settings_check_password(const char *password);

// Hash and store a new password. Passing an empty string clears it.
void settings_set_password(const char *password);

extern settings_t g_settings;

void settings_begin(void);
bool settings_save(void);
