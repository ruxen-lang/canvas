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

/* ---- status codes shared with src/lib.rx (keep in sync!) ---- */

#define RXC_OK            0
#define RXC_ERR_BAD_ARGS  1  /* invalid dimensions / null handle / bad channel */

/* ---- the host object ---- */

typedef struct {
    int32_t   width;
    int32_t   height;
    uint32_t *pixels;      /* width*height, 0xAARRGGBB, non-premultiplied */
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
