// ===========================================================================
// gate_display.c -- Stage 0.1: JD9365 MIPI-DSI panel bring-up
// ===========================================================================
// Produces the panel configuration the firmware then uses verbatim, and
// settles the ONE pin this board family does not agree on.
//
// Waveshare's own BSPs use different backlight/reset pins per model (26/27 on
// the 3.4"/4" board, 32/33 on the 7"), and no BSP for the 10.1" is published.
// board.h therefore carries the 26/27 pair as a hypothesis. This gate walks
// the candidates so a black screen resolves to a pin number rather than to a
// shrug.
//
// The failure this is designed to catch: DSI initialises cleanly, the log
// looks perfect, and the screen stays dark because the backlight was never
// driven. That is indistinguishable from a dead panel unless you test for it.
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_ldo_regulator.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "driver/gpio.h"
#include "esp_lcd_jd9365_10_1.h"
#include "board.h"

static const char *TAG = "gate0.1";

// Candidate backlight pins. These are the ONLY two that appear as a backlight
// in any Waveshare P4 BSP -- 26 on the 3.4"/4" board, 32 on the 7". Guessing
// further pins was tempting and is a bad idea: driving an unknown GPIO high
// can fight whatever is actually attached to it. If neither of these lights
// the panel, the answer comes from the schematic, not from poking.
// No longer a hunt: the JC8012P4A1 BSP gives the backlight as GPIO 23.
// Kept as a list only so a wrong board shows up as a black screen again
// rather than as a silent success.
static const gpio_num_t BL_CANDIDATES[] = { BSP_LCD_BACKLIGHT };

static esp_lcd_panel_handle_t panel;
static esp_lcd_dsi_bus_handle_t dsi_bus;
static esp_lcd_panel_io_handle_t panel_io;

// Fill the active framebuffer with a flat RGB565 colour.
static void fill(uint16_t colour)
{
    void *fb = NULL;
    esp_err_t err = esp_lcd_dpi_panel_get_frame_buffer(panel, 1, &fb);
    if (err != ESP_OK || !fb) {
        // Returning silently here made a failed framebuffer fetch look
        // identical to a dark backlight. Never again.
        ESP_LOGE(TAG, "fill: get_frame_buffer failed (%s) -- NO PIXELS WRITTEN",
                 esp_err_to_name(err));
        return;
    }
    uint16_t *p = (uint16_t *)fb;
    const size_t n = (size_t)PANEL_H_RES * PANEL_V_RES;
    for (size_t i = 0; i < n; i++) p[i] = colour;
    esp_lcd_panel_draw_bitmap(panel, 0, 0, PANEL_H_RES, PANEL_V_RES, fb);
}

esp_lcd_panel_handle_t gate_display_panel(void) { return panel; }

void gate_display_run(void)
{
    ESP_LOGI(TAG, "=== Stage 0.1: JD9365 %dx%d MIPI-DSI, %d lanes @ %d Mbps ===",
             PANEL_H_RES, PANEL_V_RES, PANEL_DSI_LANES, PANEL_DSI_LANE_MBPS);

    // The DSI PHY is powered from an internal LDO, not from the main rail.
    // Skip this and the bus initialises into silence.
    esp_ldo_channel_handle_t ldo = NULL;
    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id = PANEL_MIPI_LDO_CHAN,
        .voltage_mv = PANEL_MIPI_LDO_MV,
    };
    if (esp_ldo_acquire_channel(&ldo_cfg, &ldo) != ESP_OK) {
        ESP_LOGE(TAG, "FAIL: MIPI DPHY LDO channel %d", PANEL_MIPI_LDO_CHAN);
        return;
    }
    ESP_LOGI(TAG, "  DPHY LDO ch%d at %d mV", PANEL_MIPI_LDO_CHAN, PANEL_MIPI_LDO_MV);

    esp_lcd_dsi_bus_config_t bus_cfg = JD9365_PANEL_BUS_DSI_2CH_CONFIG();
    if (esp_lcd_new_dsi_bus(&bus_cfg, &dsi_bus) != ESP_OK) {
        ESP_LOGE(TAG, "FAIL: esp_lcd_new_dsi_bus"); return;
    }

    esp_lcd_dbi_io_config_t io_cfg = JD9365_PANEL_IO_DBI_CONFIG();
    if (esp_lcd_new_panel_io_dbi(dsi_bus, &io_cfg, &panel_io) != ESP_OK) {
        ESP_LOGE(TAG, "FAIL: esp_lcd_new_panel_io_dbi"); return;
    }

    // The vendor macro ships num_fbs = 1. We override to 2 so the compositor
    // can build the next frame while the DSI scans the current one -- that is
    // what makes tear-free transitions possible at all.
    esp_lcd_dpi_panel_config_t dpi_cfg = JD9365_800_1280_PANEL_60HZ_DPI_CONFIG(LCD_COLOR_PIXEL_FORMAT_RGB565);
    dpi_cfg.num_fbs = PANEL_NUM_FBS;

    jd9365_vendor_config_t vendor = {
        .flags.use_mipi_interface = 1,
        .mipi_config = {
            .dsi_bus = dsi_bus,
            .dpi_config = &dpi_cfg,
            .lane_num = PANEL_DSI_LANES,
        },
    };
    esp_lcd_panel_dev_config_t dev_cfg = {
        .reset_gpio_num = BSP_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor,
    };

    if (esp_lcd_new_panel_jd9365(panel_io, &dev_cfg, &panel) != ESP_OK) {
        ESP_LOGE(TAG, "FAIL: esp_lcd_new_panel_jd9365 (reset pin %d wrong?)", BSP_LCD_RST);
        return;
    }
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));
    ESP_LOGI(TAG, "  panel initialised, %d framebuffer(s) of %.2f MiB",
             PANEL_NUM_FBS, PANEL_FB_BYTES / 1048576.0);

    // ---- The backlight hunt -----------------------------------------------
    // Paint white first so a lit backlight is unmistakable, then drive each
    // candidate in turn. Whichever makes the screen light is the answer.
    fill(0xFFFF);
    ESP_LOGW(TAG, "--- WATCH THE SCREEN: driving backlight candidates ---");
    for (size_t i = 0; i < sizeof(BL_CANDIDATES) / sizeof(BL_CANDIDATES[0]); i++) {
        gpio_num_t pin = BL_CANDIDATES[i];
        gpio_config_t gc = {
            .pin_bit_mask = 1ULL << pin,
            .mode = GPIO_MODE_OUTPUT,
        };
        gpio_config(&gc);
        gpio_set_level(pin, 1);
        ESP_LOGW(TAG, "  GPIO %-2d HIGH for 3 s -- lit? then BSP_LCD_BACKLIGHT = %d", pin, pin);
        vTaskDelay(pdMS_TO_TICKS(3000));
        gpio_set_level(pin, 0);
        vTaskDelay(pdMS_TO_TICKS(400));
    }
    // The GSL3680 sits in reset until this is released. Nothing later works
    // without it, and it costs one line.
    gpio_config_t tprst = { .pin_bit_mask = 1ULL << BSP_TOUCH_RST, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&tprst);
    gpio_set_level(BSP_TOUCH_RST, 1);

    // Drive EVERY candidate high for the rest of the run. Parking on just the
    // board.h guess would mean that if the real pin is the other one, the
    // colour test below runs on a dark screen and teaches nothing.
    for (size_t i = 0; i < sizeof(BL_CANDIDATES) / sizeof(BL_CANDIDATES[0]); i++) {
        gpio_set_level(BL_CANDIDATES[i], 1);
    }
    ESP_LOGW(TAG, "  all candidates now HIGH -- colour test below is visible either way");
    ESP_LOGW(TAG, "  NOTE WHICH 3 s WINDOW LIT THE PANEL: that pin is the answer.");

    // ---- Colour order and inversion ---------------------------------------
    // Named colours, announced before each. If red shows as blue the element
    // order is BGR; if every colour is its complement, the panel wants
    // inversion. Both are cheap to see and expensive to guess wrong.
    const struct { uint16_t v; const char *name; } swatches[] = {
        { 0xF800, "RED"   },
        { 0x07E0, "GREEN" },
        { 0x001F, "BLUE"  },
        { 0xFFFF, "WHITE" },
        { 0x0000, "BLACK" },
    };
    ESP_LOGI(TAG, "--- colour check ---");
    for (size_t i = 0; i < sizeof(swatches) / sizeof(swatches[0]); i++) {
        ESP_LOGI(TAG, "  showing %s", swatches[i].name);
        fill(swatches[i].v);
        vTaskDelay(pdMS_TO_TICKS(1200));
    }
    ESP_LOGI(TAG, "  HOW TO READ THAT: if RED appeared blue, element order is BGR;");
    ESP_LOGI(TAG, "  if every colour appeared as its complement, inversion is needed.");

    // ---- Measured refresh rate --------------------------------------------
    // The vendor timings compute to 68.7 Hz, not the 60 the macro name claims.
    // Measure rather than believe either number.
    ESP_LOGI(TAG, "--- refresh rate ---");
    const int frames = 60;
    int64_t t0 = esp_timer_get_time();
    for (int i = 0; i < frames; i++) fill((i & 1) ? 0x0000 : 0x1082);
    int64_t dt = esp_timer_get_time() - t0;
    ESP_LOGI(TAG, "  %d full-screen fills in %.0f ms -> %.1f fps (CPU-bound fill, not vsync)",
             frames, dt / 1000.0, frames * 1e6 / dt);
    ESP_LOGI(TAG, "  vendor timings predict %.1f Hz scan-out",
             PANEL_DPI_CLK_MHZ * 1e6 /
             ((double)(PANEL_H_RES + PANEL_HSYNC_BACK_PORCH + PANEL_HSYNC_PULSE_WIDTH + PANEL_HSYNC_FRONT_PORCH) *
              (double)(PANEL_V_RES + PANEL_VSYNC_BACK_PORCH + PANEL_VSYNC_PULSE_WIDTH + PANEL_VSYNC_FRONT_PORCH)));

    ESP_LOGI(TAG, "=== Stage 0.1 complete ===");
}
