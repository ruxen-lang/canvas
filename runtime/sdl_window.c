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
 *   - ruxen_canvas_window_destroy_for(host)   tear one window down
 *   - ruxen_canvas_window_destroy()           tear ALL windows down (legacy)
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
 *     window id:       Uint32 windowID    @ 8  (for the multi-window demux)
 *   MULTI-WINDOW: up to RX_MAX_WINDOWS live windows, each one RxWin slot keyed
 *   by its owning RxHost. SDL's single event queue is demuxed per windowID in
 *   the pump. A single-window app touches exactly one slot (backward compatible).
 */

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <string.h>

/* Ruxen's String runtime constructor (library/std/string/runtime/string.c): a
 * Ruxen String IS a malloc'd NUL-terminated char*, so a clipboard string returned
 * to the Ruxen side must be built with this (NOT SDL's own malloc — Ruxen frees it
 * with its allocator on drop). C copies the SDL text through here, then SDL_free's
 * the SDL copy. */
extern char *ruxen_string_from(const char *s);

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
/* push an IME composition event (marked text + start/length) — text is COPIED
 * into the ring slot, so the SDL buffer never dangles (defined in skia_shim.c). */
extern int64_t ruxen_canvas_push_event_text(int64_t self, int64_t kind, int64_t start,
                                            int64_t length, int64_t text_ptr);
/* push a KeyDown carrying its keyboard MODIFIER bitfield (RX_MOD_*) in the ring
 * slot's side-channel (defined in skia_shim.c). */
extern int64_t ruxen_canvas_push_event_mods(int64_t self, int64_t kind, double a, double b,
                                            int64_t mods);

/* event-kind tags (must match the Rxc module in src/lib.rx) */
#define RX_EV_POINTER_MOVE 0
#define RX_EV_POINTER_DOWN 1
#define RX_EV_POINTER_UP   2
#define RX_EV_KEY_DOWN     3
#define RX_EV_RESIZE       4
#define RX_EV_CLOSE        5
#define RX_EV_SCROLL       6
#define RX_EV_TEXT_INPUT   7
#define RX_EV_TEXT_EDITING 8   /* IME composition (marked text + cursor) */
#define RX_EV_FILE_DROP    9   /* a file was dropped onto the window (path in text) */

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
#define SDL_TEXTINPUT_EV          0x303u
#define SDL_TEXTEDITING_EV        0x302u
#define SDL_MOUSEMOTION_EV        0x400u
#define SDL_MOUSEBUTTONDOWN_EV    0x401u
#define SDL_MOUSEBUTTONUP_EV      0x402u
#define SDL_MOUSEWHEEL_EV         0x403u
/* Drag-and-drop (Phase-1.5). SDL_DROPFILE delivers ONE dropped file path per
 * event (a multi-file drop arrives as several DROPFILE events). SDL_DropEvent ABI
 * differs from the other window events: { Uint32 type@0; Uint32 timestamp@4;
 * char *file@8; Uint32 windowID@16; }. So the dropped-path pointer is at offset 8
 * and the windowID is at offset 16 (NOT the usual offset 8). The `file` pointer
 * is SDL-malloc'd and the receiver MUST SDL_free it (SDL's documented contract) —
 * we copy it into the ring slot at pump time, then free SDL's copy immediately, so
 * no SDL pointer ever dangles (the same side-channel discipline as IME marked
 * text). DROPBEGIN/DROPCOMPLETE bracket a multi-file drag; we don't surface them
 * (each file's DROPFILE is self-contained). */
#define SDL_DROPFILE_EV           0x1000u
#define SDL_DROPTEXT_EV           0x1001u
#define SDL_DROPBEGIN_EV          0x1002u
#define SDL_DROPCOMPLETE_EV       0x1003u
#define SDL_DROPEVENT_FILE_OFF    8    /* char *file */
#define SDL_DROPEVENT_WINDOWID_OFF 16  /* Uint32 windowID (drop events ONLY) */
/* SDL_EventState enable constant (SDL_ENABLE = 1); SDL_EventState(SDL_DROPFILE,
 * SDL_ENABLE) ensures drop events are delivered (they are enabled by default once
 * a window exists, but we ask explicitly for robustness). */
#define SDL_ENABLE                1
/* SDL_KeyboardEvent ABI: state@12, repeat@13 (Uint8), keysym.sym@20 (verified
 * against SDL_events.h on this host — the repeat flag is at 13, NOT 10). */
#define SDL_KEY_REPEAT_OFF        13
/* SDL_TextInputEvent: the UTF-8 NUL-terminated text starts at offset 12. */
#define SDL_TEXTINPUT_TEXT_OFF    12
/* SDL_TextEditingEvent ABI: { Uint32 type@0; Uint32 timestamp@4; Uint32 windowID@8;
 * char text[32]@12; Sint32 start@44; Sint32 length@48; }. The marked composition
 * text (NUL-terminated UTF-8, up to 32 bytes) is at offset 12; the composition
 * cursor `start` and selection `length` (codepoints) at 44 / 48. This is the
 * UNcommitted, in-progress IME text (CJK/diacritic composition) — distinct from
 * the committed SDL_TEXTINPUT path. */
#define SDL_TEXTEDITING_TEXT_OFF   12
#define SDL_TEXTEDITING_START_OFF  44
#define SDL_TEXTEDITING_LENGTH_OFF 48
/* Every window-associated SDL event struct begins {Uint32 type; Uint32
 * timestamp; Uint32 windowID; ...}, so the originating window's SDL windowID is
 * at byte offset 8 for mouse motion/button/wheel, keyboard, text-input, and
 * window events. The pump reads it to DEMUX each event to the owning window's
 * host ring (multi-window). SDL_QUIT has no windowID (it is app-global). */
#define SDL_EVENT_WINDOWID_OFF    8

/* Control keysyms we still forward via KeyDown (TextInput owns printable chars).
 * SDLK values: editing + navigation. The arrow keys are SDLK_RIGHT/LEFT/DOWN/UP
 * = 0x4000004F..52 | (1<<30) = 1073741903..1073741906. */
#define SDLK_BACKSPACE   8
#define SDLK_TAB         9
#define SDLK_RETURN      13
#define SDLK_ESCAPE      27
#define SDLK_DELETE      127
#define SDLK_RIGHT       1073741903
#define SDLK_LEFT        1073741904
#define SDLK_DOWN        1073741905
#define SDLK_UP          1073741906
#define SDLK_HOME        1073741898
#define SDLK_END         1073741901
/* SDL_Keymod bits (SDL_keycode.h, stable ABI) — the modifier state SDL_GetModState
 * returns. We fold left/right into one bit per modifier and map to the canvas-side
 * RX_MOD_* small enum (rx_canvas_internal.h) for the KeyDown side-channel. */
#define KMOD_LSHIFT 0x0001u
#define KMOD_RSHIFT 0x0002u
#define KMOD_LCTRL  0x0040u
#define KMOD_RCTRL  0x0080u
#define KMOD_LALT   0x0100u
#define KMOD_RALT   0x0200u
#define KMOD_LGUI   0x0400u
#define KMOD_RGUI   0x0800u
/* Canvas-side modifier bits (keep in sync with RX_MOD_* in rx_canvas_internal.h
 * and the Window.mod_* constants in src/window.rx). */
#define RX_MOD_SHIFT 1
#define RX_MOD_CTRL  2
#define RX_MOD_ALT   4
#define RX_MOD_GUI   8
/* SDL_WindowEventID subtype (SDL_WindowEvent.event @ byte 12, Uint8). */
#define SDL_WINDOWEVENT_RESIZED   5
/* MINIMIZED/MAXIMIZED/RESTORED: window-state transitions (SDL_WindowEventID).
 * MINIMIZED hides the window — its drawable is occluded/zero-area, so present is
 * a no-op until RESTORED (the minimized-present contract; see RxWin.minimized).
 * MAXIMIZED/RESTORED change the backing size, so we re-derive + emit Resize. */
#define SDL_WINDOWEVENT_MINIMIZED 6
#define SDL_WINDOWEVENT_MAXIMIZED 7
#define SDL_WINDOWEVENT_RESTORED  8
/* DISPLAY_CHANGED (SDL 2.0.18+): the window moved to a display with a different
 * backing scale / refresh — the backing drawable size may change, so we re-derive
 * the surface + emit Event.Resize. DESIGN DECISION: no dedicated DisplayChanged
 * Event variant — Resize already carries the new design size and triggers exactly
 * the surface re-creation a DPI change needs, so a one-impl variant would add no
 * payload (greenlit; see docs/ROADMAP.md Phase-1.5). */
#define SDL_WINDOWEVENT_DISPLAY_CHANGED 18
/* SDL_WINDOW_RESIZABLE — let the user resize the window; we re-create the GPU
 * surface/drawable at the new backing size and emit Event.Resize. */
#define SDL_WINDOW_RESIZABLE      0x00000020u
/* SDL_SetWindowFullscreen flag: FULLSCREEN_DESKTOP is the modern borderless
 * "fake fullscreen" (the window grows to the desktop resolution with no display
 * mode switch) — instant alt-tab, no resolution flicker, the default every
 * desktop toolkit uses now. The exclusive SDL_WINDOW_FULLSCREEN (0x1, a real mode
 * switch) is deliberately NOT used. 0 = windowed (restore from fullscreen). */
#define SDL_WINDOW_FULLSCREEN_DESKTOP 0x00001001u

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

/* SDL_DisplayMode ABI: { Uint32 format; int w; int h; int refresh_rate; void
 * *driverdata; }. refresh_rate (Hz, int) is at byte offset 12 (format@0, w@4,
 * h@8). We over-allocate the out buffer to be safe against trailing fields.
 * Used by the frame-pacing refresh-rate hint (docs/decisions/frame-pacing.md). */
#define SDL_DISPLAYMODE_REFRESH_OFF 12

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
/* Enable/disable an SDL event type (drag-and-drop: ensure DROPFILE is on).
 * OPTIONAL — a miss just means we rely on SDL's default (drop on once a window
 * exists). Returns the prior state; we ignore it. */
static uint8_t (*s_EventState)(uint32_t type, int state);
static void  (*s_DestroyTexture)(void *);
static void  (*s_DestroyRenderer)(void *);
static void  (*s_DestroyWindow)(void *);
static uint32_t (*s_GetWindowID)(void *);   /* event-demux: window -> SDL windowID */
static const char *(*s_GetError)(void);
/* Text-input mode: when started, SDL emits SDL_TEXTINPUT (layout/shift-correct
 * UTF-8) instead of relying on raw keysyms. OPTIONAL — a miss just means no text
 * events (control keys via KEYDOWN still work). */
static void  (*s_StartTextInput)(void);
/* Current keyboard modifier state (KMOD_* bitfield). Read at pump time to carry
 * shift/ctrl/alt/gui on each KeyDown. OPTIONAL — a miss just means KeyDown events
 * carry 0 modifiers (the keycode is unaffected). */
static uint16_t (*s_GetModState)(void);
/* Clipboard (E2 desktop services). Work headless under the dummy video driver —
 * no live window needed (verified). SDL_GetClipboardText returns a malloc'd UTF-8
 * string SDL owns; release it with SDL_free. OPTIONAL — a miss reports Err. */
static char *(*s_GetClipboardText)(void);
static int   (*s_SetClipboardText)(const char *);
static void  (*s_SDL_free)(void *);
/* System mouse cursors (E2 desktop services): create a stock cursor by
 * SDL_SystemCursor id, set it. OPTIONAL — a miss (or no real video backend, e.g.
 * the dummy driver) reports Err. Created cursors are cached per process. */
static void *(*s_CreateSystemCursor)(int /*SDL_SystemCursor*/);
static void  (*s_SetCursor)(void *);
/* Window-management entry points (Phase-1.5 desktop window control). OPTIONAL —
 * a miss makes the corresponding setter Err (RXC_ERR_NO_SDL), never a crash.
 * SetWindowFullscreen returns 0 on success; the rest are void. Min/Max size take
 * (window, w, h) in logical points. */
static int   (*s_SetWindowFullscreen)(void *window, uint32_t flags);
static void  (*s_MaximizeWindow)(void *window);
static void  (*s_MinimizeWindow)(void *window);
static void  (*s_RestoreWindow)(void *window);
static void  (*s_SetWindowMinimumSize)(void *window, int min_w, int min_h);
static void  (*s_SetWindowMaximumSize)(void *window, int max_w, int max_h);

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
/* Display-mode query for the frame-pacing refresh-rate hint (docs/decisions/
 * frame-pacing.md). OPTIONAL: a miss just means refresh_rate reports Err. The
 * out struct is an SDL_DisplayMode (see SDL_DISPLAYMODE_REFRESH_OFF below). */
static int   (*s_GetDesktopDisplayMode)(int displayIndex, void *mode);

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

/* ---- multi-window state (docs/MULTIWINDOW.md) ----
 *
 * Each live OS window is one RxWin slot, keyed by the RxHost pointer that owns
 * it (`owner`). A small fixed-size table (no heap, no language feature) tracks
 * up to RX_MAX_WINDOWS concurrent windows. Every ruxen_canvas_window_* entry
 * point already receives `self` (the host pointer); it looks its slot up by
 * `self`, so a 1-window app touches exactly one slot and behaves identically to
 * the old single-`s_*`-global design (backward compatible).
 *
 * The SDL library handle (s_lib) and its dlsym'd function pointers stay
 * process-global — SDL is loaded once. Only the per-window OS objects (window /
 * renderer / texture / GL context / Metal view+layer+drawable) move into a slot.
 *
 * SDL_PollEvent is a single PROCESS-WIDE queue across all windows; each SDL
 * event carries the originating window's SDL windowID. ruxen_canvas_window_pump
 * drains that one queue and DEMUXES each event to the owning slot's host ring by
 * windowID (so window B's click never lands in window A's event stream). Each
 * slot records its `win_id` (SDL_GetWindowID) at create for this routing. */
#define RX_MAX_WINDOWS 16

typedef struct {
    void   *owner;     /* the RxHost this window belongs to; NULL = free slot */
    void   *win;       /* SDL_Window* */
    void   *ren;       /* SDL_Renderer* (raster path; NULL for GL/Metal) */
    void   *tex;       /* SDL_Texture* (raster path; NULL for GL/Metal) */
    uint32_t win_id;   /* SDL_GetWindowID(win) — for the pump's event demux */
    int     tex_w;     /* logical framebuffer width  (texture size) */
    int     tex_h;     /* logical framebuffer height */
    int     scale;     /* design->window-point factor (the show factor) */
    int     out_w;     /* renderer output (backing) pixel size — Retina-true */
    int     out_h;
    /* Minimized-present contract (Phase-1.5): set on SDL_WINDOWEVENT_MINIMIZED,
     * cleared on RESTORED/MAXIMIZED. While set, present/gl_present/metal_present
     * are no-ops (RXC_OK) — a minimized window's drawable is occluded/zero-area, so
     * presenting wastes a frame and some drivers error on a zero-size swap. */
    int     minimized;
    /* GL-window state: glctx != NULL means this window is SDL_WINDOW_OPENGL with
     * a current GL context — present is a buffer swap, not a texture blit. */
    void   *glctx;     /* SDL_GLContext, or NULL */
    /* Metal-window state: mtl_layer != NULL means a SDL_WINDOW_METAL window whose
     * CAMetalLayer we render into per frame; mtl_drawable is the frame's drawable
     * (NULL between frames). docs/GPU.md. The device/queue are the rx_metal
     * singleton (one per process), stashed here for present. */
    void   *mtl_view;
    void   *mtl_layer;
    void   *mtl_device;
    void   *mtl_queue;
    void   *mtl_drawable;
} RxWin;

static RxWin s_wins[RX_MAX_WINDOWS];

static int   s_gl_have_fns = 0;

/* Find the slot owned by `owner`, or NULL if it has no live window. */
static RxWin *rx_win_for(void *owner) {
    if (!owner) return NULL;
    for (int i = 0; i < RX_MAX_WINDOWS; i++) {
        if (s_wins[i].owner == owner) return &s_wins[i];
    }
    return NULL;
}

/* Claim a free slot for `owner` (zeroed, owner set), or NULL when the table is
 * full. Caller must already have checked rx_win_for(owner) == NULL. */
static RxWin *rx_win_alloc(void *owner) {
    for (int i = 0; i < RX_MAX_WINDOWS; i++) {
        if (s_wins[i].owner == NULL) {
            memset(&s_wins[i], 0, sizeof(RxWin));
            s_wins[i].owner = owner;
            s_wins[i].scale = 1;
            return &s_wins[i];
        }
    }
    return NULL;
}

/* Find the slot whose live window has SDL windowID `id` (the pump's demux), or
 * NULL when no slot matches (an event for a window we don't track). */
static RxWin *rx_win_by_id(uint32_t id) {
    if (id == 0) return NULL;
    for (int i = 0; i < RX_MAX_WINDOWS; i++) {
        if (s_wins[i].owner && s_wins[i].win_id == id) return &s_wins[i];
    }
    return NULL;
}

/* How many slots currently hold a live window. */
static int rx_win_count(void) {
    int n = 0;
    for (int i = 0; i < RX_MAX_WINDOWS; i++) if (s_wins[i].owner) n++;
    return n;
}

static void *sym(const char *name) { return dlsym(s_lib, name); }

/* Begin SDL text-input mode so the pump receives SDL_TEXTINPUT (layout/shift-
 * correct UTF-8) for printable typing. Called once after a window is created;
 * a no-op when SDL_StartTextInput is unavailable. */
static void rx_start_text_input(void) {
    if (s_StartTextInput) s_StartTextInput();
}

/* Ensure SDL delivers file-drop events (drag-and-drop). SDL enables DROPFILE by
 * default once a window exists, but we ask explicitly for robustness across SDL
 * builds. A no-op when SDL_EventState is unavailable. Called once per window create. */
static void rx_enable_file_drop(void) {
    if (s_EventState) s_EventState(SDL_DROPFILE_EV, SDL_ENABLE);
}

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
    s_EventState     = (uint8_t (*)(uint32_t, int))sym("SDL_EventState");  /* OPTIONAL */
    s_DestroyTexture = (void (*)(void *))sym("SDL_DestroyTexture");
    s_DestroyRenderer= (void (*)(void *))sym("SDL_DestroyRenderer");
    s_DestroyWindow  = (void (*)(void *))sym("SDL_DestroyWindow");
    s_GetWindowID    = (uint32_t (*)(void *))sym("SDL_GetWindowID");  /* event demux */
    s_GetError       = (const char *(*)(void))sym("SDL_GetError");
    s_StartTextInput = (void (*)(void))sym("SDL_StartTextInput");  /* OPTIONAL */
    s_GetModState    = (uint16_t (*)(void))sym("SDL_GetModState");  /* OPTIONAL */
    s_GetClipboardText = (char *(*)(void))sym("SDL_GetClipboardText");  /* OPTIONAL */
    s_SetClipboardText = (int (*)(const char *))sym("SDL_SetClipboardText");
    s_SDL_free         = (void (*)(void *))sym("SDL_free");
    s_CreateSystemCursor = (void *(*)(int))sym("SDL_CreateSystemCursor");  /* OPTIONAL */
    s_SetCursor        = (void (*)(void *))sym("SDL_SetCursor");
    /* Window management (Phase-1.5). OPTIONAL — a miss makes the setter Err. */
    s_SetWindowFullscreen  = (int (*)(void *, uint32_t))sym("SDL_SetWindowFullscreen");
    s_MaximizeWindow       = (void (*)(void *))sym("SDL_MaximizeWindow");
    s_MinimizeWindow       = (void (*)(void *))sym("SDL_MinimizeWindow");
    s_RestoreWindow        = (void (*)(void *))sym("SDL_RestoreWindow");
    s_SetWindowMinimumSize = (void (*)(void *, int, int))sym("SDL_SetWindowMinimumSize");
    s_SetWindowMaximumSize = (void (*)(void *, int, int))sym("SDL_SetWindowMaximumSize");
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
    s_GetDesktopDisplayMode = (int (*)(int, void *))sym("SDL_GetDesktopDisplayMode");
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
    /* idempotent for the owning host (it already has a live window). */
    if (rx_win_for((void *)h)) return RXC_OK;
    if (!rx_window_allowed()) return RXC_ERR_NO_SDL;   /* forked harness: headless */
    if (!load_sdl()) return RXC_ERR_NO_SDL;
    if (s_Init(SDL_INIT_VIDEO) != 0) return RXC_ERR_NO_SDL;

    RxWin *w = rx_win_alloc((void *)h);
    if (!w) return RXC_ERR_BUSY;   /* window table full */
    w->scale = (int)scale;
    /* ALLOW_HIGHDPI: the backing store is the true Retina pixel size, so macOS
     * does NOT upscale the window a second time on top of our scale. The texture
     * holds the logical framebuffer; RenderCopy maps it to the renderer's output
     * (backing) size once. Without this flag the window was upscaled twice
     * (our scale × Retina) — double blur. */
    w->win = s_CreateWindow(t,
                            (int)SDL_WINDOWPOS_CENTERED, (int)SDL_WINDOWPOS_CENTERED,
                            h->width * w->scale, h->height * w->scale,
                            SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_RESIZABLE);
    if (!w->win) { w->owner = NULL; return RXC_ERR_NO_SDL; }
    w->ren = s_CreateRenderer(w->win, -1, 0);
    if (!w->ren) { s_DestroyWindow(w->win); w->owner = NULL; return RXC_ERR_NO_SDL; }
    w->tex = s_CreateTexture(w->ren, SDL_PIXELFORMAT_ARGB8888,
                             SDL_TEXTUREACCESS_STREAMING, h->width, h->height);
    if (!w->tex) {
        s_DestroyRenderer(w->ren);
        s_DestroyWindow(w->win);
        w->owner = NULL;
        return RXC_ERR_NO_SDL;
    }
    w->tex_w = h->width;
    w->tex_h = h->height;
    /* The real backing pixel size (Retina ~2x the logical window). */
    w->out_w = h->width * w->scale;
    w->out_h = h->height * w->scale;
    if (s_GetRendererOutputSize) {
        int ow = 0, oh = 0;
        if (s_GetRendererOutputSize(w->ren, &ow, &oh) == 0 && ow > 0 && oh > 0) {
            w->out_w = ow;
            w->out_h = oh;
        }
    }
    if (s_GetWindowID) w->win_id = s_GetWindowID(w->win);  /* for the pump's demux */
    rx_start_text_input();   /* printable typing -> SDL_TEXTINPUT */
    rx_enable_file_drop();   /* drag-and-drop -> SDL_DROPFILE */
    return RXC_OK;
}

/* Whether ANY window is currently up (0/1) — the legacy no-arg probe. With
 * multiple windows this reports "at least one window is shown". */
int64_t ruxen_canvas_window_is_shown(void) {
    return rx_win_count() > 0 ? 1 : 0;
}

/* Blit the host framebuffer to the window. host pixels are 0xAARRGGBB,
 * exactly SDL_PIXELFORMAT_ARGB8888 — a straight upload, no conversion. */
int64_t ruxen_canvas_window_present(int64_t self) {
    RxHostPrefix *h = (RxHostPrefix *)self;
    if (!h || !h->pixels) return RXC_ERR_BAD_ARGS;
    RxWin *w = rx_win_for((void *)h);
    if (!w || !w->tex || h->width != w->tex_w || h->height != w->tex_h) return RXC_ERR_PRESENT;
    if (w->minimized) return RXC_OK;   /* minimized: skip present (occluded drawable) */
    if (s_UpdateTexture(w->tex, NULL, h->pixels, h->width * 4) != 0) return RXC_ERR_PRESENT;
    if (s_RenderClear(w->ren) != 0) return RXC_ERR_PRESENT;
    if (s_RenderCopy(w->ren, w->tex, NULL, NULL) != 0) return RXC_ERR_PRESENT;
    s_RenderPresent(w->ren);
    return RXC_OK;
}

/* ---- desktop window management (Phase-1.5) ----
 *
 * Per-window setters: each resolves ITS slot by `self` (the owning RxHost), so a
 * multi-window app controls each window independently. All return:
 *   RXC_OK          on success
 *   RXC_ERR_PRESENT when `self` has no live window (nothing to control)
 *   RXC_ERR_NO_SDL  when the SDL entry point isn't resolved on this host
 * The forked test harness opens no real window, so headless these Err with
 * RXC_ERR_PRESENT — the pin asserts that contract; the live behaviour is proven
 * by examples/window_mgmt_verify.c on a real display.
 *
 * On fullscreen/maximize/minimize/restore SDL fires SDL_WINDOWEVENT_* (RESIZED /
 * MINIMIZED / MAXIMIZED / RESTORED); the pump handles those — re-deriving the
 * backing surface and emitting Event.Resize — so these setters do NOT synthesize
 * a Resize themselves (the live event is the single source of truth). */

/* Toggle borderless desktop fullscreen (FULLSCREEN_DESKTOP) on/off for `self`'s
 * window. The pump's RESIZED handler re-creates the surface at the new drawable
 * size and emits Event.Resize. */
int64_t ruxen_canvas_window_set_fullscreen(int64_t self, int64_t on) {
    RxWin *w = rx_win_for((void *)self);
    if (!w || !w->win) return RXC_ERR_PRESENT;
    if (!s_SetWindowFullscreen) return RXC_ERR_NO_SDL;
    uint32_t flags = on ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0u;
    return s_SetWindowFullscreen(w->win, flags) == 0 ? RXC_OK : RXC_ERR_PRESENT;
}

int64_t ruxen_canvas_window_maximize(int64_t self) {
    RxWin *w = rx_win_for((void *)self);
    if (!w || !w->win) return RXC_ERR_PRESENT;
    if (!s_MaximizeWindow) return RXC_ERR_NO_SDL;
    s_MaximizeWindow(w->win);
    return RXC_OK;
}

int64_t ruxen_canvas_window_minimize(int64_t self) {
    RxWin *w = rx_win_for((void *)self);
    if (!w || !w->win) return RXC_ERR_PRESENT;
    if (!s_MinimizeWindow) return RXC_ERR_NO_SDL;
    s_MinimizeWindow(w->win);
    return RXC_OK;
}

int64_t ruxen_canvas_window_restore(int64_t self) {
    RxWin *w = rx_win_for((void *)self);
    if (!w || !w->win) return RXC_ERR_PRESENT;
    if (!s_RestoreWindow) return RXC_ERR_NO_SDL;
    s_RestoreWindow(w->win);
    return RXC_OK;
}

/* Set the window's minimum size in logical points (>= 0). The caller (Window#
 * set_min_size) validates non-negativity; we pass through. */
int64_t ruxen_canvas_window_set_min_size(int64_t self, int64_t min_w, int64_t min_h) {
    if (min_w < 0 || min_h < 0) return RXC_ERR_BAD_ARGS;
    RxWin *w = rx_win_for((void *)self);
    if (!w || !w->win) return RXC_ERR_PRESENT;
    if (!s_SetWindowMinimumSize) return RXC_ERR_NO_SDL;
    s_SetWindowMinimumSize(w->win, (int)min_w, (int)min_h);
    return RXC_OK;
}

int64_t ruxen_canvas_window_set_max_size(int64_t self, int64_t max_w, int64_t max_h) {
    if (max_w < 0 || max_h < 0) return RXC_ERR_BAD_ARGS;
    RxWin *w = rx_win_for((void *)self);
    if (!w || !w->win) return RXC_ERR_PRESENT;
    if (!s_SetWindowMaximumSize) return RXC_ERR_NO_SDL;
    s_SetWindowMaximumSize(w->win, (int)max_w, (int)max_h);
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
 * current on it. Returns RXC_OK on success (slot glctx set), or RXC_ERR_NO_SDL /
 * RXC_ERR_BUSY / RXC_ERR_PRESENT on a clean failure. Idempotent for the owning
 * host. On ANY failure the window state is left torn down so the caller can
 * fall back to the raster show path. */
int64_t ruxen_canvas_window_create_gl(int64_t self, int64_t win_scale) {
    RxHostPrefix *h = (RxHostPrefix *)self;
    if (!h) return RXC_ERR_BAD_ARGS;
    int ws = (int)win_scale;
    if (ws < 1) ws = 1;
    if (ws > 8) ws = 8;
    /* idempotent for the owning host if it already has a GL window. */
    RxWin *existing = rx_win_for((void *)h);
    if (existing) return existing->glctx ? RXC_OK : RXC_ERR_BUSY;
    if (!rx_window_allowed()) return RXC_ERR_NO_SDL;   /* forked harness: headless */
    if (!load_sdl()) return RXC_ERR_NO_SDL;
    if (!s_gl_have_fns) return RXC_ERR_NO_SDL;   /* no GL entry points */
    if (s_Init(SDL_INIT_VIDEO) != 0) return RXC_ERR_NO_SDL;

    RxWin *w = rx_win_alloc((void *)h);
    if (!w) return RXC_ERR_BUSY;   /* window table full */

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
    w->win = s_CreateWindow("ruxen-gl",
                            (int)SDL_WINDOWPOS_CENTERED, (int)SDL_WINDOWPOS_CENTERED,
                            h->width * ws, h->height * ws,
                            SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_RESIZABLE);
    if (!w->win) { w->owner = NULL; return RXC_ERR_NO_SDL; }
    w->glctx = s_GL_CreateContext(w->win);
    if (!w->glctx) {
        s_DestroyWindow(w->win); w->owner = NULL;
        return RXC_ERR_PRESENT;
    }
    if (s_GL_MakeCurrent(w->win, w->glctx) != 0) {
        s_GL_DeleteContext(w->glctx);
        s_DestroyWindow(w->win); w->owner = NULL;
        return RXC_ERR_PRESENT;
    }
    if (s_GL_SetSwapInterval) s_GL_SetSwapInterval(1);   /* vsync; best-effort */
    /* scale is the DESIGN->window-point factor (the show_gpu_scaled factor),
     * used by the pump to map mouse POINTS (0..width*ws) back to design coords
     * (/ ws). NOT the backing/dpr scale — SDL reports mouse in logical points
     * under ALLOW_HIGHDPI; the backing drawable size is tracked separately for
     * the render surface. */
    w->scale = ws;
    w->tex_w = h->width;     /* reuse the size guard fields for the GL window */
    w->tex_h = h->height;
    if (s_GetWindowID) w->win_id = s_GetWindowID(w->win);
    rx_start_text_input();   /* printable typing -> SDL_TEXTINPUT */
    rx_enable_file_drop();   /* drag-and-drop -> SDL_DROPFILE */
    return RXC_OK;
}

/* Whether the window owned by `self` is a GL window (0/1). */
int64_t ruxen_canvas_window_is_gl(int64_t self) {
    RxWin *w = rx_win_for((void *)self);
    return (w && w->glctx) ? 1 : 0;
}

/* Resolve a GL function by name through SDL_GL_GetProcAddress, returned as an
 * int64 pointer (0 when unavailable). This is the loader Skia's
 * gr_glinterface_assemble_*_interface calls back into. The GL context must be
 * current — it is, immediately after create_gl made it current. SDL's GL proc
 * resolution is per-process (context-independent for core entry points), so this
 * does not need a host; we only gate on SOME GL window existing. */
int64_t ruxen_canvas_window_gl_get_proc(int64_t name) {
    const char *n = (const char *)name;
    if (!n || !s_GL_GetProcAddress) return 0;
    /* require at least one live GL context so a current context exists. */
    int have_gl = 0;
    for (int i = 0; i < RX_MAX_WINDOWS; i++) {
        if (s_wins[i].owner && s_wins[i].glctx) { have_gl = 1; break; }
    }
    if (!have_gl) return 0;
    return (int64_t)s_GL_GetProcAddress(n);
}

/* The drawable pixel size of `self`'s GL window (HiDPI-correct via
 * SDL_GL_GetDrawableSize). Packs width in the high 32 bits, height in the low
 * 32 — one int64 return, no out-params across the ABI. 0 when no GL window. */
int64_t ruxen_canvas_window_gl_drawable_size(int64_t self) {
    RxWin *win = rx_win_for((void *)self);
    if (!win || !win->glctx) return 0;
    int w = win->tex_w, hh = win->tex_h;
    if (s_GL_GetDrawableSize) s_GL_GetDrawableSize(win->win, &w, &hh);
    if (w <= 0 || hh <= 0) return 0;
    return ((int64_t)(uint32_t)w << 32) | (int64_t)(uint32_t)hh;
}

/* Present a GPU frame: swap the GL back buffer to the screen. Returns RXC_OK
 * or RXC_ERR_PRESENT when there is no GL window for this host. */
int64_t ruxen_canvas_window_gl_present(int64_t self) {
    RxWin *w = rx_win_for((void *)self);
    if (!w || !w->glctx || !s_GL_SwapWindow) return RXC_ERR_PRESENT;
    if (w->minimized) return RXC_OK;   /* minimized: skip the buffer swap */
    s_GL_SwapWindow(w->win);
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
    RxWin *existing = rx_win_for((void *)h);
    if (existing) return existing->mtl_layer ? RXC_OK : RXC_ERR_BUSY;
    if (!rx_window_allowed()) return RXC_ERR_NO_SDL;   /* forked harness: headless */
    if (!load_sdl()) return RXC_ERR_NO_SDL;
    if (!s_metal_have_fns) return RXC_ERR_NO_SDL;   /* no SDL_Metal_* */
    if (!metal_objc_init()) return RXC_ERR_NO_SDL;  /* no objc runtime */
    if (s_Init(SDL_INIT_VIDEO) != 0) return RXC_ERR_NO_SDL;

    RxWin *w = rx_win_alloc((void *)h);
    if (!w) return RXC_ERR_BUSY;   /* window table full */

    /* ALLOW_HIGHDPI so the view's backing store is the true Retina pixel size;
     * we render the Skia Metal surface at THAT size (queried below) and present
     * 1:1 — crisp by construction. win_scale opens a LARGER window (win_scale ×
     * the logical/design size) for on-screen size; the design->backing content
     * scale (begin_frame) then fills it crisply at native density — so a 2x
     * window of a 320-design UI is 640 logical points, 1280 backing pixels on a
     * 2x display, content scale 4. */
    w->win = s_CreateWindow("ruxen-metal",
                            (int)SDL_WINDOWPOS_CENTERED, (int)SDL_WINDOWPOS_CENTERED,
                            h->width * ws, h->height * ws,
                            SDL_WINDOW_METAL | SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!w->win) { w->owner = NULL; return RXC_ERR_NO_SDL; }

    w->mtl_view = s_Metal_CreateView(w->win);
    if (!w->mtl_view) { s_DestroyWindow(w->win); w->owner = NULL; return RXC_ERR_PRESENT; }
    w->mtl_layer = s_Metal_GetLayer(w->mtl_view);
    if (!w->mtl_layer) {
        s_Metal_DestroyView(w->mtl_view);
        s_DestroyWindow(w->win); w->owner = NULL;
        return RXC_ERR_PRESENT;
    }

    /* The true backing (drawable) pixel size — on Retina ~2x the logical size.
     * We size the layer's drawable AND the Skia surface to this, so the GPU
     * renders at native resolution (crisp text/shapes), presented 1:1. */
    int dpw = h->width, dph = h->height;
    if (s_Metal_GetDrawableSize) s_Metal_GetDrawableSize(w->win, &dpw, &dph);
    if (dpw <= 0 || dph <= 0) { dpw = h->width; dph = h->height; }

    w->mtl_device = (void *)device;
    w->mtl_queue  = (void *)queue;
    /* Configure the layer: our device, BGRA8 (matches the host packing), and
     * framebufferOnly=NO so Skia may render into the drawable's texture. The
     * drawableSize is the native backing size (HiDPI-correct). */
    mtl_msg_p(w->mtl_layer, "setDevice:", w->mtl_device);
    mtl_msg_u(w->mtl_layer, "setPixelFormat:", MTL_PIXELFORMAT_BGRA8);
    mtl_msg_u(w->mtl_layer, "setFramebufferOnly:", 0);
    mtl_set_drawable_size(w->mtl_layer, (double)dpw, (double)dph);

    /* scale = the DESIGN->window-point factor (the show_gpu_scaled factor), so
     * the pump maps mouse POINTS (0..width*ws) back to design coords (/ ws). This
     * is SEPARATE from the backing/dpr scale: tex_w/h track the native backing
     * (drawable) size for the render surface; the pump never uses them. SDL
     * reports mouse in logical points under ALLOW_HIGHDPI, so the dpr does not
     * enter the pump. */
    w->scale = ws;
    w->tex_w = dpw;
    w->tex_h = dph;
    if (s_GetWindowID) w->win_id = s_GetWindowID(w->win);
    rx_start_text_input();   /* printable typing -> SDL_TEXTINPUT */
    rx_enable_file_drop();   /* drag-and-drop -> SDL_DROPFILE */
    return RXC_OK;
}

/* Whether the window owned by `self` is a Metal window (0/1). */
int64_t ruxen_canvas_window_is_metal(int64_t self) {
    RxWin *w = rx_win_for((void *)self);
    return (w && w->mtl_layer) ? 1 : 0;
}

/* True (1) when SDL2 itself could be dlopen'd on this host (the host-aware
 * loader found a usable libSDL2). A capability probe that does NOT open a window
 * — it proves the loader resolves SDL2 here even when the harness is headless.
 * 0 when no SDL2 is installed anywhere on the candidate paths. */
int64_t ruxen_canvas_sdl_available(void) {
    return load_sdl() ? 1 : 0;
}

/* Ensure SDL is loaded + the video subsystem inited so the clipboard works
 * (SDL clipboard requires video; it does NOT require a window — verified to
 * round-trip headless under the dummy driver). Under the forked test harness we
 * force SDL_VIDEODRIVER=dummy BEFORE init: the real macOS Cocoa pasteboard touches
 * AppKit, which is unsafe after fork()-without-exec() (same reason real windows
 * are gated off). The dummy driver's clipboard is process-local but round-trips a
 * set->get, which is exactly what the headless pin asserts. Outside the harness
 * (real apps) the real driver + system clipboard are used. Returns 1 on success. */
static int rx_clipboard_init(void) {
    if (!load_sdl()) return 0;
    if (!s_GetClipboardText || !s_SetClipboardText) return 0;
    if (getenv("RUXEN_TEST_FORMAT") && !getenv("RUXEN_CANVAS_WINDOW")) {
        /* fork-safe headless: dummy video driver (only takes effect if video is
         * not already inited — harmless if a real window already set it up). */
        setenv("SDL_VIDEODRIVER", "dummy", 0);
    }
    if (s_Init(SDL_INIT_VIDEO) != 0) return 0;
    return 1;
}

/* Set the system clipboard to `text` (a Ruxen &String -> NUL-terminated char*).
 * Returns RXC_OK, or RXC_ERR_NO_SDL when SDL / the clipboard is unavailable. */
int64_t ruxen_canvas_clipboard_set(int64_t text) {
    const char *t = (const char *)text;
    if (!t) return RXC_ERR_BAD_ARGS;
    if (!rx_clipboard_init()) return RXC_ERR_NO_SDL;
    return s_SetClipboardText(t) == 0 ? RXC_OK : RXC_ERR_NO_SDL;
}

/* Get the system clipboard text as a freshly Ruxen-allocated String (malloc'd via
 * ruxen_string_from, so the Ruxen owner frees it correctly on drop). Returns the
 * String pointer, or 0 (NULL) when SDL / the clipboard is unavailable — the Ruxen
 * wrapper maps 0 to Err and a non-zero pointer to Ok(text) (text may be the empty
 * string when the clipboard holds no text, which is a valid Ok). The SDL-owned
 * copy is released with SDL_free. */
int64_t ruxen_canvas_clipboard_get(void) {
    if (!rx_clipboard_init()) return 0;
    char *sdl_text = s_GetClipboardText();   /* SDL-malloc'd; never NULL per SDL docs */
    if (!sdl_text) return 0;
    char *owned = ruxen_string_from(sdl_text);   /* Ruxen-malloc'd copy */
    if (s_SDL_free) s_SDL_free(sdl_text);
    return (int64_t)owned;
}

/* True (1) when the clipboard is usable here (SDL loaded + video inited + the
 * clipboard symbols resolved). The Ruxen wrapper uses this to return Err cleanly
 * without round-tripping a string. */
int64_t ruxen_canvas_clipboard_available(void) {
    return rx_clipboard_init() ? 1 : 0;
}

/* ---- mouse cursors (E2 desktop services) ----
 *
 * A canvas-side small int enum (RX_CURSOR_*) maps to SDL_SystemCursor ids. Stock
 * cursors are created lazily and CACHED per process (SDL system cursors are
 * process-global; one of each is all we ever need, and they live for the process
 * — same never-freed-singleton model as the Metal device / default font). */
#define RX_CURSOR_ARROW     0
#define RX_CURSOR_IBEAM     1
#define RX_CURSOR_HAND      2
#define RX_CURSOR_CROSSHAIR 3
#define RX_CURSOR_RESIZE_H  4
#define RX_CURSOR_RESIZE_V  5
#define RX_CURSOR_MAX       5

/* SDL_SystemCursor ids (SDL_mouse.h, stable ABI). */
#define SDL_SYSTEM_CURSOR_ARROW     0
#define SDL_SYSTEM_CURSOR_IBEAM     1
#define SDL_SYSTEM_CURSOR_CROSSHAIR 3
#define SDL_SYSTEM_CURSOR_SIZEWE    7   /* horizontal resize */
#define SDL_SYSTEM_CURSOR_SIZENS    8   /* vertical resize */
#define SDL_SYSTEM_CURSOR_HAND      11

static void *s_cursor_cache[RX_CURSOR_MAX + 1];   /* lazily created, process-global */

static int rx_cursor_sdl_id(int kind) {
    switch (kind) {
    case RX_CURSOR_IBEAM:     return SDL_SYSTEM_CURSOR_IBEAM;
    case RX_CURSOR_HAND:      return SDL_SYSTEM_CURSOR_HAND;
    case RX_CURSOR_CROSSHAIR: return SDL_SYSTEM_CURSOR_CROSSHAIR;
    case RX_CURSOR_RESIZE_H:  return SDL_SYSTEM_CURSOR_SIZEWE;
    case RX_CURSOR_RESIZE_V:  return SDL_SYSTEM_CURSOR_SIZENS;
    case RX_CURSOR_ARROW:
    default:                  return SDL_SYSTEM_CURSOR_ARROW;
    }
}

/* Set the mouse cursor to a stock system cursor (RX_CURSOR_* kind). The created
 * cursor is cached per process so repeated sets don't leak. Returns RXC_OK, or
 * RXC_ERR_NO_SDL when SDL / the cursor backend is unavailable (e.g. the dummy
 * video driver under the test harness — SDL_CreateSystemCursor is "not currently
 * supported" there, so this Errs headless, which the pin asserts; the real cursor
 * is verified outside the harness). An out-of-range kind is RXC_ERR_BAD_ARGS. */
int64_t ruxen_canvas_set_cursor(int64_t kind) {
    /* cursor is process-global, not per-window — no host handle needed */
    if (kind < 0 || kind > RX_CURSOR_MAX) return RXC_ERR_BAD_ARGS;
    if (!load_sdl() || !s_CreateSystemCursor || !s_SetCursor) return RXC_ERR_NO_SDL;
    if (s_Init(SDL_INIT_VIDEO) != 0) return RXC_ERR_NO_SDL;
    void *cur = s_cursor_cache[kind];
    if (!cur) {
        cur = s_CreateSystemCursor(rx_cursor_sdl_id((int)kind));
        if (!cur) return RXC_ERR_NO_SDL;   /* no real cursor backend (e.g. dummy) */
        s_cursor_cache[kind] = cur;
    }
    s_SetCursor(cur);
    return RXC_OK;
}

/* True (1) when stock system cursors are usable here (SDL loaded + a real video
 * backend that supports SDL_CreateSystemCursor). The Ruxen wrapper uses this to
 * report the capability without setting a cursor. */
int64_t ruxen_canvas_cursor_available(void) {
    if (!load_sdl() || !s_CreateSystemCursor || !s_SetCursor) return 0;
    if (s_Init(SDL_INIT_VIDEO) != 0) return 0;
    /* Probe by actually creating the arrow (cached): the dummy driver resolves the
     * symbol but returns NULL, so symbol presence alone is not enough. */
    if (!s_cursor_cache[RX_CURSOR_ARROW]) {
        void *cur = s_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW);
        if (!cur) return 0;
        s_cursor_cache[RX_CURSOR_ARROW] = cur;
    }
    return 1;
}

/* The desktop display's refresh rate in Hz for display 0 (the frame-pacing hint,
 * docs/decisions/frame-pacing.md). Returns the rate (> 0) on success, or a
 * NEGATIVE -RXC_ERR_NO_SDL when SDL / the display-mode query is unavailable or
 * reports an unspecified (0) rate — so the caller maps it to Err and picks a
 * fallback (typically 60) rather than dividing by a bogus 0. SDL must be inited
 * for the video subsystem; load_sdl + SDL_Init(VIDEO) ensure that. The harness
 * (headless, gated) returns the Err channel, which the pin asserts. */
int64_t ruxen_canvas_refresh_rate(void) {
    if (!load_sdl() || !s_GetDesktopDisplayMode) return -RXC_ERR_NO_SDL;
    if (s_Init(SDL_INIT_VIDEO) != 0) return -RXC_ERR_NO_SDL;
    /* SDL_DisplayMode is 24-32 bytes; over-allocate to be safe against trailing
     * fields across SDL minor versions. */
    unsigned char mode[64];
    memset(mode, 0, sizeof mode);
    if (s_GetDesktopDisplayMode(0, mode) != 0) return -RXC_ERR_NO_SDL;
    int32_t hz = 0;
    memcpy(&hz, mode + SDL_DISPLAYMODE_REFRESH_OFF, 4);
    if (hz <= 0) return -RXC_ERR_NO_SDL;   /* 0 = unspecified; force a fallback */
    return (int64_t)hz;
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
 * target. The drawable is held in the slot's mtl_drawable until present releases it; a
 * second call without a present releases the prior unpresented drawable first
 * (no leak). */
int64_t ruxen_canvas_window_metal_next_drawable(int64_t self) {
    RxWin *w = rx_win_for((void *)self);
    if (!w || !w->mtl_layer) return 0;
    /* a prior unpresented drawable would leak — drop our reference to it. */
    w->mtl_drawable = NULL;
    void *drawable = mtl_msg(w->mtl_layer, "nextDrawable");
    if (!drawable) return 0;                  /* no display / off-screen */
    void *texture = mtl_msg(drawable, "texture");
    if (!texture) return 0;
    w->mtl_drawable = drawable;
    return (int64_t)texture;
}

/* The drawable pixel size of `self`'s Metal layer (HiDPI-correct). Packs
 * width<<32 | height. 0 when no Metal window. We track it from drawableSize set
 * at create; the layer's drawableSize is authoritative on HiDPI. */
int64_t ruxen_canvas_window_metal_drawable_size(int64_t self) {
    RxWin *win = rx_win_for((void *)self);
    if (!win || !win->mtl_layer) return 0;
    int w = win->tex_w, hh = win->tex_h;
    if (w <= 0 || hh <= 0) return 0;
    return ((int64_t)(uint32_t)w << 32) | (int64_t)(uint32_t)hh;
}

/* Present the frame's acquired drawable: enqueue a command buffer on the queue
 * that presents the drawable, then commit it. Returns RXC_OK, or RXC_ERR_PRESENT
 * when there is no Metal window / no acquired drawable. After present the
 * drawable is consumed (next frame acquires a fresh one). */
int64_t ruxen_canvas_window_metal_present(int64_t self) {
    RxWin *w = rx_win_for((void *)self);
    if (!w || !w->mtl_layer) return RXC_ERR_PRESENT;
    if (w->minimized) { w->mtl_drawable = NULL; return RXC_OK; }  /* drop drawable, skip */
    if (!w->mtl_drawable || !w->mtl_queue) return RXC_ERR_PRESENT;
    /* id cmdbuf = [queue commandBuffer]; [cmdbuf presentDrawable:drawable];
     * [cmdbuf commit]; */
    void *cmdbuf = mtl_msg(w->mtl_queue, "commandBuffer");
    if (!cmdbuf) { w->mtl_drawable = NULL; return RXC_ERR_PRESENT; }
    mtl_msg_p(cmdbuf, "presentDrawable:", w->mtl_drawable);
    mtl_msg(cmdbuf, "commit");
    w->mtl_drawable = NULL;     /* consumed; next frame acquires a fresh drawable */
    return RXC_OK;
}

/* Coordinate mapping (the pump): SDL2 reports mouse in LOGICAL window POINTS
 * (0..width*ws), not backing pixels, even under ALLOW_HIGHDPI. The app works in
 * DESIGN coords (0..width). The window opens at width*ws (the show /
 * show_scaled / show_gpu_scaled factor), so point -> design is point / ws =
 * point / scale. The slot's `scale` is set to the show factor by EVERY windowed
 * backend (raster, GL, Metal), so this maps points to design coords on all. The
 * Retina backing/dpr does NOT enter here (SDL accounts for it in the point
 * coordinates; the backing size is tracked separately, only for the render
 * surface). Factored out so the headless pump-mapping test exercises the EXACT
 * arithmetic the pump uses. */
static double rx_map_point_scale(int32_t window_point, int scale) {
    return (double)window_point / (double)(scale > 0 ? scale : 1);
}

/* Decode the FIRST UTF-8 character of a NUL-terminated string to a Unicode
 * codepoint (>= 0), or -1 on an empty/invalid lead byte. ASCII is the fast path;
 * 2–4 byte sequences are decoded with continuation-byte validation. An SDL
 * SDL_TEXTINPUT carries one grapheme per event in practice, so the first char is
 * the typed codepoint. */
static int32_t rx_utf8_first_codepoint(const unsigned char *s) {
    if (!s || s[0] == 0) return -1;
    unsigned char c0 = s[0];
    if (c0 < 0x80) return (int32_t)c0;                       /* ASCII */
    if ((c0 & 0xE0) == 0xC0) {                               /* 2-byte */
        if ((s[1] & 0xC0) != 0x80) return -1;
        return ((c0 & 0x1F) << 6) | (s[1] & 0x3F);
    }
    if ((c0 & 0xF0) == 0xE0) {                               /* 3-byte */
        if ((s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80) return -1;
        return ((c0 & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
    }
    if ((c0 & 0xF8) == 0xF0) {                               /* 4-byte */
        if ((s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80 || (s[3] & 0xC0) != 0x80) return -1;
        return ((c0 & 0x07) << 18) | ((s[1] & 0x3F) << 12) |
               ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
    }
    return -1;   /* invalid lead byte */
}

/* True for the CONTROL keysyms we forward via KeyDown (editing + navigation).
 * Printable characters are NOT forwarded here — they arrive as SDL_TEXTINPUT.
 * This is the allowlist that keeps raw keysyms from inserting garbage. */
static int rx_is_control_keysym(int32_t sym) {
    switch (sym) {
    case SDLK_BACKSPACE: case SDLK_TAB:   case SDLK_RETURN: case SDLK_ESCAPE:
    case SDLK_DELETE:    case SDLK_LEFT:   case SDLK_RIGHT:  case SDLK_UP:
    case SDLK_DOWN:      case SDLK_HOME:   case SDLK_END:
        return 1;
    default:
        return 0;
    }
}

/* Fold SDL's KMOD_* bitfield into the canvas-side RX_MOD_* mask (left/right
 * merged). Pure, so the pump and the keydown test seam share it. */
static int64_t rx_fold_keymods(uint16_t kmod) {
    int64_t m = 0;
    if (kmod & (KMOD_LSHIFT | KMOD_RSHIFT)) m |= RX_MOD_SHIFT;
    if (kmod & (KMOD_LCTRL  | KMOD_RCTRL))  m |= RX_MOD_CTRL;
    if (kmod & (KMOD_LALT   | KMOD_RALT))   m |= RX_MOD_ALT;
    if (kmod & (KMOD_LGUI   | KMOD_RGUI))   m |= RX_MOD_GUI;
    return m;
}

/* shim hook: invalidate a windowed GL host's persistent GPU surface so the next
 * begin_frame rebuilds it at the new backing size (defined in skia_shim.c). */
void ruxen_canvas_host_gl_invalidate_surface(int64_t self);

/* On a window resize: the design->point factor (the slot's scale) is unchanged
 * (the design stays h->width), but the backing drawable grows/shrinks. For Metal, update the
 * layer's drawableSize to the new backing pixels so the next frame's drawable +
 * GPU surface are the new size (begin_frame picks them up via the drawable). For
 * GL (persistent surface), tell the shim to drop + rebuild the surface at the new
 * size on the next frame. */
static void rx_window_on_resized(RxWin *w) {
    if (!w) return;
    if (w->mtl_layer && s_Metal_GetDrawableSize) {
        int dpw = 0, dph = 0;
        s_Metal_GetDrawableSize(w->win, &dpw, &dph);
        if (dpw > 0 && dph > 0) {
            mtl_set_drawable_size(w->mtl_layer, (double)dpw, (double)dph);
            w->tex_w = dpw;   /* window_metal_drawable_size now returns the new size */
            w->tex_h = dph;
        }
    } else if (w->glctx) {
        /* GL: the drawable size is queried fresh each rebuild; just invalidate. */
        ruxen_canvas_host_gl_invalidate_surface((int64_t)w->owner);
    }
}

/* Drain the PROCESS-WIDE SDL event queue once, DEMUXING each event to the host
 * ring of the window it originated from (by SDL windowID). Returns the number of
 * events forwarded to `self`'s window specifically — so a single-window app sees
 * the same count it always did, and a multi-window app gets each window's input
 * in its own ring regardless of which window's pump call drained the queue.
 *
 * SDL has ONE event queue shared by all windows, so the first window to pump in
 * a frame drains everything; events for other windows are routed to their rings
 * and surface when THOSE windows poll. We never drop a window's events. */
int64_t ruxen_canvas_window_pump(int64_t self) {
    if (!self) return -RXC_ERR_BAD_ARGS;
    /* No live window for this host (and the queue belongs to live windows): with
     * no windows at all there is nothing to pump. */
    if (rx_win_count() == 0) return 0;
    int64_t forwarded = 0;
    unsigned char ev[64];
    while (s_PollEvent(ev)) {
        uint32_t type;
        memcpy(&type, ev, sizeof type);
        int32_t xi, yi, sym;
        uint32_t wid = 0;
        /* windowID is at offset 8 for every window-associated event; route to
         * the owning slot. SDL_QUIT carries no windowID (handled below). */
        memcpy(&wid, ev + SDL_EVENT_WINDOWID_OFF, 4);
        RxWin *tw = rx_win_by_id(wid);   /* target window for this event */
        int64_t towner = tw ? (int64_t)tw->owner : 0;
        int tscale = tw ? tw->scale : 1;
        int is_self = (towner == self);
        switch (type) {
        case SDL_MOUSEMOTION_EV:
            if (!tw) break;
            memcpy(&xi, ev + 20, 4); memcpy(&yi, ev + 24, 4);
            ruxen_canvas_push_event(towner, RX_EV_POINTER_MOVE,
                                    rx_map_point_scale(xi, tscale), rx_map_point_scale(yi, tscale));
            if (is_self) forwarded++;
            break;
        case SDL_MOUSEBUTTONDOWN_EV:
            if (!tw) break;
            memcpy(&xi, ev + 20, 4); memcpy(&yi, ev + 24, 4);
            ruxen_canvas_push_event(towner, RX_EV_POINTER_DOWN,
                                    rx_map_point_scale(xi, tscale), rx_map_point_scale(yi, tscale));
            if (is_self) forwarded++;
            break;
        case SDL_MOUSEBUTTONUP_EV:
            if (!tw) break;
            memcpy(&xi, ev + 20, 4); memcpy(&yi, ev + 24, 4);
            ruxen_canvas_push_event(towner, RX_EV_POINTER_UP,
                                    rx_map_point_scale(xi, tscale), rx_map_point_scale(yi, tscale));
            if (is_self) forwarded++;
            break;
        case SDL_MOUSEWHEEL_EV:
            /* SDL_MouseWheelEvent: Sint32 x @ 16, y @ 20 (wheel deltas; +y = up,
             * +x = right). Wheel deltas are integer "clicks", not coords — NOT
             * scaled. Forwarded as Event.Scroll(dx, dy). */
            if (!tw) break;
            memcpy(&xi, ev + 16, 4); memcpy(&yi, ev + 20, 4);
            ruxen_canvas_push_event(towner, RX_EV_SCROLL, (double)xi, (double)yi);
            if (is_self) forwarded++;
            break;
        case SDL_TEXTINPUT_EV: {
            /* SDL_TextInputEvent: NUL-terminated UTF-8 at offset 12. This is the
             * ONLY path that inserts printable characters (layout/shift-correct).
             * Emit the first codepoint as Event.TextInput to the focused window. */
            if (!tw) break;
            int32_t cp = rx_utf8_first_codepoint(ev + SDL_TEXTINPUT_TEXT_OFF);
            if (cp >= 0) {
                ruxen_canvas_push_event(towner, RX_EV_TEXT_INPUT, (double)cp, 0.0);
                if (is_self) forwarded++;
            }
            break;
        }
        case SDL_TEXTEDITING_EV: {
            /* SDL_TextEditingEvent: the IN-PROGRESS (uncommitted) IME composition —
             * marked UTF-8 text @ offset 12, cursor `start` @ 44, selection
             * `length` @ 48. This is what CJK / diacritic input needs BEYOND the
             * committed TEXTINPUT path: emit Event.TextEditing(start, length) with
             * the marked text carried in the ring slot (copied, never a dangling
             * SDL pointer). An empty marked text (composition cleared) is still a
             * valid event — the app uses it to erase the marked region. */
            if (!tw) break;
            int32_t start = 0, length = 0;
            memcpy(&start,  ev + SDL_TEXTEDITING_START_OFF,  4);
            memcpy(&length, ev + SDL_TEXTEDITING_LENGTH_OFF, 4);
            ruxen_canvas_push_event_text(towner, RX_EV_TEXT_EDITING, start, length,
                                         (int64_t)(ev + SDL_TEXTEDITING_TEXT_OFF));
            if (is_self) forwarded++;
            break;
        }
        case SDL_KEYDOWN_EV: {
            /* Skip auto-repeat (held key) so a control key doesn't machine-gun. */
            if (ev[SDL_KEY_REPEAT_OFF] != 0) break;
            if (!tw) break;
            memcpy(&sym, ev + 20, 4);
            /* Forward CONTROL keys only — editing/navigation. Printable chars are
             * owned by SDL_TEXTINPUT above; forwarding their raw keysyms here
             * would insert garbage (no shift/layout). */
            if (rx_is_control_keysym(sym)) {
                /* Carry the live keyboard modifier state (shift/ctrl/alt/gui) as a
                 * side-channel on the ring slot — the KeyDown payload stays a bare
                 * keycode, modifiers are read via Window#key_modifiers post-poll.
                 * SDL_GetModState is the current global state at pump time (the held
                 * modifiers when this key fired); robust across the event-struct
                 * layout, unlike decoding keysym.mod at a guessed offset. */
                int64_t mods = s_GetModState ? rx_fold_keymods(s_GetModState()) : 0;
                ruxen_canvas_push_event_mods(towner, RX_EV_KEY_DOWN, (double)sym, 0.0, mods);
                if (is_self) forwarded++;
            }
            break;
        }
        case SDL_WINDOWEVENT_EV: {
            /* SDL_WindowEvent: Uint8 event @ 12, Sint32 data1 @ 16, data2 @ 20.
             * On RESIZED, data1/data2 are the new window size in POINTS; we emit
             * Resize in DESIGN coords (/ scale) and re-size the backing surface of
             * the originating window. MINIMIZED/MAXIMIZED/RESTORED/DISPLAY_CHANGED
             * carry no size in data1/data2 we can trust, so they re-derive the
             * backing surface and emit Resize in the window's DESIGN size (taken
             * from the owning host) — which is exactly what a DPI change needs L2
             * to re-process (the content scale changes, the design size doesn't). */
            if (!tw) break;
            unsigned char subtype = ev[12];
            if (subtype == SDL_WINDOWEVENT_RESIZED) {
                int32_t w1, h1;
                memcpy(&w1, ev + 16, 4); memcpy(&h1, ev + 20, 4);
                rx_window_on_resized(tw);
                ruxen_canvas_push_event(towner, RX_EV_RESIZE,
                                        rx_map_point_scale(w1, tscale), rx_map_point_scale(h1, tscale));
                if (is_self) forwarded++;
            } else if (subtype == SDL_WINDOWEVENT_MINIMIZED) {
                /* Minimized: skip presenting until restored (occluded drawable). No
                 * Resize — the design size is unchanged; only visibility changed. */
                tw->minimized = 1;
            } else if (subtype == SDL_WINDOWEVENT_MAXIMIZED ||
                       subtype == SDL_WINDOWEVENT_RESTORED ||
                       subtype == SDL_WINDOWEVENT_DISPLAY_CHANGED) {
                tw->minimized = 0;                  /* visible again / state change */
                rx_window_on_resized(tw);           /* re-derive backing surface */
                /* Emit Resize carrying the DESIGN size (owning host's width/height),
                 * so L2 re-derives layout + content scale. SDL also fires RESIZED
                 * for an actual size change; this guarantees a Resize even for a
                 * pure DPI/display move that keeps the window size constant. */
                RxHostPrefix *oh = (RxHostPrefix *)tw->owner;
                if (oh) {
                    ruxen_canvas_push_event(towner, RX_EV_RESIZE,
                                            (double)oh->width, (double)oh->height);
                    if (is_self) forwarded++;
                }
            }
            break;
        }
        case SDL_DROPFILE_EV: {
            /* A file was dropped onto a window. SDL_DropEvent's windowID is at
             * offset 16 (NOT the usual 8) and the path pointer at offset 8 — and
             * SDL OWNS that path string (SDL-malloc'd); we MUST SDL_free it. Copy
             * it into the ring slot (the side-channel) at pump time, then free
             * SDL's copy immediately, so no SDL pointer dangles. SDL_DROPFILE
             * carries NO drop position, so the FileDrop event has no coords (0,0);
             * the path is read back via Window#dropped_file_path. A multi-file drop
             * is several DROPFILE events — each handled here independently. */
            char *path = NULL;
            memcpy(&path, ev + SDL_DROPEVENT_FILE_OFF, sizeof(char *));
            uint32_t dwid = 0;
            memcpy(&dwid, ev + SDL_DROPEVENT_WINDOWID_OFF, 4);
            RxWin *dw = rx_win_by_id(dwid);
            if (dw) {
                ruxen_canvas_push_event_text((int64_t)dw->owner, RX_EV_FILE_DROP,
                                             0, 0, (int64_t)(path ? path : ""));
                if ((int64_t)dw->owner == self) forwarded++;
            }
            if (path && s_SDL_free) s_SDL_free(path);   /* SDL owns it — free now */
            break;
        }
        case SDL_QUIT_EV:
            /* App-wide quit: no windowID. Deliver a CloseRequested to EVERY live
             * window so each can tear down. Count it once for `self`. */
            for (int i = 0; i < RX_MAX_WINDOWS; i++) {
                if (s_wins[i].owner) {
                    ruxen_canvas_push_event((int64_t)s_wins[i].owner, RX_EV_CLOSE, 0.0, 0.0);
                    if ((int64_t)s_wins[i].owner == self) forwarded++;
                }
            }
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
 * passes the scale directly and runs the EXACT mapping the pump uses (rx_map_point_scale)
 * on a synthetic window-point pointer event, pushing the design-mapped result
 * into the host's ring. The test then polls it back and asserts point / scale.
 * Test-only; mirrors the pump's arithmetic so a regression in either is caught. */
int64_t ruxen_canvas_window_pump_test_pointer(int64_t self, int64_t show_scale,
                                              int64_t win_x, int64_t win_y) {
    if (!self) return RXC_ERR_BAD_ARGS;
    int sc = (show_scale >= 1) ? (int)show_scale : 1;
    ruxen_canvas_push_event(self, RX_EV_POINTER_DOWN,
                            rx_map_point_scale((int32_t)win_x, sc),
                            rx_map_point_scale((int32_t)win_y, sc));
    return RXC_OK;
}

/* Test seam: run the pump's SDL_TEXTINPUT handling on a synthetic UTF-8 string —
 * decode the first codepoint and emit Event.TextInput (or nothing on invalid).
 * Returns the codepoint pushed (>= 0), or -1 when nothing was emitted. */
int64_t ruxen_canvas_window_pump_test_text(int64_t self, int64_t utf8_ptr) {
    if (!self) return RXC_ERR_BAD_ARGS;
    const unsigned char *s = (const unsigned char *)utf8_ptr;
    int32_t cp = rx_utf8_first_codepoint(s);
    if (cp >= 0) ruxen_canvas_push_event(self, RX_EV_TEXT_INPUT, (double)cp, 0.0);
    return (int64_t)cp;
}

/* Test seam: run the pump's SDL_TEXTEDITING handling on a synthetic composition
 * (marked UTF-8 text + start + length). Emits Event.TextEditing exactly as the
 * pump does — pushing the marked text (copied into the ring) + cursor. The real
 * pump can't be driven headless (no live SDL window), so this exercises the same
 * push_event_text path the live handler uses. Returns RXC_OK. */
int64_t ruxen_canvas_window_pump_test_textediting(int64_t self, int64_t utf8_ptr,
                                                  int64_t start, int64_t length) {
    if (!self) return RXC_ERR_BAD_ARGS;
    return ruxen_canvas_push_event_text(self, RX_EV_TEXT_EDITING, start, length, utf8_ptr);
}

/* Test seam: run the pump's SDL_KEYDOWN handling on a synthetic (keysym, repeat,
 * mods). Applies the SAME filter the pump uses: skip when repeat != 0; emit
 * KeyDown only for control keysyms (printable keysyms are dropped — TextInput owns
 * them). `mods` is a canvas-side RX_MOD_* mask (already folded — the test passes
 * the final bits, the live pump folds them from SDL_GetModState); it rides the
 * ring slot's side-channel exactly as the live pump's KeyDown does. Returns 1 if a
 * KeyDown was emitted, 0 if filtered. */
int64_t ruxen_canvas_window_pump_test_keydown(int64_t self, int64_t sym, int64_t repeat,
                                              int64_t mods) {
    if (!self) return RXC_ERR_BAD_ARGS;
    if (repeat != 0) return 0;                       /* auto-repeat: filtered */
    if (!rx_is_control_keysym((int32_t)sym)) return 0; /* printable: filtered */
    ruxen_canvas_push_event_mods(self, RX_EV_KEY_DOWN, (double)sym, 0.0, mods);
    return 1;
}

/* Test seam: run the pump's SDL_WINDOWEVENT subtype handling (Phase-1.5) on a
 * synthetic window-state transition, headless. The live pump can't run in the
 * forked harness (no real SDL window), so this allocates a TRANSIENT test slot
 * for `self`, applies the EXACT minimized/Resize logic the pump's WINDOWEVENT
 * case uses (set minimized on MINIMIZED; clear it + emit Resize on MAXIMIZED /
 * RESTORED / DISPLAY_CHANGED), then frees the slot. Returns the slot's resulting
 * `minimized` flag (0/1). The test polls the host ring to assert the Resize.
 * `design_w/design_h` stand in for the owning host's logical size (this seam
 * doesn't read the real host struct, so the caller passes it explicitly). */
int64_t ruxen_canvas_window_pump_test_window_event(int64_t self, int64_t subtype,
                                                   int64_t design_w, int64_t design_h) {
    if (!self) return RXC_ERR_BAD_ARGS;
    /* a real window for this host would shadow the test slot — refuse so we never
     * stomp a live slot (the harness never has one, but be defensive). */
    if (rx_win_for((void *)self)) return RXC_ERR_BUSY;
    RxWin *w = rx_win_alloc((void *)self);
    if (!w) return RXC_ERR_BUSY;
    /* win stays NULL (no OS window) — teardown is a plain memset, no SDL calls. */
    int result = 0;
    unsigned char st = (unsigned char)subtype;
    if (st == SDL_WINDOWEVENT_MINIMIZED) {
        w->minimized = 1;
        result = 1;
    } else if (st == SDL_WINDOWEVENT_MAXIMIZED ||
               st == SDL_WINDOWEVENT_RESTORED ||
               st == SDL_WINDOWEVENT_DISPLAY_CHANGED) {
        w->minimized = 0;
        /* rx_window_on_resized is a no-op here (no glctx / mtl_layer); the Resize
         * emission is the observable part, carrying the design size. */
        rx_window_on_resized(w);
        ruxen_canvas_push_event(self, RX_EV_RESIZE, (double)design_w, (double)design_h);
        result = 0;
    }
    /* free the transient slot (owner = NULL) so it never leaks into another op. */
    memset(w, 0, sizeof(RxWin));
    return result;
}

/* Test seam: run the pump's SDL_DROPFILE handling on a synthetic dropped path,
 * headless. The live pump can't run in the forked harness (no real SDL window),
 * so this exercises the SAME push path the live DROPFILE handler uses — emit
 * Event.FileDrop carrying the path through the ring's owned-path side-channel
 * (push_event_text with the FileDrop kind, which strdup's the full path, no 32-
 * byte truncation). Poll it back as Event.FileDrop + Window#dropped_file_path.
 * Returns RXC_OK (or the push error code). */
int64_t ruxen_canvas_window_pump_test_dropfile(int64_t self, int64_t path_ptr) {
    if (!self) return RXC_ERR_BAD_ARGS;
    return ruxen_canvas_push_event_text(self, RX_EV_FILE_DROP, 0, 0, path_ptr);
}

/* Tear ONE window slot down (idempotent on a free slot). The dlopen handle
 * stays cached.
 *
 * Teardown order matters for the GPU path (docs/GPU.md): the Skia GPU surface +
 * gr_direct_context are released by skia_shim.c's host_drop BEFORE this runs
 * (host_drop calls note_host_dropped -> here). By the time we delete the GL
 * context, no Skia object still references it. The GL context is deleted before
 * the window it was created against, the reverse of creation order. */
static void rx_win_teardown(RxWin *w) {
    if (!w || !w->owner) return;
    if (w->tex) { s_DestroyTexture(w->tex); w->tex = NULL; }
    if (w->ren) { s_DestroyRenderer(w->ren); w->ren = NULL; }
    if (w->glctx) { if (s_GL_DeleteContext) s_GL_DeleteContext(w->glctx); w->glctx = NULL; }
    /* Metal: the GPU surface + gr_context referencing the layer are released by
     * skia_shim's host_drop BEFORE this runs. Any in-flight drawable was already
     * consumed at present (or is dropped here). Order: drawable -> layer (owned
     * by the view) -> view -> window — the reverse of creation. We do not own
     * the device/queue (rx_metal singleton), so they are not destroyed here. */
    w->mtl_drawable = NULL;
    w->mtl_layer    = NULL;   /* owned by the view; freed with it */
    if (w->mtl_view) { if (s_Metal_DestroyView) s_Metal_DestroyView(w->mtl_view); w->mtl_view = NULL; }
    w->mtl_device = NULL;
    w->mtl_queue  = NULL;
    if (w->win) { s_DestroyWindow(w->win); w->win = NULL; }
    memset(w, 0, sizeof(RxWin));   /* frees the slot (owner = NULL) */
}

/* Tear down ALL live windows (idempotent). The legacy no-arg entry point: in a
 * single-window app this is "destroy the window"; with multiple windows it tears
 * them all down. Per-window teardown is ruxen_canvas_window_destroy_for. */
int64_t ruxen_canvas_window_destroy(void) {
    for (int i = 0; i < RX_MAX_WINDOWS; i++) rx_win_teardown(&s_wins[i]);
    return RXC_OK;
}

/* Tear down ONLY the window owned by `self` (idempotent). This is what
 * Window#hide routes through, so hiding one window leaves the others up. */
int64_t ruxen_canvas_window_destroy_for(int64_t self) {
    rx_win_teardown(rx_win_for((void *)self));
    return RXC_OK;
}

/* Called by skia_shim.c's host_drop: a host being freed while it owns a window
 * must take THAT window down with it, or present/pump would read freed memory
 * through the stale owner pointer. Only this host's slot is torn down. */
void ruxen_canvas_window_note_host_dropped(int64_t self) {
    rx_win_teardown(rx_win_for((void *)self));
}
