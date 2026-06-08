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
typedef int  (*pfn_canvas_get_save_count)(sk_canvas_t *);
typedef void (*pfn_canvas_restore_to_count)(sk_canvas_t *, int save_count);
typedef void (*pfn_canvas_translate)(sk_canvas_t *, float dx, float dy);
typedef void (*pfn_canvas_scale)(sk_canvas_t *, float sx, float sy);
typedef void (*pfn_canvas_rotate_degrees)(sk_canvas_t *, float degrees);
typedef void (*pfn_canvas_reset_matrix)(sk_canvas_t *);
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
    pfn_canvas_get_save_count   canvas_get_save_count;
    pfn_canvas_restore_to_count canvas_restore_to_count;
    pfn_canvas_translate        canvas_translate;
    pfn_canvas_scale            canvas_scale;
    pfn_canvas_rotate_degrees   canvas_rotate_degrees;
    pfn_canvas_reset_matrix     canvas_reset_matrix;
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

    pfn_typeface_create_default typeface_create_default;
    pfn_font_new_with_values    font_new_with_values;
    pfn_font_set_size           font_set_size;
    pfn_font_delete             font_delete;
    pfn_font_measure_text       font_measure_text;
    pfn_font_get_metrics        font_get_metrics;

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
} RxSkia;

/* Lazily dlopen()s libSkiaSharp and resolves the table on first call; returns
 * a pointer to the process-wide singleton. Never NULL — check ->available.
 * Defined in runtime/skia_shim.c. */
const RxSkia *rx_skia(void);

#endif /* RX_SKIA_CAPI_H */
