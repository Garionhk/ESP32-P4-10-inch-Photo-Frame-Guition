// ===========================================================================
// sdcard.h -- microSD on SDMMC slot 0
// ===========================================================================
// The one non-obvious part is the power rail: the card's VDD and all six bus
// pull-ups hang off the P4's internal LDO_VO4 (schematic sheet 4_CONN), so it
// has to be switched on in software. Miss it and every mount times out at every
// width and every clock -- indistinguishable from a missing or dead card.
#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

esp_err_t sdcard_mount(void);
void      sdcard_unmount(void);
bool      sdcard_mounted(void);

// Capacity/free in bytes. Returns false if not mounted.
bool sdcard_usage(uint64_t *total, uint64_t *freeb);
