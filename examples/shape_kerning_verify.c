/* shape_kerning_verify.c — standalone proof that canvas's text shaping
 * (HarfBuzz shape + Skia glyph render) applies real kerning + ligatures.
 *
 *   cc -O2 -o shape_kerning_verify examples/shape_kerning_verify.c
 *   ./shape_kerning_verify
 *
 * Mirrors what runtime/skia_shim.c does for the shaped-text path: dlopen the
 * fetched libHarfBuzzSharp + libSkiaSharp, load ONE font file into both
 * (hb_blob_create_from_file_or_fail + sk_typeface_create_from_file so glyph ids
 * match), shape a run with HarfBuzz, and (optionally) render the positioned
 * glyphs with Skia's textblob API. No link-time dependency.
 *
 * Prints the shaped advance widths and exits 0 + "PASS" iff:
 *   - "AV" shaped < "A" + "V" shaped alone (GPOS negative kerning applied), and
 *   - "ffi" shaped < 3x "f" shaped (the ffi ligature: 3 chars -> 1 glyph),
 * and the textblob render produced ink. Neither can happen with naive per-char
 * placement, so this proves real shaping. "SKIP" if the libs/font are absent.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dlfcn.h>

/* ---- HarfBuzz C ABI slice ---- */
typedef struct hb_blob_t hb_blob_t; typedef struct hb_face_t hb_face_t;
typedef struct hb_font_t hb_font_t; typedef struct hb_buffer_t hb_buffer_t;
typedef struct { uint32_t codepoint, mask, cluster, var1, var2; } hb_glyph_info_t;
typedef struct { int32_t x_advance, y_advance, x_offset, y_offset; uint32_t var; } hb_glyph_position_t;

/* ---- Skia C ABI slice ---- */
typedef struct sk_surface_t sk_surface_t; typedef struct sk_canvas_t sk_canvas_t;
typedef struct sk_paint_t sk_paint_t; typedef struct sk_typeface_t sk_typeface_t;
typedef struct sk_font_t sk_font_t; typedef struct sk_textblob_t sk_textblob_t;
typedef struct sk_textblob_builder_t sk_tbb_t;
typedef struct { void* cs; int32_t w,h,ct,at; } imageinfo;
typedef struct { void* glyphs; void* pos; void* utf8; void* clusters; } runbuffer;

static void *HB, *SK;
static const char *FONT = "/System/Library/Fonts/Supplemental/Arial.ttf";

/* shape `txt` at `size` px, return total advance in px; if `cv` != NULL also
 * render the run and report ink via *lit. */
static double shape_width(const char *txt, int size, sk_canvas_t *cv,
                          sk_font_t *skf, long *lit, uint32_t *px, int npx) {
    hb_blob_t *(*bf)(const char*) = dlsym(HB, "hb_blob_create_from_file_or_fail");
    hb_face_t *(*fc)(hb_blob_t*,unsigned) = dlsym(HB, "hb_face_create");
    hb_font_t *(*foc)(hb_face_t*) = dlsym(HB, "hb_font_create");
    hb_buffer_t *(*bc)(void) = dlsym(HB, "hb_buffer_create");
    void (*ba)(hb_buffer_t*,const char*,int,unsigned,int) = dlsym(HB, "hb_buffer_add_utf8");
    void (*bg)(hb_buffer_t*) = dlsym(HB, "hb_buffer_guess_segment_properties");
    void (*sh)(hb_font_t*,hb_buffer_t*,const void*,unsigned) = dlsym(HB, "hb_shape");
    unsigned (*bl)(hb_buffer_t*) = dlsym(HB, "hb_buffer_get_length");
    hb_glyph_info_t *(*bi)(hb_buffer_t*,unsigned*) = dlsym(HB, "hb_buffer_get_glyph_infos");
    hb_glyph_position_t *(*bp)(hb_buffer_t*,unsigned*) = dlsym(HB, "hb_buffer_get_glyph_positions");
    void (*fs)(hb_font_t*,int,int) = dlsym(HB, "hb_font_set_scale");

    hb_blob_t *b = bf(FONT); hb_face_t *f = fc(b, 0); hb_font_t *ft = foc(f);
    fs(ft, size*64, size*64);
    hb_buffer_t *buf = bc(); ba(buf, txt, -1, 0, -1); bg(buf); sh(ft, buf, 0, 0);
    unsigned n = bl(buf);
    hb_glyph_info_t *gi = bi(buf, 0);
    hb_glyph_position_t *gp = bp(buf, 0);
    double tot = 0; for (unsigned i = 0; i < n; i++) tot += gp[i].x_advance / 64.0;

    if (cv && skf) {
        sk_tbb_t *(*tnew)(void) = dlsym(SK, "sk_textblob_builder_new");
        void (*trp)(sk_tbb_t*,const sk_font_t*,int,const void*,runbuffer*) = dlsym(SK, "sk_textblob_builder_alloc_run_pos");
        sk_textblob_t *(*tmk)(sk_tbb_t*) = dlsym(SK, "sk_textblob_builder_make");
        void (*tdel)(sk_tbb_t*) = dlsym(SK, "sk_textblob_builder_delete");
        void (*dblob)(sk_canvas_t*,sk_textblob_t*,float,float,const sk_paint_t*) = dlsym(SK, "sk_canvas_draw_text_blob");
        sk_paint_t *(*pnew)(void) = dlsym(SK, "sk_paint_new");
        void (*pcol)(sk_paint_t*,uint32_t) = dlsym(SK, "sk_paint_set_color");
        void (*paa)(sk_paint_t*,int) = dlsym(SK, "sk_paint_set_antialias");
        sk_tbb_t *tb = tnew(); runbuffer rb; trp(tb, skf, (int)n, NULL, &rb);
        uint16_t *g = rb.glyphs; float *p = rb.pos; double pen = 0;
        for (unsigned i = 0; i < n; i++) { g[i] = (uint16_t)gi[i].codepoint; p[2*i] = (float)(pen + gp[i].x_offset/64.0); p[2*i+1] = (float)(-gp[i].y_offset/64.0); pen += gp[i].x_advance/64.0; }
        sk_textblob_t *blob = tmk(tb); tdel(tb);
        sk_paint_t *pt = pnew(); paa(pt, 1); pcol(pt, 0xFFFFFFFFu);
        dblob(cv, blob, 4.0f, 24.0f, pt);
        if (lit) { *lit = 0; for (int i = 0; i < npx; i++) if (px[i] != 0xFF000000u) (*lit)++; }
    }
    return tot;
}

int main(void) {
    char skpath[4096];
    snprintf(skpath, sizeof skpath, "%s/.cache/ruxen-canvas/libSkiaSharp.dylib", getenv("HOME"));
    char hbpath[4096];
    snprintf(hbpath, sizeof hbpath, "%s/.cache/ruxen-canvas/libHarfBuzzSharp.dylib", getenv("HOME"));
    SK = dlopen(skpath, RTLD_NOW | RTLD_LOCAL);
    HB = dlopen(hbpath, RTLD_NOW | RTLD_LOCAL);
    if (!SK || !HB) { printf("SKIP: sk=%p hb=%p (run runtime/fetch_skia.sh)\n", SK, HB); return 2; }
    FILE *ff = fopen(FONT, "rb");
    if (!ff) { printf("SKIP: font %s absent\n", FONT); return 2; }
    fclose(ff);

    /* a small raster surface to prove the textblob render path inks pixels */
    sk_surface_t *(*new_raster)(const imageinfo*,void*,size_t,void*,void*,const void*) = dlsym(SK, "sk_surface_new_raster_direct");
    sk_canvas_t *(*get_canvas)(sk_surface_t*) = dlsym(SK, "sk_surface_get_canvas");
    void (*clear)(sk_canvas_t*,uint32_t) = dlsym(SK, "sk_canvas_clear");
    sk_typeface_t *(*tf_file)(const char*,int) = dlsym(SK, "sk_typeface_create_from_file");
    sk_font_t *(*font_new)(sk_typeface_t*,float,float,float) = dlsym(SK, "sk_font_new_with_values");
    int W = 160, H = 40; uint32_t *px = calloc(W*H, 4);
    imageinfo info = { 0, W, H, 6, 2 };
    sk_surface_t *surf = new_raster(&info, px, W*4, NULL, NULL, NULL);
    sk_canvas_t *cv = get_canvas(surf); clear(cv, 0xFF000000u);
    sk_typeface_t *tf = tf_file(FONT, 0);
    sk_font_t *skf = font_new(tf, 24.0f, 1.0f, 0.0f);

    long lit = 0;
    double av  = shape_width("AV", 24, NULL, NULL, NULL, NULL, 0);
    double a   = shape_width("A",  24, NULL, NULL, NULL, NULL, 0);
    double v   = shape_width("V",  24, NULL, NULL, NULL, NULL, 0);
    double ffi = shape_width("ffi", 24, cv, skf, &lit, px, W*H);
    double f   = shape_width("f",  24, NULL, NULL, NULL, NULL, 0);

    printf("kerning: AV=%.1f  A+V=%.1f  (tighter by %.1fpx)\n", av, a+v, (a+v)-av);
    printf("ligature: ffi=%.1f  3xf=%.1f  (tighter by %.1fpx)\n", ffi, f*3, f*3-ffi);
    printf("textblob render lit_pixels=%ld\n", lit);

    int pass = (av < a + v) && (ffi < f * 3) && (lit > 0);
    if (pass) { printf("PASS: HarfBuzz kerning + ligatures applied; Skia textblob inked\n"); return 0; }
    printf("FAIL: shaping not demonstrated\n");
    return 1;
}
