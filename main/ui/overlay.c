// ===========================================================================
// overlay.c -- the bottom-strip overlay, layout 2a from the design doc
// ===========================================================================
// "Two-tier rail": a 132 px data row of three cells over a 64 px status row,
// under an 8 px gradient hairline. The design was drawn 1:1 against this
// panel, so the numbers here are its numbers.
//
// Two tiers, and the split is the point:
//   GLANCEABLE  time, date, condition, temperature, feels, high/low
//   HOUSEKEEPING setup address, signal, uptime -- one small size, one row
//
// The data row swaps between pages; the status row does not, because
// housekeeping is not something you page through.
#include "overlay.h"
#include "theme.h"
#include "app_data.h"
#include "sun_moon.h"
#include "board.h"
#include "settings.h"
#include "icons.h"
#include "fonts.h"
#include "net/wifi.h"
#include "net/webui.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "driver/ppa.h"

static const char *TAG = "overlay";

#define MAX_PAGES 2

static overlay_cfg_t  s_cfg;
static int            s_w, s_h;          // strip size in VIEW space
static int            s_px, s_py, s_pw, s_ph;   // its placement in PANEL space
static ppa_srm_rotation_angle_t s_rot;
static uint8_t       *s_layer;
// A second buffer holding the strip rotated 180. LVGL always draws the right
// way up; the rotation is a single PPA pass applied when the layer changes --
// twice a second at most, so it never touches the frame budget.
static uint8_t       *s_layer_rot;
static ppa_client_handle_t s_ppa_rot;
static void rotate_layer(void);
static lv_display_t  *s_disp;

static lv_obj_t *s_page[MAX_PAGES];
static int       s_page_count;
static int       s_cur_page;
static bool      s_hidden;
static bool      s_dirty;

// Page 1 (clock / condition / temperature)
static lv_obj_t *s_time, *s_date, *s_cond_lbl, *s_cond_txt, *s_icon_wx;
static lv_obj_t *s_temp, *s_feels, *s_hi, *s_lo;
static lv_obj_t *s_tri_up, *s_tri_dn, *s_hilo_row;
// Page 2 (sun / moon / air quality)
static lv_obj_t *s_sun_rise, *s_sun_set, *s_icon_moon, *s_moon_txt;
static lv_obj_t *s_daylen, *s_arc;
static lv_obj_t *s_aqi_num, *s_aqi_lbl, *s_aqi_pm;
// Landscape ticker extras
static bool      s_land;            // the two-row ticker layout is in use
static bool      s_moon_oneline;    // ticker writes the phase on one line
static lv_obj_t *s_aqi_seg[5];      // the 5-segment AQI scale
static lv_obj_t *s_wifi_bar[3];     // 3-bar signal glyph
static lv_obj_t *s_st_rssi;
// Status row (shared)
static lv_obj_t *s_st_addr, *s_st_up, *s_st_dot;
static lv_obj_t *s_st_pagedot[MAX_PAGES];   // layout 3c's page indicator

static bool page_has(int page);

// ---------------------------------------------------------------------------
// Small builders. Every stack uses the design's 8 px rhythm and 24 px padding.
// ---------------------------------------------------------------------------
static lv_obj_t *plain(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t colour,
                       int tracking, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(colour), 0);
    if (tracking) lv_obj_set_style_text_letter_space(l, tracking, 0);
    return l;
}

// A vertical divider between data cells: 1 px of the deep shade at 55%.
static void divider(lv_obj_t *parent, const theme_t *th)
{
    lv_obj_t *d = plain(parent);
    lv_obj_set_size(d, 1, LV_PCT(100));
    lv_obj_set_style_bg_color(d, lv_color_hex(th->deep), 0);
    lv_obj_set_style_bg_opa(d, 140, 0);      // .55 of full
}

// One data cell: fixed width, centred vertically, 24 px padding.
static lv_obj_t *cell(lv_obj_t *row, int w, lv_flex_align_t cross)
{
    lv_obj_t *c = plain(row);
    lv_obj_set_size(c, w, LV_PCT(100));
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_CENTER, cross, cross);
    lv_obj_set_style_pad_hor(c, STRIP_PAD_X, 0);
    lv_obj_set_style_pad_row(c, STRIP_RHYTHM, 0);
    return c;
}

// A filled dot: the ticker's separator between groups, and the status LED.
static lv_obj_t *dot(lv_obj_t *parent, int size, uint32_t rgb)
{
    lv_obj_t *d = plain(parent);
    lv_obj_set_size(d, size, size);
    lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(d, lv_color_hex(rgb), 0);
    lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    return d;
}

// A flat rectangle: signal bars and the AQI scale. Both are a handful of
// fixed-size blocks, which is cheaper and sharper than a canvas.
static lv_obj_t *bar(lv_obj_t *parent, int w, int h, uint32_t rgb)
{
    lv_obj_t *b = plain(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, lv_color_hex(rgb), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    return b;
}

// A horizontal group on the ticker. Height is EXPLICIT -- the row height is
// known, so there is nothing for content-sizing to resolve vertically, and a
// content-sized chain resolving to zero is precisely what made the portrait
// high/low row invisible while its geometry logged as correct.
static lv_obj_t *hgroup(lv_obj_t *parent, int gap, lv_flex_align_t cross, int h)
{
    lv_obj_t *g = plain(parent);
    lv_obj_set_flex_flow(g, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(g, LV_FLEX_ALIGN_START, cross, cross);
    lv_obj_set_style_pad_column(g, gap, 0);
    lv_obj_set_size(g, LV_SIZE_CONTENT, h);
    return g;
}

// ---------------------------------------------------------------------------
static void build_status_row(lv_obj_t *parent, const theme_t *th)
{
    lv_obj_t *row = plain(parent);
    lv_obj_set_size(row, s_w, STRIP_STATUS_H);
    lv_obj_set_pos(row, 0, STRIP_HAIRLINE_H + STRIP_DATA_H);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(row, STRIP_PAD_X, 0);
    // 1 px rule above the status row, in the dim step.
    lv_obj_set_style_border_color(row, lv_color_hex(th->dim), 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_TOP, 0);

    lv_obj_t *left = plain(row);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(left, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(left, 10, 0);
    lv_obj_set_size(left, LV_SIZE_CONTENT, LV_SIZE_CONTENT);

    // A 6 px accent dot: the design's one piece of colour down here.
    s_st_dot = plain(left);
    lv_obj_set_size(s_st_dot, 6, 6);
    lv_obj_set_style_radius(s_st_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_st_dot, lv_color_hex(th->acc), 0);
    lv_obj_set_style_bg_opa(s_st_dot, LV_OPA_COVER, 0);

    s_st_addr = label(left, FONT_STATUS, th->dim, 0, "");

    lv_obj_t *right = plain(row);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(right, 26, 0);
    lv_obj_set_size(right, LV_SIZE_CONTENT, LV_SIZE_CONTENT);

    // Layout 3c puts a page indicator here rather than the signal/battery
    // cluster of 2a: two 7 px dots, the active one in the accent. It tells you
    // the strip is rotating rather than stuck, which is worth more on a wall
    // than a dBm reading -- and the battery half of 2a's cluster was never
    // implementable anyway, since there is no fuel-gauge driver.
    lv_obj_t *dots = plain(right);
    lv_obj_set_flex_flow(dots, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dots, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(dots, 6, 0);
    lv_obj_set_size(dots, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    for (int i = 0; i < MAX_PAGES; i++) {
        lv_obj_t *d = plain(dots);
        lv_obj_set_size(d, 7, 7);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(d, lv_color_hex(th->deep), 0);
        s_st_pagedot[i] = d;
    }

    // Uptime takes the primary text step, as in the design.
    s_st_up = label(right, FONT_STATUS, th->t1, 0, "");
}

// Page 1: NOW | SKY | TEMP
// The design's equal thirds, restored. They were unbalanced (246/262/290)
// only because Montserrat is much wider than the condensed face the design
// assumes, which clipped the right-hand cell. With the real face the design's
// own `1fr 1px 1fr 1px 1fr` fits as drawn.
// NOT the design's equal thirds.
//
// The design puts "feels 20째  ^22째  v13째" in a 266 px cell, which leaves
// 218 px after its own 24 px padding. Measured in IBM Plex Condensed that row
// needs about 222 px BEFORE the arrows, and the arrows plus their gaps add
// another 40 -- so it overflows, and a flex row aligned to the end overflows
// to the LEFT. The result was the first items silently clipped away: "feels"
// gone entirely, the up arrow reduced to a stub, and only the low temperature
// intact. It looked like an arrow-drawing bug and was a width bug.
//
// The time and condition cells have room to spare, so the width comes from
// them and the design's proportions bend rather than its content breaking.
#define CELL_L 240
#define CELL_M 240
#define CELL_R 318      // 240 + 1 + 240 + 1 + 318 = 800

static void build_page_weather(lv_obj_t *page, const theme_t *th)
{

    // --- time and date ---
    lv_obj_t *c1 = cell(page, CELL_L, LV_FLEX_ALIGN_START);
    // 68 px for real now, not a scaled 48 -- so no transform, and the
    // layout box matches what is actually drawn.
    s_time = label(c1, FONT_TIME, th->t1, 0, "--:--");
    s_date = label(c1, FONT_DATE, th->dim, 1, "");

    divider(page, th);

    // --- condition: icon then a labelled value ---
    lv_obj_t *c2 = plain(page);
    lv_obj_set_size(c2, CELL_M, LV_PCT(100));
    lv_obj_set_flex_flow(c2, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(c2, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(c2, STRIP_PAD_X, 0);
    lv_obj_set_style_pad_column(c2, 16, 0);

    s_icon_wx = icon_create_sized(c2, 56, 56);

    lv_obj_t *c2t = plain(c2);
    lv_obj_set_flex_flow(c2t, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(c2t, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(c2t, STRIP_RHYTHM, 0);
    lv_obj_set_size(c2t, LV_SIZE_CONTENT, LV_SIZE_CONTENT);

    s_cond_lbl = label(c2t, FONT_LABEL, th->dim, 4, "CONDITION");
    s_cond_txt = label(c2t, FONT_COND, th->t1, 0, "--");
    lv_label_set_long_mode(s_cond_txt, LV_LABEL_LONG_WRAP);
    // 170 is the design's MAX width, not a fixed one. Setting it as a fixed
    // width made the icon-plus-text group 250 px wide inside a 218 px cell,
    // so the whole group sat hard left and overflowed instead of centring.
    lv_obj_set_width(s_cond_txt, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(s_cond_txt, 170, 0);

    divider(page, th);

    // --- temperature, right aligned ---
    lv_obj_t *c3 = cell(page, CELL_R, LV_FLEX_ALIGN_END);
    s_temp = label(c3, FONT_VALUE, th->t1, 0, "--");

    // EXPLICIT size, not LV_SIZE_CONTENT.
    //
    // A content-sized row inside a content-sized chain resolved to zero here,
    // and LVGL clips children to their parent -- so the arrows and numbers had
    // perfectly correct absolute coordinates and were still never painted. A
    // dump of the layer showed the whole region untouched.
    //
    // The cell has a known width and the row has a known height, so there is
    // nothing for content-sizing to work out. Saying so removes the entire
    // class of problem.
    lv_obj_t *r = plain(c3);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, 6, 0);
    lv_obj_set_size(r, CELL_R - 2 * STRIP_PAD_X, 26);
    s_hilo_row = r;

    // "feels" removed at the user's request: the row carries the high and low
    // only, which also gives them the whole cell.
    s_feels = NULL;
    // Arrow and number go DIRECTLY into the row -- no per-pair wrapper.
    //
    // They used to sit in their own LV_SIZE_CONTENT groups, which put the
    // labels three content-sized containers deep (cell -> row -> group ->
    // label). LVGL resolved those to ZERO WIDTH: the text and font were both
    // correct, but nothing was drawn, so the numbers vanished and only the
    // arrows remained. Flattening the tree removes the nesting that caused it.
    //
    // An explicit width belts-and-braces it: these hold at most "-40°", so a
    // fixed box also guarantees the row's total width is predictable.
    s_tri_up = icon_create_sized(r, TRI_SIZE, TRI_SIZE);
    s_hi     = label(r, FONT_STATUS, th->acc, 0, "");
    lv_obj_set_width(s_hi, 46);
    lv_obj_set_style_pad_right(s_hi, 8, 0);   // space before the next pair

    s_tri_dn = icon_create_sized(r, TRI_SIZE, TRI_SIZE);
    s_lo     = label(r, FONT_STATUS, th->dim, 0, "");
    lv_obj_set_width(s_lo, 46);

    // Drawn from build() after lv_obj_update_layout(), not here.
}

// Page 2: the same three-cell rail carrying sun, moon and air quality.
// Not in the design doc -- see the note in the commit/summary. It exists so
// the two overlay items the design omits are not silently dropped.
// Screen 2, layout 3b: a daylight track, a compact moon, and the AQI.
//
// The design's columns are 1.45fr / 1fr / 0.85fr rather than screen 1's equal
// thirds -- the arc needs the width and the AQI does not. 3c notes that the
// column rules should land at identical x on both screens so nothing jumps
// during the cross-fade; that is true of 3c's own screen 2, but 3b deliberately
// re-weights them to make room for the track. Following 3b is the instruction,
// so the rules do move between screens here.
#define SUN_C1 350      // 1.45fr
#define SUN_C2 242      // 1.00fr
#define SUN_C3 206      // 0.85fr   (350 + 1 + 242 + 1 + 206 = 800)

static void build_page_sun(lv_obj_t *page, const theme_t *th)
{
    // --- daylight: label + length, the arc, then the two endpoints ---
    lv_obj_t *c1 = plain(page);
    lv_obj_set_size(c1, SUN_C1, LV_PCT(100));
    lv_obj_set_flex_flow(c1, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(c1, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(c1, STRIP_PAD_X, 0);
    lv_obj_set_style_pad_ver(c1, 14, 0);

    lv_obj_t *top = plain(c1);
    lv_obj_set_size(top, SUN_C1 - 2 * STRIP_PAD_X, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    label(top, FONT_LABEL, th->dim, 5, "DAYLIGHT");
    s_daylen = label(top, FONT_STATUS, th->dim, 0, "");

    s_arc = icon_create_sized(c1, ARC_W, ARC_H);

    lv_obj_t *ends = plain(c1);
    lv_obj_set_size(ends, SUN_C1 - 2 * STRIP_PAD_X, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(ends, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ends, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    // Sunrise and sunset read as the ENDS OF THE TRACK rather than as two
    // loose numbers -- which is the whole point of the arc.
    s_sun_rise = label(ends, FONT_COND, th->t1, 0, "--:--");
    s_sun_set  = label(ends, FONT_COND, th->t1, 0, "--:--");

    divider(page, th);

    // --- moon ---
    lv_obj_t *c2 = plain(page);
    lv_obj_set_size(c2, SUN_C2, LV_PCT(100));
    lv_obj_set_flex_flow(c2, LV_FLEX_FLOW_COLUMN);
    // Centred, not left-aligned: it is the middle cell of three and read as
    // a unit with the disc above the caption.
    lv_obj_set_flex_align(c2, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(c2, 12, 0);
    lv_obj_set_style_pad_row(c2, 8, 0);
    label(c2, FONT_LABEL, th->dim, 5, "MOON");
    // 44 px, as layout 3b draws it. At the 64 px default the caption, disc and
    // two text lines came to more than the 132 px row, so the last line was
    // cut off -- which is why "73% lit" was missing its bottom.
    s_icon_moon = icon_create_sized(c2, 44, 44);
    s_moon_txt = label(c2, FONT_DETAIL, th->t1, 0, "");
    lv_label_set_long_mode(s_moon_txt, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_moon_txt, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_moon_txt, SUN_C2 - 24);

    divider(page, th);

    // --- air quality, right aligned inside its padding ---
    lv_obj_t *c3 = plain(page);
    lv_obj_set_size(c3, SUN_C3, LV_PCT(100));
    lv_obj_set_flex_flow(c3, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(c3, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_hor(c3, STRIP_PAD_X, 0);
    lv_obj_set_style_pad_ver(c3, 14, 0);
    label(c3, FONT_LABEL, th->dim, 5, "AQI");
    // 62 px in the design; FONT_VALUE is 56 and close enough that a whole
    // extra face is not worth the flash and the glyph cache.
    s_aqi_num = label(c3, FONT_VALUE, th->t1, 0, "--");
    lv_obj_t *bot = plain(c3);
    lv_obj_set_flex_flow(bot, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(bot, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_row(bot, 5, 0);
    lv_obj_set_size(bot, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    s_aqi_lbl = label(bot, FONT_DETAIL, th->acc, 0, "");
    s_aqi_pm  = label(bot, FONT_STATUS, th->dim, 0, "");
}

// ===========================================================================
// Landscape: the "Frame Landscape" ticker
// ===========================================================================
// The portrait rail cannot simply be rotated into this space. Landscape has
// 1280 px of width and only 88 px of band, so the design drops cells, rules
// and stacked captions entirely: two single-line rows, groups pushed to the
// outer edges, separated by 4 px dots.
//
// SPACING. The design uses an 18 px gap between groups but only 7 px between
// an arrow and the number it marks. Both come from ONE 7 px column gap plus an
// 11 px left MARGIN on whichever item begins a new group -- LVGL's flex adds
// margins to both placement and content width, so this needs no wrappers.
// That matters: the portrait build nested its high/low pair three
// content-sized containers deep, LVGL resolved the labels to zero width, and
// correct text in a correct font was never painted. Every item here is a
// direct child of its group.
#define BAND_LEAD (BAND_GAP - BAND_PAIR_GAP)   // 11: turns the 7 px gap into 18

// Screen 1 -- TIME & WEATHER.
static void build_land_page_weather(lv_obj_t *page, const theme_t *th)
{
    // Left: clock and date, bottom-aligned. The design aligns them on the
    // baseline; LVGL has no baseline alignment, and with a 36 px number beside
    // 22 px text the bottom edge is within a pixel of it.
    lv_obj_t *l = hgroup(page, 16, LV_FLEX_ALIGN_END, BAND_ROWA_H);
    s_time = label(l, FONT_L_CLOCK, th->t1, 0, "--:--");
    s_date = label(l, FONT_DATE, th->dim, 1, "");

    // Right: icon, condition, temperature, feels-like, then the high/low pair.
    lv_obj_t *r = hgroup(page, BAND_PAIR_GAP, LV_FLEX_ALIGN_CENTER, BAND_ROWA_H);

    s_icon_wx = icon_create_sized(r, 30, 30);

    s_cond_txt = label(r, FONT_L_MED, th->t1, 0, "--");
    lv_obj_set_style_margin_left(s_cond_txt, BAND_LEAD, 0);
    // One line, always. Row A is 48 px and there is no second line to wrap to.
    lv_label_set_long_mode(s_cond_txt, LV_LABEL_LONG_CLIP);

    lv_obj_set_style_margin_left(dot(r, 4, th->deep), BAND_LEAD, 0);

    s_temp = label(r, FONT_L_BIG, th->t1, 0, "--");
    lv_obj_set_style_margin_left(s_temp, BAND_LEAD, 0);

    lv_obj_set_style_margin_left(dot(r, 4, th->deep), BAND_LEAD, 0);

    s_feels = label(r, FONT_DATE, th->dim, 0, "");
    lv_obj_set_style_margin_left(s_feels, BAND_LEAD, 0);

    s_tri_up = icon_create_sized(r, 13, 13);
    lv_obj_set_style_margin_left(s_tri_up, BAND_LEAD, 0);
    s_hi = label(r, FONT_DATE, th->acc, 0, "");

    s_tri_dn = icon_create_sized(r, 13, 13);
    lv_obj_set_style_margin_left(s_tri_dn, BAND_LEAD, 0);
    s_lo = label(r, FONT_DATE, th->dim, 0, "");
}

// Screen 2 -- SUN, MOON & AIR.
static void build_land_page_sun(lv_obj_t *page, const theme_t *th)
{
    // Left: sunrise and sunset as an up/down pair, then the day length.
    // No arc here -- the 300x40 track needs a height this row does not have,
    // and the two times carry the same information on one line.
    lv_obj_t *l = hgroup(page, 14, LV_FLEX_ALIGN_CENTER, BAND_ROWA_H);
    s_arc = NULL;

    lv_obj_t *up = icon_create_sized(l, 15, 15);
    s_sun_rise = label(l, FONT_L_SUN, th->t1, 0, "--:--");
    lv_obj_t *dn = icon_create_sized(l, 15, 15);
    s_sun_set  = label(l, FONT_L_SUN2, th->dim, 0, "--:--");
    icon_draw_triangle(up, true,  th->acc);
    icon_draw_triangle(dn, false, th->dim);

    lv_obj_t *sep = dot(l, 4, th->deep);
    lv_obj_set_style_margin_left(sep, 4, 0);
    lv_obj_set_style_margin_right(sep, 4, 0);
    s_daylen = label(l, FONT_DATE, th->dim, 0, "");

    // Right: moon, then the air quality run.
    lv_obj_t *r = hgroup(page, BAND_GAP, LV_FLEX_ALIGN_CENTER, BAND_ROWA_H);

    s_icon_moon = icon_create_sized(r, 28, 28);
    s_moon_txt  = label(r, FONT_L_MED, th->t1, 0, "");
    lv_label_set_long_mode(s_moon_txt, LV_LABEL_LONG_CLIP);
    // The rail wrote the phase over two lines because its cell was 218 px
    // wide. There is room for one line here, so it gets one.
    s_moon_oneline = true;

    dot(r, 4, th->deep);

    s_aqi_num = label(r, FONT_L_BIG, th->t1, 0, "--");
    s_aqi_lbl = label(r, FONT_DATE, th->acc, 0, "");

    // The 5-segment scale. Explicit size throughout: five 13x6 blocks with
    // 2 px between them is 73 px, and saying so keeps it out of any
    // content-size resolution at all.
    lv_obj_t *segs = plain(r);
    lv_obj_set_flex_flow(segs, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(segs, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(segs, 2, 0);
    lv_obj_set_size(segs, 5 * 13 + 4 * 2, 6);
    for (int i = 0; i < 5; i++) s_aqi_seg[i] = bar(segs, 13, 6, th->deep);

    s_aqi_pm = label(r, FONT_DATE, th->dim, 0, "");
}

// Row B -- housekeeping, on both screens. No rule above it: the design
// separates the rows by position alone.
static void build_land_status_row(lv_obj_t *parent, const theme_t *th)
{
    lv_obj_t *row = plain(parent);
    lv_obj_set_size(row, s_w, BAND_ROWB_H);
    lv_obj_set_pos(row, 0, BAND_HAIRLINE_H + BAND_ROWA_H);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(row, BAND_PAD_X, 0);

    lv_obj_t *l = hgroup(row, 10, LV_FLEX_ALIGN_CENTER, BAND_ROWB_H);
    s_st_dot  = dot(l, 6, th->acc);
    s_st_addr = label(l, FONT_STATUS, th->dim, 0, "");

    lv_obj_t *r = hgroup(row, 24, LV_FLEX_ALIGN_CENTER, BAND_ROWB_H);

    // Signal: three bars rising left to right, lit according to RSSI. Aligned
    // on their BASE, so the short bar sits on the same line as the tall one.
    lv_obj_t *sig = hgroup(r, 2, LV_FLEX_ALIGN_END, 13);
    static const int bh[3] = { 4, 8, 13 };
    for (int i = 0; i < 3; i++) s_wifi_bar[i] = bar(sig, 4, bh[i], th->deep);
    s_st_rssi = label(r, FONT_STATUS, th->dim, 0, "");

    // The design also draws a battery here. There is no fuel-gauge driver --
    // the device at 0x36 reads all zeros -- so it is left out rather than
    // shown reading a number nobody measured. The board does bring the pack
    // to GPIO52 through a 68k/100k divider, which is the way in if it is
    // wanted later.

    s_st_up = label(r, FONT_STATUS, th->t1, 0, "");

    lv_obj_t *dots = hgroup(r, 6, LV_FLEX_ALIGN_CENTER, BAND_ROWB_H);
    for (int i = 0; i < MAX_PAGES; i++) s_st_pagedot[i] = dot(dots, 7, th->deep);
}

// Which pages exist, from the four enable flags.
static bool page_has(int page)
{
    if (page == 0) return s_cfg.enabled[OV_CLOCK] || s_cfg.enabled[OV_WEATHER];
    return s_cfg.enabled[OV_SUN] || s_cfg.enabled[OV_AIRQUALITY];
}

static void build(void)
{
    const theme_t *th = theme_now();

    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    memset(s_page, 0, sizeof(s_page));
    s_page_count = 0;

    // lv_obj_clean() frees the widgets but leaves every pointer below aiming
    // at freed memory. refresh() runs straight after this and would write
    // through them -- so a rebuild that drops a page (an enable-mask change,
    // or simply the other orientation's layout) has to clear them all first.
    s_time = s_date = s_cond_lbl = s_cond_txt = s_icon_wx = NULL;
    s_temp = s_feels = s_hi = s_lo = NULL;
    s_tri_up = s_tri_dn = s_hilo_row = NULL;
    s_sun_rise = s_sun_set = s_icon_moon = s_moon_txt = NULL;
    s_daylen = s_arc = NULL;
    s_aqi_num = s_aqi_lbl = s_aqi_pm = NULL;
    s_st_addr = s_st_up = s_st_dot = s_st_rssi = NULL;
    memset(s_aqi_seg,     0, sizeof(s_aqi_seg));
    memset(s_wifi_bar,    0, sizeof(s_wifi_bar));
    memset(s_st_pagedot,  0, sizeof(s_st_pagedot));
    s_moon_oneline = false;

    // The design's strip is OPAQUE (#0A0603 over the photo), not translucent.
    // The opacity setting still scales it at composite time for anyone who
    // prefers the photo showing through.
    lv_obj_set_style_bg_color(scr, lv_color_hex(th->bg), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    // The hairline: accent fading through the deep step to nothing.
    // 8 px on the portrait rail, 4 px on the landscape ticker.
    const int hair_h = s_land ? BAND_HAIRLINE_H : STRIP_HAIRLINE_H;
    lv_obj_t *hair = plain(scr);
    lv_obj_set_size(hair, s_w, hair_h);
    lv_obj_set_pos(hair, 0, 0);
    lv_obj_set_style_bg_color(hair, lv_color_hex(th->acc), 0);
    lv_obj_set_style_bg_grad_color(hair, lv_color_hex(th->deep), 0);
    lv_obj_set_style_bg_grad_dir(hair, LV_GRAD_DIR_HOR, 0);
    lv_obj_set_style_bg_opa(hair, LV_OPA_COVER, 0);

    for (int p = 0; p < MAX_PAGES; p++) {
        if (!page_has(p)) continue;
        lv_obj_t *pg = plain(scr);
        lv_obj_set_size(pg, s_w, s_land ? BAND_ROWA_H : STRIP_DATA_H);
        lv_obj_set_pos(pg, 0, hair_h);
        lv_obj_set_flex_flow(pg, LV_FLEX_FLOW_ROW);

        if (s_land) {
            // One row, two groups, pushed to the 32 px insets.
            lv_obj_set_flex_align(pg, LV_FLEX_ALIGN_SPACE_BETWEEN,
                                  LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_hor(pg, BAND_PAD_X, 0);
            if (p == 0) build_land_page_weather(pg, th);
            else        build_land_page_sun(pg, th);
        } else {
            lv_obj_set_flex_align(pg, LV_FLEX_ALIGN_START,
                                  LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            if (p == 0) build_page_weather(pg, th); else build_page_sun(pg, th);
        }

        lv_obj_set_style_opa(pg, s_page_count == 0 ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        s_page[s_page_count++] = pg;
    }

    if (s_land) build_land_status_row(scr, th); else build_status_row(scr, th);
    if (s_cur_page >= s_page_count) s_cur_page = 0;

    // Resolve the layout NOW. Everything below needs real geometry, and LVGL
    // does not compute it until it renders -- which is after this function.
    lv_obj_update_layout(scr);

    if (s_tri_up) icon_draw_triangle(s_tri_up, true,  th->acc);
    if (s_tri_dn) icon_draw_triangle(s_tri_dn, false, th->dim);


    ESP_LOGI(TAG, "theme \"%s\", %s layout, %d page(s): %s",
             th->name, s_land ? "ticker" : "rail", s_page_count,
             s_page_count == 1 ? "static" : "rotating");
}

// ---------------------------------------------------------------------------
static void refresh(void)
{
    char b[64];
    const theme_t *th = theme_now();
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);

    if (s_time) {
        strftime(b, sizeof(b), "%H:%M", &t);
        lv_label_set_text(s_time, b);
        strftime(b, sizeof(b), "%a %d %b %Y", &t);
        lv_label_set_text(s_date, b);
    }

    if (s_temp) {
        if (g_data.weatherValid) {
            const bool imp = g_settings.imperial;
            const float tc = imp ? g_data.tempC * 9.0f / 5.0f + 32.0f : g_data.tempC;
            const float fl = imp ? g_data.feelsLikeC * 9.0f / 5.0f + 32.0f : g_data.feelsLikeC;
            snprintf(b, sizeof(b), "%.0f°%c", tc, imp ? 'F' : 'C');
            lv_label_set_text(s_temp, b);
            // The ticker carries "feels 15°" as the design draws it. The rail
            // does not -- it was dropped from the portrait cell on request,
            // and that cell has no room for it anyway.
            if (s_feels) {
                snprintf(b, sizeof(b), "feels %.0f°", fl);
                lv_label_set_text(s_feels, b);
            }
            if (g_data.dailyValid) {
                const float hi = imp ? g_data.tempMaxC * 9.0f / 5.0f + 32.0f : g_data.tempMaxC;
                const float lo = imp ? g_data.tempMinC * 9.0f / 5.0f + 32.0f : g_data.tempMinC;
                snprintf(b, sizeof(b), "%.0f°", hi);
                lv_label_set_text(s_hi, b);
                snprintf(b, sizeof(b), "%.0f°", lo);
                lv_label_set_text(s_lo, b);
            }
            lv_label_set_text(s_cond_txt, theme_weather_text(g_data.weatherCode));
        } else {
            lv_label_set_text(s_temp, "--");
            if (s_feels) lv_label_set_text(s_feels, "");
            lv_label_set_text(s_hi, "");
            lv_label_set_text(s_lo, "");
            lv_label_set_text(s_cond_txt, "--");
        }
        // Hide the markers when there is no high/low to mark, rather than
        // leaving two arrows pointing at nothing.
        if (s_tri_up) {
            const bool show = g_data.weatherValid && g_data.dailyValid;
            lv_obj_set_style_opa(s_tri_up, show ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
            lv_obj_set_style_opa(s_tri_dn, show ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        }

        static int drawn = -9999;
        int code = g_data.weatherValid ? g_data.weatherCode : -1;
        if (s_icon_wx && code != drawn) { icon_draw_weather(s_icon_wx, code); drawn = code; }
    }

    if (s_sun_rise) {
        struct tm x;
        localtime_r(&g_data.sunriseToday, &x);
        snprintf(b, sizeof(b), "%02d:%02d", x.tm_hour, x.tm_min);
        lv_label_set_text(s_sun_rise, b);
        localtime_r(&g_data.sunsetToday, &x);
        // Just the time: the arc already says which end is which.
        snprintf(b, sizeof(b), "%02d:%02d", x.tm_hour, x.tm_min);
        lv_label_set_text(s_sun_set, b);

        // Day length, and where we are along it.
        const time_t rise = g_data.sunriseToday, set = g_data.sunsetToday;
        float frac = -1.0f;
        if (set > rise) {
            const long len = (long)(set - rise);
            snprintf(b, sizeof(b), "%ldh %02ldm", len / 3600, (len % 3600) / 60);
            lv_label_set_text(s_daylen, b);
            frac = (float)(now - rise) / (float)len;   // <0 or >1 outside the day
        } else {
            lv_label_set_text(s_daylen, "");
        }
        // Redraw only when the sun has visibly moved: 1/300 of the track is
        // under a pixel on a 300 px arc, so anything finer is wasted work.
        static int drawn_frac = -9999;
        int fq = (int)(frac * 300.0f);
        if (s_arc && fq != drawn_frac) { icon_draw_daylight(s_arc, frac); drawn_frac = fq; }

        // Two short lines on the rail, whose cell is 218 px wide and cannot
        // fit "Waning Gibbous 74%" on one. The ticker has the width, so it
        // gets the single line the design draws.
        snprintf(b, sizeof(b), s_moon_oneline ? "%s %.0f%%" : "%s\n%.0f%%",
                 moonPhaseName(g_data.moonPhase), g_data.moonIlluminationPct);
        lv_label_set_text(s_moon_txt, b);
        static int drawn_ph = -1;
        int ph = (int)(g_data.moonPhase * 100.0f);
        if (s_icon_moon && ph != drawn_ph) { icon_draw_moon(s_icon_moon, g_data.moonPhase); drawn_ph = ph; }
    }

    if (s_aqi_num) {
        if (g_data.aqiValid) {
            snprintf(b, sizeof(b), "%d", g_data.aqi);
            lv_label_set_text(s_aqi_num, b);
            // Theme steps only. The band is carried by the word beside it.
            lv_obj_set_style_text_color(s_aqi_num, lv_color_hex(th->t1), 0);
            lv_label_set_text(s_aqi_lbl, theme_aqi_label(g_data.aqi));
            lv_obj_set_style_text_color(s_aqi_lbl, lv_color_hex(th->acc), 0);
            snprintf(b, sizeof(b), "PM2.5 %d", g_data.pm25);
            lv_label_set_text(s_aqi_pm, b);
            // The 5-segment scale reads as a POSITION on the band ladder, not
            // as a colour code: lit segments take the accent, the rest the
            // deep step, so the strip stays one hue.
            const int aqi = g_data.aqi;
            const int lit = aqi <= 50 ? 1 : aqi <= 100 ? 2 :
                            aqi <= 150 ? 3 : aqi <= 200 ? 4 : 5;
            for (int i = 0; i < 5; i++)
                if (s_aqi_seg[i])
                    lv_obj_set_style_bg_color(s_aqi_seg[i],
                        lv_color_hex(i < lit ? th->acc : th->deep), 0);
        } else {
            lv_label_set_text(s_aqi_num, "--");
            lv_obj_set_style_text_color(s_aqi_num, lv_color_hex(th->t1), 0);
            lv_label_set_text(s_aqi_lbl, wifi_ready() ? "updating" : "offline");
            lv_label_set_text(s_aqi_pm, "");
            for (int i = 0; i < 5; i++)
                if (s_aqi_seg[i])
                    lv_obj_set_style_bg_color(s_aqi_seg[i], lv_color_hex(th->deep), 0);
        }
    }

    // ---- status row ----
    if (s_st_addr) {
        if (webui_is_ap())          snprintf(b, sizeof(b), "join %s", webui_ap_ssid());
        else if (wifi_ready())      snprintf(b, sizeof(b), "setup %s", webui_address());
        else if (wifi_connecting()) snprintf(b, sizeof(b), "connecting");
        else                        snprintf(b, sizeof(b), "offline");
        lv_label_set_text(s_st_addr, b);
        // The dot is the accent when connected, the deep step when not: the
        // one glanceable health cue in the housekeeping row.
        lv_obj_set_style_bg_color(s_st_dot, lv_color_hex(wifi_ready() ? th->acc : th->deep), 0);

        for (int i = 0; i < MAX_PAGES; i++)
            if (s_st_pagedot[i])
                lv_obj_set_style_bg_color(s_st_pagedot[i],
                    lv_color_hex(i == s_cur_page ? th->acc : th->deep), 0);

        // Signal, on the ticker's row B. Thresholds chosen so the design's
        // own example -- -58 dBm -- lights two bars of three, as it draws it.
        if (s_st_rssi) {
            const int rssi = wifi_rssi();
            if (wifi_ready()) snprintf(b, sizeof(b), "%d dBm", rssi);
            else              snprintf(b, sizeof(b), "--");
            lv_label_set_text(s_st_rssi, b);
            const int lit = !wifi_ready() ? 0 :
                            rssi >= -50 ? 3 : rssi >= -65 ? 2 : rssi >= -80 ? 1 : 0;
            for (int i = 0; i < 3; i++)
                if (s_wifi_bar[i])
                    lv_obj_set_style_bg_color(s_wifi_bar[i],
                        lv_color_hex(i < lit ? th->acc : th->deep), 0);
        }

        const int64_t up = esp_timer_get_time() / 1000000;
        if (up >= 86400) snprintf(b, sizeof(b), "up %lldd %02lldh", up / 86400, (up % 86400) / 3600);
        else             snprintf(b, sizeof(b), "up %lldh %02lldm", up / 3600, (up % 3600) / 60);
        lv_label_set_text(s_st_up, b);
    }
}

// ---------------------------------------------------------------------------
static uint32_t lv_tick_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px)
{
    (void)area; (void)px;
    lv_display_flush_ready(disp);
}

esp_err_t overlay_init(int display_w, int display_h, const overlay_cfg_t *cfg)
{
    // The strip is always authored along the bottom of the VIEW, so its size
    // comes from the view's dimensions -- which are the panel's, swapped in
    // landscape.
    const int view_w = g_settings.landscape ? display_h : display_w;
    const int view_h = g_settings.landscape ? display_w : display_h;
    s_w = view_w;
    s_land = g_settings.landscape;
    // Portrait takes the design's 16% of height (204 px). Landscape does NOT:
    // the ticker is a FIXED 88 px band, which is both what the landscape
    // design specifies and 40 px of picture the 16% rule would have spent on
    // empty background -- 1280x712 of photo instead of 1280x672.
    s_h = s_land ? BAND_H : view_h * OVERLAY_NUM / OVERLAY_DEN;
    s_cfg = *cfg;

    // Then place it in panel space. Photos use 90 degrees for landscape, so a
    // landscape view's bottom edge lands against the panel's far x edge; the
    // 180 variants put each on the opposite side.
    if (!g_settings.landscape) {
        s_rot = g_settings.rotate180 ? PPA_SRM_ROTATION_ANGLE_180 : PPA_SRM_ROTATION_ANGLE_0;
        s_pw = s_w;  s_ph = s_h;
        s_px = 0;
        s_py = g_settings.rotate180 ? 0 : (display_h - s_h);
    } else {
        s_rot = g_settings.rotate180 ? PPA_SRM_ROTATION_ANGLE_270 : PPA_SRM_ROTATION_ANGLE_90;
        // Rotating swaps the extents: a 1280x88 ticker becomes an 88x1280 band.
        s_pw = s_h;  s_ph = s_w;
        s_px = g_settings.rotate180 ? 0 : (display_w - s_h);
        s_py = 0;
    }

    const size_t bytes = (size_t)s_w * s_h * 4;
    s_layer = heap_caps_aligned_alloc(128, bytes, MALLOC_CAP_SPIRAM);
    if (!s_layer) { ESP_LOGE(TAG, "no PSRAM for a %dx%d ARGB layer", s_w, s_h); return ESP_ERR_NO_MEM; }
    memset(s_layer, 0, bytes);

    // A rotated copy is needed for every mounting except plain portrait.
    if (s_rot != PPA_SRM_ROTATION_ANGLE_0) {
        s_layer_rot = heap_caps_aligned_alloc(128, bytes, MALLOC_CAP_SPIRAM);
        if (!s_layer_rot) { ESP_LOGE(TAG, "no PSRAM for the rotated layer"); return ESP_ERR_NO_MEM; }
        memset(s_layer_rot, 0, bytes);
        ppa_client_config_t pc = { .oper_type = PPA_OPERATION_SRM };
        if (ppa_register_client(&pc, &s_ppa_rot) != ESP_OK) return ESP_FAIL;
    }

    lv_init();
    lv_tick_set_cb(lv_tick_ms);      // must precede any rendering
    if (!fonts_init(s_land)) { ESP_LOGE(TAG, "font init failed"); return ESP_FAIL; }
    s_disp = lv_display_create(s_w, s_h);
    if (!s_disp) return ESP_FAIL;
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_ARGB8888);
    lv_display_set_flush_cb(s_disp, flush_cb);
    lv_display_set_buffers(s_disp, s_layer, NULL, bytes, LV_DISPLAY_RENDER_MODE_DIRECT);

    build();
    refresh();
    lv_timer_handler();
    rotate_layer();

    ESP_LOGI(TAG, "after overlay: DMA %u free / %u largest",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
    ESP_LOGI(TAG, "strip %dx%d in view space -> %dx%d at panel (%d,%d), rotation %d",
             s_w, s_h, s_pw, s_ph, s_px, s_py, (int)s_rot);
    // Each layout is now drawn at its own design's height, so a mismatch here
    // means the geometry constants have drifted rather than that the strip is
    // the wrong shape for the design.
    const int rows = s_land ? BAND_H
                            : STRIP_HAIRLINE_H + STRIP_DATA_H + STRIP_STATUS_H;
    if (s_h != rows) {
        ESP_LOGW(TAG, "strip is %d px but the %s rows total %d",
                 s_h, s_land ? "ticker's" : "rail's", rows);
    }
    return ESP_OK;
}

void overlay_apply_config(const overlay_cfg_t *cfg)
{
    s_cfg = *cfg;
    build();
    refresh();
}

// The page no longer advances on its own clock. It moves when the PHOTO
// moves, so the strip and the picture change together instead of drifting in
// and out of step with each other.
void overlay_next_page(void)
{
    if (s_page_count < 2) return;
    lv_obj_set_style_opa(s_page[s_cur_page], LV_OPA_TRANSP, 0);
    s_cur_page = (s_cur_page + 1) % s_page_count;
    lv_obj_set_style_opa(s_page[s_cur_page], LV_OPA_COVER, 0);
    s_dirty = true;
}

void overlay_set_hidden(bool hidden) { s_hidden = hidden; s_dirty = true; }
bool overlay_hidden(void)            { return s_hidden; }

bool overlay_tick(void)
{
    if (s_page_count == 0) return false;
    static int64_t last;
    int64_t now = esp_timer_get_time();
    bool dirty = false;
    if (now - last > 500000) { refresh(); last = now; dirty = true; }
    lv_timer_handler();
    if (s_dirty) { dirty = true; s_dirty = false; }
    if (dirty) rotate_layer();
    return dirty;
}

// Hidden counts as not visible, so the compositor simply stops blending it.
bool        overlay_visible(void)    { return s_page_count > 0 && !s_hidden; }
int         overlay_x(void)          { return s_px; }
int         overlay_y(void)          { return s_py; }
int         overlay_width(void)      { return s_pw; }
int         overlay_height(void)     { return s_ph; }
const void *overlay_pixels(void)
{
    // The compositor asks for pixels; it should not have to know about
    // mounting. Hand back the rotated copy when there is one.
    return s_layer_rot ? (const void *)s_layer_rot : (const void *)s_layer;
}

// Rotate the freshly drawn strip. Called only when overlay_tick() reports a
// change, so this is ~2 passes per second over 800x204 -- about 5 ms each.
static void rotate_layer(void)
{
    if (!s_layer_rot) return;
    ppa_srm_oper_config_t op = {0};
    op.in.buffer  = s_layer;
    op.in.pic_w   = s_w;  op.in.pic_h   = s_h;
    op.in.block_w = s_w;  op.in.block_h = s_h;
    op.in.srm_cm  = PPA_SRM_COLOR_MODE_ARGB8888;

    op.out.buffer      = s_layer_rot;
    op.out.buffer_size = (uint32_t)s_w * s_h * 4;
    op.out.pic_w       = s_pw;  op.out.pic_h = s_ph;   // panel-space extents
    op.out.srm_cm      = PPA_SRM_COLOR_MODE_ARGB8888;

    op.rotation_angle = s_rot;
    op.scale_x = 1.0f;  op.scale_y = 1.0f;
    op.mode = PPA_TRANS_MODE_BLOCKING;
    ppa_do_scale_rotate_mirror(s_ppa_rot, &op);
}
int         overlay_stride(void)     { return s_pw * 4; }
int         overlay_page_count(void) { return s_page_count; }
