// ===========================================================================
// gate_sdcard.c -- Stage 0.3: microSD on SDMMC slot 0
// ===========================================================================
// Produces two values the firmware depends on:
//
//   1. That the card mounts at 4-bit on slot 0 at all. Slot 0's pins are fixed
//      in P4 silicon and cannot be remapped, so this either works or the card
//      is wired somewhere else entirely.
//
//   2. Sequential read and write MB/s. These set two expectations the user
//      will notice: how long a web upload takes, and whether a cache entry can
//      be read fast enough to prefetch the next photo during a dwell.
//
// Worth stating why there is no bus conflict to worry about: the ESP32-C6 that
// provides WiFi hangs off SDMMC *slot 1* via esp_hosted. On many P4 boards the
// card and the radio contend; on this one they do not.
#include <string.h>
#include <stdio.h>
#include <unistd.h>          // fsync -- the VFS cache would otherwise be what we timed
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "board.h"

static const char *TAG = "gate0.3";

#define BENCH_CHUNK  (64 * 1024)
#define BENCH_TOTAL  (8 * 1024 * 1024)   // 8 MiB: long enough to swamp caching

static sdmmc_card_t *card;
static sd_pwr_ctrl_handle_t sd_pwr;

static void bench(void)
{
    uint8_t *buf = heap_caps_aligned_alloc(64, BENCH_CHUNK, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!buf) { ESP_LOGE(TAG, "no DMA buffer for benchmark"); return; }
    memset(buf, 0xA5, BENCH_CHUNK);

    const char *path = BSP_SD_MOUNT "/bench.bin";
    const int chunks = BENCH_TOTAL / BENCH_CHUNK;

    // ---- write ----
    FILE *f = fopen(path, "wb");
    if (!f) { ESP_LOGE(TAG, "cannot create %s", path); heap_caps_free(buf); return; }
    int64_t t0 = esp_timer_get_time();
    for (int i = 0; i < chunks; i++) {
        if (fwrite(buf, 1, BENCH_CHUNK, f) != BENCH_CHUNK) { ESP_LOGE(TAG, "short write"); break; }
    }
    fflush(f);
    fsync(fileno(f));    // without this we would be timing the VFS cache
    int64_t w_us = esp_timer_get_time() - t0;
    fclose(f);

    // ---- read ----
    f = fopen(path, "rb");
    if (!f) { ESP_LOGE(TAG, "cannot reopen %s", path); heap_caps_free(buf); return; }
    t0 = esp_timer_get_time();
    for (int i = 0; i < chunks; i++) {
        if (fread(buf, 1, BENCH_CHUNK, f) != BENCH_CHUNK) { ESP_LOGE(TAG, "short read"); break; }
    }
    int64_t r_us = esp_timer_get_time() - t0;
    fclose(f);
    remove(path);
    heap_caps_free(buf);

    double w_mbs = BENCH_TOTAL / (w_us / 1e6) / 1048576.0;
    double r_mbs = BENCH_TOTAL / (r_us / 1e6) / 1048576.0;
    ESP_LOGI(TAG, "  sequential write %6.2f MiB/s", w_mbs);
    ESP_LOGI(TAG, "  sequential read  %6.2f MiB/s", r_mbs);

    // Translate into the two things the user actually experiences.
    ESP_LOGI(TAG, "--- what this means ---");
    ESP_LOGI(TAG, "  a 5 MB upload writes in ~%.1f s", 5.0 / w_mbs);
    // 80 KB is the measured middle of gate 0.5's cache entries (60-95 KB),
    // not the 350 KB the plan guessed before those were measured.
    ESP_LOGI(TAG, "  an 80 KB cache entry loads in ~%.0f ms", 0.078 / r_mbs * 1000.0);
    if (r_mbs < 3.0) {
        ESP_LOGW(TAG, "  SLOW CARD. Bus is fine; this is the card itself.");
        ESP_LOGW(TAG, "  A modern SDHC Class 10 should give 10-30x this. Re-measure");
        ESP_LOGW(TAG, "  before making any I/O-dependent decision.");
    }
}

// One mount attempt at a given width and clock. Returns ESP_OK on success.
static esp_err_t try_mount(int width, int freq_khz, const char *label)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = BSP_SD_SLOT;
    host.max_freq_khz = freq_khz;
    // Without this the card is unpowered and every rung below times out.
    host.pwr_ctrl_handle = sd_pwr;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = width;
    slot.clk = BSP_SD_CLK;
    slot.cmd = BSP_SD_CMD;
    slot.d0  = BSP_SD_D0;
    slot.d1  = BSP_SD_D1;
    slot.d2  = BSP_SD_D2;
    slot.d3  = BSP_SD_D3;
    // The board carries 5.1k external pull-ups on all six lines; the internal
    // ones cost nothing and rescue a marginal card.
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_sdmmc_mount_config_t mcfg = {
        // NEVER format on failure: this card holds the user's photos. A mount
        // failure must look like a mount failure, not like an empty card.
        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 32 * 1024,
    };

    esp_err_t err = esp_vfs_fat_sdmmc_mount(BSP_SD_MOUNT, &host, &slot, &mcfg, &card);
    ESP_LOGI(TAG, "  %-28s : %s", label, esp_err_to_name(err));
    return err;
}

void gate_sdcard_run(void)
{
    ESP_LOGI(TAG, "=== Stage 0.3: microSD, SDMMC slot %d ===", BSP_SD_SLOT);
    ESP_LOGI(TAG, "  pins CLK %d CMD %d D0-D3 %d/%d/%d/%d (fixed in P4 silicon)",
             BSP_SD_CLK, BSP_SD_CMD, BSP_SD_D0, BSP_SD_D1, BSP_SD_D2, BSP_SD_D3);

    // Power the card FIRST. Its VDD and all six bus pull-ups hang off the P4's
    // internal LDO_VO4 (schematic sheet 4_CONN), so until this is switched on
    // the card cannot answer and every mount times out identically.
    sd_pwr_ctrl_ldo_config_t ldo = { .ldo_chan_id = BSP_SD_LDO_CHAN };
    esp_err_t perr = sd_pwr_ctrl_new_on_chip_ldo(&ldo, &sd_pwr);
    ESP_LOGI(TAG, "  LDO_VO%d power rail: %s", BSP_SD_LDO_CHAN, esp_err_to_name(perr));
    if (perr != ESP_OK) {
        ESP_LOGE(TAG, "FAIL: cannot enable the card's power rail; nothing else can work.");
        return;
    }

    // A ladder rather than one attempt. Each rung fails differently, and the
    // rung that succeeds is itself the finding:
    //   4-bit fails but 1-bit works  -> a data line (D1/D2/D3) is the problem
    //   only 400 kHz works           -> signal integrity, back the clock off
    //   every rung times out         -> the card is not talking at all:
    //                                   not seated, or not powered
    ESP_LOGI(TAG, "--- mount attempts ---");
    struct { int width, khz; const char *label; } rungs[] = {
        { 4, SDMMC_FREQ_HIGHSPEED, "4-bit @ 40 MHz (default)" },
        { 4, SDMMC_FREQ_DEFAULT,   "4-bit @ 20 MHz"           },
        { 1, SDMMC_FREQ_DEFAULT,   "1-bit @ 20 MHz"           },
        { 1, SDMMC_FREQ_PROBING,   "1-bit @ 400 kHz"          },
    };

    esp_err_t err = ESP_FAIL;
    int won = -1;
    for (size_t i = 0; i < sizeof(rungs) / sizeof(rungs[0]); i++) {
        err = try_mount(rungs[i].width, rungs[i].khz, rungs[i].label);
        if (err == ESP_OK) { won = (int)i; break; }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "FAIL: no configuration mounted the card.");
        if (err == ESP_ERR_TIMEOUT) {
            ESP_LOGE(TAG, "  Every rung timed out on send_op_cond -- the card never");
            ESP_LOGE(TAG, "  answered at all. The LDO rail IS on, so this is not a power");
            ESP_LOGE(TAG, "  problem and not a filesystem one either. Check, in order:");
            ESP_LOGE(TAG, "    1. the card is pushed fully in until it clicks");
            ESP_LOGE(TAG, "    2. it is a microSD, not an adapter with nothing in it");
            ESP_LOGE(TAG, "    3. the card reads in a computer (a dead card looks like this)");
        } else if (err == ESP_FAIL) {
            ESP_LOGE(TAG, "  Card answered but the filesystem would not mount -- that IS");
            ESP_LOGE(TAG, "  a format problem. Must be FAT32 (exFAT is not supported).");
        }
        return;
    }

    ESP_LOGI(TAG, "PASS: mounted with \"%s\"", rungs[won].label);
    if (won > 0) {
        ESP_LOGW(TAG, "  NOTE: the default configuration did NOT work. board.h should");
        ESP_LOGW(TAG, "  record width %d / %d kHz, or the app will fail the same way.",
                 rungs[won].width, rungs[won].khz);
    }

    sdmmc_card_print_info(stdout, card);

    uint64_t total = 0, freeb = 0;
    if (esp_vfs_fat_info(BSP_SD_MOUNT, &total, &freeb) == ESP_OK) {
        ESP_LOGI(TAG, "  capacity %.2f GB, free %.2f GB", total / 1e9, freeb / 1e9);
    }

    bench();

    // Deliberately left MOUNTED: gate 0.7 needs a real photo off this card,
    // and remounting between the two would only add a way to fail.
    ESP_LOGI(TAG, "  (card left mounted for gate 0.7)");
    ESP_LOGI(TAG, "=== Stage 0.3 complete ===");
}

void gate_sdcard_unmount(void)
{
    if (card) {
        esp_vfs_fat_sdcard_unmount(BSP_SD_MOUNT, card);
        card = NULL;
    }
}
