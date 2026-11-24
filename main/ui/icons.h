// ===========================================================================
// icons.h -- weather condition and moon phase glyphs
// ===========================================================================
// Drawn with LVGL canvas primitives rather than shipped as bitmaps: a moon
// phase changes every night and a condition icon has to cover the whole WMO
// range, so a handful of circles and lines beats an image set that would have
// to be regenerated to change a colour.
#pragma once

#include "lvgl.h"

#define ICON_SIZE 64

// Create a canvas of ICON_SIZE and return it. The caller owns placement.
lv_obj_t *icon_create(lv_obj_t *parent);

// A canvas of arbitrary size, for the daylight arc.
lv_obj_t *icon_create_sized(lv_obj_t *parent, int w, int h);

// Layout 3b's daylight track: the whole arc in the deep step, the elapsed
// portion in the accent, and a dot at the sun's current position.
// `frac` is 0 at sunrise and 1 at sunset; outside the day, pass <0 or >1.
#define ARC_W 300
#define ARC_H 40
void icon_draw_daylight(lv_obj_t *canvas, float frac);

// A solid triangle, for the high/low markers. The design draws these as 12x12
// SVGs; LV_SYMBOL_UP/DOWN cannot be used because those glyphs live in LVGL's
// bundled Montserrat faces, and the strip is set in IBM Plex.
#define TRI_SIZE 14
void icon_draw_triangle(lv_obj_t *canvas, bool up, uint32_t rgb);

// Redraw for a WMO weather code (as Open-Meteo returns it).
void icon_draw_weather(lv_obj_t *canvas, int wmo_code);

// Redraw for a moon phase in [0,1]: 0 and 1 are new, 0.5 is full.
void icon_draw_moon(lv_obj_t *canvas, float phase);
