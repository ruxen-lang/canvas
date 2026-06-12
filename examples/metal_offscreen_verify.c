/* metal_offscreen_verify.c — standalone proof that canvas's Metal backend
 * renders on the GPU and reads pixels back, HEADLESS (no window/display).
 *
 *   cc -O2 -o metal_offscreen_verify examples/metal_offscreen_verify.c
 *   ./metal_offscreen_verify
 *
 * Why this lives OUTSIDE the test harness: Apple forbids using Metal in a
 * process that fork()s without exec(). The `ruxen test` harness runs each test
 * case in a forked child, and Metal's runtime shader-compiler XPC service
 * (MTLCompilerService) is unreachable post-fork — `clear` works there but any
 * draw needing a runtime-compiled shader (draw_rect) dies in the forked child.
 * So the real GPU render+readback is verified HERE, in a normal (non-forked)
 * process, and the in-harness `tests/gpu_backend.rx` pins only the capability +
 * clean-fallback contract. See docs/GPU.md.
 *
 * This mirrors exactly what runtime/skia_shim.c does for the offscreen Metal
 * path: MTLCreateSystemDefaultDevice + [device newCommandQueue] (objc runtime),
 * gr_direct_context_make_metal, an offscreen BGRA sk_surface_new_render_target,
 * draw, gr_direct_context_flush_and_submit(sync), sk_surface_read_pixels. It
 * dlopen()s the same fetched libSkiaSharp.dylib — no link-time dependency.
 *
 * Exit 0 and prints "PASS" iff a blue rect drawn on the GPU reads back at the
 * expected pixels (and the background stays black); nonzero + "SKIP"/"FAIL"
 * otherwise (e.g. no GPU / no dylib).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dlfcn.h>

/* minimal slice of the Skia C ABI (matches runtime/skia/skia_capi.h) */
typedef struct sk_surface_t          sk_surface_t;
typedef struct sk_canvas_t           sk_canvas_t;
typedef struct sk_paint_t            sk_paint_t;
typedef struct gr_direct_context_t   gr_direct_context_t;
typedef struct gr_recording_context_t gr_recording_context_t;
typedef struct { void *cs; int32_t w, h, ct, at; } imageinfo;  /* sk_imageinfo_t */
typedef struct { float l, t, r, b; } rectf;                    /* sk_rect_t */

/* sk_colortype_t BGRA_8888 = 6; sk_alphatype_t premul = 2; paint fill = 0 */
enum { CT_BGRA = 6, AT_PREMUL = 2, PAINT_FILL = 0 };

static void *sym(void *h, const char *n) { return dlsym(h, n); }

int main(void) {
    /* --- Metal device + command queue (Metal.framework + objc runtime) --- */
    void *mtl  = dlopen("/System/Library/Frameworks/Metal.framework/Metal", RTLD_NOW | RTLD_GLOBAL);
    void *objc = dlopen("/usr/lib/libobjc.A.dylib", RTLD_NOW | RTLD_GLOBAL);
    if (!mtl || !objc) { printf("SKIP: no Metal.framework / objc runtime\n"); return 2; }
    void *(*MTLCreate)(void)        = (void *(*)(void))sym(mtl, "MTLCreateSystemDefaultDevice");
    void *(*msg_send)(void *, void *) = (void *(*)(void *, void *))sym(objc, "objc_msgSend");
    void *(*sel_reg)(const char *)  = (void *(*)(const char *))sym(objc, "sel_registerName");
    if (!MTLCreate || !msg_send || !sel_reg) { printf("SKIP: missing Metal/objc symbols\n"); return 2; }
    void *device = MTLCreate();
    if (!device) { printf("SKIP: no Metal device (headless GPU unavailable)\n"); return 2; }
    void *queue = msg_send(device, sel_reg("newCommandQueue"));
    if (!queue) { printf("SKIP: could not create command queue\n"); return 2; }

    /* --- Skia: dlopen the same fetched dylib the shim uses --- */
    const char *home = getenv("HOME");
    char path[4096];
    snprintf(path, sizeof path, "%s/.cache/ruxen-canvas/libSkiaSharp.dylib", home ? home : "");
    void *sk = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!sk) { printf("SKIP: %s not found (run runtime/fetch_skia.sh)\n", path); return 2; }

    gr_direct_context_t *(*make_metal)(void *, void *) = sym(sk, "gr_direct_context_make_metal");
    sk_surface_t *(*new_rt)(gr_recording_context_t *, int, const imageinfo *, int, int, const void *, int)
        = sym(sk, "sk_surface_new_render_target");
    sk_canvas_t *(*get_canvas)(sk_surface_t *)              = sym(sk, "sk_surface_get_canvas");
    void (*canvas_clear)(sk_canvas_t *, uint32_t)           = sym(sk, "sk_canvas_clear");
    void (*draw_rect)(sk_canvas_t *, const rectf *, const sk_paint_t *) = sym(sk, "sk_canvas_draw_rect");
    sk_paint_t *(*paint_new)(void)                          = sym(sk, "sk_paint_new");
    void (*paint_delete)(sk_paint_t *)                      = sym(sk, "sk_paint_delete");
    void (*paint_color)(sk_paint_t *, uint32_t)             = sym(sk, "sk_paint_set_color");
    void (*paint_style)(sk_paint_t *, int)                  = sym(sk, "sk_paint_set_style");
    void (*paint_aa)(sk_paint_t *, int)                     = sym(sk, "sk_paint_set_antialias");
    void (*flush_submit)(gr_direct_context_t *, int)        = sym(sk, "gr_direct_context_flush_and_submit");
    int  (*read_pixels)(sk_surface_t *, imageinfo *, void *, size_t, int, int) = sym(sk, "sk_surface_read_pixels");
    void (*surf_unref)(sk_surface_t *)                      = sym(sk, "sk_surface_unref");
    void (*ctx_unref)(gr_recording_context_t *)            = sym(sk, "gr_recording_context_unref");
    if (!make_metal || !new_rt || !get_canvas || !canvas_clear || !draw_rect ||
        !paint_new || !paint_color || !paint_style || !read_pixels) {
        printf("SKIP: Metal/offscreen/readback symbols absent in this dylib\n");
        return 2;
    }

    /* --- GrDirectContext over Metal + offscreen BGRA surface --- */
    gr_direct_context_t *ctx = make_metal(device, queue);
    if (!ctx) { printf("FAIL: gr_direct_context_make_metal returned NULL\n"); return 1; }
    imageinfo info = { 0, 8, 8, CT_BGRA, AT_PREMUL };
    sk_surface_t *surf = new_rt((gr_recording_context_t *)ctx, 1, &info, 0, 0 /*top-left*/, 0, 0);
    if (!surf) { printf("FAIL: sk_surface_new_render_target returned NULL\n"); return 1; }
    sk_canvas_t *cv = get_canvas(surf);

    /* clear to opaque black, draw an opaque blue rect [2,5) x [1,3) */
    canvas_clear(cv, 0xFF000000u);
    sk_paint_t *p = paint_new();
    paint_aa(p, 0);
    paint_style(p, PAINT_FILL);
    paint_color(p, 0xFF0080FFu);                 /* 0xAARRGGBB: opaque rgb(0,128,255) */
    rectf r = { 2.0f, 1.0f, 5.0f, 3.0f };
    draw_rect(cv, &r, p);
    if (paint_delete) paint_delete(p);
    if (flush_submit) flush_submit(ctx, 1);      /* sync: finish GPU before readback */

    /* --- read the GPU pixels back (BGRA -> host 0xAARRGGBB) --- */
    uint32_t px[64];
    memset(px, 0, sizeof px);
    imageinfo dst = { 0, 8, 8, CT_BGRA, AT_PREMUL };
    int ok = read_pixels(surf, &dst, px, 8 * 4, 0, 0);

    uint32_t fill = 0xFF0080FFu, bg = 0xFF000000u;
    int pass = ok &&
               px[1 * 8 + 2] == fill &&          /* (2,1) inside */
               px[2 * 8 + 4] == fill &&          /* (4,2) inside */
               px[1 * 8 + 5] == bg &&            /* (5,1) exclusive right edge */
               px[0]         == bg;              /* (0,0) outside */

    printf("read_pixels ok=%d  (2,1)=0x%08X  (4,2)=0x%08X  (5,1)=0x%08X  (0,0)=0x%08X\n",
           ok, px[1 * 8 + 2], px[2 * 8 + 4], px[1 * 8 + 5], px[0]);

    if (surf_unref) surf_unref(surf);
    if (ctx_unref)  ctx_unref((gr_recording_context_t *)ctx);

    if (pass) { printf("PASS: Metal rendered on GPU and read back byte-exact\n"); return 0; }
    printf("FAIL: GPU readback did not match expected pixels\n");
    return 1;
}
