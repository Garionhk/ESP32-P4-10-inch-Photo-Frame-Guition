// ===========================================================================
// board.h -- THE pin map and hardware profile
// ===========================================================================
// Target: Guition JC8012P4A1
//   ESP32-P4 (RISC-V dual core) rev v1.3 + ESP32-C6-MINI-1 (WiFi 6 / BLE 5)
//   16 MB NOR flash (W25Q128), 32 MB PSRAM (in-package)
//   10.1" JD9365 MIPI-DSI panel, 800x1280 native portrait, 2 data lanes
//   GSL3680 capacitive touch, ES8311 audio, RX8025T RTC, microSD, 2MP CSI cam
//
// NOTE: this was originally written for a Waveshare ESP32-P4-WIFI6-Touch-LCD-10.1
// and every backlight/touch value was wrong as a result. Stage 0 caught it: the
// screen stayed black on both Waveshare backlight candidates, no touch answered
// at any GT911 address, and the flash measured 16 MB against a 32 MB spec. The
// board is a Guition JC8012P4A1; values below now come from its own BSP.
//
// PROVENANCE -- every value below is one of three things, and the difference
// matters when something doesn't come up:
//
//   [VENDOR]  Read out of Waveshare's own published components:
//             waveshare/esp32_p4_wifi6_touch_lcd_xc v3.0.1 (pin map) and
//             waveshare/esp_lcd_jd9365_10_1 v2.0.0 (panel timings).
//             These are facts about shipped hardware, not guesses.
//
//   [SOC]     Fixed by the ESP32-P4 silicon; cannot differ per board.
//
//   [STAGE0]  NOT yet confirmed on this board. A hypothesis with a reason.
//             Gate 0.1/0.2 replaces it with a measured value. Until then it
//             is marked, so a dark screen is traced to the right line.
//
// The CYD project's rule applies here too: a constant that is right on one
// unit can be wrong on the next, so anything not [VENDOR] or [SOC] gets
// verified before the application depends on it.
#pragma once

#include "driver/gpio.h"

// ---- Board identity -------------------------------------------------------
#define BOARD_NAME              "Guition JC8012P4A1"

// ---- Panel: JD9365, 800x1280 native portrait ------------------------------
// [VENDOR] waveshare/esp_lcd_jd9365_10_1 -- JD9365_800_1280_PANEL_60HZ_DPI_CONFIG
// and JD9365_PANEL_BUS_DSI_2CH_CONFIG.
//
// The panel scans PORTRAIT. Landscape is a rotation we apply, and the whole
// point of the cache design is that the rotation happens once per photo (in
// the single PPA fit pass) rather than once per frame.
#define PANEL_H_RES             800     // [VENDOR] native, portrait
#define PANEL_V_RES             1280    // [VENDOR] native, portrait
#define PANEL_DSI_LANES         2       // [VENDOR]
#define PANEL_DSI_LANE_MBPS     1500    // [VENDOR] 2 x 1500 Mbps carries 800x1280x16 @60
#define PANEL_DPI_CLK_MHZ       80      // [VENDOR]

// [VENDOR] video timings. Refresh = clk / h_total / v_total
//          = 80e6 / 880 / 1324 = 68.7 Hz
#define PANEL_HSYNC_BACK_PORCH  20
#define PANEL_HSYNC_PULSE_WIDTH 20
#define PANEL_HSYNC_FRONT_PORCH 40
#define PANEL_VSYNC_BACK_PORCH  10
#define PANEL_VSYNC_PULSE_WIDTH 4
#define PANEL_VSYNC_FRONT_PORCH 30

// [VENDOR] LDO_VO3 feeds VDD_MIPI_DPHY on every P4 board Waveshare ships.
#define PANEL_MIPI_LDO_CHAN     3
#define PANEL_MIPI_LDO_MV       2500

// Framebuffers. The vendor macro ships num_fbs=1; we want 2 so the compositor
// can draw the next frame while the DSI scans out the current one. At
// 800*1280*2 bytes that is 1.95 MiB each -- affordable only because of the
// 32 MB PSRAM, and the reason tear-free compositing is on the table at all.
#define PANEL_NUM_FBS           2
#define PANEL_FB_BYTES          (PANEL_H_RES * PANEL_V_RES * 2)

// ---- Display control pins -------------------------------------------------
// [VENDOR] JC8012P4A1 BSP, pins_config.h (github.com/profi-max/JC8012P4A1_BSP_ESP32P4)
//
// The backlight is a plain GPIO here -- NOT the I2C brightness controller at
// 0x45 that Waveshare's DSI boards use, and NOT GPIO 26 or 32. Chasing those
// is what produced three runs of a black screen on an otherwise healthy panel
// (JD9365 answered with LCD ID 93 65 04 the whole time).
//
// CONFIRMED on hardware 2026-09-01: driving 23 lights the panel.
#define BSP_LCD_BACKLIGHT       GPIO_NUM_23
#define BSP_LCD_RST             GPIO_NUM_27

// CONFIRMED on hardware: R/G/B bars read red, green, blue top to bottom, so
// the element order is RGB and the panel does NOT want inversion. Worth
// recording because the sibling CYD project needed inversion ON despite its
// panel being documented otherwise -- these are measured, never assumed.
#define PANEL_RGB_ELEMENT_ORDER LCD_RGB_ELEMENT_ORDER_RGB
#define PANEL_NEEDS_INVERSION   0

// Backlight is PWM (LEDC) rather than a bare GPIO, so brightness is a setting
// and a night-dimming schedule stays possible later.
#define BSP_LCD_BL_LEDC_CH      1
#define BSP_LCD_BL_FREQ_HZ      5000

// ---- I2C: GT911 touch + ES8311 codec share one bus ------------------------
// [VENDOR] identical across every Waveshare P4 BSP checked (xc and 7b), which
// is why these are trusted without a gate.
#define BSP_I2C_SCL             GPIO_NUM_8
#define BSP_I2C_SDA             GPIO_NUM_7
#define BSP_I2C_NUM             1
#define BSP_I2C_CLK_HZ          400000

// [VENDOR] Touch is a GSL3680 at 0x40 -- not a GT911, which is why nothing
// ever answered at 0x5D/0x14. Both scans DID see 0x40; it was misread as the
// ES7210 codec, an address the two parts happen to share.
//
// The GSL3680 is not a register-only part: it needs its reset toggled and then
// an ~88 KB firmware blob pushed over I2C before it reports a single touch.
// Budget real work for this, not a driver include.
#define BSP_TOUCH_ADDR          0x40
#define BSP_TOUCH_RST           GPIO_NUM_22
#define BSP_TOUCH_INT           GPIO_NUM_21

// Other devices on the same bus, identified during Stage 0 so a future scan
// is not a mystery: 0x18 = ES8311 codec, 0x32 = RX8025T RTC (battery-backed).
#define BSP_RTC_ADDR            0x32
#define BSP_CODEC_ADDR          0x18

// ---- microSD: SDMMC slot 0 ------------------------------------------------
// [SOC] Slot 0 pins are fixed in P4 silicon and cannot be remapped. [VENDOR]
// JC8012P4A1 README confirms slot 0 with 5.1k pull-ups on CLK/CMD.
//
// This is what makes the design work: the ESP32-C6 hangs off SDMMC *slot 1*
// via esp_hosted, so WiFi and the card do not contend for a bus. Worth stating
// because on many P4 boards they do.
#define BSP_SD_CLK              GPIO_NUM_43
#define BSP_SD_CMD              GPIO_NUM_44
#define BSP_SD_D0               GPIO_NUM_39
#define BSP_SD_D1               GPIO_NUM_40
#define BSP_SD_D2               GPIO_NUM_41
#define BSP_SD_D3               GPIO_NUM_42
#define BSP_SD_SLOT             0
#define BSP_SD_WIDTH            4
#define BSP_SD_MOUNT            "/sdcard"

// ---- Audio (present, unused by this firmware) -----------------------------
// [VENDOR] Kept so the pins are documented as taken rather than free.
#define BSP_I2S_MCLK            GPIO_NUM_13
#define BSP_I2S_SCLK            GPIO_NUM_12
#define BSP_I2S_LCLK            GPIO_NUM_10
#define BSP_I2S_DOUT            GPIO_NUM_9
#define BSP_I2S_DSIN            GPIO_NUM_11
#define BSP_POWER_AMP_IO        GPIO_NUM_53

// ---- Imaging pipeline limits ----------------------------------------------
// The hardware JPEG decoder has no downscale-during-decode and is only
// characterised by Espressif up to 1920x1080. Anything larger is handed to the
// software decoder, which CAN downscale by 1/2, 1/4, 1/8 and so caps peak RAM.
//
// This single fact is what forces the whole cache design: a 12 MP phone photo
// cannot be shown directly, at any speed.
#define JPEG_HW_MAX_DIM         2048

// Photos are cached fitted into a square box rather than at screen geometry.
// Consequence: the landscape/portrait setting costs nothing to change, because
// the orientation is applied in the per-photo PPA pass, not baked into cache.
#define CACHE_FIT_BOX           1280

// ---- microSD power rail ---------------------------------------------------
// [VENDOR, from the JC8012P4A1 schematic, sheet 4_CONN "TF CARD"]
//
// The card's VDD *and* all six 5.1k bus pull-ups are fed from ESP_LDO_VO4 --
// the ESP32-P4's own internal LDO channel 4, not from a always-on 3V3 rail.
// It must be switched on in software before the card will answer anything,
// exactly like the MIPI DPHY rail on channel 3.
//
// Symptom when it is missed: every mount attempt fails with ESP_ERR_TIMEOUT on
// send_op_cond, at every bus width and every clock down to 400 kHz, because the
// card is simply unpowered. That looks identical to a missing or dead card, and
// it is NOT a filesystem problem -- reformatting the card changes nothing.
#define BSP_SD_LDO_CHAN         4

// ---- ESP32-C6 WiFi co-processor (esp_hosted over SDIO) --------------------
// [VENDOR] JC8012P4A1 Simple_WiFi_hosted example, sdkconfig.esp32-p4_r3.
//
// The C6 sits on SDMMC *slot 1* while the microSD is on slot 0, so the radio
// and the card never contend. On many P4 boards they do.
//
// Recorded here for documentation; the driver itself is configured through
// Kconfig (CONFIG_ESP_HOSTED_SDIO_PIN_*) rather than these defines.
#define BSP_C6_SDIO_SLOT        1
#define BSP_C6_SDIO_CLK         18
#define BSP_C6_SDIO_CMD         19
#define BSP_C6_SDIO_D0          14
#define BSP_C6_SDIO_D1          15
#define BSP_C6_SDIO_D2          16
#define BSP_C6_SDIO_D3          17
#define BSP_C6_RESET_GPIO       54     // active HIGH on this board
#define BSP_C6_SDIO_KHZ         40000
