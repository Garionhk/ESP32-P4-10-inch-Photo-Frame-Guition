// ===========================================================================
// immich.h -- photos from a self-hosted Immich server
// ===========================================================================
// Two things make this cheap on a device this size:
//
//   * We ask for the "preview" rendition, not the original. Immich renders it
//     server-side at roughly 1440 px, which is inside the P4 hardware
//     decoder's range -- so an Immich photo skips the whole software-decode
//     ingest path that a 12 MP original would need.
//   * Downloads stream to the card. Nothing is buffered whole.
//
// A failure is never fatal: the frame falls back to the SD library, which is
// the same last-good rule the weather layer follows.
#pragma once

#include <stdbool.h>
#include <stddef.h>

// Selection of what the frame plays.
typedef enum {
    SOURCE_SD_ONLY = 0,
    SOURCE_IMMICH_ONLY,
    SOURCE_BOTH,
} source_mode_t;

// Fetch one random photo from the configured albums into the SD cache.
// `out_path` receives the cached file's path. Returns false on any failure,
// including "not configured" -- the caller falls back to the card.
bool immich_next_photo(char *out_path, size_t out_len);

// JSON array of {id,name,count} for the settings page's album picker.
// Caller frees. NULL if unreachable or unconfigured.
char *immich_albums_json(void);

// True when a URL and API key are both set.
bool immich_configured(void);

// Trim the cache to the configured size, oldest first.
void immich_prune_cache(void);
