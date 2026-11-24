#include "icons.h"
#include "theme.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

// Everything here is drawn in the ACTIVE THEME's steps -- accent, primary and
// background -- and nothing else. No fixed yellows, greys or blues: a
// condition icon is not a colour-coded reading, so it has no business
// introducing a second hue into a one-hue strip.
//
// The shapes follow the design doc's SVGs: an outlined sun in the accent, an
// outlined cloud in the primary text step. Outlines are drawn as a filled
// shape with a slightly smaller shape in the background colour laid over it,
// because the canvas has filled primitives but no stroked ones.

// Canvases are no longer all ICON_SIZE: the moon is 44 px in layout 3b.
// The canvas BUFFER's dimensions, not the object's laid-out size.
//
// lv_obj_get_width() returns 0 until LVGL has run a layout pass, and the
// triangles are drawn immediately after they are created -- so it returned
// zero, every point collapsed onto the origin, and what appeared was a
// degenerate blob rather than an arrow, in a zero-width object the label next
// to it then overlapped. The buffer size is correct from the moment the
// canvas exists.
static inline int cw(lv_obj_t *c)
{
    lv_draw_buf_t *b = lv_canvas_get_draw_buf(c);
    return b ? (int)b->header.w : 0;
}

static inline int ch(lv_obj_t *c)
{
    lv_draw_buf_t *b = lv_canvas_get_draw_buf(c);
    return b ? (int)b->header.h : 0;
}

// The condition shapes are authored against a 64 px canvas (ICON_SIZE) and
// scaled to whatever the canvas actually is. The portrait rail draws them at
// 56 px, which is close enough to the authored size that nothing showed; the
// landscape ticker draws them at 30, where unscaled geometry falls straight
// off the buffer -- the clear-sky sun's rays reach 25 px from a centre only
// 15 px from the edge, and rain was drawn at x=19/32/45 on a 30 px canvas.
//
// Rounded rather than truncated, so a 30 px icon keeps the proportions of the
// 64 px one instead of collapsing toward zero.
#define SC(v, w)  ((int)((v) >= 0 ? (((v) * (w) + ICON_SIZE / 2) / ICON_SIZE) \
                                  : -((-(v) * (w) + ICON_SIZE / 2) / ICON_SIZE)))

// Line weights and radii scale the same way but may never reach zero: a
// stroke rounded down to nothing is an invisible icon, not a small one.
static inline int lw(int v, int w) { const int r = SC(v, w); return r < 1 ? 1 : r; }

static void disc(lv_layer_t *l, int cx, int cy, int r, uint32_t rgb)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = lv_color_hex(rgb);
    d.bg_opa   = LV_OPA_COVER;
    d.radius   = LV_RADIUS_CIRCLE;
    lv_area_t a = { cx - r, cy - r, cx + r, cy + r };
    lv_draw_rect(l, &d, &a);
}

static void bar(lv_layer_t *l, int x1, int y1, int x2, int y2, int w, uint32_t rgb)
{
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = lv_color_hex(rgb);
    d.width = w;
    d.opa   = LV_OPA_COVER;
    d.round_start = d.round_end = 1;
    d.p1.x = x1; d.p1.y = y1; d.p2.x = x2; d.p2.y = y2;
    lv_draw_line(l, &d);
}

// A ring: outer disc in `rgb`, inner disc in the background colour.
static void ring(lv_layer_t *l, int cx, int cy, int r, int thick, uint32_t rgb, uint32_t bg)
{
    disc(l, cx, cy, r, rgb);
    disc(l, cx, cy, r - thick, bg);
}

// The cloud silhouette, optionally inset -- called twice to make an outline.
static void cloud_shape(lv_layer_t *l, int cx, int cy, int inset, uint32_t rgb, int w)
{
    disc(l, cx - SC(11, w), cy + SC(3, w), SC(9,  w) - inset, rgb);
    disc(l, cx + SC(2,  w), cy - SC(4, w), SC(13, w) - inset, rgb);
    disc(l, cx + SC(14, w), cy + SC(3, w), SC(10, w) - inset, rgb);

    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = lv_color_hex(rgb);
    d.bg_opa   = LV_OPA_COVER;
    d.radius   = SC(5, w);
    lv_area_t a = { cx - SC(15, w) + inset, cy + SC(3,  w) - inset,
                    cx + SC(16, w) - inset, cy + SC(13, w) - inset };
    lv_draw_rect(l, &d, &a);
}

static void cloud_outline(lv_layer_t *l, int cx, int cy, uint32_t rgb, uint32_t bg, int w)
{
    cloud_shape(l, cx, cy, 0, rgb, w);
    cloud_shape(l, cx, cy, lw(3, w), bg, w);
}

static void sun_outline(lv_layer_t *l, int cx, int cy, int r, bool rays,
                        uint32_t rgb, uint32_t bg, int w)
{
    const int t = lw(3, w);
    if (rays) {
        const int r0 = r + SC(5, w), r1 = r + SC(11, w);
        for (int i = 0; i < 8; i++) {
            const float a = (float)i * (float)M_PI / 4.0f;
            bar(l, cx + (int)(cosf(a) * r0), cy + (int)(sinf(a) * r0),
                   cx + (int)(cosf(a) * r1), cy + (int)(sinf(a) * r1), t, rgb);
        }
    }
    ring(l, cx, cy, r, t, rgb, bg);
}

lv_obj_t *icon_create(lv_obj_t *parent)
{
    const uint32_t sz = lv_canvas_buf_size(ICON_SIZE, ICON_SIZE, 32, LV_DRAW_BUF_STRIDE_ALIGN);
    void *buf = heap_caps_aligned_alloc(64, sz, MALLOC_CAP_SPIRAM);
    if (!buf) return NULL;
    memset(buf, 0, sz);

    lv_obj_t *cv = lv_canvas_create(parent);
    lv_canvas_set_buffer(cv, buf, ICON_SIZE, ICON_SIZE, LV_COLOR_FORMAT_ARGB8888);
    lv_obj_set_size(cv, ICON_SIZE, ICON_SIZE);
    return cv;
}

void icon_draw_triangle(lv_obj_t *canvas, bool up, uint32_t rgb)
{
    if (!canvas) return;

    const int w = cw(canvas), h = ch(canvas);
    if (w <= 0 || h <= 0) return;

    // Clear to FULLY TRANSPARENT by hand rather than via lv_canvas_fill_bg().
    // Filling with black at zero opacity left opaque black behind on one of
    // the two arrows -- the up one covered part of the number beside it while
    // the down one was fine, from identical code. Writing zeroed ARGB pixels
    // is unambiguous.
    lv_draw_buf_t *db = lv_canvas_get_draw_buf(canvas);
    if (db && db->data) memset(db->data, 0, db->data_size);

    lv_layer_t l;
    lv_canvas_init_layer(canvas, &l);

    // Built from horizontal bars rather than lv_draw_triangle. The triangle
    // primitive rendered one direction correctly and not the other from the
    // same three points, and a stack of rectangles is both predictable and
    // exactly as sharp at this size.
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = lv_color_hex(rgb);
    d.bg_opa   = LV_OPA_COVER;
    d.radius   = 0;

    const int m = 2;                 // inset, so the shape is not clipped
    const int rows = h - 2 * m;
    for (int i = 0; i < rows; i++) {
        // Widest at the base, a single pixel at the tip.
        const int frac_num = up ? i : (rows - 1 - i);
        const int half = ((w - 2 * m) * frac_num) / (2 * (rows - 1 ? rows - 1 : 1));
        const int y = m + i;
        lv_area_t a = { w / 2 - half, y, w / 2 + half, y };
        lv_draw_rect(&l, &d, &a);
    }
    lv_canvas_finish_layer(canvas, &l);

}

lv_obj_t *icon_create_sized(lv_obj_t *parent, int w, int h)
{
    const uint32_t sz = lv_canvas_buf_size(w, h, 32, LV_DRAW_BUF_STRIDE_ALIGN);
    void *buf = heap_caps_aligned_alloc(64, sz, MALLOC_CAP_SPIRAM);
    if (!buf) return NULL;
    memset(buf, 0, sz);
    lv_obj_t *cv = lv_canvas_create(parent);
    lv_canvas_set_buffer(cv, buf, w, h, LV_COLOR_FORMAT_ARGB8888);
    lv_obj_set_size(cv, w, h);
    return cv;
}

// The daylight arc.
//
// The design draws a quadratic curve; LVGL has circular arcs, so this is the
// circle that passes through the same three points. For a 300 px span rising
// 30 px in the middle, the radius follows from the sagitta:
//
//     R = c^2 / (8s) + s/2  =  300^2/(8*30) + 15  =  390
//
// with the centre 390 px below the apex -- far off the bottom of a 40 px
// canvas, which is exactly what makes the visible sliver shallow.
void icon_draw_daylight(lv_obj_t *canvas, float frac)
{
    if (!canvas) return;
    const theme_t *th = theme_now();

    lv_canvas_fill_bg(canvas, lv_color_hex(th->bg), LV_OPA_TRANSP);
    lv_layer_t l;
    lv_canvas_init_layer(canvas, &l);

    const int cx = ARC_W / 2, cy = 400, R = 390;
    // Endpoints of the visible sliver, in LVGL's angle convention
    // (0 deg = 3 o'clock, increasing clockwise, so the top is 270).
    const float a0 = 247.4f, a1 = 292.6f, span = a1 - a0;

    lv_draw_arc_dsc_t d;
    lv_draw_arc_dsc_init(&d);
    d.center.x = cx; d.center.y = cy;
    d.radius = R;
    d.width = 2;
    d.opa = LV_OPA_COVER;

    // Whole track first.
    d.color = lv_color_hex(th->deep);
    d.start_angle = a0; d.end_angle = a1;
    lv_draw_arc(&l, &d);

    if (frac > 0.0f && frac < 1.0f) {
        // Elapsed portion, from sunrise to now.
        d.color = lv_color_hex(th->acc);
        d.start_angle = a0; d.end_angle = a0 + span * frac;
        lv_draw_arc(&l, &d);

        // The sun itself, with a soft accent halo behind it.
        const float ang = (a0 + span * frac) * (float)M_PI / 180.0f;
        const int sx = cx + (int)(cosf(ang) * R);
        const int sy = cy + (int)(sinf(ang) * R);
        disc(&l, sx, sy, 10, th->deep);
        disc(&l, sx, sy, 6,  th->t1);
    }
    lv_canvas_finish_layer(canvas, &l);
}

void icon_draw_weather(lv_obj_t *canvas, int code)
{
    if (!canvas) return;
    const theme_t *th = theme_now();
    const uint32_t acc = th->acc, t1 = th->t1, bg = th->bg;
    const int W = cw(canvas);
    if (W <= 0) return;
    const int C = W / 2;
    // Geometry is scaled against 15/16 of the canvas, not all of it. The
    // authored 64 px shapes run edge to edge, so scaling them to the exact
    // canvas width leaves no margin and the widest compositions land ON the
    // last column: overcast (two clouds, the right one offset) filled x=4..29
    // of a 30 px buffer, and partly-cloudy touched both the top and right of
    // a 56 px one. A uniform 6% inset fixes every case at once and keeps the
    // proportions, where nudging each composition by hand would not.
    const int G = W * 15 / 16;

    lv_canvas_fill_bg(canvas, lv_color_hex(bg), LV_OPA_TRANSP);
    lv_layer_t l;
    lv_canvas_init_layer(canvas, &l);

    switch (code) {
    case 0:                                    // clear
        sun_outline(&l, C, C, SC(14, G), true, acc, bg, G);
        break;
    case 1: case 2:                            // mainly / partly cloudy
        sun_outline(&l, C + SC(9, G), C - SC(11, G), SC(9, G), true, acc, bg, G);
        cloud_outline(&l, C - SC(4, G), C + SC(9, G), t1, bg, G);
        break;
    case 3:                                    // overcast
        cloud_outline(&l, C - SC(5, G), C - SC(6, G), t1, bg, G);
        cloud_outline(&l, C + SC(4, G), C + SC(7, G), t1, bg, G);
        break;
    case 45: case 48:                          // fog
        for (int i = 0; i < 4; i++) {
            const int y = C - SC(13, G) + i * SC(9, G);
            bar(&l, C - SC(20, G), y, C + SC(20, G), y, lw(3, G), t1);
        }
        break;
    case 51: case 53: case 55: case 56: case 57:   // drizzle
        cloud_outline(&l, C, C - SC(8, G), t1, bg, G);
        for (int i = 0; i < 3; i++)
            bar(&l, C + SC(-13 + 13 * i, G), C + SC(16, G),
                    C + SC(-16 + 13 * i, G), C + SC(23, G), lw(3, G), acc);
        break;
    case 61: case 63: case 65: case 66: case 67:   // rain
    case 80: case 81: case 82:
        cloud_outline(&l, C, C - SC(8, G), t1, bg, G);
        for (int i = 0; i < 3; i++)
            bar(&l, C + SC(-13 + 13 * i, G), C + SC(14, G),
                    C + SC(-17 + 13 * i, G), C + SC(25, G), lw(4, G), acc);
        break;
    case 71: case 73: case 75: case 77:            // snow
    case 85: case 86:
        cloud_outline(&l, C, C - SC(8, G), t1, bg, G);
        for (int i = 0; i < 3; i++)
            disc(&l, C + SC(-13 + 13 * i, G), C + SC(20, G), lw(3, G), acc);
        break;
    case 95: case 96: case 99:                     // thunderstorm
        cloud_outline(&l, C, C - SC(8, G), t1, bg, G);
        bar(&l, C + SC(4, G), C + SC(12, G), C - SC(4, G), C + SC(20, G), lw(4, G), acc);
        bar(&l, C - SC(4, G), C + SC(20, G), C + SC(3, G), C + SC(20, G), lw(4, G), acc);
        bar(&l, C + SC(3, G), C + SC(20, G), C - SC(5, G), C + SC(28, G), lw(4, G), acc);
        break;
    default:
        cloud_outline(&l, C, C, t1, bg, G);
        break;
    }
    lv_canvas_finish_layer(canvas, &l);
}

void icon_draw_moon(lv_obj_t *canvas, float phase)
{
    if (!canvas) return;
    const theme_t *th = theme_now();
    const int C = cw(canvas) / 2;
    // Lit in the accent, unlit in the deep step: still one hue, and the
    // contrast between the two steps is what reads as a phase.
    const uint32_t lit = th->acc, dark = th->deep, bg = th->bg;

    lv_canvas_fill_bg(canvas, lv_color_hex(bg), LV_OPA_TRANSP);
    lv_layer_t l;
    lv_canvas_init_layer(canvas, &l);

    const int R = C - 2;
    disc(&l, C, C, R, dark);

    const float f = phase;
    if (f >= 0.47f && f <= 0.53f) {
        disc(&l, C, C, R, lit);                      // full
    } else if (f > 0.03f && f < 0.97f) {
        const float illum = (1.0f - cosf(2.0f * (float)M_PI * f)) / 2.0f;
        const bool waxing = (f < 0.5f);

        // Light one half, then push a terminator disc across it. The
        // terminator is a circle rather than a true ellipse -- the canvas has
        // no ellipse primitive -- which is very slightly off near the
        // quarters and correct everywhere else.
        lv_area_t clip = waxing ? (lv_area_t){ C, C - R, C + R, C + R }
                                : (lv_area_t){ C - R, C - R, C, C + R };
        lv_area_t prev = l._clip_area;
        l._clip_area = clip;
        disc(&l, C, C, R, lit);
        l._clip_area = prev;

        const int tr = (int)(R * fabsf(1.0f - 2.0f * illum));
        if (tr > 0) disc(&l, C, C, tr, (illum > 0.5f) ? lit : dark);
    }
    lv_canvas_finish_layer(canvas, &l);
}
