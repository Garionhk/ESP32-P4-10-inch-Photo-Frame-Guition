// ===========================================================================
// lv_mem_psram.c -- LVGL allocates from PSRAM, never from DMA-capable RAM
// ===========================================================================
// This exists because of a measurement, not a preference:
//
//   after fonts:   DMA 41039 free / 23552 largest
//   after overlay: DMA 16571 free /  5632 largest   <-- SD card fails here
//
// Building the overlay was consuming ~24 KB of DMA-capable internal RAM, and
// the SD driver then could not allocate a transfer buffer at all. Every
// photo read failed with allocate_dma_buf, and the TLS handshake failed too --
// which surfaced as getaddrinfo errors that look nothing like memory pressure.
//
// LVGL has no need of DMA-capable memory. Its one large buffer, the ARGB
// overlay layer, is allocated by us in PSRAM and handed to it; everything
// else is widgets, styles and glyph caches that the CPU alone touches. So it
// gets PSRAM exclusively, and the scarce internal pool is left to the things
// that genuinely require it: the SD card, the SDIO link to the C6, and TLS.
//
// This is the right lever. Tuning SPIRAM_MALLOC_ALWAYSINTERNAL was not: it
// moves EVERY small allocation in the firmware, and dropping it to 128 broke
// lwip's resolver while still leaving LVGL partly in internal RAM.
#include "lvgl.h"
#include "esp_heap_caps.h"

void lv_mem_init(void)   { }
void lv_mem_deinit(void) { }

void lv_mem_remove_pool(lv_mem_pool_t pool) { LV_UNUSED(pool); }

void *lv_malloc_core(size_t size)
{
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void *lv_realloc_core(void *p, size_t new_size)
{
    return heap_caps_realloc(p, new_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void lv_free_core(void *p)
{
    heap_caps_free(p);
}

void lv_mem_monitor_core(lv_mem_monitor_t *mon)
{
    // Report the PSRAM pool, since that is where LVGL now lives.
    multi_heap_info_t info;
    heap_caps_get_info(&info, MALLOC_CAP_SPIRAM);
    mon->total_size = info.total_free_bytes + info.total_allocated_bytes;
    mon->free_cnt   = info.free_blocks;
    mon->free_size  = info.total_free_bytes;
    mon->free_biggest_size = info.largest_free_block;
    mon->used_cnt   = info.allocated_blocks;
    mon->max_used   = info.total_allocated_bytes;
    mon->used_pct   = mon->total_size ? (uint8_t)(100 * mon->max_used / mon->total_size) : 0;
    mon->frag_pct   = 0;
}

lv_result_t lv_mem_test_core(void)
{
    return LV_RESULT_OK;
}
