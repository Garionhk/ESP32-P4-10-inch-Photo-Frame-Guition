// ===========================================================================
// display.h -- JD9365 MIPI-DSI panel, framebuffers, backlight
// ===========================================================================
// Every value this uses was established in Stage 0.1 and lives in board.h.
// Two of them would each have produced a permanently black screen:
// the backlight is GPIO 23 (not an I2C controller, and not 26/32), and the
// DPHY runs off internal LDO channel 3.
#pragma once

#include "esp_err.h"
#include "esp_lcd_types.h"
#include <stdint.h>

// Bring up the DPHY rail, the DSI bus, the JD9365 and the backlight.
esp_err_t display_init(void);

// The panel scans PORTRAIT (800x1280). Landscape is a rotation applied once
// per photo in the fit pass, never per frame -- see photo_loader.
int display_width(void);
int display_height(void);

// Framebuffers, for the compositor to render into. There are PANEL_NUM_FBS of
// them; index them 0..n-1.
void *display_framebuffer(int index);
int   display_framebuffer_count(void);

// Hand a framebuffer to the panel. Blocks until the DSI has taken it.
esp_err_t display_present(void *fb);

// 0-100. Backlight is a plain GPIO here, so this is on/off until the LEDC
// path is wired up; the signature is right so callers do not change later.
esp_err_t display_set_brightness(int percent);
