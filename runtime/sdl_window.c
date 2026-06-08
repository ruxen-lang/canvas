/*
 * sdl_window.c — a real OS window for the canvas software backend.
 *
 * Strategy: dlopen("libSDL2-2.0.so.0") at runtime with self-declared
 * prototypes. The SDL2 *runtime* library ships with every desktop distro;
 * the -devel headers do not — so this file declares the dozen symbols it
 * needs itself and never includes <SDL2/SDL.h>. No link-time dependency,
 * no build-time dependency; on a machine without SDL2 the show() call
 * returns an error and everything else (the software framebuffer, the
 * event ring, read_pixel) keeps working headless.
 *
 * The window is a *presenter* for the existing RxHost framebuffer:
 *   - ruxen_canvas_window_show(host, title)   create window/renderer/texture
 *   - ruxen_canvas_window_present(host)       blit host->pixels, vsync flip
 *   - ruxen_canvas_window_pump(host)          SDL events -> the ring buffer
 *   - ruxen_canvas_window_destroy()           tear down
 *
 * The pump translates SDL input into the SAME RxEvent stream tests feed
 * with push_event, so L2/L3 dispatch code is identical headless vs windowed.
 *
 * SDL2 ABI notes (stable since 2.0.0):
 *   SDL_Event is a 56-byte union; we read fields at fixed offsets from a
 *   64-byte buffer instead of declaring the union:
 *     all events:      Uint32 type            @ 0
 *     mouse button:    Sint32 x @ 20, y @ 24  (button Uint8 @ 16)
 *     mouse motion:    Sint32 x @ 20, y @ 24
 *     keyboard:        Sint32 keysym.sym @ 20
 *   One window only for this slice (mirrors the single-RxHost model).
 */

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <string.h>

/* ---- status codes (keep in sync with skia_shim.c) ---- */
#define RXC_OK             0
#define RXC_ERR_BAD_ARGS   1
#define RXC_ERR_PRESENT    5
#define RXC_ERR_NO_SDL     6  /* libSDL2 unavailable / init failed */
#define RXC_ERR_BUSY       7  /* another host already owns the window */

/* ---- the slice of RxHost this file needs (layout-prefix of the real
 * struct in skia_shim.c; only width/height/pixels are touched here) ---- */
typedef struct {
    int32_t   width;
    int32_t   height;
    uint32_t *pixels;
} RxHostPrefix;

/* push into the host's event ring (defined in skia_shim.c) */
extern int64_t ruxen_canvas_push_event(int64_t self, int64_t kind, double a, double b);

/* event-kind tags (must match the Rxc module in src/lib.rx) */
#define RX_EV_POINTER_MOVE 0
#define RX_EV_POINTER_DOWN 1
#define RX_EV_POINTER_UP   2
#define RX_EV_KEY_DOWN     3
#define RX_EV_RESIZE       4
#define RX_EV_CLOSE        5
#define RX_EV_SCROLL       6

/* ---- SDL2 constants (from the stable public ABI) ---- */
#define SDL_INIT_VIDEO            0x00000020u
#define SDL_WINDOWPOS_CENTERED    0x2FFF0000u
#define SDL_WINDOW_SHOWN          0x00000004u
#define SDL_WINDOW_OPENGL         0x00000002u
#define SDL_PIXELFORMAT_ARGB8888  0x16362004u
#define SDL_TEXTUREACCESS_STREAMING 1
#define SDL_QUIT_EV               0x100u
#define SDL_WINDOWEVENT_EV        0x200u
#define SDL_KEYDOWN_EV            0x300u
#define SDL_MOUSEMOTION_EV        0x400u
#define SDL_MOUSEBUTTONDOWN_EV    0x401u
#define SDL_MOUSEBUTTONUP_EV      0x402u
#define SDL_MOUSEWHEEL_EV         0x403u
/* SDL_WindowEventID subtype (SDL_WindowEvent.event @ byte 12, Uint8). */
#define SDL_WINDOWEVENT_RESIZED   5
/* SDL_WINDOW_RESIZABLE — let the user resize the window; we re-create the GPU
 * surface/drawable at the new backing size and emit Event.Resize. */
#define SDL_WINDOW_RESIZABLE      0x00000020u

/* SDL_GLattr (from SDL_video.h — stable ABI) used to request a GL context the
 * Ganesh GL backend can render into. We ask for a core-profile context with a
 * stencil + a few framebuffer bits Skia's GL surface expects. */
#define SDL_GL_RED_SIZE           0
#define SDL_GL_GREEN_SIZE         1
#define SDL_GL_BLUE_SIZE          2
#define SDL_GL_ALPHA_SIZE         3
#define SDL_GL_DEPTH_SIZE         6
#define SDL_GL_STENCIL_SIZE       7
#define SDL_GL_DOUBLEBUFFER       5
#define SDL_GL_CONTEXT_MAJOR_VERSION 17
#define SDL_GL_CONTEXT_MINOR_VERSION 18
#define SDL_GL_CONTEXT_PROFILE_MASK  21
#define SDL_GL_CONTEXT_PROFILE_CORE  0x0001

/* GL_FRAMEBUFFER_BINDING — the name of the currently-bound default framebuffer.
 * Skia's GrBackendRenderTarget needs the FBO id the window's GL context draws
 * into; on most desktop GL contexts SDL gives us FBO 0, but we query it to be
 * correct on platforms (e.g. some EGL/ANGLE setups) where it is not. */
#define GL_FRAMEBUFFER_BINDING    0x8CA6
/* GL pixel format for an 8-bit BGRA / RGBA backbuffer — Skia maps the
 * render-target color type from this. We use RGBA8 (0x8058 == GL_RGBA8). */
#define GL_RGBA8                  0x8058

/* SDL_WINDOW_METAL — request a window backed by a Metal-capable view (so
 * SDL_Metal_CreateView / SDL_Metal_GetLayer give us a CAMetalLayer). */
#define SDL_WINDOW_METAL          0x20000000u
/* SDL_WINDOW_ALLOW_HIGHDPI — the window's backing store is the true Retina pixel
 * size, so we render at native resolution and present 1:1 (no double upscale /
 * blur). docs/GPU.md. */
#define SDL_WINDOW_ALLOW_HIGHDPI  0x00002000u
/* MTLPixelFormatBGRA8Unorm = 80 — the CAMetalLayer pixel format; matches the
 * host 0xAARRGGBB packing so Skia's BGRA surface renders correctly. */
#define MTL_PIXELFORMAT_BGRA8     80

/* ---- dlsym'd entry points ---- */
static void *s_lib = NULL;
static int   (*s_Init)(uint32_t);
static void *(*s_CreateWindow)(const char *, int, int, int, int, uint32_t);
static void *(*s_CreateRenderer)(void *, int, uint32_t);
static void *(*s_CreateTexture)(void *, uint32_t, int, int, int);
static int   (*s_UpdateTexture)(void *, const void *, const void *, int);
static int   (*s_RenderClear)(void *);
static int   (*s_RenderCopy)(void *, void *, const void *, const void *);
static void  (*s_RenderPresent)(void *);
static int   (*s_PollEvent)(void *);
static void  (*s_DestroyTexture)(void *);
static void  (*s_DestroyRenderer)(void *);
static void  (*s_DestroyWindow)(void *);
static const char *(*s_GetError)(void);

/* GL-context entry points (resolved best-effort; a miss only disables the GPU
 * path — the raster path above never depends on them). These are the SDL side
 * of the ADR's `rx_gpu_context` seam: SDL owns creating + making-current the GL
 * context; Skia's gr_* consumes it (docs/GPU.md). */
static int   (*s_GL_SetAttribute)(int attr, int value);
static void *(*s_GL_CreateContext)(void *window);
static int   (*s_GL_MakeCurrent)(void *window, void *context);
static void *(*s_GL_GetProcAddress)(const char *proc);
static void  (*s_GL_SwapWindow)(void *window);
static void  (*s_GL_GetDrawableSize)(void *window, int *w, int *h);
static int   (*s_GL_SetSwapInterval)(int interval);
static void  (*s_GL_DeleteContext)(void *context);

/* SDL Metal-view entry points (resolved best-effort; a miss only forecloses the
 * windowed-Metal path). SDL_Metal_CreateView attaches a Metal-backed view to a
 * SDL_WINDOW_METAL window; SDL_Metal_GetLayer returns its CAMetalLayer (docs/GPU.md).
 * The Apple side of the rx_gpu_context seam: SDL owns the view + layer, Skia's
 * gr_* renders into the layer's drawable texture. */
static void *(*s_Metal_CreateView)(void *window);
static void *(*s_Metal_GetLayer)(void *view);
static void  (*s_Metal_DestroyView)(void *view);
static void  (*s_Metal_GetDrawableSize)(void *window, int *w, int *h);
static int   (*s_GetRendererOutputSize)(void *renderer, int *w, int *h);

/* Objective-C runtime + Metal device, for the CAMetalLayer drawable lifecycle
 * (configure the layer, acquire the next drawable, present it). Resolved lazily
 * in metal_objc_init(); a miss forecloses windowed Metal. */
static void *(*s_objc_msgSend)(void *, void *);
static void *(*s_sel)(const char *);
static int   s_metal_have_fns = 0;
static int   s_metal_objc_ok = 0;

/* ---- single-window state ---- */
static void *s_win = NULL;
static void *s_ren = NULL;
static void *s_tex = NULL;
static int   s_tex_w = 0;
static int   s_tex_h = 0;
static int   s_scale = 1;   /* logical window = framebuffer * scale */
static int   s_out_w = 0;    /* renderer output (backing) pixel size — Retina-true */
static int   s_out_h = 0;    /* used by the pump to map device->framebuffer coords */
static void *s_owner = NULL; /* the RxHost the window belongs to */

/* ---- GL-window state (the GPU presenter; mutually exclusive with the raster
 * texture path above for a given window). s_glctx != NULL means this window was
 * created SDL_WINDOW_OPENGL with a current GL context — present is then a buffer
 * swap, not a texture blit. */
static void *s_glctx = NULL;   /* SDL_GLContext for s_win, or NULL */
static int   s_gl_have_fns = 0;

/* ---- Metal-window state (the on-screen Metal presenter; mutually exclusive
 * with the GL/raster paths for a given window). s_mtl_layer != NULL means this
 * window is a SDL_WINDOW_METAL window whose CAMetalLayer we render into per
 * frame. s_mtl_drawable is the drawable currently acquired for the in-flight
 * frame (NULL between frames). docs/GPU.md. */
static void *s_mtl_view     = NULL;   /* SDL_MetalView */
static void *s_mtl_layer    = NULL;   /* CAMetalLayer (id) */
static void *s_mtl_device   = NULL;   /* id<MTLDevice> (the rx_metal singleton) */
static void *s_mtl_queue    = NULL;   /* id<MTLCommandQueue> (rx_metal singleton) */
static void *s_mtl_drawable = NULL;   /* the frame's CAMetalDrawable, or NULL */

static void *sym(const char *name) { return dlsym(s_lib, name); }

static int load_sdl(void) {
    if (s_lib) return 1;
    /* Host-aware: try a candidate list, first that dlopen()s wins. macOS does
     * NOT have a standalone SDL2 on the default loader path and dlopen does not
     * search Homebrew dirs, so we list the macOS dylib names + the Homebrew full
     * paths (arm64 /opt/homebrew, x86_64 /usr/local) + the framework, and keep
     * the Linux SO name. $RUXEN_CANVAS_SDL2 overrides with an explicit path. A
     * miss on ALL candidates → return 0 → clean headless fallback (the
     * framebuffer/event path keeps working). */
    static const char *const sdl_candidates[] = {
        "libSDL2-2.0.0.dylib",                          /* macOS, on the path */
        "libSDL2.dylib",
        "/opt/homebrew/lib/libSDL2-2.0.0.dylib",        /* arm64 Homebrew */
        "/usr/local/lib/libSDL2-2.0.0.dylib",           /* x86_64 Homebrew */
        "/Library/Frameworks/SDL2.framework/SDL2",      /* framework install */
        "libSDL2-2.0.so.0",                             /* Linux */
    };
    const char *env = getenv("RUXEN_CANVAS_SDL2");
    if (env && env[0]) s_lib = dlopen(env, RTLD_NOW | RTLD_LOCAL);
    for (size_t i = 0; !s_lib && i < sizeof(sdl_candidates) / sizeof(sdl_candidates[0]); i++) {
        s_lib = dlopen(sdl_candidates[i], RTLD_NOW | RTLD_LOCAL);
    }
    if (!s_lib) return 0;
    s_Init           = (int (*)(uint32_t))sym("SDL_Init");
    s_CreateWindow   = (void *(*)(const char *, int, int, int, int, uint32_t))sym("SDL_CreateWindow");
    s_CreateRenderer = (void *(*)(void *, int, uint32_t))sym("SDL_CreateRenderer");
    s_CreateTexture  = (void *(*)(void *, uint32_t, int, int, int))sym("SDL_CreateTexture");
    s_UpdateTexture  = (int (*)(void *, const void *, const void *, int))sym("SDL_UpdateTexture");
    s_RenderClear    = (int (*)(void *))sym("SDL_RenderClear");
    s_RenderCopy     = (int (*)(void *, void *, const void *, const void *))sym("SDL_RenderCopy");
    s_RenderPresent  = (void (*)(void *))sym("SDL_RenderPresent");
    s_PollEvent      = (int (*)(void *))sym("SDL_PollEvent");
    s_DestroyTexture = (void (*)(void *))sym("SDL_DestroyTexture");
    s_DestroyRenderer= (void (*)(void *))sym("SDL_DestroyRenderer");
    s_DestroyWindow  = (void (*)(void *))sym("SDL_DestroyWindow");
    s_GetError       = (const char *(*)(void))sym("SDL_GetError");
    if (!s_Init || !s_CreateWindow || !s_CreateRenderer || !s_CreateTexture ||
        !s_UpdateTexture || !s_RenderClear || !s_RenderCopy || !s_RenderPresent ||
        !s_PollEvent) {
        /* never leave a half-initialized handle: the s_lib guard above
         * would otherwise report success with NULL function pointers */
        dlclose(s_lib);
        s_lib = NULL;
        return 0;
    }
    /* GL entry points are OPTIONAL: resolve them, but a miss only forecloses the
     * GPU path (s_gl_have_fns stays 0). The raster window above does not need
     * them, so we never fail load_sdl for their absence. */
    s_GL_SetAttribute   = (int (*)(int, int))sym("SDL_GL_SetAttribute");
    s_GL_CreateContext  = (void *(*)(void *))sym("SDL_GL_CreateContext");
    s_GL_MakeCurrent    = (int (*)(void *, void *))sym("SDL_GL_MakeCurrent");
    s_GL_GetProcAddress = (void *(*)(const char *))sym("SDL_GL_GetProcAddress");
    s_GL_SwapWindow     = (void (*)(void *))sym("SDL_GL_SwapWindow");
    s_GL_GetDrawableSize= (void (*)(void *, int *, int *))sym("SDL_GL_GetDrawableSize");
    s_GL_SetSwapInterval= (int (*)(int))sym("SDL_GL_SetSwapInterval");
    s_GL_DeleteContext  = (void (*)(void *))sym("SDL_GL_DeleteContext");
    s_gl_have_fns = (s_GL_CreateContext && s_GL_MakeCurrent && s_GL_GetProcAddress &&
                     s_GL_SwapWindow) ? 1 : 0;

    /* SDL Metal-view entry points are OPTIONAL too (a miss only forecloses the
     * windowed-Metal path; present as of SDL 2.0.8+, macOS). */
    s_Metal_CreateView  = (void *(*)(void *))sym("SDL_Metal_CreateView");
    s_Metal_GetLayer    = (void *(*)(void *))sym("SDL_Metal_GetLayer");
    s_Metal_DestroyView = (void (*)(void *))sym("SDL_Metal_DestroyView");
    s_Metal_GetDrawableSize = (void (*)(void *, int *, int *))sym("SDL_Metal_GetDrawableSize");
    s_GetRendererOutputSize = (int (*)(void *, int *, int *))sym("SDL_GetRendererOutputSize");
    s_metal_have_fns = (s_Metal_CreateView && s_Metal_GetLayer) ? 1 : 0;
    return 1;
}

/* Resolve the Objective-C runtime (objc_msgSend / sel_registerName) once — the
 * CAMetalLayer drawable lifecycle (configure / nextDrawable / present) is driven
 * through message sends. A miss forecloses windowed Metal. */
static int metal_objc_init(void) {
    if (s_metal_objc_ok) return 1;
    void *objc = dlopen("/usr/lib/libobjc.A.dylib", RTLD_NOW | RTLD_GLOBAL);
    if (!objc) objc = dlopen("libobjc.A.dylib", RTLD_NOW | RTLD_GLOBAL);
    if (!objc) return 0;
    s_objc_msgSend = (void *(*)(void *, void *))dlsym(objc, "objc_msgSend");
    s_sel          = (void *(*)(const char *))dlsym(objc, "sel_registerName");
    /* QuartzCore must be loaded so CAMetalLayer + its selectors are present. */
    dlopen("/System/Library/Frameworks/QuartzCore.framework/QuartzCore", RTLD_NOW | RTLD_GLOBAL);
    s_metal_objc_ok = (s_objc_msgSend && s_sel) ? 1 : 0;
    return s_metal_objc_ok;
}

/* Tiny objc message-send wrappers, cast per signature for the arm64/x86_64 ABI
 * (no struct returns, no variadics in this set). */
static void *mtl_msg(void *obj, const char *selname) {
    return s_objc_msgSend(obj, s_sel(selname));
}
static void mtl_msg_p(void *obj, const char *selname, void *arg) {
    void (*m)(void *, void *, void *) = (void (*)(void *, void *, void *))s_objc_msgSend;
    m(obj, s_sel(selname), arg);
}
static void mtl_msg_u(void *obj, const char *selname, unsigned long arg) {
    void (*m)(void *, void *, unsigned long) =
        (void (*)(void *, void *, unsigned long))s_objc_msgSend;
    m(obj, s_sel(selname), arg);
}
/* [layer setDrawableSize:(CGSize){w,h}] — CGSize is two doubles by value, passed
 * in the SIMD/float regs on arm64, so we need the matching cast. */
static void mtl_set_drawable_size(void *layer, double w, double h) {
    void (*m)(void *, void *, double, double) =
        (void (*)(void *, void *, double, double))s_objc_msgSend;
    m(layer, s_sel("setDrawableSize:"), w, h);
}

/* Whether opening a REAL OS window is permitted in this process. macOS forbids
 * CoreFoundation / AppKit / Metal windowing after fork() without exec(); the
 * `ruxen test` harness forks per test case, so opening real windows there is
 * unsafe and, under parallel fan-out, races the WindowServer (flaky). So under
 * the test harness (detected via RUXEN_TEST_FORMAT) we default to NOT opening a
 * real window — the windowing entry points return RXC_ERR_NO_SDL and callers
 * fall back to the deterministic headless framebuffer path. An operator (or a
 * single-threaded live run) can force real windows with RUXEN_CANVAS_WINDOW=1.
 * Outside the harness (real apps), windows always open. The standalone examples
 * (examples/metal_window_verify.c) prove the on-screen path directly. */
static int rx_window_allowed(void) {
    const char *force = getenv("RUXEN_CANVAS_WINDOW");
    if (force && force[0] == '1') return 1;
    if (getenv("RUXEN_TEST_FORMAT")) return 0;   /* forked test harness: headless */
    return 1;
}

/* Create the OS window at scale x the host framebuffer (HiDPI-correct: the
 * backing store is the true Retina pixel size, presented 1:1). The pump maps
 * pointer coords back to framebuffer coordinates. */
int64_t ruxen_canvas_window_show(int64_t self, int64_t title, int64_t scale) {
    RxHostPrefix *h = (RxHostPrefix *)self;
    const char *t = (const char *)title;
    if (scale == 0) scale = 1; /* 0 = auto (callers without a preference) */
    if (!h || !t || scale < 1 || scale > 8) return RXC_ERR_BAD_ARGS;
    if (s_win) {
        /* idempotent for the owning host; an error for any other host —
         * one window per process, and silently "succeeding" would leave
         * the second Window presenting someone else's framebuffer */
        return s_owner == (void *)h ? RXC_OK : RXC_ERR_BUSY;
    }
    if (!rx_window_allowed()) return RXC_ERR_NO_SDL;   /* forked harness: headless */
    if (!load_sdl()) return RXC_ERR_NO_SDL;
    if (s_Init(SDL_INIT_VIDEO) != 0) return RXC_ERR_NO_SDL;

    s_scale = (int)scale;
    /* ALLOW_HIGHDPI: the backing store is the true Retina pixel size, so macOS
     * does NOT upscale the window a second time on top of our scale. The texture
     * holds the logical framebuffer; RenderCopy maps it to the renderer's output
     * (backing) size once. Without this flag the window was upscaled twice
     * (our scale × Retina) — double blur. */
    s_win = s_CreateWindow(t,
                           (int)SDL_WINDOWPOS_CENTERED, (int)SDL_WINDOWPOS_CENTERED,
                           h->width * s_scale, h->height * s_scale,
                           SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_RESIZABLE);
    if (!s_win) return RXC_ERR_NO_SDL;
    s_ren = s_CreateRenderer(s_win, -1, 0);
    if (!s_ren) { s_DestroyWindow(s_win); s_win = NULL; return RXC_ERR_NO_SDL; }
    s_tex = s_CreateTexture(s_ren, SDL_PIXELFORMAT_ARGB8888,
                            SDL_TEXTUREACCESS_STREAMING, h->width, h->height);
    if (!s_tex) {
        s_DestroyRenderer(s_ren); s_ren = NULL;
        s_DestroyWindow(s_win);   s_win = NULL;
        return RXC_ERR_NO_SDL;
    }
    s_tex_w = h->width;
    s_tex_h = h->height;
    /* The real backing pixel size (Retina ~2x the logical window). */
    s_out_w = h->width * s_scale;
    s_out_h = h->height * s_scale;
    if (s_GetRendererOutputSize) {
        int ow = 0, oh = 0;
        if (s_GetRendererOutputSize(s_ren, &ow, &oh) == 0 && ow > 0 && oh > 0) {
            s_out_w = ow;
            s_out_h = oh;
        }
    }
    s_owner = (void *)h;
    return RXC_OK;
}

/* Whether a window is currently up (0/1). */
int64_t ruxen_canvas_window_is_shown(void) {
    return s_win ? 1 : 0;
}

/* Blit the host framebuffer to the window. host pixels are 0xAARRGGBB,
 * exactly SDL_PIXELFORMAT_ARGB8888 — a straight upload, no conversion. */
int64_t ruxen_canvas_window_present(int64_t self) {
    RxHostPrefix *h = (RxHostPrefix *)self;
    if (!h || !h->pixels) return RXC_ERR_BAD_ARGS;
    if (!s_win || s_owner != (void *)h ||
        h->width != s_tex_w || h->height != s_tex_h) return RXC_ERR_PRESENT;
    if (s_UpdateTexture(s_tex, NULL, h->pixels, h->width * 4) != 0) return RXC_ERR_PRESENT;
    if (s_RenderClear(s_ren) != 0) return RXC_ERR_PRESENT;
    if (s_RenderCopy(s_ren, s_tex, NULL, NULL) != 0) return RXC_ERR_PRESENT;
    s_RenderPresent(s_ren);
    return RXC_OK;
}

/* ---- GL-context seam (the GPU backend's `rx_gpu_context`, docs/GPU.md) ----
 *
 * These create + make-current an SDL GL context for a window, and expose the
 * three things Skia's Ganesh GL backend needs from the host: a GL proc loader
 * (to build the GrGLInterface), the default-framebuffer id + drawable size (to
 * build the GrBackendRenderTarget), and a buffer swap (to present). skia_shim.c
 * calls these through declared prototypes; it never touches SDL directly.
 *
 * EVERYTHING here fails cleanly and bounded: a host with no SDL / no display /
 * no GL entry points gets RXC_ERR_* and the caller falls back to the raster
 * path. No call blocks — SDL_GL_CreateContext returns promptly (or NULL) on a
 * headless host; we never spin. */

/* prototypes for the GL function loader skia_shim.c hands to Skia */
int64_t ruxen_canvas_window_gl_get_proc(int64_t name);

/* Create an SDL_WINDOW_OPENGL window for this host and make a GL context
 * current on it. Returns RXC_OK on success (s_glctx set), or RXC_ERR_NO_SDL /
 * RXC_ERR_BUSY / RXC_ERR_PRESENT on a clean failure. Idempotent for the owning
 * host. On ANY failure the window state is left torn down so the caller can
 * fall back to the raster show path. */
int64_t ruxen_canvas_window_create_gl(int64_t self, int64_t win_scale) {
    RxHostPrefix *h = (RxHostPrefix *)self;
    if (!h) return RXC_ERR_BAD_ARGS;
    int ws = (int)win_scale;
    if (ws < 1) ws = 1;
    if (ws > 8) ws = 8;
    if (s_win) {
        /* a window already exists: only OK if it is THIS host's GL window */
        if (s_owner == (void *)h && s_glctx) return RXC_OK;
        return RXC_ERR_BUSY;
    }
    if (!rx_window_allowed()) return RXC_ERR_NO_SDL;   /* forked harness: headless */
    if (!load_sdl()) return RXC_ERR_NO_SDL;
    if (!s_gl_have_fns) return RXC_ERR_NO_SDL;   /* no GL entry points */
    if (s_Init(SDL_INIT_VIDEO) != 0) return RXC_ERR_NO_SDL;

    /* Request a GL context Skia can render into: 8-bit RGBA, a stencil buffer
     * (Ganesh uses stencil for some clips/AA), double-buffered, core profile.
     * SetAttribute is best-effort — failures here are not fatal; the context
     * create is the real gate. */
    if (s_GL_SetAttribute) {
        s_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
        s_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
        s_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
        s_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
        s_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
        s_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
        s_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        s_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    }

    /* ALLOW_HIGHDPI: SDL_GL_GetDrawableSize then returns the true Retina backing
     * size, which we size the Ganesh GL surface to — native-resolution, crisp.
     * win_scale opens a larger on-screen window; the design->backing content
     * scale fills it crisply (see window_create_metal). */
    s_win = s_CreateWindow("ruxen-gl",
                           (int)SDL_WINDOWPOS_CENTERED, (int)SDL_WINDOWPOS_CENTERED,
                           h->width * ws, h->height * ws,
                           SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_RESIZABLE);
    if (!s_win) return RXC_ERR_NO_SDL;
    s_glctx = s_GL_CreateContext(s_win);
    if (!s_glctx) {
        s_DestroyWindow(s_win); s_win = NULL;
        return RXC_ERR_PRESENT;
    }
    if (s_GL_MakeCurrent(s_win, s_glctx) != 0) {
        s_GL_DeleteContext(s_glctx); s_glctx = NULL;
        s_DestroyWindow(s_win);      s_win = NULL;
        return RXC_ERR_PRESENT;
    }
    if (s_GL_SetSwapInterval) s_GL_SetSwapInterval(1);   /* vsync; best-effort */
    /* s_scale is the DESIGN->window-point factor (the show_gpu_scaled factor),
     * used by the pump to map mouse POINTS (0..width*ws) back to design coords
     * (/ ws). NOT the backing/dpr scale — SDL reports mouse in logical points
     * under ALLOW_HIGHDPI; the backing drawable size is tracked separately for
     * the render surface. */
    s_scale = ws;
    s_tex_w = h->width;     /* reuse the size guard fields for the GL window */
    s_tex_h = h->height;
    s_owner = (void *)h;
    return RXC_OK;
}

/* Whether the current window is a GL window owned by `self` (0/1). */
int64_t ruxen_canvas_window_is_gl(int64_t self) {
    return (s_glctx && s_owner == (void *)self) ? 1 : 0;
}

/* Resolve a GL function by name through SDL_GL_GetProcAddress, returned as an
 * int64 pointer (0 when unavailable). This is the loader Skia's
 * gr_glinterface_assemble_*_interface calls back into. The GL context must be
 * current (it is, after create_gl). */
int64_t ruxen_canvas_window_gl_get_proc(int64_t name) {
    const char *n = (const char *)name;
    if (!n || !s_glctx || !s_GL_GetProcAddress) return 0;
    return (int64_t)s_GL_GetProcAddress(n);
}

/* The drawable pixel size of the GL window (HiDPI-correct via
 * SDL_GL_GetDrawableSize). Packs width in the high 32 bits, height in the low
 * 32 — one int64 return, no out-params across the ABI. 0 when no GL window. */
int64_t ruxen_canvas_window_gl_drawable_size(int64_t self) {
    if (!s_glctx || s_owner != (void *)self) return 0;
    int w = s_tex_w, hh = s_tex_h;
    if (s_GL_GetDrawableSize) s_GL_GetDrawableSize(s_win, &w, &hh);
    if (w <= 0 || hh <= 0) return 0;
    return ((int64_t)(uint32_t)w << 32) | (int64_t)(uint32_t)hh;
}

/* Present a GPU frame: swap the GL back buffer to the screen. Returns RXC_OK
 * or RXC_ERR_PRESENT when there is no GL window for this host. */
int64_t ruxen_canvas_window_gl_present(int64_t self) {
    if (!s_glctx || s_owner != (void *)self || !s_GL_SwapWindow) return RXC_ERR_PRESENT;
    s_GL_SwapWindow(s_win);
    return RXC_OK;
}

/* ---- on-screen Metal: CAMetalLayer + drawable lifecycle (docs/GPU.md) ----
 *
 * Create a SDL_WINDOW_METAL window for this host, attach a Metal view, get its
 * CAMetalLayer, and configure it with the (caller-provided) Metal device +
 * command queue — the rx_metal() singleton in skia_shim.c. Per frame the shim
 * calls next_drawable (acquire the layer's next CAMetalDrawable + its MTLTexture
 * for Skia to wrap), renders, then present (presentDrawable + commit). On ANY
 * failure the window is left torn down so the caller can fall back to GL/raster.
 * Bounded + clean on a headless / no-SDL / no-Metal host (no display => no
 * layer/drawable, returns an Err, never blocks). */
int64_t ruxen_canvas_window_create_metal(int64_t self, int64_t device, int64_t queue,
                                         int64_t win_scale) {
    RxHostPrefix *h = (RxHostPrefix *)self;
    if (!h || !device || !queue) return RXC_ERR_BAD_ARGS;
    int ws = (int)win_scale;
    if (ws < 1) ws = 1;
    if (ws > 8) ws = 8;
    if (s_win) {
        if (s_owner == (void *)h && s_mtl_layer) return RXC_OK;
        return RXC_ERR_BUSY;
    }
    if (!rx_window_allowed()) return RXC_ERR_NO_SDL;   /* forked harness: headless */
    if (!load_sdl()) return RXC_ERR_NO_SDL;
    if (!s_metal_have_fns) return RXC_ERR_NO_SDL;   /* no SDL_Metal_* */
    if (!metal_objc_init()) return RXC_ERR_NO_SDL;  /* no objc runtime */
    if (s_Init(SDL_INIT_VIDEO) != 0) return RXC_ERR_NO_SDL;

    /* ALLOW_HIGHDPI so the view's backing store is the true Retina pixel size;
     * we render the Skia Metal surface at THAT size (queried below) and present
     * 1:1 — crisp by construction. win_scale opens a LARGER window (win_scale ×
     * the logical/design size) for on-screen size; the design->backing content
     * scale (begin_frame) then fills it crisply at native density — so a 2x
     * window of a 320-design UI is 640 logical points, 1280 backing pixels on a
     * 2x display, content scale 4. */
    s_win = s_CreateWindow("ruxen-metal",
                           (int)SDL_WINDOWPOS_CENTERED, (int)SDL_WINDOWPOS_CENTERED,
                           h->width * ws, h->height * ws,
                           SDL_WINDOW_METAL | SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!s_win) return RXC_ERR_NO_SDL;

    s_mtl_view = s_Metal_CreateView(s_win);
    if (!s_mtl_view) { s_DestroyWindow(s_win); s_win = NULL; return RXC_ERR_PRESENT; }
    s_mtl_layer = s_Metal_GetLayer(s_mtl_view);
    if (!s_mtl_layer) {
        s_Metal_DestroyView(s_mtl_view); s_mtl_view = NULL;
        s_DestroyWindow(s_win); s_win = NULL;
        return RXC_ERR_PRESENT;
    }

    /* The true backing (drawable) pixel size — on Retina ~2x the logical size.
     * We size the layer's drawable AND the Skia surface to this, so the GPU
     * renders at native resolution (crisp text/shapes), presented 1:1. */
    int dpw = h->width, dph = h->height;
    if (s_Metal_GetDrawableSize) s_Metal_GetDrawableSize(s_win, &dpw, &dph);
    if (dpw <= 0 || dph <= 0) { dpw = h->width; dph = h->height; }

    s_mtl_device = (void *)device;
    s_mtl_queue  = (void *)queue;
    /* Configure the layer: our device, BGRA8 (matches the host packing), and
     * framebufferOnly=NO so Skia may render into the drawable's texture. The
     * drawableSize is the native backing size (HiDPI-correct). */
    mtl_msg_p(s_mtl_layer, "setDevice:", s_mtl_device);
    mtl_msg_u(s_mtl_layer, "setPixelFormat:", MTL_PIXELFORMAT_BGRA8);
    mtl_msg_u(s_mtl_layer, "setFramebufferOnly:", 0);
    mtl_set_drawable_size(s_mtl_layer, (double)dpw, (double)dph);

    /* s_scale = the DESIGN->window-point factor (the show_gpu_scaled factor), so
     * the pump maps mouse POINTS (0..width*ws) back to design coords (/ ws). This
     * is SEPARATE from the backing/dpr scale: s_tex_w/h track the native backing
     * (drawable) size for the render surface; the pump never uses them. SDL
     * reports mouse in logical points under ALLOW_HIGHDPI, so the dpr does not
     * enter the pump. */
    s_scale = ws;
    s_tex_w = dpw;
    s_tex_h = dph;
    s_owner = (void *)h;
    return RXC_OK;
}

/* Whether the current window is a Metal window owned by `self` (0/1). */
int64_t ruxen_canvas_window_is_metal(int64_t self) {
    return (s_mtl_layer && s_owner == (void *)self) ? 1 : 0;
}

/* True (1) when SDL2 itself could be dlopen'd on this host (the host-aware
 * loader found a usable libSDL2). A capability probe that does NOT open a window
 * — it proves the loader resolves SDL2 here even when the harness is headless.
 * 0 when no SDL2 is installed anywhere on the candidate paths. */
int64_t ruxen_canvas_sdl_available(void) {
    return load_sdl() ? 1 : 0;
}

/* True (1) when the on-screen Metal present path COULD run: SDL2 loaded AND its
 * SDL_Metal_CreateView/GetLayer resolved AND the objc runtime is reachable. Does
 * NOT prove a display exists (nextDrawable may still be nil headless). */
int64_t ruxen_canvas_window_metal_available(void) {
    if (!load_sdl() || !s_metal_have_fns) return 0;
    return metal_objc_init() ? 1 : 0;
}

/* Acquire the layer's next drawable for this frame and return its MTLTexture as
 * an int64 handle (0 on failure — e.g. headless, no display, so nextDrawable is
 * nil). The caller (skia_shim) wraps this texture in a Metal backend render
 * target. The drawable is held in s_mtl_drawable until present releases it; a
 * second call without a present releases the prior unpresented drawable first
 * (no leak). */
int64_t ruxen_canvas_window_metal_next_drawable(int64_t self) {
    if (!s_mtl_layer || s_owner != (void *)self) return 0;
    /* a prior unpresented drawable would leak — drop our reference to it. */
    s_mtl_drawable = NULL;
    void *drawable = mtl_msg(s_mtl_layer, "nextDrawable");
    if (!drawable) return 0;                  /* no display / off-screen */
    void *texture = mtl_msg(drawable, "texture");
    if (!texture) return 0;
    s_mtl_drawable = drawable;
    return (int64_t)texture;
}

/* The drawable pixel size of the Metal layer (HiDPI-correct). Packs width<<32 |
 * height. 0 when no Metal window. We track it from drawableSize set at create;
 * the layer's drawableSize is authoritative on HiDPI. */
int64_t ruxen_canvas_window_metal_drawable_size(int64_t self) {
    if (!s_mtl_layer || s_owner != (void *)self) return 0;
    int w = s_tex_w, hh = s_tex_h;
    if (w <= 0 || hh <= 0) return 0;
    return ((int64_t)(uint32_t)w << 32) | (int64_t)(uint32_t)hh;
}

/* Present the frame's acquired drawable: enqueue a command buffer on the queue
 * that presents the drawable, then commit it. Returns RXC_OK, or RXC_ERR_PRESENT
 * when there is no Metal window / no acquired drawable. After present the
 * drawable is consumed (next frame acquires a fresh one). */
int64_t ruxen_canvas_window_metal_present(int64_t self) {
    if (!s_mtl_layer || s_owner != (void *)self) return RXC_ERR_PRESENT;
    if (!s_mtl_drawable || !s_mtl_queue) return RXC_ERR_PRESENT;
    /* id cmdbuf = [queue commandBuffer]; [cmdbuf presentDrawable:drawable];
     * [cmdbuf commit]; */
    void *cmdbuf = mtl_msg(s_mtl_queue, "commandBuffer");
    if (!cmdbuf) { s_mtl_drawable = NULL; return RXC_ERR_PRESENT; }
    mtl_msg_p(cmdbuf, "presentDrawable:", s_mtl_drawable);
    mtl_msg(cmdbuf, "commit");
    s_mtl_drawable = NULL;     /* consumed; next frame acquires a fresh drawable */
    return RXC_OK;
}

/* Coordinate mapping (the pump): SDL2 reports mouse in LOGICAL window POINTS
 * (0..width*ws), not backing pixels, even under ALLOW_HIGHDPI. The app works in
 * DESIGN coords (0..width). The window opens at width*ws (the show /
 * show_scaled / show_gpu_scaled factor), so point -> design is point / ws =
 * point / s_scale. s_scale is set to the show factor by EVERY windowed backend
 * (raster, GL, Metal), so this maps points to design coords on all of them. The
 * Retina backing/dpr does NOT enter here (SDL accounts for it in the point
 * coordinates; the backing size is tracked separately, only for the render
 * surface). Factored out so the headless pump-mapping test exercises the EXACT
 * arithmetic the pump uses. */
static double rx_map_point(int32_t window_point) {
    return (double)window_point / (double)(s_scale > 0 ? s_scale : 1);
}

/* shim hook: invalidate a windowed GL host's persistent GPU surface so the next
 * begin_frame rebuilds it at the new backing size (defined in skia_shim.c). */
void ruxen_canvas_host_gl_invalidate_surface(int64_t self);

/* On a window resize: the design->point factor s_scale is unchanged (the design
 * stays h->width), but the backing drawable grows/shrinks. For Metal, update the
 * layer's drawableSize to the new backing pixels so the next frame's drawable +
 * GPU surface are the new size (begin_frame picks them up via the drawable). For
 * GL (persistent surface), tell the shim to drop + rebuild the surface at the new
 * size on the next frame. */
static void rx_window_on_resized(int64_t self) {
    if (s_mtl_layer && s_Metal_GetDrawableSize) {
        int dpw = 0, dph = 0;
        s_Metal_GetDrawableSize(s_win, &dpw, &dph);
        if (dpw > 0 && dph > 0) {
            mtl_set_drawable_size(s_mtl_layer, (double)dpw, (double)dph);
            s_tex_w = dpw;   /* window_metal_drawable_size now returns the new size */
            s_tex_h = dph;
        }
    } else if (s_glctx) {
        /* GL: the drawable size is queried fresh each rebuild; just invalidate. */
        ruxen_canvas_host_gl_invalidate_surface(self);
    }
}

int64_t ruxen_canvas_window_pump(int64_t self) {
    if (!self) return -RXC_ERR_BAD_ARGS;
    if (!s_win || s_owner != (void *)self) return 0;
    int64_t forwarded = 0;
    unsigned char ev[64];
    while (s_PollEvent(ev)) {
        uint32_t type;
        memcpy(&type, ev, sizeof type);
        int32_t xi, yi, sym;
        switch (type) {
        case SDL_MOUSEMOTION_EV:
            memcpy(&xi, ev + 20, 4); memcpy(&yi, ev + 24, 4);
            ruxen_canvas_push_event(self, RX_EV_POINTER_MOVE,
                                    rx_map_point(xi), rx_map_point(yi));
            forwarded++;
            break;
        case SDL_MOUSEBUTTONDOWN_EV:
            memcpy(&xi, ev + 20, 4); memcpy(&yi, ev + 24, 4);
            ruxen_canvas_push_event(self, RX_EV_POINTER_DOWN,
                                    rx_map_point(xi), rx_map_point(yi));
            forwarded++;
            break;
        case SDL_MOUSEBUTTONUP_EV:
            memcpy(&xi, ev + 20, 4); memcpy(&yi, ev + 24, 4);
            ruxen_canvas_push_event(self, RX_EV_POINTER_UP,
                                    rx_map_point(xi), rx_map_point(yi));
            forwarded++;
            break;
        case SDL_MOUSEWHEEL_EV:
            /* SDL_MouseWheelEvent: Sint32 x @ 16, y @ 20 (wheel deltas; +y = up,
             * +x = right). Wheel deltas are integer "clicks", not coords — NOT
             * scaled by s_scale. Forwarded as Event.Scroll(dx, dy). */
            memcpy(&xi, ev + 16, 4); memcpy(&yi, ev + 20, 4);
            ruxen_canvas_push_event(self, RX_EV_SCROLL, (double)xi, (double)yi);
            forwarded++;
            break;
        case SDL_KEYDOWN_EV:
            memcpy(&sym, ev + 20, 4);
            ruxen_canvas_push_event(self, RX_EV_KEY_DOWN, (double)sym, 0.0);
            forwarded++;
            break;
        case SDL_WINDOWEVENT_EV: {
            /* SDL_WindowEvent: Uint8 event @ 12, Sint32 data1 @ 16, data2 @ 20.
             * On RESIZED, data1/data2 are the new window size in POINTS; we emit
             * Resize in DESIGN coords (/ s_scale) and re-size the backing surface. */
            unsigned char subtype = ev[12];
            if (subtype == SDL_WINDOWEVENT_RESIZED) {
                int32_t w1, h1;
                memcpy(&w1, ev + 16, 4); memcpy(&h1, ev + 20, 4);
                rx_window_on_resized(self);
                ruxen_canvas_push_event(self, RX_EV_RESIZE,
                                        rx_map_point(w1), rx_map_point(h1));
                forwarded++;
            }
            break;
        }
        case SDL_QUIT_EV:
            ruxen_canvas_push_event(self, RX_EV_CLOSE, 0.0, 0.0);
            forwarded++;
            break;
        default:
            break;
        }
    }
    return forwarded;
}

/* ---- test seam for the pump's coordinate mapping (headless) ----
 *
 * The real pump only runs with a live SDL window (gated off in the test
 * harness), so we cannot drive it with synthetic SDL events headless. This seam
 * sets s_scale directly and runs the EXACT mapping the pump uses (rx_map_point)
 * on a synthetic window-point pointer event, pushing the design-mapped result
 * into the host's ring. The test then polls it back and asserts point / scale.
 * Test-only; mirrors the pump's arithmetic so a regression in either is caught. */
int64_t ruxen_canvas_window_pump_test_pointer(int64_t self, int64_t show_scale,
                                              int64_t win_x, int64_t win_y) {
    if (!self) return RXC_ERR_BAD_ARGS;
    int saved = s_scale;
    s_scale = (show_scale >= 1) ? (int)show_scale : 1;
    ruxen_canvas_push_event(self, RX_EV_POINTER_DOWN,
                            rx_map_point((int32_t)win_x), rx_map_point((int32_t)win_y));
    s_scale = saved;
    return RXC_OK;
}

/* Tear the window down (idempotent). The dlopen handle stays cached.
 *
 * Teardown order matters for the GPU path (docs/GPU.md): the Skia GPU surface +
 * gr_direct_context are released by skia_shim.c's host_drop BEFORE this runs
 * (host_drop calls note_host_dropped -> here). By the time we delete the GL
 * context, no Skia object still references it. The GL context is deleted before
 * the window it was created against, the reverse of creation order. */
int64_t ruxen_canvas_window_destroy(void) {
    if (s_tex) { s_DestroyTexture(s_tex); s_tex = NULL; }
    if (s_ren) { s_DestroyRenderer(s_ren); s_ren = NULL; }
    if (s_glctx) { if (s_GL_DeleteContext) s_GL_DeleteContext(s_glctx); s_glctx = NULL; }
    /* Metal: the GPU surface + gr_context referencing the layer are released by
     * skia_shim's host_drop BEFORE this runs. Any in-flight drawable was already
     * consumed at present (or is dropped here). Order: drawable -> layer (owned
     * by the view) -> view -> window — the reverse of creation. We do not own
     * the device/queue (rx_metal singleton), so they are not destroyed here. */
    s_mtl_drawable = NULL;
    s_mtl_layer    = NULL;   /* owned by the view; freed with it */
    if (s_mtl_view) { if (s_Metal_DestroyView) s_Metal_DestroyView(s_mtl_view); s_mtl_view = NULL; }
    s_mtl_device = NULL;
    s_mtl_queue  = NULL;
    if (s_win) { s_DestroyWindow(s_win); s_win = NULL; }
    s_tex_w = s_tex_h = 0;
    s_out_w = s_out_h = 0;
    s_scale = 1;
    s_owner = NULL;
    return RXC_OK;
}

/* Called by skia_shim.c's host_drop: a host being freed while it owns the
 * window must take the window down with it, or present/pump would read
 * freed memory through the stale owner pointer. */
void ruxen_canvas_window_note_host_dropped(int64_t self) {
    if (s_owner == (void *)self) {
        ruxen_canvas_window_destroy();
    }
}
