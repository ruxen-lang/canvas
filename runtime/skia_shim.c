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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <dlfcn.h>

#include "rx_canvas_internal.h"
#include "skia/skia_capi.h"

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


/* ---- Skia loader (dlopen, lazy, process-wide singleton) ----
 *
 * libSkiaSharp is fetched by runtime/fetch_skia.sh and dlopen()'d here — never
 * linked (see docs/SKIA.md). rx_skia() resolves the sk_* table on first call;
 * if the library or any required symbol is missing, ->available stays 0 and
 * every drawing op falls back to the software raster path below. */

static void *rx_skia_dlopen(void) {
    /* Search order: explicit override, the fetch-script cache, then the system
     * loader. dlopen of an absolute path is CWD-independent. */
    const char *env = getenv("RUXEN_CANVAS_SKIA");
    if (env && env[0]) {
        void *h = dlopen(env, RTLD_NOW | RTLD_LOCAL);
        if (h) return h;
    }
    char path[4096];
    const char *cache = getenv("RUXEN_CANVAS_CACHE");
    if (cache && cache[0]) {
        snprintf(path, sizeof(path), "%s/libSkiaSharp.so", cache);
        void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        if (h) return h;
    }
    const char *home = getenv("HOME");
    if (home && home[0]) {
        snprintf(path, sizeof(path), "%s/.cache/ruxen-canvas/libSkiaSharp.so", home);
        void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        if (h) return h;
    }
    return dlopen("libSkiaSharp.so", RTLD_NOW | RTLD_LOCAL);
}

const RxSkia *rx_skia(void) {
    static RxSkia skia;       /* zero-initialized: available = 0 */
    static int initialized = 0;
    if (initialized) return &skia;
    initialized = 1;

    void *lib = rx_skia_dlopen();
    if (!lib) return &skia;   /* not available; software fallback */

    /* Two resolution tiers (incremental-FFI discipline, docs/FFI.md):
     *
     *  - REQUIRED: the symbols every currently-implemented op needs (surface
     *    setup + clear + draw_rect). A miss here disables the whole backend so
     *    we never half-use a mismatched library — ->available stays 0 and we
     *    fall back to software.
     *  - OPTIONAL: symbols for capabilities bound in later commits (ovals,
     *    rounded rects, lines, text). They are resolved into the table when
     *    present, but a miss does NOT disable the backend; each wrapping method
     *    must null-check its own pointer before calling and return Err if it is
     *    absent. This keeps an older/newer library from silently dropping the
     *    whole backend just because one not-yet-used symbol was renamed. */
    int ok = 1;
#define RX_SK_REQUIRED(field, sym)                                  \
    do {                                                            \
        *(void **)(&skia.field) = dlsym(lib, sym);                  \
        if (!skia.field) ok = 0;                                    \
    } while (0)
#define RX_SK_OPTIONAL(field, sym)                                  \
    do { *(void **)(&skia.field) = dlsym(lib, sym); } while (0)

    RX_SK_REQUIRED(surface_new_raster_direct, "sk_surface_new_raster_direct");
    RX_SK_REQUIRED(surface_get_canvas,        "sk_surface_get_canvas");
    RX_SK_REQUIRED(surface_unref,             "sk_surface_unref");
    RX_SK_REQUIRED(canvas_clear,              "sk_canvas_clear");
    RX_SK_REQUIRED(canvas_draw_rect,          "sk_canvas_draw_rect");
    RX_SK_REQUIRED(paint_new,                 "sk_paint_new");
    RX_SK_REQUIRED(paint_delete,              "sk_paint_delete");
    RX_SK_REQUIRED(paint_set_color,           "sk_paint_set_color");
    RX_SK_REQUIRED(paint_set_antialias,       "sk_paint_set_antialias");
    RX_SK_REQUIRED(paint_set_style,           "sk_paint_set_style");

    RX_SK_OPTIONAL(paint_set_stroke_width,    "sk_paint_set_stroke_width");
    RX_SK_OPTIONAL(canvas_draw_oval,          "sk_canvas_draw_oval");
    RX_SK_OPTIONAL(canvas_draw_round_rect,    "sk_canvas_draw_round_rect");
    RX_SK_OPTIONAL(canvas_draw_rrect,         "sk_canvas_draw_rrect");
    RX_SK_OPTIONAL(canvas_draw_line,          "sk_canvas_draw_line");
    RX_SK_OPTIONAL(canvas_draw_simple_text,   "sk_canvas_draw_simple_text");
    RX_SK_OPTIONAL(rrect_new,                 "sk_rrect_new");
    RX_SK_OPTIONAL(rrect_delete,              "sk_rrect_delete");
    RX_SK_OPTIONAL(rrect_set_rect_radii,      "sk_rrect_set_rect_radii");
    RX_SK_OPTIONAL(paint_set_shader,          "sk_paint_set_shader");
    RX_SK_OPTIONAL(paint_set_maskfilter,      "sk_paint_set_maskfilter");
    RX_SK_OPTIONAL(shader_new_linear_gradient,"sk_shader_new_linear_gradient");
    RX_SK_OPTIONAL(shader_new_radial_gradient,"sk_shader_new_radial_gradient");
    RX_SK_OPTIONAL(shader_unref,              "sk_shader_unref");
    RX_SK_OPTIONAL(maskfilter_new_blur,       "sk_maskfilter_new_blur");
    RX_SK_OPTIONAL(maskfilter_unref,          "sk_maskfilter_unref");
    RX_SK_OPTIONAL(canvas_save,               "sk_canvas_save");
    RX_SK_OPTIONAL(canvas_restore,            "sk_canvas_restore");
    RX_SK_OPTIONAL(canvas_get_save_count,     "sk_canvas_get_save_count");
    RX_SK_OPTIONAL(canvas_restore_to_count,   "sk_canvas_restore_to_count");
    RX_SK_OPTIONAL(canvas_translate,          "sk_canvas_translate");
    RX_SK_OPTIONAL(canvas_scale,              "sk_canvas_scale");
    RX_SK_OPTIONAL(canvas_rotate_degrees,     "sk_canvas_rotate_degrees");
    RX_SK_OPTIONAL(canvas_reset_matrix,       "sk_canvas_reset_matrix");
    RX_SK_OPTIONAL(canvas_clip_rect,          "sk_canvas_clip_rect_with_operation");
    RX_SK_OPTIONAL(canvas_clip_rrect,         "sk_canvas_clip_rrect_with_operation");
    RX_SK_OPTIONAL(data_new_from_file,        "sk_data_new_from_file");
    RX_SK_OPTIONAL(data_unref,                "sk_data_unref");
    RX_SK_OPTIONAL(image_new_from_encoded,    "sk_image_new_from_encoded");
    RX_SK_OPTIONAL(image_get_width,           "sk_image_get_width");
    RX_SK_OPTIONAL(image_get_height,          "sk_image_get_height");
    RX_SK_OPTIONAL(image_unref,               "sk_image_unref");
    RX_SK_OPTIONAL(canvas_draw_image_rect,    "sk_canvas_draw_image_rect");
    RX_SK_OPTIONAL(typeface_create_default,   "sk_typeface_create_default");
    RX_SK_OPTIONAL(font_new_with_values,      "sk_font_new_with_values");
    RX_SK_OPTIONAL(font_set_size,             "sk_font_set_size");
    RX_SK_OPTIONAL(font_delete,               "sk_font_delete");
    RX_SK_OPTIONAL(font_measure_text,         "sk_font_measure_text");
    RX_SK_OPTIONAL(font_get_metrics,          "sk_font_get_metrics");
#undef RX_SK_REQUIRED
#undef RX_SK_OPTIONAL

    /* Keep the handle open for the process lifetime (no dlclose): the resolved
     * pointers must stay valid. */
    skia.available = ok;
    return &skia;
}

/* Lazily create (once per host) a raster-direct Skia surface that wraps the
 * host's own 0xAARRGGBB framebuffer, and return its canvas — or NULL when Skia
 * is unavailable / creation failed. The surface is cached on the host and torn
 * down in host_drop before the pixels are freed. */
static sk_canvas_t *rx_host_canvas(RxHost *h) {
    if (!h) return NULL;
    if (h->sk_canvas) return (sk_canvas_t *)h->sk_canvas;
    if (h->sk_tried) return NULL;
    h->sk_tried = 1;

    const RxSkia *sk = rx_skia();
    if (!sk->available) return NULL;

    sk_imageinfo_t info;
    info.colorspace = NULL;
    info.width      = h->width;
    info.height     = h->height;
    info.colorType  = RX_SK_COLORTYPE_BGRA_8888;  /* 0xAARRGGBB on LE == B,G,R,A */
    info.alphaType  = RX_SK_ALPHA_PREMUL;

    sk_surface_t *surf = sk->surface_new_raster_direct(
        &info, h->pixels, (size_t)h->width * 4, NULL, NULL, NULL);
    if (!surf) return NULL;
    sk_canvas_t *canvas = sk->surface_get_canvas(surf);
    if (!canvas) { sk->surface_unref(surf); return NULL; }

    h->sk_surface = surf;
    h->sk_canvas  = canvas;
    return canvas;
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
/* defined in runtime/sdl_window.c — tears the OS window down when its
 * owning host is dropped (both files always compile together) */
void ruxen_canvas_window_note_host_dropped(int64_t self);

void ruxen_canvas_host_drop(int64_t self) {
    RxHost *h = (RxHost *)self;
    if (!h) return;
    ruxen_canvas_window_note_host_dropped(self);
    /* The Skia surface wraps h->pixels — unref it before freeing the buffer.
     * (sk_canvas is owned by the surface; no separate release.) */
    if (h->sk_surface) {
        const RxSkia *sk = rx_skia();
        if (sk->available) sk->surface_unref((sk_surface_t *)h->sk_surface);
        h->sk_surface = NULL;
        h->sk_canvas  = NULL;
    }
    free(h->pixels);
    free(h);
}

/* True (1) when the real Skia library is loaded and its required symbols
 * resolved; 0 when the shim is on its software-raster fallback. A process-wide
 * capability probe (does not by itself prove a surface was created — see
 * ruxen_canvas_skia_active for that). */
int64_t ruxen_canvas_skia_available(int64_t self) {
    (void)self;
    return rx_skia()->available ? 1 : 0;
}

/* True (1) only when THIS host has a live Skia raster surface — i.e. Skia is
 * loaded AND sk_surface_new_raster_direct actually succeeded for this buffer,
 * so draws are genuinely going through Skia (not silently falling back). This
 * is the unambiguous "Skia is rendering into me" signal the pin tests assert,
 * forcing the surface to be created if it has not been yet. */
int64_t ruxen_canvas_skia_active(int64_t self) {
    RxHost *h = (RxHost *)self;
    if (!h) return 0;
    return rx_host_canvas(h) != NULL ? 1 : 0;
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
    /* Reset canvas state so NOTHING (transform or clip) leaks across frames.
     * restore_to_count(1) pops everything above the pristine base; reset_matrix
     * clears any base-level matrix change. Then we push one save so the whole
     * frame runs at depth >= 2 — this is what lets the NEXT begin_frame's
     * restore_to_count(1) discard even a clip the caller applied WITHOUT a save
     * (a base-level clip is not undone by restore_to_count alone, and there is
     * no reset-clip call). Same approach Flutter uses (an outer per-frame save). */
    sk_canvas_t *canvas = rx_host_canvas(h);
    if (canvas) {
        const RxSkia *sk = rx_skia();
        if (sk->canvas_restore_to_count) sk->canvas_restore_to_count(canvas, 1);
        if (sk->canvas_reset_matrix) sk->canvas_reset_matrix(canvas);
        if (sk->canvas_save) sk->canvas_save(canvas);
    }
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

/* ---- canvas state: save/restore + transforms + clipping ----
 *
 * These manage the Skia canvas's matrix + clip stack — the foundation L2 uses
 * for scrolling (translate), nested layouts, and overflow/masking (clip).
 * They are best-effort state ops: on the software fallback (no Skia surface)
 * they no-op and return OK, so an L2 save/translate/draw/restore sequence stays
 * balanced (draws just land untransformed). All require an open frame; state is
 * reset at begin_frame so nothing leaks between frames. */

/* Push the current matrix+clip; returns the save count to pair with
 * restore_to (0 outside a frame / no surface). */
int64_t ruxen_canvas_save(int64_t self) {
    RxHost *h = (RxHost *)self;
    if (!h || !h->in_frame) return 0;
    sk_canvas_t *canvas = rx_host_canvas(h);
    const RxSkia *sk = rx_skia();
    if (canvas && sk->canvas_save) return (int64_t)sk->canvas_save(canvas);
    return 0;
}

/* Pop the last save (matrix+clip). */
int64_t ruxen_canvas_restore(int64_t self) {
    RxHost *h = (RxHost *)self;
    if (!h) return RXC_ERR_BAD_ARGS;
    if (!h->in_frame) return RXC_ERR_NO_FRAME;
    sk_canvas_t *canvas = rx_host_canvas(h);
    const RxSkia *sk = rx_skia();
    if (canvas && sk->canvas_restore) sk->canvas_restore(canvas);
    return RXC_OK;
}

/* Restore down to a save count from a prior save (unwinds nested saves). */
int64_t ruxen_canvas_restore_to_count(int64_t self, int64_t count) {
    RxHost *h = (RxHost *)self;
    if (!h) return RXC_ERR_BAD_ARGS;
    if (!h->in_frame) return RXC_ERR_NO_FRAME;
    sk_canvas_t *canvas = rx_host_canvas(h);
    const RxSkia *sk = rx_skia();
    if (canvas && sk->canvas_restore_to_count && count >= 1) {
        sk->canvas_restore_to_count(canvas, (int)count);
    }
    return RXC_OK;
}

/* The current save-stack depth (1 at the base). */
int64_t ruxen_canvas_save_count(int64_t self) {
    RxHost *h = (RxHost *)self;
    if (!h) return 0;
    sk_canvas_t *canvas = rx_host_canvas(h);
    const RxSkia *sk = rx_skia();
    if (canvas && sk->canvas_get_save_count) return (int64_t)sk->canvas_get_save_count(canvas);
    return 1;
}

int64_t ruxen_canvas_translate(int64_t self, double dx, double dy) {
    RxHost *h = (RxHost *)self;
    if (!h) return RXC_ERR_BAD_ARGS;
    if (!h->in_frame) return RXC_ERR_NO_FRAME;
    if (!rxc_finite_pixels(dx) || !rxc_finite_pixels(dy)) return RXC_ERR_BAD_ARGS;
    sk_canvas_t *canvas = rx_host_canvas(h);
    const RxSkia *sk = rx_skia();
    if (canvas && sk->canvas_translate) sk->canvas_translate(canvas, (float)dx, (float)dy);
    return RXC_OK;
}

int64_t ruxen_canvas_scale(int64_t self, double sx, double sy) {
    RxHost *h = (RxHost *)self;
    if (!h) return RXC_ERR_BAD_ARGS;
    if (!h->in_frame) return RXC_ERR_NO_FRAME;
    if (!rxc_finite_pixels(sx) || !rxc_finite_pixels(sy)) return RXC_ERR_BAD_ARGS;
    sk_canvas_t *canvas = rx_host_canvas(h);
    const RxSkia *sk = rx_skia();
    if (canvas && sk->canvas_scale) sk->canvas_scale(canvas, (float)sx, (float)sy);
    return RXC_OK;
}

int64_t ruxen_canvas_rotate(int64_t self, double degrees) {
    RxHost *h = (RxHost *)self;
    if (!h) return RXC_ERR_BAD_ARGS;
    if (!h->in_frame) return RXC_ERR_NO_FRAME;
    if (!rxc_finite_pixels(degrees)) return RXC_ERR_BAD_ARGS;
    sk_canvas_t *canvas = rx_host_canvas(h);
    const RxSkia *sk = rx_skia();
    if (canvas && sk->canvas_rotate_degrees) sk->canvas_rotate_degrees(canvas, (float)degrees);
    return RXC_OK;
}

/* Intersect the clip with a rectangle (antialiased). Scope with save/restore. */
int64_t ruxen_canvas_clip_rect(int64_t self, double x, double y, double w, double hgt) {
    RxHost *h = (RxHost *)self;
    if (!h) return RXC_ERR_BAD_ARGS;
    if (!h->in_frame) return RXC_ERR_NO_FRAME;
    if (rxc_is_nan(w) || rxc_is_nan(hgt)) return RXC_ERR_BAD_ARGS;
    if (!rxc_finite_pixels(x) || !rxc_finite_pixels(y) ||
        !rxc_finite_pixels(w) || !rxc_finite_pixels(hgt)) {
        return RXC_ERR_BAD_ARGS;
    }
    sk_canvas_t *canvas = rx_host_canvas(h);
    const RxSkia *sk = rx_skia();
    if (canvas && sk->canvas_clip_rect) {
        sk_rect_t rect = { (float)x, (float)y, (float)(x + w), (float)(y + hgt) };
        sk->canvas_clip_rect(canvas, &rect, RX_SK_CLIP_INTERSECT, 1);
    }
    return RXC_OK;
}

/* Intersect the clip with a uniform rounded rectangle (antialiased) — rounded
 * masks / overflow. Scope with save/restore. */
int64_t ruxen_canvas_clip_round_rect(int64_t self, double x, double y, double w, double hgt,
                                     double radius) {
    RxHost *h = (RxHost *)self;
    if (!h) return RXC_ERR_BAD_ARGS;
    if (!h->in_frame) return RXC_ERR_NO_FRAME;
    if (rxc_is_nan(w) || rxc_is_nan(hgt)) return RXC_ERR_BAD_ARGS;
    if (!rxc_finite_pixels(x) || !rxc_finite_pixels(y) || !rxc_finite_pixels(w) ||
        !rxc_finite_pixels(hgt) || !rxc_finite_pixels(radius)) {
        return RXC_ERR_BAD_ARGS;
    }
    sk_canvas_t *canvas = rx_host_canvas(h);
    const RxSkia *sk = rx_skia();
    if (canvas && sk->canvas_clip_rrect && sk->rrect_new && sk->rrect_set_rect_radii &&
        sk->rrect_delete) {
        double rad = radius > 0.0 ? radius : 0.0;
        sk_rrect_t *rr = sk->rrect_new();
        if (rr) {
            sk_rect_t rect = { (float)x, (float)y, (float)(x + w), (float)(y + hgt) };
            sk_vector_t radii[4] = {
                { (float)rad, (float)rad }, { (float)rad, (float)rad },
                { (float)rad, (float)rad }, { (float)rad, (float)rad },
            };
            sk->rrect_set_rect_radii(rr, &rect, radii);
            sk->canvas_clip_rrect(canvas, rr, RX_SK_CLIP_INTERSECT, 1);
            sk->rrect_delete(rr);
        }
    }
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

    sk_canvas_t *canvas = rx_host_canvas(h);
    if (canvas) {
        const RxSkia *sk = rx_skia();
        sk->canvas_clear(canvas, (sk_color_t)rxc_pack(r, g, b, a));
        return RXC_OK;
    }

    /* software fallback */
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

    sk_canvas_t *canvas = rx_host_canvas(h);
    if (canvas) {
        const RxSkia *sk = rx_skia();
        sk_paint_t *paint = sk->paint_new();
        if (!paint) return RXC_ERR_BAD_ARGS;
        sk->paint_set_antialias(paint, 0);   /* crisp integer-aligned edges */
        sk->paint_set_style(paint, RX_SK_PAINT_FILL);
        sk->paint_set_color(paint, (sk_color_t)rxc_pack(r, g, b, a));
        sk_rect_t rect = { (float)x, (float)y, (float)(x + w), (float)(y + hgt) };
        sk->canvas_draw_rect(canvas, &rect, paint);   /* Skia clips to surface */
        sk->paint_delete(paint);
        return RXC_OK;
    }

    /* software fallback: half-open floor box [floor(x),floor(x+w)) clipped */
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

/* ---- Skia-native primitives (no software fallback) ----
 *
 * These shapes have no software-raster implementation: when Skia is not loaded
 * they return RXC_ERR_NO_SKIA so the Ruxen side surfaces a clear Err (never a
 * silent no-op — docs/FFI.md). Each is antialiased. `stroke_w > 0` strokes an
 * outline of that width; `stroke_w <= 0` fills. Color arrives packed as
 * 0xAARRGGBB in the low 32 bits of `argb`. */

/* Build a paint for the given packed color and stroke width, or NULL on
 * allocation failure. Caller owns it (paint_delete). */
static sk_paint_t *rx_make_paint(const RxSkia *sk, int64_t argb, double stroke_w) {
    sk_paint_t *p = sk->paint_new();
    if (!p) return NULL;
    sk->paint_set_antialias(p, 1);
    if (stroke_w > 0.0) {
        sk->paint_set_style(p, RX_SK_PAINT_STROKE);
        if (sk->paint_set_stroke_width) sk->paint_set_stroke_width(p, (float)stroke_w);
    } else {
        sk->paint_set_style(p, RX_SK_PAINT_FILL);
    }
    sk->paint_set_color(p, (sk_color_t)(uint32_t)argb);
    return p;
}

/* Common entry guard: validate host + frame, ensure the Skia surface and the
 * given drawing function pointer are live. Returns the canvas (and the table)
 * or NULL with *err set. */
static sk_canvas_t *rx_skia_draw_begin(RxHost *h, const void *fnptr,
                                       const RxSkia **out_sk, int64_t *err) {
    if (!h) { *err = RXC_ERR_BAD_ARGS; return NULL; }
    if (!h->in_frame) { *err = RXC_ERR_NO_FRAME; return NULL; }
    sk_canvas_t *canvas = rx_host_canvas(h);
    if (!canvas || !fnptr) { *err = RXC_ERR_NO_SKIA; return NULL; }
    *out_sk = rx_skia();
    return canvas;
}

/* Filled/stroked circle centered at (cx, cy). */
int64_t ruxen_canvas_draw_circle(int64_t self, double cx, double cy, double radius,
                                 double stroke_w, int64_t argb) {
    RxHost *h = (RxHost *)self;
    const RxSkia *sk = NULL;
    int64_t err = RXC_OK;
    sk_canvas_t *canvas = rx_skia_draw_begin(h, h ? (const void *)rx_skia()->canvas_draw_oval : NULL,
                                             &sk, &err);
    if (!canvas) return err;
    if (rxc_is_nan(radius) || !(radius > 0.0)) return RXC_OK;   /* empty */
    if (!rxc_finite_pixels(cx) || !rxc_finite_pixels(cy) ||
        !rxc_finite_pixels(radius) || !rxc_finite_pixels(stroke_w)) {
        return RXC_ERR_BAD_ARGS;
    }
    sk_paint_t *paint = rx_make_paint(sk, argb, stroke_w);
    if (!paint) return RXC_ERR_BAD_ARGS;
    sk_rect_t bounds = { (float)(cx - radius), (float)(cy - radius),
                         (float)(cx + radius), (float)(cy + radius) };
    sk->canvas_draw_oval(canvas, &bounds, paint);
    sk->paint_delete(paint);
    return RXC_OK;
}

/* Filled/stroked rounded rectangle with a single uniform corner radius. */
int64_t ruxen_canvas_draw_round_rect(int64_t self, double x, double y, double w, double hgt,
                                     double radius, double stroke_w, int64_t argb) {
    RxHost *h = (RxHost *)self;
    const RxSkia *sk = NULL;
    int64_t err = RXC_OK;
    sk_canvas_t *canvas = rx_skia_draw_begin(h, h ? (const void *)rx_skia()->canvas_draw_round_rect : NULL,
                                             &sk, &err);
    if (!canvas) return err;
    if (rxc_is_nan(w) || rxc_is_nan(hgt)) return RXC_ERR_BAD_ARGS;
    if (!(w > 0.0) || !(hgt > 0.0)) return RXC_OK;              /* empty */
    if (!rxc_finite_pixels(x) || !rxc_finite_pixels(y) ||
        !rxc_finite_pixels(w) || !rxc_finite_pixels(hgt) ||
        !rxc_finite_pixels(radius) || !rxc_finite_pixels(stroke_w)) {
        return RXC_ERR_BAD_ARGS;
    }
    double rad = radius > 0.0 ? radius : 0.0;
    sk_paint_t *paint = rx_make_paint(sk, argb, stroke_w);
    if (!paint) return RXC_ERR_BAD_ARGS;
    sk_rect_t rect = { (float)x, (float)y, (float)(x + w), (float)(y + hgt) };
    sk->canvas_draw_round_rect(canvas, &rect, (float)rad, (float)rad, paint);
    sk->paint_delete(paint);
    return RXC_OK;
}

/* Filled/stroked rounded rectangle with independent corner radii (each a single
 * symmetric x=y radius): tl, tr, br, bl. Enables one-side-only / pill / tab
 * shapes. Needs the sk_rrect builder symbols. */
int64_t ruxen_canvas_draw_rrect_radii(int64_t self, double x, double y, double w, double hgt,
                                      double tl, double tr, double br, double bl,
                                      double stroke_w, int64_t argb) {
    RxHost *h = (RxHost *)self;
    if (!h) return RXC_ERR_BAD_ARGS;
    if (!h->in_frame) return RXC_ERR_NO_FRAME;
    sk_canvas_t *canvas = rx_host_canvas(h);
    const RxSkia *sk = rx_skia();
    if (!canvas || !sk->canvas_draw_rrect || !sk->rrect_new ||
        !sk->rrect_set_rect_radii || !sk->rrect_delete) {
        return RXC_ERR_NO_SKIA;
    }
    if (rxc_is_nan(w) || rxc_is_nan(hgt)) return RXC_ERR_BAD_ARGS;
    if (!(w > 0.0) || !(hgt > 0.0)) return RXC_OK;              /* empty */
    if (!rxc_finite_pixels(x) || !rxc_finite_pixels(y) ||
        !rxc_finite_pixels(w) || !rxc_finite_pixels(hgt) ||
        !rxc_finite_pixels(tl) || !rxc_finite_pixels(tr) ||
        !rxc_finite_pixels(br) || !rxc_finite_pixels(bl) ||
        !rxc_finite_pixels(stroke_w)) {
        return RXC_ERR_BAD_ARGS;
    }
    if (tl < 0.0) tl = 0.0;
    if (tr < 0.0) tr = 0.0;
    if (br < 0.0) br = 0.0;
    if (bl < 0.0) bl = 0.0;

    sk_paint_t *paint = rx_make_paint(sk, argb, stroke_w);
    if (!paint) return RXC_ERR_BAD_ARGS;
    sk_rrect_t *rr = sk->rrect_new();
    if (!rr) { sk->paint_delete(paint); return RXC_ERR_BAD_ARGS; }
    sk_rect_t rect = { (float)x, (float)y, (float)(x + w), (float)(y + hgt) };
    /* radii order: upper-left, upper-right, lower-right, lower-left */
    sk_vector_t radii[4] = {
        { (float)tl, (float)tl }, { (float)tr, (float)tr },
        { (float)br, (float)br }, { (float)bl, (float)bl },
    };
    sk->rrect_set_rect_radii(rr, &rect, radii);
    sk->canvas_draw_rrect(canvas, rr, paint);
    sk->rrect_delete(rr);
    sk->paint_delete(paint);
    return RXC_OK;
}

/* Stroked line from (x0, y0) to (x1, y1). stroke_w <= 0 draws a 1px hairline. */
int64_t ruxen_canvas_draw_line(int64_t self, double x0, double y0, double x1, double y1,
                               double stroke_w, int64_t argb) {
    RxHost *h = (RxHost *)self;
    const RxSkia *sk = NULL;
    int64_t err = RXC_OK;
    sk_canvas_t *canvas = rx_skia_draw_begin(h, h ? (const void *)rx_skia()->canvas_draw_line : NULL,
                                             &sk, &err);
    if (!canvas) return err;
    if (!rxc_finite_pixels(x0) || !rxc_finite_pixels(y0) ||
        !rxc_finite_pixels(x1) || !rxc_finite_pixels(y1) ||
        !rxc_finite_pixels(stroke_w)) {
        return RXC_ERR_BAD_ARGS;
    }
    double width = stroke_w > 0.0 ? stroke_w : 1.0;
    sk_paint_t *paint = rx_make_paint(sk, argb, width);
    if (!paint) return RXC_ERR_BAD_ARGS;
    sk->canvas_draw_line(canvas, (float)x0, (float)y0, (float)x1, (float)y1, paint);
    sk->paint_delete(paint);
    return RXC_OK;
}

/* ---- gradients & shadows (Skia-only) ----
 *
 * Two-stop gradients (the common UI case) — colours arrive packed as
 * 0xAARRGGBB; the shim builds the 2-element colour/point arrays locally so no
 * array crosses the FFI. Soft shadows use a blur mask filter. All antialiased;
 * RXC_ERR_NO_SKIA when the backend or the needed symbols are absent. */

/* A fill paint carrying a 2-stop linear gradient between (x0,y0)->(x1,y1).
 * The paint owns the shader (we drop our ref after set). NULL on failure. */
static sk_paint_t *rx_linear_gradient_paint(const RxSkia *sk,
        double x0, double y0, double x1, double y1, int64_t argb0, int64_t argb1) {
    if (!sk->shader_new_linear_gradient || !sk->paint_set_shader || !sk->shader_unref) return NULL;
    sk_point_t pts[2] = { { (float)x0, (float)y0 }, { (float)x1, (float)y1 } };
    sk_color_t cols[2] = { (sk_color_t)(uint32_t)argb0, (sk_color_t)(uint32_t)argb1 };
    sk_shader_t *sh = sk->shader_new_linear_gradient(pts, cols, NULL, 2, RX_SK_TILE_CLAMP, NULL);
    if (!sh) return NULL;
    sk_paint_t *p = sk->paint_new();
    if (!p) { sk->shader_unref(sh); return NULL; }
    sk->paint_set_antialias(p, 1);
    sk->paint_set_style(p, RX_SK_PAINT_FILL);
    sk->paint_set_shader(p, sh);
    sk->shader_unref(sh);
    return p;
}

/* A fill paint carrying a 2-stop radial gradient: argb0 at the centre,
 * argb1 at the rim. NULL on failure. */
static sk_paint_t *rx_radial_gradient_paint(const RxSkia *sk,
        double cx, double cy, double radius, int64_t argb0, int64_t argb1) {
    if (!sk->shader_new_radial_gradient || !sk->paint_set_shader || !sk->shader_unref) return NULL;
    sk_point_t center = { (float)cx, (float)cy };
    sk_color_t cols[2] = { (sk_color_t)(uint32_t)argb0, (sk_color_t)(uint32_t)argb1 };
    sk_shader_t *sh = sk->shader_new_radial_gradient(&center, (float)radius, cols, NULL, 2,
                                                     RX_SK_TILE_CLAMP, NULL);
    if (!sh) return NULL;
    sk_paint_t *p = sk->paint_new();
    if (!p) { sk->shader_unref(sh); return NULL; }
    sk->paint_set_antialias(p, 1);
    sk->paint_set_style(p, RX_SK_PAINT_FILL);
    sk->paint_set_shader(p, sh);
    sk->shader_unref(sh);
    return p;
}

/* Fill a rectangle with a 2-stop linear gradient. */
int64_t ruxen_canvas_fill_rect_gradient(int64_t self, double x, double y, double w, double hgt,
        double gx0, double gy0, double gx1, double gy1, int64_t argb0, int64_t argb1) {
    RxHost *h = (RxHost *)self;
    if (!h) return RXC_ERR_BAD_ARGS;
    if (!h->in_frame) return RXC_ERR_NO_FRAME;
    sk_canvas_t *canvas = rx_host_canvas(h);
    const RxSkia *sk = rx_skia();
    if (!canvas || !sk->canvas_draw_rect || !sk->shader_new_linear_gradient ||
        !sk->paint_set_shader || !sk->shader_unref) {
        return RXC_ERR_NO_SKIA;
    }
    if (rxc_is_nan(w) || rxc_is_nan(hgt)) return RXC_ERR_BAD_ARGS;
    if (!(w > 0.0) || !(hgt > 0.0)) return RXC_OK;
    if (!rxc_finite_pixels(x) || !rxc_finite_pixels(y) || !rxc_finite_pixels(w) ||
        !rxc_finite_pixels(hgt) || !rxc_finite_pixels(gx0) || !rxc_finite_pixels(gy0) ||
        !rxc_finite_pixels(gx1) || !rxc_finite_pixels(gy1)) {
        return RXC_ERR_BAD_ARGS;
    }
    sk_paint_t *paint = rx_linear_gradient_paint(sk, gx0, gy0, gx1, gy1, argb0, argb1);
    if (!paint) return RXC_ERR_NO_SKIA;
    sk_rect_t rect = { (float)x, (float)y, (float)(x + w), (float)(y + hgt) };
    sk->canvas_draw_rect(canvas, &rect, paint);
    sk->paint_delete(paint);
    return RXC_OK;
}

/* Fill a uniform rounded rectangle with a 2-stop linear gradient (button bg). */
int64_t ruxen_canvas_fill_round_rect_gradient(int64_t self, double x, double y, double w, double hgt,
        double radius, double gx0, double gy0, double gx1, double gy1, int64_t argb0, int64_t argb1) {
    RxHost *h = (RxHost *)self;
    if (!h) return RXC_ERR_BAD_ARGS;
    if (!h->in_frame) return RXC_ERR_NO_FRAME;
    sk_canvas_t *canvas = rx_host_canvas(h);
    const RxSkia *sk = rx_skia();
    if (!canvas || !sk->canvas_draw_round_rect || !sk->shader_new_linear_gradient ||
        !sk->paint_set_shader || !sk->shader_unref) {
        return RXC_ERR_NO_SKIA;
    }
    if (rxc_is_nan(w) || rxc_is_nan(hgt)) return RXC_ERR_BAD_ARGS;
    if (!(w > 0.0) || !(hgt > 0.0)) return RXC_OK;
    if (!rxc_finite_pixels(x) || !rxc_finite_pixels(y) || !rxc_finite_pixels(w) ||
        !rxc_finite_pixels(hgt) || !rxc_finite_pixels(radius) || !rxc_finite_pixels(gx0) ||
        !rxc_finite_pixels(gy0) || !rxc_finite_pixels(gx1) || !rxc_finite_pixels(gy1)) {
        return RXC_ERR_BAD_ARGS;
    }
    double rad = radius > 0.0 ? radius : 0.0;
    sk_paint_t *paint = rx_linear_gradient_paint(sk, gx0, gy0, gx1, gy1, argb0, argb1);
    if (!paint) return RXC_ERR_NO_SKIA;
    sk_rect_t rect = { (float)x, (float)y, (float)(x + w), (float)(y + hgt) };
    sk->canvas_draw_round_rect(canvas, &rect, (float)rad, (float)rad, paint);
    sk->paint_delete(paint);
    return RXC_OK;
}

/* Fill a circle with a 2-stop radial gradient (argb0 centre -> argb1 rim). */
int64_t ruxen_canvas_fill_circle_radial(int64_t self, double cx, double cy, double radius,
        int64_t argb0, int64_t argb1) {
    RxHost *h = (RxHost *)self;
    if (!h) return RXC_ERR_BAD_ARGS;
    if (!h->in_frame) return RXC_ERR_NO_FRAME;
    sk_canvas_t *canvas = rx_host_canvas(h);
    const RxSkia *sk = rx_skia();
    if (!canvas || !sk->canvas_draw_oval || !sk->shader_new_radial_gradient ||
        !sk->paint_set_shader || !sk->shader_unref) {
        return RXC_ERR_NO_SKIA;
    }
    if (rxc_is_nan(radius) || !(radius > 0.0)) return RXC_OK;
    if (!rxc_finite_pixels(cx) || !rxc_finite_pixels(cy) || !rxc_finite_pixels(radius)) {
        return RXC_ERR_BAD_ARGS;
    }
    sk_paint_t *paint = rx_radial_gradient_paint(sk, cx, cy, radius, argb0, argb1);
    if (!paint) return RXC_ERR_NO_SKIA;
    sk_rect_t bounds = { (float)(cx - radius), (float)(cy - radius),
                         (float)(cx + radius), (float)(cy + radius) };
    sk->canvas_draw_oval(canvas, &bounds, paint);
    sk->paint_delete(paint);
    return RXC_OK;
}

/* Draw a soft (blurred) rounded rectangle — a drop shadow. `blur` is the blur
 * radius in pixels; the caller offsets the rect for the shadow direction. */
int64_t ruxen_canvas_draw_round_rect_shadow(int64_t self, double x, double y, double w, double hgt,
        double radius, double blur, int64_t argb) {
    RxHost *h = (RxHost *)self;
    if (!h) return RXC_ERR_BAD_ARGS;
    if (!h->in_frame) return RXC_ERR_NO_FRAME;
    sk_canvas_t *canvas = rx_host_canvas(h);
    const RxSkia *sk = rx_skia();
    if (!canvas || !sk->canvas_draw_round_rect || !sk->maskfilter_new_blur ||
        !sk->paint_set_maskfilter || !sk->maskfilter_unref) {
        return RXC_ERR_NO_SKIA;
    }
    if (rxc_is_nan(w) || rxc_is_nan(hgt)) return RXC_ERR_BAD_ARGS;
    if (!(w > 0.0) || !(hgt > 0.0)) return RXC_OK;
    if (!rxc_finite_pixels(x) || !rxc_finite_pixels(y) || !rxc_finite_pixels(w) ||
        !rxc_finite_pixels(hgt) || !rxc_finite_pixels(radius) || !rxc_finite_pixels(blur)) {
        return RXC_ERR_BAD_ARGS;
    }
    double rad = radius > 0.0 ? radius : 0.0;
    sk_paint_t *paint = sk->paint_new();
    if (!paint) return RXC_ERR_BAD_ARGS;
    sk->paint_set_antialias(paint, 1);
    sk->paint_set_style(paint, RX_SK_PAINT_FILL);
    sk->paint_set_color(paint, (sk_color_t)(uint32_t)argb);
    /* Skia blur sigma ~= radius/2 for a comparable visual spread. */
    double sigma = blur > 0.0 ? blur * 0.5 : 0.0;
    if (sigma > 0.0) {
        sk_maskfilter_t *mf = sk->maskfilter_new_blur(RX_SK_BLUR_NORMAL, (float)sigma);
        if (mf) {
            sk->paint_set_maskfilter(paint, mf);
            sk->maskfilter_unref(mf);
        }
    }
    sk_rect_t rect = { (float)x, (float)y, (float)(x + w), (float)(y + hgt) };
    sk->canvas_draw_round_rect(canvas, &rect, (float)rad, (float)rad, paint);
    sk->paint_delete(paint);
    return RXC_OK;
}

/* ---- images (Skia-only) ----
 *
 * An image is a decoded sk_image owned by the Ruxen Image; its raw pointer
 * crosses the FFI as an int64 handle (load returns it, draw takes it, drop
 * frees it). Encoded bytes (PNG/JPEG/WebP) are decoded via Skia's codecs. */

/* Decode an image file; returns the sk_image pointer as an int64 handle, or 0
 * on failure (bad path / undecodable / Skia unavailable). `path` is the C
 * string of an &String from Ruxen. */
int64_t ruxen_canvas_image_load(int64_t path) {
    const char *p = (const char *)path;
    if (!p) return 0;
    const RxSkia *sk = rx_skia();
    if (!sk->available || !sk->data_new_from_file || !sk->image_new_from_encoded ||
        !sk->data_unref) {
        return 0;
    }
    sk_data_t *data = sk->data_new_from_file(p);
    if (!data) return 0;
    sk_image_t *img = sk->image_new_from_encoded(data);
    sk->data_unref(data);   /* the image keeps its own ref to the pixels */
    return (int64_t)img;    /* 0 if the bytes were not a decodable image */
}

/* Identity accessor: a RawImage's handle IS the pointer; this lets the Ruxen
 * side pass it to draw calls as a plain Int. */
int64_t ruxen_canvas_image_ptr(int64_t self) { return self; }

int64_t ruxen_canvas_image_is_null(int64_t self) { return self == 0 ? 1 : 0; }

int64_t ruxen_canvas_image_width(int64_t self) {
    const RxSkia *sk = rx_skia();
    if (!self || !sk->image_get_width) return 0;
    return (int64_t)sk->image_get_width((sk_image_t *)self);
}

int64_t ruxen_canvas_image_height(int64_t self) {
    const RxSkia *sk = rx_skia();
    if (!self || !sk->image_get_height) return 0;
    return (int64_t)sk->image_get_height((sk_image_t *)self);
}

void ruxen_canvas_image_drop(int64_t self) {
    const RxSkia *sk = rx_skia();
    if (self && sk->image_unref) sk->image_unref((sk_image_t *)self);
}

/* Shared blit: draw `image`'s `src` region into `dst` (linear sampling). */
static int64_t rx_draw_image(RxHost *h, sk_image_t *image, sk_rect_t src, sk_rect_t dst) {
    if (!h || !image) return RXC_ERR_BAD_ARGS;
    if (!h->in_frame) return RXC_ERR_NO_FRAME;
    sk_canvas_t *canvas = rx_host_canvas(h);
    const RxSkia *sk = rx_skia();
    if (!canvas || !sk->canvas_draw_image_rect) return RXC_ERR_NO_SKIA;
    /* {max_aniso, use_cubic, cubic_b, cubic_c, filter, mipmap} */
    sk_sampling_options_t samp = { 0, 0, 0.0f, 0.0f, RX_SK_FILTER_LINEAR, 0 };
    sk->canvas_draw_image_rect(canvas, image, &src, &dst, &samp, NULL, 0);
    return RXC_OK;
}

/* Draw an image at (x, y) at its natural pixel size. */
int64_t ruxen_canvas_draw_image(int64_t self, int64_t img, double x, double y) {
    RxHost *h = (RxHost *)self;
    sk_image_t *image = (sk_image_t *)img;
    const RxSkia *sk = rx_skia();
    if (!h || !image) return RXC_ERR_BAD_ARGS;
    if (!h->in_frame) return RXC_ERR_NO_FRAME;
    if (!sk->image_get_width || !sk->image_get_height) return RXC_ERR_NO_SKIA;
    if (!rxc_finite_pixels(x) || !rxc_finite_pixels(y)) return RXC_ERR_BAD_ARGS;
    float iw = (float)sk->image_get_width(image);
    float ih = (float)sk->image_get_height(image);
    sk_rect_t src = { 0.0f, 0.0f, iw, ih };
    sk_rect_t dst = { (float)x, (float)y, (float)x + iw, (float)y + ih };
    return rx_draw_image(h, image, src, dst);
}

/* Draw an image scaled to fill the destination rectangle. */
int64_t ruxen_canvas_draw_image_rect(int64_t self, int64_t img, double dx, double dy,
                                     double dw, double dh) {
    RxHost *h = (RxHost *)self;
    sk_image_t *image = (sk_image_t *)img;
    const RxSkia *sk = rx_skia();
    if (!h || !image) return RXC_ERR_BAD_ARGS;
    if (!h->in_frame) return RXC_ERR_NO_FRAME;
    if (!sk->image_get_width || !sk->image_get_height) return RXC_ERR_NO_SKIA;
    if (rxc_is_nan(dw) || rxc_is_nan(dh)) return RXC_ERR_BAD_ARGS;
    if (!(dw > 0.0) || !(dh > 0.0)) return RXC_OK;
    if (!rxc_finite_pixels(dx) || !rxc_finite_pixels(dy) ||
        !rxc_finite_pixels(dw) || !rxc_finite_pixels(dh)) {
        return RXC_ERR_BAD_ARGS;
    }
    float iw = (float)sk->image_get_width(image);
    float ih = (float)sk->image_get_height(image);
    sk_rect_t src = { 0.0f, 0.0f, iw, ih };
    sk_rect_t dst = { (float)dx, (float)dy, (float)(dx + dw), (float)(dy + dh) };
    return rx_draw_image(h, image, src, dst);
}

/* Draw a sub-region (sx,sy,sw,sh) of an image into a destination rect — for
 * sprite sheets / atlases. */
int64_t ruxen_canvas_draw_image_rect_src(int64_t self, int64_t img,
        double sx, double sy, double sw, double sh,
        double dx, double dy, double dw, double dh) {
    RxHost *h = (RxHost *)self;
    sk_image_t *image = (sk_image_t *)img;
    if (!h || !image) return RXC_ERR_BAD_ARGS;
    if (rxc_is_nan(sw) || rxc_is_nan(sh) || rxc_is_nan(dw) || rxc_is_nan(dh)) return RXC_ERR_BAD_ARGS;
    if (!(sw > 0.0) || !(sh > 0.0) || !(dw > 0.0) || !(dh > 0.0)) return RXC_OK;
    if (!rxc_finite_pixels(sx) || !rxc_finite_pixels(sy) || !rxc_finite_pixels(sw) ||
        !rxc_finite_pixels(sh) || !rxc_finite_pixels(dx) || !rxc_finite_pixels(dy) ||
        !rxc_finite_pixels(dw) || !rxc_finite_pixels(dh)) {
        return RXC_ERR_BAD_ARGS;
    }
    sk_rect_t src = { (float)sx, (float)sy, (float)(sx + sw), (float)(sy + sh) };
    sk_rect_t dst = { (float)dx, (float)dy, (float)(dx + dw), (float)(dy + dh) };
    return rx_draw_image(h, image, src, dst);
}

#include "bitmap_font.h"

/* Pixel size of the default Skia font. The bitmap fallback is a fixed 7px
 * face; this is chosen close to it so layouts stay sane across backends. */
#define RXC_SKIA_FONT_PX 13.0f

/* The process-wide default Skia font (system default typeface at a fixed
 * size), built once on first use. NULL when Skia / the font symbols are
 * unavailable, in which case callers use the 5x7 bitmap path. */
static sk_font_t *rx_default_font(void) {
    static sk_font_t *font = NULL;
    static int tried = 0;
    if (tried) return font;
    tried = 1;
    const RxSkia *sk = rx_skia();
    /* Require EVERY text symbol the font path uses (draw + measure + metrics),
     * so draw_text/measure_text/text_height all make the same backend decision.
     * If any is missing they fall back to the bitmap together — never Skia-draw
     * with bitmap-measured advances (which would mis-center labels). */
    if (!sk->available || !sk->font_new_with_values || !sk->canvas_draw_simple_text ||
        !sk->font_measure_text || !sk->font_get_metrics || !sk->font_set_size) {
        return NULL;
    }
    sk_typeface_t *tf = sk->typeface_create_default ? sk->typeface_create_default() : NULL;
    sk_font_t *f = sk->font_new_with_values(tf, RXC_SKIA_FONT_PX, 1.0f, 0.0f);
    if (!f) return NULL;
    /* Guard against an empty-typeface font (some Skia builds give a NULL
     * typeface zero glyphs — zero-width, no ink). A real face measures 'M' as a
     * positive advance; if not, drop it so the bitmap path takes over. */
    float probe = sk->font_measure_text(f, "M", 1, RX_SK_TEXT_UTF8, NULL, NULL);
    if (!(probe > 0.0f)) {
        if (sk->font_delete) sk->font_delete(f);
        return NULL;
    }
    font = f;
    return font;
}

/* The default font with its size set to `size` px (resized in place — single
 * owner, so safe). NULL when no Skia font is available. Pass RXC_SKIA_FONT_PX
 * for the default size. */
static sk_font_t *rx_font_at(double size) {
    sk_font_t *font = rx_default_font();
    if (!font) return NULL;
    const RxSkia *sk = rx_skia();
    if (sk->font_set_size && size > 0.0) sk->font_set_size(font, (float)size);
    return font;
}

/* Shared text impl at an explicit font size. `argb` is packed 0xAARRGGBB. */
static int64_t rx_draw_text_impl(RxHost *h, const char *s, double x, double y,
                                 int64_t argb, double size) {
    if (!h || !s) return RXC_ERR_BAD_ARGS;
    if (!h->in_frame) return RXC_ERR_NO_FRAME;
    if (!rxc_finite_pixels(x) || !rxc_finite_pixels(y)) return RXC_ERR_BAD_ARGS;

    /* Skia path: a real antialiased font. (x, y) is the baseline origin, the
     * same convention as the bitmap path below. */
    sk_canvas_t *canvas = rx_host_canvas(h);
    const RxSkia *sk = rx_skia();
    sk_font_t *font = rx_font_at(size);
    if (canvas && font && sk->canvas_draw_simple_text) {
        sk_paint_t *paint = sk->paint_new();
        if (!paint) return RXC_ERR_BAD_ARGS;
        sk->paint_set_antialias(paint, 1);
        sk->paint_set_style(paint, RX_SK_PAINT_FILL);
        sk->paint_set_color(paint, (sk_color_t)(uint32_t)argb);
        sk->canvas_draw_simple_text(canvas, s, strlen(s), RX_SK_TEXT_UTF8,
                                    (float)x, (float)y, font, paint);
        sk->paint_delete(paint);
        return RXC_OK;
    }

    /* software fallback: 5x7 bitmap font (fixed size — `size` is ignored) */
    uint32_t src = (uint32_t)argb;
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

static int64_t rx_measure_impl(const char *s, double size) {
    if (!s) return 0;
    const RxSkia *sk = rx_skia();
    sk_font_t *font = rx_font_at(size);
    if (sk->available && font && sk->font_measure_text) {
        float w = sk->font_measure_text(font, s, strlen(s), RX_SK_TEXT_UTF8, NULL, NULL);
        if (!(w > 0.0f)) return 0;
        return (int64_t)(w + 0.5f);
    }
    size_t n = strlen(s);
    return n ? (int64_t)(n * RXC_ADVANCE - 1) : 0;
}

static int64_t rx_text_height_impl(double size) {
    const RxSkia *sk = rx_skia();
    sk_font_t *font = rx_font_at(size);
    if (sk->available && font && sk->font_get_metrics) {
        sk_fontmetrics_t m;
        sk->font_get_metrics(font, &m);   /* ascent is negative (above baseline) */
        float hgt = m.descent - m.ascent;
        if (hgt >= 1.0f) return (int64_t)(hgt + 0.5f);
    }
    return RXC_GLYPH_H;
}

/* Width in pixels of `n` characters at the bitmap font's one size. The
 * character count crosses the FFI; kept for the software path / callers that
 * only have a count. */
int64_t ruxen_canvas_measure_text_n(int64_t self, int64_t n) {
    (void)self;
    if (n <= 0) return 0;
    return n * RXC_ADVANCE - 1;
}

/* Advance width in pixels of `text` as it would actually be drawn. Uses Skia's
 * real font metrics when active (so measure matches draw for centering), else
 * the bitmap advance. `text` is the C string pointer (an &String from Ruxen). */
int64_t ruxen_canvas_measure_text(int64_t self, int64_t text) {
    (void)self;
    return rx_measure_impl((const char *)text, RXC_SKIA_FONT_PX);
}

/* Advance width of `text` at an explicit font `size` px. */
int64_t ruxen_canvas_measure_text_sized(int64_t self, int64_t text, double size) {
    (void)self;
    return rx_measure_impl((const char *)text, size);
}

/* The font's line height in pixels (ascent above + descent below the
 * baseline). Skia metrics when active, else the bitmap's 7px. */
int64_t ruxen_canvas_text_height(int64_t self) {
    (void)self;
    return rx_text_height_impl(RXC_SKIA_FONT_PX);
}

/* Line height at an explicit font `size` px. */
int64_t ruxen_canvas_text_height_sized(int64_t self, double size) {
    (void)self;
    return rx_text_height_impl(size);
}

/* Draw a single line of text at (x, y) BASELINE origin, at the default size.
 * Color as separate r,g,b,a channels (validated). */
int64_t ruxen_canvas_draw_text(int64_t self, int64_t text, double x, double y,
                               int64_t r, int64_t g, int64_t b, int64_t a) {
    if (!rxc_check_color(r, g, b, a)) return RXC_ERR_BAD_ARGS;
    return rx_draw_text_impl((RxHost *)self, (const char *)text, x, y,
                             (int64_t)rxc_pack(r, g, b, a), RXC_SKIA_FONT_PX);
}

/* Draw text at an explicit font `size` px (Skia path); color packed 0xAARRGGBB.
 * The software bitmap fallback renders at its fixed size. */
int64_t ruxen_canvas_draw_text_sized(int64_t self, int64_t text, double x, double y,
                                     double size, int64_t argb) {
    return rx_draw_text_impl((RxHost *)self, (const char *)text, x, y, argb, size);
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
