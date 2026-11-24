#include "immich.h"
#include "fetch.h"
#include "settings.h"
#include "wifi.h"
#include "board.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_random.h"
#include "cJSON.h"

static const char *TAG = "immich";

#define CACHE_DIR   BSP_SD_MOUNT "/immich"

bool immich_configured(void)
{
    return g_settings.immich_url[0] && g_settings.immich_key[0];
}

// Pick the TLS mode from the URL itself, so the user never has to declare it.
// A pasted CA implies a private certificate; otherwise https means the public
// bundle and http means none.
static fetch_tls_t tls_for_url(void)
{
    if (strncmp(g_settings.immich_url, "http://", 7) == 0) return FETCH_TLS_NONE;
    return g_settings.immich_ca[0] ? FETCH_TLS_PEM : FETCH_TLS_BUNDLE;
}

// Join base + path, tolerating a trailing slash or a trailing /api on the base.
// Both are things people paste, and neither should be an error.
static void build_url(char *out, size_t n, const char *path)
{
    char base[SETTINGS_URL_MAX];
    strncpy(base, g_settings.immich_url, sizeof(base) - 1);
    base[sizeof(base) - 1] = '\0';

    size_t bl = strlen(base);
    while (bl && base[bl - 1] == '/') base[--bl] = '\0';

    // Strip a trailing web-UI route if one was pasted. Copying the address
    // out of the browser gives something like
    // "https://immich.example.com/photos", and appending "/api" to that
    // yields a path the server answers with HTTP 200 and the SPA's HTML --
    // so it looks like it worked, parses as nothing, and shows no albums.
    //
    // Only a KNOWN final segment is removed. Immich is often reverse-proxied
    // under a subpath ("https://host/immich"), and stripping every path
    // would break those installs.
    static const char *ui_routes[] = {
        "/photos", "/albums", "/explore", "/map", "/sharing", "/library",
        "/archive", "/favorites", "/trash", "/people", "/places",
        "/utilities", "/admin", "/search", "/partners", "/api",
    };
    for (size_t i = 0; i < sizeof(ui_routes) / sizeof(ui_routes[0]); i++) {
        const size_t rl = strlen(ui_routes[i]);
        if (bl > rl && strcasecmp(base + bl - rl, ui_routes[i]) == 0) {
            base[bl - rl] = '\0';
            bl -= rl;
            break;
        }
    }

    snprintf(out, n, "%s/api%s", base, path);
}

char *immich_albums_json(void)
{
    if (!immich_configured() || !wifi_ready()) return NULL;

    char url[SETTINGS_URL_MAX + 32];
    build_url(url, sizeof(url), "/albums");

    fetch_req_t r = {
        .url = url, .method = "GET", .api_key = g_settings.immich_key,
        .tls = tls_for_url(), .ca_pem = g_settings.immich_ca,
        // A real library has a lot of albums: 89 of them come to 63 KB, and
        // the 16 KB this used to allow truncated the JSON mid-object. The
        // parse then failed and the picker showed nothing, with no hint that
        // the response had simply been cut off. Buffers in PSRAM, so the
        // headroom costs nothing that matters.
        .limit = 192 * 1024,
    };
    int alb_len = 0;
    char *body = fetch_request(&r, &alb_len);
    if (!body) return NULL;
    ESP_LOGI(TAG, "album list: %d bytes", alb_len);

    // An HTML body here means the URL is pointing at the web app rather than
    // the API. That returns 200, so nothing upstream flags it -- this is the
    // only place it can be caught and named.
    if (body[0] == '<') {
        ESP_LOGE(TAG, "server returned a web page, not JSON.");
        ESP_LOGE(TAG, "  The URL should be the server root, e.g. https://host --");
        ESP_LOGE(TAG, "  not the address of a page like /photos.");
        free(body);
        return NULL;
    }

    cJSON *doc = cJSON_Parse(body);
    free(body);
    if (!cJSON_IsArray(doc)) { cJSON_Delete(doc); return NULL; }

    // Re-emit only what the picker needs. The raw album list carries thumbnail
    // ids, owner records and share links, and the settings page has no use for
    // any of it -- forwarding it whole would be several KB per album.
    cJSON *out = cJSON_CreateArray();
    cJSON *a = NULL;
    cJSON_ArrayForEach(a, doc) {
        cJSON *id   = cJSON_GetObjectItemCaseSensitive(a, "id");
        cJSON *name = cJSON_GetObjectItemCaseSensitive(a, "albumName");
        cJSON *cnt  = cJSON_GetObjectItemCaseSensitive(a, "assetCount");
        if (!cJSON_IsString(id) || !cJSON_IsString(name)) continue;
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "id", id->valuestring);
        cJSON_AddStringToObject(e, "name", name->valuestring);
        cJSON_AddNumberToObject(e, "count", cJSON_IsNumber(cnt) ? cnt->valuedouble : 0);
        cJSON_AddItemToArray(out, e);
    }
    cJSON_Delete(doc);

    const int n = cJSON_GetArraySize(out);
    char *json = cJSON_PrintUnformatted(out);
    cJSON_Delete(out);
    ESP_LOGI(TAG, "%d album(s) offered to the picker", n);
    return json;
}

// Ask the server for a batch of random assets from the chosen albums.
// Returns a malloc'd asset id, or NULL.
static char *random_asset_id(void)
{
    char url[SETTINGS_URL_MAX + 32];
    build_url(url, sizeof(url), "/search/random");

    // Ask for several and pick one locally. One request per photo would be
    // fine, but a batch costs the same round trip and gives the frame
    // something to fall back on if an asset turns out to be undecodable.
    // The ids expand to a JSON array with quotes and commas, so the body has
    // to allow for roughly twice the stored list plus the wrapper.
    char body[SETTINGS_ALBUMS_MAX * 2 + 160];
    if (g_settings.immich_albums[0]) {
        // The stored value is a comma-separated list of ids; turn it into a
        // JSON array without needing a builder.
        char ids[SETTINGS_ALBUMS_MAX * 2];
        size_t o = 0;
        ids[o++] = '[';
        const char *p = g_settings.immich_albums;
        while (*p && o < sizeof(ids) - 4) {
            if (o > 1) ids[o++] = ',';
            ids[o++] = '"';
            while (*p && *p != ',' && o < sizeof(ids) - 4) ids[o++] = *p++;
            ids[o++] = '"';
            if (*p == ',') p++;
        }
        ids[o++] = ']';
        ids[o] = '\0';
        snprintf(body, sizeof(body),
                 "{\"albumIds\":%s,\"size\":8,\"type\":\"IMAGE\"}", ids);
    } else {
        // No albums chosen: the whole library.
        snprintf(body, sizeof(body), "{\"size\":8,\"type\":\"IMAGE\"}");
    }

    fetch_req_t r = {
        .url = url, .method = "POST", .body = body,
        .content_type = "application/json",
        .api_key = g_settings.immich_key,
        .tls = tls_for_url(), .ca_pem = g_settings.immich_ca,
        // Eight assets with their metadata is comfortably under this; the
        // old 16 KB was close enough to the edge to be worth widening too.
        .limit = 64 * 1024,
    };
    char *resp = fetch_request(&r, NULL);
    if (!resp) return NULL;

    if (resp[0] == '<') {
        ESP_LOGE(TAG, "search returned HTML -- the URL points at the web app, not the API");
        free(resp);
        return NULL;
    }
    cJSON *doc = cJSON_Parse(resp);
    free(resp);
    if (!cJSON_IsArray(doc) || cJSON_GetArraySize(doc) == 0) {
        if (doc) ESP_LOGW(TAG, "search returned no assets -- wrong album id?");
        cJSON_Delete(doc);
        return NULL;
    }

    const int n = cJSON_GetArraySize(doc);
    cJSON *pick = cJSON_GetArrayItem(doc, (int)(esp_random() % (uint32_t)n));
    cJSON *id = cJSON_GetObjectItemCaseSensitive(pick, "id");
    char *out = (cJSON_IsString(id)) ? strdup(id->valuestring) : NULL;
    cJSON_Delete(doc);
    return out;
}

bool immich_next_photo(char *out_path, size_t out_len)
{
    if (!immich_configured() || !wifi_ready()) return false;

    mkdir(CACHE_DIR, 0777);

    char *id = random_asset_id();
    if (!id) return false;

    // Immich ids are UUIDs, but nothing guarantees that, so size for the
    // longest thing the server could hand back rather than for the expected.
    char path[sizeof(CACHE_DIR) + 300];
    snprintf(path, sizeof(path), CACHE_DIR "/%s.jpg", id);

    // Already cached: an outage later still leaves this photo playable, which
    // is the whole reason for caching to the card rather than to RAM.
    struct stat st;
    if (stat(path, &st) == 0 && st.st_size > 0) {
        snprintf(out_path, out_len, "%s", path);
        free(id);
        return true;
    }

    char url[SETTINGS_URL_MAX + 96];
    char rel[96];
    // "preview" is the server-rendered ~1440 px rendition. Asking for the
    // original would mean pulling a 12 MP file over WiFi and then software
    // decoding it -- about 1.5 s per photo -- for no visible gain on an
    // 800x1280 panel.
    snprintf(rel, sizeof(rel), "/assets/%s/thumbnail?size=preview", id);
    build_url(url, sizeof(url), rel);

    fetch_req_t r = {
        .url = url, .method = "GET", .api_key = g_settings.immich_key,
        .tls = tls_for_url(), .ca_pem = g_settings.immich_ca,
    };
    size_t bytes = 0;
    bool ok = fetch_to_file(&r, path, &bytes);
    free(id);

    if (!ok) return false;
    ESP_LOGI(TAG, "cached %s (%u KB)", path, (unsigned)(bytes / 1024));
    snprintf(out_path, out_len, "%s", path);

    immich_prune_cache();
    return true;
}

void immich_prune_cache(void)
{
    const uint32_t cap_mb = g_settings.immich_cache_mb ? g_settings.immich_cache_mb : 64;
    const uint64_t cap = (uint64_t)cap_mb * 1024 * 1024;

    DIR *d = opendir(CACHE_DIR);
    if (!d) return;

    uint64_t total = 0;
    char oldest[sizeof(CACHE_DIR) + 300] = {0};
    time_t oldest_t = 0;
    int files = 0;

    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char p[sizeof(CACHE_DIR) + 300];
        snprintf(p, sizeof(p), CACHE_DIR "/%s", e->d_name);
        struct stat st;
        if (stat(p, &st) != 0) continue;
        total += st.st_size;
        files++;
        if (!oldest[0] || st.st_mtime < oldest_t) {
            oldest_t = st.st_mtime;
            strncpy(oldest, p, sizeof(oldest) - 1);
        }
    }
    closedir(d);

    // One file per call rather than a loop: pruning runs after every download,
    // so the cache converges without ever blocking playback on a long sweep.
    if (total > cap && oldest[0]) {
        ESP_LOGI(TAG, "cache %u MB over %u MB cap, dropping %s",
                 (unsigned)(total >> 20), (unsigned)cap_mb, oldest);
        remove(oldest);
    }
}
