// ===========================================================================
// main.c -- ESP32-P4 digital photo frame
// ===========================================================================
// Milestone: photos off the card, full screen, cycling with hardware-blitted
// transitions. Web UI, Immich and the overlay come next.
//
// Two Stage 0 measurements shape the loop below more than anything else:
//
//   0.4  Every transition runs on ppa_do_blend at ~54 fps. SRM would have been
//        29 fps for the same geometric work.
//   0.7  Decoding a 12 MP photo costs ~1.5 s and no amount of downscaling
//        changes that. So the next photo is decoded in a background task
//        DURING the current photo's dwell -- by the time the transition
//        starts, it is already sitting in PSRAM.
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/idf_additions.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_psram.h"
#include "esp_heap_caps.h"
#include "esp_random.h"

#include "board.h"
#include "board/display.h"
#include "board/sdcard.h"
#include "photo/photo.h"
#include "photo/playlist.h"
#include "photo/transitions.h"
#include "ui/overlay.h"
#include "settings.h"
#include "app_data.h"
#include "sun_moon.h"
#include "net/wifi.h"
#include "net/webui.h"
#include "net/weather.h"
#include "net/airquality.h"
#include "net/immich.h"
#include "board/touch.h"

AppData g_data;

static const char *TAG = "frame";

// ---- settings that will move to NVS ---------------------------------------

// Filled from NVS at boot -- see overlay_config_from_settings().
static overlay_cfg_t s_overlay;

// The saved mask is the source of truth; the checkbox order on the settings
// page matches the OV_* order exactly so the two never drift.
static void overlay_config_from_settings(void)
{
    for (int i = 0; i < OV_ITEM_COUNT; i++)
        s_overlay.enabled[i] = (g_settings.ov_items >> i) & 1;
    s_overlay.opacity = g_settings.ov_opacity ? g_settings.ov_opacity : 255;
    s_overlay.page_ms = g_settings.ov_page_ms ? g_settings.ov_page_ms : 10000;
}
static const uint32_t CFG_TRANS_MS    = 700;
static const char    *CFG_PHOTO_ROOT  = BSP_SD_MOUNT;

// Run every transition once, back to back, and print the frame rate of each.
// Useful when tuning the compositor; OFF by default because it blocks the
// whole startup path for about twelve seconds.
//
// Nothing else runs while it does: no overlay refresh, and no weather or air
// quality fetch. That delay is why the frame showed "no network" for the first
// twenty-odd seconds of every boot -- the radio was up at 7 s, but the first
// fetch could not happen until the benchmark finished.
#define CFG_BENCH_TRANSITIONS  0

// ---- the two display-ready frames -----------------------------------------
static frame_t s_front, s_back;

// The loader fills s_back while s_front is on screen. These two say whether
// it succeeded and whether the compositor has consumed it yet.
static SemaphoreHandle_t s_need_load;    // compositor -> loader: go
static SemaphoreHandle_t s_load_done;    // loader -> compositor: ready
static volatile bool     s_load_ok;
static volatile bool     s_want_prev;   // set by a double tap

// Decide where the next photo comes from. Immich is only ever chosen when it
// is actually usable, so an unreachable server degrades to the card rather
// than to a blank frame -- the same last-good rule the weather layer follows.
static bool pick_immich(void)
{
    if (!immich_configured() || !wifi_ready()) return false;
    switch (g_settings.source_mode) {
        case SOURCE_IMMICH_ONLY: return true;
        case SOURCE_BOTH:        return (esp_random() % 100) >= g_settings.sd_weight;
        default:                 return false;
    }
}

// Weather and air quality do BLOCKING HTTPS fetches -- one to two seconds
// normally, up to fifteen on a timeout. On the render loop that froze the
// whole frame, gestures included, every fifteen minutes. They get their own
// task; they still run sequentially with each other, so there is never more
// than one TLS session alive.
static void net_task(void *arg)
{
    (void)arg;
    for (;;) {
        weather_tick();
        airquality_tick();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void loader_task(void *arg)
{
    (void)arg;
    for (;;) {
        xSemaphoreTake(s_need_load, portMAX_DELAY);

        s_load_ok = false;
        // Try a few candidates: a single unreadable file must not stall the
        // slideshow, and the playlist can only screen headers, not decodes.
        for (int attempt = 0; attempt < 4 && !s_load_ok; attempt++) {
            char immich_path[384];
            const char *p = NULL;

            if (s_want_prev) {
                // Going back is always within the card's history: an Immich
                // photo that was never cached cannot be revisited.
                p = playlist_prev();
                if (!p) p = playlist_next();  // nothing older; carry on forwards
            } else {
                if (pick_immich() && immich_next_photo(immich_path, sizeof(immich_path)))
                    p = immich_path;
                if (!p) p = playlist_next();  // fall back to the card
            }
            if (!p) break;

            photo_err_t e = photo_load(p, &s_back, g_settings.landscape);
            if (e == PHOTO_OK) s_load_ok = true;
            else ESP_LOGW(TAG, "load %s: %s", p, photo_err_str(e));
        }
        xSemaphoreGive(s_load_done);
    }
}

static void swap_frames(void)
{
    frame_t t = s_front; s_front = s_back; s_back = t;
}

void app_main(void)
{
    ESP_LOGI(TAG, "%s", BOARD_NAME);

    if (!esp_psram_is_initialized()) {
        ESP_LOGE(TAG, "PSRAM did not initialise -- nothing here can work.");
        return;
    }

    settings_begin();
    touch_bus_init();          // must precede the panel -- see touch.h
    ESP_ERROR_CHECK(display_init());
    // After the panel: the touch controller shares its supply and is held in
    // reset until then. Not fatal if it fails -- the frame is still a frame.
    if (touch_init() != ESP_OK) ESP_LOGW(TAG, "no touch; gestures unavailable");
    ESP_ERROR_CHECK(transitions_init());

    if (sdcard_mount() != ESP_OK) {
        ESP_LOGE(TAG, "no card -- nothing to show yet.");
        return;
    }

    const int W = display_width(), H = display_height();
    ESP_ERROR_CHECK(frame_alloc(&s_front, W, H));
    ESP_ERROR_CHECK(frame_alloc(&s_back,  W, H));
    ESP_LOGI(TAG, "PSRAM free after buffers: %.1f MiB",
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1048576.0);

    overlay_config_from_settings();
    ESP_ERROR_CHECK(overlay_init(W, H, &s_overlay));

    // Network is optional by design: a frame with no network is still a frame.
    // Photos play, and sun/sunset stay right because they are local math.
    if (wifi_begin() == ESP_OK) {
        bool connected = false;
        if (wifi_connect() == ESP_OK) {
            // Give it a window to associate before deciding it has failed.
            for (int i = 0; i < 120 && !wifi_ready(); i++) vTaskDelay(pdMS_TO_TICKS(100));
            connected = wifi_ready();
        }

        if (connected) {
            wifi_start_sntp();
            webui_begin();
        } else {
            // Unconfigured, or configured for a network that is not here.
            // Either way the answer is the same: put up the setup portal so
            // the frame can be fixed without a serial cable. It keeps
            // retrying the real network in the background.
            ESP_LOGW(TAG, "not connected -- starting the setup portal");
            webui_start_setup_ap();
            webui_begin();
        }
    } else {
        ESP_LOGW(TAG, "no co-processor link -- running offline");
    }
    weather_begin();
    airquality_begin();
    sunmoon_recompute();

    if (playlist_scan(CFG_PHOTO_ROOT) == 0 && !immich_configured()) {
        ESP_LOGE(TAG, "no usable photos under %s (%d rejected) and no Immich server",
                 CFG_PHOTO_ROOT, playlist_rejected());
        return;
    }

    s_need_load = xSemaphoreCreateBinary();
    s_load_done = xSemaphoreCreateBinary();
    // Core 0, low priority: it must never starve the compositor on core 1.
    // Stacks in PSRAM, not internal RAM.
    //
    // Task stacks default to internal RAM, and these three came to 20 KB of
    // it. Internal RAM is what the ESP32-C6 link allocates its receive
    // buffers from, and starving it makes the radio drop packets
    // ("mempool OOM (RX)", "dma_alloc(1536) failed") -- which presents as no
    // DNS and an unreachable web server, with nothing pointing at task
    // creation. None of these tasks runs with the cache disabled or touches
    // DMA from its stack, so PSRAM is safe for all three.
    xTaskCreatePinnedToCoreWithCaps(loader_task, "loader", 8192, NULL, 3, NULL, 0,
                                    MALLOC_CAP_SPIRAM);
    xTaskCreatePinnedToCoreWithCaps(net_task,    "net",    8192, NULL, 2, NULL, 0,
                                    MALLOC_CAP_SPIRAM);

    // First photo, drawn without a transition -- there is nothing to come from.
    xSemaphoreGive(s_need_load);
    xSemaphoreTake(s_load_done, portMAX_DELAY);
    if (!s_load_ok) { ESP_LOGE(TAG, "could not load any photo"); return; }
    swap_frames();

    void *fb = display_framebuffer(0);
    int   fb_index = 0;
    memcpy(fb, s_front.pixels, (size_t)W * H * 2);
    {
        frame_t t = { .pixels = fb, .w = W, .h = H };
        if (overlay_visible())
            transition_overlay(overlay_pixels(), overlay_stride(),
                               overlay_x(), overlay_y(),
                               overlay_width(), overlay_height(),
                               s_overlay.opacity, &t);
    }
    display_present(fb);

    // Start the next decode immediately: it has the whole dwell to finish.
    xSemaphoreGive(s_need_load);

#if CFG_BENCH_TRANSITIONS
    // Wait for the second photo so there is a real pair to transition between.
    xSemaphoreTake(s_load_done, portMAX_DELAY);
    if (s_load_ok) {
        ESP_LOGI(TAG, "=== transition benchmark (%dx%d, %d ms each) ===", W, H, CFG_TRANS_MS);
        double worst = 1e9; const char *worst_name = "";
        for (int t = 0; t < TR_COUNT; t++) {
            int64_t t0 = esp_timer_get_time();
            int frames = 0;
            // Split the two costs. If render is the larger, we are compute
            // bound and the region maths still has slack in it. If present is,
            // we are waiting on the panel and no amount of PPA tuning helps.
            int64_t t_render = 0, t_present = 0;
            for (;;) {
                float p = (float)(esp_timer_get_time() - t0) / (CFG_TRANS_MS * 1000.0f);
                if (p > 1.0f) p = 1.0f;
                fb_index ^= 1;
                frame_t target = { .pixels = display_framebuffer(fb_index), .w = W, .h = H };

                int64_t a = esp_timer_get_time();
                transition_step((transition_t)t, &s_front, &s_back, p, &target);
                int64_t b = esp_timer_get_time();
                display_present(target.pixels);
                int64_t c = esp_timer_get_time();
                t_render += b - a; t_present += c - b;

                frames++;
                if (p >= 1.0f) break;
            }
            int64_t dt = esp_timer_get_time() - t0;
            double fps = frames * 1e6 / dt;
            ESP_LOGI(TAG, "  %-16s %2d fr %4lld ms  %5.1f fps   render %5.2f ms  present %5.2f ms",
                     transition_name((transition_t)t), frames, dt / 1000, fps,
                     t_render / 1000.0 / frames, t_present / 1000.0 / frames);
            if (fps < worst) { worst = fps; worst_name = transition_name((transition_t)t); }
            // Alternate direction so each run starts from the other photo.
            swap_frames();
            vTaskDelay(pdMS_TO_TICKS(250));
        }
        ESP_LOGI(TAG, "  slowest: %s at %.1f fps", worst_name, worst);
        ESP_LOGI(TAG, "=== benchmark done, entering slideshow ===");
    }
    xSemaphoreGive(s_need_load);
#endif

    for (;;) {
        // Hold the photo, but keep the overlay live: the clock ticks, pages
        // rotate, and fetched values land. Only the strip is recomposited, so
        // this costs about a fifth of a frame each time.
        int64_t hold_until = esp_timer_get_time() + (int64_t)(g_settings.dwell_s * 1000u) * 1000;
        bool advance_now = false;
        while (esp_timer_get_time() < hold_until && !advance_now) {
            // Gestures are checked far more often than the strip is redrawn,
            // so a tap feels immediate rather than landing on the next
            // 200 ms tick.
            gesture_t g;
            while ((g = touch_take()) != GESTURE_NONE) {
                ESP_LOGI(TAG, "gesture: %s", gesture_name(g));
                switch (g) {
                    case GESTURE_TAP:        advance_now = true; break;
                    case GESTURE_DOUBLE_TAP: s_want_prev = true; advance_now = true; break;
                    case GESTURE_SWIPE_DOWN: overlay_set_hidden(true);  break;
                    case GESTURE_SWIPE_UP:   overlay_set_hidden(false); break;
                    default: break;
                }
            }
            if (advance_now) break;
            // 10 ms, and no unconditional repaint below: the queue is drained
            // ~100 times a second instead of ~14, which is the difference
            // between a tap feeling instant and feeling sticky.
            vTaskDelay(pdMS_TO_TICKS(10));
            const bool ov_dirty = overlay_tick();
            // Sun times need a real date. Recompute once, the moment SNTP
            // lands, rather than leaving 1970's sunrise on screen.
            static bool ap_dropped;
            if (!ap_dropped && webui_is_ap() && wifi_ready()) {
                webui_stop_setup_ap();
                wifi_start_sntp();
                ap_dropped = true;
            }
            static bool sun_done;
            if (!sun_done && wifi_time_valid()) { sunmoon_recompute(); sun_done = true; }
            // Redraw whenever the overlay is up, AND once more on the frame
            // it is hidden. Skipping the redraw entirely when hidden left the
            // last composited frame on screen -- panel and all -- so a swipe
            // down appeared to do nothing at all.
            static bool was_visible = true;
            const bool vis = overlay_visible();
            if (ov_dirty || vis != was_visible) {
                fb_index ^= 1;
                frame_t t = { .pixels = display_framebuffer(fb_index), .w = W, .h = H };
                memcpy(t.pixels, s_front.pixels, (size_t)W * H * 2);
                if (vis)
                    transition_overlay(overlay_pixels(), overlay_stride(),
                                       overlay_x(), overlay_y(),
                                       overlay_width(), overlay_height(),
                                       s_overlay.opacity, &t);
                display_present(t.pixels);
                was_visible = vis;
            }
        }

        // The loader has had the full dwell. If it is still going, wait --
        // better a longer dwell than a torn or empty frame.
        int64_t t_wait = esp_timer_get_time();
        xSemaphoreTake(s_load_done, portMAX_DELAY);
        t_wait = esp_timer_get_time() - t_wait;
        if (t_wait > 100000) {
            ESP_LOGW(TAG, "waited %lld ms on the decoder -- dwell was too short",
                     t_wait / 1000);
        }
        if (!s_load_ok) { xSemaphoreGive(s_need_load); continue; }

        transition_t tr = (transition_t)(esp_random() % TR_COUNT);

        // Render into the framebuffer the panel is NOT scanning.
        int64_t t0 = esp_timer_get_time();
        int frames = 0;
        for (;;) {
            float p = (float)(esp_timer_get_time() - t0) / (CFG_TRANS_MS * 1000.0f);
            if (p > 1.0f) p = 1.0f;

            fb_index ^= 1;
            frame_t target = { .pixels = display_framebuffer(fb_index), .w = W, .h = H };
            transition_step(tr, &s_front, &s_back, p, &target);
            display_present(target.pixels);
            frames++;

            if (p >= 1.0f) break;
        }
        int64_t dt = esp_timer_get_time() - t0;
        ESP_LOGI(TAG, "%-14s %2d frames in %4lld ms = %.1f fps",
                 transition_name(tr), frames, dt / 1000, frames * 1e6 / dt);

        swap_frames();
        overlay_next_page();              // the strip moves with the picture
        s_want_prev = false;
        xSemaphoreGive(s_need_load);      // decode the next one during the dwell
    }
}
