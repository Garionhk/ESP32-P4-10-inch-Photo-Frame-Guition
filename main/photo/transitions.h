// ===========================================================================
// transitions.h -- the transition catalogue
// ===========================================================================
// Every one of these is built on ppa_do_blend, NOT on scale-rotate-mirror,
// and that is a measurement rather than a preference. Gate 0.4:
//
//   blend fullscreen        18.72 ms   313.0 MiB/s   53.4 fps
//   blend alpha=255 (copy)  18.50 ms   316.7 MiB/s   54.0 fps
//   srm 1:1 copy            34.51 ms   113.2 MiB/s   29.0 fps
//
// A blend with a fully opaque foreground IS a copy, and it runs 1.87x faster
// than the SRM engine despite moving MORE bytes (three framebuffers to two).
// Built the obvious way on SRM, the geometric transitions would have been the
// slowest things in the set. SRM is reserved for the per-photo fit pass and
// for zoom, the two jobs that genuinely need geometry.
#pragma once

#include "photo.h"
#include <stdint.h>

typedef enum {
    TR_CROSSFADE = 0,
    TR_SLIDE_LEFT,
    TR_SLIDE_RIGHT,
    TR_SLIDE_UP,
    TR_SLIDE_DOWN,
    TR_PUSH_LEFT,
    TR_WIPE_LEFT,
    TR_BARN_OPEN,
    TR_BLINDS,
    TR_DISSOLVE_BANDS,
    TR_COUNT
} transition_t;

const char *transition_name(transition_t t);

// Render one step of a transition into `dst`. `p` runs 0.0 -> 1.0.
// `from` and `to` are display-ready frames at dst's geometry.
esp_err_t transition_step(transition_t t, const frame_t *from, const frame_t *to,
                          float p, frame_t *dst);

esp_err_t transitions_init(void);

// Blend an ARGB8888 overlay strip over a finished frame, using the layer's own
// per-pixel alpha. `opacity` scales that alpha; 255 leaves it as authored.
esp_err_t transition_overlay(const void *argb, int stride, int ov_x, int ov_y,
                             int ov_w, int ov_h, uint8_t opacity, frame_t *dst);
