#include "photo.h"
#include "board.h"
#include "settings.h"
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "driver/jpeg_decode.h"
#include "driver/ppa.h"
#include "esp_jpeg_dec.h"
#include "esp_jpeg_common.h"

static const char *TAG = "photo";

static ppa_client_handle_t s_ppa_srm;
static ppa_client_handle_t s_ppa_fill;

const char *photo_err_str(photo_err_t e)
{
    switch (e) {
        case PHOTO_OK:               return "ok";
        case PHOTO_ERR_NOT_JPEG:     return "not a JPEG (PNG or other format)";
        case PHOTO_ERR_PROGRESSIVE:  return "progressive JPEG (baseline only)";
        case PHOTO_ERR_MALFORMED:    return "malformed or truncated";
        case PHOTO_ERR_IO:           return "read error";
        case PHOTO_ERR_MEMORY:       return "out of memory";
    }
    return "?";
}

esp_err_t frame_alloc(frame_t *f, int w, int h)
{
    // 128-byte alignment: the PPA is a DMA master and external memory has to
    // meet both L1 and L2 cache line requirements.
    f->pixels = heap_caps_aligned_alloc(128, (size_t)w * h * 2, MALLOC_CAP_SPIRAM);
    if (!f->pixels) return ESP_ERR_NO_MEM;
    f->w = w; f->h = h;
    memset(f->pixels, 0, (size_t)w * h * 2);
    return ESP_OK;
}

void frame_free(frame_t *f)
{
    if (f->pixels) { heap_caps_free(f->pixels); f->pixels = NULL; }
    f->w = f->h = 0;
}

// ---------------------------------------------------------------------------
// Format sniffing. Two files out of seven on the test card were PNGs wearing a
// .jpeg extension, so the name is never trusted. The check walks to the SOF
// marker rather than stopping at FF D8, because progressive JPEGs share
// baseline's magic bytes and cannot be decoded by esp_new_jpeg.
// ---------------------------------------------------------------------------
// NOTE: this expects the WHOLE file. Handed a truncated head it will report
// MALFORMED for any photo whose EXIF pushes the SOF marker past the end --
// which is most phone photos. photo_probe() seeks through the file instead.
static photo_err_t sniff(const uint8_t *b, size_t len, int *w, int *h)
{
    if (len < 16) return PHOTO_ERR_MALFORMED;
    if (b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G') return PHOTO_ERR_NOT_JPEG;
    if (b[0] != 0xFF || b[1] != 0xD8) return PHOTO_ERR_NOT_JPEG;

    size_t i = 2;
    while (i + 9 < len) {
        if (b[i] != 0xFF)  { i++; continue; }
        uint8_t m = b[i + 1];
        if (m == 0xFF)     { i++; continue; }
        if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) { i += 2; continue; }

        uint16_t seg = (uint16_t)((b[i + 2] << 8) | b[i + 3]);
        // SOFn, excluding DHT (C4), JPG (C8) and DAC (CC)
        if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
            if (m == 0xC2) return PHOTO_ERR_PROGRESSIVE;
            if (m != 0xC0 && m != 0xC1) return PHOTO_ERR_NOT_JPEG;
            if (h) *h = (b[i + 5] << 8) | b[i + 6];
            if (w) *w = (b[i + 7] << 8) | b[i + 8];
            return PHOTO_OK;
        }
        if (m == 0xDA) break;                 // start of scan, no frame header
        i += 2 + seg;
    }
    return PHOTO_ERR_MALFORMED;
}

static uint8_t *read_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0) { fclose(f); return NULL; }

    uint8_t *buf = heap_caps_aligned_alloc(16, len, MALLOC_CAP_SPIRAM);
    if (!buf) { fclose(f); return NULL; }

    // Read in chunks rather than one 250 KB fread.
    //
    // The destination is PSRAM, so the SD driver bounces every transfer
    // through a DMA-capable buffer -- and it sizes that buffer to the
    // transfer. A whole-file read therefore demanded a very large internal
    // DMA allocation, competing directly with the ESP32-C6 link, which needs
    // a DMA buffer for every packet it receives. The card won, the radio
    // started dropping packets with "mempool OOM (RX)", and the symptom was
    // no DNS and an unreachable web server -- nothing pointing at photo I/O.
    //
    // 16 KB keeps the bounce buffer small and short-lived. It costs a few
    // extra fread calls on a read that already takes a couple of hundred
    // milliseconds on this card.
    #define READ_CHUNK (16 * 1024)
    size_t got = 0;
    while (got < (size_t)len) {
        const size_t want = ((size_t)len - got) > READ_CHUNK ? READ_CHUNK : (size_t)len - got;
        const size_t n = fread(buf + got, 1, want, f);
        if (n == 0) break;
        got += n;
    }
    fclose(f);
    if (got != (size_t)len) { heap_caps_free(buf); return NULL; }
    *out_len = len;
    return buf;
}

// ---------------------------------------------------------------------------
// Decode into a temporary RGB565 buffer at the source's own size (or a
// downscale of it). Caller frees via heap_caps_free.
// ---------------------------------------------------------------------------
static uint16_t *decode_hw(const uint8_t *jpg, size_t len, int w, int h, int *out_w, int *out_h)
{
    jpeg_decoder_handle_t dec = NULL;
    jpeg_decode_engine_cfg_t ecfg = { .timeout_ms = 5000 };
    if (jpeg_new_decoder_engine(&ecfg, &dec) != ESP_OK) return NULL;

    // The decoder pads YUV420/422 output up to 16-pixel boundaries -- 1440x1080
    // actually lands as 1440x1088. Allocating w*h*2 is short by exactly that
    // padding and the decode is rejected outright.
    const int wp = (w + 15) & ~15, hp = (h + 15) & ~15;
    size_t need = (size_t)wp * hp * 2;

    jpeg_decode_memory_alloc_cfg_t mcfg = { .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER };
    size_t got = 0;
    uint16_t *out = (uint16_t *)jpeg_alloc_decoder_mem(need, &mcfg, &got);
    if (!out) { jpeg_del_decoder_engine(dec); return NULL; }

    jpeg_decode_cfg_t cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_RGB,
    };
    uint32_t olen = 0;
    esp_err_t err = jpeg_decoder_process(dec, &cfg, jpg, len, (uint8_t *)out, got, &olen);
    jpeg_del_decoder_engine(dec);
    if (err != ESP_OK) { heap_caps_free(out); return NULL; }

    *out_w = wp; *out_h = hp;      // padded geometry; the fit pass crops via block_w/h
    return out;
}

static uint16_t *decode_sw(const uint8_t *jpg, size_t len, int w, int h, int *out_w, int *out_h)
{
    // Scale during decode. Gate 0.7 measured this as saving memory rather than
    // time, so the divisor is chosen purely to keep the buffer affordable:
    // a 12 MP photo at 1/1 is 22 MiB, which will not fit beside the
    // framebuffers once the compositor holds them.
    int div = 1;
    while ((w / div) > 2048 || (h / div) > 2048) div *= 2;
    if (div > 8) div = 8;                         // the scaler caps at 1/8

    jpeg_dec_config_t cfg = DEFAULT_JPEG_DEC_CONFIG();
    cfg.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    if (div > 1) {
        // Both axes must be integer multiples of 8 or the decode fails.
        cfg.scale.width  = (w / div) & ~7;
        cfg.scale.height = (h / div) & ~7;
    }

    jpeg_dec_handle_t dec = NULL;
    if (jpeg_dec_open(&cfg, &dec) != JPEG_ERR_OK) return NULL;

    jpeg_dec_io_t io = { .inbuf = (uint8_t *)jpg, .inbuf_len = (int)len };
    jpeg_dec_header_info_t info = {0};
    if (jpeg_dec_parse_header(dec, &io, &info) != JPEG_ERR_OK) { jpeg_dec_close(dec); return NULL; }

    int outlen = 0;
    if (jpeg_dec_get_outbuf_len(dec, &outlen) != JPEG_ERR_OK || outlen <= 0) {
        jpeg_dec_close(dec); return NULL;
    }
    uint16_t *out = (uint16_t *)jpeg_calloc_align(outlen, 16);
    if (!out) { jpeg_dec_close(dec); return NULL; }
    io.outbuf = (uint8_t *)out;

    jpeg_error_t err = jpeg_dec_process(dec, &io);
    jpeg_dec_close(dec);
    if (err != JPEG_ERR_OK) { jpeg_free_align(out); return NULL; }

    *out_w = (div > 1) ? ((w / div) & ~7) : w;
    *out_h = (div > 1) ? ((h / div) & ~7) : h;
    return out;
}

// ---------------------------------------------------------------------------
// The single PPA pass: scale to fit, rotate for orientation, letterbox.
// This is the ONLY place rotation happens, and it runs once per photo rather
// than once per frame -- which is what makes the orientation setting free.
// ---------------------------------------------------------------------------
// `swap_bytes` compensates for the source's RGB565 byte order.
//
// The P4's hardware JPEG decoder emits RGB565 with the opposite byte order to
// the one the PPA and the panel use, and there is no flag on the decoder to
// change it -- jpeg_decode_cfg_t offers only rgb_order (RGB/BGR) and a YUV
// conversion standard. The PPA's input config does have byte_swap, so the
// correction happens here, inside the fit pass that every photo already goes
// through. It costs nothing extra.
//
// The symptom when this is wrong is distinctive and worth recording: smooth
// gradients turn into rainbow contour bands. A plain R/B swap would shift the
// hues but leave gradients smooth; scrambling bits ACROSS the 5-6-5 field
// boundaries is what produces banding.
static esp_err_t fit_into(uint16_t *src, int sw, int sh, int crop_w, int crop_h,
                          frame_t *dst, bool landscape, bool swap_bytes)
{
    if (!s_ppa_srm) {
        ppa_client_config_t c = { .oper_type = PPA_OPERATION_SRM };
        ESP_RETURN_ON_ERROR(ppa_register_client(&c, &s_ppa_srm), TAG, "ppa srm client");
    }
    if (!s_ppa_fill) {
        ppa_client_config_t c = { .oper_type = PPA_OPERATION_FILL };
        ESP_RETURN_ON_ERROR(ppa_register_client(&c, &s_ppa_fill), TAG, "ppa fill client");
    }

    // In landscape the photo is rotated 90 deg, so it must be fitted against
    // the panel's SWAPPED dimensions.
    const int fit_w = landscape ? dst->h : dst->w;
    const int fit_h = landscape ? dst->w : dst->h;

    float sx = (float)fit_w / crop_w;
    float sy = (float)fit_h / crop_h;
    float scale = (sx < sy) ? sx : sy;            // contain, never crop
    // The PPA scaler tops out at 4x; beyond that the source is tiny anyway.
    if (scale > 4.0f) scale = 4.0f;

    int out_w = (int)(crop_w * scale) & ~1;
    int out_h = (int)(crop_h * scale) & ~1;
    if (out_w > fit_w) out_w = fit_w;
    if (out_h > fit_h) out_h = fit_h;

    // Letterbox: clear the whole frame to black first, so anything the photo
    // does not cover is black rather than whatever was there before.
    //
    // This is a PPA fill, NOT a CPU memset. A memset writes through the cache,
    // while the PPA and the display DMA read PSRAM directly -- so the cleared
    // bars can still hold stale memory by the time they are scanned out, and
    // the letterbox shows garbage instead of black. Keeping the clear in the
    // same DMA domain as the blit that follows avoids the coherency question
    // entirely, and is faster than a 2 MiB CPU write besides.
    {
        ppa_fill_oper_config_t fill = {0};
        fill.out.buffer      = dst->pixels;
        fill.out.buffer_size = (uint32_t)dst->w * dst->h * 2;
        fill.out.pic_w       = dst->w;
        fill.out.pic_h       = dst->h;
        fill.out.fill_cm     = PPA_FILL_COLOR_MODE_RGB565;
        fill.fill_block_w    = dst->w;
        fill.fill_block_h    = dst->h;
        fill.fill_argb_color.val = 0xFF000000;   // opaque black
        fill.mode = PPA_TRANS_MODE_BLOCKING;
        esp_err_t ferr = ppa_do_fill(s_ppa_fill, &fill);
        if (ferr != ESP_OK) {
            // Fall back rather than leave the frame undefined; a visible
            // letterbox beats an unpredictable one.
            ESP_LOGW(TAG, "ppa fill: %s -- falling back to memset", esp_err_to_name(ferr));
            memset(dst->pixels, 0, (size_t)dst->w * dst->h * 2);
        }
    }

    int off_x, off_y;
    if (landscape) {
        // After a 90 deg rotation, source x maps to dest y and vice versa.
        off_x = (dst->w - out_h) / 2;
        off_y = (dst->h - out_w) / 2;
    } else {
        off_x = (dst->w - out_w) / 2;
        off_y = (dst->h - out_h) / 2;
    }
    if (off_x < 0) off_x = 0;
    if (off_y < 0) off_y = 0;

    ppa_srm_oper_config_t op = {0};
    op.in.buffer         = src;
    op.in.pic_w          = sw;      op.in.pic_h  = sh;
    op.in.block_w        = crop_w;  op.in.block_h = crop_h;
    op.in.block_offset_x = 0;       op.in.block_offset_y = 0;
    op.in.srm_cm         = PPA_SRM_COLOR_MODE_RGB565;


    op.out.buffer        = dst->pixels;
    op.out.buffer_size   = (uint32_t)dst->w * dst->h * 2;
    op.out.pic_w         = dst->w;  op.out.pic_h = dst->h;
    op.out.block_offset_x = off_x;  op.out.block_offset_y = off_y;
    op.out.srm_cm        = PPA_SRM_COLOR_MODE_RGB565;

    op.byte_swap      = swap_bytes;      // top-level, not part of op.in
    // Four mountings, one PPA pass. Flipping costs nothing extra: the photo
    // already goes through a scale-and-rotate on its way to the frame buffer.
    op.rotation_angle = landscape
        ? (g_settings.rotate180 ? PPA_SRM_ROTATION_ANGLE_270 : PPA_SRM_ROTATION_ANGLE_90)
        : (g_settings.rotate180 ? PPA_SRM_ROTATION_ANGLE_180 : PPA_SRM_ROTATION_ANGLE_0);
    op.scale_x = scale;
    op.scale_y = scale;
    op.mode = PPA_TRANS_MODE_BLOCKING;

    return ppa_do_scale_rotate_mirror(s_ppa_srm, &op);
}

// Walk the segment chain in the FILE itself, seeking over each one, rather
// than sniffing a fixed-size head buffer.
//
// The first version read 1024 bytes and gave up. That works only for files
// whose EXIF happens to be small: a phone photo's APP1 segment is routinely
// tens of kilobytes (it carries an embedded thumbnail), which pushes the SOF
// marker well past any fixed head. Four perfectly good photos were rejected as
// "malformed" because of it. An APP1 segment can be 64 KB on its own and a
// file may carry several, so there is no head size that is both safe and
// cheap -- seeking is the answer.
photo_err_t photo_probe(const char *path, int *w, int *h)
{
    FILE *f = fopen(path, "rb");
    if (!f) return PHOTO_ERR_IO;

    uint8_t b[4];
    if (fread(b, 1, 2, f) != 2) { fclose(f); return PHOTO_ERR_MALFORMED; }

    if (b[0] == 0x89 && b[1] == 'P') { fclose(f); return PHOTO_ERR_NOT_JPEG; }
    if (b[0] != 0xFF || b[1] != 0xD8) { fclose(f); return PHOTO_ERR_NOT_JPEG; }

    for (;;) {
        int c = fgetc(f);
        if (c == EOF) { fclose(f); return PHOTO_ERR_MALFORMED; }
        if (c != 0xFF) continue;                   // resynchronise on a marker

        int m;
        do { m = fgetc(f); } while (m == 0xFF);    // fill bytes
        if (m == EOF) { fclose(f); return PHOTO_ERR_MALFORMED; }
        if (m == 0x01 || (m >= 0xD0 && m <= 0xD9)) continue;   // no length field

        if (fread(b, 1, 2, f) != 2) { fclose(f); return PHOTO_ERR_MALFORMED; }
        int seg = (b[0] << 8) | b[1];
        if (seg < 2) { fclose(f); return PHOTO_ERR_MALFORMED; }

        // SOFn, excluding DHT (C4), JPG (C8) and DAC (CC)
        if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
            if (m == 0xC2) { fclose(f); return PHOTO_ERR_PROGRESSIVE; }
            if (m != 0xC0 && m != 0xC1) { fclose(f); return PHOTO_ERR_NOT_JPEG; }
            uint8_t sof[5];
            if (fread(sof, 1, 5, f) != 5) { fclose(f); return PHOTO_ERR_MALFORMED; }
            if (h) *h = (sof[1] << 8) | sof[2];
            if (w) *w = (sof[3] << 8) | sof[4];
            fclose(f);
            return PHOTO_OK;
        }
        if (m == 0xDA) { fclose(f); return PHOTO_ERR_MALFORMED; }   // scan, no frame

        if (fseek(f, seg - 2, SEEK_CUR) != 0) { fclose(f); return PHOTO_ERR_MALFORMED; }
    }
}

photo_err_t photo_load(const char *path, frame_t *dst, bool landscape)
{
    size_t len = 0;
    int64_t t0 = esp_timer_get_time();
    uint8_t *jpg = read_file(path, &len);
    if (!jpg) return PHOTO_ERR_IO;
    int64_t t_read = esp_timer_get_time() - t0;

    int w = 0, h = 0;
    photo_err_t cls = sniff(jpg, len, &w, &h);
    if (cls != PHOTO_OK) { heap_caps_free(jpg); return cls; }

    // Fast path vs slow path, decided by the HW decoder's ceiling.
    const bool hw = (w <= JPEG_HW_MAX_DIM && h <= JPEG_HW_MAX_DIM);
    int dw = 0, dh = 0;
    t0 = esp_timer_get_time();
    uint16_t *raw = hw ? decode_hw(jpg, len, w, h, &dw, &dh)
                       : decode_sw(jpg, len, w, h, &dw, &dh);
    int64_t t_dec = esp_timer_get_time() - t0;
    heap_caps_free(jpg);
    if (!raw) return PHOTO_ERR_MEMORY;

    // The HW path returns a 16-px-padded buffer; crop back to the real image
    // so the padding never reaches the screen.
    const int crop_w = hw ? w : dw;
    const int crop_h = hw ? h : dh;

    t0 = esp_timer_get_time();
    // Only the hardware decoder needs the correction; esp_new_jpeg is asked
    // for RGB565_LE explicitly, which already matches the PPA.
    esp_err_t err = fit_into(raw, dw, dh, crop_w, crop_h, dst, landscape, hw);
    int64_t t_fit = esp_timer_get_time() - t0;

    if (hw) heap_caps_free(raw); else jpeg_free_align(raw);
    if (err != ESP_OK) { ESP_LOGE(TAG, "fit: %s", esp_err_to_name(err)); return PHOTO_ERR_MEMORY; }

    ESP_LOGI(TAG, "%s %dx%d [%s] read %lldms decode %lldms fit %lldms",
             path, w, h, hw ? "HW" : "SW",
             t_read / 1000, t_dec / 1000, t_fit / 1000);
    return PHOTO_OK;
}
