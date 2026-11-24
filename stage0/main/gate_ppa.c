// ===========================================================================
// gate_ppa.c -- Stage 0.4: PPA throughput
// ===========================================================================
// THE gate the transition catalogue depends on.
//
// The plan assumes every transition is a PPA operation on full-screen RGB565
// buffers in PSRAM, and that a crossfade can run smoothly. That assumption has
// one number behind it: how long a full-screen blend actually takes. Nothing
// here is guessed -- this gate measures it and the measured value sets each
// transition's duration and step count.
//
// Why crossfade is the expensive case, and the others are not:
//
//   crossfade   reads TWO full framebuffers, writes one   -> 3 x 1.95 MiB
//   slide/wipe  reads parts of two, writes one            -> 2 x 1.95 MiB
//
// So if crossfade lands at N fps, the geometric transitions land near 1.5N.
// A "pass" here is not a boolean; it is a table of microseconds that
// transitions.c reads its constants from.
#include <string.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "driver/ppa.h"
#include "board.h"

static const char *TAG = "gate0.4";

// Full-screen buffers, panel-native geometry. Allocated in PSRAM because that
// is where the real ones live -- benchmarking out of internal RAM would give a
// flattering number the application could never reproduce.
#define W   PANEL_H_RES
#define H   PANEL_V_RES
#define PX_BYTES 2

static void *buf_a, *buf_b, *buf_out;
static ppa_client_handle_t ppa_blend, ppa_srm;

// Report one operation as time, throughput, and -- the number that matters --
// the frame rate a transition made of this op would run at.
static void report(const char *what, int64_t us, size_t bytes_moved)
{
    double ms   = us / 1000.0;
    double mbps = bytes_moved / (us / 1e6) / (1024.0 * 1024.0);
    double fps  = 1e6 / (double)us;
    ESP_LOGI(TAG, "  %-28s %7.2f ms  %7.1f MiB/s  -> %5.1f fps", what, ms, mbps, fps);
}

// Average over several runs: PSRAM timing varies with cache state, and a
// single sample would be noise as often as signal.
#define REPS 5

static int64_t timed_blend(uint8_t alpha)
{
    // Fields are assigned rather than designated-initialised: the colour-mode
    // member sits in an anonymous union, and initialising it inline trips
    // -Wmissing-braces, which this project builds with -Werror.
    ppa_blend_oper_config_t cfg = {0};

    cfg.in_bg.buffer   = buf_a;
    cfg.in_bg.pic_w    = W;  cfg.in_bg.pic_h  = H;
    cfg.in_bg.block_w  = W;  cfg.in_bg.block_h = H;
    cfg.in_bg.blend_cm = PPA_BLEND_COLOR_MODE_RGB565;

    cfg.in_fg.buffer   = buf_b;
    cfg.in_fg.pic_w    = W;  cfg.in_fg.pic_h  = H;
    cfg.in_fg.block_w  = W;  cfg.in_fg.block_h = H;
    cfg.in_fg.blend_cm = PPA_BLEND_COLOR_MODE_RGB565;

    cfg.out.buffer      = buf_out;
    cfg.out.buffer_size = W * H * PX_BYTES;
    cfg.out.pic_w       = W;  cfg.out.pic_h = H;
    cfg.out.blend_cm    = PPA_BLEND_COLOR_MODE_RGB565;

    // A fixed foreground alpha is exactly how a crossfade ramps: step this
    // value 0 -> 255 across the transition and the blend does the rest.
    cfg.fg_alpha_update_mode = PPA_ALPHA_FIX_VALUE;
    cfg.fg_alpha_fix_val     = alpha;
    cfg.bg_alpha_update_mode = PPA_ALPHA_NO_CHANGE;
    cfg.mode                 = PPA_TRANS_MODE_BLOCKING;

    int64_t t0 = esp_timer_get_time();
    esp_err_t err = ppa_do_blend(ppa_blend, &cfg);
    int64_t dt = esp_timer_get_time() - t0;
    if (err != ESP_OK) { ESP_LOGE(TAG, "ppa_do_blend: %s", esp_err_to_name(err)); return -1; }
    return dt;
}

// One scale-rotate-mirror pass. Two things use this: the per-photo fit pass
// (cache -> screen geometry, including the landscape rotation) and the zoom
// transition. Rotation is measured separately because it is the operation the
// orientation setting depends on being cheap.
static int64_t timed_srm(float scale, ppa_srm_rotation_angle_t rot)
{
    // A 90/270 rotation swaps the output geometry -- which is exactly the
    // landscape case, so getting this right here is what proves the
    // orientation setting is cheap.
    bool swap = (rot == PPA_SRM_ROTATION_ANGLE_90 || rot == PPA_SRM_ROTATION_ANGLE_270);

    // Scaling UP must read a smaller source block, or the result does not fit
    // the output picture -- the PPA rejects that outright, which is how the
    // first run of this gate failed. A zoom transition works the same way:
    // take a centred block and blow it up to fill the screen.
    uint32_t bw = (uint32_t)(W / scale) & ~1u;
    uint32_t bh = (uint32_t)(H / scale) & ~1u;

    ppa_srm_oper_config_t cfg = {0};
    cfg.in.buffer  = buf_a;
    cfg.in.pic_w   = W;   cfg.in.pic_h   = H;
    cfg.in.block_w = bw;  cfg.in.block_h = bh;
    cfg.in.block_offset_x = (W - bw) / 2;
    cfg.in.block_offset_y = (H - bh) / 2;
    cfg.in.srm_cm  = PPA_SRM_COLOR_MODE_RGB565;

    cfg.out.buffer      = buf_out;
    cfg.out.buffer_size = W * H * PX_BYTES;
    cfg.out.pic_w       = swap ? H : W;
    cfg.out.pic_h       = swap ? W : H;
    cfg.out.srm_cm      = PPA_SRM_COLOR_MODE_RGB565;

    cfg.rotation_angle = rot;
    cfg.scale_x = scale;
    cfg.scale_y = scale;
    cfg.mode    = PPA_TRANS_MODE_BLOCKING;

    int64_t t0 = esp_timer_get_time();
    esp_err_t err = ppa_do_scale_rotate_mirror(ppa_srm, &cfg);
    int64_t dt = esp_timer_get_time() - t0;
    if (err != ESP_OK) { ESP_LOGE(TAG, "ppa_do_srm: %s", esp_err_to_name(err)); return -1; }
    return dt;
}

static int64_t avg(int64_t (*fn)(void))
{
    int64_t total = 0;
    for (int i = 0; i < REPS; i++) {
        int64_t r = fn();
        if (r < 0) return -1;
        total += r;
    }
    return total / REPS;
}

static int64_t f_blend(void)      { return timed_blend(128); }
// A blend with a fully-opaque foreground IS a copy. Gate 0.4's first run showed
// the blend engine running 2.8x faster per byte than SRM, so the geometric
// transitions (slide/wipe/push/blinds) -- which need no scaling at all -- may
// be far cheaper built this way than on ppa_do_scale_rotate_mirror.
static int64_t f_blend_copy(void) { return timed_blend(255); }
static int64_t f_srm_copy(void)   { return timed_srm(1.0f, PPA_SRM_ROTATION_ANGLE_0); }
static int64_t f_srm_rot90(void)  { return timed_srm(1.0f, PPA_SRM_ROTATION_ANGLE_90); }
static int64_t f_srm_zoom(void)   { return timed_srm(1.15f, PPA_SRM_ROTATION_ANGLE_0); }

void gate_ppa_run(void)
{
    ESP_LOGI(TAG, "=== Stage 0.4: PPA throughput (%dx%d RGB565, PSRAM) ===", W, H);

    const size_t fb = (size_t)W * H * PX_BYTES;
    ESP_LOGI(TAG, "framebuffer = %u bytes (%.2f MiB) each", (unsigned)fb, fb / 1048576.0);

    // PPA is a DMA master: its buffers must be PSRAM and cache-aligned.
    buf_a   = heap_caps_aligned_alloc(128, fb, MALLOC_CAP_SPIRAM);
    buf_b   = heap_caps_aligned_alloc(128, fb, MALLOC_CAP_SPIRAM);
    buf_out = heap_caps_aligned_alloc(128, fb, MALLOC_CAP_SPIRAM);
    if (!buf_a || !buf_b || !buf_out) {
        ESP_LOGE(TAG, "FAIL: could not allocate 3 x %.2f MiB in PSRAM "
                      "(is CONFIG_SPIRAM set, and did PSRAM enumerate?)", fb / 1048576.0);
        return;
    }
    memset(buf_a, 0x3A, fb);
    memset(buf_b, 0xC5, fb);

    ppa_client_config_t bc = { .oper_type = PPA_OPERATION_BLEND };
    ppa_client_config_t sc = { .oper_type = PPA_OPERATION_SRM };
    if (ppa_register_client(&bc, &ppa_blend) != ESP_OK ||
        ppa_register_client(&sc, &ppa_srm) != ESP_OK) {
        ESP_LOGE(TAG, "FAIL: ppa_register_client");
        return;
    }

    ESP_LOGI(TAG, "--- per-operation cost (mean of %d) ---", REPS);

    int64_t t_blend = avg(f_blend);
    if (t_blend > 0) report("blend fullscreen (crossfade)", t_blend, 3 * fb);

    int64_t t_copy = avg(f_srm_copy);
    if (t_copy > 0)  report("srm 1:1 copy (slide/wipe)",    t_copy, 2 * fb);

    int64_t t_rot = avg(f_srm_rot90);
    if (t_rot > 0)   report("srm rotate 90 (orientation)",  t_rot, 2 * fb);

    int64_t t_zoom = avg(f_srm_zoom);
    if (t_zoom > 0)  report("srm scale 1.15x (zoom)",       t_zoom, 2 * fb);

    int64_t t_bcopy = avg(f_blend_copy);
    if (t_bcopy > 0) report("blend alpha=255 (copy)",       t_bcopy, 3 * fb);

    if (t_bcopy > 0 && t_copy > 0) {
        ESP_LOGI(TAG, "--- how to build slides/wipes ---");
        if (t_bcopy < t_copy) {
            ESP_LOGI(TAG, "  blend-as-copy is %.2fx FASTER than SRM (%.1f vs %.1f fps).",
                     (double)t_copy / t_bcopy, 1e6 / t_bcopy, 1e6 / t_copy);
            ESP_LOGI(TAG, "  -> build slide/wipe/push/blinds on ppa_do_blend, NOT on SRM.");
        } else {
            ESP_LOGI(TAG, "  SRM is still faster for a straight copy (%.1f vs %.1f fps)",
                     1e6 / t_copy, 1e6 / t_bcopy);
            ESP_LOGI(TAG, "  -> keep the geometric transitions on SRM after all.");
        }
    }

    // ---- The verdict, in the terms the application actually needs ---------
    ESP_LOGI(TAG, "--- what this means for transitions ---");
    if (t_blend > 0) {
        double fps = 1e6 / t_blend;
        ESP_LOGI(TAG, "  crossfade sustains %.1f fps", fps);
        // 24 fps is the floor where motion still reads as motion rather than
        // as stepping. Below it, the plan's fallbacks apply.
        if (fps >= 30.0)      ESP_LOGI(TAG, "  VERDICT: comfortable. 600 ms crossfade = %d steps.", (int)(0.6 * fps));
        else if (fps >= 24.0) ESP_LOGW(TAG, "  VERDICT: usable. Keep crossfade <= 500 ms and prefer geometric transitions.");
        else                  ESP_LOGE(TAG, "  VERDICT: too slow for a full-screen crossfade. "
                                            "Fall back to half-vertical-resolution blend, or drop crossfade "
                                            "and lean on the write-only transitions.");
    }
    if (t_rot > 0) {
        // The orientation setting is only "free" if this is a once-per-photo
        // cost, not a once-per-frame one. State it plainly either way.
        ESP_LOGI(TAG, "  rotate 90 costs %.1f ms ONCE PER PHOTO -- orientation switching is %s",
                 t_rot / 1000.0, (t_rot < 50000) ? "free in practice" : "worth reconsidering");
    }

    ppa_unregister_client(ppa_blend);
    ppa_unregister_client(ppa_srm);
    heap_caps_free(buf_a); heap_caps_free(buf_b); heap_caps_free(buf_out);
    ESP_LOGI(TAG, "=== Stage 0.4 complete ===");
}
