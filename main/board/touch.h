// ===========================================================================
// touch.h -- GSL3680 capacitive panel, reduced to gestures
// ===========================================================================
// The panel reports raw points; the frame only cares about four things. All
// gesture decisions are made ON RELEASE, so one press can produce exactly one
// event -- never a tap that also fires as a swipe.
#pragma once

#include "esp_err.h"
#include <stdbool.h>

typedef enum {
    GESTURE_NONE = 0,
    GESTURE_TAP,          // next photo
    GESTURE_DOUBLE_TAP,   // previous photo
    GESTURE_SWIPE_UP,     // show the info panel
    GESTURE_SWIPE_DOWN,   // hide the info panel
} gesture_t;

// Needs the panel powered, so call this AFTER display_init(): the GSL3680
// shares the display's supply and is held in reset until GPIO 22 is released.
// Claim I2C port 1 with the new-style driver BEFORE the panel is created.
//
// The JD9365 driver opens the same port with the LEGACY i2c_bus driver (it
// pokes a backlight controller at 0x45 that this board does not have). The two
// drivers cannot share a port, and whoever gets there second is left with a
// handle that fails every transfer -- which is why touch initialisation could
// not even create its panel IO. Claiming it first costs the JD9365 nothing,
// since the device it wants is absent here anyway.
esp_err_t touch_bus_init(void);

esp_err_t touch_init(void);

// Take the next recognised gesture, or GESTURE_NONE if there is none waiting.
//
// Polling runs in its own task rather than on the render loop. It has to:
// a transition takes ~700 ms during which the render loop is not looking at
// the panel at all, so the second half of a double tap was simply lost, and a
// tap during a transition did nothing.
gesture_t touch_take(void);

const char *gesture_name(gesture_t g);
