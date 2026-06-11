/*
 * skia_capi.h — the minimal slice of Skia's flat C API that the canvas shim
 * binds, plus the runtime loader struct.
 *
 * Skia is not linked: runtime/skia_shim.c dlopen()s libSkiaSharp.so (fetched
 * by runtime/fetch_skia.sh) and resolves these symbols at runtime, exactly as
 * sdl_window.c does for SDL2. So this header declares only ABI shapes (opaque
 * types, enums, structs, function-pointer typedefs) — never `extern`
 * prototypes that would create a link dependency.
 *
 * The full upstream `include/c/sk_*.h` set is large and was removed from
 * Skia's mainline; rather than vendor it, this is a hand-authored subset whose
 * struct layouts and enum values are pinned by an empirical ABI probe (a red
 * rect drawn into a 0xAARRGGBB raster-direct buffer reads back byte-exact —
 * see docs/SKIA.md). Keep it tiny: add only what the shim actually calls.
 */
#ifndef RX_SKIA_CAPI_H
#define RX_SKIA_CAPI_H

#include <stdint.h>
#include <stddef.h>

/* ---- opaque handles ---- */
typedef struct sk_surface_t    sk_surface_t;
typedef struct sk_canvas_t     sk_canvas_t;
typedef struct sk_paint_t      sk_paint_t;
typedef struct sk_rrect_t      sk_rrect_t;
typedef struct sk_shader_t     sk_shader_t;
typedef struct sk_maskfilter_t sk_maskfilter_t;
typedef struct sk_image_t      sk_image_t;
typedef struct sk_data_t       sk_data_t;
typedef struct sk_font_t       sk_font_t;
typedef struct sk_typeface_t   sk_typeface_t;
typedef struct sk_colorspace_t sk_colorspace_t;
typedef struct sk_path_t       sk_path_t;
typedef struct sk_fontstyle_t  sk_fontstyle_t;
typedef struct sk_textblob_t         sk_textblob_t;
typedef struct sk_textblob_builder_t sk_textblob_builder_t;

/* ---- Ganesh (GPU) opaque handles ----
 * The GPU backend (docs/GPU.md). A gr_direct_context drives a GL/Metal/Vulkan
 * device; a gr_backendrendertarget wraps the window's default framebuffer; the
 * GPU SkSurface renders into it. All reached through symbols the prebuilt
 * libSkiaSharp already exports — no new dependency. GL-first: only the gl_*
 * entry points are bound this cycle (Metal/Vulkan deferred per the ADR). */
typedef struct gr_direct_context_t    gr_direct_context_t;
typedef struct gr_recording_context_t gr_recording_context_t;
typedef struct gr_glinterface_t       gr_glinterface_t;
typedef struct gr_backendrendertarget_t gr_backendrendertarget_t;

/* Packed 0xAARRGGBB, matching our framebuffer + RxHost.pixels. */
typedef uint32_t sk_color_t;

/* ---- enums (values ABI-pinned by the probe) ---- */
/* sk_colortype_t: our buffer is little-endian 0xAARRGGBB == bytes B,G,R,A. */
enum { RX_SK_COLORTYPE_BGRA_8888 = 6 };
/* sk_alphatype_t */
enum { RX_SK_ALPHA_OPAQUE = 1, RX_SK_ALPHA_PREMUL = 2, RX_SK_ALPHA_UNPREMUL = 3 };
/* sk_paint_style_t */
enum { RX_SK_PAINT_FILL = 0, RX_SK_PAINT_STROKE = 1, RX_SK_PAINT_STROKE_AND_FILL = 2 };
/* sk_text_encoding_t */
enum { RX_SK_TEXT_UTF8 = 0, RX_SK_TEXT_UTF16 = 1, RX_SK_TEXT_UTF32 = 2, RX_SK_TEXT_GLYPH = 3 };
/* sk_shader_tilemode_t */
enum { RX_SK_TILE_CLAMP = 0, RX_SK_TILE_REPEAT = 1, RX_SK_TILE_MIRROR = 2, RX_SK_TILE_DECAL = 3 };
/* sk_blurstyle_t */
enum { RX_SK_BLUR_NORMAL = 0, RX_SK_BLUR_SOLID = 1, RX_SK_BLUR_OUTER = 2, RX_SK_BLUR_INNER = 3 };
/* sk_clipop_t (probe-pinned: 1 = intersect) */
enum { RX_SK_CLIP_DIFFERENCE = 0, RX_SK_CLIP_INTERSECT = 1 };
/* sk_filter_mode_t */
enum { RX_SK_FILTER_NEAREST = 0, RX_SK_FILTER_LINEAR = 1 };
/* sk_path_filltype_t (probe-pinned by upstream SkiaSharp: winding = 0). */
enum { RX_SK_PATH_FILL_WINDING = 0, RX_SK_PATH_FILL_EVENODD = 1 };
/* arc_to large-arc / sweep flags (SVG semantics) */
enum { RX_SK_PATH_ARC_SMALL = 0, RX_SK_PATH_ARC_LARGE = 1 };
enum { RX_SK_PATH_ARC_CCW = 0, RX_SK_PATH_ARC_CW = 1 };

/* gr_surfaceorigin_t — a window's GL default framebuffer is bottom-left
 * origin (GL convention); an FBO-backed texture is top-left. */
enum { RX_GR_SURFACE_ORIGIN_TOP_LEFT = 0, RX_GR_SURFACE_ORIGIN_BOTTOM_LEFT = 1 };
/* The GL sized internal format we request for the window backbuffer color
 * attachment. GL_RGBA8 == 0x8058; Skia maps the render-target color type from
 * it. We keep the GPU surface at RGBA_8888 (not BGRA) because that is the GL
 * default-framebuffer format; color is converted by Skia at draw, and present
 * is a swap (no CPU readback in the present path), so this does not need to
 * match the raster path's BGRA buffer. */
enum { RX_GR_GL_RGBA8 = 0x8058 };
/* sk_colortype_t for the GPU surface — RGBA_8888 == 4 (BGRA_8888 == 6 above). */
enum { RX_SK_COLORTYPE_RGBA_8888 = 4 };

/* Which GPU backend a host's surface is rendered by — the gpu_backend_kind slot.
 * 0 when the host is on the raster/software path. Probed via
 * ruxen_canvas_gpu_backend_kind so L2/tests can tell GL from Metal. */
enum { RX_GPU_KIND_NONE = 0, RX_GPU_KIND_GL = 1, RX_GPU_KIND_METAL = 2 };

/* ---- by-value structs (layout ABI-pinned) ---- */
typedef struct {
    sk_colorspace_t *colorspace;   /* NULL = sRGB/unmanaged */
    int32_t          width;
    int32_t          height;
    int32_t          colorType;    /* sk_colortype_t */
    int32_t          alphaType;    /* sk_alphatype_t */
} sk_imageinfo_t;

typedef struct { float left, top, right, bottom; } sk_rect_t;
typedef struct { float x, y; } sk_vector_t;   /* a per-corner (x,y) radius */
typedef struct { float x, y; } sk_point_t;    /* a gradient endpoint / center */

/* sk_sampling_options_t — layout ABI-pinned by probe (24 bytes). The leading
 * max_aniso field is REQUIRED: without it the fields shift and `filter` is
 * misread, silently degrading scaling to nearest-neighbor (verified — the
 * 20-byte form drew blocky; this one interpolates). A NULL sampling crashes
 * draw_image_rect, so we always pass this with filter = linear. */
typedef struct {
    int32_t max_aniso;
    int32_t use_cubic;
    float   cubic_b, cubic_c;
    int32_t filter;   /* sk_filter_mode_t */
    int32_t mipmap;   /* sk_mipmap_mode_t (0 = none) */
} sk_sampling_options_t;

/* Full sk_fontmetrics_t so &metrics has the right size; we read ascent/descent. */
typedef struct {
    uint32_t flags;
    float top, ascent, descent, bottom, leading;
    float avg_char_width, max_char_width, x_min, x_max, x_height, cap_height;
    float underline_thickness, underline_position;
    float strikeout_thickness, strikeout_position;
} sk_fontmetrics_t;

/* ---- function-pointer typedefs ---- */
typedef sk_surface_t *(*pfn_surface_new_raster_direct)(const sk_imageinfo_t *, void *pixels,
        size_t row_bytes, void *release_proc, void *context, const void *props);
typedef sk_canvas_t  *(*pfn_surface_get_canvas)(sk_surface_t *);
typedef void          (*pfn_surface_unref)(sk_surface_t *);

typedef void (*pfn_canvas_clear)(sk_canvas_t *, sk_color_t);
typedef void (*pfn_canvas_draw_rect)(sk_canvas_t *, const sk_rect_t *, const sk_paint_t *);
typedef void (*pfn_canvas_draw_oval)(sk_canvas_t *, const sk_rect_t *, const sk_paint_t *);
typedef void (*pfn_canvas_draw_round_rect)(sk_canvas_t *, const sk_rect_t *, float rx, float ry,
        const sk_paint_t *);
typedef void (*pfn_canvas_draw_line)(sk_canvas_t *, float x0, float y0, float x1, float y1,
        const sk_paint_t *);
typedef void (*pfn_canvas_draw_rrect)(sk_canvas_t *, const sk_rrect_t *, const sk_paint_t *);
typedef void (*pfn_canvas_draw_simple_text)(sk_canvas_t *, const void *text, size_t byte_len,
        int encoding, float x, float y, const sk_font_t *, const sk_paint_t *);

/* canvas state stack + transforms + clipping */
typedef int  (*pfn_canvas_save)(sk_canvas_t *);
typedef void (*pfn_canvas_restore)(sk_canvas_t *);
/* save_layer pushes an offscreen layer (bounds NULL = whole canvas); the
 * matching restore composites it down. Returns the save count (>= 1) to pair
 * with restore / restore_to. Group opacity is done via save_layer with an
 * alpha-carrying paint (this build has no sk_canvas_save_layer_alpha — the
 * convenience wrapper was removed upstream; see ruxen_canvas_save_layer_alpha). */
typedef int  (*pfn_canvas_save_layer)(sk_canvas_t *, const sk_rect_t *bounds,
        const sk_paint_t *paint);
typedef int  (*pfn_canvas_get_save_count)(sk_canvas_t *);
typedef void (*pfn_canvas_restore_to_count)(sk_canvas_t *, int save_count);
typedef void (*pfn_canvas_translate)(sk_canvas_t *, float dx, float dy);
typedef void (*pfn_canvas_scale)(sk_canvas_t *, float sx, float sy);
typedef void (*pfn_canvas_rotate_degrees)(sk_canvas_t *, float degrees);
typedef void (*pfn_canvas_reset_matrix)(sk_canvas_t *);
typedef void (*pfn_canvas_skew)(sk_canvas_t *, float sx, float sy);
/* sk_canvas_concat in THIS libSkiaSharp build takes a 4x4 SkM44 — sixteen floats
 * in COLUMN-MAJOR order, NOT a 3x3 sk_matrix_t. Empirically pinned (see the shim
 * comment on ruxen_canvas_concat): sk_canvas_get_matrix round-trips through
 * sk_canvas_concat only as 16 column-major floats, with the 2D affine occupying:
 *   col0 = (scaleX, skewY,  0, 0)
 *   col1 = (skewX,  scaleY, 0, 0)
 *   col2 = (0,      0,      1, 0)
 *   col3 = (transX, transY, 0, 1)
 * i.e. scaleX@[0] scaleY@[5] transX@[12] transY@[13] skewY@[1] skewX@[4]. The
 * 6-arg Canvas#concat fills these slots; all other entries are the 4x4 identity. */
typedef void (*pfn_canvas_concat)(sk_canvas_t *, const float m44_colmajor[16]);
/* clip op: 1 = intersect (ABI-pinned by probe); doAA != 0 for antialiased edges */
typedef void (*pfn_canvas_clip_rect)(sk_canvas_t *, const sk_rect_t *, int op, int do_aa);
typedef void (*pfn_canvas_clip_rrect)(sk_canvas_t *, const sk_rrect_t *, int op, int do_aa);

/* images: decode encoded bytes (PNG/JPEG/WebP) -> sk_image, draw scaled */
typedef sk_data_t  *(*pfn_data_new_from_file)(const char *path);
typedef void        (*pfn_data_unref)(sk_data_t *);
typedef sk_image_t *(*pfn_image_new_from_encoded)(sk_data_t *);
typedef int         (*pfn_image_get_width)(const sk_image_t *);
typedef int         (*pfn_image_get_height)(const sk_image_t *);
typedef void        (*pfn_image_unref)(sk_image_t *);
typedef void        (*pfn_canvas_draw_image_rect)(sk_canvas_t *, const sk_image_t *,
        const sk_rect_t *src, const sk_rect_t *dst, const sk_sampling_options_t *,
        const sk_paint_t *, int constraint);

/* radii[4] order: upper-left, upper-right, lower-right, lower-left. */
typedef sk_rrect_t *(*pfn_rrect_new)(void);
typedef void        (*pfn_rrect_delete)(sk_rrect_t *);
typedef void        (*pfn_rrect_set_rect_radii)(sk_rrect_t *, const sk_rect_t *,
        const sk_vector_t radii[4]);

typedef sk_paint_t *(*pfn_paint_new)(void);
typedef void        (*pfn_paint_delete)(sk_paint_t *);
typedef void        (*pfn_paint_set_color)(sk_paint_t *, sk_color_t);
typedef void        (*pfn_paint_set_antialias)(sk_paint_t *, int /*bool*/);
typedef void        (*pfn_paint_set_style)(sk_paint_t *, int /*sk_paint_style_t*/);
typedef void        (*pfn_paint_set_stroke_width)(sk_paint_t *, float);
typedef void        (*pfn_paint_set_shader)(sk_paint_t *, sk_shader_t *);
typedef void        (*pfn_paint_set_maskfilter)(sk_paint_t *, sk_maskfilter_t *);

/* colors[count] packed 0xAARRGGBB; pos[count] in [0,1] or NULL for even spacing;
 * matrix NULL = identity. */
typedef sk_shader_t *(*pfn_shader_new_linear_gradient)(const sk_point_t pts[2],
        const sk_color_t colors[], const float pos[], int count, int tile_mode,
        const void *local_matrix);
typedef sk_shader_t *(*pfn_shader_new_radial_gradient)(const sk_point_t *center, float radius,
        const sk_color_t colors[], const float pos[], int count, int tile_mode,
        const void *local_matrix);
typedef void         (*pfn_shader_unref)(sk_shader_t *);
typedef sk_maskfilter_t *(*pfn_maskfilter_new_blur)(int blur_style, float sigma);
typedef void             (*pfn_maskfilter_unref)(sk_maskfilter_t *);

typedef sk_typeface_t *(*pfn_typeface_create_default)(void);
/* Resolve a typeface by family name + style; NULL when the family is unknown
 * (the shim then falls back to the default face — a missing font must not break
 * rendering). The returned typeface is owned (release with typeface_unref). */
typedef sk_typeface_t *(*pfn_typeface_create_from_name)(const char *family_name,
        const sk_fontstyle_t *style);
typedef void           (*pfn_typeface_unref)(sk_typeface_t *);
/* weight/width are SkFontStyle ints (400 = normal weight, 5 = normal width);
 * slant 0 = upright. Owned: release with fontstyle_delete. */
typedef sk_fontstyle_t *(*pfn_fontstyle_new)(int weight, int width, int slant);
typedef void            (*pfn_fontstyle_delete)(sk_fontstyle_t *);
typedef sk_font_t     *(*pfn_font_new_with_values)(sk_typeface_t *, float size, float scale_x,
        float skew_x);
typedef void           (*pfn_font_set_size)(sk_font_t *, float);
typedef void           (*pfn_font_delete)(sk_font_t *);
typedef float          (*pfn_font_measure_text)(const sk_font_t *, const void *text, size_t byte_len,
        int encoding, sk_rect_t *bounds, const sk_paint_t *);
typedef float          (*pfn_font_get_metrics)(const sk_font_t *, sk_fontmetrics_t *);

/* ---- paths (arbitrary contours: lines, béziers, arcs) ----
 * An sk_path is a mutable builder owned on the Ruxen side as an int64 handle;
 * the shim appends verbs to it, then sk_canvas_draw_path fills/strokes it with
 * a paint. arc_to uses SVG semantics: (rx, ry, x-axis-rotate-deg, large-arc,
 * sweep, x, y). All coordinates are device pixels. */
typedef sk_path_t *(*pfn_path_new)(void);
typedef void       (*pfn_path_delete)(sk_path_t *);
typedef void       (*pfn_path_move_to)(sk_path_t *, float x, float y);
typedef void       (*pfn_path_line_to)(sk_path_t *, float x, float y);
typedef void       (*pfn_path_quad_to)(sk_path_t *, float cx, float cy, float x, float y);
typedef void       (*pfn_path_cubic_to)(sk_path_t *, float c1x, float c1y,
        float c2x, float c2y, float x, float y);
typedef void       (*pfn_path_arc_to)(sk_path_t *, float rx, float ry, float x_axis_rotate,
        int large_arc, int sweep, float x, float y);
typedef void       (*pfn_path_close)(sk_path_t *);
typedef void       (*pfn_path_set_filltype)(sk_path_t *, int /*sk_path_filltype_t*/);
typedef void       (*pfn_canvas_draw_path)(sk_canvas_t *, const sk_path_t *, const sk_paint_t *);

/* ---- Ganesh GL backend (the GPU surface, docs/GPU.md) ----
 *
 * These are the gr_* / sk_surface_new_backend_render_target symbols the prebuilt
 * libSkiaSharp exports (Skia's include/c/gr_context.h + sk_surface.h C API). All
 * are OPTIONAL in the loader: a miss disables only the GPU rung, never the
 * raster backend. ABI shapes below are the upstream SkiaSharp C-API ones.
 *
 * gr_gl_framebufferinfo_t describes the GL default-framebuffer the window's GL
 * context draws into: { fboid, format }. fboid is the bound FBO (0 on most
 * desktop GL); format is the sized internal color format (GL_RGBA8). Some
 * SkiaSharp builds add a trailing `protected` bool — we over-allocate by a few
 * bytes at the call site to be safe rather than risk a short read. */
typedef struct {
    unsigned int fFBOID;
    unsigned int fFormat;
} gr_gl_framebufferinfo_t;

/* The GL proc loader callback Skia calls to resolve each GL entry point. ctx is
 * an opaque user pointer (we pass NULL and resolve via SDL_GL_GetProcAddress in
 * a small trampoline). */
typedef void *(*gr_gl_get_proc)(void *ctx, const char *name);

/* Assemble a GrGLInterface from a proc loader (preferred — explicit about which
 * context's procs we bind). Returns an owned interface (gr_glinterface_unref).
 * gr_glinterface_create_native_interface() is the no-callback alternative that
 * uses the current context's procs; we try assemble first, then native. */
typedef gr_glinterface_t *(*pfn_gr_glinterface_assemble_gl)(void *ctx, gr_gl_get_proc get);
typedef gr_glinterface_t *(*pfn_gr_glinterface_create_native)(void);
typedef void              (*pfn_gr_glinterface_unref)(gr_glinterface_t *);

/* Build a GPU device context over the (current) GL context described by the
 * interface. NULL on failure.
 *
 * Releasing it: there is NO `gr_direct_context_unref` in this Skia C API — a
 * GrDirectContext is-a GrRecordingContext, and the canonical release is
 * `gr_recording_context_unref(gr_recording_context_t*)` (verified absent vs
 * present by `nm` on the fetched libSkiaSharp; the header confirms it). We
 * upcast the direct context to gr_recording_context_t* to unref it. */
typedef gr_direct_context_t *(*pfn_gr_direct_context_make_gl)(const gr_glinterface_t *);
typedef void                 (*pfn_gr_recording_context_unref)(gr_recording_context_t *);
typedef void                 (*pfn_gr_direct_context_flush)(gr_direct_context_t *);
typedef void                 (*pfn_gr_direct_context_flush_and_submit)(gr_direct_context_t *, int sync);

/* Wrap the window's default framebuffer as a GrBackendRenderTarget. Owned:
 * gr_backendrendertarget_delete. */
typedef gr_backendrendertarget_t *(*pfn_gr_backendrendertarget_new_gl)(int width, int height,
        int samples, int stencils, const gr_gl_framebufferinfo_t *glInfo);
typedef void (*pfn_gr_backendrendertarget_delete)(gr_backendrendertarget_t *);

/* Create a GPU-backed SkSurface that renders into the backend render target.
 * origin = bottom-left for a GL window; colortype = RGBA_8888. NULL on failure.
 * Released with the usual sk_surface_unref. */
typedef sk_surface_t *(*pfn_surface_new_backend_render_target)(gr_recording_context_t *ctx,
        const gr_backendrendertarget_t *rt, int origin, int colortype,
        sk_colorspace_t *cs, const void *props);

/* ---- Ganesh Metal backend + offscreen GPU surface + readback (docs/GPU.md) ----
 *
 * Metal is the native Apple GPU path (this build is SK_METAL=1 — verified). The
 * device + command queue come from Metal.framework / the objc runtime (created
 * in skia_shim.c, not by Skia); gr_direct_context_make_metal consumes them.
 *
 * The headless prize: sk_surface_new_render_target makes an OFFSCREEN GPU
 * surface (Skia allocates its own MTLTexture — no window, no CAMetalLayer), and
 * sk_surface_read_pixels copies the rendered GPU pixels back to CPU memory. With
 * a BGRA_8888 surface the readback is byte-identical to the host's 0xAARRGGBB
 * framebuffer, so the existing read_pixel oracle observes real GPU output. */

/* device + queue are id<MTLDevice> / id<MTLCommandQueue> (opaque). */
typedef gr_direct_context_t *(*pfn_gr_direct_context_make_metal)(void *device, void *queue);

/* Offscreen GPU surface: Skia allocates the render target. budgeted!=0 lets the
 * context recycle it; sampleCount 0 = no MSAA; origin top-left for offscreen;
 * mips!=0 builds mipmaps (0 here). NULL on failure. sk_surface_unref to free. */
typedef sk_surface_t *(*pfn_surface_new_render_target)(gr_recording_context_t *ctx,
        int budgeted, const sk_imageinfo_t *info, int sample_count, int origin,
        const void *props, int should_create_with_mips);

/* ON-SCREEN Metal: wrap a CAMetalDrawable's MTLTexture as a backend render
 * target, so a GPU SkSurface renders directly into the window's drawable
 * (docs/GPU.md). gr_mtl_textureinfo_t is just { const void* fTexture } — the
 * drawable's texture pointer (from window_metal_next_drawable). The resulting
 * render target feeds sk_surface_new_backend_render_target. Released per frame
 * (gr_backendrendertarget_delete) since each frame wraps a fresh drawable. */
typedef struct { const void *fTexture; } gr_mtl_textureinfo_t;
typedef gr_backendrendertarget_t *(*pfn_gr_backendrendertarget_new_metal)(int width, int height,
        const gr_mtl_textureinfo_t *mtlInfo);

/* GPU -> CPU readback: copy the surface's pixels into dst (described by dstInfo)
 * at (srcX, srcY). Returns nonzero on success. */
typedef int (*pfn_surface_read_pixels)(sk_surface_t *surface, sk_imageinfo_t *dst_info,
        void *dst_pixels, size_t dst_row_bytes, int src_x, int src_y);

/* GPU -> CPU readback: copy the surface's pixels into dst (described by dstInfo)
 * at (srcX, srcY). Returns nonzero on success. */
typedef int (*pfn_surface_read_pixels)(sk_surface_t *surface, sk_imageinfo_t *dst_info,
        void *dst_pixels, size_t dst_row_bytes, int src_x, int src_y);

/* ---- positioned-glyph rendering (for shaped text, docs/SHAPING.md) ----
 *
 * libSkiaSharp has no SkShaper, but it DOES expose the textblob API to render a
 * pre-positioned glyph run. We shape with HarfBuzz (below), then build a run of
 * (glyphId, x, y) and draw it. sk_typeface_create_from_file loads the SAME font
 * file HarfBuzz shapes, so the glyph ids match. The runbuffer's `glyphs` is a
 * uint16_t[count] and `pos` is a float[2*count] (x,y per glyph). */
typedef struct {
    void *glyphs;     /* uint16_t[count] — glyph ids */
    void *pos;        /* float[2*count]  — x,y per glyph (for alloc_run_pos) */
    void *utf8text;
    void *clusters;
} sk_textblob_runbuffer_t;

typedef sk_typeface_t *(*pfn_typeface_create_from_file)(const char *path, int index);
typedef sk_textblob_builder_t *(*pfn_textblob_builder_new)(void);
typedef void           (*pfn_textblob_builder_delete)(sk_textblob_builder_t *);
/* Allocate a run of `count` positioned glyphs for `font`; fills *runbuffer with
 * pointers to write glyph ids + (x,y) positions into. bounds may be NULL. */
typedef void           (*pfn_textblob_builder_alloc_run_pos)(sk_textblob_builder_t *,
        const sk_font_t *font, int count, const sk_rect_t *bounds,
        sk_textblob_runbuffer_t *runbuffer);
typedef sk_textblob_t *(*pfn_textblob_builder_make)(sk_textblob_builder_t *);
typedef void           (*pfn_textblob_unref)(sk_textblob_t *);
typedef void           (*pfn_canvas_draw_text_blob)(sk_canvas_t *, sk_textblob_t *,
        float x, float y, const sk_paint_t *);

/* ---- HarfBuzz shaping (the hb_* flat C API, docs/SHAPING.md) ----
 *
 * Shape a UTF-8 run into positioned glyphs (kerning, ligatures; and RTL/complex
 * when direction/script are set). dlopen'd from libHarfBuzzSharp; bound as an
 * OPTIONAL tier (a miss only forecloses shaping). All handles are opaque
 * pointers; the glyph info/position structs are HarfBuzz's stable ABI (5x uint32
 * / int32 each). hb advances/offsets are in font units scaled by hb_font_set_scale
 * — we set scale = size*64, so values are 26.6 fixed (divide by 64 for pixels). */
typedef struct hb_blob_t   hb_blob_t;
typedef struct hb_face_t   hb_face_t;
typedef struct hb_font_t   hb_font_t;
typedef struct hb_buffer_t hb_buffer_t;
typedef struct { uint32_t codepoint, mask, cluster, var1, var2; } hb_glyph_info_t;
typedef struct { int32_t x_advance, y_advance, x_offset, y_offset; uint32_t var; } hb_glyph_position_t;

typedef hb_blob_t   *(*pfn_hb_blob_create_from_file_or_fail)(const char *path);
typedef hb_face_t   *(*pfn_hb_face_create)(hb_blob_t *, unsigned int index);
typedef hb_font_t   *(*pfn_hb_font_create)(hb_face_t *);
typedef void         (*pfn_hb_font_set_scale)(hb_font_t *, int x_scale, int y_scale);
typedef hb_buffer_t *(*pfn_hb_buffer_create)(void);
typedef void         (*pfn_hb_buffer_add_utf8)(hb_buffer_t *, const char *text,
        int text_length, unsigned int item_offset, int item_length);
typedef void         (*pfn_hb_buffer_set_direction)(hb_buffer_t *, int direction);
typedef void         (*pfn_hb_buffer_guess_segment_properties)(hb_buffer_t *);
typedef void         (*pfn_hb_shape)(hb_font_t *, hb_buffer_t *, const void *features,
        unsigned int num_features);
typedef unsigned int (*pfn_hb_buffer_get_length)(hb_buffer_t *);
typedef hb_glyph_info_t     *(*pfn_hb_buffer_get_glyph_infos)(hb_buffer_t *, unsigned int *length);
typedef hb_glyph_position_t *(*pfn_hb_buffer_get_glyph_positions)(hb_buffer_t *, unsigned int *length);
typedef void         (*pfn_hb_buffer_destroy)(hb_buffer_t *);
typedef void         (*pfn_hb_font_destroy)(hb_font_t *);
typedef void         (*pfn_hb_face_destroy)(hb_face_t *);
typedef void         (*pfn_hb_blob_destroy)(hb_blob_t *);

/* hb_direction_t (stable ABI): 4 = LTR, 5 = RTL, 6 = TTB, 7 = BTT. 0 = invalid
 * (use guess_segment_properties). We expose LTR/RTL/auto across the FFI. */
enum { RX_HB_DIR_INVALID = 0, RX_HB_DIR_LTR = 4, RX_HB_DIR_RTL = 5 };

/* The resolved HarfBuzz loader (dlopen'd separately from Skia). */
typedef struct {
    int available;   /* 1 iff the dylib loaded and all shaping symbols resolved */
    pfn_hb_blob_create_from_file_or_fail  blob_create_from_file;
    pfn_hb_face_create                    face_create;
    pfn_hb_font_create                    font_create;
    pfn_hb_font_set_scale                 font_set_scale;
    pfn_hb_buffer_create                  buffer_create;
    pfn_hb_buffer_add_utf8                buffer_add_utf8;
    pfn_hb_buffer_set_direction           buffer_set_direction;
    pfn_hb_buffer_guess_segment_properties buffer_guess_segment_properties;
    pfn_hb_shape                          shape;
    pfn_hb_buffer_get_length              buffer_get_length;
    pfn_hb_buffer_get_glyph_infos         buffer_get_glyph_infos;
    pfn_hb_buffer_get_glyph_positions     buffer_get_glyph_positions;
    pfn_hb_buffer_destroy                 buffer_destroy;
    pfn_hb_font_destroy                   font_destroy;
    pfn_hb_face_destroy                   face_destroy;
    pfn_hb_blob_destroy                   blob_destroy;
} RxHB;

/* Lazily dlopen()s libHarfBuzzSharp and resolves the table on first call;
 * process-wide singleton. Never NULL — check ->available. (runtime/skia_shim.c) */
const RxHB *rx_hb(void);

/* ---- the resolved loader ---- */
typedef struct {
    int available;    /* 1 iff the .so loaded and all required symbols resolved */

    pfn_surface_new_raster_direct surface_new_raster_direct;
    pfn_surface_get_canvas        surface_get_canvas;
    pfn_surface_unref             surface_unref;

    pfn_canvas_clear            canvas_clear;
    pfn_canvas_draw_rect        canvas_draw_rect;
    pfn_canvas_draw_oval        canvas_draw_oval;
    pfn_canvas_draw_round_rect  canvas_draw_round_rect;
    pfn_canvas_draw_rrect       canvas_draw_rrect;
    pfn_canvas_draw_line        canvas_draw_line;
    pfn_canvas_draw_simple_text canvas_draw_simple_text;

    pfn_canvas_save             canvas_save;
    pfn_canvas_restore          canvas_restore;
    pfn_canvas_save_layer       canvas_save_layer;
    pfn_canvas_get_save_count   canvas_get_save_count;
    pfn_canvas_restore_to_count canvas_restore_to_count;
    pfn_canvas_translate        canvas_translate;
    pfn_canvas_scale            canvas_scale;
    pfn_canvas_rotate_degrees   canvas_rotate_degrees;
    pfn_canvas_reset_matrix     canvas_reset_matrix;
    pfn_canvas_skew             canvas_skew;
    pfn_canvas_concat           canvas_concat;
    pfn_canvas_clip_rect        canvas_clip_rect;
    pfn_canvas_clip_rrect       canvas_clip_rrect;

    pfn_data_new_from_file      data_new_from_file;
    pfn_data_unref              data_unref;
    pfn_image_new_from_encoded  image_new_from_encoded;
    pfn_image_get_width         image_get_width;
    pfn_image_get_height        image_get_height;
    pfn_image_unref             image_unref;
    pfn_canvas_draw_image_rect  canvas_draw_image_rect;

    pfn_rrect_new               rrect_new;
    pfn_rrect_delete            rrect_delete;
    pfn_rrect_set_rect_radii    rrect_set_rect_radii;

    pfn_paint_new              paint_new;
    pfn_paint_delete           paint_delete;
    pfn_paint_set_color        paint_set_color;
    pfn_paint_set_antialias    paint_set_antialias;
    pfn_paint_set_style        paint_set_style;
    pfn_paint_set_stroke_width paint_set_stroke_width;
    pfn_paint_set_shader       paint_set_shader;
    pfn_paint_set_maskfilter   paint_set_maskfilter;

    pfn_shader_new_linear_gradient shader_new_linear_gradient;
    pfn_shader_new_radial_gradient shader_new_radial_gradient;
    pfn_shader_unref               shader_unref;
    pfn_maskfilter_new_blur        maskfilter_new_blur;
    pfn_maskfilter_unref           maskfilter_unref;

    pfn_typeface_create_default   typeface_create_default;
    pfn_typeface_create_from_name typeface_create_from_name;
    pfn_typeface_unref            typeface_unref;
    pfn_fontstyle_new             fontstyle_new;
    pfn_fontstyle_delete          fontstyle_delete;
    pfn_font_new_with_values      font_new_with_values;
    pfn_font_set_size             font_set_size;
    pfn_font_delete               font_delete;
    pfn_font_measure_text         font_measure_text;
    pfn_font_get_metrics          font_get_metrics;

    pfn_path_new           path_new;
    pfn_path_delete        path_delete;
    pfn_path_move_to       path_move_to;
    pfn_path_line_to       path_line_to;
    pfn_path_quad_to       path_quad_to;
    pfn_path_cubic_to      path_cubic_to;
    pfn_path_arc_to        path_arc_to;
    pfn_path_close         path_close;
    pfn_path_set_filltype  path_set_filltype;
    pfn_canvas_draw_path   canvas_draw_path;

    /* Ganesh GL backend (OPTIONAL; absence disables only the GPU rung). */
    pfn_gr_glinterface_assemble_gl       gr_glinterface_assemble_gl;
    pfn_gr_glinterface_create_native     gr_glinterface_create_native;
    pfn_gr_glinterface_unref             gr_glinterface_unref;
    pfn_gr_direct_context_make_gl        gr_direct_context_make_gl;
    pfn_gr_recording_context_unref       gr_recording_context_unref;
    pfn_gr_direct_context_flush          gr_direct_context_flush;
    pfn_gr_direct_context_flush_and_submit gr_direct_context_flush_and_submit;
    pfn_gr_backendrendertarget_new_gl    gr_backendrendertarget_new_gl;
    pfn_gr_backendrendertarget_delete    gr_backendrendertarget_delete;
    pfn_surface_new_backend_render_target surface_new_backend_render_target;
    int gpu_gl_ok;   /* 1 iff every required GPU-GL symbol resolved */

    /* Ganesh Metal backend + offscreen GPU surface + readback (OPTIONAL;
     * absence disables only the Metal rung). gr_backendrendertarget_new_metal
     * wraps an on-screen drawable's texture; offscreen uses surface_new_render_target. */
    pfn_gr_direct_context_make_metal     gr_direct_context_make_metal;
    pfn_surface_new_render_target        surface_new_render_target;
    pfn_gr_backendrendertarget_new_metal gr_backendrendertarget_new_metal;
    pfn_surface_read_pixels              surface_read_pixels;
    int gpu_metal_ok;  /* 1 iff offscreen GPU-Metal/readback symbols resolved */
    int gpu_metal_window_ok;  /* 1 iff on-screen drawable-wrap symbols also resolved */

    /* Positioned-glyph rendering for shaped text (OPTIONAL; absence disables
     * only shaping — the non-shaped text path is untouched). */
    pfn_typeface_create_from_file        typeface_create_from_file;
    pfn_textblob_builder_new             textblob_builder_new;
    pfn_textblob_builder_delete          textblob_builder_delete;
    pfn_textblob_builder_alloc_run_pos   textblob_builder_alloc_run_pos;
    pfn_textblob_builder_make            textblob_builder_make;
    pfn_textblob_unref                   textblob_unref;
    pfn_canvas_draw_text_blob            canvas_draw_text_blob;
    int glyph_render_ok;  /* 1 iff every textblob/typeface-from-file symbol resolved */
} RxSkia;

/* Lazily dlopen()s libSkiaSharp and resolves the table on first call; returns
 * a pointer to the process-wide singleton. Never NULL — check ->available.
 * Defined in runtime/skia_shim.c. */
const RxSkia *rx_skia(void);

#endif /* RX_SKIA_CAPI_H */
