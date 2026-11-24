// ===========================================================================
// gate_bigphoto.c -- Stage 0.7: software decode of an oversized photo
// ===========================================================================
// The last unmeasured number in the imaging pipeline, and the one the whole
// cache design rests on.
//
// The hardware JPEG decoder cannot downscale and tops out around 1920x1080.
// A 12 MP phone photo (4000x3000) decodes to ~24 MB of RGB565 and simply does
// not fit beside the framebuffers -- it cannot be displayed directly at any
// speed. So ingest decodes it ONCE in software with a scale applied *during*
// decode, which is what caps peak memory, then hardware-encodes a cache entry.
//
// This gate measures what that costs, and produces two values the firmware
// needs:
//
//   1. Wall-clock time per photo. Ingest runs at low priority behind playback,
//      so this sets how long a fresh card takes to become watchable, and
//      whether the ingest task must yield inside the decode or merely between
//      photos.
//
//   2. Peak PSRAM during decode. This decides whether ingest can run while the
//      compositor holds its buffers, or has to wait for a gap.
//
// esp_new_jpeg expresses scale as a TARGET RESOLUTION (not a divisor), capped
// at 1/8, and both axes must be multiples of 8. So a 4000x3000 source at 1/4
// is not 1000x750 -- 750 is not a multiple of 8. Rounding is mandatory, and
// getting it wrong is a decode failure rather than a slightly-off image.
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_jpeg_dec.h"
#include "esp_jpeg_common.h"
#include "board.h"

static const char *TAG = "gate0.7";

#define PREFERRED_PATH  BSP_SD_MOUNT "/stage0/big.jpg"

// Round down to a multiple of 8 -- required by the scaler on both axes.
static inline int mul8(int v) { return v & ~7; }

// Find the largest .jpg on the card. The gate asks for a specific file, but a
// card that already holds photos is the more likely case, and the largest file
// is the best stand-in for "a big camera JPEG".
#define MAX_CANDIDATES 32

typedef struct { char path[256]; long size; } candidate_t;
static candidate_t g_cand[MAX_CANDIDATES];
static int    g_jpeg_count;
static double g_jpeg_bytes;
static long   g_jpeg_min = 0x7FFFFFFF;

static bool find_largest_jpeg(const char *dir, char *out, size_t out_len, long *out_size, int depth)
{
    if (depth > 3) return false;                 // don't walk a deep tree forever
    DIR *d = opendir(dir);
    if (!d) return false;

    bool found = false;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;       // skip dotfiles and . / ..

        char path[320];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);

        struct stat st;
        if (stat(path, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            if (find_largest_jpeg(path, out, out_len, out_size, depth + 1)) found = true;
            continue;
        }

        const char *dot = strrchr(e->d_name, '.');
        if (!dot) continue;
        if (strcasecmp(dot, ".jpg") != 0 && strcasecmp(dot, ".jpeg") != 0) continue;

        g_jpeg_count++;
        g_jpeg_bytes += st.st_size;
        if (st.st_size < g_jpeg_min) g_jpeg_min = st.st_size;
        if (g_jpeg_count <= MAX_CANDIDATES) {
            candidate_t *c = &g_cand[g_jpeg_count - 1];
            strncpy(c->path, path, sizeof(c->path) - 1);
            c->size = st.st_size;
        }

        if (st.st_size > *out_size) {
            *out_size = st.st_size;
            strncpy(out, path, out_len - 1);
            out[out_len - 1] = '\0';
            found = true;
        }
    }
    closedir(d);
    return found;
}

// Identify what the file actually IS before blaming the decoder. Returns true
// if it looks like a baseline JPEG this decoder can handle.
//
// Two things masquerade as a decodable .jpeg and both are common:
//   - a PNG (or other format) that was simply renamed
//   - a PROGRESSIVE JPEG (SOF2), which embedded decoders almost never support
// Neither is a bug in our code, and neither is visible without looking.
typedef enum { JK_BASELINE, JK_PROGRESSIVE, JK_PNG, JK_OTHER } jpeg_kind_t;

static jpeg_kind_t describe_jpeg(const uint8_t *b, size_t len)
{
    ESP_LOGI(TAG, "  first bytes: %02X %02X %02X %02X %02X %02X %02X %02X",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);

    if (b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G') {
        ESP_LOGE(TAG, "  This is a PNG with a .jpg/.jpeg name -- not a JPEG at all.");
        return JK_PNG;
    }
    if (b[0] != 0xFF || b[1] != 0xD8) {
        ESP_LOGE(TAG, "  No JPEG SOI marker (expected FF D8) -- not a JPEG.");
        return JK_OTHER;
    }

    // Walk the segment chain looking for the frame header.
    size_t i = 2;
    while (i + 3 < len) {
        if (b[i] != 0xFF) { i++; continue; }
        uint8_t m = b[i + 1];
        if (m == 0xFF) { i++; continue; }
        if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) { i += 2; continue; }

        uint16_t seg = (uint16_t)((b[i + 2] << 8) | b[i + 3]);
        if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
            int h = (b[i + 5] << 8) | b[i + 6];
            int w = (b[i + 7] << 8) | b[i + 8];
            const char *kind =
                (m == 0xC0) ? "baseline (SOF0)" :
                (m == 0xC1) ? "extended sequential (SOF1)" :
                (m == 0xC2) ? "PROGRESSIVE (SOF2)" :
                (m == 0xC3) ? "lossless (SOF3)" : "unusual SOF";
            ESP_LOGI(TAG, "  frame: %s, %dx%d, %d component(s)", kind, w, h, b[i + 9]);
            if (m == 0xC2) {
                ESP_LOGE(TAG, "  PROGRESSIVE JPEG -- esp_new_jpeg decodes BASELINE only.");
                ESP_LOGE(TAG, "  This is a real constraint for the photo frame, not a test");
                ESP_LOGE(TAG, "  artefact: ingest must detect and skip these, or re-save them.");
                return JK_PROGRESSIVE;
            }
            return (m == 0xC0 || m == 0xC1) ? JK_BASELINE : JK_OTHER;
        }
        if (m == 0xDA) break;              // start of scan; no frame header found
        i += 2 + seg;
    }
    ESP_LOGE(TAG, "  no SOF marker found -- malformed or truncated");
    return JK_OTHER;
}

// One decode at a given divisor. Returns wall-clock µs, or -1.
static int64_t decode_at(uint8_t *jpg, size_t jpg_len, int div, int src_w, int src_h)
{
    jpeg_dec_config_t cfg = DEFAULT_JPEG_DEC_CONFIG();
    // RGB565 little-endian: what the compositor and the PPA actually consume,
    // so this measures the real path rather than a convenient one.
    cfg.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    if (div > 1) {
        cfg.scale.width  = mul8(src_w / div);
        cfg.scale.height = mul8(src_h / div);
    }

    jpeg_dec_handle_t dec = NULL;
    if (jpeg_dec_open(&cfg, &dec) != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "  1/%d: jpeg_dec_open failed", div);
        return -1;
    }

    jpeg_dec_io_t io = { .inbuf = jpg, .inbuf_len = (int)jpg_len };
    jpeg_dec_header_info_t info = {0};
    if (jpeg_dec_parse_header(dec, &io, &info) != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "  1/%d: parse_header failed", div);
        jpeg_dec_close(dec);
        return -1;
    }

    int outlen = 0;
    if (jpeg_dec_get_outbuf_len(dec, &outlen) != JPEG_ERR_OK || outlen <= 0) {
        ESP_LOGE(TAG, "  1/%d: get_outbuf_len failed", div);
        jpeg_dec_close(dec);
        return -1;
    }

    size_t before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    uint8_t *out = (uint8_t *)jpeg_calloc_align(outlen, 16);
    if (!out) {
        ESP_LOGE(TAG, "  1/%d: cannot allocate %d bytes (%.2f MiB) of output",
                 div, outlen, outlen / 1048576.0);
        jpeg_dec_close(dec);
        return -1;
    }
    io.outbuf = out;

    int64_t t0 = esp_timer_get_time();
    jpeg_error_t err = jpeg_dec_process(dec, &io);
    int64_t dt = esp_timer_get_time() - t0;

    size_t during = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t used = (before > during) ? (before - during) : 0;

    if (err != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "  1/%d: decode failed (%d)", div, err);
        jpeg_free_align(out);
        jpeg_dec_close(dec);
        return -1;
    }

    ESP_LOGI(TAG, "  1/%-2d -> %4dx%-4d  %8.1f ms   out %5.2f MiB   peak PSRAM %5.2f MiB",
             div,
             (div > 1) ? mul8(src_w / div) : src_w,
             (div > 1) ? mul8(src_h / div) : src_h,
             dt / 1000.0, outlen / 1048576.0, used / 1048576.0);

    jpeg_free_align(out);
    jpeg_dec_close(dec);
    return dt;
}

void gate_bigphoto_run(void)
{
    ESP_LOGI(TAG, "=== Stage 0.7: software decode with downscale (ingest path) ===");

    char ignored[320] = {0};
    long biggest = 0;
    find_largest_jpeg(BSP_SD_MOUNT, ignored, sizeof(ignored), &biggest, 0);

    if (g_jpeg_count == 0) {
        ESP_LOGE(TAG, "  FAIL: no .jpg/.jpeg anywhere on the card.");
        return;
    }
    ESP_LOGI(TAG, "  library: %d file(s), %.1f MiB total, %.2f MiB avg, "
                  "smallest %.2f MiB, largest %.2f MiB",
             g_jpeg_count, g_jpeg_bytes / 1048576.0,
             g_jpeg_bytes / g_jpeg_count / 1048576.0,
             g_jpeg_min / 1048576.0, biggest / 1048576.0);

    // Largest first: the biggest file is the best stand-in for a camera photo.
    int n = (g_jpeg_count < MAX_CANDIDATES) ? g_jpeg_count : MAX_CANDIDATES;
    for (int i = 0; i < n - 1; i++)
        for (int j = i + 1; j < n; j++)
            if (g_cand[j].size > g_cand[i].size) {
                candidate_t t = g_cand[i]; g_cand[i] = g_cand[j]; g_cand[j] = t;
            }

    // Try each in turn. A renamed PNG or a progressive JPEG is not a failure of
    // this gate -- it is a fact about the library, and the survey at the end
    // says how much of it the firmware would actually be able to show.
    int usable = 0, png = 0, progressive = 0, other = 0;
    bool measured = false;

    for (int i = 0; i < n; i++) {
        ESP_LOGI(TAG, "--- [%d/%d] %s (%.2f MiB) ---",
                 i + 1, n, g_cand[i].path, g_cand[i].size / 1048576.0);

        long size = g_cand[i].size;
        uint8_t *jpg = heap_caps_aligned_alloc(16, size, MALLOC_CAP_SPIRAM);
        if (!jpg) { ESP_LOGE(TAG, "  cannot allocate %ld bytes", size); continue; }

        FILE *f = fopen(g_cand[i].path, "rb");
        if (!f) { ESP_LOGE(TAG, "  cannot open"); heap_caps_free(jpg); continue; }
        size_t got = fread(jpg, 1, size, f);
        fclose(f);
        if (got != (size_t)size) {
            ESP_LOGE(TAG, "  short read"); heap_caps_free(jpg); continue;
        }

        jpeg_kind_t kind = describe_jpeg(jpg, size);
        if (kind == JK_PNG)              png++;
        else if (kind == JK_PROGRESSIVE) progressive++;
        else if (kind != JK_BASELINE)    other++;

        if (kind != JK_BASELINE) { heap_caps_free(jpg); continue; }
        usable++;

        // Measure on the FIRST usable file only; the rest are just surveyed.
        if (measured) { heap_caps_free(jpg); continue; }

        jpeg_dec_config_t probe_cfg = DEFAULT_JPEG_DEC_CONFIG();
        probe_cfg.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
        jpeg_dec_handle_t probe = NULL;
        int src_w = 0, src_h = 0;
        if (jpeg_dec_open(&probe_cfg, &probe) == JPEG_ERR_OK) {
            jpeg_dec_io_t io = { .inbuf = jpg, .inbuf_len = (int)size };
            jpeg_dec_header_info_t info = {0};
            if (jpeg_dec_parse_header(probe, &io, &info) == JPEG_ERR_OK) {
                src_w = info.width; src_h = info.height;
            }
            jpeg_dec_close(probe);
        }
        if (src_w == 0 || src_h == 0) {
            ESP_LOGE(TAG, "  header parse failed despite a valid SOF");
            heap_caps_free(jpg); continue;
        }

        ESP_LOGI(TAG, "  %dx%d (%.1f MP); full RGB565 would be %.1f MiB",
                 src_w, src_h, src_w * (double)src_h / 1e6,
                 src_w * (double)src_h * 2 / 1048576.0);
        if (src_w > JPEG_HW_MAX_DIM || src_h > JPEG_HW_MAX_DIM)
            ESP_LOGI(TAG, "  -> exceeds the HW decoder; this is the ingest case");
        else
            ESP_LOGW(TAG, "  -> small enough for the HW decoder; a bigger photo would");

        ESP_LOGI(TAG, "  decode at each scale:");
        int64_t t_quarter = -1;
        for (int div = 1; div <= 8; div *= 2) {
            int64_t t = decode_at(jpg, size, div, src_w, src_h);
            if (div == 4) t_quarter = t;
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        if (t_quarter > 0) {
            const double sec = t_quarter / 1e6;
            ESP_LOGI(TAG, "  --> 1/4 decode costs %.2f s per photo", sec);
            ESP_LOGI(TAG, "  --> a 500-photo card caches in ~%.0f min", 500 * sec / 60.0);
            if (sec > 1.0)
                ESP_LOGW(TAG, "  --> >1 s: ingest must yield INSIDE the decode loop");
            else
                ESP_LOGI(TAG, "  --> under 1 s: yielding between photos is enough");
            measured = true;
        }
        heap_caps_free(jpg);
    }

    // ---- the survey is itself a finding -----------------------------------
    ESP_LOGI(TAG, "--- library survey ---");
    ESP_LOGI(TAG, "  %d baseline JPEG (usable), %d PNG-in-disguise, %d progressive, %d other",
             usable, png, progressive, other);
    if (png > 0) {
        ESP_LOGW(TAG, "  EXTENSION IS NOT FORMAT. %d file(s) named .jpg/.jpeg are PNGs.", png);
        ESP_LOGW(TAG, "  Ingest MUST sniff magic bytes rather than trust the name, or it");
        ESP_LOGW(TAG, "  will hand garbage to the decoder for a whole class of library.");
    }
    if (progressive > 0)
        ESP_LOGW(TAG, "  %d progressive JPEG(s): baseline-only decoder cannot read these.", progressive);
    if (!measured)
        ESP_LOGE(TAG, "  NOT MEASURED: no baseline JPEG on the card. Copy real camera photos.");

    ESP_LOGI(TAG, "=== Stage 0.7 complete ===");
}
