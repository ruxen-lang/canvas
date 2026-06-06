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
#define SDL_PIXELFORMAT_ARGB8888  0x16362004u
#define SDL_TEXTUREACCESS_STREAMING 1
#define SDL_QUIT_EV               0x100u
#define SDL_KEYDOWN_EV            0x300u
#define SDL_MOUSEMOTION_EV        0x400u
#define SDL_MOUSEBUTTONDOWN_EV    0x401u
#define SDL_MOUSEBUTTONUP_EV      0x402u

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

/* ---- single-window state ---- */
static void *s_win = NULL;
static void *s_ren = NULL;
static void *s_tex = NULL;
static int   s_tex_w = 0;
static int   s_tex_h = 0;

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
        return 0;
    }
    return 1;
}

/* Create the OS window sized to the host's framebuffer. */
int64_t ruxen_canvas_window_show(int64_t self, int64_t title) {
    RxHostPrefix *h = (RxHostPrefix *)self;
    const char *t = (const char *)title;
    if (!h || !t) return RXC_ERR_BAD_ARGS;
    if (s_win) return RXC_OK; /* already shown */
    if (!load_sdl()) return RXC_ERR_NO_SDL;
    if (s_Init(SDL_INIT_VIDEO) != 0) return RXC_ERR_NO_SDL;

    s_win = s_CreateWindow(t,
                           (int)SDL_WINDOWPOS_CENTERED, (int)SDL_WINDOWPOS_CENTERED,
                           h->width, h->height, SDL_WINDOW_SHOWN);
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
    if (!s_win || h->width != s_tex_w || h->height != s_tex_h) return RXC_ERR_PRESENT;
    if (s_UpdateTexture(s_tex, NULL, h->pixels, h->width * 4) != 0) return RXC_ERR_PRESENT;
    if (s_RenderClear(s_ren) != 0) return RXC_ERR_PRESENT;
    if (s_RenderCopy(s_ren, s_tex, NULL, NULL) != 0) return RXC_ERR_PRESENT;
    s_RenderPresent(s_ren);
    return RXC_OK;
}

/* Drain SDL's event queue into the host's RxEvent ring. Returns the
 * number of events forwarded (>= 0), or a negative status on bad args.
 * Unknown SDL event types are skipped. */
int64_t ruxen_canvas_window_pump(int64_t self) {
    if (!self) return -RXC_ERR_BAD_ARGS;
    if (!s_win) return 0;
    int64_t forwarded = 0;
    unsigned char ev[64];
    while (s_PollEvent(ev)) {
        uint32_t type;
        memcpy(&type, ev, sizeof type);
        int32_t xi, yi, sym;
        switch (type) {
        case SDL_MOUSEMOTION_EV:
            memcpy(&xi, ev + 20, 4); memcpy(&yi, ev + 24, 4);
            ruxen_canvas_push_event(self, RX_EV_POINTER_MOVE, (double)xi, (double)yi);
            forwarded++;
            break;
        case SDL_MOUSEBUTTONDOWN_EV:
            memcpy(&xi, ev + 20, 4); memcpy(&yi, ev + 24, 4);
            ruxen_canvas_push_event(self, RX_EV_POINTER_DOWN, (double)xi, (double)yi);
            forwarded++;
            break;
        case SDL_MOUSEBUTTONUP_EV:
            memcpy(&xi, ev + 20, 4); memcpy(&yi, ev + 24, 4);
            ruxen_canvas_push_event(self, RX_EV_POINTER_UP, (double)xi, (double)yi);
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

/* Tear the window down (idempotent). The dlopen handle stays cached. */
int64_t ruxen_canvas_window_destroy(void) {
    if (s_tex) { s_DestroyTexture(s_tex); s_tex = NULL; }
    if (s_ren) { s_DestroyRenderer(s_ren); s_ren = NULL; }
    if (s_win) { s_DestroyWindow(s_win); s_win = NULL; }
    s_tex_w = s_tex_h = 0;
    return RXC_OK;
}
