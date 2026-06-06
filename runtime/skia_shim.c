/*
 * skia_shim.c — C shim bridging Ruxen's `canvas` (L1) FFI to the rendering
 * backend.
 *
 * This is step (1) of the incremental-FFI discipline (see docs/FFI.md):
 * every capability the L1 `Canvas` exposes is first wrapped here as a flat
 * C function with a stable, Ruxen-friendly C-ABI signature, then declared in
 * a `lib` block in src/lib.rx, then surfaced as a `Canvas` method.
 *
 * Symbols are prefixed `ruxen_canvas_*` so they never collide with the host
 * runtime. All pointers cross the ABI as machine-word integers (int64_t);
 * `void` returns map to no return value (see docs/FFI.md).
 *
 * Backend: a deterministic software raster target. It implements the exact
 * ABI the GPU backend will use, so Skia can be slotted in behind these
 * signatures later (prebuilt-vs-source is still an open decision —
 * docs/ROADMAP.md) without touching the Ruxen side. It is also what the
 * pin tests run against: every bound call is verified by reading pixels
 * back from the framebuffer.
 *
 * This file contains NO platform code. Live windowing lives in
 * runtime/sdl_window.c, which attaches to a host through the present/pump/
 * close hooks declared in rx_canvas_internal.h.
 */

#include <stdint.h>
#include <stdlib.h>
#include <time.h>

#include "rx_canvas_internal.h"

/* floor()/isnan() without libm, so the shim adds no link dependencies.
 * (Geometry values are device pixels — far inside int64 range.) */
static int64_t rxc_floor_to_i64(double v) {
    int64_t i = (int64_t)v;          /* truncates toward zero */
    return (v < (double)i) ? i - 1 : i;
}

static int rxc_is_nan(double v) {
    return v != v;
}

/* Geometry must be finite and within sane pixel range before it can be
 * floored: casting NaN/Inf/huge doubles to int64 is undefined behavior.
 * NaN fails both comparisons, so this rejects NaN too. */
static int rxc_finite_pixels(double v) {
    return v > -1.0e9 && v < 1.0e9;
}



/* ---- lifecycle ---- */

/* Create a host with a width*height framebuffer, initially fully
 * transparent. Returns the handle as int64_t, or 0 on bad dimensions /
 * allocation failure. */
int64_t ruxen_canvas_host_new(int64_t width, int64_t height) {
    if (width <= 0 || height <= 0 || width > 16384 || height > 16384) {
        return 0;
    }
    RxHost *h = (RxHost *)calloc(1, sizeof(RxHost));
    if (!h) return 0;
    h->width  = (int32_t)width;
    h->height = (int32_t)height;
    h->pixels = (uint32_t *)calloc((size_t)(width * height), sizeof(uint32_t));
    if (!h->pixels) {
        free(h);
        return 0;
    }
    return (int64_t)h;
}

/* True when the handle is null — lets the Ruxen side turn an allocation
 * failure in ruxen_canvas_host_new into a proper Err instead of a zombie
 * object. */
int64_t ruxen_canvas_host_is_null(int64_t self) {
    return self == 0 ? 1 : 0;
}

/* Tear the host down. Called from the Ruxen side's drop — deterministic,
 * no GC. */
void ruxen_canvas_host_drop(int64_t self) {
    RxHost *h = (RxHost *)self;
    if (!h) return;
    free(h->pixels);
    free(h);
}

int64_t ruxen_canvas_host_width(int64_t self) {
    RxHost *h = (RxHost *)self;
    return h ? h->width : 0;
}

int64_t ruxen_canvas_host_height(int64_t self) {
    RxHost *h = (RxHost *)self;
    return h ? h->height : 0;
}

/* Read one pixel back as packed 0xAARRGGBB. Returns -1 when out of bounds.
 * This is the observation hook every pin test uses. */
int64_t ruxen_canvas_read_pixel(int64_t self, int64_t x, int64_t y) {
    RxHost *h = (RxHost *)self;
    if (!h || x < 0 || y < 0 || x >= h->width || y >= h->height) {
        return -1;
    }
    return (int64_t)h->pixels[y * h->width + x];
}

/* ---- frame discipline ---- */

int64_t ruxen_canvas_begin_frame(int64_t self) {
    RxHost *h = (RxHost *)self;
    if (!h) return RXC_ERR_BAD_ARGS;
    if (h->in_frame) return RXC_ERR_IN_FRAME;
    h->in_frame = 1;
    return RXC_OK;
}

/* End the frame. The software backend has nothing to flip; presenting to
 * a live window is the explicit ruxen_canvas_window_present call in
 * runtime/sdl_window.c (the Ruxen Window.end_frame does both). */
int64_t ruxen_canvas_end_frame(int64_t self) {
    RxHost *h = (RxHost *)self;
    if (!h) return RXC_ERR_BAD_ARGS;
    if (!h->in_frame) return RXC_ERR_NO_FRAME;
    h->in_frame = 0;
    return RXC_OK;
}

/* ---- drawing ---- */

static int rxc_check_color(int64_t r, int64_t g, int64_t b, int64_t a) {
    return r >= 0 && r <= 255 && g >= 0 && g <= 255 &&
           b >= 0 && b <= 255 && a >= 0 && a <= 255;
}

static uint32_t rxc_pack(int64_t r, int64_t g, int64_t b, int64_t a) {
    return ((uint32_t)a << 24) | ((uint32_t)r << 16) |
           ((uint32_t)g << 8)  |  (uint32_t)b;
}

/* Source-over blend of src (non-premultiplied) onto dst. */
static uint32_t rxc_blend(uint32_t dst, uint32_t src) {
    uint32_t sa = (src >> 24) & 0xFF;
    if (sa == 255) return src;
    if (sa == 0)   return dst;
    uint32_t da = (dst >> 24) & 0xFF;
    uint32_t inv = 255 - sa;
    uint32_t out_a = sa + (da * inv + 127) / 255;
    if (out_a == 0) return 0;
    uint32_t channels[3];
    int shift;
    for (shift = 0; shift < 3; shift++) {
        uint32_t sc = (src >> (16 - 8 * shift)) & 0xFF;
        uint32_t dc = (dst >> (16 - 8 * shift)) & 0xFF;
        /* blend premultiplied, then un-premultiply by out_a */
        uint32_t num = sc * sa * 255 + dc * da * inv;
        channels[shift] = num / (out_a * 255);
        if (channels[shift] > 255) channels[shift] = 255;
    }
    return (out_a << 24) | (channels[0] << 16) | (channels[1] << 8) | channels[2];
}

/* Clear the whole surface to a solid color (replaces, no blending —
 * matching SkCanvas::clear semantics). */
int64_t ruxen_canvas_clear(int64_t self, int64_t r, int64_t g, int64_t b, int64_t a) {
    RxHost *h = (RxHost *)self;
    if (!h || !rxc_check_color(r, g, b, a)) return RXC_ERR_BAD_ARGS;
    if (!h->in_frame) return RXC_ERR_NO_FRAME;
    uint32_t px = rxc_pack(r, g, b, a);
    int64_t n = (int64_t)h->width * h->height;
    for (int64_t i = 0; i < n; i++) h->pixels[i] = px;
    return RXC_OK;
}

/* Fill an axis-aligned rect with source-over blending, clipped to the
 * surface. Geometry comes in as doubles (Ruxen Float32 widens at the call);
 * the filled pixel box is [floor(x), floor(x+w)) x [floor(y), floor(y+h)). */
int64_t ruxen_canvas_draw_rect(int64_t self, double x, double y, double w, double hgt,
                               int64_t r, int64_t g, int64_t b, int64_t a) {
    RxHost *h = (RxHost *)self;
    if (!h || !rxc_check_color(r, g, b, a)) return RXC_ERR_BAD_ARGS;
    if (!h->in_frame) return RXC_ERR_NO_FRAME;
    if (rxc_is_nan(w) || rxc_is_nan(hgt)) return RXC_ERR_BAD_ARGS;
    if (!(w > 0.0) || !(hgt > 0.0)) return RXC_OK;     /* empty: nothing to draw */
    if (!rxc_finite_pixels(x) || !rxc_finite_pixels(y) ||
        !rxc_finite_pixels(w) || !rxc_finite_pixels(hgt)) {
        return RXC_ERR_BAD_ARGS;
    }

    int64_t x0 = rxc_floor_to_i64(x);
    int64_t y0 = rxc_floor_to_i64(y);
    int64_t x1 = rxc_floor_to_i64(x + w);
    int64_t y1 = rxc_floor_to_i64(y + hgt);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > h->width)  x1 = h->width;
    if (y1 > h->height) y1 = h->height;

    uint32_t src = rxc_pack(r, g, b, a);
    for (int64_t py = y0; py < y1; py++) {
        uint32_t *row = h->pixels + py * h->width;
        for (int64_t px = x0; px < x1; px++) {
            row[px] = rxc_blend(row[px], src);
        }
    }
    return RXC_OK;
}

/* ---- text: embedded 5x7 bitmap font ---- */
/* Classic 5x7 ASCII font (0x20..0x7E), column-major: 5 bytes per glyph,
 * bit 0 of each byte is the top row. Good enough for the counter-app
 * slice; Skia paragraph / HarfBuzz shaping replaces this later. */

#define RXC_GLYPH_W   5
#define RXC_GLYPH_H   7
#define RXC_ADVANCE   6  /* glyph width + 1px spacing */

static const uint8_t rxc_font5x7[95][5] = {
    {0x00,0x00,0x00,0x00,0x00}, /* ' ' */
    {0x00,0x00,0x5F,0x00,0x00}, /* '!' */
    {0x00,0x07,0x00,0x07,0x00}, /* '"' */
    {0x14,0x7F,0x14,0x7F,0x14}, /* '#' */
    {0x24,0x2A,0x7F,0x2A,0x12}, /* '$' */
    {0x23,0x13,0x08,0x64,0x62}, /* '%' */
    {0x36,0x49,0x55,0x22,0x50}, /* '&' */
    {0x00,0x05,0x03,0x00,0x00}, /* ''' */
    {0x00,0x1C,0x22,0x41,0x00}, /* '(' */
    {0x00,0x41,0x22,0x1C,0x00}, /* ')' */
    {0x14,0x08,0x3E,0x08,0x14}, /* '*' */
    {0x08,0x08,0x3E,0x08,0x08}, /* '+' */
    {0x00,0x50,0x30,0x00,0x00}, /* ',' */
    {0x08,0x08,0x08,0x08,0x08}, /* '-' */
    {0x00,0x60,0x60,0x00,0x00}, /* '.' */
    {0x20,0x10,0x08,0x04,0x02}, /* '/' */
    {0x3E,0x51,0x49,0x45,0x3E}, /* '0' */
    {0x00,0x42,0x7F,0x40,0x00}, /* '1' */
    {0x42,0x61,0x51,0x49,0x46}, /* '2' */
    {0x21,0x41,0x45,0x4B,0x31}, /* '3' */
    {0x18,0x14,0x12,0x7F,0x10}, /* '4' */
    {0x27,0x45,0x45,0x45,0x39}, /* '5' */
    {0x3C,0x4A,0x49,0x49,0x30}, /* '6' */
    {0x01,0x71,0x09,0x05,0x03}, /* '7' */
    {0x36,0x49,0x49,0x49,0x36}, /* '8' */
    {0x06,0x49,0x49,0x29,0x1E}, /* '9' */
    {0x00,0x36,0x36,0x00,0x00}, /* ':' */
    {0x00,0x56,0x36,0x00,0x00}, /* ';' */
    {0x08,0x14,0x22,0x41,0x00}, /* '<' */
    {0x14,0x14,0x14,0x14,0x14}, /* '=' */
    {0x00,0x41,0x22,0x14,0x08}, /* '>' */
    {0x02,0x01,0x51,0x09,0x06}, /* '?' */
    {0x32,0x49,0x79,0x41,0x3E}, /* '@' */
    {0x7E,0x11,0x11,0x11,0x7E}, /* 'A' */
    {0x7F,0x49,0x49,0x49,0x36}, /* 'B' */
    {0x3E,0x41,0x41,0x41,0x22}, /* 'C' */
    {0x7F,0x41,0x41,0x22,0x1C}, /* 'D' */
    {0x7F,0x49,0x49,0x49,0x41}, /* 'E' */
    {0x7F,0x09,0x09,0x09,0x01}, /* 'F' */
    {0x3E,0x41,0x49,0x49,0x7A}, /* 'G' */
    {0x7F,0x08,0x08,0x08,0x7F}, /* 'H' */
    {0x00,0x41,0x7F,0x41,0x00}, /* 'I' */
    {0x20,0x40,0x41,0x3F,0x01}, /* 'J' */
    {0x7F,0x08,0x14,0x22,0x41}, /* 'K' */
    {0x7F,0x40,0x40,0x40,0x40}, /* 'L' */
    {0x7F,0x02,0x0C,0x02,0x7F}, /* 'M' */
    {0x7F,0x04,0x08,0x10,0x7F}, /* 'N' */
    {0x3E,0x41,0x41,0x41,0x3E}, /* 'O' */
    {0x7F,0x09,0x09,0x09,0x06}, /* 'P' */
    {0x3E,0x41,0x51,0x21,0x5E}, /* 'Q' */
    {0x7F,0x09,0x19,0x29,0x46}, /* 'R' */
    {0x46,0x49,0x49,0x49,0x31}, /* 'S' */
    {0x01,0x01,0x7F,0x01,0x01}, /* 'T' */
    {0x3F,0x40,0x40,0x40,0x3F}, /* 'U' */
    {0x1F,0x20,0x40,0x20,0x1F}, /* 'V' */
    {0x3F,0x40,0x38,0x40,0x3F}, /* 'W' */
    {0x63,0x14,0x08,0x14,0x63}, /* 'X' */
    {0x07,0x08,0x70,0x08,0x07}, /* 'Y' */
    {0x61,0x51,0x49,0x45,0x43}, /* 'Z' */
    {0x00,0x7F,0x41,0x41,0x00}, /* '[' */
    {0x02,0x04,0x08,0x10,0x20}, /* '\' */
    {0x00,0x41,0x41,0x7F,0x00}, /* ']' */
    {0x04,0x02,0x01,0x02,0x04}, /* '^' */
    {0x40,0x40,0x40,0x40,0x40}, /* '_' */
    {0x00,0x01,0x02,0x04,0x00}, /* '`' */
    {0x20,0x54,0x54,0x54,0x78}, /* 'a' */
    {0x7F,0x48,0x44,0x44,0x38}, /* 'b' */
    {0x38,0x44,0x44,0x44,0x20}, /* 'c' */
    {0x38,0x44,0x44,0x48,0x7F}, /* 'd' */
    {0x38,0x54,0x54,0x54,0x18}, /* 'e' */
    {0x08,0x7E,0x09,0x01,0x02}, /* 'f' */
    {0x0C,0x52,0x52,0x52,0x3E}, /* 'g' */
    {0x7F,0x08,0x04,0x04,0x78}, /* 'h' */
    {0x00,0x44,0x7D,0x40,0x00}, /* 'i' */
    {0x20,0x40,0x44,0x3D,0x00}, /* 'j' */
    {0x7F,0x10,0x28,0x44,0x00}, /* 'k' */
    {0x00,0x41,0x7F,0x40,0x00}, /* 'l' */
    {0x7C,0x04,0x18,0x04,0x78}, /* 'm' */
    {0x7C,0x08,0x04,0x04,0x78}, /* 'n' */
    {0x38,0x44,0x44,0x44,0x38}, /* 'o' */
    {0x7C,0x14,0x14,0x14,0x08}, /* 'p' */
    {0x08,0x14,0x14,0x18,0x7C}, /* 'q' */
    {0x7C,0x08,0x04,0x04,0x08}, /* 'r' */
    {0x48,0x54,0x54,0x54,0x20}, /* 's' */
    {0x04,0x3F,0x44,0x40,0x20}, /* 't' */
    {0x3C,0x40,0x40,0x20,0x7C}, /* 'u' */
    {0x1C,0x20,0x40,0x20,0x1C}, /* 'v' */
    {0x3C,0x40,0x30,0x40,0x3C}, /* 'w' */
    {0x44,0x28,0x10,0x28,0x44}, /* 'x' */
    {0x0C,0x50,0x50,0x50,0x3C}, /* 'y' */
    {0x44,0x64,0x54,0x4C,0x44}, /* 'z' */
    {0x00,0x08,0x36,0x41,0x00}, /* '{' */
    {0x00,0x00,0x7F,0x00,0x00}, /* '|' */
    {0x00,0x41,0x36,0x08,0x00}, /* '}' */
    {0x10,0x08,0x08,0x10,0x08}, /* '~' */
};

/* Width in pixels of an n-character single line at this font's one size.
 * Each glyph advances RXC_ADVANCE px; no trailing gap. The CHARACTER COUNT
 * crosses the FFI (not the string): the metric stays defined here, and the
 * call uses only integer arguments. */
int64_t ruxen_canvas_measure_text_n(int64_t self, int64_t n) {
    (void)self;
    if (n <= 0) return 0;
    return n * RXC_ADVANCE - 1;
}

/* The font's line height (ascent above the baseline), in pixels. */
int64_t ruxen_canvas_text_height(int64_t self) {
    (void)self;
    return RXC_GLYPH_H;
}

/* Draw a single line of text. (x, y) is the BASELINE origin: glyphs occupy
 * the 7 rows above y. Characters outside printable ASCII render as a
 * replacement box. Source-over blended, clipped to the surface. */
int64_t ruxen_canvas_draw_text(int64_t self, int64_t text, double x, double y,
                               int64_t r, int64_t g, int64_t b, int64_t a) {
    RxHost *h = (RxHost *)self;
    const char *s = (const char *)text;
    if (!h || !s || !rxc_check_color(r, g, b, a)) return RXC_ERR_BAD_ARGS;
    if (!h->in_frame) return RXC_ERR_NO_FRAME;
    if (!rxc_finite_pixels(x) || !rxc_finite_pixels(y)) return RXC_ERR_BAD_ARGS;

    uint32_t src = rxc_pack(r, g, b, a);
    int64_t pen_x = rxc_floor_to_i64(x);
    int64_t top   = rxc_floor_to_i64(y) - RXC_GLYPH_H;

    for (; *s; s++, pen_x += RXC_ADVANCE) {
        unsigned char ch = (unsigned char)*s;
        const uint8_t *glyph;
        uint8_t box[5] = {0x7F, 0x41, 0x41, 0x41, 0x7F}; /* replacement box */
        if (ch >= 0x20 && ch <= 0x7E) {
            glyph = rxc_font5x7[ch - 0x20];
        } else {
            glyph = box;
        }
        for (int col = 0; col < RXC_GLYPH_W; col++) {
            int64_t px = pen_x + col;
            if (px < 0 || px >= h->width) continue;
            for (int row = 0; row < RXC_GLYPH_H; row++) {
                if (!((glyph[col] >> row) & 1)) continue;
                int64_t py = top + row;
                if (py < 0 || py >= h->height) continue;
                uint32_t *p = h->pixels + py * h->width + px;
                *p = rxc_blend(*p, src);
            }
        }
    }
    return RXC_OK;
}

/* ---- event queue ---- */
/* The platform pump (sdl_window.c) and the tests push events in; the Ruxen
 * side polls them out one at a time. poll pops the next event into the
 * `pending` slot and returns its kind (-1 when the queue is empty); the
 * payload accessors then read from `pending`. */

int64_t ruxen_canvas_push_event(int64_t self, int64_t kind, double a, double b) {
    RxHost *h = (RxHost *)self;
    if (!h || kind < 0 || kind > RXC_EVENT_KIND_MAX) return RXC_ERR_BAD_ARGS;
    if (h->ev_count >= RXC_EVENT_CAP) return RXC_ERR_QUEUE_FULL;
    int32_t tail = (h->ev_head + h->ev_count) % RXC_EVENT_CAP;
    h->events[tail].kind = (int32_t)kind;
    h->events[tail].a = a;
    h->events[tail].b = b;
    h->ev_count++;
    return RXC_OK;
}

int64_t ruxen_canvas_poll_event(int64_t self) {
    RxHost *h = (RxHost *)self;
    if (!h || h->ev_count == 0) return -1;
    h->pending = h->events[h->ev_head];
    h->ev_head = (h->ev_head + 1) % RXC_EVENT_CAP;
    h->ev_count--;
    return h->pending.kind;
}

double ruxen_canvas_event_a(int64_t self) {
    RxHost *h = (RxHost *)self;
    return h ? h->pending.a : 0.0;
}

double ruxen_canvas_event_b(int64_t self) {
    RxHost *h = (RxHost *)self;
    return h ? h->pending.b : 0.0;
}

/* Integer-typed companions: the Ruxen side currently can't lower Int<->
 * Float casts inside class methods, so the conversions happen here. The
 * double-typed entry points above remain the canonical ABI for the
 * eventual Float32 event payloads. */

int64_t ruxen_canvas_push_event_i(int64_t self, int64_t kind, int64_t a, int64_t b) {
    return ruxen_canvas_push_event(self, kind, (double)a, (double)b);
}

int64_t ruxen_canvas_event_ai(int64_t self) {
    RxHost *h = (RxHost *)self;
    return h ? (int64_t)h->pending.a : 0;
}

int64_t ruxen_canvas_event_bi(int64_t self) {
    RxHost *h = (RxHost *)self;
    return h ? (int64_t)h->pending.b : 0;
}

/* ---- frame pacing ---- */

/* Sleep for ms milliseconds (render-loop pacing for L2/apps). The handle
 * is accepted-and-ignored so the binding follows the uniform self-first
 * method ABI. */
void ruxen_canvas_sleep_ms(int64_t self, int64_t ms) {
    (void)self;
    if (ms <= 0) return;
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}
