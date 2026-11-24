// ===========================================================================
// stage0_main.c -- the Stage 0 gate harness
// ===========================================================================
// Before any application code is written, each gate establishes a hardware
// fact as a VALUE the firmware then uses -- not as a vague "it seems to work".
// This is the rule the CYD project ran on, and it is why that project's
// board.h contains two constants that contradict the usual advice for the
// board: they were measured.
//
// Gates run independently and in increasing order of dependency, so a failure
// in one does not hide the results of the others. In particular gate 0.4 (PPA)
// needs no display, no card and no network -- it is the gate the transition
// catalogue depends on, so it must be readable even on a board whose panel
// never lights.
//
// Record what comes out in docs/stage0_results.md. The application does not
// get written against a gate that has not produced a number.
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_psram.h"
#include "esp_heap_caps.h"
#include "esp_flash.h"
#include "esp_lcd_types.h"
#include "board.h"

static const char *TAG = "stage0";

void gate_display_run(void);
void gate_touch_run(void);
void gate_sdcard_run(void);
void gate_ppa_run(void);
void gate_jpeg_run(void);
void gate_bigphoto_run(void);
void gate_sdcard_unmount(void);
void gate_touch_rescan_after_panel(void);
void gate_backlight_run(esp_lcd_panel_handle_t panel);
esp_lcd_panel_handle_t gate_display_panel(void);

// Gate 0.0: the environment every other gate assumes. Chiefly, that PSRAM
// actually enumerated -- without it nothing else in this project is possible,
// and the failure is much clearer stated here than as an allocation error
// three gates later.
static void gate_environment(void)
{
    ESP_LOGI(TAG, "=== Stage 0.0: environment ===");
    ESP_LOGI(TAG, "  board: %s", BOARD_NAME);

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    ESP_LOGI(TAG, "  chip:  %s, %d core(s), silicon rev v%d.%d",
             CONFIG_IDF_TARGET, chip.cores,
             chip.revision / 100, chip.revision % 100);

    uint32_t flash_size = 0;
    if (esp_flash_get_physical_size(NULL, &flash_size) == ESP_OK) {
        ESP_LOGI(TAG, "  flash: %.0f MB", flash_size / (1024.0 * 1024.0));
    }

    if (esp_psram_is_initialized()) {
        size_t psram = esp_psram_get_size();
        ESP_LOGI(TAG, "  PSRAM: %.0f MB, %u bytes free to malloc",
                 psram / (1024.0 * 1024.0),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

        // State the budget explicitly -- this is the arithmetic the whole
        // architecture rests on, and it is better checked than assumed.
        const double fb = PANEL_FB_BYTES / 1048576.0;
        ESP_LOGI(TAG, "  budget: %d panel FB (%.2f MiB ea) + 2 photo (%.2f) "
                      "+ LVGL ARGB layer (%.2f) = %.1f MiB of %.0f",
                 PANEL_NUM_FBS, fb, fb * 2,
                 PANEL_H_RES * PANEL_V_RES * 4 / 1048576.0,
                 PANEL_NUM_FBS * fb + 2 * fb + PANEL_H_RES * PANEL_V_RES * 4 / 1048576.0,
                 psram / (1024.0 * 1024.0));
    } else {
        ESP_LOGE(TAG, "  PSRAM: NOT INITIALISED -- nothing else here can work.");
        ESP_LOGE(TAG, "         Check CONFIG_SPIRAM and the HEX/200MHz mode settings.");
    }

    ESP_LOGI(TAG, "  internal heap free: %u bytes",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG, "=== Stage 0.0 complete ===");
}

void app_main(void)
{
    printf("\n\n");
    ESP_LOGI(TAG, "###########################################################");
    ESP_LOGI(TAG, "#  ESP32-P4 Photo Frame -- Stage 0 hardware gates          #");
    ESP_LOGI(TAG, "#  Each gate produces a value the firmware will use.       #");
    ESP_LOGI(TAG, "#  Record results in docs/stage0_results.md.               #");
    ESP_LOGI(TAG, "###########################################################\n");

    gate_environment();
    vTaskDelay(pdMS_TO_TICKS(500));

    // 0.4 runs early and deliberately: it needs no peripheral beyond PSRAM,
    // and it is the gate the transition design depends on. If the panel never
    // lights, this number is still the one worth having.
    gate_ppa_run();
    vTaskDelay(pdMS_TO_TICKS(500));

    gate_jpeg_run();
    vTaskDelay(pdMS_TO_TICKS(500));

    gate_touch_run();
    vTaskDelay(pdMS_TO_TICKS(500));

    gate_sdcard_run();
    vTaskDelay(pdMS_TO_TICKS(500));

    // Needs the card mounted by 0.3, so it has to follow it.
    gate_bigphoto_run();
    gate_sdcard_unmount();
    vTaskDelay(pdMS_TO_TICKS(500));

    // Display last: it is the only gate that needs a human watching the screen,
    // and it ends by parking on a backlight candidate.
    gate_display_run();

    // Only meaningful once the panel is up: on a DSI panel the touch
    // controller usually shares the panel's power/reset domain.
    vTaskDelay(pdMS_TO_TICKS(300));
    gate_touch_rescan_after_panel();

    // Why the screen is black, tested rather than guessed.
    gate_backlight_run(gate_display_panel());

    ESP_LOGI(TAG, "\n### All gates run. Transcribe the numbers before writing app code. ###");

    while (1) vTaskDelay(pdMS_TO_TICKS(10000));
}
