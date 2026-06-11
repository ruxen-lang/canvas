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

/* Ruxen's String runtime constructor (library/std/string/runtime/string.c): a
 * Ruxen String IS a malloc'd NUL-terminated char*. Used to return the IME marked
 * text to the Ruxen side as a Ruxen-owned String (see ruxen_canvas_event_text). */
extern char *ruxen_string_from(const char *s);

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

/* The native-library basenames to try, in platform-preference order. The
 * fetch script installs the platform's convention into the cache (.dylib on
 * macOS, .so on Linux — see runtime/fetch_skia.sh); we try the native name
 * first but always try both, so a cross-named or system-installed copy still
 * resolves. The system-loader (no path) form uses the same names. */
#if defined(__APPLE__)
static const char *const rx_skia_basenames[] = { "libSkiaSharp.dylib", "libSkiaSharp.so" };
#else
static const char *const rx_skia_basenames[] = { "libSkiaSharp.so", "libSkiaSharp.dylib" };
#endif
#define RX_SKIA_NBASENAMES (int)(sizeof(rx_skia_basenames) / sizeof(rx_skia_basenames[0]))

/* dlopen each basename inside `dir` (absolute), returning the first that loads
 * or NULL. dlopen of an absolute path is CWD-independent. */
static void *rx_skia_dlopen_in_dir(const char *dir) {
    char path[4096];
    for (int i = 0; i < RX_SKIA_NBASENAMES; i++) {
        snprintf(path, sizeof(path), "%s/%s", dir, rx_skia_basenames[i]);
        void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        if (h) return h;
    }
    return NULL;
}

static void *rx_skia_dlopen(void) {
    /* Search order: explicit override, the fetch-script cache (env override,
     * then $HOME/.cache), then the system loader. Each cache location tries the
     * platform's library basenames (.dylib on macOS, .so on Linux). */
    const char *env = getenv("RUXEN_CANVAS_SKIA");
    if (env && env[0]) {
        /* An explicit override is a full path (no basename guessing). */
        void *h = dlopen(env, RTLD_NOW | RTLD_LOCAL);
        if (h) return h;
    }
    const char *cache = getenv("RUXEN_CANVAS_CACHE");
    if (cache && cache[0]) {
        void *h = rx_skia_dlopen_in_dir(cache);
        if (h) return h;
    }
    const char *home = getenv("HOME");
    if (home && home[0]) {
        char dir[4096];
        snprintf(dir, sizeof(dir), "%s/.cache/ruxen-canvas", home);
        void *h = rx_skia_dlopen_in_dir(dir);
        if (h) return h;
    }
    /* Fall back to the system loader (LD_LIBRARY_PATH / DYLD path / ldconfig). */
    for (int i = 0; i < RX_SKIA_NBASENAMES; i++) {
        void *h = dlopen(rx_skia_basenames[i], RTLD_NOW | RTLD_LOCAL);
        if (h) return h;
    }
    return NULL;
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
    RX_SK_OPTIONAL(paint_set_blendmode,       "sk_paint_set_blendmode");
    RX_SK_OPTIONAL(shader_new_linear_gradient,"sk_shader_new_linear_gradient");
    RX_SK_OPTIONAL(shader_new_radial_gradient,"sk_shader_new_radial_gradient");
    RX_SK_OPTIONAL(shader_unref,              "sk_shader_unref");
    RX_SK_OPTIONAL(maskfilter_new_blur,       "sk_maskfilter_new_blur");
    RX_SK_OPTIONAL(maskfilter_unref,          "sk_maskfilter_unref");
    RX_SK_OPTIONAL(imagefilter_new_blur,      "sk_imagefilter_new_blur");
    RX_SK_OPTIONAL(imagefilter_unref,         "sk_imagefilter_unref");
    RX_SK_OPTIONAL(paint_set_imagefilter,     "sk_paint_set_imagefilter");
    RX_SK_OPTIONAL(canvas_save,               "sk_canvas_save");
    RX_SK_OPTIONAL(canvas_restore,            "sk_canvas_restore");
    RX_SK_OPTIONAL(canvas_save_layer,         "sk_canvas_save_layer");
    RX_SK_OPTIONAL(canvas_get_save_count,     "sk_canvas_get_save_count");
    RX_SK_OPTIONAL(canvas_restore_to_count,   "sk_canvas_restore_to_count");
    RX_SK_OPTIONAL(canvas_translate,          "sk_canvas_translate");
    RX_SK_OPTIONAL(canvas_scale,              "sk_canvas_scale");
    RX_SK_OPTIONAL(canvas_rotate_degrees,     "sk_canvas_rotate_degrees");
    RX_SK_OPTIONAL(canvas_reset_matrix,       "sk_canvas_reset_matrix");
    RX_SK_OPTIONAL(canvas_skew,               "sk_canvas_skew");
    RX_SK_OPTIONAL(canvas_concat,             "sk_canvas_concat");
    RX_SK_OPTIONAL(canvas_clip_rect,          "sk_canvas_clip_rect_with_operation");
    RX_SK_OPTIONAL(canvas_clip_rrect,         "sk_canvas_clip_rrect_with_operation");
    RX_SK_OPTIONAL(data_new_from_file,        "sk_data_new_from_file");
    RX_SK_OPTIONAL(data_unref,                "sk_data_unref");
    RX_SK_OPTIONAL(image_new_from_encoded,    "sk_image_new_from_encoded");
    RX_SK_OPTIONAL(image_get_width,           "sk_image_get_width");
    RX_SK_OPTIONAL(image_get_height,          "sk_image_get_height");
    RX_SK_OPTIONAL(image_unref,               "sk_image_unref");
    RX_SK_OPTIONAL(canvas_draw_image_rect,    "sk_canvas_draw_image_rect");
    RX_SK_OPTIONAL(surface_new_image_snapshot, "sk_surface_new_image_snapshot");
    RX_SK_OPTIONAL(typeface_create_default,   "sk_typeface_create_default");
    RX_SK_OPTIONAL(typeface_create_from_name, "sk_typeface_create_from_name");
    RX_SK_OPTIONAL(typeface_unref,            "sk_typeface_unref");
    RX_SK_OPTIONAL(fontstyle_new,             "sk_fontstyle_new");
    RX_SK_OPTIONAL(fontstyle_delete,          "sk_fontstyle_delete");
    RX_SK_OPTIONAL(font_new_with_values,      "sk_font_new_with_values");
    RX_SK_OPTIONAL(font_set_size,             "sk_font_set_size");
    RX_SK_OPTIONAL(font_delete,               "sk_font_delete");
    RX_SK_OPTIONAL(font_measure_text,         "sk_font_measure_text");
    RX_SK_OPTIONAL(font_get_metrics,          "sk_font_get_metrics");
    RX_SK_OPTIONAL(path_new,                  "sk_path_new");
    RX_SK_OPTIONAL(path_delete,               "sk_path_delete");
    RX_SK_OPTIONAL(path_move_to,              "sk_path_move_to");
    RX_SK_OPTIONAL(path_line_to,              "sk_path_line_to");
    RX_SK_OPTIONAL(path_quad_to,              "sk_path_quad_to");
    RX_SK_OPTIONAL(path_cubic_to,             "sk_path_cubic_to");
    RX_SK_OPTIONAL(path_arc_to,               "sk_path_arc_to");
    RX_SK_OPTIONAL(path_close,                "sk_path_close");
    RX_SK_OPTIONAL(path_set_filltype,         "sk_path_set_filltype");
    RX_SK_OPTIONAL(canvas_draw_path,          "sk_canvas_draw_path");

    /* Path effects (dashing). The symbols ARE present in the pinned 3.119.4
     * libSkiaSharp — Phase-1 searched `sk_patheffect_*` (wrong) and missed the
     * real `sk_path_effect_*` names (docs/ROADMAP.md Phase-1.5). OPTIONAL: a miss
     * makes ruxen_canvas_draw_dashed_line return RXC_ERR_NO_SKIA (honest probe). */
    RX_SK_OPTIONAL(path_effect_create_dash,   "sk_path_effect_create_dash");
    RX_SK_OPTIONAL(path_effect_unref,         "sk_path_effect_unref");
    RX_SK_OPTIONAL(paint_set_path_effect,     "sk_paint_set_path_effect");

    /* Ganesh GL backend (docs/GPU.md). OPTIONAL: a miss leaves gpu_gl_ok = 0,
     * disabling only the GPU rung — the raster backend is untouched. */
    RX_SK_OPTIONAL(gr_glinterface_assemble_gl,    "gr_glinterface_assemble_gl_interface");
    RX_SK_OPTIONAL(gr_glinterface_create_native,  "gr_glinterface_create_native_interface");
    RX_SK_OPTIONAL(gr_glinterface_unref,          "gr_glinterface_unref");
    RX_SK_OPTIONAL(gr_direct_context_make_gl,     "gr_direct_context_make_gl");
    /* A GrDirectContext is-a GrRecordingContext; this build exports only the
     * recording-context unref (there is no gr_direct_context_unref). */
    RX_SK_OPTIONAL(gr_recording_context_unref,    "gr_recording_context_unref");
    RX_SK_OPTIONAL(gr_direct_context_flush,       "gr_direct_context_flush");
    RX_SK_OPTIONAL(gr_direct_context_flush_and_submit, "gr_direct_context_flush_and_submit");
    RX_SK_OPTIONAL(gr_backendrendertarget_new_gl, "gr_backendrendertarget_new_gl");
    RX_SK_OPTIONAL(gr_backendrendertarget_delete, "gr_backendrendertarget_delete");
    RX_SK_OPTIONAL(surface_new_backend_render_target, "sk_surface_new_backend_render_target");

    /* Ganesh Metal backend + offscreen GPU surface + readback (docs/GPU.md).
     * OPTIONAL: a miss leaves gpu_metal_ok = 0, disabling only the Metal rung. */
    RX_SK_OPTIONAL(gr_direct_context_make_metal,  "gr_direct_context_make_metal");
    RX_SK_OPTIONAL(surface_new_render_target,     "sk_surface_new_render_target");
    RX_SK_OPTIONAL(gr_backendrendertarget_new_metal, "gr_backendrendertarget_new_metal");
    RX_SK_OPTIONAL(surface_read_pixels,           "sk_surface_read_pixels");

    /* Positioned-glyph rendering for shaped text (docs/SHAPING.md). OPTIONAL:
     * a miss leaves glyph_render_ok = 0, disabling only shaping. */
    RX_SK_OPTIONAL(typeface_create_from_file,     "sk_typeface_create_from_file");
    RX_SK_OPTIONAL(textblob_builder_new,          "sk_textblob_builder_new");
    RX_SK_OPTIONAL(textblob_builder_delete,       "sk_textblob_builder_delete");
    RX_SK_OPTIONAL(textblob_builder_alloc_run_pos,"sk_textblob_builder_alloc_run_pos");
    RX_SK_OPTIONAL(textblob_builder_make,         "sk_textblob_builder_make");
    RX_SK_OPTIONAL(textblob_unref,                "sk_textblob_unref");
    RX_SK_OPTIONAL(canvas_draw_text_blob,         "sk_canvas_draw_text_blob");
#undef RX_SK_REQUIRED
#undef RX_SK_OPTIONAL

    /* The GPU-GL rung needs: a way to build the GL interface (assemble OR
     * native), make a direct context, wrap the FBO, and create the surface.
     * flush/unref are needed for correct teardown. If any is absent, the GPU
     * path is unavailable and rx_host_canvas falls back to raster. */
    skia.gpu_gl_ok =
        (skia.gr_glinterface_assemble_gl || skia.gr_glinterface_create_native) &&
        skia.gr_direct_context_make_gl &&
        skia.gr_recording_context_unref &&
        skia.gr_backendrendertarget_new_gl &&
        skia.gr_backendrendertarget_delete &&
        skia.surface_new_backend_render_target &&
        skia.surface_unref ? 1 : 0;

    /* The GPU-Metal rung needs: make a direct context from a device+queue,
     * create the offscreen render-target surface, read it back, unref the
     * context + surface. (The device/queue themselves come from Metal.framework
     * via rx_metal(), resolved separately below.) */
    skia.gpu_metal_ok =
        skia.gr_direct_context_make_metal &&
        skia.surface_new_render_target &&
        skia.surface_read_pixels &&
        skia.gr_recording_context_unref &&
        skia.surface_unref ? 1 : 0;

    /* On-screen Metal additionally needs: wrap a drawable's texture as a backend
     * render target and create a surface over it. */
    skia.gpu_metal_window_ok =
        skia.gr_direct_context_make_metal &&
        skia.gr_backendrendertarget_new_metal &&
        skia.gr_backendrendertarget_delete &&
        skia.surface_new_backend_render_target &&
        skia.gr_recording_context_unref &&
        skia.surface_unref ? 1 : 0;

    /* Shaped-glyph rendering needs: load the font file as a typeface, build a
     * positioned-glyph run, and draw the blob. (HarfBuzz does the shaping; see
     * rx_hb().) If any is absent, shaping is unavailable and the non-shaped text
     * path stays the fallback. */
    skia.glyph_render_ok =
        skia.typeface_create_from_file &&
        skia.font_new_with_values &&
        skia.textblob_builder_new &&
        skia.textblob_builder_alloc_run_pos &&
        skia.textblob_builder_make &&
        skia.canvas_draw_text_blob &&
        skia.textblob_unref ? 1 : 0;

    /* Keep the handle open for the process lifetime (no dlclose): the resolved
     * pointers must stay valid. */
    skia.available = ok;
    return &skia;
}

/* ---- HarfBuzz loader (text shaping, docs/SHAPING.md) ----
 *
 * libHarfBuzzSharp is fetched by runtime/fetch_skia.sh and dlopen()'d here —
 * never linked, exactly like Skia/SDL. rx_hb() resolves the hb_* table on first
 * call; if the library or any required symbol is missing, ->available stays 0
 * and the shaped-text ops report a clean Err (the non-shaped path is unaffected).
 * Process-wide singleton. */
static void *rx_hb_dlopen(void) {
    const char *env = getenv("RUXEN_CANVAS_HARFBUZZ");
    if (env && env[0]) {
        void *h = dlopen(env, RTLD_NOW | RTLD_LOCAL);
        if (h) return h;
    }
    char path[4096];
    const char *cache = getenv("RUXEN_CANVAS_CACHE");
    if (cache && cache[0]) {
        snprintf(path, sizeof(path), "%s/libHarfBuzzSharp.dylib", cache);
        void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        if (h) return h;
    }
    const char *home = getenv("HOME");
    if (home && home[0]) {
        snprintf(path, sizeof(path), "%s/.cache/ruxen-canvas/libHarfBuzzSharp.dylib", home);
        void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        if (h) return h;
    }
    void *h = dlopen("libHarfBuzzSharp.dylib", RTLD_NOW | RTLD_LOCAL);
    if (h) return h;
    return dlopen("libHarfBuzzSharp.so", RTLD_NOW | RTLD_LOCAL);
}

const RxHB *rx_hb(void) {
    static RxHB hb;
    static int initialized = 0;
    if (initialized) return &hb;
    initialized = 1;

    void *lib = rx_hb_dlopen();
    if (!lib) return &hb;   /* not available; shaping reports Err */

    int ok = 1;
#define RX_HB_REQ(field, sym)                                  \
    do {                                                       \
        *(void **)(&hb.field) = dlsym(lib, sym);               \
        if (!hb.field) ok = 0;                                 \
    } while (0)
    RX_HB_REQ(blob_create_from_file,             "hb_blob_create_from_file_or_fail");
    RX_HB_REQ(face_create,                       "hb_face_create");
    RX_HB_REQ(font_create,                       "hb_font_create");
    RX_HB_REQ(font_set_scale,                    "hb_font_set_scale");
    RX_HB_REQ(buffer_create,                     "hb_buffer_create");
    RX_HB_REQ(buffer_add_utf8,                   "hb_buffer_add_utf8");
    RX_HB_REQ(buffer_set_direction,              "hb_buffer_set_direction");
    RX_HB_REQ(buffer_guess_segment_properties,   "hb_buffer_guess_segment_properties");
    RX_HB_REQ(shape,                             "hb_shape");
    RX_HB_REQ(buffer_get_length,                 "hb_buffer_get_length");
    RX_HB_REQ(buffer_get_glyph_infos,            "hb_buffer_get_glyph_infos");
    RX_HB_REQ(buffer_get_glyph_positions,        "hb_buffer_get_glyph_positions");
    RX_HB_REQ(buffer_destroy,                    "hb_buffer_destroy");
    RX_HB_REQ(font_destroy,                      "hb_font_destroy");
    RX_HB_REQ(face_destroy,                      "hb_face_destroy");
    RX_HB_REQ(blob_destroy,                      "hb_blob_destroy");
#undef RX_HB_REQ

    hb.available = ok;
    return &hb;
}

/* ---- Metal device + command queue (Apple GPU, docs/GPU.md) ----
 *
 * The GrDirectContext for Metal needs an id<MTLDevice> + id<MTLCommandQueue>.
 * These come from Metal.framework + the Objective-C runtime, reached by dlopen
 * (no link-time dependency — same discipline as Skia/SDL). The device + queue
 * are a PROCESS-WIDE singleton: there is one GPU, and a device outlives any one
 * surface (the same never-freed-singleton model as the default font). They are
 * created on first use and cached; a host's GrDirectContext + surface are the
 * per-host objects torn down in host_drop, NOT the device/queue.
 *
 * MTLCreateSystemDefaultDevice() returns the system GPU WITHOUT any window or
 * display — this is what makes offscreen, headless Metal rendering possible. */
typedef struct {
    int   tried;
    int   available;   /* 1 iff device + queue were created */
    void *device;      /* id<MTLDevice> */
    void *queue;       /* id<MTLCommandQueue> */
} RxMetal;

static const RxMetal *rx_metal(void) {
    static RxMetal m;
    if (m.tried) return &m;
    m.tried = 1;

    /* Metal.framework: MTLCreateSystemDefaultDevice is a plain C function (it
     * does not appear in nm's exported list but dlsym resolves it). */
    void *mtl = dlopen("/System/Library/Frameworks/Metal.framework/Metal",
                       RTLD_NOW | RTLD_GLOBAL);
    if (!mtl) return &m;
    void *(*create_device)(void) = (void *(*)(void))dlsym(mtl, "MTLCreateSystemDefaultDevice");
    if (!create_device) return &m;

    /* Objective-C runtime: we need one no-arg message send ([device
     * newCommandQueue]). On arm64/x86_64 a simple id-returning, no-struct
     * objc_msgSend is called through a plain function-pointer cast. */
    void *objc = dlopen("/usr/lib/libobjc.A.dylib", RTLD_NOW | RTLD_GLOBAL);
    if (!objc) objc = dlopen("libobjc.A.dylib", RTLD_NOW | RTLD_GLOBAL);
    if (!objc) return &m;
    void *(*msg_send)(void *, void *) = (void *(*)(void *, void *))dlsym(objc, "objc_msgSend");
    void *(*sel_reg)(const char *)    = (void *(*)(const char *))dlsym(objc, "sel_registerName");
    if (!msg_send || !sel_reg) return &m;

    void *device = create_device();
    if (!device) return &m;                 /* no GPU available */
    void *queue = msg_send(device, sel_reg("newCommandQueue"));
    if (!queue) return &m;

    m.device    = device;
    m.queue     = queue;
    m.available = 1;
    return &m;
}

/* Lazily create (once per host) a raster-direct Skia surface that wraps the
 * host's own 0xAARRGGBB framebuffer, and return its canvas — or NULL when Skia
 * is unavailable / creation failed. The surface is cached on the host and torn
 * down in host_drop before the pixels are freed. */
static sk_canvas_t *rx_host_gpu_canvas(RxHost *h);   /* fwd: GPU rung below */

static sk_canvas_t *rx_host_canvas(RxHost *h) {
    if (!h) return NULL;
    /* GPU rung (top of the ladder, docs/GPU.md): a host put into GPU mode draws
     * into its GPU-backed surface. Every drawing wrapper funnels through here,
     * so routing all draws to the GPU canvas is exactly this one line — the
     * ruxen_canvas_* ABI above is unchanged. If GPU mode is requested but the
     * surface could not be built, we have already torn GPU mode down in
     * enable_gpu, so gpu_requested is 0 here and we fall through to raster. */
    if (h->is_gpu) {
        if (h->gpu_surface) return (sk_canvas_t *)h->sk_canvas;
        return NULL;   /* GPU active flag but no surface: refuse (no wrong CPU draw) */
    }
    if (h->gpu_requested) {
        sk_canvas_t *g = rx_host_gpu_canvas(h);
        if (g) return g;
        /* GPU build failed mid-flight: fall through to raster below. */
    }
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


/* ---- GPU (Ganesh GL) backend (docs/GPU.md) ----
 *
 * The GPU rung of the backend-selection ladder. It is attempted ONLY when the
 * host was put into GPU mode (gpu_requested, set by ruxen_canvas_host_enable_gpu
 * after a GL window + context exist). On ANY failure we leave the GPU fields
 * NULL and the caller falls back to the raster path — a GPU op that can't run
 * must fall back cleanly, never produce wrong pixels (the non-negotiable
 * invariant).
 *
 * These prototypes are the GL-context seam in runtime/sdl_window.c (both TUs
 * always compile together). The GL context must be current before any gr_*
 * call; create_gl makes it current and it stays so for this single-thread,
 * single-window host. */
int64_t ruxen_canvas_window_create_gl(int64_t self, int64_t win_scale);
int64_t ruxen_canvas_window_gl_get_proc(int64_t name);
int64_t ruxen_canvas_window_gl_drawable_size(int64_t self);
int64_t ruxen_canvas_window_gl_present(int64_t self);
/* on-screen Metal seam (runtime/sdl_window.c) */
int64_t ruxen_canvas_window_create_metal(int64_t self, int64_t device, int64_t queue, int64_t win_scale);
int64_t ruxen_canvas_window_metal_next_drawable(int64_t self);
int64_t ruxen_canvas_window_metal_drawable_size(int64_t self);
int64_t ruxen_canvas_window_metal_present(int64_t self);

/* The proc-loader trampoline Skia calls to resolve each GL entry point. It
 * forwards to SDL_GL_GetProcAddress via the shim's window seam. ctx is unused
 * (the current GL context is implicit in SDL). */
static void *rx_gl_get_proc(void *ctx, const char *name) {
    (void)ctx;
    return (void *)(intptr_t)ruxen_canvas_window_gl_get_proc((int64_t)name);
}

/* Build the GrGLInterface for the current GL context: prefer the explicit
 * proc-loader assembly, fall back to the native (current-context) interface.
 * Returns an owned interface (gr_glinterface_unref) or NULL. */
static gr_glinterface_t *rx_make_gl_interface(const RxSkia *sk) {
    gr_glinterface_t *gi = NULL;
    if (sk->gr_glinterface_assemble_gl) {
        gi = sk->gr_glinterface_assemble_gl(NULL, rx_gl_get_proc);
    }
    if (!gi && sk->gr_glinterface_create_native) {
        gi = sk->gr_glinterface_create_native();
    }
    return gi;
}

/* Lazily create (once per host) the GPU-backed Skia surface over this host's GL
 * window, and return its canvas — or NULL on any failure (caller falls back to
 * raster). Caches gr_context / gr_target / gpu_surface / gl_interface and sets
 * is_gpu on success. Only ever called for a gpu_requested host. */
static sk_canvas_t *rx_host_gpu_canvas(RxHost *h) {
    if (!h || !h->gpu_requested) return NULL;
    if (h->gpu_surface) return (sk_canvas_t *)h->sk_canvas;
    if (h->gpu_tried) return NULL;
    h->gpu_tried = 1;

    const RxSkia *sk = rx_skia();
    if (!sk->available || !sk->gpu_gl_ok) return NULL;

    /* The GL context must already be current (create_gl did that). Drawable
     * size is the HiDPI-correct backbuffer size; 0 means no GL window. */
    int64_t packed = ruxen_canvas_window_gl_drawable_size((int64_t)h);
    if (packed == 0) return NULL;
    int gw = (int)(uint32_t)(packed >> 32);
    int gh = (int)(uint32_t)(packed & 0xFFFFFFFF);
    if (gw <= 0 || gh <= 0) return NULL;

    gr_glinterface_t *gi = rx_make_gl_interface(sk);
    if (!gi) return NULL;

    gr_direct_context_t *ctx = sk->gr_direct_context_make_gl(gi);
    if (!ctx) {
        if (sk->gr_glinterface_unref) sk->gr_glinterface_unref(gi);
        return NULL;
    }

    /* Wrap the window's default framebuffer (FBO 0 on desktop GL, RGBA8). The
     * struct may have a trailing field in some SkiaSharp builds; we declare the
     * two ABI-stable leading fields and zero the rest implicitly. */
    gr_gl_framebufferinfo_t fbo;
    fbo.fFBOID  = 0;                 /* window default framebuffer */
    fbo.fFormat = RX_GR_GL_RGBA8;
    gr_backendrendertarget_t *rt =
        sk->gr_backendrendertarget_new_gl(gw, gh, /*samples*/0, /*stencils*/8, &fbo);
    if (!rt) {
        sk->gr_recording_context_unref((gr_recording_context_t *)ctx);
        if (sk->gr_glinterface_unref) sk->gr_glinterface_unref(gi);
        return NULL;
    }

    sk_surface_t *surf = sk->surface_new_backend_render_target(
        (gr_recording_context_t *)ctx, rt,
        RX_GR_SURFACE_ORIGIN_BOTTOM_LEFT, RX_SK_COLORTYPE_RGBA_8888, NULL, NULL);
    if (!surf) {
        sk->gr_backendrendertarget_delete(rt);
        sk->gr_recording_context_unref((gr_recording_context_t *)ctx);
        if (sk->gr_glinterface_unref) sk->gr_glinterface_unref(gi);
        return NULL;
    }
    sk_canvas_t *canvas = sk->surface_get_canvas(surf);
    if (!canvas) {
        sk->surface_unref(surf);
        sk->gr_backendrendertarget_delete(rt);
        sk->gr_recording_context_unref((gr_recording_context_t *)ctx);
        if (sk->gr_glinterface_unref) sk->gr_glinterface_unref(gi);
        return NULL;
    }

    h->gr_context       = ctx;
    h->gr_target        = rt;
    h->gpu_surface      = surf;
    h->gl_interface     = gi;
    h->sk_surface       = NULL;   /* GPU host has no raster-direct surface */
    h->sk_canvas        = canvas;
    h->is_gpu           = 1;
    h->gpu_backend_kind = RX_GPU_KIND_GL;
    return canvas;
}

static void rx_host_gpu_teardown(RxHost *h);   /* fwd */

/* Invalidate a windowed GL host's persistent GPU surface after a window resize,
 * so the NEXT begin_frame rebuilds it at the new backing (drawable) size. The GL
 * default framebuffer was resized by the driver; we drop the now-wrongly-sized
 * GrBackendRenderTarget + surface (+ context/interface) and clear the gpu_tried
 * guard, leaving the host gpu_requested so rx_host_canvas rebuilds. Metal does
 * not need this (its surface is per-frame). Called from the SDL resize handler. */
void ruxen_canvas_host_gl_invalidate_surface(int64_t self) {
    RxHost *h = (RxHost *)self;
    if (!h) return;
    if (!h->is_gpu || h->gpu_windowed || h->gpu_offscreen) return;  /* GL windowed only */
    if (h->gpu_backend_kind != RX_GPU_KIND_GL) return;
    rx_host_gpu_teardown(h);     /* drops surface->target->context->interface;
                                  * sets is_gpu=0, gpu_backend_kind=NONE. */
    /* gpu_requested stays 1, so rx_host_canvas's gpu_requested branch calls
     * rx_host_gpu_canvas to REBUILD the surface at the new drawable size on the
     * next frame. Clearing gpu_tried re-arms that builder. */
    h->gpu_tried = 0;
}

/* ---- Metal offscreen GPU surface (the headless pixel-verified path) ----
 *
 * Create (once per host) a GrDirectContext over the process-wide Metal device +
 * queue, and an OFFSCREEN BGRA_8888 GPU surface sized to the host. Skia
 * allocates the backing MTLTexture — no window, no CAMetalLayer, no display.
 * Returns the surface's canvas, or NULL on any failure (caller falls back to
 * raster — a Metal op that can't run must fall back cleanly, never wrong pixels).
 *
 * BGRA_8888 is deliberate: end_frame reads this surface back into h->pixels with
 * a BGRA dst info, so the bytes land in the host's 0xAARRGGBB order and the
 * existing read_pixel oracle observes the real GPU output byte-identically.
 *
 * The GrDirectContext is cached in gr_context and unref'd in host_drop. The
 * device + queue are NOT per-host (rx_metal singleton). */
static sk_canvas_t *rx_host_metal_offscreen_canvas(RxHost *h) {
    if (!h || !h->gpu_requested) return NULL;
    if (h->gpu_surface) return (sk_canvas_t *)h->sk_canvas;
    if (h->gpu_tried) return NULL;
    h->gpu_tried = 1;

    const RxSkia *sk = rx_skia();
    if (!sk->available || !sk->gpu_metal_ok) return NULL;
    const RxMetal *mtl = rx_metal();
    if (!mtl->available) return NULL;

    gr_direct_context_t *ctx = sk->gr_direct_context_make_metal(mtl->device, mtl->queue);
    if (!ctx) return NULL;

    sk_imageinfo_t info;
    info.colorspace = NULL;
    info.width      = h->width;
    info.height     = h->height;
    info.colorType  = RX_SK_COLORTYPE_BGRA_8888;   /* readback matches 0xAARRGGBB */
    info.alphaType  = RX_SK_ALPHA_PREMUL;

    sk_surface_t *surf = sk->surface_new_render_target(
        (gr_recording_context_t *)ctx, /*budgeted*/1, &info, /*samples*/0,
        RX_GR_SURFACE_ORIGIN_TOP_LEFT, NULL, /*mips*/0);
    if (!surf) {
        sk->gr_recording_context_unref((gr_recording_context_t *)ctx);
        return NULL;
    }
    sk_canvas_t *canvas = sk->surface_get_canvas(surf);
    if (!canvas) {
        sk->surface_unref(surf);
        sk->gr_recording_context_unref((gr_recording_context_t *)ctx);
        return NULL;
    }

    h->gr_context       = ctx;
    h->gr_target        = NULL;   /* offscreen: Skia owns the texture */
    h->gpu_surface      = surf;
    h->gl_interface     = NULL;
    h->sk_surface       = NULL;
    h->sk_canvas        = canvas;
    h->is_gpu           = 1;
    h->gpu_offscreen    = 1;
    h->gpu_backend_kind = RX_GPU_KIND_METAL;
    return canvas;
}

/* ---- on-screen Metal: per-frame drawable surface (docs/GPU.md) ----
 *
 * Unlike the offscreen path (one persistent surface), an on-screen Metal frame
 * renders into the CAMetalLayer's NEXT drawable, which is fresh each frame and
 * consumed by present. So the GrDirectContext is persistent (built once at
 * enable, stored in gr_context), but the SkSurface + GrBackendRenderTarget are
 * PER-FRAME: begin_frame acquires the drawable and builds them; end_frame
 * flushes; present presents the drawable; then the per-frame surface + target
 * are released. This matches Skia's documented Metal swapchain flow. */

/* Build (or return) the persistent GrDirectContext for a windowed-Metal host. */
static gr_direct_context_t *rx_metal_window_context(RxHost *h) {
    if (h->gr_context) return (gr_direct_context_t *)h->gr_context;
    const RxSkia *sk = rx_skia();
    const RxMetal *mtl = rx_metal();
    if (!sk->available || !sk->gpu_metal_window_ok || !mtl->available) return NULL;
    gr_direct_context_t *ctx = sk->gr_direct_context_make_metal(mtl->device, mtl->queue);
    if (!ctx) return NULL;
    h->gr_context = ctx;
    return ctx;
}

/* Acquire this frame's drawable and build a GPU surface over its texture; set it
 * as the host's live canvas. Returns the canvas, or NULL when no drawable is
 * available (headless / no display — the frame then draws nothing and present is
 * a clean no-op, never wrong pixels). Per-frame: gr_target + gpu_surface are the
 * frame's, released in rx_metal_window_end_frame. */
static void rx_metal_window_end_frame(RxHost *h);   /* fwd */

static sk_canvas_t *rx_metal_window_begin_frame(RxHost *h) {
    const RxSkia *sk = rx_skia();
    /* Release the PRIOR frame's drawable surface + target before acquiring the
     * next (present consumed the drawable; the wrapping surface/RT are stale).
     * Doing it here, not in shim end_frame, keeps the drawable alive through the
     * Window#present that runs between end_frame and the next begin_frame. */
    rx_metal_window_end_frame(h);

    gr_direct_context_t *ctx = rx_metal_window_context(h);
    if (!ctx) return NULL;

    /* acquire the layer's next drawable -> its MTLTexture (0/NULL headless). */
    int64_t tex = ruxen_canvas_window_metal_next_drawable((int64_t)h);
    if (!tex) return NULL;

    int64_t packed = ruxen_canvas_window_metal_drawable_size((int64_t)h);
    int dw = packed ? (int)(uint32_t)(packed >> 32) : h->width;
    int dh = packed ? (int)(uint32_t)(packed & 0xFFFFFFFF) : h->height;
    if (dw <= 0 || dh <= 0) return NULL;

    gr_mtl_textureinfo_t mtlinfo;
    mtlinfo.fTexture = (const void *)(intptr_t)tex;
    gr_backendrendertarget_t *rt = sk->gr_backendrendertarget_new_metal(dw, dh, &mtlinfo);
    if (!rt) return NULL;

    /* The drawable's texture is BGRA8 (we set the layer's pixelFormat), top-left
     * origin for a Metal layer. */
    sk_surface_t *surf = sk->surface_new_backend_render_target(
        (gr_recording_context_t *)ctx, rt,
        RX_GR_SURFACE_ORIGIN_TOP_LEFT, RX_SK_COLORTYPE_BGRA_8888, NULL, NULL);
    if (!surf) {
        sk->gr_backendrendertarget_delete(rt);
        return NULL;
    }
    sk_canvas_t *canvas = sk->surface_get_canvas(surf);
    if (!canvas) {
        sk->surface_unref(surf);
        sk->gr_backendrendertarget_delete(rt);
        return NULL;
    }
    h->gr_target   = rt;
    h->gpu_surface = surf;
    h->sk_canvas   = canvas;
    return canvas;
}

/* Release this frame's drawable surface + render target (after flush/present).
 * The GrDirectContext (gr_context) persists across frames. */
static void rx_metal_window_end_frame(RxHost *h) {
    const RxSkia *sk = rx_skia();
    if (h->gpu_surface) {
        if (sk->surface_unref) sk->surface_unref((sk_surface_t *)h->gpu_surface);
        h->gpu_surface = NULL;
    }
    if (h->gr_target) {
        if (sk->gr_backendrendertarget_delete)
            sk->gr_backendrendertarget_delete((gr_backendrendertarget_t *)h->gr_target);
        h->gr_target = NULL;
    }
    h->sk_canvas = NULL;
}

/* Release the GPU objects in the correct order (surface -> target -> context ->
 * interface), BEFORE the GL context/window are destroyed by sdl_window.c. Safe
 * to call on a non-GPU host (all NULL). */
static void rx_host_gpu_teardown(RxHost *h) {
    if (!h) return;
    const RxSkia *sk = rx_skia();
    if (h->gpu_surface) {
        if (sk->surface_unref) sk->surface_unref((sk_surface_t *)h->gpu_surface);
        h->gpu_surface = NULL;
    }
    if (h->gr_target) {
        if (sk->gr_backendrendertarget_delete)
            sk->gr_backendrendertarget_delete((gr_backendrendertarget_t *)h->gr_target);
        h->gr_target = NULL;
    }
    if (h->gr_context) {
        if (sk->gr_recording_context_unref)
            sk->gr_recording_context_unref((gr_recording_context_t *)h->gr_context);
        h->gr_context = NULL;
    }
    if (h->gl_interface) {
        if (sk->gr_glinterface_unref)
            sk->gr_glinterface_unref((gr_glinterface_t *)h->gl_interface);
        h->gl_interface = NULL;
    }
    /* The Metal device + command queue are a process-wide singleton (rx_metal),
     * not per-host — a device outlives any one surface, so they are NOT released
     * here (same lifetime as the default font). Only the per-host GrDirectContext
     * + surface above are torn down. */
    if (h->is_gpu) { h->sk_canvas = NULL; h->is_gpu = 0; }
    h->gpu_backend_kind = RX_GPU_KIND_NONE;
    h->gpu_offscreen = 0;
    h->gpu_windowed = 0;
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

/* Identity accessor: a RawHost's handle IS its pointer. Lets the Ruxen side pass
 * a host as a plain Int to a binding that takes the host by value (e.g.
 * RawImage.snapshot_of, whose RawImage return type only resolves in its own file).
 * Mirrors ruxen_canvas_image_ptr. */
int64_t ruxen_canvas_host_ptr(int64_t self) { return self; }

/* Tear the host down. Called from the Ruxen side's drop — deterministic,
 * no GC. */
/* defined in runtime/sdl_window.c — tears the OS window down when its
 * owning host is dropped (both files always compile together) */
void ruxen_canvas_window_note_host_dropped(int64_t self);

void ruxen_canvas_host_drop(int64_t self) {
    RxHost *h = (RxHost *)self;
    if (!h) return;
    /* GPU objects reference the GL context, so they MUST be released before the
     * GL context/window are torn down. Order: GPU surface stack first, then the
     * window (note_host_dropped deletes the GL context + window), then the
     * raster surface + pixels. */
    rx_host_gpu_teardown(h);
    ruxen_canvas_window_note_host_dropped(self);
    /* The Skia surface wraps h->pixels — unref it before freeing the buffer.
     * (sk_canvas is owned by the surface; no separate release.) */
    if (h->sk_surface) {
        const RxSkia *sk = rx_skia();
        if (sk->available) sk->surface_unref((sk_surface_t *)h->sk_surface);
        h->sk_surface = NULL;
        h->sk_canvas  = NULL;
    }
    /* Free any owned dropped-file paths still held: the most-recently-polled one
     * in `pending`, plus every UNPOLLED ring slot (a host dropped with file-drop
     * events still queued). Without this a never-polled drop would leak its path. */
    if (h->pending.drop_path) { free(h->pending.drop_path); h->pending.drop_path = NULL; }
    for (int32_t i = 0; i < RXC_EVENT_CAP; i++) {
        if (h->events[i].drop_path) { free(h->events[i].drop_path); h->events[i].drop_path = NULL; }
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

/* True (1) when SOME GPU backend (Ganesh GL or Metal) COULD run in this process:
 * Skia is loaded AND the required gr_* symbols for at least one backend resolved.
 * A capability probe: it does NOT prove a context or surface exists for any host
 * (use ruxen_canvas_gpu_active for that). Mirrors skia_available. */
int64_t ruxen_canvas_gpu_available(int64_t self) {
    (void)self;
    const RxSkia *sk = rx_skia();
    if (!sk->available) return 0;
    return (sk->gpu_gl_ok || sk->gpu_metal_ok) ? 1 : 0;
}

/* True (1) when the Metal GPU backend specifically could run: the Metal Skia
 * symbols resolved AND a Metal device + queue could be created (rx_metal). This
 * is what gates the headless offscreen render+readback path. */
int64_t ruxen_canvas_gpu_metal_available(int64_t self) {
    (void)self;
    const RxSkia *sk = rx_skia();
    if (!sk->available || !sk->gpu_metal_ok) return 0;
    return rx_metal()->available ? 1 : 0;
}

/* Put this host into GPU mode: create a GL window + current context for it,
 * then build the GPU-backed Skia surface. Returns RXC_OK when the GPU surface
 * is live (subsequent draws target it and present is a GL swap), or a clean
 * RXC_ERR_* on ANY failure — in which case the host is left in its prior
 * (raster/software) state, NOT half-GPU. The caller (Window) falls back to the
 * raster show path on error. Idempotent once GPU is active. */
int64_t ruxen_canvas_host_enable_gpu(int64_t self, int64_t win_scale) {
    RxHost *h = (RxHost *)self;
    if (!h) return RXC_ERR_BAD_ARGS;
    if (h->is_gpu) return RXC_OK;                 /* already on GPU */
    const RxSkia *sk = rx_skia();
    if (!sk->available || !sk->gpu_gl_ok) return RXC_ERR_NO_SKIA;
    /* A GPU host cannot have already created a raster-direct surface for this
     * frame's pixels — that would mean draws already went to the CPU buffer.
     * Require GPU to be enabled before any frame/draw. */
    if (h->sk_surface || h->sk_canvas) return RXC_ERR_IN_FRAME;

    /* Create the GL window (win_scale × design for on-screen size) + make its
     * context current. Bounded + clean on a headless host (Err, never blocks). */
    int64_t wc = ruxen_canvas_window_create_gl(self, win_scale);
    if (wc != RXC_OK) return wc;

    h->gpu_requested = 1;
    h->gpu_tried     = 0;     /* allow the build attempt now */
    sk_canvas_t *canvas = rx_host_gpu_canvas(h);
    if (!canvas) {
        /* GPU surface creation failed AFTER the GL window came up: tear the GL
         * window back down so the host can fall back to the raster window path
         * cleanly (no orphaned GL window). */
        h->gpu_requested = 0;
        ruxen_canvas_window_note_host_dropped(self);
        return RXC_ERR_NO_SKIA;
    }
    /* DESIGN size = the logical framebuffer size; begin_frame scales design->
     * backing so logical-coord apps fill the native GL drawable at native
     * density (crisp). Same as the Metal windowed path. */
    h->design_w = h->width;
    h->design_h = h->height;
    return RXC_OK;
}

/* Put this host into OFFSCREEN Metal GPU mode (the headless, pixel-verified
 * path — docs/GPU.md): build a GrDirectContext over the system Metal device and
 * an offscreen BGRA GPU surface sized to the host. No window / display needed.
 * Subsequent draws target the GPU surface; end_frame flushes + reads the pixels
 * back into the host framebuffer so read_pixel observes real GPU output.
 *
 * Returns RXC_OK when the Metal surface is live, or a clean RXC_ERR_* on ANY
 * failure — in which case the host is left in its prior (raster/software) state,
 * NOT half-GPU (a Metal op that can't run falls back cleanly). Idempotent once
 * a GPU surface is active. Unlike enable_gpu, this needs NO SDL/window. */
int64_t ruxen_canvas_host_enable_gpu_offscreen(int64_t self) {
    RxHost *h = (RxHost *)self;
    if (!h) return RXC_ERR_BAD_ARGS;
    if (h->is_gpu) return RXC_OK;                 /* already on GPU */
    const RxSkia *sk = rx_skia();
    if (!sk->available || !sk->gpu_metal_ok) return RXC_ERR_NO_SKIA;
    if (!rx_metal()->available) return RXC_ERR_NO_SKIA;   /* no GPU device */
    /* Must be enabled before any frame/draw created a raster-direct surface. */
    if (h->sk_surface || h->sk_canvas) return RXC_ERR_IN_FRAME;

    h->gpu_requested = 1;
    h->gpu_tried     = 0;     /* allow the build attempt now */
    sk_canvas_t *canvas = rx_host_metal_offscreen_canvas(h);
    if (!canvas) {
        h->gpu_requested = 0;     /* fall back to raster cleanly */
        return RXC_ERR_NO_SKIA;
    }
    return RXC_OK;
}

/* Put this host into ON-SCREEN Metal GPU mode (docs/GPU.md): open a
 * SDL_WINDOW_METAL window with a CAMetalLayer configured for the system Metal
 * device, and a persistent GrDirectContext over it. Per frame, begin_frame
 * acquires the layer's next drawable + builds a GPU surface over it; end_frame
 * flushes; Window#present presents the drawable. Returns RXC_OK when the window
 * + layer + context + a first drawable are live, or a clean RXC_ERR_* on ANY
 * failure — in which case the host is torn back down (NOT half-GPU) so the
 * caller falls back to the GL-window / raster path. Needs SDL + a display.
 *
 * The "first drawable" check is the gate that distinguishes a real display from
 * a headless host: nextDrawable is nil without a window-server-backed layer, so
 * a headless host fails here and falls back cleanly. */
int64_t ruxen_canvas_host_enable_gpu_windowed(int64_t self, int64_t win_scale) {
    RxHost *h = (RxHost *)self;
    if (!h) return RXC_ERR_BAD_ARGS;
    if (h->is_gpu) return RXC_OK;
    const RxSkia *sk = rx_skia();
    if (!sk->available || !sk->gpu_metal_window_ok) return RXC_ERR_NO_SKIA;
    const RxMetal *mtl = rx_metal();
    if (!mtl->available) return RXC_ERR_NO_SKIA;          /* no GPU device */
    if (h->sk_surface || h->sk_canvas) return RXC_ERR_IN_FRAME;

    /* Open the Metal window + layer (win_scale × design for on-screen size),
     * configured with our device + queue. Bounded + clean on a headless host. */
    int64_t wc = ruxen_canvas_window_create_metal(
        self, (int64_t)(intptr_t)mtl->device, (int64_t)(intptr_t)mtl->queue, win_scale);
    if (wc != RXC_OK) return wc;

    /* Build the persistent GrDirectContext and prove a drawable can be acquired
     * (real display). On failure, tear the window back down for a clean fallback. */
    if (!rx_metal_window_context(h)) {
        ruxen_canvas_window_note_host_dropped(self);
        return RXC_ERR_NO_SKIA;
    }
    int64_t tex = ruxen_canvas_window_metal_next_drawable(self);
    if (!tex) {
        /* no drawable -> headless / no display: fall back to GL/raster. */
        rx_host_gpu_teardown(h);
        ruxen_canvas_window_note_host_dropped(self);
        return RXC_ERR_PRESENT;
    }

    h->gpu_requested    = 1;
    h->gpu_windowed     = 1;
    h->is_gpu           = 1;
    h->gpu_backend_kind = RX_GPU_KIND_METAL;
    /* DESIGN size = the logical framebuffer size (what Window.open allocated).
     * begin_frame scales design->backing so a logical-coord app fills the native
     * drawable at native density (crisp), no per-app wiring. */
    h->design_w = h->width;
    h->design_h = h->height;
    /* The drawable we just acquired to gate is released when the first
     * begin_frame acquires its own; nothing rendered into it. */
    return RXC_OK;
}

/* True (1) only when THIS host has a live GPU-backed Skia surface — a
 * GrDirectContext + GPU surface exist and draws genuinely go through the GPU
 * (GL window backend OR offscreen Metal). The unambiguous "GPU is rendering into
 * me" signal (mirrors skia_active for the raster surface). */
int64_t ruxen_canvas_gpu_active(int64_t self) {
    RxHost *h = (RxHost *)self;
    if (!h) return 0;
    return (h->is_gpu && h->gpu_surface) ? 1 : 0;
}

static void rx_host_content_scale(RxHost *h, double *sx, double *sy);  /* fwd */

/* Set the DESIGN size for this host — the logical coordinate space the app draws
 * in. begin_frame then applies a base content-scale = surface(backing) size /
 * design size, so design-coordinate draws fill the native backing surface at
 * native resolution. Windowed hosts set this automatically at enable; this entry
 * lets an OFFSCREEN surface opt into the same content-scale (e.g. a backing-sized
 * framebuffer with a smaller design size — the headless content-scale test).
 * Pass 0,0 to disable (back to scale 1). Returns RXC_OK. */
int64_t ruxen_canvas_host_set_design_size(int64_t self, int64_t design_w, int64_t design_h) {
    RxHost *h = (RxHost *)self;
    if (!h) return RXC_ERR_BAD_ARGS;
    if (design_w < 0 || design_h < 0) return RXC_ERR_BAD_ARGS;
    h->design_w = (int32_t)design_w;
    h->design_h = (int32_t)design_h;
    return RXC_OK;
}

/* The content scale applied to design coordinates this frame, packed as two
 * 16.16 fixed-point ratios: (sx << 32) | sy, each = round(scale * 65536). 1.0 ==
 * 0x10000. Lets L2/tests read the active design->backing scale (1.0 when none).
 * Sized fields probe — not part of the draw ABI. */
int64_t ruxen_canvas_content_scale(int64_t self) {
    RxHost *h = (RxHost *)self;
    if (!h) return ((int64_t)0x10000 << 32) | 0x10000;
    double sx = 1.0, sy = 1.0;
    rx_host_content_scale(h, &sx, &sy);
    int64_t fx = (int64_t)(sx * 65536.0 + 0.5);
    int64_t fy = (int64_t)(sy * 65536.0 + 0.5);
    return (fx << 32) | (fy & 0xFFFFFFFF);
}

/* Which GPU backend is live for this host: RX_GPU_KIND_NONE (0, raster),
 * RX_GPU_KIND_GL (1), or RX_GPU_KIND_METAL (2). The gpu_backend_kind slot, so
 * L2/tests can tell which rung selection landed on. */
int64_t ruxen_canvas_gpu_backend_kind(int64_t self) {
    RxHost *h = (RxHost *)self;
    if (!h) return RX_GPU_KIND_NONE;
    return (h->is_gpu && h->gpu_surface) ? h->gpu_backend_kind : RX_GPU_KIND_NONE;
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

static sk_canvas_t *rx_metal_window_begin_frame(RxHost *h);   /* fwd */

/* The PIXEL size of the host's active rendering surface. For an ON-SCREEN GPU
 * host (windowed Metal OR windowed GL) this is the window's drawable (backing)
 * size; otherwise it is the framebuffer size (h->width/height — which for the
 * offscreen content-scale test IS the backing size). Used to derive the
 * design->backing content scale. */
static void rx_host_surface_size(RxHost *h, int *out_w, int *out_h) {
    int sw = h->width, sh = h->height;
    int64_t packed = 0;
    if (h->gpu_windowed) {
        /* windowed Metal: per-frame drawable */
        packed = ruxen_canvas_window_metal_drawable_size((int64_t)h);
    } else if (h->is_gpu && !h->gpu_offscreen && h->gpu_backend_kind == RX_GPU_KIND_GL) {
        /* windowed GL: the GL default-framebuffer drawable */
        packed = ruxen_canvas_window_gl_drawable_size((int64_t)h);
    }
    if (packed != 0) {
        int dw = (int)(uint32_t)(packed >> 32);
        int dh = (int)(uint32_t)(packed & 0xFFFFFFFF);
        if (dw > 0 && dh > 0) { sw = dw; sh = dh; }
    }
    *out_w = sw;
    *out_h = sh;
}

/* The HiDPI design->backing content scale for this host: surface(backing) size /
 * design size. Returns 1.0/1.0 (no scale) when design_w/h is unset (offscreen /
 * test surfaces draw at explicit pixel sizes). Applied as the base transform in
 * begin_frame so DESIGN-coordinate draws fill the backing surface at native
 * resolution (crisp glyphs/shapes). docs/GPU.md. */
static void rx_host_content_scale(RxHost *h, double *sx, double *sy) {
    *sx = 1.0; *sy = 1.0;
    if (h->design_w <= 0 || h->design_h <= 0) return;   /* no content scale */
    int sw = 0, sh = 0;
    rx_host_surface_size(h, &sw, &sh);
    if (sw > 0 && h->design_w > 0) *sx = (double)sw / (double)h->design_w;
    if (sh > 0 && h->design_h > 0) *sy = (double)sh / (double)h->design_h;
}

int64_t ruxen_canvas_begin_frame(int64_t self) {
    RxHost *h = (RxHost *)self;
    if (!h) return RXC_ERR_BAD_ARGS;
    if (h->in_frame) return RXC_ERR_IN_FRAME;
    h->in_frame = 1;
    /* Reset paint state that must not leak across frames: blend mode back to the
     * default SrcOver (the transform/clip reset is below, on the Skia canvas). */
    h->blend_mode = RX_BLEND_SRC_OVER;
    /* On-screen Metal: acquire this frame's drawable + build the per-frame GPU
     * surface BEFORE any drawing. If no drawable (headless/no display), the
     * frame has no GPU canvas and draws are skipped — a clean no-op, never wrong
     * pixels (present is then also a no-op). */
    if (h->gpu_windowed) {
        rx_metal_window_begin_frame(h);   /* sets h->sk_canvas, or leaves NULL */
    }
    /* Reset canvas state so NOTHING (transform or clip) leaks across frames.
     * restore_to_count(1) pops everything above the pristine base; reset_matrix
     * clears any base-level matrix change.
     *
     * Then apply the HiDPI content scale (design->backing) as the FIRST transform
     * — so DESIGN-coordinate draws fill the native-pixel backing surface and Skia
     * rasterizes at native density (crisp). This is a no-op (scale 1) for
     * offscreen/test surfaces (design_w/h unset). Finally push one save so the
     * whole frame runs at depth >= 2 — letting the NEXT begin_frame's
     * restore_to_count(1) discard even a base-level clip the caller applied
     * without a save. The content scale lives BELOW that save, so it is the
     * pristine base every frame re-establishes (and save/restore can't lose it). */
    sk_canvas_t *canvas = rx_host_canvas(h);
    if (canvas) {
        const RxSkia *sk = rx_skia();
        if (sk->canvas_restore_to_count) sk->canvas_restore_to_count(canvas, 1);
        if (sk->canvas_reset_matrix) sk->canvas_reset_matrix(canvas);
        double sx = 1.0, sy = 1.0;
        rx_host_content_scale(h, &sx, &sy);
        if ((sx != 1.0 || sy != 1.0) && sk->canvas_scale) {
            sk->canvas_scale(canvas, (float)sx, (float)sy);
        }
        if (sk->canvas_save) sk->canvas_save(canvas);
    }
    return RXC_OK;
}

/* End the frame. The software/raster backend has nothing to flip; presenting to
 * a live window is the explicit ruxen_canvas_window_present call in
 * runtime/sdl_window.c (the Ruxen Window.end_frame does both).
 *
 * GPU path (docs/GPU.md): Skia batches GPU draws, so the frame's commands must
 * be flushed + submitted to the GL context here, BEFORE the GL buffer swap the
 * Window present step issues. Without the flush the swap would present an empty
 * or partial backbuffer. We prefer flush_and_submit (ensures the GL commands are
 * handed to the driver), falling back to plain flush. */
int64_t ruxen_canvas_end_frame(int64_t self) {
    RxHost *h = (RxHost *)self;
    if (!h) return RXC_ERR_BAD_ARGS;
    if (!h->in_frame) return RXC_ERR_NO_FRAME;
    h->in_frame = 0;
    if (h->is_gpu && h->gr_context) {
        const RxSkia *sk = rx_skia();
        /* Flush + submit the batched GPU draws to the device. flush_and_submit
         * (sync=1 for the offscreen path) guarantees the work is done before we
         * read it back; for the windowed path it precedes the buffer swap. */
        int sync = h->gpu_offscreen ? 1 : 0;
        if (sk->gr_direct_context_flush_and_submit) {
            sk->gr_direct_context_flush_and_submit((gr_direct_context_t *)h->gr_context, sync);
        } else if (sk->gr_direct_context_flush) {
            sk->gr_direct_context_flush((gr_direct_context_t *)h->gr_context);
        }
        /* Offscreen Metal: copy the GPU pixels back into the host framebuffer so
         * read_pixel observes the real GPU output. The surface is BGRA_8888, so
         * a BGRA dst info lands the bytes in the host's 0xAARRGGBB order — the
         * raster oracle is byte-identical to the GPU result. A failed readback
         * is reported (never leaves a stale/garbage buffer silently). */
        if (h->gpu_offscreen && h->gpu_surface && sk->surface_read_pixels) {
            sk_imageinfo_t dst;
            dst.colorspace = NULL;
            dst.width      = h->width;
            dst.height     = h->height;
            dst.colorType  = RX_SK_COLORTYPE_BGRA_8888;
            dst.alphaType  = RX_SK_ALPHA_PREMUL;
            int ok = sk->surface_read_pixels((sk_surface_t *)h->gpu_surface, &dst,
                                             h->pixels, (size_t)h->width * 4, 0, 0);
            if (!ok) return RXC_ERR_PRESENT;
        }
    }
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

/* ---- offscreen layers (Skia-only) ----
 *
 * save_layer pushes an offscreen layer onto the same save stack as save();
 * subsequent draws accumulate into it, and the matching restore() composites
 * the whole layer down at once — the basis for group opacity and blended
 * overlays (fade transitions, translucent panels). Unlike the matrix/clip
 * save ops (which no-op on the software fallback to keep the stack balanced),
 * a layer that silently failed to composite would yield WRONG pixels, not just
 * unstyled ones — so these are strictly Skia-only and report the failure.
 *
 * Both return the layer's save count (>= 1) on success, to pair with restore /
 * restore_to. Failure is signalled as a NEGATIVE value: -RXC_ERR_* (the Ruxen
 * side maps any negative back to the matching Err, non-negative to Ok(count)).
 * Skia save counts are always >= 1, so the sign is an unambiguous channel. */

/* Push a whole-canvas offscreen layer (bounds = NULL). */
int64_t ruxen_canvas_save_layer(int64_t self) {
    RxHost *h = (RxHost *)self;
    if (!h) return -RXC_ERR_BAD_ARGS;
    if (!h->in_frame) return -RXC_ERR_NO_FRAME;
    sk_canvas_t *canvas = rx_host_canvas(h);
    const RxSkia *sk = rx_skia();
    if (!canvas || !sk->canvas_save_layer) return -RXC_ERR_NO_SKIA;
    return (int64_t)sk->canvas_save_layer(canvas, NULL, NULL);
}

/* Push a whole-canvas offscreen layer whose paint carries a Gaussian BLUR image
 * filter (sigma px): everything drawn into the layer is blurred when the matching
 * restore composites it down. The general blur primitive — it blurs arbitrary
 * content (shapes, text, paths), generalizing the rrect-only drop shadow. Returns
 * the layer's save count (>= 1) to pair with restore / restore_to, or a NEGATIVE
 * -RXC_ERR_* on failure. sigma <= 0 is rejected (a non-blurring blur layer is a
 * caller bug; use plain save_layer). The filter is unref'd after save_layer takes
 * its own ref (the layer owns it for its lifetime). Skia-only. */
int64_t ruxen_canvas_save_layer_blur(int64_t self, double sigma) {
    RxHost *h = (RxHost *)self;
    if (!h) return -RXC_ERR_BAD_ARGS;
    if (!h->in_frame) return -RXC_ERR_NO_FRAME;
    if (rxc_is_nan(sigma) || !(sigma > 0.0) || !rxc_finite_pixels(sigma)) {
        return -RXC_ERR_BAD_ARGS;
    }
    sk_canvas_t *canvas = rx_host_canvas(h);
    const RxSkia *sk = rx_skia();
    if (!canvas || !sk->canvas_save_layer || !sk->imagefilter_new_blur ||
        !sk->paint_set_imagefilter || !sk->imagefilter_unref ||
        !sk->paint_new || !sk->paint_delete) {
        return -RXC_ERR_NO_SKIA;
    }
    sk_paint_t *lp = sk->paint_new();
    if (!lp) return -RXC_ERR_BAD_ARGS;
    /* tile_mode 1 = clamp (decal edges to the source). NULL input = blur the
     * layer's own content; NULL crop = no crop. */
    sk_imagefilter_t *filt = sk->imagefilter_new_blur((float)sigma, (float)sigma, 1, NULL, NULL);
    if (!filt) { sk->paint_delete(lp); return -RXC_ERR_NO_SKIA; }
    sk->paint_set_imagefilter(lp, filt);
    int count = sk->canvas_save_layer(canvas, NULL, lp);
    sk->imagefilter_unref(filt);   /* the layer holds its own ref now */
    sk->paint_delete(lp);
    return (int64_t)count;
}

/* Push a whole-canvas offscreen layer composited with a uniform group opacity
 * `alpha` (0..255). Out-of-range alpha is rejected.
 *
 * This build's libSkiaSharp does NOT export sk_canvas_save_layer_alpha (verified
 * by nm — only sk_canvas_save_layer / _rec exist; the _alpha convenience wrapper
 * was removed upstream). We implement group opacity the canonical way instead:
 * sk_canvas_save_layer(bounds=NULL, paint) where the paint carries only the
 * group alpha — SkCanvas applies the layer paint's alpha as the whole-layer
 * opacity on restore, which is exactly what saveLayerAlpha did internally. The
 * paint's RGB is irrelevant for this (alpha-only) use. */
int64_t ruxen_canvas_save_layer_alpha(int64_t self, int64_t alpha) {
    RxHost *h = (RxHost *)self;
    if (!h) return -RXC_ERR_BAD_ARGS;
    if (!h->in_frame) return -RXC_ERR_NO_FRAME;
    if (alpha < 0 || alpha > 255) return -RXC_ERR_BAD_ARGS;
    sk_canvas_t *canvas = rx_host_canvas(h);
    const RxSkia *sk = rx_skia();
    if (!canvas || !sk->canvas_save_layer || !sk->paint_new ||
        !sk->paint_set_color || !sk->paint_delete) {
        return -RXC_ERR_NO_SKIA;
    }
    sk_paint_t *paint = sk->paint_new();
    if (!paint) return -RXC_ERR_BAD_ARGS;
    /* alpha in the high byte; RGB ignored for layer group opacity. */
    sk->paint_set_color(paint, (sk_color_t)((uint32_t)alpha << 24));
    int count = sk->canvas_save_layer(canvas, NULL, paint);
    sk->paint_delete(paint);   /* saveLayer copies the paint; safe to free now */
    return (int64_t)count;
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

/* Skew the coordinate system by (sx, sy): x' = x + sx*y, y' = y + sy*x. Composes
 * with the current matrix, scoped by save/restore — like translate/scale/rotate.
 * Skia-only (no software-fallback transform); a no-op when the backend is absent. */
int64_t ruxen_canvas_skew(int64_t self, double sx, double sy) {
    RxHost *h = (RxHost *)self;
    if (!h) return RXC_ERR_BAD_ARGS;
    if (!h->in_frame) return RXC_ERR_NO_FRAME;
    if (!rxc_finite_pixels(sx) || !rxc_finite_pixels(sy)) return RXC_ERR_BAD_ARGS;
    sk_canvas_t *canvas = rx_host_canvas(h);
    const RxSkia *sk = rx_skia();
    if (canvas && sk->canvas_skew) sk->canvas_skew(canvas, (float)sx, (float)sy);
    return RXC_OK;
}

/* Concatenate a full 2D affine onto the current matrix. The six args are the top
 * two rows of the 3x3 (row-major): a=scaleX b=skewX c=transX / d=skewY e=scaleY
 * f=transY; the perspective row is fixed to identity (0,0,1). This is the general
 * primitive translate/scale/rotate/skew are special cases of. Composes + is scoped
 * by save/restore. Skia-only; a no-op when the backend is absent. */
int64_t ruxen_canvas_concat(int64_t self, double a, double b, double c,
                            double d, double e, double f) {
    RxHost *h = (RxHost *)self;
    if (!h) return RXC_ERR_BAD_ARGS;
    if (!h->in_frame) return RXC_ERR_NO_FRAME;
    if (!rxc_finite_pixels(a) || !rxc_finite_pixels(b) || !rxc_finite_pixels(c) ||
        !rxc_finite_pixels(d) || !rxc_finite_pixels(e) || !rxc_finite_pixels(f)) {
        return RXC_ERR_BAD_ARGS;
    }
    sk_canvas_t *canvas = rx_host_canvas(h);
    const RxSkia *sk = rx_skia();
    if (canvas && sk->canvas_concat) {
        /* a=scaleX b=skewX c=transX / d=skewY e=scaleY f=transY -> a 4x4 SkM44 in
         * COLUMN-MAJOR order (this build's sk_canvas_concat ABI; see skia_capi.h).
         * col0=(scaleX,skewY,0,0) col1=(skewX,scaleY,0,0) col2=(0,0,1,0)
         * col3=(transX,transY,0,1). */
        float m44[16] = {
            (float)a, (float)d, 0.0f, 0.0f,   /* col0: scaleX, skewY */
            (float)b, (float)e, 0.0f, 0.0f,   /* col1: skewX,  scaleY */
            0.0f,     0.0f,     1.0f, 0.0f,   /* col2 */
            (float)c, (float)f, 0.0f, 1.0f,   /* col3: transX, transY */
        };
        sk->canvas_concat(canvas, m44);
    }
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

/* Map the canvas-side RX_BLEND_* enum to this build's SkBlendMode int (pinned
 * empirically: SrcOver=3, Clear=0, Src=1, Screen=14, Multiply=24). Unknown ->
 * SrcOver, so a bad value degrades to the safe default rather than a wrong mode. */
static int rxc_skblend(int32_t mode) {
    switch (mode) {
    case RX_BLEND_CLEAR:    return 0;   /* SkBlendMode::kClear */
    case RX_BLEND_SRC:      return 1;   /* kSrc */
    case RX_BLEND_SCREEN:   return 14;  /* kScreen */
    case RX_BLEND_MULTIPLY: return 24;  /* kMultiply */
    case RX_BLEND_SRC_OVER:
    default:                return 3;   /* kSrcOver (default) */
    }
}

/* Apply the host's current blend mode to a freshly-built paint. A no-op for the
 * default SrcOver (Skia paints already default to SrcOver, so we skip the call)
 * and when the symbol is absent (older lib) — the draw then uses SrcOver, which
 * is the documented fallback. Every draw wrapper that builds a Skia paint calls
 * this just before issuing the draw. */
static void rx_apply_blend(const RxSkia *sk, sk_paint_t *paint, const RxHost *h) {
    if (!h || h->blend_mode == RX_BLEND_SRC_OVER) return;
    if (sk->paint_set_blendmode) sk->paint_set_blendmode(paint, rxc_skblend(h->blend_mode));
}

/* Set the current blend mode for subsequent draws (docs/ROADMAP.md Phase-1 E1).
 * The mode is a small RX_BLEND_* int; out-of-range is rejected so a caller can't
 * silently set a meaningless mode. State persists until changed or until the next
 * begin_frame resets it to SrcOver. Does NOT require the Skia backend to STORE the
 * mode (it just sets host state); a draw applies it only when Skia is active. */
int64_t ruxen_canvas_set_blend_mode(int64_t self, int64_t mode) {
    RxHost *h = (RxHost *)self;
    if (!h) return RXC_ERR_BAD_ARGS;
    if (mode < 0 || mode > RX_BLEND_MAX) return RXC_ERR_BAD_ARGS;
    h->blend_mode = (int32_t)mode;
    return RXC_OK;
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
        rx_apply_blend(sk, paint, h);
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
static sk_paint_t *rx_make_paint(const RxSkia *sk, const RxHost *h, int64_t argb, double stroke_w) {
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
    rx_apply_blend(sk, p, h);   /* honor the host's current blend mode */
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
    sk_paint_t *paint = rx_make_paint(sk, h, argb, stroke_w);
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
    sk_paint_t *paint = rx_make_paint(sk, h, argb, stroke_w);
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

    sk_paint_t *paint = rx_make_paint(sk, h, argb, stroke_w);
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
    sk_paint_t *paint = rx_make_paint(sk, h, argb, width);
    if (!paint) return RXC_ERR_BAD_ARGS;
    sk->canvas_draw_line(canvas, (float)x0, (float)y0, (float)x1, (float)y1, paint);
    sk->paint_delete(paint);
    return RXC_OK;
}

/* Stroked DASHED line from (x0,y0) to (x1,y1): a 2-interval [on_len, off_len]
 * dash pattern with a `phase` offset into it (Skia repeats the pattern along the
 * stroke). `phase` shifts the start of the dash run (e.g. animate it for a
 * marching-ants selection). Skia-only — RXC_ERR_NO_SKIA when the backend or the
 * dash symbols are absent (the symbols ARE present in the pinned build, see the
 * loader note; this is the honest fallback, never a NULL-stub lie). A
 * non-positive on_len, a negative off_len, or a non-positive stroke_w is
 * RXC_ERR_BAD_ARGS (a dash needs a real stroke + a real "on" run).
 *
 * Memory: the dash path-effect is OWNED — created, set on the paint (the paint
 * takes its own reference), then unref'd here, then the paint is deleted. No
 * leak on any exit path. */
int64_t ruxen_canvas_draw_dashed_line(int64_t self, double x0, double y0,
                                      double x1, double y1, double stroke_w,
                                      double on_len, double off_len, double phase,
                                      int64_t argb) {
    RxHost *h = (RxHost *)self;
    const RxSkia *sk = NULL;
    int64_t err = RXC_OK;
    /* gate on BOTH the line draw AND the dash creator so a build missing dash
     * Errs cleanly rather than drawing a solid line that lies about being dashed. */
    sk_canvas_t *canvas = rx_skia_draw_begin(
        h, h ? (const void *)rx_skia()->canvas_draw_line : NULL, &sk, &err);
    if (!canvas) return err;
    if (!sk->path_effect_create_dash || !sk->path_effect_unref ||
        !sk->paint_set_path_effect) {
        return RXC_ERR_NO_SKIA;
    }
    if (!rxc_finite_pixels(x0) || !rxc_finite_pixels(y0) ||
        !rxc_finite_pixels(x1) || !rxc_finite_pixels(y1) ||
        !rxc_finite_pixels(stroke_w) || !rxc_finite_pixels(on_len) ||
        !rxc_finite_pixels(off_len) || !rxc_finite_pixels(phase)) {
        return RXC_ERR_BAD_ARGS;
    }
    if (!(stroke_w > 0.0) || !(on_len > 0.0) || off_len < 0.0) {
        return RXC_ERR_BAD_ARGS;
    }
    sk_paint_t *paint = rx_make_paint(sk, h, argb, stroke_w);
    if (!paint) return RXC_ERR_BAD_ARGS;
    const float intervals[2] = { (float)on_len, (float)off_len };
    sk_path_effect_t *dash = sk->path_effect_create_dash(intervals, 2, (float)phase);
    if (!dash) { sk->paint_delete(paint); return RXC_ERR_NO_SKIA; }
    sk->paint_set_path_effect(paint, dash);
    sk->canvas_draw_line(canvas, (float)x0, (float)y0, (float)x1, (float)y1, paint);
    sk->path_effect_unref(dash);   /* paint holds its own ref; safe to drop ours */
    sk->paint_delete(paint);
    return RXC_OK;
}

/* ---- paths (Skia-only) ----
 *
 * An sk_path is a mutable builder owned by the Ruxen `Path`: its raw pointer
 * crosses the FFI as an int64 handle (path_new returns it, the builder ops and
 * draw take it, path_drop frees it) — exactly the ownership shape `Image` uses.
 *
 * The builder ops (move_to/line_to/.../close) require the path symbols but NOT
 * a live canvas, so a `Path` can be constructed before any frame; they no-op
 * when a needed symbol is missing (the handle is then 0 and draw_path reports
 * the clear Err). The Skia-only gate that surfaces RXC_ERR_NO_SKIA is the
 * *draw*, mirroring the image flow (load returns 0 → draw is the failure
 * point). Nothing structured crosses the ABI: only the int64 handle and scalar
 * device-pixel coordinates. */

/* Allocate an empty path; returns its sk_path pointer as an int64 handle, or 0
 * when Skia / the path symbols are unavailable. */
int64_t ruxen_canvas_path_new(void) {
    const RxSkia *sk = rx_skia();
    if (!sk->available || !sk->path_new) return 0;
    return (int64_t)sk->path_new();
}

int64_t ruxen_canvas_path_is_null(int64_t self) { return self == 0 ? 1 : 0; }

/* Identity accessor: a RawPath's handle IS the pointer; lets the Ruxen side
 * pass it to draw_path as a plain Int (mirrors ruxen_canvas_image_ptr). */
int64_t ruxen_canvas_path_ptr(int64_t self) { return self; }

void ruxen_canvas_path_drop(int64_t self) {
    const RxSkia *sk = rx_skia();
    if (self && sk->path_delete) sk->path_delete((sk_path_t *)self);
}

/* Begin a new sub-contour at (x, y). */
int64_t ruxen_canvas_path_move_to(int64_t self, double x, double y) {
    sk_path_t *p = (sk_path_t *)self;
    const RxSkia *sk = rx_skia();
    if (!p || !sk->path_move_to) return RXC_ERR_NO_SKIA;
    if (!rxc_finite_pixels(x) || !rxc_finite_pixels(y)) return RXC_ERR_BAD_ARGS;
    sk->path_move_to(p, (float)x, (float)y);
    return RXC_OK;
}

/* Straight segment from the current point to (x, y). */
int64_t ruxen_canvas_path_line_to(int64_t self, double x, double y) {
    sk_path_t *p = (sk_path_t *)self;
    const RxSkia *sk = rx_skia();
    if (!p || !sk->path_line_to) return RXC_ERR_NO_SKIA;
    if (!rxc_finite_pixels(x) || !rxc_finite_pixels(y)) return RXC_ERR_BAD_ARGS;
    sk->path_line_to(p, (float)x, (float)y);
    return RXC_OK;
}

/* Quadratic bézier through control (cx, cy) to (x, y). */
int64_t ruxen_canvas_path_quad_to(int64_t self, double cx, double cy, double x, double y) {
    sk_path_t *p = (sk_path_t *)self;
    const RxSkia *sk = rx_skia();
    if (!p || !sk->path_quad_to) return RXC_ERR_NO_SKIA;
    if (!rxc_finite_pixels(cx) || !rxc_finite_pixels(cy) ||
        !rxc_finite_pixels(x) || !rxc_finite_pixels(y)) {
        return RXC_ERR_BAD_ARGS;
    }
    sk->path_quad_to(p, (float)cx, (float)cy, (float)x, (float)y);
    return RXC_OK;
}

/* Cubic bézier through controls (c1x, c1y), (c2x, c2y) to (x, y). */
int64_t ruxen_canvas_path_cubic_to(int64_t self, double c1x, double c1y,
                                   double c2x, double c2y, double x, double y) {
    sk_path_t *p = (sk_path_t *)self;
    const RxSkia *sk = rx_skia();
    if (!p || !sk->path_cubic_to) return RXC_ERR_NO_SKIA;
    if (!rxc_finite_pixels(c1x) || !rxc_finite_pixels(c1y) ||
        !rxc_finite_pixels(c2x) || !rxc_finite_pixels(c2y) ||
        !rxc_finite_pixels(x) || !rxc_finite_pixels(y)) {
        return RXC_ERR_BAD_ARGS;
    }
    sk->path_cubic_to(p, (float)c1x, (float)c1y, (float)c2x, (float)c2y, (float)x, (float)y);
    return RXC_OK;
}

/* SVG-style elliptical arc to (x, y): radii (rx, ry), x-axis rotation in
 * degrees, large-arc flag (0/1), clockwise-sweep flag (0/1). */
int64_t ruxen_canvas_path_arc_to(int64_t self, double rx, double ry, double x_axis_rotate,
                                 int64_t large_arc, int64_t sweep, double x, double y) {
    sk_path_t *p = (sk_path_t *)self;
    const RxSkia *sk = rx_skia();
    if (!p || !sk->path_arc_to) return RXC_ERR_NO_SKIA;
    if (!rxc_finite_pixels(rx) || !rxc_finite_pixels(ry) || !rxc_finite_pixels(x_axis_rotate) ||
        !rxc_finite_pixels(x) || !rxc_finite_pixels(y)) {
        return RXC_ERR_BAD_ARGS;
    }
    int la = large_arc ? RX_SK_PATH_ARC_LARGE : RX_SK_PATH_ARC_SMALL;
    int sw = sweep ? RX_SK_PATH_ARC_CW : RX_SK_PATH_ARC_CCW;
    sk->path_arc_to(p, (float)rx, (float)ry, (float)x_axis_rotate, la, sw, (float)x, (float)y);
    return RXC_OK;
}

/* Close the current sub-contour (straight segment back to its start). */
int64_t ruxen_canvas_path_close(int64_t self) {
    sk_path_t *p = (sk_path_t *)self;
    const RxSkia *sk = rx_skia();
    if (!p || !sk->path_close) return RXC_ERR_NO_SKIA;
    sk->path_close(p);
    return RXC_OK;
}

/* Set the fill rule: even_odd != 0 selects even-odd, else non-zero winding. */
int64_t ruxen_canvas_path_set_fill_type(int64_t self, int64_t even_odd) {
    sk_path_t *p = (sk_path_t *)self;
    const RxSkia *sk = rx_skia();
    if (!p || !sk->path_set_filltype) return RXC_ERR_NO_SKIA;
    sk->path_set_filltype(p, even_odd ? RX_SK_PATH_FILL_EVENODD : RX_SK_PATH_FILL_WINDING);
    return RXC_OK;
}

/* Fill (stroke_w <= 0) or stroke (stroke_w > 0) the path with a solid color.
 * Antialiased; Skia-only — RXC_ERR_NO_SKIA when the backend or draw symbol is
 * absent (never a silent no-op). Color packed 0xAARRGGBB in `argb`. */
int64_t ruxen_canvas_draw_path(int64_t self, int64_t path, double stroke_w, int64_t argb) {
    RxHost *h = (RxHost *)self;
    sk_path_t *p = (sk_path_t *)path;
    const RxSkia *sk = NULL;
    int64_t err = RXC_OK;
    sk_canvas_t *canvas = rx_skia_draw_begin(h, h ? (const void *)rx_skia()->canvas_draw_path : NULL,
                                             &sk, &err);
    if (!canvas) return err;
    if (!p) return RXC_ERR_BAD_ARGS;
    if (!rxc_finite_pixels(stroke_w)) return RXC_ERR_BAD_ARGS;
    sk_paint_t *paint = rx_make_paint(sk, h, argb, stroke_w);
    if (!paint) return RXC_ERR_BAD_ARGS;
    sk->canvas_draw_path(canvas, p, paint);
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

/* ---- render-to-texture / raster cache (Skia-only) ----
 *
 * Snapshot THIS host's current rendering surface into an immutable sk_image,
 * returned as an image handle the Ruxen side wraps in an `Image` (so it reuses
 * the whole existing draw_image path — no new draw ABI). The decisive primitive
 * for caching an expensive subtree: draw it once into an offscreen Canvas, snapshot
 * it, then blit the snapshot cheaply every frame.
 *
 * The snapshot is a COPY of the surface contents at call time — it does not alias
 * the surface's pixels, so further draws into the surface (or freeing the offscreen
 * host) leave the image intact. The returned image is caller-owned: the Ruxen
 * `Image` frees it via ruxen_canvas_image_drop (sk_image_unref), the SAME ownership
 * as a loaded image, so there is exactly one free path and no double-free.
 *
 * Returns the sk_image pointer as an int64 handle, or 0 on failure (no Skia / no
 * surface / snapshot symbol absent). Drawing offscreen never touches another
 * canvas's framebuffer: each host owns its own pixels buffer; only an explicit
 * draw_image of the snapshot moves content between them. */
int64_t ruxen_canvas_host_snapshot(int64_t self) {
    RxHost *h = (RxHost *)self;
    if (!h) return 0;
    const RxSkia *sk = rx_skia();
    if (!sk->available || !sk->surface_new_image_snapshot) return 0;
    /* Force the surface into existence (raster-direct, or the GPU surface for a
     * GPU host) so we snapshot whatever the canvas draws into. rx_host_canvas
     * leaves the surface handle in h->sk_surface (raster) or h->gpu_surface (GPU). */
    sk_canvas_t *canvas = rx_host_canvas(h);
    if (!canvas) return 0;
    sk_surface_t *surf = h->gpu_surface ? (sk_surface_t *)h->gpu_surface
                                        : (sk_surface_t *)h->sk_surface;
    if (!surf) return 0;
    sk_image_t *img = sk->surface_new_image_snapshot(surf);
    return (int64_t)img;   /* caller-owned; freed by ruxen_canvas_image_drop */
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

/* ---- font family cache ----
 *
 * Selecting a typeface by family name (sk_typeface_create_from_name) is the
 * "configurable font family" capability. Each resolved family gets one cached
 * sk_font_t (built once, resized in place per call like the default font),
 * owned for the process lifetime — the same never-freed singleton model as
 * rx_default_font (the surface/typeface tables outlive any one host). The cache
 * is tiny and append-only; a full cache reuses the default font.
 *
 * GRACEFUL FALLBACK: an unknown / unavailable family is NOT an error. When
 * sk_typeface_create_from_name returns NULL (or yields a zero-glyph face), we
 * fall back to the default font so an uninstalled font never breaks rendering.
 * We still cache that decision under the requested name, so the fallback is
 * resolved once. (A missing Skia backend is the only Err case — handled by the
 * callers, which use the bitmap path when rx_font_for_family returns NULL.) */
#define RXC_FAMILY_CACHE_CAP 16
#define RXC_FAMILY_NAME_MAX  63

typedef struct {
    char       name[RXC_FAMILY_NAME_MAX + 1];
    sk_font_t *font;   /* the family's face, or the default face on fallback */
} RxFamilyEntry;

/* Build a font for `family` from a freshly matched typeface, or NULL if the
 * family can't be resolved into a usable (positive-advance) face. Caller owns
 * nothing extra: the font keeps a ref to the typeface, so we drop our ref. */
static sk_font_t *rx_build_family_font(const RxSkia *sk, const char *family) {
    if (!sk->typeface_create_from_name || !sk->font_new_with_values) return NULL;
    sk_fontstyle_t *style = NULL;
    if (sk->fontstyle_new) style = sk->fontstyle_new(400, 5, 0);  /* normal/normal/upright */
    sk_typeface_t *tf = sk->typeface_create_from_name(family, style);
    if (style && sk->fontstyle_delete) sk->fontstyle_delete(style);
    if (!tf) return NULL;
    sk_font_t *f = sk->font_new_with_values(tf, RXC_SKIA_FONT_PX, 1.0f, 0.0f);
    if (sk->typeface_unref) sk->typeface_unref(tf);  /* the font holds its own ref */
    if (!f) return NULL;
    /* Reject a zero-glyph face (mirrors rx_default_font's probe) so it falls
     * back to the default rather than drawing nothing. */
    float probe = sk->font_measure_text(f, "M", 1, RX_SK_TEXT_UTF8, NULL, NULL);
    if (!(probe > 0.0f)) {
        if (sk->font_delete) sk->font_delete(f);
        return NULL;
    }
    return f;
}

/* The cached font for `family` at `size` px, resized in place. Falls back to
 * the default font when the family is empty/unknown/unavailable. NULL only when
 * no Skia font is available at all (Skia absent) — callers then use the bitmap
 * path, exactly as for the default font. */
static sk_font_t *rx_font_for_family(const char *family, double size) {
    sk_font_t *fallback = rx_default_font();
    if (!fallback) return NULL;                       /* no Skia text at all */
    if (!family || !family[0]) return rx_font_at(size);

    static RxFamilyEntry cache[RXC_FAMILY_CACHE_CAP];
    static int count = 0;
    const RxSkia *sk = rx_skia();

    for (int i = 0; i < count; i++) {
        if (strncmp(cache[i].name, family, RXC_FAMILY_NAME_MAX) == 0) {
            sk_font_t *f = cache[i].font;
            if (sk->font_set_size && size > 0.0) sk->font_set_size(f, (float)size);
            return f;
        }
    }

    /* Miss: resolve the family (or fall back to the default face) and cache it.
     * If the cache is full, just use the default font without caching. */
    sk_font_t *resolved = rx_build_family_font(sk, family);
    if (!resolved) resolved = fallback;               /* graceful fallback */
    if (count < RXC_FAMILY_CACHE_CAP) {
        strncpy(cache[count].name, family, RXC_FAMILY_NAME_MAX);
        cache[count].name[RXC_FAMILY_NAME_MAX] = '\0';
        cache[count].font = resolved;
        count++;
    }
    if (sk->font_set_size && size > 0.0) sk->font_set_size(resolved, (float)size);
    return resolved;
}

/* Shared text impl at an explicit, already-resolved `font` (which already has
 * its size set). `argb` is packed 0xAARRGGBB. A NULL font selects the bitmap
 * fallback. */
static int64_t rx_draw_text_with_font(RxHost *h, const char *s, double x, double y,
                                      int64_t argb, sk_font_t *font) {
    if (!h || !s) return RXC_ERR_BAD_ARGS;
    if (!h->in_frame) return RXC_ERR_NO_FRAME;
    if (!rxc_finite_pixels(x) || !rxc_finite_pixels(y)) return RXC_ERR_BAD_ARGS;

    /* Skia path: a real antialiased font. (x, y) is the baseline origin, the
     * same convention as the bitmap path below. */
    sk_canvas_t *canvas = rx_host_canvas(h);
    const RxSkia *sk = rx_skia();
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

/* Draw text with the default font at `size` px (the family-less path). */
static int64_t rx_draw_text_impl(RxHost *h, const char *s, double x, double y,
                                 int64_t argb, double size) {
    return rx_draw_text_with_font(h, s, x, y, argb, rx_font_at(size));
}

/* Measure with an already-resolved `font` (NULL = bitmap advance). */
static int64_t rx_measure_with_font(const char *s, sk_font_t *font) {
    if (!s) return 0;
    const RxSkia *sk = rx_skia();
    if (sk->available && font && sk->font_measure_text) {
        float w = sk->font_measure_text(font, s, strlen(s), RX_SK_TEXT_UTF8, NULL, NULL);
        if (!(w > 0.0f)) return 0;
        return (int64_t)(w + 0.5f);
    }
    size_t n = strlen(s);
    return n ? (int64_t)(n * RXC_ADVANCE - 1) : 0;
}

static int64_t rx_measure_impl(const char *s, double size) {
    return rx_measure_with_font(s, rx_font_at(size));
}

/* Line height of an already-resolved `font` (NULL = bitmap 7px). */
static int64_t rx_text_height_with_font(sk_font_t *font) {
    const RxSkia *sk = rx_skia();
    if (sk->available && font && sk->font_get_metrics) {
        sk_fontmetrics_t m;
        sk->font_get_metrics(font, &m);   /* ascent is negative (above baseline) */
        float hgt = m.descent - m.ascent;
        if (hgt >= 1.0f) return (int64_t)(hgt + 0.5f);
    }
    return RXC_GLYPH_H;
}

static int64_t rx_text_height_impl(double size) {
    return rx_text_height_with_font(rx_font_at(size));
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

/* ---- configurable font family ----
 *
 * Pick a typeface by family name. The family resolves through the process-wide
 * cache (rx_font_for_family), which falls back to the default face for an
 * unknown/uninstalled family — a missing font never breaks rendering, and is
 * NOT an error. `text` and `family` are C-string pointers (borrowed &Strings).
 *
 * draw_text_font is Skia-only: picking a family is meaningless for the 5x7
 * bitmap, so it returns RXC_ERR_NO_SKIA when no Skia font is available (rather
 * than silently drawing the bitmap face, which would ignore the family).
 * measure_text_font / text_height_font always return a usable number, falling
 * back to the bitmap metrics when Skia is absent. */

int64_t ruxen_canvas_draw_text_font(int64_t self, int64_t text, double x, double y,
                                    double size, int64_t family, int64_t argb) {
    RxHost *h = (RxHost *)self;
    const char *s = (const char *)text;
    const char *fam = (const char *)family;
    if (!h || !s) return RXC_ERR_BAD_ARGS;
    if (!h->in_frame) return RXC_ERR_NO_FRAME;
    sk_font_t *font = rx_font_for_family(fam, size);
    if (!font) return RXC_ERR_NO_SKIA;   /* no Skia text face -> family unhonorable */
    return rx_draw_text_with_font(h, s, x, y, argb, font);
}

/* Advance width of `text` in `family` at `size` px (bitmap advance when Skia
 * is absent — so it always returns a usable number; an unknown family measures
 * like the default face). */
int64_t ruxen_canvas_measure_text_font(int64_t self, int64_t text, double size, int64_t family) {
    (void)self;
    return rx_measure_with_font((const char *)text, rx_font_for_family((const char *)family, size));
}

/* Line height of `family` at `size` px (bitmap 7px when Skia is absent). */
int64_t ruxen_canvas_text_height_font(int64_t self, double size, int64_t family) {
    (void)self;
    return rx_text_height_with_font(rx_font_for_family((const char *)family, size));
}

/* ---- paragraphs: multi-line, word-wrapped, aligned text ----
 *
 * The fetched libSkiaSharp does NOT ship SkParagraph / SkShaper (verified by
 * nm — that needs a separate HarfBuzzSharp+ICU build). So word-wrap is done
 * HERE, in the shim, on top of the already-bound Skia font measure + draw:
 * greedy line-breaking on ASCII whitespace to fit `max_width`, each wrapped
 * line drawn at line-height spacing and aligned (left/center/right). This needs
 * no new native library and covers the vast majority of UI text (wrapping
 * labels, text blocks, multi-line content).
 *
 * SCOPE: Latin word-wrap. Proper international shaping — bidi, ligatures,
 * complex scripts, grapheme/line-break tables — is deliberately deferred to a
 * later HarfBuzz/ICU follow-up (docs/ROADMAP.md). Whitespace here is ASCII
 * space/tab/newline; an explicit '\n' forces a line break.
 *
 * Skia-only: a wrapped paragraph cannot be faithfully reproduced on the 5x7
 * bitmap fallback (no real advances), so these report RXC_ERR_NO_SKIA when no
 * Skia font is available rather than mis-laying-out text.
 *
 * Alignment within the max_width column: 0 = left, 1 = center, 2 = right. */
enum { RXC_ALIGN_LEFT = 0, RXC_ALIGN_CENTER = 1, RXC_ALIGN_RIGHT = 2 };

/* True for the ASCII whitespace we break on. */
static int rxc_is_space(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/* Measure a byte range [s, s+len) with an already-resolved Skia font, in whole
 * device pixels. Empty range measures 0. */
static int rxc_measure_run(const RxSkia *sk, sk_font_t *font, const char *s, size_t len) {
    if (len == 0) return 0;
    float w = sk->font_measure_text(font, s, len, RX_SK_TEXT_UTF8, NULL, NULL);
    if (!(w > 0.0f)) return 0;
    return (int)(w + 0.5f);
}

static int rxc_hb_direction(int64_t dir);   /* fwd: defined in the shaping section */

/* ---- shaper context: shaped paragraph lines (docs/SHAPING.md) ----
 *
 * A paragraph-wide HarfBuzz + Skia handle set, built ONCE for the whole
 * paragraph (not per line): the HarfBuzz font (for shaping a byte sub-range) and
 * the matching Skia typeface+font (the SAME font file, so glyph ids agree, plus
 * a measured line height). rx_paragraph_layout consults this when shaping is
 * requested, so its greedy word-wrap measures + draws each line through the
 * shaped glyph path; with a NULL context the layout uses the non-shaped
 * measure+draw exactly as before. */
typedef struct {
    const RxSkia *sk;
    const RxHB   *hb;
    hb_blob_t    *blob;
    hb_face_t    *face;
    hb_font_t    *hfont;     /* HarfBuzz font, scaled to size*64 (26.6 fixed) */
    sk_typeface_t *tf;
    sk_font_t    *skf;       /* Skia font over the same file, size set */
    int           hbdir;     /* RX_HB_DIR_* or INVALID for auto */
    int           line_height;
    int           ok;        /* 1 iff every handle was created */
} RxShaper;

/* Build the paragraph shaper for `font_path` at `size` px and `dir`. Returns ok=0
 * (and releases anything partial) when shaping is unavailable or the font file
 * can't be opened — the caller then reports a clean Err. */
static void rx_shaper_init(RxShaper *s, const char *font_path, double size, int64_t dir) {
    memset(s, 0, sizeof(*s));
    s->sk = rx_skia();
    s->hb = rx_hb();
    if (!s->sk->available || !s->sk->glyph_render_ok || !s->hb->available) return;
    if (!font_path || !font_path[0]) return;

    int scale = (int)(size * 64.0 + 0.5);
    s->blob = s->hb->blob_create_from_file(font_path);
    if (!s->blob) return;
    s->face = s->hb->face_create(s->blob, 0);
    s->hfont = s->face ? s->hb->font_create(s->face) : NULL;
    if (!s->hfont) return;
    s->hb->font_set_scale(s->hfont, scale, scale);
    s->hbdir = rxc_hb_direction(dir);

    s->tf = s->sk->typeface_create_from_file(font_path, 0);
    s->skf = s->sk->font_new_with_values(s->tf, (float)size, 1.0f, 0.0f);
    if (!s->skf) return;
    int lh = (int)rx_text_height_with_font(s->skf);
    s->line_height = lh < 1 ? 1 : lh;
    s->ok = 1;
}

static void rx_shaper_drop(RxShaper *s) {
    if (s->skf && s->sk->font_delete) s->sk->font_delete(s->skf);
    if (s->tf && s->sk->typeface_unref) s->sk->typeface_unref(s->tf);
    if (s->hfont) s->hb->font_destroy(s->hfont);
    if (s->face) s->hb->face_destroy(s->face);
    if (s->blob) s->hb->blob_destroy(s->blob);
    memset(s, 0, sizeof(*s));
}

/* Shape the byte sub-range [start, start+len) of `text` and return its total
 * advance width in whole device pixels (kerning + ligatures applied). Uses
 * hb_buffer_add_utf8's item_offset/item_length to shape the sub-range with the
 * surrounding text as context. Empty range = 0. */
static int rx_shaper_measure(RxShaper *s, const char *text, size_t start, size_t len) {
    if (len == 0 || !s->ok) return 0;
    hb_buffer_t *buf = s->hb->buffer_create();
    if (!buf) return 0;
    s->hb->buffer_add_utf8(buf, text, -1, (unsigned int)start, (int)len);
    if (s->hbdir != RX_HB_DIR_INVALID) s->hb->buffer_set_direction(buf, s->hbdir);
    s->hb->buffer_guess_segment_properties(buf);
    s->hb->shape(s->hfont, buf, NULL, 0);
    unsigned int n = s->hb->buffer_get_length(buf);
    hb_glyph_position_t *gp = s->hb->buffer_get_glyph_positions(buf, NULL);
    double adv = 0.0;
    for (unsigned int i = 0; i < n; i++) adv += gp[i].x_advance / 64.0;
    s->hb->buffer_destroy(buf);
    return (int)(adv + 0.5);
}

/* Draw the shaped byte sub-range [start, start+len) at baseline (x, y) with
 * packed color argb, onto `canvas`. Builds a positioned-glyph textblob from the
 * HarfBuzz shaping and draws it (the same render path as draw_text_shaped). */
static void rx_shaper_draw(RxShaper *s, sk_canvas_t *canvas, const char *text,
                           size_t start, size_t len, double x, double y, int64_t argb) {
    if (len == 0 || !s->ok || !canvas) return;
    hb_buffer_t *buf = s->hb->buffer_create();
    if (!buf) return;
    s->hb->buffer_add_utf8(buf, text, -1, (unsigned int)start, (int)len);
    if (s->hbdir != RX_HB_DIR_INVALID) s->hb->buffer_set_direction(buf, s->hbdir);
    s->hb->buffer_guess_segment_properties(buf);
    s->hb->shape(s->hfont, buf, NULL, 0);
    unsigned int n = s->hb->buffer_get_length(buf);
    if (n == 0) { s->hb->buffer_destroy(buf); return; }
    hb_glyph_info_t *gi = s->hb->buffer_get_glyph_infos(buf, NULL);
    hb_glyph_position_t *gp = s->hb->buffer_get_glyph_positions(buf, NULL);

    sk_textblob_builder_t *b = s->sk->textblob_builder_new();
    if (b) {
        sk_textblob_runbuffer_t rb;
        s->sk->textblob_builder_alloc_run_pos(b, s->skf, (int)n, NULL, &rb);
        uint16_t *gout = (uint16_t *)rb.glyphs;
        float    *pout = (float *)rb.pos;
        double penx = 0.0;
        for (unsigned int i = 0; i < n; i++) {
            gout[i] = (uint16_t)gi[i].codepoint;
            pout[2 * i + 0] = (float)(penx + gp[i].x_offset / 64.0);
            pout[2 * i + 1] = (float)(-(gp[i].y_offset / 64.0));
            penx += gp[i].x_advance / 64.0;
        }
        sk_textblob_t *blobt = s->sk->textblob_builder_make(b);
        s->sk->textblob_builder_delete(b);
        if (blobt) {
            sk_paint_t *paint = s->sk->paint_new();
            if (paint) {
                s->sk->paint_set_antialias(paint, 1);
                s->sk->paint_set_style(paint, RX_SK_PAINT_FILL);
                s->sk->paint_set_color(paint, (sk_color_t)(uint32_t)argb);
                s->sk->canvas_draw_text_blob(canvas, blobt, (float)x, (float)y, paint);
                s->sk->paint_delete(paint);
            }
            s->sk->textblob_unref(blobt);
        }
    }
    s->hb->buffer_destroy(buf);
}

/* Greedy word-wrap + (optionally) draw of `text` into a `max_width` column at
 * font `font`, with the first baseline at (x, y0 + ascent...) — actually y0 is
 * the FIRST line's baseline, and each subsequent line is one line_height below.
 *
 * If `draw` is nonzero, each line is rendered via canvas_draw_simple_text at its
 * aligned x; otherwise nothing is drawn (measure-only). Returns the number of
 * lines laid out, and writes the widest line's width to *out_max_w. The caller
 * derives total height = n_lines * line_height.
 *
 * Breaking rules (greedy):
 *   - words are maximal non-whitespace runs; an explicit '\n' forces a break.
 *   - a word is appended to the current line if (current line width + space +
 *     word) still fits max_width; otherwise the current line is flushed and the
 *     word starts a new line.
 *   - a single word WIDER than max_width that is alone on its line is placed
 *     anyway (it overflows the column rather than looping forever) — correctness
 *     over fitting; we never split inside a word this round.
 *
 * Each line is drawn from the ORIGINAL text buffer by [start,end) offsets, so no
 * copying/NUL-termination is needed and draw uses the exact source bytes. */
static int rx_paragraph_layout(RxHost *h, const RxSkia *sk, sk_font_t *font,
                               const char *text, double x, double y0,
                               double max_width, int line_height, int align,
                               int64_t argb, int draw, int *out_max_w,
                               RxShaper *shaper) {
    int max_col = (max_width > 0.0) ? (int)(max_width + 0.5) : 0;
    int n_lines = 0;
    int widest = 0;

    sk_paint_t *paint = NULL;
    if (draw) {
        paint = sk->paint_new();
        if (!paint) return -1;           /* signal allocation failure */
        sk->paint_set_antialias(paint, 1);
        sk->paint_set_style(paint, RX_SK_PAINT_FILL);
        sk->paint_set_color(paint, (sk_color_t)(uint32_t)argb);
    }

    size_t i = 0;
    /* Current pending line is the byte range [line_start, line_end); line_w is
     * its measured width. -1 line_start means "no line started yet". */
    size_t line_start = 0, line_end = 0;
    int    line_w = 0;
    int    have_line = 0;

    /* Flush the current line: align + (optionally) draw it, advance the row.
     * When `shaper` is set, the line is drawn through the HarfBuzz-shaped glyph
     * path (kerning/ligatures); otherwise the plain canvas_draw_simple_text. */
    #define RXC_FLUSH_LINE()                                                      \
        do {                                                                      \
            int lw = line_w;                                                      \
            if (lw > widest) widest = lw;                                         \
            if (draw) {                                                           \
                double off = 0.0;                                                 \
                if (align == RXC_ALIGN_CENTER) off = ((double)max_col - lw) / 2.0;\
                else if (align == RXC_ALIGN_RIGHT) off = (double)max_col - lw;    \
                if (off < 0.0) off = 0.0;                                         \
                double by = y0 + (double)n_lines * line_height;                   \
                size_t llen = line_end - line_start;                             \
                if (llen > 0) {                                                   \
                    if (shaper)                                                   \
                        rx_shaper_draw(shaper, rx_host_canvas(h), text,           \
                            line_start, llen, x + off, by, argb);                 \
                    else                                                          \
                        sk->canvas_draw_simple_text(rx_host_canvas(h),            \
                            text + line_start, llen, RX_SK_TEXT_UTF8,             \
                            (float)(x + off), (float)by, font, paint);           \
                }                                                                 \
            }                                                                     \
            n_lines++;                                                            \
            have_line = 0; line_w = 0;                                            \
        } while (0)

    while (text[i]) {
        /* skip run of whitespace; an explicit newline forces a break of any
         * pending line (and produces an empty line only via blank input runs we
         * collapse — a lone '\n' between words just ends the current line). */
        if (rxc_is_space((unsigned char)text[i])) {
            int had_newline = 0;
            while (text[i] && rxc_is_space((unsigned char)text[i])) {
                if (text[i] == '\n') had_newline = 1;
                i++;
            }
            if (had_newline && have_line) RXC_FLUSH_LINE();
            continue;
        }
        /* a word: maximal non-whitespace run [w_start, w_end) */
        size_t w_start = i;
        while (text[i] && !rxc_is_space((unsigned char)text[i])) i++;
        size_t w_end = i;
        int word_w = shaper ? rx_shaper_measure(shaper, text, w_start, w_end - w_start)
                            : rxc_measure_run(sk, font, text + w_start, w_end - w_start);

        if (!have_line) {
            /* start a fresh line with this word (even if it overflows alone) */
            line_start = w_start; line_end = w_end; line_w = word_w;
            have_line = 1;
        } else {
            /* candidate width if we append " word": measure the joined range so
             * the inter-word space advance is exact for the font. Shaped when a
             * shaper is set, so wrap + alignment use kerned/ligature widths. */
            int joined = shaper ? rx_shaper_measure(shaper, text, line_start, w_end - line_start)
                                : rxc_measure_run(sk, font, text + line_start, w_end - line_start);
            if (max_col > 0 && joined > max_col) {
                /* would overflow: flush the current line, word starts the next */
                RXC_FLUSH_LINE();
                line_start = w_start; line_end = w_end; line_w = word_w;
                have_line = 1;
            } else {
                /* extend the current line to include the space + word */
                line_end = w_end;
                line_w = joined;
            }
        }
    }
    if (have_line) RXC_FLUSH_LINE();

    #undef RXC_FLUSH_LINE

    if (draw && paint) sk->paint_delete(paint);
    if (out_max_w) *out_max_w = widest;
    return n_lines;
}

/* Resolve the paragraph font + line height; shared by draw/measure. Returns the
 * font (size set) or NULL when no Skia font is available; writes the line height
 * to *out_lh. */
static sk_font_t *rx_paragraph_font(const char *family, double size, int *out_lh) {
    sk_font_t *font = rx_font_for_family(family, size);
    if (!font) { *out_lh = 0; return NULL; }
    int lh = (int)rx_text_height_with_font(font);
    if (lh < 1) lh = 1;
    *out_lh = lh;
    return font;
}

/* Draw a word-wrapped, aligned paragraph of `text` into a `max_width` column.
 * (x, y) is the origin: x is the column's left edge, y is the FIRST line's
 * baseline. Subsequent lines stack one line-height below. Returns the laid-out
 * TOTAL HEIGHT in pixels (n_lines * line_height) so L2 can size a text block,
 * or a NEGATIVE -RXC_ERR_* on failure (Skia-only — no bitmap word-wrap). */
int64_t ruxen_canvas_draw_paragraph(int64_t self, int64_t text, double x, double y,
                                    double max_width, double size, int64_t family,
                                    int64_t align, int64_t argb) {
    RxHost *h = (RxHost *)self;
    const char *s = (const char *)text;
    const char *fam = (const char *)family;
    if (!h || !s) return -RXC_ERR_BAD_ARGS;
    if (!h->in_frame) return -RXC_ERR_NO_FRAME;
    if (!rxc_finite_pixels(x) || !rxc_finite_pixels(y) || !rxc_finite_pixels(max_width)) {
        return -RXC_ERR_BAD_ARGS;
    }
    if (align < 0 || align > RXC_ALIGN_RIGHT) return -RXC_ERR_BAD_ARGS;

    sk_canvas_t *canvas = rx_host_canvas(h);
    const RxSkia *sk = rx_skia();
    int lh = 0;
    sk_font_t *font = rx_paragraph_font(fam, size, &lh);
    if (!canvas || !font || !sk->canvas_draw_simple_text || !sk->font_measure_text) {
        return -RXC_ERR_NO_SKIA;
    }
    int n = rx_paragraph_layout(h, sk, font, s, x, y, max_width, lh,
                                (int)align, argb, /*draw*/1, NULL, /*shaper*/NULL);
    if (n < 0) return -RXC_ERR_BAD_ARGS;   /* paint alloc failed */
    return (int64_t)n * lh;
}

/* Measure a word-wrapped paragraph WITHOUT drawing: returns the laid-out size
 * packed as (max_line_width << 32) | total_height — both in pixels — so L2 gets
 * the wrapped block's width and height in one call for layout. NEGATIVE
 * -RXC_ERR_* on failure (Skia-only). */
int64_t ruxen_canvas_measure_paragraph(int64_t self, int64_t text, double max_width,
                                       double size, int64_t family) {
    (void)self;
    const char *s = (const char *)text;
    const char *fam = (const char *)family;
    if (!s) return -RXC_ERR_BAD_ARGS;
    if (!rxc_finite_pixels(max_width)) return -RXC_ERR_BAD_ARGS;
    const RxSkia *sk = rx_skia();
    int lh = 0;
    sk_font_t *font = rx_paragraph_font(fam, size, &lh);
    if (!font || !sk->font_measure_text) return -RXC_ERR_NO_SKIA;
    int max_w = 0;
    int n = rx_paragraph_layout(NULL, sk, font, s, 0.0, 0.0, max_width, lh,
                                RXC_ALIGN_LEFT, 0, /*draw*/0, &max_w, /*shaper*/NULL);
    if (n < 0) return -RXC_ERR_BAD_ARGS;
    int64_t height = (int64_t)n * lh;
    return ((int64_t)(uint32_t)max_w << 32) | (int64_t)(uint32_t)height;
}

/* ---- shaped text: HarfBuzz shape + Skia glyph render (docs/SHAPING.md) ----
 *
 * The fetched libSkiaSharp has no SkShaper, so a single run is shaped with
 * HarfBuzz (kerning, ligatures; RTL/complex with an explicit direction) and the
 * resulting positioned glyphs are rendered with Skia's textblob API. HarfBuzz
 * and Skia are fed the SAME font FILE (sk_typeface_create_from_file +
 * hb_blob_create_from_file_or_fail), so glyph ids match.
 *
 * SCOPE (bounded first increment): ONE run, ONE font (by file path), with
 * left-to-right / right-to-left / auto direction. Proper bidi + line-break +
 * grapheme segmentation (ICU) and multi-run paragraph integration are a later
 * follow-up. This is Skia+HarfBuzz-only — a clean Err when either is absent; the
 * non-shaped draw_text / draw_paragraph path is the fallback.
 *
 * hb advances are 26.6 fixed because we set hb scale = size*64; divide by 64 for
 * device pixels. */

/* Map the FFI direction arg (0 auto / 1 LTR / 2 RTL) to an hb_direction_t, or 0
 * for "auto" (then guess_segment_properties picks it from the script). */
static int rxc_hb_direction(int64_t dir) {
    if (dir == 1) return RX_HB_DIR_LTR;
    if (dir == 2) return RX_HB_DIR_RTL;
    return RX_HB_DIR_INVALID;   /* auto */
}

/* Shape `text` with `font_path` at `size` px and `dir`, then either draw the
 * positioned glyph run at baseline (x, y) with color `argb` (draw != 0) or just
 * measure it. Returns the run's total advance width in pixels, or a NEGATIVE
 * -RXC_ERR_* on failure. All HarfBuzz + Skia objects are created and released
 * within the call (bounded first increment — no font cache yet). */
static int64_t rx_shape_run(RxHost *h, const char *text, double x, double y,
                            double size, const char *font_path, int64_t dir,
                            int64_t argb, int draw) {
    if (!text || !font_path || !font_path[0]) return -RXC_ERR_BAD_ARGS;
    if (draw) {
        if (!h) return -RXC_ERR_BAD_ARGS;
        if (!h->in_frame) return -RXC_ERR_NO_FRAME;
        if (!rxc_finite_pixels(x) || !rxc_finite_pixels(y)) return -RXC_ERR_BAD_ARGS;
    }
    if (!rxc_finite_pixels(size) || !(size > 0.0)) return -RXC_ERR_BAD_ARGS;

    const RxSkia *sk = rx_skia();
    const RxHB *hb = rx_hb();
    if (!sk->available || !sk->glyph_render_ok || !hb->available) return -RXC_ERR_NO_SKIA;

    sk_canvas_t *canvas = NULL;
    if (draw) {
        canvas = rx_host_canvas(h);
        if (!canvas) return -RXC_ERR_NO_SKIA;
    }

    int scale = (int)(size * 64.0 + 0.5);

    /* HarfBuzz: blob -> face -> font, shape the buffer. */
    hb_blob_t *blob = hb->blob_create_from_file(font_path);
    if (!blob) return -RXC_ERR_BAD_ARGS;        /* unreadable / not a font file */
    hb_face_t *face = hb->face_create(blob, 0);
    hb_font_t *hfont = face ? hb->font_create(face) : NULL;
    if (!hfont) {
        if (face) hb->face_destroy(face);
        hb->blob_destroy(blob);
        return -RXC_ERR_BAD_ARGS;
    }
    hb->font_set_scale(hfont, scale, scale);

    hb_buffer_t *buf = hb->buffer_create();
    if (!buf) {
        hb->font_destroy(hfont); hb->face_destroy(face); hb->blob_destroy(blob);
        return -RXC_ERR_BAD_ARGS;
    }
    hb->buffer_add_utf8(buf, text, -1, 0, -1);
    int hbdir = rxc_hb_direction(dir);
    if (hbdir != RX_HB_DIR_INVALID) hb->buffer_set_direction(buf, hbdir);
    hb->buffer_guess_segment_properties(buf);   /* fills script/lang (+ dir if auto) */
    hb->shape(hfont, buf, NULL, 0);

    unsigned int n = hb->buffer_get_length(buf);
    hb_glyph_info_t *gi = hb->buffer_get_glyph_infos(buf, NULL);
    hb_glyph_position_t *gp = hb->buffer_get_glyph_positions(buf, NULL);

    /* total advance in pixels (the shaped run width). */
    double total_adv = 0.0;
    for (unsigned int i = 0; i < n; i++) total_adv += gp[i].x_advance / 64.0;
    int64_t width_px = (int64_t)(total_adv + 0.5);

    int64_t result = width_px;

    if (draw && n > 0) {
        /* Skia: the SAME font file as a typeface + a size-set font. */
        sk_typeface_t *tf = sk->typeface_create_from_file(font_path, 0);
        sk_font_t *skf = sk->font_new_with_values(tf, (float)size, 1.0f, 0.0f);
        if (skf && sk->textblob_builder_new) {
            sk_textblob_builder_t *b = sk->textblob_builder_new();
            if (b) {
                sk_textblob_runbuffer_t rb;
                sk->textblob_builder_alloc_run_pos(b, skf, (int)n, NULL, &rb);
                uint16_t *gout = (uint16_t *)rb.glyphs;
                float    *pout = (float *)rb.pos;
                double penx = 0.0;
                for (unsigned int i = 0; i < n; i++) {
                    gout[i] = (uint16_t)gi[i].codepoint;     /* glyph id from HB */
                    pout[2 * i + 0] = (float)(penx + gp[i].x_offset / 64.0);
                    pout[2 * i + 1] = (float)(-(gp[i].y_offset / 64.0));
                    penx += gp[i].x_advance / 64.0;
                }
                sk_textblob_t *blobt = sk->textblob_builder_make(b);
                sk->textblob_builder_delete(b);
                if (blobt) {
                    sk_paint_t *paint = sk->paint_new();
                    if (paint) {
                        sk->paint_set_antialias(paint, 1);
                        sk->paint_set_style(paint, RX_SK_PAINT_FILL);
                        sk->paint_set_color(paint, (sk_color_t)(uint32_t)argb);
                        sk->canvas_draw_text_blob(canvas, blobt, (float)x, (float)y, paint);
                        sk->paint_delete(paint);
                    }
                    sk->textblob_unref(blobt);
                } else {
                    result = -RXC_ERR_NO_SKIA;
                }
            }
        } else {
            result = -RXC_ERR_NO_SKIA;
        }
        if (skf && sk->font_delete) sk->font_delete(skf);
        if (tf && sk->typeface_unref) sk->typeface_unref(tf);
    }

    hb->buffer_destroy(buf);
    hb->font_destroy(hfont);
    hb->face_destroy(face);
    hb->blob_destroy(blob);
    return result;
}

/* True (1) when text shaping can run: Skia's glyph-render API resolved AND the
 * HarfBuzz shaper loaded. A capability probe (mirrors skia_available). */
int64_t ruxen_canvas_shaping_available(int64_t self) {
    (void)self;
    const RxSkia *sk = rx_skia();
    return (sk->available && sk->glyph_render_ok && rx_hb()->available) ? 1 : 0;
}

/* Draw one HarfBuzz-shaped run of `text` in the font FILE `font_path` at `size`
 * px, baseline origin (x, y), color packed 0xAARRGGBB in `argb`. `direction`:
 * 0 auto / 1 LTR / 2 RTL. Returns the shaped run's advance WIDTH in pixels
 * (>= 0), or a NEGATIVE -RXC_ERR_* on failure (Skia+HarfBuzz-only). */
int64_t ruxen_canvas_draw_text_shaped(int64_t self, int64_t text, double x, double y,
                                      double size, int64_t font_path, int64_t direction,
                                      int64_t argb) {
    return rx_shape_run((RxHost *)self, (const char *)text, x, y, size,
                        (const char *)font_path, direction, argb, /*draw*/1);
}

/* Advance WIDTH in pixels of a HarfBuzz-shaped run (kerning/ligatures applied),
 * without drawing — for layout/centering. `direction`: 0 auto / 1 LTR / 2 RTL.
 * Negative -RXC_ERR_* on failure (Skia+HarfBuzz-only). */
int64_t ruxen_canvas_measure_text_shaped(int64_t self, int64_t text, double size,
                                         int64_t font_path, int64_t direction) {
    return rx_shape_run((RxHost *)self, (const char *)text, 0.0, 0.0, size,
                        (const char *)font_path, direction, 0, /*draw*/0);
}

/* ---- shaped paragraphs: word-wrap with SHAPED line widths + glyph render ----
 *
 * The same greedy whitespace word-wrap as ruxen_canvas_draw_paragraph, but each
 * line's width (for the wrap decision + alignment) is the HarfBuzz-SHAPED advance
 * (kerning/ligatures), and each wrapped line is rendered through the shaped glyph
 * path. So wrapping and alignment of e.g. a line with "ffi"/"AV" differ from the
 * naive per-char path. ONE font (by file path), greedy whitespace wrap (no ICU
 * line-break this round); Skia+HarfBuzz-only (clean Err when absent — callers
 * fall back to the non-shaped draw_paragraph). The non-shaped path is untouched.
 *
 * Direction (0 auto / 1 LTR / 2 RTL) applies to each shaped line. */

/* Draw a word-wrapped paragraph with SHAPED lines (kerning/ligatures) at the
 * given alignment (0 left / 1 center / 2 right). (x, y) is the column left edge
 * + first baseline; lines stack one line-height below. Returns the laid-out
 * TOTAL HEIGHT in pixels, or a NEGATIVE -RXC_ERR_* on failure. */
int64_t ruxen_canvas_draw_paragraph_shaped(int64_t self, int64_t text, double x, double y,
        double max_width, double size, int64_t font_path, int64_t align, int64_t direction,
        int64_t argb) {
    RxHost *h = (RxHost *)self;
    const char *s = (const char *)text;
    const char *fp = (const char *)font_path;
    if (!h || !s) return -RXC_ERR_BAD_ARGS;
    if (!h->in_frame) return -RXC_ERR_NO_FRAME;
    if (!rxc_finite_pixels(x) || !rxc_finite_pixels(y) || !rxc_finite_pixels(max_width)) {
        return -RXC_ERR_BAD_ARGS;
    }
    if (!rxc_finite_pixels(size) || !(size > 0.0)) return -RXC_ERR_BAD_ARGS;
    if (align < 0 || align > RXC_ALIGN_RIGHT) return -RXC_ERR_BAD_ARGS;

    sk_canvas_t *canvas = rx_host_canvas(h);
    if (!canvas) return -RXC_ERR_NO_SKIA;

    RxShaper shaper;
    rx_shaper_init(&shaper, fp, size, direction);
    if (!shaper.ok) { rx_shaper_drop(&shaper); return -RXC_ERR_NO_SKIA; }

    int n = rx_paragraph_layout(h, shaper.sk, shaper.skf, s, x, y, max_width,
                                shaper.line_height, (int)align, argb,
                                /*draw*/1, NULL, &shaper);
    int lh = shaper.line_height;
    rx_shaper_drop(&shaper);
    if (n < 0) return -RXC_ERR_BAD_ARGS;
    return (int64_t)n * lh;
}

/* Measure a shaped word-wrapped paragraph WITHOUT drawing: returns
 * (max_shaped_line_width << 32) | total_height. NEGATIVE -RXC_ERR_* on failure. */
int64_t ruxen_canvas_measure_paragraph_shaped(int64_t self, int64_t text, double max_width,
        double size, int64_t font_path, int64_t direction) {
    (void)self;
    const char *s = (const char *)text;
    const char *fp = (const char *)font_path;
    if (!s) return -RXC_ERR_BAD_ARGS;
    if (!rxc_finite_pixels(max_width)) return -RXC_ERR_BAD_ARGS;
    if (!rxc_finite_pixels(size) || !(size > 0.0)) return -RXC_ERR_BAD_ARGS;

    RxShaper shaper;
    rx_shaper_init(&shaper, fp, size, direction);
    if (!shaper.ok) { rx_shaper_drop(&shaper); return -RXC_ERR_NO_SKIA; }

    int max_w = 0;
    int n = rx_paragraph_layout(NULL, shaper.sk, shaper.skf, s, 0.0, 0.0, max_width,
                                shaper.line_height, RXC_ALIGN_LEFT, 0,
                                /*draw*/0, &max_w, &shaper);
    int lh = shaper.line_height;
    rx_shaper_drop(&shaper);
    if (n < 0) return -RXC_ERR_BAD_ARGS;
    int64_t height = (int64_t)n * lh;
    return ((int64_t)(uint32_t)max_w << 32) | (int64_t)(uint32_t)height;
}

/* ---- event queue ---- */
/* The platform pump (sdl_window.c) and the tests push events in; the Ruxen
 * side polls them out one at a time. poll pops the next event into the
 * `pending` slot and returns its kind (-1 when the queue is empty); the
 * payload accessors then read from `pending`. */

/* FileDrop event-kind tag — must match RX_EV_FILE_DROP in sdl_window.c and the
 * FileDrop variant's tag in src/event.rx (the highest tag, RXC_EVENT_KIND_MAX). */
#define RX_EV_FILE_DROP 9

int64_t ruxen_canvas_push_event(int64_t self, int64_t kind, double a, double b) {
    RxHost *h = (RxHost *)self;
    if (!h || kind < 0 || kind > RXC_EVENT_KIND_MAX) return RXC_ERR_BAD_ARGS;
    if (h->ev_count >= RXC_EVENT_CAP) return RXC_ERR_QUEUE_FULL;
    int32_t tail = (h->ev_head + h->ev_count) % RXC_EVENT_CAP;
    /* a previously-polled slot may still alias a drop_path it no longer owns
     * (poll moves ownership to `pending` and NULLs the slot, so this is normally
     * already NULL — but free defensively to never leak on an unexpected path). */
    if (h->events[tail].drop_path) { free(h->events[tail].drop_path); h->events[tail].drop_path = NULL; }
    h->events[tail].kind = (int32_t)kind;
    h->events[tail].a = a;
    h->events[tail].b = b;
    h->events[tail].text[0] = '\0';   /* no marked text (clear stale slot content) */
    h->events[tail].mods = 0;         /* no key modifiers (clear stale slot content) */
    h->ev_count++;
    return RXC_OK;
}

/* Push a KeyDown carrying its keyboard MODIFIER bitfield (RX_MOD_*). The live
 * pump (sdl_window.c) reads SDL_GetModState() and the keydown test seam passes a
 * synthetic mask; both funnel here so the mods side-channel rides the SAME ring
 * slot as the keycode. Identical to push_event except it also stamps `mods`. */
int64_t ruxen_canvas_push_event_mods(int64_t self, int64_t kind, double a, double b,
                                     int64_t mods) {
    RxHost *h = (RxHost *)self;
    if (!h || kind < 0 || kind > RXC_EVENT_KIND_MAX) return RXC_ERR_BAD_ARGS;
    if (h->ev_count >= RXC_EVENT_CAP) return RXC_ERR_QUEUE_FULL;
    int32_t tail = (h->ev_head + h->ev_count) % RXC_EVENT_CAP;
    if (h->events[tail].drop_path) { free(h->events[tail].drop_path); h->events[tail].drop_path = NULL; }
    h->events[tail].kind = (int32_t)kind;
    h->events[tail].a = a;
    h->events[tail].b = b;
    h->events[tail].text[0] = '\0';
    h->events[tail].mods = (int32_t)mods;
    h->ev_count++;
    return RXC_OK;
}

/* Push an IME composition (TextEditing) event carrying the marked composition
 * TEXT alongside the (start, length) cursor/selection. The text is a
 * NUL-terminated UTF-8 C string (an SDL marked-text chunk, or a Ruxen &String in
 * the test seam); it is COPIED into the ring slot (truncated to RXC_EVENT_TEXT_CAP-1
 * bytes — SDL caps composition chunks at 32 bytes anyway), so no pointer outlives
 * the call (never a dangling SDL buffer). kind must be the TextEditing tag. */
int64_t ruxen_canvas_push_event_text(int64_t self, int64_t kind, int64_t start,
                                     int64_t length, int64_t text_ptr) {
    RxHost *h = (RxHost *)self;
    const char *t = (const char *)text_ptr;
    if (!h || kind < 0 || kind > RXC_EVENT_KIND_MAX) return RXC_ERR_BAD_ARGS;
    if (h->ev_count >= RXC_EVENT_CAP) return RXC_ERR_QUEUE_FULL;
    int32_t tail = (h->ev_head + h->ev_count) % RXC_EVENT_CAP;
    /* free any stale drop_path this slot still aliases (see push_event). */
    if (h->events[tail].drop_path) { free(h->events[tail].drop_path); h->events[tail].drop_path = NULL; }
    h->events[tail].kind = (int32_t)kind;
    h->events[tail].a = (double)start;
    h->events[tail].b = (double)length;
    if (kind == RX_EV_FILE_DROP) {
        /* A FILE PATH — routinely longer than the inline cap, so it gets its own
         * owned heap copy (drop_path). The inline `text` stays empty so the IME
         * accessor never returns a path. strdup may fail (OOM) → store NULL, the
         * accessor then returns "" (a dropped-but-pathless event, never a crash). */
        h->events[tail].text[0] = '\0';
        h->events[tail].drop_path = (t && t[0]) ? strdup(t) : NULL;
    } else if (t) {
        /* IME marked text: copied inline, capped at 32 bytes (SDL's composition
         * cap), exactly as before. */
        size_t n = strlen(t);
        if (n >= RXC_EVENT_TEXT_CAP) n = RXC_EVENT_TEXT_CAP - 1;
        memcpy(h->events[tail].text, t, n);
        h->events[tail].text[n] = '\0';
    } else {
        h->events[tail].text[0] = '\0';
    }
    h->events[tail].mods = 0;   /* text events carry no key modifiers */
    h->ev_count++;
    return RXC_OK;
}

/* The marked composition TEXT of the most-recently-polled event (a Ruxen-owned
 * String via ruxen_string_from). Empty string for non-TextEditing events. The
 * text was copied into `pending` at poll time, so this never dereferences a
 * foreign/stale pointer. */
int64_t ruxen_canvas_event_text(int64_t self) {
    RxHost *h = (RxHost *)self;
    return (int64_t)ruxen_string_from(h ? h->pending.text : "");
}

int64_t ruxen_canvas_poll_event(int64_t self) {
    RxHost *h = (RxHost *)self;
    if (!h || h->ev_count == 0) return -1;
    /* MOVE the dropped-path ownership from the ring slot into `pending`: free the
     * path the PREVIOUS poll left in `pending`, struct-copy the slot (which aliases
     * its drop_path into pending), then NULL the slot so only `pending` owns it.
     * Result: exactly one owner at all times, freed on the next poll or host_drop. */
    if (h->pending.drop_path) { free(h->pending.drop_path); h->pending.drop_path = NULL; }
    int32_t head = h->ev_head;
    h->pending = h->events[head];
    h->events[head].drop_path = NULL;   /* ownership moved to pending */
    h->ev_head = (h->ev_head + 1) % RXC_EVENT_CAP;
    h->ev_count--;
    return h->pending.kind;
}

/* The dropped FILE PATH of the most-recently-polled event (a Ruxen-owned String
 * via ruxen_string_from). Empty string for any non-FileDrop event (or a drop with
 * no path). The path was MOVED into `pending` at poll time (owned heap copy), so
 * this never dereferences a foreign/stale SDL pointer. Read it immediately after
 * polling an Event.FileDrop, before the next poll frees it. */
int64_t ruxen_canvas_event_drop_path(int64_t self) {
    RxHost *h = (RxHost *)self;
    const char *p = (h && h->pending.drop_path) ? h->pending.drop_path : "";
    return (int64_t)ruxen_string_from(p);
}

/* The keyboard MODIFIER bitfield (RX_MOD_*) of the most-recently-polled event.
 * Non-zero only for a KeyDown whose modifiers were set (shift/ctrl/alt/gui); 0
 * for every other event kind and for a plain unmodified key. Read it right after
 * polling an Event.KeyDown, the same side-channel discipline as event_drop_path /
 * event_text. */
int64_t ruxen_canvas_event_mods(int64_t self) {
    RxHost *h = (RxHost *)self;
    return h ? (int64_t)h->pending.mods : 0;
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

/* Monotonic nanosecond clock for the engine timebase (docs/decisions/
 * frame-pacing.md). CLOCK_MONOTONIC never jumps backward (NTP/DST-immune), so a
 * frame delta is always >= 0. The epoch is unspecified-but-fixed for the process;
 * only differences are meaningful — which is all an animation/frame clock needs.
 * int64 nanoseconds does not overflow for ~292 years. The handle is accepted-and-
 * ignored (the clock is process-wide, like sleep_ms). */
int64_t ruxen_canvas_ticks_ns(int64_t self) {
    (void)self;
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

/* Paced-present wait to an ABSOLUTE monotonic target tick (docs/decisions/
 * frame-pacing.md). Sleeps remaining = target_ns - now; returns immediately when
 * the frame already overran (remaining <= 0) — pacing fills the slack of an early
 * frame, it never ADDS latency to a late one. The absolute target makes the
 * cadence self-correcting (the caller advances target += interval each frame, so
 * early/late frames converge back onto the grid with no drift). nanosleep
 * guarantees a MINIMUM sleep; a few hundred µs of scheduler over-sleep is correct
 * for a 2D UI (we never want to present BEFORE the boundary). On the Metal/GL
 * on-screen paths the present already blocks on vsync, so remaining is <= 0 and
 * this is a no-op-by-design there; it is the software clock for raster/headless. */
void ruxen_canvas_wait_until_ns(int64_t self, int64_t target_ns) {
    (void)self;
    int64_t now = ruxen_canvas_ticks_ns(0);
    int64_t remaining = target_ns - now;
    if (remaining <= 0) return;
    struct timespec ts;
    ts.tv_sec  = (time_t)(remaining / 1000000000LL);
    ts.tv_nsec = (long)(remaining % 1000000000LL);
    nanosleep(&ts, NULL);
}
