#include "fetch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "fetch";

static esp_http_client_handle_t open_client(const fetch_req_t *req)
{
    esp_http_client_config_t cfg = {
        .url = req->url,
        .timeout_ms = 15000,
        .buffer_size = 2048,
        .method = (req->method && strcmp(req->method, "POST") == 0)
                  ? HTTP_METHOD_POST : HTTP_METHOD_GET,
    };

    switch (req->tls) {
        case FETCH_TLS_BUNDLE: cfg.crt_bundle_attach = esp_crt_bundle_attach; break;
        case FETCH_TLS_PEM:    cfg.cert_pem = req->ca_pem;                    break;
        case FETCH_TLS_NONE:   break;   // plain HTTP; nothing to verify
    }

    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return NULL;

    if (req->api_key)      esp_http_client_set_header(c, "x-api-key", req->api_key);
    if (req->content_type) esp_http_client_set_header(c, "Content-Type", req->content_type);
    esp_http_client_set_header(c, "Accept", "application/json");
    return c;
}

// One read that understands chunked transfer encoding.
//
// esp_http_client_read() returning 0 does NOT mean the body is finished. Both
// Open-Meteo and Immich reply with Transfer-Encoding: chunked, and a read that
// lands on a chunk boundary returns 0 with more still to come. Both loops
// below used to treat that as end-of-body, which truncated the response:
// valid bytes, invalid document. Observed as a run of consecutive
// "unparseable response" warnings that walked the retry backoff out to 8
// minutes, having fetched successfully minutes earlier -- so it depends on how
// the body happens to be split across chunks, not on anything in the request.
// The same truncation would corrupt an Immich JPEG silently, on the card.
//
// esp_http_client_is_complete_data_received() is the only thing that
// distinguishes a boundary from the end.
//
// Returns >0 bytes read, 0 at the genuine end of the body, -1 on error.
static int read_body(esp_http_client_handle_t c, char *dst, size_t cap)
{
    int idle = 0;
    for (;;) {
        const int n = esp_http_client_read(c, dst, cap);
        if (n > 0) return n;
        if (n < 0) return -1;
        if (esp_http_client_is_complete_data_received(c)) return 0;
        if (++idle > 40) {                     // ~2 s with nothing arriving
            ESP_LOGW(TAG, "stalled waiting for the rest of the body");
            return -1;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// Open, send any body, and read the status. Returns the status code, or -1.
static int begin(esp_http_client_handle_t c, const fetch_req_t *req)
{
    const size_t blen = req->body ? strlen(req->body) : 0;
    if (esp_http_client_open(c, blen) != ESP_OK) return -1;
    if (blen && esp_http_client_write(c, req->body, blen) < 0) return -1;
    esp_http_client_fetch_headers(c);
    return esp_http_client_get_status_code(c);
}

char *fetch_request(const fetch_req_t *req, int *out_len)
{
    esp_http_client_handle_t c = open_client(req);
    if (!c) return NULL;

    int status = begin(c, req);
    if (status != 200) {
        // 401 means the API key is wrong; 404 usually means the base URL has
        // a stray /api on the end. Saying which is worth the log line.
        ESP_LOGW(TAG, "HTTP %d for %s", status, req->url);
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        return NULL;
    }

    const size_t limit = req->limit ? req->limit : 4096;
    char *buf = malloc(limit + 1);
    if (!buf) { esp_http_client_close(c); esp_http_client_cleanup(c); return NULL; }

    size_t got = 0;
    while (got < limit) {
        const int n = read_body(c, buf + got, limit - got);
        if (n <= 0) break;
        got += n;
    }
    buf[got] = '\0';

    esp_http_client_close(c);
    esp_http_client_cleanup(c);

    // Filling the buffer exactly almost always means the response was longer
    // than we allowed. The result is a body that is valid bytes but invalid
    // JSON, which fails to parse for a reason nothing else would reveal.
    if (got >= limit)
        ESP_LOGW(TAG, "response hit the %u byte limit and was truncated -- "
                      "the parse below will fail", (unsigned)limit);

    if (got == 0) { free(buf); return NULL; }
    if (out_len) *out_len = (int)got;
    return buf;
}

char *fetch_json(const char *url, size_t limit)
{
    // Everything Open-Meteo serves is public HTTPS.
    fetch_req_t r = { .url = url, .tls = FETCH_TLS_BUNDLE, .limit = limit };
    return fetch_request(&r, NULL);
}

bool fetch_to_file(const fetch_req_t *req, const char *path, size_t *out_bytes)
{
    esp_http_client_handle_t c = open_client(req);
    if (!c) return false;

    int status = begin(c, req);
    if (status != 200) {
        ESP_LOGW(TAG, "HTTP %d downloading %s", status, req->url);
        esp_http_client_close(c); esp_http_client_cleanup(c);
        return false;
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "cannot create %s", path);
        esp_http_client_close(c); esp_http_client_cleanup(c);
        return false;
    }

    char *buf = malloc(2048);
    if (!buf) { fclose(f); esp_http_client_close(c); esp_http_client_cleanup(c); return false; }

    size_t total = 0;
    bool ok = true;
    for (;;) {
        const int n = read_body(c, buf, 2048);
        if (n == 0) break;
        if (n < 0) { ok = false; break; }
        if (fwrite(buf, 1, n, f) != (size_t)n) { ESP_LOGE(TAG, "short write"); ok = false; break; }
        total += n;
    }

    free(buf);
    fclose(f);
    esp_http_client_close(c);
    esp_http_client_cleanup(c);

    // Never leave a 0-byte OR a half-written photo: a truncated JPEG on the
    // card looks like a valid file and fails much later, at decode time.
    if (!ok || total == 0) { remove(path); return false; }
    if (out_bytes) *out_bytes = total;
    return true;
}
