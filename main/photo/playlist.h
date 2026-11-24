// ===========================================================================
// playlist.h -- what to show next
// ===========================================================================
// A shuffled walk that never repeats until the pool is exhausted, because a
// frame that shows the same photo twice in a minute looks broken even though
// random sampling would do exactly that.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PLAYLIST_MAX      512
#define PLAYLIST_PATH_MAX 256

// Walk the card and collect decodable photos. Files are CLASSIFIED, not
// trusted by extension -- 2 of 7 files on the first test card were PNGs
// wearing a .jpeg name.
int playlist_scan(const char *root);

// One row of the browse listing. Includes files the playlist itself rejected,
// so a renamed PNG can be found and removed rather than sitting there
// invisible and undeletable.
typedef struct {
    char     path[PLAYLIST_PATH_MAX];
    uint32_t size;
    bool     usable;          // false = present but not decodable
} photo_info_t;

// Enumerate every .jpg/.jpeg on the card, usable or not.
int playlist_enumerate(photo_info_t *out, int max);

int  playlist_count(void);
int  playlist_rejected(void);       // found, but not decodable
// Next path in the shuffled order. NULL if the playlist is empty.
const char *playlist_next(void);

// Step back through what has already been shown. A double tap means "I want
// the one before", which only works if the order is remembered -- a reshuffled
// walk cannot be run backwards.
const char *playlist_prev(void);
