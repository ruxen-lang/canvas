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
typedef struct sk_font_t       sk_font_t;
typedef struct sk_typeface_t   sk_typeface_t;
typedef struct sk_colorspace_t sk_colorspace_t;

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

typedef sk_typeface_t *(*pfn_typeface_create_default)(void);
typedef sk_font_t     *(*pfn_font_new_with_values)(sk_typeface_t *, float size, float scale_x,
        float skew_x);
typedef void           (*pfn_font_set_size)(sk_font_t *, float);
typedef void           (*pfn_font_delete)(sk_font_t *);
typedef float          (*pfn_font_measure_text)(const sk_font_t *, const void *text, size_t byte_len,
        int encoding, sk_rect_t *bounds, const sk_paint_t *);
typedef float          (*pfn_font_get_metrics)(const sk_font_t *, sk_fontmetrics_t *);

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

    pfn_rrect_new               rrect_new;
    pfn_rrect_delete            rrect_delete;
    pfn_rrect_set_rect_radii    rrect_set_rect_radii;

    pfn_paint_new              paint_new;
    pfn_paint_delete           paint_delete;
    pfn_paint_set_color        paint_set_color;
    pfn_paint_set_antialias    paint_set_antialias;
    pfn_paint_set_style        paint_set_style;
    pfn_paint_set_stroke_width paint_set_stroke_width;

    pfn_typeface_create_default typeface_create_default;
    pfn_font_new_with_values    font_new_with_values;
    pfn_font_set_size           font_set_size;
    pfn_font_delete             font_delete;
    pfn_font_measure_text       font_measure_text;
    pfn_font_get_metrics        font_get_metrics;
} RxSkia;

/* Lazily dlopen()s libSkiaSharp and resolves the table on first call; returns
 * a pointer to the process-wide singleton. Never NULL — check ->available.
 * Defined in runtime/skia_shim.c. */
const RxSkia *rx_skia(void);

#endif /* RX_SKIA_CAPI_H */
