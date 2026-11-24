// ===========================================================================
// overlay.h -- the bottom-fifth information strip
// ===========================================================================
// Four items, each independently switchable, packed into pages of at most two.
// The packing rule, exactly as specified:
//
//   4 enabled -> 2 pages:  [Clock | Temp]  and  [Sun | AQI]
//   3 enabled -> 2 pages:  two on the first, one on the second
//   2 enabled -> ONE page showing both, no rotation
//   1 enabled -> one page
//   0 enabled -> no overlay at all; photos run full bleed
//
// LVGL renders into a standalone ARGB8888 layer and never touches the panel.
// The compositor PPA-blends the strip over each photo frame, which is what
// makes the bar translucent rather than a solid cut-out.
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    OV_CLOCK = 0,
    OV_WEATHER,
    OV_SUN,
    OV_AIRQUALITY,
    OV_ITEM_COUNT
} overlay_item_t;

typedef struct {
    bool     enabled[OV_ITEM_COUNT];
    uint8_t  opacity;        // 0-255, applied on top of the bar's own alpha
    uint32_t page_ms;        // dwell per page
} overlay_cfg_t;

// w/h are the FULL display dimensions; the strip is sized from them.
esp_err_t overlay_init(int display_w, int display_h, const overlay_cfg_t *cfg);

// Re-pack pages after an enable-mask change.
void overlay_apply_config(const overlay_cfg_t *cfg);

// Call regularly. Advances the page rotation and refreshes live values.
// Returns true when the strip changed and needs recompositing. The values
// refresh twice a second, so a caller that repaints unconditionally does ~14
// pointless 2 MB composites per second -- and only looks at the touch queue
// between them.
bool overlay_tick(void);

// Swipe down hides the strip, swipe up brings it back. Hiding is a display
// state, not a configuration change -- it is not persisted, so a restart
// brings the panel back.
void overlay_set_hidden(bool hidden);
bool overlay_hidden(void);

// Advance to the next page. Called when the PHOTO changes rather than on a
// timer of its own, so the strip and the picture move together.
void overlay_next_page(void);

// True when there is anything to draw at all.
bool overlay_visible(void);

// Where the strip sits IN PANEL COORDINATES, whatever the mounting.
//
// The strip is authored along the bottom of the VIEW. In landscape the view is
// turned 90 degrees relative to the panel, so "the bottom" is a vertical band
// down one side of the panel -- which is why a strip placed naively at panel
// y ended up running down the left edge.
int overlay_x(void);
int overlay_y(void);
int overlay_width(void);
int overlay_height(void);

// The ARGB8888 layer, strip-sized. Rows are `overlay_stride()` bytes apart.
const void *overlay_pixels(void);
int overlay_stride(void);

int overlay_page_count(void);
