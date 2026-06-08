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

/* ---- SDL2 constants (from the stable public ABI) ---- */
#define SDL_INIT_VIDEO            0x00000020u
#define SDL_WINDOWPOS_CENTERED    0x2FFF0000u
#define SDL_WINDOW_SHOWN          0x00000004u
#define SDL_WINDOW_OPENGL         0x00000002u
#define SDL_PIXELFORMAT_ARGB8888  0x16362004u
#define SDL_TEXTUREACCESS_STREAMING 1
#define SDL_QUIT_EV               0x100u
#define SDL_KEYDOWN_EV            0x300u
#define SDL_MOUSEMOTION_EV        0x400u
#define SDL_MOUSEBUTTONDOWN_EV    0x401u
#define SDL_MOUSEBUTTONUP_EV      0x402u

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

/* ---- single-window state ---- */
static void *s_win = NULL;
static void *s_ren = NULL;
static void *s_tex = NULL;
static int   s_tex_w = 0;
static int   s_tex_h = 0;
static int   s_scale = 1;   /* window = framebuffer * scale; pump divides */
static void *s_owner = NULL; /* the RxHost the window belongs to */

/* ---- GL-window state (the GPU presenter; mutually exclusive with the raster
 * texture path above for a given window). s_glctx != NULL means this window was
 * created SDL_WINDOW_OPENGL with a current GL context — present is then a buffer
 * swap, not a texture blit. */
static void *s_glctx = NULL;   /* SDL_GLContext for s_win, or NULL */
static int   s_gl_have_fns = 0;

static void *sym(const char *name) { return dlsym(s_lib, name); }

static int load_sdl(void) {
    if (s_lib) return 1;
    s_lib = dlopen("libSDL2-2.0.so.0", RTLD_NOW | RTLD_LOCAL);
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
    return 1;
}

/* Create the OS window at scale x the host framebuffer (the renderer
 * stretches the texture; the pump divides pointer coords back down, so
 * the app always works in framebuffer coordinates). */
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
    if (!load_sdl()) return RXC_ERR_NO_SDL;
    if (s_Init(SDL_INIT_VIDEO) != 0) return RXC_ERR_NO_SDL;

    s_scale = (int)scale;
    s_win = s_CreateWindow(t,
                           (int)SDL_WINDOWPOS_CENTERED, (int)SDL_WINDOWPOS_CENTERED,
                           h->width * s_scale, h->height * s_scale, SDL_WINDOW_SHOWN);
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
int64_t ruxen_canvas_window_create_gl(int64_t self) {
    RxHostPrefix *h = (RxHostPrefix *)self;
    if (!h) return RXC_ERR_BAD_ARGS;
    if (s_win) {
        /* a window already exists: only OK if it is THIS host's GL window */
        if (s_owner == (void *)h && s_glctx) return RXC_OK;
        return RXC_ERR_BUSY;
    }
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

    s_win = s_CreateWindow("ruxen-gl",
                           (int)SDL_WINDOWPOS_CENTERED, (int)SDL_WINDOWPOS_CENTERED,
                           h->width, h->height, SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN);
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
    s_scale = 1;
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

/* Drain SDL's event queue into the host's RxEvent ring. Returns the
 * number of events forwarded (>= 0), or a negative status on bad args.
 * Unknown SDL event types are skipped. */
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
                                    (double)(xi / s_scale), (double)(yi / s_scale));
            forwarded++;
            break;
        case SDL_MOUSEBUTTONDOWN_EV:
            memcpy(&xi, ev + 20, 4); memcpy(&yi, ev + 24, 4);
            ruxen_canvas_push_event(self, RX_EV_POINTER_DOWN,
                                    (double)(xi / s_scale), (double)(yi / s_scale));
            forwarded++;
            break;
        case SDL_MOUSEBUTTONUP_EV:
            memcpy(&xi, ev + 20, 4); memcpy(&yi, ev + 24, 4);
            ruxen_canvas_push_event(self, RX_EV_POINTER_UP,
                                    (double)(xi / s_scale), (double)(yi / s_scale));
            forwarded++;
            break;
        case SDL_KEYDOWN_EV:
            memcpy(&sym, ev + 20, 4);
            ruxen_canvas_push_event(self, RX_EV_KEY_DOWN, (double)sym, 0.0);
            forwarded++;
            break;
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
    if (s_win) { s_DestroyWindow(s_win); s_win = NULL; }
    s_tex_w = s_tex_h = 0;
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
