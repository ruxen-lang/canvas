/* metal_window_verify.c — standalone proof that canvas's ON-SCREEN Metal path
 * shows GPU output in a real window. RUN THIS ON A METAL DESKTOP (a logged-in
 * macOS session with a display) — it cannot be verified headless or in the test
 * harness (macOS forbids CoreFoundation/Metal/AppKit after fork() without
 * exec(), and the harness forks per case; see docs/GPU.md).
 *
 *   cc -O2 -o metal_window_verify examples/metal_window_verify.c
 *   ./metal_window_verify            # a window appears showing a blue rect on
 *                                    # black, drawn on the GPU, for ~2 seconds
 *
 * Mirrors exactly what runtime/sdl_window.c + runtime/skia_shim.c do for the
 * windowed-Metal path: dlopen SDL2 + libSkiaSharp + Metal.framework + the objc
 * runtime; SDL_WINDOW_METAL window -> SDL_Metal_CreateView -> SDL_Metal_GetLayer
 * -> CAMetalLayer (configured with the system MTLDevice, BGRA8, framebufferOnly
 * NO); per frame: [layer nextDrawable] -> [drawable texture] ->
 * gr_backendrendertarget_new_metal -> sk_surface_new_backend_render_target ->
 * draw -> gr_direct_context_flush_and_submit -> [queue commandBuffer]
 * [presentDrawable:] [commit]. No link-time dependency.
 *
 * Exit 0 + "PASS" once a frame was presented to the window; "SKIP" if SDL2 / the
 * libs / a display are unavailable.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dlfcn.h>

/* ---- Skia C ABI slice ---- */
typedef struct sk_surface_t sk_surface_t; typedef struct sk_canvas_t sk_canvas_t;
typedef struct sk_paint_t sk_paint_t;
typedef struct gr_direct_context_t gr_direct_context_t;
typedef struct gr_recording_context_t gr_recording_context_t;
typedef struct gr_backendrendertarget_t gr_backendrendertarget_t;
typedef struct { void* cs; int32_t w,h,ct,at; } imageinfo;
typedef struct { float l,t,r,b; } rectf;
typedef struct { const void* fTexture; } mtl_texinfo;
enum { CT_BGRA = 6, AT_PREMUL = 2, ORIGIN_TL = 0, PAINT_FILL = 0 };

/* SDL constants */
#define SDL_INIT_VIDEO 0x20u
#define SDL_WINDOWPOS_CENTERED 0x2FFF0000u
#define SDL_WINDOW_SHOWN 0x4u
#define SDL_WINDOW_METAL 0x20000000u
#define SDL_WINDOW_ALLOW_HIGHDPI 0x00002000u
#define MTL_PIXFMT_BGRA8 80

static void *DL(const char *const *names, int n) {
    for (int i = 0; i < n; i++) { void *h = dlopen(names[i], RTLD_NOW | RTLD_GLOBAL); if (h) return h; }
    return NULL;
}

int main(void) {
    const char *sdl_names[] = { "libSDL2-2.0.0.dylib", "/opt/homebrew/lib/libSDL2-2.0.0.dylib",
                                "/usr/local/lib/libSDL2-2.0.0.dylib", "libSDL2.dylib" };
    void *SDL = DL(sdl_names, 4);
    void *MTL = dlopen("/System/Library/Frameworks/Metal.framework/Metal", RTLD_NOW | RTLD_GLOBAL);
    void *OBJC = dlopen("/usr/lib/libobjc.A.dylib", RTLD_NOW | RTLD_GLOBAL);
    dlopen("/System/Library/Frameworks/QuartzCore.framework/QuartzCore", RTLD_NOW | RTLD_GLOBAL);
    if (!SDL || !MTL || !OBJC) { printf("SKIP: SDL2/Metal/objc unavailable (sdl=%p mtl=%p objc=%p)\n", SDL, MTL, OBJC); return 2; }

    char skpath[4096]; snprintf(skpath, sizeof skpath, "%s/.cache/ruxen-canvas/libSkiaSharp.dylib", getenv("HOME"));
    void *SK = dlopen(skpath, RTLD_NOW | RTLD_LOCAL);
    if (!SK) { printf("SKIP: %s absent (run runtime/fetch_skia.sh)\n", skpath); return 2; }

    /* SDL + Metal-view */
    int   (*SDL_Init)(uint32_t) = dlsym(SDL, "SDL_Init");
    void *(*SDL_CreateWindow)(const char*,int,int,int,int,uint32_t) = dlsym(SDL, "SDL_CreateWindow");
    void *(*SDL_Metal_CreateView)(void*) = dlsym(SDL, "SDL_Metal_CreateView");
    void *(*SDL_Metal_GetLayer)(void*) = dlsym(SDL, "SDL_Metal_GetLayer");
    void  (*SDL_Delay)(uint32_t) = dlsym(SDL, "SDL_Delay");
    void  (*SDL_DestroyWindow)(void*) = dlsym(SDL, "SDL_DestroyWindow");
    void  (*SDL_Metal_GetDrawableSize)(void*,int*,int*) = dlsym(SDL, "SDL_Metal_GetDrawableSize");
    if (!SDL_Init || !SDL_CreateWindow || !SDL_Metal_CreateView || !SDL_Metal_GetLayer) {
        printf("SKIP: SDL_Metal_* not in this SDL2 build\n"); return 2;
    }

    /* Metal device + queue + objc */
    void *(*MTLCreate)(void) = dlsym(MTL, "MTLCreateSystemDefaultDevice");
    void *(*msg)(void*,void*) = dlsym(OBJC, "objc_msgSend");
    void *(*sel)(const char*) = dlsym(OBJC, "sel_registerName");
    void *(*msg_p)(void*,void*,void*) = (void*(*)(void*,void*,void*))dlsym(OBJC, "objc_msgSend");
    void  (*msg_u)(void*,void*,unsigned long) = (void(*)(void*,void*,unsigned long))dlsym(OBJC, "objc_msgSend");
    void  (*msg_sz)(void*,void*,double,double) = (void(*)(void*,void*,double,double))dlsym(OBJC, "objc_msgSend");
    void *device = MTLCreate();
    if (!device) { printf("SKIP: no Metal device\n"); return 2; }
    void *queue = msg(device, sel("newCommandQueue"));

    /* Skia Metal symbols */
    gr_direct_context_t *(*make_metal)(void*,void*) = dlsym(SK, "gr_direct_context_make_metal");
    gr_backendrendertarget_t *(*rt_metal)(int,int,const mtl_texinfo*) = dlsym(SK, "gr_backendrendertarget_new_metal");
    void (*rt_del)(gr_backendrendertarget_t*) = dlsym(SK, "gr_backendrendertarget_delete");
    sk_surface_t *(*surf_brt)(gr_recording_context_t*,const gr_backendrendertarget_t*,int,int,void*,const void*) = dlsym(SK, "sk_surface_new_backend_render_target");
    sk_canvas_t *(*get_canvas)(sk_surface_t*) = dlsym(SK, "sk_surface_get_canvas");
    void (*canvas_clear)(sk_canvas_t*,uint32_t) = dlsym(SK, "sk_canvas_clear");
    void (*draw_rect)(sk_canvas_t*,const rectf*,const sk_paint_t*) = dlsym(SK, "sk_canvas_draw_rect");
    sk_paint_t *(*paint_new)(void) = dlsym(SK, "sk_paint_new");
    void (*paint_color)(sk_paint_t*,uint32_t) = dlsym(SK, "sk_paint_set_color");
    void (*paint_style)(sk_paint_t*,int) = dlsym(SK, "sk_paint_set_style");
    void (*flush_submit)(gr_direct_context_t*,int) = dlsym(SK, "gr_direct_context_flush_and_submit");
    void (*surf_unref)(sk_surface_t*) = dlsym(SK, "sk_surface_unref");
    if (!make_metal || !rt_metal || !surf_brt || !draw_rect) { printf("SKIP: Skia Metal symbols absent\n"); return 2; }

    if (SDL_Init(SDL_INIT_VIDEO) != 0) { printf("SKIP: SDL_Init failed (no display?)\n"); return 2; }
    int LW = 320, LH = 240;                               /* logical window size */
    /* ALLOW_HIGHDPI so the backing store is the true Retina pixel size; we render
     * at THAT size (queried below) and present 1:1 — crisp, no upscale. */
    void *win = SDL_CreateWindow("ruxen metal window verify", (int)SDL_WINDOWPOS_CENTERED,
                                 (int)SDL_WINDOWPOS_CENTERED, LW, LH,
                                 SDL_WINDOW_METAL | SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!win) { printf("SKIP: no window (headless?)\n"); return 2; }
    void *view = SDL_Metal_CreateView(win);
    void *layer = view ? SDL_Metal_GetLayer(view) : NULL;
    if (!layer) { printf("SKIP: no CAMetalLayer\n"); SDL_DestroyWindow(win); return 2; }

    /* The true backing (Retina) pixel size — render at this for crispness. */
    int W = LW, H = LH;
    if (SDL_Metal_GetDrawableSize) SDL_Metal_GetDrawableSize(win, &W, &H);
    if (W <= 0 || H <= 0) { W = LW; H = LH; }
    printf("logical %dx%d -> backing %dx%d (dpr ~%.1f)\n", LW, LH, W, H, (double)W / LW);

    msg_p(layer, sel("setDevice:"), device);
    msg_u(layer, sel("setPixelFormat:"), MTL_PIXFMT_BGRA8);
    msg_u(layer, sel("setFramebufferOnly:"), 0);
    msg_sz(layer, sel("setDrawableSize:"), (double)W, (double)H);

    gr_direct_context_t *ctx = make_metal(device, queue);
    if (!ctx) { printf("FAIL: make_metal NULL\n"); return 1; }

    int presented = 0;
    for (int frame = 0; frame < 120; frame++) {           /* ~2s at 60Hz */
        void *drawable = msg(layer, sel("nextDrawable"));
        if (!drawable) { if (SDL_Delay) SDL_Delay(16); continue; }
        void *texture = msg(drawable, sel("texture"));
        mtl_texinfo ti; ti.fTexture = texture;
        gr_backendrendertarget_t *rt = rt_metal(W, H, &ti);   /* native backing size */
        sk_surface_t *surf = rt ? surf_brt((gr_recording_context_t*)ctx, rt, ORIGIN_TL, CT_BGRA, NULL, NULL) : NULL;
        if (!surf) { if (rt) rt_del(rt); break; }
        sk_canvas_t *cv = get_canvas(surf);
        canvas_clear(cv, 0xFF000000u);                    /* black */
        sk_paint_t *p = paint_new(); paint_style(p, PAINT_FILL); paint_color(p, 0xFF0080FFu);
        /* rect in native pixels (scaled from the logical 40..280 x 40..200). */
        float sx = (float)W / LW, sy = (float)H / LH;
        rectf r = { 40.0f * sx, 40.0f * sy, 280.0f * sx, 200.0f * sy };
        draw_rect(cv, &r, p);
        if (flush_submit) flush_submit(ctx, 0);
        /* present: [[queue commandBuffer] presentDrawable:drawable]; commit */
        void *cmd = msg(queue, sel("commandBuffer"));
        msg_p(cmd, sel("presentDrawable:"), drawable);
        msg(cmd, sel("commit"));
        surf_unref(surf); rt_del(rt);
        presented = 1;
        if (SDL_Delay) SDL_Delay(16);
    }

    SDL_DestroyWindow(win);
    if (presented) { printf("PASS: presented GPU frames to an on-screen Metal window\n"); return 0; }
    printf("FAIL: no drawable could be acquired (no display?)\n");
    return 1;
}
