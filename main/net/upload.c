#include "upload.h"
#include "board.h"
#include "photo/playlist.h"
#include "photo/photo.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include "esp_log.h"

static const char *TAG = "upload";

#define UPLOAD_DIR   BSP_SD_MOUNT "/photos"
#define CHUNK        4096

// Pull `boundary=...` out of the Content-Type header.
static bool get_boundary(httpd_req_t *r, char *out, size_t out_len)
{
    char ct[160];
    if (httpd_req_get_hdr_value_str(r, "Content-Type", ct, sizeof(ct)) != ESP_OK) return false;
    char *b = strstr(ct, "boundary=");
    if (!b) return false;
    b += 9;
    if (*b == '"') b++;
    size_t n = strcspn(b, "\";");
    if (n == 0 || n + 3 >= out_len) return false;
    // The delimiter on the wire is the boundary with two leading dashes.
    out[0] = '-'; out[1] = '-';
    memcpy(out + 2, b, n);
    out[n + 2] = '\0';
    return true;
}

// Take the filename from a Content-Disposition line, stripped of any path.
// A browser sends only a basename, but a hand-rolled client could send
// "../../etc/something" and we are writing to a real filesystem.
static void safe_filename(const char *disp, char *out, size_t out_len)
{
    out[0] = '\0';
    const char *f = strstr(disp, "filename=");
    if (!f) return;
    f += 9;
    if (*f == '"') f++;
    size_t n = strcspn(f, "\"\r\n");
    if (n == 0) return;

    char raw[128];
    if (n >= sizeof(raw)) n = sizeof(raw) - 1;
    memcpy(raw, f, n);
    raw[n] = '\0';

    const char *base = raw;
    for (const char *p = raw; *p; p++) if (*p == '/' || *p == '\\') base = p + 1;

    size_t o = 0;
    for (const char *p = base; *p && o < out_len - 1; p++) {
        // Whitelist rather than blacklist: FAT is unforgiving and the set of
        // characters that are safe is much shorter than the set that is not.
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
            (*p >= '0' && *p <= '9') || *p == '.' || *p == '-' || *p == '_')
            out[o++] = *p;
    }
    out[o] = '\0';
}

esp_err_t upload_handle(httpd_req_t *req)
{
    char boundary[80];
    if (!get_boundary(req, boundary, sizeof(boundary)))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "not multipart");

    const size_t blen = strlen(boundary);
    mkdir(UPLOAD_DIR, 0777);

    char *buf = malloc(CHUNK + 1);
    if (!buf) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");

    FILE *f = NULL;
    char fname[96] = {0};
    int  remaining = req->content_len;
    int  saved = 0;
    bool in_data = false;

    // `carry` holds back the tail of each chunk, because a boundary can be
    // split across two reads. Without it a file gets its own terminator
    // written into it whenever the split lands badly -- which is rare, and
    // therefore exactly the kind of bug that ships.
    char carry[128];
    size_t carry_len = 0;

    while (remaining > 0) {
        int want = remaining > CHUNK ? CHUNK : remaining;
        int n = httpd_req_recv(req, buf, want);
        if (n <= 0) break;
        remaining -= n;

        // Work over carry + this chunk as one contiguous span.
        size_t total = carry_len + n;
        char *work = malloc(total + 1);
        if (!work) break;
        memcpy(work, carry, carry_len);
        memcpy(work + carry_len, buf, n);
        work[total] = '\0';
        carry_len = 0;

        size_t pos = 0;
        while (pos < total) {
            if (!in_data) {
                // Look for a part header block ending in a blank line.
                char *hdr_end = NULL;
                for (size_t i = pos; i + 3 < total; i++)
                    if (memcmp(work + i, "\r\n\r\n", 4) == 0) { hdr_end = work + i; break; }
                if (!hdr_end) break;                 // need more bytes

                size_t hlen = (hdr_end - work) - pos;
                char hdrs[512];
                size_t copy = hlen < sizeof(hdrs) - 1 ? hlen : sizeof(hdrs) - 1;
                memcpy(hdrs, work + pos, copy);
                hdrs[copy] = '\0';

                if (strstr(hdrs, "filename=")) {
                    safe_filename(hdrs, fname, sizeof(fname));
                    if (fname[0]) {
                        char path[160];
                        snprintf(path, sizeof(path), "%s/%s", UPLOAD_DIR, fname);
                        f = fopen(path, "wb");
                        if (!f) ESP_LOGE(TAG, "cannot create %s", path);
                        else     ESP_LOGI(TAG, "receiving %s", fname);
                        in_data = true;
                    }
                }
                pos = (hdr_end - work) + 4;
                if (!in_data) continue;
            }

            // Stream data until the next boundary appears.
            char *bnd = NULL;
            for (size_t i = pos; i + blen <= total; i++)
                if (memcmp(work + i, boundary, blen) == 0) { bnd = work + i; break; }

            if (bnd) {
                // Drop the CRLF that precedes the delimiter; it is framing,
                // not part of the file.
                size_t dlen = (bnd - work) - pos;
                if (dlen >= 2) dlen -= 2;
                if (f && dlen) fwrite(work + pos, 1, dlen, f);
                if (f) { fclose(f); f = NULL; saved++; ESP_LOGI(TAG, "saved %s", fname); }
                in_data = false;
                pos = (bnd - work) + blen;
            } else {
                // Keep back enough that a boundary split across reads is
                // still recognised next time round.
                size_t keep = blen + 4;
                if (total - pos > keep) {
                    size_t writable = total - pos - keep;
                    if (f) fwrite(work + pos, 1, writable, f);
                    pos += writable;
                }
                carry_len = total - pos;
                if (carry_len > sizeof(carry)) carry_len = sizeof(carry);
                memcpy(carry, work + pos, carry_len);
                pos = total;
            }
        }
        free(work);
    }

    if (f) fclose(f);
    free(buf);

    // Rescan so an uploaded photo joins the rotation without a reboot.
    int n = playlist_scan(BSP_SD_MOUNT);
    ESP_LOGI(TAG, "%d file(s) saved; playlist now %d photo(s)", saved, n);

    char msg[320];
    snprintf(msg, sizeof(msg),
        "<!DOCTYPE html><html><head><meta charset=utf-8>"
        "<meta http-equiv=refresh content='2;url=/'></head>"
        "<body style='background:#12151a;color:#e8ecf1;font:16px sans-serif;"
        "text-align:center;padding:40px'>"
        "<h2 style='color:#6fd3ef'>Uploaded %d file(s)</h2>"
        "<p>%d photos in the rotation.</p></body></html>", saved, n);
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, msg, HTTPD_RESP_USE_STRLEN);
}
