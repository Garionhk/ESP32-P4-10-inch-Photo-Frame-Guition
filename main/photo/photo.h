// ===========================================================================
// photo.h -- load a file from the card into a display-ready frame
// ===========================================================================
// Everything here is shaped by three Stage 0 measurements:
//
//   0.5  HW decode of a <=2048 px JPEG costs 10-16 ms. Fast path.
//   0.7  SW decode of a 12 MP photo costs ~1.5 s, and downscaling during
//        decode saves MEMORY, not time (1/8 is only 1.7x faster than 1/1).
//   0.7  2 of 7 files named .jpg on the test card were PNGs. Extension is
//        not format, and progressive JPEGs share baseline's magic bytes.
#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

// A frame at panel-native geometry, ready to composite. RGB565, PSRAM,
// cache-line aligned so the PPA can use it directly.
typedef struct {
    uint16_t *pixels;
    int       w, h;
} frame_t;

typedef enum {
    PHOTO_OK = 0,
    PHOTO_ERR_NOT_JPEG,       // a PNG, or not an image at all
    PHOTO_ERR_PROGRESSIVE,    // valid JPEG, but esp_new_jpeg is baseline-only
    PHOTO_ERR_MALFORMED,
    PHOTO_ERR_IO,
    PHOTO_ERR_MEMORY,
} photo_err_t;

const char *photo_err_str(photo_err_t e);

esp_err_t frame_alloc(frame_t *f, int w, int h);
void      frame_free(frame_t *f);

// Decode `path` and fit it into `dst` at dst's geometry, letterboxed to
// preserve aspect. Rotation for landscape/portrait happens here -- ONCE per
// photo, in the same PPA pass as the scale, which is what makes the
// orientation setting free.
photo_err_t photo_load(const char *path, frame_t *dst, bool landscape);

// Classify without decoding. Cheap: reads only the header.
photo_err_t photo_probe(const char *path, int *w, int *h);
