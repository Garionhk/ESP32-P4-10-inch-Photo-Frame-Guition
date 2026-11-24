#include "transitions.h"
#include "board.h"
#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "driver/ppa.h"

static const char *TAG = "trans";
static ppa_client_handle_t s_blend;

// ---------------------------------------------------------------------------
// Blend the ARGB8888 overlay strip over a finished frame.
//
// This is where the frosted bar actually becomes translucent: the PPA blends
// using the layer's own per-pixel alpha, so the photo shows through the panel
// rather than being covered by it. It touches only the bottom fifth --
// 800x256 rather than 800x1280 -- so it costs about a fifth of a full blend.
// ---------------------------------------------------------------------------
esp_err_t transition_overlay(const void *argb, int stride, int ov_x, int ov_y,
                             int ov_w, int ov_h, uint8_t opacity, frame_t *dst)
{
    (void)stride;
    if (!argb || ov_h <= 0) return ESP_OK;

    ppa_blend_oper_config_t op = {0};
    // Background: the photo, in place, at the strip's rows.
    op.in_bg.buffer = dst->pixels;
    op.in_bg.pic_w = dst->w;  op.in_bg.pic_h = dst->h;
    op.in_bg.block_w = ov_w;  op.in_bg.block_h = ov_h;
    op.in_bg.block_offset_x = ov_x;
    op.in_bg.block_offset_y = ov_y;
    op.in_bg.blend_cm = PPA_BLEND_COLOR_MODE_RGB565;

    // Foreground: the overlay, carrying its own alpha channel.
    op.in_fg.buffer = argb;
    op.in_fg.pic_w = ov_w;    op.in_fg.pic_h = ov_h;
    op.in_fg.block_w = ov_w;  op.in_fg.block_h = ov_h;
    op.in_fg.blend_cm = PPA_BLEND_COLOR_MODE_ARGB8888;

    op.out.buffer = dst->pixels;
    op.out.buffer_size = (uint32_t)dst->w * dst->h * 2;
    op.out.pic_w = dst->w;    op.out.pic_h = dst->h;
    op.out.block_offset_x = ov_x;
    op.out.block_offset_y = ov_y;
    op.out.blend_cm = PPA_BLEND_COLOR_MODE_RGB565;

    if (opacity >= 255) {
        // Use the layer's per-pixel alpha as authored.
        op.fg_alpha_update_mode = PPA_ALPHA_NO_CHANGE;
    } else {
        // Scale every pixel's alpha, so the opacity setting dims the whole bar
        // without flattening the text against the background.
        op.fg_alpha_update_mode = PPA_ALPHA_SCALE;
        op.fg_alpha_scale_ratio = opacity / 255.0f;
    }
    op.bg_alpha_update_mode = PPA_ALPHA_NO_CHANGE;
    op.mode = PPA_TRANS_MODE_BLOCKING;

    return ppa_do_blend(s_blend, &op);
}

esp_err_t transitions_init(void)
{
    if (s_blend) return ESP_OK;
    ppa_client_config_t c = { .oper_type = PPA_OPERATION_BLEND };
    return ppa_register_client(&c, &s_blend);
}

const char *transition_name(transition_t t)
{
    static const char *n[TR_COUNT] = {
        "crossfade", "slide-left", "slide-right", "slide-up", "slide-down",
        "push-left", "wipe-left", "barn-open", "blinds", "dissolve-bands",
    };
    return (t < TR_COUNT) ? n[t] : "?";
}

// ---------------------------------------------------------------------------
// The two primitives every transition is made of.
// ---------------------------------------------------------------------------

// Copy a rectangle from src to dst. Implemented as a blend with an opaque
// foreground because that path is 1.87x faster than SRM (gate 0.4).
// Background is pointed at the source too: with fg alpha 255 the background
// contributes nothing, so this avoids a second real read of the destination.
static esp_err_t blit(const frame_t *src, int sx, int sy,
                      frame_t *dst, int dx, int dy, int w, int h)
{
    if (w <= 0 || h <= 0) return ESP_OK;

    // Clamp to both surfaces; a transition that walks a rectangle off the edge
    // should shrink it, not fail.
    if (sx < 0) { w += sx; dx -= sx; sx = 0; }
    if (sy < 0) { h += sy; dy -= sy; sy = 0; }
    if (dx < 0) { w += dx; sx -= dx; dx = 0; }
    if (dy < 0) { h += dy; sy -= dy; dy = 0; }
    if (sx + w > src->w) w = src->w - sx;
    if (sy + h > src->h) h = src->h - sy;
    if (dx + w > dst->w) w = dst->w - dx;
    if (dy + h > dst->h) h = dst->h - dy;
    if (w <= 0 || h <= 0) return ESP_OK;

    ppa_blend_oper_config_t op = {0};
    op.in_bg.buffer = src->pixels;
    op.in_bg.pic_w = src->w;  op.in_bg.pic_h = src->h;
    op.in_bg.block_w = w;     op.in_bg.block_h = h;
    op.in_bg.block_offset_x = sx; op.in_bg.block_offset_y = sy;
    op.in_bg.blend_cm = PPA_BLEND_COLOR_MODE_RGB565;

    op.in_fg.buffer = src->pixels;
    op.in_fg.pic_w = src->w;  op.in_fg.pic_h = src->h;
    op.in_fg.block_w = w;     op.in_fg.block_h = h;
    op.in_fg.block_offset_x = sx; op.in_fg.block_offset_y = sy;
    op.in_fg.blend_cm = PPA_BLEND_COLOR_MODE_RGB565;

    op.out.buffer = dst->pixels;
    op.out.buffer_size = (uint32_t)dst->w * dst->h * 2;
    op.out.pic_w = dst->w;    op.out.pic_h = dst->h;
    op.out.block_offset_x = dx; op.out.block_offset_y = dy;
    op.out.blend_cm = PPA_BLEND_COLOR_MODE_RGB565;

    op.fg_alpha_update_mode = PPA_ALPHA_FIX_VALUE;
    op.fg_alpha_fix_val     = 255;              // opaque: the blend is a copy
    op.bg_alpha_update_mode = PPA_ALPHA_NO_CHANGE;
    op.mode = PPA_TRANS_MODE_BLOCKING;

    return ppa_do_blend(s_blend, &op);
}

// Mix two full frames with a fixed foreground alpha.
static esp_err_t mix(const frame_t *bg, const frame_t *fg, uint8_t alpha, frame_t *dst)
{
    ppa_blend_oper_config_t op = {0};
    op.in_bg.buffer = bg->pixels;
    op.in_bg.pic_w = bg->w; op.in_bg.pic_h = bg->h;
    op.in_bg.block_w = bg->w; op.in_bg.block_h = bg->h;
    op.in_bg.blend_cm = PPA_BLEND_COLOR_MODE_RGB565;

    op.in_fg.buffer = fg->pixels;
    op.in_fg.pic_w = fg->w; op.in_fg.pic_h = fg->h;
    op.in_fg.block_w = fg->w; op.in_fg.block_h = fg->h;
    op.in_fg.blend_cm = PPA_BLEND_COLOR_MODE_RGB565;

    op.out.buffer = dst->pixels;
    op.out.buffer_size = (uint32_t)dst->w * dst->h * 2;
    op.out.pic_w = dst->w; op.out.pic_h = dst->h;
    op.out.blend_cm = PPA_BLEND_COLOR_MODE_RGB565;

    op.fg_alpha_update_mode = PPA_ALPHA_FIX_VALUE;
    op.fg_alpha_fix_val     = alpha;
    op.bg_alpha_update_mode = PPA_ALPHA_NO_CHANGE;
    op.mode = PPA_TRANS_MODE_BLOCKING;

    return ppa_do_blend(s_blend, &op);
}

// Blend a horizontal strip of two frames. Same as mix() but bounded to
// [y, y+h), so a staggered dissolve costs one operation per band.
static esp_err_t mix_band(const frame_t *bg, const frame_t *fg, uint8_t alpha,
                          frame_t *dst, int y, int h)
{
    if (h <= 0) return ESP_OK;

    ppa_blend_oper_config_t op = {0};
    op.in_bg.buffer = bg->pixels;
    op.in_bg.pic_w = bg->w; op.in_bg.pic_h = bg->h;
    op.in_bg.block_w = bg->w; op.in_bg.block_h = h;
    op.in_bg.block_offset_y = y;
    op.in_bg.blend_cm = PPA_BLEND_COLOR_MODE_RGB565;

    op.in_fg.buffer = fg->pixels;
    op.in_fg.pic_w = fg->w; op.in_fg.pic_h = fg->h;
    op.in_fg.block_w = fg->w; op.in_fg.block_h = h;
    op.in_fg.block_offset_y = y;
    op.in_fg.blend_cm = PPA_BLEND_COLOR_MODE_RGB565;

    op.out.buffer = dst->pixels;
    op.out.buffer_size = (uint32_t)dst->w * dst->h * 2;
    op.out.pic_w = dst->w; op.out.pic_h = dst->h;
    op.out.block_offset_y = y;
    op.out.blend_cm = PPA_BLEND_COLOR_MODE_RGB565;

    op.fg_alpha_update_mode = PPA_ALPHA_FIX_VALUE;
    op.fg_alpha_fix_val     = alpha;
    op.bg_alpha_update_mode = PPA_ALPHA_NO_CHANGE;
    op.mode = PPA_TRANS_MODE_BLOCKING;

    return ppa_do_blend(s_blend, &op);
}

// Ease in/out. Linear motion reads as mechanical on a photo frame; this is the
// difference between "a slideshow" and "a product".
static float ease(float p)
{
    return (p < 0.5f) ? (2.0f * p * p) : (1.0f - 2.0f * (1.0f - p) * (1.0f - p));
}

// ---------------------------------------------------------------------------
// THE RULE every transition below obeys: the regions written must TILE the
// screen exactly once. No transition may blit the whole outgoing frame and
// then paint the incoming one on top.
//
// That naive form is what the first version did, and it cost most of the frame
// budget. Measured on hardware:
//
//   push-left  (already exact)          34.4 fps
//   slide-*    (full bg + incoming)     23-25 fps
//   blinds     (full bg + 10 bands)     24.5 fps
//
// A slide drawing a full background plus the incoming rect writes ~1.5 screens
// per frame; blinds wrote up to 2. Push-left was the only one writing exactly
// one, and it was the only one hitting its predicted rate. The hardware was
// never the problem.
//
// Double buffering is why this cannot be solved by drawing incrementally: the
// buffer being rendered into was last written TWO frames ago, so every frame
// must fully define the screen on its own.
// ---------------------------------------------------------------------------
esp_err_t transition_step(transition_t t, const frame_t *from, const frame_t *to,
                          float p, frame_t *dst)
{
    if (p < 0) p = 0;
    if (p > 1) p = 1;
    const int W = dst->w, H = dst->h;
    const float e = ease(p);

    switch (t) {
    case TR_CROSSFADE:
        // Genuinely needs every pixel of both inputs; one full-screen blend.
        return mix(from, to, (uint8_t)(e * 255.0f), dst);

    // ---- slides: outgoing holds still, incoming slides over it ------------
    case TR_SLIDE_LEFT: {
        int x = (int)(W * (1.0f - e)) & ~1;              // incoming covers [x, W)
        ESP_RETURN_ON_ERROR(blit(from, 0, 0, dst, 0, 0, x, H), TAG, "sl from");
        return blit(to, 0, 0, dst, x, 0, W - x, H);
    }
    case TR_SLIDE_RIGHT: {
        int x = (int)(W * (1.0f - e)) & ~1;              // incoming covers [0, W-x)
        ESP_RETURN_ON_ERROR(blit(to, x, 0, dst, 0, 0, W - x, H), TAG, "sr to");
        return blit(from, W - x, 0, dst, W - x, 0, x, H);
    }
    case TR_SLIDE_UP: {
        int y = (int)(H * (1.0f - e)) & ~1;
        ESP_RETURN_ON_ERROR(blit(from, 0, 0, dst, 0, 0, W, y), TAG, "su from");
        return blit(to, 0, 0, dst, 0, y, W, H - y);
    }
    case TR_SLIDE_DOWN: {
        int y = (int)(H * (1.0f - e)) & ~1;
        ESP_RETURN_ON_ERROR(blit(to, 0, y, dst, 0, 0, W, H - y), TAG, "sd to");
        return blit(from, 0, H - y, dst, 0, H - y, W, y);
    }

    // ---- push: both frames travel together --------------------------------
    case TR_PUSH_LEFT: {
        int x = (int)(W * e) & ~1;
        ESP_RETURN_ON_ERROR(blit(from, x, 0, dst, 0, 0, W - x, H), TAG, "push out");
        return blit(to, 0, 0, dst, W - x, 0, x, H);
    }

    // ---- wipe: hard edge sweeping across ----------------------------------
    case TR_WIPE_LEFT: {
        int x = (int)(W * e) & ~1;
        ESP_RETURN_ON_ERROR(blit(to, 0, 0, dst, 0, 0, x, H), TAG, "wipe to");
        return blit(from, x, 0, dst, x, 0, W - x, H);
    }

    // ---- barn door: three stripes, no overlap ------------------------------
    case TR_BARN_OPEN: {
        int half = (int)((W / 2) * e) & ~1;
        int l = W / 2 - half, r = W / 2 + half;
        ESP_RETURN_ON_ERROR(blit(from, 0, 0, dst, 0, 0, l, H), TAG, "barn l");
        ESP_RETURN_ON_ERROR(blit(to, l, 0, dst, l, 0, r - l, H), TAG, "barn c");
        return blit(from, r, 0, dst, r, 0, W - r, H);
    }

    // ---- blinds: each band is incoming-on-top, outgoing-below --------------
    // Eight bands rather than ten: every band costs two PPA round-trips, and
    // past a point the per-operation overhead outweighs the effect.
    case TR_BLINDS: {
        const int bands = 8;
        const int bh = ((H / bands) + 1) & ~1;
        int grow = (int)(bh * e) & ~1;
        for (int i = 0; i < bands; i++) {
            int y = i * bh;
            if (y >= H) break;
            int hh = (y + bh > H) ? (H - y) : bh;
            int g = (grow > hh) ? hh : grow;
            if (g > 0)
                ESP_RETURN_ON_ERROR(blit(to, 0, y, dst, 0, y, W, g), TAG, "blind to");
            if (hh - g > 0)
                ESP_RETURN_ON_ERROR(blit(from, 0, y + g, dst, 0, y + g, W, hh - g), TAG, "blind from");
        }
        return ESP_OK;
    }

    // ---- dissolve bands: staggered cross-fade, one op per band ------------
    // A true per-pixel dissolve would need a mask the PPA cannot express;
    // staggering the start of each band sells the same idea for one blend each.
    case TR_DISSOLVE_BANDS: {
        const int bands = 12;
        const int bh = ((H / bands) + 1) & ~1;
        for (int i = 0; i < bands; i++) {
            int y = i * bh;
            if (y >= H) break;
            int hh = (y + bh > H) ? (H - y) : bh;

            float local = (e * (bands + 4) - i) / 4.0f;
            if (local < 0) local = 0;
            if (local > 1) local = 1;

            if (local <= 0.0f) {
                ESP_RETURN_ON_ERROR(blit(from, 0, y, dst, 0, y, W, hh), TAG, "diss from");
            } else if (local >= 1.0f) {
                ESP_RETURN_ON_ERROR(blit(to, 0, y, dst, 0, y, W, hh), TAG, "diss to");
            } else {
                ESP_RETURN_ON_ERROR(mix_band(from, to, (uint8_t)(local * 255.0f), dst, y, hh),
                                    TAG, "diss mix");
            }
        }
        return ESP_OK;
    }

    default:
        return mix(from, to, (uint8_t)(e * 255.0f), dst);
    }
}
