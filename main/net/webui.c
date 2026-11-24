#include "webui.h"
#include "webpage.h"
#include "wifi.h"
#include "settings.h"
#include "board.h"
#include "photo/playlist.h"
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "lwip/sockets.h"
#include "upload.h"
#include "immich.h"
#include "ui/theme.h"
#include "mbedtls/base64.h"

static const char *TAG = "httpd";

static httpd_handle_t s_server;
static bool           s_ap_mode;
static char           s_addr[24] = "0.0.0.0";
static char           s_ap_ssid[24];
static TaskHandle_t   s_dns_task;
static volatile bool  s_dns_run;

// ---------------------------------------------------------------------------
// Form parsing. Values arrive percent-encoded from a normal form POST.
// ---------------------------------------------------------------------------
static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode(char *s)
{
    char *o = s;
    for (char *p = s; *p; p++) {
        if (*p == '+') { *o++ = ' '; }
        else if (*p == '%' && hexval(p[1]) >= 0 && hexval(p[2]) >= 0) {
            *o++ = (char)(hexval(p[1]) * 16 + hexval(p[2]));
            p += 2;
        } else *o++ = *p;
    }
    *o = '\0';
}

// Pull one field out of an application/x-www-form-urlencoded body.
static bool form_get(const char *body, const char *key, char *out, size_t out_len)
{
    size_t klen = strlen(key);
    const char *p = body;
    while (p && *p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            const char *v = p + klen + 1;
            const char *e = strchr(v, '&');
            size_t n = e ? (size_t)(e - v) : strlen(v);
            if (n >= out_len) n = out_len - 1;
            memcpy(out, v, n);
            out[n] = '\0';
            url_decode(out);
            return true;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Authentication
//
// HTTP Basic, checked against a salted SHA-256 in NVS. Two deliberate holes,
// both worth stating plainly rather than discovering later:
//
//   * The setup access point is NEVER protected. It is the recovery path for
//     a frame configured for a network that no longer exists, and a password
//     there could strand the device permanently.
//   * Basic auth base64-encodes the password, which is encoding, not
//     encryption. On a plain-HTTP LAN it is no more exposed than the settings
//     form itself, but it is not protection against anyone on your network.
// ---------------------------------------------------------------------------
static bool authorised(httpd_req_t *r)
{
    if (s_ap_mode) return true;                 // setup portal stays open
    if (!g_settings.web_pw_set) return true;    // nothing set yet

    char hdr[160];
    if (httpd_req_get_hdr_value_str(r, "Authorization", hdr, sizeof(hdr)) != ESP_OK)
        return false;
    if (strncmp(hdr, "Basic ", 6) != 0) return false;

    unsigned char dec[128];
    size_t dlen = 0;
    if (mbedtls_base64_decode(dec, sizeof(dec) - 1, &dlen,
                              (const unsigned char *)hdr + 6, strlen(hdr + 6)) != 0)
        return false;
    dec[dlen] = '\0';

    // "user:password" -- the username is ignored; there is only one account.
    char *colon = strchr((char *)dec, ':');
    if (!colon) return false;
    return settings_check_password(colon + 1);
}

static esp_err_t deny(httpd_req_t *r)
{
    httpd_resp_set_status(r, "401 Unauthorized");
    httpd_resp_set_hdr(r, "WWW-Authenticate", "Basic realm=\"Photo Frame\"");
    return httpd_resp_send(r, "Authentication required", HTTPD_RESP_USE_STRLEN);
}

#define REQUIRE_AUTH(r) do { if (!authorised(r)) return deny(r); } while (0)

// ---------------------------------------------------------------------------
// Handlers
// ---------------------------------------------------------------------------
static esp_err_t sock_open(httpd_handle_t hd, int sockfd)
{
    (void)hd;
    ESP_LOGI(TAG, "connection opened (fd %d)", sockfd);
    return ESP_OK;
}

static void sock_close(httpd_handle_t hd, int sockfd)
{
    (void)hd;
    ESP_LOGI(TAG, "connection closed (fd %d)", sockfd);
    close(sockfd);
}

static esp_err_t h_root(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    httpd_resp_set_type(r, "text/html");
    return httpd_resp_send(r, SETTINGS_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t h_status(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    // Bit order matches the checkbox order on the page.
    const uint8_t ov = g_settings.ov_items;

    char buf[512];
    int n = snprintf(buf, sizeof(buf),
        "{\"mode\":\"%s\",\"ip\":\"%s\",\"ssid\":\"%s\",\"lat\":%.4f,\"lon\":%.4f,"
        "\"tz\":\"%s\",\"orient\":%d,\"units\":%d,\"ov\":%d,\"photos\":%d,\"rssi\":%d,"
        "\"thm\":%d,\"rot180\":%d,\"dwell\":%d,\"pw\":%d,\"imurl\":\"%s\",\"imalb\":\"%s\",\"srcm\":%d,\"imcmb\":%d,"
        "\"imkey\":%d}",
        s_ap_mode ? "Setup mode" : "Connected", s_addr, g_settings.wifi_ssid,
        g_settings.latitude, g_settings.longitude, g_settings.tz,
        g_settings.landscape ? 1 : 0, g_settings.imperial ? 1 : 0,
        ov, playlist_count(), wifi_rssi(), g_settings.theme,
        g_settings.rotate180 ? 1 : 0, g_settings.dwell_s,
        g_settings.web_pw_set ? 1 : 0,
        g_settings.immich_url, g_settings.immich_albums,
        g_settings.source_mode, g_settings.immich_cache_mb,
        // Whether a key is stored, never the key itself -- the page has no
        // reason to see it, and it would end up in the browser's cache.
        g_settings.immich_key[0] ? 1 : 0);
    httpd_resp_set_type(r, "application/json");
    return httpd_resp_send(r, buf, n);
}

static esp_err_t h_scan(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    httpd_resp_set_type(r, "application/json");

    wifi_scan_config_t cfg = { .show_hidden = false };
    if (esp_wifi_scan_start(&cfg, true) != ESP_OK)
        return httpd_resp_send(r, "[]", 2);

    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n > 20) n = 20;
    wifi_ap_record_t *aps = calloc(n ? n : 1, sizeof(wifi_ap_record_t));
    if (!aps) return httpd_resp_send(r, "[]", 2);
    esp_wifi_scan_get_ap_records(&n, aps);

    httpd_resp_sendstr_chunk(r, "[");
    // De-duplicate: a mesh network shows the same SSID once per band and per
    // access point, which makes the list useless without this.
    for (int i = 0; i < n; i++) {
        bool dup = false;
        for (int j = 0; j < i; j++)
            if (strcmp((char *)aps[i].ssid, (char *)aps[j].ssid) == 0) { dup = true; break; }
        if (dup || aps[i].ssid[0] == '\0') continue;

        char item[128];
        snprintf(item, sizeof(item), "%s{\"ssid\":\"%s\",\"rssi\":%d}",
                 i ? "," : "", (char *)aps[i].ssid, aps[i].rssi);
        httpd_resp_sendstr_chunk(r, item);
    }
    httpd_resp_sendstr_chunk(r, "]");
    httpd_resp_sendstr_chunk(r, NULL);
    free(aps);
    return ESP_OK;
}

static esp_err_t h_save(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    int len = r->content_len;
    // 6 KB, not 2: the Immich certificate field alone allows 2 KB, and a POST
    // carrying it plus every other field would otherwise be rejected outright
    // with a 400 before a single value was parsed.
    if (len <= 0 || len > 6144) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "bad body");

    char *body = calloc(1, len + 1);
    if (!body) return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");

    int got = 0;
    while (got < len) {
        int n = httpd_req_recv(r, body + got, len - got);
        if (n <= 0) { free(body); return ESP_FAIL; }
        got += n;
    }

    char v[SETTINGS_STR_MAX];
    if (form_get(body, "ssid", v, sizeof(v)) && v[0])
        strncpy(g_settings.wifi_ssid, v, sizeof(g_settings.wifi_ssid) - 1);

    // An empty password field KEEPS the stored one. The page never sends the
    // saved password back to the browser, so blank means "unchanged" rather
    // than "erase" -- otherwise every settings edit would need it retyped.
    if (form_get(body, "pass", v, sizeof(v)) && v[0])
        strncpy(g_settings.wifi_pass, v, sizeof(g_settings.wifi_pass) - 1);

    if (form_get(body, "lat", v, sizeof(v)) && v[0]) g_settings.latitude  = strtof(v, NULL);
    if (form_get(body, "lon", v, sizeof(v)) && v[0]) g_settings.longitude = strtof(v, NULL);
    // The custom field wins when filled in, matching the CYD project: the
    // dropdown covers the common cases, the text box covers everything else.
    char tzc[SETTINGS_STR_MAX];
    if (form_get(body, "tzcustom", tzc, sizeof(tzc)) && tzc[0])
        strncpy(g_settings.tz, tzc, sizeof(g_settings.tz) - 1);
    else if (form_get(body, "tz", v, sizeof(v)) && v[0])
        strncpy(g_settings.tz, v, sizeof(g_settings.tz) - 1);

    if (form_get(body, "orient", v, sizeof(v))) g_settings.landscape = (v[0] == '1');
    // Absent means unchecked, as with every other checkbox on this page.
    g_settings.rotate180 = form_get(body, "rot180", v, sizeof(v));
    if (form_get(body, "units",  v, sizeof(v))) g_settings.imperial  = (v[0] == '1');
    if (form_get(body, "dwell", v, sizeof(v))) {
        int d = atoi(v);
        // Clamped: below about three seconds the transition has not finished
        // before the next photo is due, and the frame looks frantic.
        g_settings.dwell_s = (uint16_t)(d < 3 ? 3 : (d > 3600 ? 3600 : d));
    }
    if (form_get(body, "thm", v, sizeof(v))) {
        int t = atoi(v);
        g_settings.theme = (uint8_t)(t < 0 || t >= THEME_COUNT ? 0 : t);
    }

    // An unchecked checkbox is simply ABSENT from a form POST -- it does not
    // arrive as 0. So the mask is rebuilt from presence, not from values, and
    // every box must be tested even when the answer is "not there".
    uint8_t mask = 0;
    if (form_get(body, "ov_clock",   v, sizeof(v))) mask |= 1 << 0;
    if (form_get(body, "ov_weather", v, sizeof(v))) mask |= 1 << 1;
    if (form_get(body, "ov_sun",     v, sizeof(v))) mask |= 1 << 2;
    if (form_get(body, "ov_aqi",     v, sizeof(v))) mask |= 1 << 3;
    g_settings.ov_items = mask;

    // ---- Immich ----
    {
        // The CA field alone can be 2 KB, so this uses its own buffer rather
        // than the small one the other fields share.
        char *big = calloc(1, SETTINGS_CA_MAX);
        if (big) {
            if (form_get(body, "imurl", big, SETTINGS_URL_MAX))
                strncpy(g_settings.immich_url, big, sizeof(g_settings.immich_url) - 1);
            // Blank keeps the stored key, same rule as the passwords.
            if (form_get(body, "imkey", big, SETTINGS_KEY_MAX) && big[0])
                strncpy(g_settings.immich_key, big, sizeof(g_settings.immich_key) - 1);
            if (form_get(body, "imca", big, SETTINGS_CA_MAX))
                strncpy(g_settings.immich_ca, big, sizeof(g_settings.immich_ca) - 1);
            free(big);
        }

        if (form_get(body, "srcm", v, sizeof(v)))  g_settings.source_mode = (uint8_t)atoi(v);
        if (form_get(body, "imcmb", v, sizeof(v))) {
            int mb = atoi(v);
            // Clamp rather than trust: 0 would make the pruner delete every
            // photo it had just downloaded.
            g_settings.immich_cache_mb = (uint8_t)(mb < 8 ? 8 : (mb > 255 ? 255 : mb));
        }

        // Album checkboxes all share the name "alb", so they arrive as
        // repeated keys. form_get() finds only the first, so this walks the
        // body directly and rebuilds the comma-separated list.
        g_settings.immich_albums[0] = '\0';
        size_t used = 0;
        const char *q = body;
        while ((q = strstr(q, "alb=")) != NULL) {
            if (q != body && q[-1] != '&') { q += 4; continue; }
            q += 4;
            size_t n = strcspn(q, "&");
            if (n && used + n + 2 < sizeof(g_settings.immich_albums)) {
                if (used) g_settings.immich_albums[used++] = ',';
                memcpy(g_settings.immich_albums + used, q, n);
                used += n;
                g_settings.immich_albums[used] = '\0';
            }
            q += n;
        }
    }

    // Blank means unchanged, exactly as for the WiFi password -- otherwise
    // every settings edit would need it retyped. But "unchanged" alone leaves
    // no way to REMOVE a password once set, which is a lockout waiting to
    // happen, so an explicit checkbox clears it.
    if (form_get(body, "webpw_clear", v, sizeof(v))) {
        settings_set_password("");
        ESP_LOGW(TAG, "web password removed");
    } else if (form_get(body, "webpw", v, sizeof(v)) && v[0]) {
        settings_set_password(v);
    }

    free(body);

    settings_save();
    ESP_LOGI(TAG, "settings saved: immich url \"%s\" key %s albums \"%s\" mode %u; restarting",
             g_settings.immich_url,
             g_settings.immich_key[0] ? "set" : "empty",
             g_settings.immich_albums, g_settings.source_mode);

    httpd_resp_set_type(r, "text/html");
    httpd_resp_send(r, SAVED_HTML, HTTPD_RESP_USE_STRLEN);

    // Restart rather than applying live: WiFi, timezone, location and
    // orientation would each need a different refresh path, and a restart
    // takes three seconds and cannot leave the device half-configured.
    vTaskDelay(pdMS_TO_TICKS(700));
    esp_restart();
    return ESP_OK;
}

static esp_err_t h_upload(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    return upload_handle(r);
}

static esp_err_t h_albums(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    httpd_resp_set_type(r, "application/json");

    char *json = immich_albums_json();
    if (!json) {
        // An empty array rather than an error: the page says "check the URL
        // and key", which is more use than a red HTTP status.
        return httpd_resp_send(r, "[]", 2);
    }
    esp_err_t e = httpd_resp_send(r, json, HTTPD_RESP_USE_STRLEN);
    free(json);
    return e;
}

// ---------------------------------------------------------------------------
// Browse and delete
// ---------------------------------------------------------------------------
static esp_err_t h_photos(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    httpd_resp_set_type(r, "application/json");

    const int MAX = 64;
    photo_info_t *list = calloc(MAX, sizeof(photo_info_t));
    if (!list) return httpd_resp_send(r, "[]", 2);
    int n = playlist_enumerate(list, MAX);

    httpd_resp_sendstr_chunk(r, "[");
    for (int i = 0; i < n; i++) {
        char item[400];
        snprintf(item, sizeof(item), "%s{\"path\":\"%s\",\"kb\":%lu,\"ok\":%d}",
                 i ? "," : "", list[i].path,
                 (unsigned long)(list[i].size / 1024), list[i].usable ? 1 : 0);
        httpd_resp_sendstr_chunk(r, item);
    }
    httpd_resp_sendstr_chunk(r, "]");
    httpd_resp_sendstr_chunk(r, NULL);
    free(list);
    return ESP_OK;
}

// Any path arriving from the network is validated rather than trusted, for
// reading as well as deleting. Three rules, each closing a real hole:
//   * it must sit under the card's mount point
//   * it must contain no ".." -- otherwise the first rule means nothing
//   * it must be a .jpg/.jpeg, so these endpoints can never reach firmware,
//     settings, or anything else that happens to live on the card
//
// The read path needs this every bit as much as the delete path: without it
// /photo would happily serve any file on the card to anyone who asked.
static bool is_photo_path(const char *path)
{
    const size_t mlen = strlen(BSP_SD_MOUNT);
    if (strncmp(path, BSP_SD_MOUNT "/", mlen + 1) != 0) return false;
    if (strstr(path, "..")) return false;

    const char *dot = strrchr(path, '.');
    if (!dot) return false;
    return strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0;
}

static esp_err_t h_delete(httpd_req_t *r)
{
    REQUIRE_AUTH(r);

    int len = r->content_len;
    if (len <= 0 || len > 512)
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "bad body");

    char *body = calloc(1, len + 1);
    if (!body) return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    int got = 0;
    while (got < len) {
        int n = httpd_req_recv(r, body + got, len - got);
        if (n <= 0) { free(body); return ESP_FAIL; }
        got += n;
    }

    char path[PLAYLIST_PATH_MAX];
    bool have = form_get(body, "path", path, sizeof(path));
    free(body);

    if (!have || !is_photo_path(path)) {
        ESP_LOGW(TAG, "refused delete of \"%s\"", have ? path : "(none)");
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "not a deletable path");
    }

    if (remove(path) != 0) {
        ESP_LOGE(TAG, "delete failed: %s", path);
        return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "delete failed");
    }
    ESP_LOGW(TAG, "deleted %s", path);

    int n = playlist_scan(BSP_SD_MOUNT);
    char msg[64];
    int mlen = snprintf(msg, sizeof(msg), "{\"ok\":1,\"photos\":%d}", n);
    httpd_resp_set_type(r, "application/json");
    return httpd_resp_send(r, msg, mlen);
}

// Serve a photo file byte-for-byte. The device does NOT decode, scale or
// thumbnail anything here -- it streams the file and the browser does the
// scaling with CSS. Generating thumbnails on-device would mean a decode and a
// re-encode per photo, on a CPU that is busy compositing at 34 fps.
static esp_err_t h_photo(httpd_req_t *r)
{
    REQUIRE_AUTH(r);

    char query[320];
    if (httpd_req_get_url_query_str(r, query, sizeof(query)) != ESP_OK)
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "no path");

    char enc[PLAYLIST_PATH_MAX];
    if (httpd_query_key_value(query, "path", enc, sizeof(enc)) != ESP_OK)
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "no path");
    url_decode(enc);

    if (!is_photo_path(enc))
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "not a photo path");

    FILE *f = fopen(enc, "rb");
    if (!f) return httpd_resp_send_err(r, HTTPD_404_NOT_FOUND, "no such photo");

    httpd_resp_set_type(r, "image/jpeg");
    // Files never change under a given name here, and the card reads at well
    // under 1 MB/s -- so let the browser keep them rather than re-fetching on
    // every refresh of the list.
    httpd_resp_set_hdr(r, "Cache-Control", "max-age=86400");

    char *buf = malloc(2048);
    if (!buf) { fclose(f); return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); }

    size_t n;
    esp_err_t err = ESP_OK;
    while ((n = fread(buf, 1, 2048, f)) > 0) {
        if (httpd_resp_send_chunk(r, buf, n) != ESP_OK) { err = ESP_FAIL; break; }
    }
    free(buf);
    fclose(f);

    if (err != ESP_OK) return ESP_FAIL;          // client went away mid-stream
    return httpd_resp_send_chunk(r, NULL, 0);
}

// Captive-portal catch-all: anything we do not recognise gets redirected to
// the settings page, which is what makes phones pop it open by themselves.
static esp_err_t h_redirect(httpd_req_t *r, httpd_err_code_t err)
{
    (void)err;
    char loc[64];
    snprintf(loc, sizeof(loc), "http://%s/", s_addr);
    httpd_resp_set_status(r, "302 Found");
    httpd_resp_set_hdr(r, "Location", loc);
    return httpd_resp_send(r, NULL, 0);
}

// ---------------------------------------------------------------------------
// Captive-portal DNS: answer every query with our own address.
// ---------------------------------------------------------------------------
static void dns_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { ESP_LOGE(TAG, "dns socket"); vTaskDelete(NULL); return; }

    struct sockaddr_in me = { .sin_family = AF_INET, .sin_port = htons(53),
                              .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(sock, (struct sockaddr *)&me, sizeof(me)) < 0) {
        ESP_LOGE(TAG, "dns bind"); close(sock); vTaskDelete(NULL); return;
    }

    struct timeval tv = { .tv_sec = 1 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t buf[512];
    while (s_dns_run) {
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &flen);
        if (n < 12) continue;

        // Turn the query into an answer in place: flip the response bit, keep
        // one question, append an A record pointing at us.
        buf[2] = 0x81; buf[3] = 0x80;          // response, recursion available
        buf[6] = 0; buf[7] = 1;                 // one answer
        buf[8] = 0; buf[9] = 0;                 // no authority
        buf[10] = 0; buf[11] = 0;               // no additional

        if (n + 16 > (int)sizeof(buf)) continue;
        uint8_t *a = buf + n;
        *a++ = 0xC0; *a++ = 0x0C;               // pointer to the question name
        *a++ = 0x00; *a++ = 0x01;               // type A
        *a++ = 0x00; *a++ = 0x01;               // class IN
        *a++ = 0; *a++ = 0; *a++ = 0; *a++ = 60;// TTL 60 s
        *a++ = 0x00; *a++ = 0x04;               // 4-byte address
        *a++ = 192; *a++ = 168; *a++ = 4; *a++ = 1;

        sendto(sock, buf, a - buf, 0, (struct sockaddr *)&from, flen);
    }
    close(sock);
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
esp_err_t webui_begin(void)
{
    if (s_server) return ESP_OK;

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers  = 12;
    // Must fit inside CONFIG_LWIP_MAX_SOCKETS, which was cut to 8 to free
    // internal RAM for DMA. The server reserves 3 sockets internally, so the
    // default of 7 no longer fits and httpd_start() refuses outright.
    cfg.max_open_sockets  = 4;
    cfg.lru_purge_enable  = true;
    cfg.stack_size        = 8192;
    cfg.uri_match_fn      = httpd_uri_match_wildcard;
    cfg.open_fn           = sock_open;
    cfg.close_fn          = sock_close;
    // Bind explicitly to every interface. The default is already INADDR_ANY,
    // but being explicit costs nothing and rules it out as a suspect.
    cfg.server_port       = 80;
    cfg.ctrl_port         = 32768;

    ESP_RETURN_ON_ERROR(httpd_start(&s_server, &cfg), TAG, "httpd_start");

    httpd_uri_t root   = { .uri = "/",       .method = HTTP_GET,  .handler = h_root };
    httpd_uri_t status = { .uri = "/status", .method = HTTP_GET,  .handler = h_status };
    httpd_uri_t scan   = { .uri = "/scan",   .method = HTTP_GET,  .handler = h_scan };
    httpd_uri_t save   = { .uri = "/save",   .method = HTTP_POST, .handler = h_save };
    httpd_uri_t upl    = { .uri = "/upload", .method = HTTP_POST, .handler = h_upload };
    httpd_uri_t phs    = { .uri = "/photos", .method = HTTP_GET,  .handler = h_photos };
    httpd_uri_t del    = { .uri = "/delete", .method = HTTP_POST, .handler = h_delete };
    httpd_uri_t pic    = { .uri = "/photo",  .method = HTTP_GET,  .handler = h_photo };
    httpd_uri_t alb    = { .uri = "/albums", .method = HTTP_GET,  .handler = h_albums };
    httpd_register_uri_handler(s_server, &root);
    httpd_register_uri_handler(s_server, &status);
    httpd_register_uri_handler(s_server, &scan);
    httpd_register_uri_handler(s_server, &save);
    httpd_register_uri_handler(s_server, &upl);
    httpd_register_uri_handler(s_server, &phs);
    httpd_register_uri_handler(s_server, &del);
    httpd_register_uri_handler(s_server, &pic);
    httpd_register_uri_handler(s_server, &alb);
    httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, h_redirect);

    if (!s_ap_mode) snprintf(s_addr, sizeof(s_addr), "%s", wifi_ip());
    ESP_LOGI(TAG, "HTTP server listening on port 80, all interfaces");
    ESP_LOGI(TAG, "settings page at http://%s/", s_addr);
    return ESP_OK;
}

void webui_stop(void)
{
    // NB: IDF also declares httpd_stop(handle); ours is webui_stop() so the
    // two never shadow each other.
    if (s_server) { httpd_stop(s_server); s_server = NULL; }
}

esp_err_t webui_start_setup_ap(void)
{
    // The SSID carries the last four hex of the MAC so two frames on a bench
    // do not collide.
    uint8_t mac[6] = {0};
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "PhotoFrame-%02X%02X", mac[4], mac[5]);

    esp_netif_t *ap_netif = wifi_netif_ap();
    if (!ap_netif) { ESP_LOGE(TAG, "no AP netif"); return ESP_FAIL; }

    wifi_config_t ap = {0};
    strncpy((char *)ap.ap.ssid, s_ap_ssid, sizeof(ap.ap.ssid) - 1);
    ap.ap.ssid_len       = strlen(s_ap_ssid);
    ap.ap.max_connection = 4;
    // OPEN on purpose. This is the recovery path for a frame configured for a
    // network that no longer exists -- a password here could strand it.
    ap.ap.authmode       = WIFI_AUTH_OPEN;
    ap.ap.channel        = 1;

    // AP+STA, not AP alone: the radio keeps retrying the real network in the
    // background and drops the setup network by itself once that succeeds.
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "apsta");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap), TAG, "ap cfg");

    // Pin the address rather than trusting the default, and restart the DHCP
    // server explicitly. Handing out leases is the whole job of this mode; if
    // it silently is not running, the portal is unreachable even though the
    // network appears in the client's list.
    esp_netif_dhcps_stop(ap_netif);
    esp_netif_ip_info_t ip = {0};
    IP4_ADDR(&ip.ip,      192, 168, 4, 1);
    IP4_ADDR(&ip.gw,      192, 168, 4, 1);
    IP4_ADDR(&ip.netmask, 255, 255, 255, 0);
    ESP_RETURN_ON_ERROR(esp_netif_set_ip_info(ap_netif, &ip), TAG, "ap ip");
    ESP_RETURN_ON_ERROR(esp_netif_dhcps_start(ap_netif), TAG, "dhcps");

    // Check LATE, not here. Bringing up the AP goes through the C6 over RPC,
    // so the interface is not up yet at this point and an immediate check
    // always reports "not running" even when it is about to. Checking too
    // early is how a working portal gets misdiagnosed as a broken one.

    s_ap_mode = true;
    strcpy(s_addr, "192.168.4.1");

    s_dns_run = true;
    xTaskCreate(dns_task, "dns", 4096, NULL, 4, &s_dns_task);

    ESP_LOGI(TAG, "setup network \"%s\" is up -- open network, on purpose", s_ap_ssid);
    ESP_LOGI(TAG, "join it and browse to http://192.168.4.1/ (it should open by itself)");

    // Now that the AP interface has had time to come up over RPC, report what
    // actually matters: whether clients will get an address.
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_netif_dhcp_status_t st = ESP_NETIF_DHCP_INIT;
    esp_netif_dhcps_get_status(ap_netif, &st);
    if (st == ESP_NETIF_DHCP_STARTED) {
        ESP_LOGI(TAG, "DHCP server running -- clients will get 192.168.4.x");
    } else {
        ESP_LOGE(TAG, "DHCP server NOT running: clients will associate and then");
        ESP_LOGE(TAG, "sit with no address. The portal is unreachable in that state.");
    }
    return ESP_OK;
}

void webui_stop_setup_ap(void)
{
    if (!s_ap_mode) return;
    s_dns_run = false;
    esp_wifi_set_mode(WIFI_MODE_STA);
    s_ap_mode = false;
    snprintf(s_addr, sizeof(s_addr), "%s", wifi_ip());
    ESP_LOGI(TAG, "setup network dropped; settings page now at http://%s/", s_addr);
}

bool        webui_is_ap(void)     { return s_ap_mode; }
const char *webui_address(void)   { return s_addr; }
const char *webui_ap_ssid(void)   { return s_ap_ssid; }
