#include "sdcard.h"
#include "board.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"

static const char *TAG = "sdcard";

static sdmmc_card_t        *s_card;
static sd_pwr_ctrl_handle_t s_pwr;

esp_err_t sdcard_mount(void)
{
    if (s_card) return ESP_OK;

    // Power first -- nothing answers until this rail is up.
    if (!s_pwr) {
        sd_pwr_ctrl_ldo_config_t ldo = { .ldo_chan_id = BSP_SD_LDO_CHAN };
        esp_err_t err = sd_pwr_ctrl_new_on_chip_ldo(&ldo, &s_pwr);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "LDO_VO%d rail: %s", BSP_SD_LDO_CHAN, esp_err_to_name(err));
            return err;
        }
    }

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = BSP_SD_SLOT;
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
    host.pwr_ctrl_handle = s_pwr;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = BSP_SD_WIDTH;
    slot.clk = BSP_SD_CLK;  slot.cmd = BSP_SD_CMD;
    slot.d0  = BSP_SD_D0;   slot.d1  = BSP_SD_D1;
    slot.d2  = BSP_SD_D2;   slot.d3  = BSP_SD_D3;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_sdmmc_mount_config_t mcfg = {
        // NEVER format on failure: this card holds the user's photos, and a
        // mount failure must look like one rather than like an empty card.
        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 32 * 1024,
    };

    esp_err_t err = esp_vfs_fat_sdmmc_mount(BSP_SD_MOUNT, &host, &slot, &mcfg, &s_card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mount: %s", esp_err_to_name(err));
        if (err == ESP_FAIL) ESP_LOGE(TAG, "  card answered but no FAT filesystem (exFAT is not supported)");
        else                 ESP_LOGE(TAG, "  card did not answer -- not seated, or dead");
        return err;
    }

    ESP_LOGI(TAG, "mounted %s: %s, %lluMB",
             BSP_SD_MOUNT, s_card->cid.name, ((uint64_t)s_card->csd.capacity * s_card->csd.sector_size) >> 20);
    return ESP_OK;
}

void sdcard_unmount(void)
{
    if (s_card) { esp_vfs_fat_sdcard_unmount(BSP_SD_MOUNT, s_card); s_card = NULL; }
}

bool sdcard_mounted(void) { return s_card != NULL; }

bool sdcard_usage(uint64_t *total, uint64_t *freeb)
{
    if (!s_card) return false;
    return esp_vfs_fat_info(BSP_SD_MOUNT, total, freeb) == ESP_OK;
}
