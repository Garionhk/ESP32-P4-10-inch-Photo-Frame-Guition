// ===========================================================================
// gate_backlight.c -- Stage 0.1b: why is the screen black?
// ===========================================================================
// The panel answers (LCD ID 93 65 04), init succeeds, both framebuffers
// allocate -- and the screen stays black on every GPIO we drive.
//
// Reading Waveshare's own code explains why the GPIO sweep was doomed: on this
// display family the backlight is NOT a GPIO. `waveshare/esp32_p4_platform`
// defines BSP_LCD_BACKLIGHT as GPIO_NUM_NC and drives brightness over I2C --
// device 0x45, register 0x96, value 0..255. The JD9365 10.1 driver does the
// same sequence internally at panel-create time:
//
//     0x95 <- 0x11      panel control
//     0x95 <- 0x17      panel control
//     0x96 <- 0x00      brightness 0
//     0x96 <- 0xFF      brightness 100%
//
// So the backlight should already be on. It is not. This gate finds out which
// link in the chain is broken, and it tests exactly three things:
//
//   A. Is there anything at 0x45 at all? Our bus scan never saw it -- but a
//      probe is a zero-length transaction, which some controllers NACK while
//      still accepting real register writes. Distinguish the two.
//
//   B. Does the frame buffer fetch actually succeed? The first version of the
//      colour test returned SILENTLY if it did not, so "black screen" may mean
//      "we never wrote a pixel" rather than "the backlight is off". That
//      ambiguity was a bug in the harness, not in the board.
//
//   C. What are the two unknown devices at 0x32 and 0x36? Read-only
//      fingerprinting: nothing here writes to a device it cannot identify.
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_mipi_dsi.h"
#include "board.h"

static const char *TAG = "gate0.1b";

// Waveshare's I2C backlight controller, from esp32_p4_platform.c.
#define BL_I2C_ADDR   0x45
#define BL_REG_CTRL   0x95
#define BL_REG_BRIGHT 0x96

static i2c_master_bus_handle_t get_bus(bool *owned)
{
    i2c_master_bus_handle_t bus = NULL;
    *owned = false;
    if (i2c_master_get_bus_handle(BSP_I2C_NUM, &bus) == ESP_OK && bus) return bus;

    i2c_master_bus_config_t bcfg = {
        .i2c_port = BSP_I2C_NUM,
        .sda_io_num = BSP_I2C_SDA,
        .scl_io_num = BSP_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&bcfg, &bus) != ESP_OK) return NULL;
    *owned = true;
    return bus;
}

// Write one register and REPORT the result. The vendor driver ignores these
// return codes, which is exactly why a missing controller looks like a working
// one right up until the screen stays dark.
static esp_err_t reg_write(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    esp_err_t err = i2c_master_transmit(dev, buf, 2, 100);
    ESP_LOGI(TAG, "    write 0x%02X <- 0x%02X : %s", reg, val, esp_err_to_name(err));
    return err;
}

// Read a few registers from an unidentified device. Read-only on purpose.
static void fingerprint(i2c_master_bus_handle_t bus, uint8_t addr)
{
    i2c_device_config_t dcfg = { .scl_speed_hz = 100000, .device_address = addr };
    i2c_master_dev_handle_t dev = NULL;
    if (i2c_master_bus_add_device(bus, &dcfg, &dev) != ESP_OK) return;

    ESP_LOGI(TAG, "  device 0x%02X:", addr);
    for (uint8_t reg = 0x00; reg <= 0x0C; reg += 2) {
        uint8_t out[2] = {0, 0};
        if (i2c_master_transmit_receive(dev, &reg, 1, out, 2, 100) == ESP_OK) {
            ESP_LOGI(TAG, "    reg 0x%02X = %02X %02X", reg, out[0], out[1]);
        }
    }
    i2c_master_bus_rm_device(dev);
}

void gate_backlight_run(esp_lcd_panel_handle_t panel)
{
    ESP_LOGI(TAG, "=== Stage 0.1b: black-screen diagnosis ===");

    bool owned = false;
    i2c_master_bus_handle_t bus = get_bus(&owned);
    if (!bus) { ESP_LOGE(TAG, "no I2C bus available"); return; }

    // The 0x45 I2C brightness controller belonged to the Waveshare boards this
    // was originally written for. The JC8012P4A1 drives the backlight from a
    // plain GPIO (23), so that whole probe is gone.

    // ---- B. Colour order and inversion, in one look ----------------------
    // The old test flashed RED/GREEN/BLUE for 1.2 s each, which is easy to
    // miss while watching for a backlight. Three stacked bars answer the same
    // question from a single glance, and stay on screen indefinitely.
    //
    //   top -> bottom reads RED  GREEN BLUE   -> element order RGB, no inversion
    //                       BLUE GREEN RED    -> element order is BGR
    //                       CYAN MAGENTA YEL  -> panel needs inversion
    ESP_LOGI(TAG, "--- B. colour bars ---");
    if (panel) {
        void *fb = NULL;
        esp_err_t err = esp_lcd_dpi_panel_get_frame_buffer(panel, 1, &fb);
        if (err != ESP_OK || !fb) {
            ESP_LOGE(TAG, "  get_frame_buffer failed (%s) -- nothing drawn",
                     esp_err_to_name(err));
        } else {
            uint16_t *p = (uint16_t *)fb;
            const int band = PANEL_V_RES / 3;
            for (int y = 0; y < PANEL_V_RES; y++) {
                uint16_t c = (y < band) ? 0xF800            // red
                           : (y < band * 2) ? 0x07E0        // green
                           : 0x001F;                        // blue
                uint16_t *row = p + (size_t)y * PANEL_H_RES;
                for (int x = 0; x < PANEL_H_RES; x++) row[x] = c;
            }
            err = esp_lcd_panel_draw_bitmap(panel, 0, 0, PANEL_H_RES, PANEL_V_RES, fb);
            ESP_LOGI(TAG, "  drew R/G/B bars: %s", esp_err_to_name(err));
            ESP_LOGW(TAG, "  >>> READ THE SCREEN TOP TO BOTTOM <<<");
            ESP_LOGW(TAG, "    RED GREEN BLUE      -> RGB order, no inversion (expected)");
            ESP_LOGW(TAG, "    BLUE GREEN RED      -> element order is BGR");
            ESP_LOGW(TAG, "    CYAN MAGENTA YELLOW -> panel needs inversion");
        }
    }

    // 0x32 is identified (RX8025T RTC); 0x36 still is not.
    ESP_LOGI(TAG, "--- C. fingerprint 0x36 (read-only) ---");
    fingerprint(bus, 0x36);

    ESP_LOGW(TAG, ">>> Panel left showing three colour bars. <<<");

    if (owned) i2c_del_master_bus(bus);
    ESP_LOGI(TAG, "=== Stage 0.1b complete ===");
}
