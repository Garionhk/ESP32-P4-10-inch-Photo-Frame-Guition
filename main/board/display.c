#include "display.h"
#include "board.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_ldo_regulator.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "driver/gpio.h"
#include "esp_lcd_jd9365_10_1.h"

static const char *TAG = "display";

static esp_lcd_panel_handle_t     s_panel;
static esp_lcd_dsi_bus_handle_t   s_bus;
static esp_lcd_panel_io_handle_t  s_io;
static esp_ldo_channel_handle_t   s_ldo;
static void                      *s_fb[PANEL_NUM_FBS];
static int                        s_fb_count;

esp_err_t display_init(void)
{
    // The DSI PHY runs from an internal LDO, not the main rail. Skip this and
    // the bus initialises into silence -- no error, just nothing.
    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id = PANEL_MIPI_LDO_CHAN,
        .voltage_mv = PANEL_MIPI_LDO_MV,
    };
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo_cfg, &s_ldo), TAG, "DPHY LDO");

    esp_lcd_dsi_bus_config_t bus_cfg = JD9365_PANEL_BUS_DSI_2CH_CONFIG();
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_cfg, &s_bus), TAG, "dsi bus");

    esp_lcd_dbi_io_config_t io_cfg = JD9365_PANEL_IO_DBI_CONFIG();
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(s_bus, &io_cfg, &s_io), TAG, "dbi io");

    // The vendor macro ships num_fbs = 1. Two is what makes tear-free
    // compositing possible: draw the next frame while the DSI scans this one.
    esp_lcd_dpi_panel_config_t dpi = JD9365_800_1280_PANEL_60HZ_DPI_CONFIG(LCD_COLOR_PIXEL_FORMAT_RGB565);
    dpi.num_fbs = PANEL_NUM_FBS;

    jd9365_vendor_config_t vendor = {
        .flags.use_mipi_interface = 1,
        .mipi_config = { .dsi_bus = s_bus, .dpi_config = &dpi, .lane_num = PANEL_DSI_LANES },
    };
    esp_lcd_panel_dev_config_t dev = {
        .reset_gpio_num = BSP_LCD_RST,
        .rgb_ele_order  = PANEL_RGB_ELEMENT_ORDER,   // measured RGB, not assumed
        .bits_per_pixel = 16,
        .vendor_config  = &vendor,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_jd9365(s_io, &dev, &s_panel), TAG, "jd9365");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "disp on");

    // Collect the framebuffers once; the compositor cycles them by index.
    s_fb_count = PANEL_NUM_FBS;
    if (PANEL_NUM_FBS >= 2) {
        ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_get_frame_buffer(s_panel, 2, &s_fb[0], &s_fb[1]),
                            TAG, "get fb");
    } else {
        ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_get_frame_buffer(s_panel, 1, &s_fb[0]),
                            TAG, "get fb");
    }

    // Start black rather than on whatever the PSRAM happened to hold -- the
    // backlight comes on next and uninitialised memory is visibly noise.
    for (int i = 0; i < s_fb_count; i++) memset(s_fb[i], 0, PANEL_FB_BYTES);
    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, PANEL_H_RES, PANEL_V_RES, s_fb[0]);

    gpio_config_t bl = { .pin_bit_mask = 1ULL << BSP_LCD_BACKLIGHT, .mode = GPIO_MODE_OUTPUT };
    ESP_RETURN_ON_ERROR(gpio_config(&bl), TAG, "backlight gpio");
    gpio_set_level(BSP_LCD_BACKLIGHT, 1);

    ESP_LOGI(TAG, "JD9365 %dx%d up, %d framebuffer(s) of %.2f MiB, backlight GPIO %d",
             PANEL_H_RES, PANEL_V_RES, s_fb_count, PANEL_FB_BYTES / 1048576.0,
             BSP_LCD_BACKLIGHT);
    return ESP_OK;
}

int   display_width(void)            { return PANEL_H_RES; }
int   display_height(void)           { return PANEL_V_RES; }
int   display_framebuffer_count(void){ return s_fb_count; }
void *display_framebuffer(int i)     { return (i >= 0 && i < s_fb_count) ? s_fb[i] : NULL; }

esp_err_t display_present(void *fb)
{
    return esp_lcd_panel_draw_bitmap(s_panel, 0, 0, PANEL_H_RES, PANEL_V_RES, fb);
}

esp_err_t display_set_brightness(int percent)
{
    gpio_set_level(BSP_LCD_BACKLIGHT, percent > 0 ? 1 : 0);
    return ESP_OK;
}
