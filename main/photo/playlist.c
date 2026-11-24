#include "playlist.h"
#include "photo.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <strings.h>
#include "esp_log.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "board.h"

static const char *TAG = "playlist";

static char (*s_paths)[PLAYLIST_PATH_MAX];
static int   s_count;
static int   s_rejected;
static int   s_order[PLAYLIST_MAX];
static int   s_pos;

static void reshuffle(void)
{
    for (int i = 0; i < s_count; i++) s_order[i] = i;
    // Fisher-Yates. Every photo appears exactly once per pass, which is the
    // whole point -- true random sampling repeats far too visibly.
    for (int i = s_count - 1; i > 0; i--) {
        int j = (int)(esp_random() % (uint32_t)(i + 1));
        int t = s_order[i]; s_order[i] = s_order[j]; s_order[j] = t;
    }
    s_pos = 0;
}

static void scan_dir(const char *dir, int depth)
{
    if (depth > 4 || s_count >= PLAYLIST_MAX) return;
    DIR *d = opendir(dir);
    if (!d) return;

    struct dirent *e;
    while ((e = readdir(d)) != NULL && s_count < PLAYLIST_MAX) {
        // Skips . / .. and the AppleDouble and Spotlight droppings a Mac
        // leaves on any card it touches.
        if (e->d_name[0] == '.') continue;

        char path[PLAYLIST_PATH_MAX];
        if (snprintf(path, sizeof(path), "%s/%s", dir, e->d_name) >= (int)sizeof(path)) continue;

        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) { scan_dir(path, depth + 1); continue; }

        const char *dot = strrchr(e->d_name, '.');
        if (!dot) continue;
        if (strcasecmp(dot, ".jpg") != 0 && strcasecmp(dot, ".jpeg") != 0) continue;

        // The extension got us this far; the header decides. A PNG named .jpeg
        // must never reach the decoder.
        int w = 0, h = 0;
        photo_err_t cls = photo_probe(path, &w, &h);
        if (cls != PHOTO_OK) {
            ESP_LOGW(TAG, "  skip %s: %s", path, photo_err_str(cls));
            s_rejected++;
            continue;
        }
        strncpy(s_paths[s_count], path, PLAYLIST_PATH_MAX - 1);
        s_paths[s_count][PLAYLIST_PATH_MAX - 1] = '\0';
        s_count++;
    }
    closedir(d);
}

int playlist_scan(const char *root)
{
    if (!s_paths) {
        s_paths = heap_caps_malloc((size_t)PLAYLIST_MAX * PLAYLIST_PATH_MAX, MALLOC_CAP_SPIRAM);
        if (!s_paths) { ESP_LOGE(TAG, "no PSRAM for the path table"); return 0; }
    }
    s_count = 0; s_rejected = 0;
    scan_dir(root, 0);
    ESP_LOGI(TAG, "%d photo(s) usable, %d rejected, under %s", s_count, s_rejected, root);
    if (s_count) reshuffle();
    return s_count;
}

// A second walk rather than a filtered view of the playlist: the playlist
// deliberately holds only decodable photos, and the browse list has to show
// the ones that were skipped too.
static void enum_dir(const char *dir, int depth, photo_info_t *out, int max, int *n)
{
    if (depth > 4 || *n >= max) return;
    DIR *d = opendir(dir);
    if (!d) return;

    struct dirent *e;
    while ((e = readdir(d)) != NULL && *n < max) {
        if (e->d_name[0] == '.') continue;

        char path[PLAYLIST_PATH_MAX];
        if (snprintf(path, sizeof(path), "%s/%s", dir, e->d_name) >= (int)sizeof(path)) continue;

        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) { enum_dir(path, depth + 1, out, max, n); continue; }

        const char *dot = strrchr(e->d_name, '.');
        if (!dot) continue;
        if (strcasecmp(dot, ".jpg") != 0 && strcasecmp(dot, ".jpeg") != 0) continue;

        photo_info_t *p = &out[(*n)++];
        strncpy(p->path, path, sizeof(p->path) - 1);
        p->path[sizeof(p->path) - 1] = '\0';
        p->size   = (uint32_t)st.st_size;
        p->usable = (photo_probe(path, NULL, NULL) == PHOTO_OK);
    }
    closedir(d);
}

int playlist_enumerate(photo_info_t *out, int max)
{
    int n = 0;
    enum_dir(BSP_SD_MOUNT, 0, out, max, &n);
    return n;
}

int playlist_count(void)    { return s_count; }
int playlist_rejected(void) { return s_rejected; }

// Recently shown, newest last. Bounded: going back further than this is not
// something anyone does, and an unbounded history on a device that runs for
// weeks is just a slow leak.
#define HISTORY 32
static int s_hist[HISTORY];
static int s_hist_len;      // how many entries are valid
static int s_back;          // how far back the user has stepped

static void history_push(int idx)
{
    if (s_hist_len == HISTORY) {
        memmove(s_hist, s_hist + 1, sizeof(int) * (HISTORY - 1));
        s_hist_len--;
    }
    s_hist[s_hist_len++] = idx;
}

const char *playlist_next(void)
{
    if (s_count == 0) return NULL;

    // If the user stepped back and now goes forward, retrace the history
    // rather than shuffling on -- otherwise "back, forward" lands somewhere
    // new, which reads as the frame losing its place.
    if (s_back > 0) {
        s_back--;
        return s_paths[s_hist[s_hist_len - 1 - s_back]];
    }

    if (s_pos >= s_count) reshuffle();       // pool exhausted: new pass
    const int idx = s_order[s_pos++];
    history_push(idx);
    return s_paths[idx];
}

const char *playlist_prev(void)
{
    if (s_count == 0) return NULL;
    // Nothing older to show: stay where we are rather than jumping randomly.
    if (s_back + 1 >= s_hist_len) return NULL;
    s_back++;
    return s_paths[s_hist[s_hist_len - 1 - s_back]];
}
