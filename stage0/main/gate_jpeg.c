// ===========================================================================
// gate_jpeg.c -- Stage 0.5 and 0.7: the JPEG pipeline
// ===========================================================================
// These two gates together justify the cache design, or kill it.
//
// The constraint driving everything: the P4's hardware JPEG decoder has NO
// downscale-during-decode, and Espressif characterises it only to ~1920x1080.
// A 12 MP phone photo (4000x3000) therefore cannot be handed to it at all --
// not slowly, not at all. Its decoded form is 24 MB of RGB565, which does not
// fit alongside the framebuffers even in 32 MB of PSRAM.
//
//   Gate 0.5  measures the FAST path: hardware decode of a cache-sized file.
//             This is what playback costs, and it must be small enough to hide
//             behind the previous photo's dwell.
//
//   Gate 0.7  measures the SLOW path: software decode of a real 12 MP photo
//             at 1/4 scale. This is what INGEST costs, paid once per photo,
//             and its peak heap decides whether ingest can run alongside
//             playback or has to yield between rows.
//
// If 0.5 is fast and 0.7 is merely slow, the design holds: pay once at ingest,
// never at playback.
#include <string.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "driver/jpeg_decode.h"
#include "driver/jpeg_encode.h"
#include "board.h"

static const char *TAG = "gate0.5";

// A synthetic JPEG stands in for a real one when no SD card is present. It
// exercises the decoder's throughput honestly -- entropy differs from a real
// photo, so treat the number as indicative and re-run against real files off
// the card once gate 0.3 passes.
static uint8_t *make_test_jpeg(int w, int h, size_t *out_len)
{
    const size_t rgb_len = (size_t)w * h * 3;
    uint8_t *rgb = heap_caps_aligned_alloc(128, rgb_len, MALLOC_CAP_SPIRAM);
    if (!rgb) { ESP_LOGE(TAG, "no PSRAM for %ux%u RGB888 source", w, h); return NULL; }

    // A gradient plus noise: flat colour would compress to almost nothing and
    // flatter the decoder; noise alone would be worst-case. This is neither.
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint8_t *p = rgb + ((size_t)y * w + x) * 3;
            p[0] = (uint8_t)(x * 255 / w);
            p[1] = (uint8_t)(y * 255 / h);
            p[2] = (uint8_t)((x ^ y) & 0xFF);
        }
    }

    jpeg_encoder_handle_t enc = NULL;
    jpeg_encode_engine_cfg_t ecfg = { .timeout_ms = 5000 };
    if (jpeg_new_encoder_engine(&ecfg, &enc) != ESP_OK) {
        ESP_LOGE(TAG, "jpeg_new_encoder_engine failed"); heap_caps_free(rgb); return NULL;
    }

    size_t out_cap = rgb_len / 4;
    jpeg_encode_memory_alloc_cfg_t mcfg = { .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER };
    size_t got = 0;
    uint8_t *jpg = (uint8_t *)jpeg_alloc_encoder_mem(out_cap, &mcfg, &got);
    if (!jpg) { ESP_LOGE(TAG, "no encoder output buffer"); jpeg_del_encoder_engine(enc); heap_caps_free(rgb); return NULL; }

    jpeg_encode_cfg_t cfg = {
        .src_type = JPEG_ENCODE_IN_FORMAT_RGB888,
        .sub_sample = JPEG_DOWN_SAMPLING_YUV420,   // what cameras emit
        .image_quality = 85,
        .width = w, .height = h,
    };
    uint32_t jpg_len = 0;
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = jpeg_encoder_process(enc, &cfg, rgb, rgb_len, jpg, got, &jpg_len);
    int64_t dt = esp_timer_get_time() - t0;

    jpeg_del_encoder_engine(enc);
    heap_caps_free(rgb);

    if (err != ESP_OK) { ESP_LOGE(TAG, "encode failed: %s", esp_err_to_name(err)); return NULL; }

    ESP_LOGI(TAG, "  HW encode %4dx%-4d q85 -> %6" PRIu32 " bytes in %6.2f ms  (ingest writes these)",
             w, h, jpg_len, dt / 1000.0);
    *out_len = jpg_len;
    return jpg;
}

static void bench_hw_decode(const uint8_t *jpg, size_t jpg_len, int w, int h)
{
    jpeg_decoder_handle_t dec = NULL;
    jpeg_decode_engine_cfg_t ecfg = { .timeout_ms = 5000 };
    if (jpeg_new_decoder_engine(&ecfg, &dec) != ESP_OK) {
        ESP_LOGE(TAG, "jpeg_new_decoder_engine failed"); return;
    }

    // The decoder pads YUV420/422 output up to 16-pixel boundaries, so a
    // 1440x1080 image actually lands as 1440x1088. Allocating w*h*2 is short
    // by exactly that padding -- which is how the Immich-preview case failed
    // on the first run of this gate.
    const int w_pad = (w + 15) & ~15;
    const int h_pad = (h + 15) & ~15;
    size_t need = (size_t)w_pad * h_pad * 2;   // RGB565 out, padded
    jpeg_decode_memory_alloc_cfg_t mcfg = { .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER };
    size_t got = 0;
    uint8_t *out = (uint8_t *)jpeg_alloc_decoder_mem(need, &mcfg, &got);
    if (!out) { ESP_LOGE(TAG, "no decoder output buffer"); jpeg_del_decoder_engine(dec); return; }

    jpeg_decode_cfg_t cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_RGB,
    };

    // Mean of several: the first decode pays for cold caches.
    const int reps = 5;
    int64_t total = 0;
    uint32_t out_len = 0;
    for (int i = 0; i < reps; i++) {
        int64_t t0 = esp_timer_get_time();
        esp_err_t err = jpeg_decoder_process(dec, &cfg, jpg, jpg_len, out, got, &out_len);
        total += esp_timer_get_time() - t0;
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "decode failed: %s", esp_err_to_name(err));
            jpeg_del_decoder_engine(dec); heap_caps_free(out); return;
        }
    }
    int64_t us = total / reps;
    ESP_LOGI(TAG, "  HW decode %4dx%-4d -> RGB565 in %6.2f ms  (%.1f fps equivalent)%s",
             w, h, us / 1000.0, 1e6 / us,
             (w_pad != w || h_pad != h) ? "  [output padded to 16px]" : "");

    jpeg_del_decoder_engine(dec);
    heap_caps_free(out);
}

void gate_jpeg_run(void)
{
    ESP_LOGI(TAG, "=== Stage 0.5: hardware JPEG codec ===");
    ESP_LOGI(TAG, "playback path: cache file (<= %d px) -> HW decode -> one PPA fit pass",
             CACHE_FIT_BOX);

    // The three shapes that actually occur:
    //   1280x800  - a landscape photo already fitted to screen
    //   1280x1280 - the cache box worst case
    //   1440x1080 - an Immich "preview" rendition, which skips ingest entirely
    const struct { int w, h; const char *what; } cases[] = {
        { 1280,  800, "landscape cache entry" },
        { 1280, 1280, "cache box worst case"   },
        { 1440, 1080, "Immich preview"         },
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        ESP_LOGI(TAG, "--- %s (%dx%d) ---", cases[i].what, cases[i].w, cases[i].h);
        size_t len = 0;
        uint8_t *jpg = make_test_jpeg(cases[i].w, cases[i].h, &len);
        if (!jpg) continue;
        bench_hw_decode(jpg, len, cases[i].w, cases[i].h);
        heap_caps_free(jpg);
    }

    ESP_LOGI(TAG, "PASS CRITERION: cache-entry decode well under the photo dwell");
    ESP_LOGI(TAG, "  (it is prefetched during the previous photo, so even ~100 ms is invisible)");
    ESP_LOGI(TAG, "=== Stage 0.5 complete ===");
}
