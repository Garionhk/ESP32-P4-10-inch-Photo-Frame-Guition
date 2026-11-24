// ===========================================================================
// gate_touch.c -- Stage 0.2: I2C bus scan and GT911 identification
// ===========================================================================
// Runs the bus scan BEFORE anything tries to talk to a specific device, for
// the same reason the CYD project ran its touch test display-free: if touch
// and display are brought up together, a fault in either one masks the other.
//
// The GT911 answers at 0x5D or 0x14 depending on how INT and RST are strapped
// during power-on. Waveshare's BSPs leave both unconnected on this family, so
// the address is fixed by board wiring we cannot read off a schematic -- it
// has to be probed. The ES8311 codec (0x18) and ES7210 (0x40) share the bus
// and are useful as proof the bus itself is alive.
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "board.h"

static const char *TAG = "gate0.2";

static const char *identify(uint8_t addr)
{
    switch (addr) {
        case 0x5D: return "GT911 touch (default strap)";
        case 0x14: return "GT911 touch (alternate strap)";
        case 0x18: return "ES8311 audio codec";
        case 0x32: return "RX8025T RTC (battery-backed)";
        case 0x36: return "unidentified (battery fuel gauge?)";
        // 0x40 was misread as the ES7210 codec for three runs. On THIS board
        // it is the GSL3680 touch controller -- the two parts share the
        // address, and the board has no ES7210 at all.
        case 0x40: return "GSL3680 touch";
        default:   return "unknown";
    }
}

// Scan a bus we were handed. Split out because this has to run TWICE -- see
// gate_touch_rescan_after_panel() below for why that matters.
static int scan_bus(i2c_master_bus_handle_t bus, bool *found_touch, uint8_t *touch_addr)
{
    int found = 0;
    *found_touch = false;
    for (uint8_t a = 0x08; a < 0x78; a++) {
        if (i2c_master_probe(bus, a, 50) == ESP_OK) {
            ESP_LOGI(TAG, "  0x%02X  %s", a, identify(a));
            found++;
            if (a == BSP_TOUCH_ADDR) { *found_touch = true; *touch_addr = a; }
        }
    }
    return found;
}

// ---------------------------------------------------------------------------
// The second scan, run AFTER the panel is up. This exists because the first
// one was ordered wrong.
//
// The CYD project's rule was to test touch display-free, so a panel fault
// could not mask a touch fault. That rule does not transfer here: on the CYD
// the touch controller was an independent SPI device, whereas on a MIPI-DSI
// panel the touch controller rides the same FPC and typically shares the
// panel's power and reset domain. Scanning before panel init therefore proves
// nothing -- a silent controller may simply be unpowered.
//
// If the touch controller appears only in THIS scan, that is the answer.
// ---------------------------------------------------------------------------
void gate_touch_rescan_after_panel(void)
{
    ESP_LOGI(TAG, "=== Stage 0.2b: I2C rescan AFTER panel init ===");

    // The panel driver already initialised this port, so take its bus rather
    // than trying to create a second one on the same port.
    i2c_master_bus_handle_t bus = NULL;
    bool owned = false;
    if (i2c_master_get_bus_handle(BSP_I2C_NUM, &bus) != ESP_OK || !bus) {
        i2c_master_bus_config_t bcfg = {
            .i2c_port = BSP_I2C_NUM,
            .sda_io_num = BSP_I2C_SDA,
            .scl_io_num = BSP_I2C_SCL,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true,
        };
        if (i2c_new_master_bus(&bcfg, &bus) != ESP_OK) {
            ESP_LOGE(TAG, "  could not obtain an I2C bus for the rescan");
            return;
        }
        owned = true;
    }

    bool got = false; uint8_t addr = 0;
    int n = scan_bus(bus, &got, &addr);

    if (got) {
        // It answers in BOTH scans, so the "shares the panel power domain"
        // theory was wrong twice over -- it was never in reset, it was simply
        // a GSL3680 being looked for at GT911 addresses.
        ESP_LOGI(TAG, "PASS: GSL3680 still present at 0x%02X after panel init.", addr);
    } else {
        ESP_LOGE(TAG, "FAIL: still no known touch controller among %d device(s).", n);
        ESP_LOGE(TAG, "  Expected GSL3680 at 0x%02X.", BSP_TOUCH_ADDR);
    }

    if (owned) i2c_del_master_bus(bus);
    ESP_LOGI(TAG, "=== Stage 0.2b complete ===");
}

void gate_touch_run(void)
{
    ESP_LOGI(TAG, "=== Stage 0.2: I2C scan (SDA=%d SCL=%d @ %d Hz) ===",
             BSP_I2C_SDA, BSP_I2C_SCL, BSP_I2C_CLK_HZ);

    i2c_master_bus_config_t bcfg = {
        .i2c_port = BSP_I2C_NUM,
        .sda_io_num = BSP_I2C_SDA,
        .scl_io_num = BSP_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    esp_err_t err = i2c_new_master_bus(&bcfg, &bus);
    if (err != ESP_OK) { ESP_LOGE(TAG, "FAIL: i2c_new_master_bus %s", esp_err_to_name(err)); return; }

    bool gt911 = false;
    uint8_t gt911_addr = 0;
    int found = scan_bus(bus, &gt911, &gt911_addr);

    if (!found) {
        ESP_LOGE(TAG, "FAIL: nothing on the bus. Wrong pins, or the bus is held low.");
    } else if (!gt911) {
        ESP_LOGW(TAG, "no touch controller yet -- bus is alive (%d device(s)).", found);
        ESP_LOGW(TAG, "  EXPECTED at this point: on a DSI panel the touch controller usually");
        ESP_LOGW(TAG, "  shares the panel's power/reset domain, so it is silent until the");
        ESP_LOGW(TAG, "  panel is up. Gate 0.2b rescans after init -- that is the real test.");
    } else {
        ESP_LOGI(TAG, "PASS: GSL3680 touch responds at 0x%02X.", gt911_addr);
        ESP_LOGW(TAG, "  It will not REPORT anything yet: the GSL3680 needs its reset");
        ESP_LOGW(TAG, "  toggled (GPIO %d) and an ~88 KB firmware blob pushed over I2C.",
                 BSP_TOUCH_RST);
    }

    i2c_del_master_bus(bus);
    ESP_LOGI(TAG, "=== Stage 0.2 complete ===");
}
