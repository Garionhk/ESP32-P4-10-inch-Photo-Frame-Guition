// ===========================================================================
// fonts.h -- IBM Plex Sans Condensed, rendered at the design's exact sizes
// ===========================================================================
// The design doc specifies 68, 56, 28, 22, 20, 17 and 12 px. Montserrat's
// built-in faces stop at 48 and are considerably wider, which is what forced
// the clock to be a blurry 1.42x scale and clipped the right-hand cell.
//
// These are rasterised from an embedded TTF, so the sizes are the design's
// sizes rather than the nearest available.
#pragma once

#include "lvgl.h"

// Call once, after lv_init(). Only the faces the ACTIVE layout uses are
// built: portrait's 2a rail and landscape's ticker share almost no sizes, and
// a face costs a glyph cache whether or not anything draws with it.
// Returns false if a face could not be built.
bool fonts_init(bool landscape);

extern const lv_font_t *FONT_TIME;    // 68 semibold
extern const lv_font_t *FONT_VALUE;   // 56 semibold
extern const lv_font_t *FONT_COND;    // 28 regular
extern const lv_font_t *FONT_DATE;    // 22 regular
extern const lv_font_t *FONT_DETAIL;  // 20 regular
extern const lv_font_t *FONT_STATUS;  // 17 regular
extern const lv_font_t *FONT_LABEL;   // 12 regular, letter-spaced captions

// Landscape ticker. The design's own scale: 36 clock, 34 big number, 30 sun
// times, 24 condition/moon, 22 secondary, 17 row B. The last two are
// FONT_DATE and FONT_STATUS, which both layouts share.
extern const lv_font_t *FONT_L_CLOCK; // 36 semibold, tabular clock
extern const lv_font_t *FONT_L_BIG;   // 34 semibold, temperature / AQI
extern const lv_font_t *FONT_L_SUN;   // 30 semibold, sunrise
extern const lv_font_t *FONT_L_SUN2;  // 30 regular,  sunset
extern const lv_font_t *FONT_L_MED;   // 24 regular,  condition / moon phrase
