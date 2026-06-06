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
 * Backend: a deterministic software raster target (the "headless" backend).
 * It implements the exact ABI the GPU backend will use, so Skia/SDL can be
 * slotted in behind these signatures later (prebuilt-vs-source is still an
 * open decision — docs/ROADMAP.md) without touching the Ruxen side. It is
 * also what the pin tests run against: every bound call is verified by
 * reading pixels back from the framebuffer.
 */

#include <stdint.h>
#include <stdlib.h>

/* floor()/isnan() without libm, so the shim adds no link dependencies.
 * (Geometry values are device pixels — far inside int64 range.) */
static int64_t rxc_floor_to_i64(double v) {
    int64_t i = (int64_t)v;          /* truncates toward zero */
    return (v < (double)i) ? i - 1 : i;
}

static int rxc_is_nan(double v) {
    return v != v;
}

/* ---- status codes shared with src/lib.rx (keep in sync!) ---- */

#define RXC_OK            0
#define RXC_ERR_BAD_ARGS  1  /* invalid dimensions / null handle / bad channel */
#define RXC_ERR_NO_FRAME  2  /* draw call outside begin_frame/end_frame */
#define RXC_ERR_IN_FRAME  3  /* begin_frame while a frame is already open */

/* ---- the host object ---- */

typedef struct {
    int32_t   width;
    int32_t   height;
    uint32_t *pixels;      /* width*height, 0xAARRGGBB, non-premultiplied */
    int32_t   in_frame;    /* begin_frame/end_frame discipline flag */
} RxHost;

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

/* Present the frame. The software backend has nothing to flip; the GPU
 * backend will swap buffers here. */
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
    if (!(w > 0.0) || !(hgt > 0.0)) return RXC_OK;     /* empty: nothing to draw */
    if (rxc_is_nan(x) || rxc_is_nan(y)) return RXC_ERR_BAD_ARGS;

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
