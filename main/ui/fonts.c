#include "fonts.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

static const char *TAG = "fonts";

// Embedded by CMake's EMBED_FILES. The symbols are derived from the paths.
extern const uint8_t plex_reg_start[] asm("_binary_IBMPlexSansCondensed_Regular_ttf_start");
extern const uint8_t plex_reg_end[]   asm("_binary_IBMPlexSansCondensed_Regular_ttf_end");
extern const uint8_t plex_sb_start[]  asm("_binary_IBMPlexSansCondensed_SemiBold_ttf_start");
extern const uint8_t plex_sb_end[]    asm("_binary_IBMPlexSansCondensed_SemiBold_ttf_end");

const lv_font_t *FONT_TIME, *FONT_VALUE, *FONT_COND,
                *FONT_DATE, *FONT_DETAIL, *FONT_STATUS, *FONT_LABEL;
const lv_font_t *FONT_L_CLOCK, *FONT_L_BIG, *FONT_L_SUN, *FONT_L_SUN2, *FONT_L_MED;

// Glyph cache per face, in entries.
//
// 96 was far too generous: seven faces at 96 entries each exhausted internal
// RAM and took the SD card's DMA buffers and the TLS handshake down with it.
// The strip shows about a dozen distinct glyphs per face -- ten digits, a
// colon and a degree sign on the big ones -- so 24 covers the working set and
// anything beyond it simply re-rasterises, which costs microseconds on text
// that changes twice a second.
#define CACHE 24

static lv_font_t *make(const uint8_t *start, const uint8_t *end, int size)
{
    lv_font_t *f = lv_tiny_ttf_create_data_ex((const void *)start, (size_t)(end - start),
                                              size, LV_FONT_KERNING_NORMAL, CACHE);
    if (!f) ESP_LOGE(TAG, "could not build a %d px face", size);
    return f;
}

bool fonts_init(bool landscape)
{
    // Semibold for the headline numbers, regular for everything else -- the
    // weights both designs use (600 and 400).
    //
    // The two layouts are built from different faces, and building both sets
    // would double the glyph caches for no benefit: portrait never draws at
    // 36 px and landscape never draws at 68. Only the active set is created;
    // the other pointers stay NULL and the builders that would use them do
    // not run.
    if (landscape) {
        FONT_L_CLOCK = make(plex_sb_start,  plex_sb_end,  36);
        FONT_L_BIG   = make(plex_sb_start,  plex_sb_end,  34);
        FONT_L_SUN   = make(plex_sb_start,  plex_sb_end,  30);
        FONT_L_SUN2  = make(plex_reg_start, plex_reg_end, 30);
        FONT_L_MED   = make(plex_reg_start, plex_reg_end, 24);
    } else {
        FONT_TIME   = make(plex_sb_start,  plex_sb_end,  68);
        FONT_VALUE  = make(plex_sb_start,  plex_sb_end,  56);
        FONT_COND   = make(plex_reg_start, plex_reg_end, 28);
        FONT_DETAIL = make(plex_reg_start, plex_reg_end, 20);
        FONT_LABEL  = make(plex_reg_start, plex_reg_end, 12);
    }
    // Shared by both: 22 px secondary text and the 17 px housekeeping row.
    FONT_DATE   = make(plex_reg_start, plex_reg_end, 22);
    FONT_STATUS = make(plex_reg_start, plex_reg_end, 17);

    const bool ok = FONT_DATE && FONT_STATUS &&
                    (landscape
                     ? (FONT_L_CLOCK && FONT_L_BIG && FONT_L_SUN &&
                        FONT_L_SUN2 && FONT_L_MED)
                     : (FONT_TIME && FONT_VALUE && FONT_COND &&
                        FONT_DETAIL && FONT_LABEL));
    if (ok) {
        ESP_LOGI(TAG, "IBM Plex Sans Condensed at %s",
                 landscape ? "36/34/30/30/24/22/17 (ticker)"
                           : "68/56/28/22/20/17/12 (rail)");
        // Internal RAM is the scarce resource on this chip, and the glyph
        // caches come out of it. Print what is left so a regression here is
        // visible immediately rather than as a DMA failure later.
        // MALLOC_CAP_INTERNAL is the wrong thing to watch: it counts RTC and
        // retention RAM that no DMA engine can reach. The SD card and the TLS
        // stack need DMA-CAPABLE memory, so that is what has to be reported --
        // measuring the wrong pool is how "80 KB free" sat next to a stream of
        // allocate_dma_buf failures.
        ESP_LOGI(TAG, "after fonts:  DMA %u free / %u largest   INTERNAL %u free",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    }
    return ok;
}
