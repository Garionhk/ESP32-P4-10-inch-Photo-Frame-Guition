// ===========================================================================
// theme.h -- overlay geometry and the theme ramps
// ===========================================================================
// Geometry and colour both come from the "Frame Overlay" design doc, layout
// 2a ("Two-tier rail"). The design was drawn at true 1:1 against this panel,
// so these are the design's own pixel values rather than a translation:
//
//   204 px  =  8 px gradient hairline
//           + 132 px data row   (three cells, 1 px dividers, 24 px padding)
//           +  64 px status row (housekeeping, one small size)
//
// Colour is one hue in four steps plus a background. Seven ramps, switched by
// a single index -- which is exactly how the design describes it, and why the
// firmware needs no per-theme code.
#pragma once

#include <stdint.h>

#define OVERLAY_NUM        4       // 4/25 = 16% of display height
#define OVERLAY_DEN        25

#define STRIP_HAIRLINE_H   8
#define STRIP_DATA_H       132
#define STRIP_STATUS_H     64
#define STRIP_PAD_X        24      // every cell is 24 px padded
#define STRIP_RHYTHM       8       // every stack uses an 8 px rhythm

// --- Landscape: the "Frame Landscape" design, a quiet two-row ticker -------
//
// Not a rotation of layout 2a. Landscape gets 800 px of height to spend and
// 1280 of width, so the design abandons stacked cells entirely: everything
// sits on TWO SINGLE-LINE ROWS, which is what makes it impossible to clip a
// cell the way the portrait build did.
//
//   88 px  =  4 px gradient hairline
//          + 48 px row A (the data, one line, space-between)
//          + 36 px row B (housekeeping, one line)
//
// 88 rather than the portrait strip's 16% of height, which would be 128. The
// design fixes the band and gives the extra 40 px back to the photo:
// 1280x712 of picture instead of 1280x672.
#define BAND_HAIRLINE_H    4
#define BAND_ROWA_H        48
#define BAND_ROWB_H        36
#define BAND_H             (BAND_HAIRLINE_H + BAND_ROWA_H + BAND_ROWB_H)   // 88
#define BAND_PAD_X         32      // inset, both rows
#define BAND_GAP           18      // between groups on row A
#define BAND_PAIR_GAP      7       // arrow to its number

// One ramp: [t1, acc, dim, deep, bg] -- primary text, accent, dim text,
// deep shade, background.
typedef struct {
    const char *name;
    uint32_t    t1, acc, dim, deep, bg;
} theme_t;

#define THEME_COUNT 7
extern const theme_t THEMES[THEME_COUNT];

// The active ramp, chosen by g_settings.theme.
const theme_t *theme_now(void);

// AQI and UV are reported as TEXT only. The band colours that normally carry
// this information (green through to maroon) are deliberately not used: the
// strip is one hue, and a second palette inside it would read as a fault
// rather than as a scale. The word does the work instead -- "Moderate",
// "Unhealthy" -- which is unambiguous without relying on colour at all.
const char *theme_aqi_label(int aqi);
const char *theme_weather_text(int code);
