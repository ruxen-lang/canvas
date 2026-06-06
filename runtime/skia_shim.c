/*
 * skia_shim.c — C shim bridging Ruxen's `canvas` (L1) FFI to the rendering
 * backend.
 *
 * This is step (1) of the incremental-FFI discipline (see docs/FFI.md):
 * every capability the L1 `Canvas` exposes is first wrapped here as a flat
 * C function with a stable, Ruxen-friendly C-ABI signature, then declared in
 * a `lib` block in src/lib.rx, then surfaced as a `Canvas` method.
 *
 * Symbols are prefixed `ruxen_canvas_*` so they never collide with the host
 * runtime. All pointers cross the ABI as machine-word integers (int64_t);
 * `void` returns map to no return value (see docs/FFI.md).
 *
 * Backend: a deterministic software raster target (the "headless" backend).
 * It implements the exact ABI the GPU backend will use, so Skia/SDL can be
 * slotted in behind these signatures later (prebuilt-vs-source is still an
 * open decision — docs/ROADMAP.md) without touching the Ruxen side. It is
 * also what the pin tests run against: every bound call is verified by
 * reading pixels back from the framebuffer.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* floor()/isnan() without libm, so the shim adds no link dependencies.
 * (Geometry values are device pixels — far inside int64 range.) */
static int64_t rxc_floor_to_i64(double v) {
    int64_t i = (int64_t)v;          /* truncates toward zero */
    return (v < (double)i) ? i - 1 : i;
}

static int rxc_is_nan(double v) {
    return v != v;
}

/* Geometry must be finite and within sane pixel range before it can be
 * floored: casting NaN/Inf/huge doubles to int64 is undefined behavior.
 * NaN fails both comparisons, so this rejects NaN too. */
static int rxc_finite_pixels(double v) {
    return v > -1.0e9 && v < 1.0e9;
}

/* ---- status codes shared with src/lib.rx (keep in sync!) ---- */

#define RXC_OK            0
#define RXC_ERR_BAD_ARGS  1  /* invalid dimensions / null handle / bad channel */
#define RXC_ERR_NO_FRAME  2  /* draw call outside begin_frame/end_frame */
#define RXC_ERR_IN_FRAME  3  /* begin_frame while a frame is already open */
#define RXC_ERR_QUEUE_FULL 4 /* event ring buffer is full */
#define RXC_ERR_PRESENT    5 /* presenting the frame to the window failed */

/* ---- the host object ---- */

#define RXC_EVENT_CAP 256
#define RXC_EVENT_KIND_MAX 5   /* CloseRequested — keep in sync with Event in src/lib.rx */

typedef struct {
    int32_t kind;   /* event-kind tag; see the Rxc module in src/lib.rx */
    double  a;      /* x / keycode / width  (event-kind dependent) */
    double  b;      /* y / unused  / height (event-kind dependent) */
} RxEvent;

/* NOT thread-safe: an RxHost has exactly one owner (the Ruxen Canvas /
 * Window) and all calls — in particular poll_event followed by the
 * pending-payload accessors — must come from one thread. */
typedef struct {
    int32_t   width;
    int32_t   height;
    uint32_t *pixels;      /* width*height, 0xAARRGGBB, non-premultiplied */
    int32_t   in_frame;    /* begin_frame/end_frame discipline flag */

    /* event ring buffer (filled by the platform pump or by tests) */
    RxEvent   events[RXC_EVENT_CAP];
    int32_t   ev_head;
    int32_t   ev_count;
    RxEvent   pending;     /* the event most recently popped by poll */

    /* live-window presentation (NULL/0 on the headless path) */
    void     *sdl_window;
    void     *sdl_renderer;
    void     *sdl_texture;
    int32_t   windowed;
} RxHost;

/* ---- SDL3 dynamic presentation backend ----
 *
 * Loaded at runtime with dlopen("libSDL3.so.0") so the package needs no
 * SDL development files and keeps zero link-time dependencies. When the
 * library or a display is unavailable, window_open reports failure and the
 * caller falls back to the headless framebuffer — same drawing ABI either
 * way. Constants below were validated empirically against SDL 3 (probe in
 * the repo history): INIT_VIDEO, ARGB8888, STREAMING, RESIZABLE, the
 * event-type values, and the f32 x/y payload offsets. */

#include <dlfcn.h>

#define RX_SDL_INIT_VIDEO      0x20u
#define RX_SDL_ARGB8888        0x16362004u
#define RX_SDL_TEX_STREAMING   1
#define RX_SDL_WIN_RESIZABLE   0x20u
#define RX_SDL_EVENT_QUIT          0x100u
#define RX_SDL_EVENT_KEY_DOWN      0x300u
#define RX_SDL_EVENT_MOUSE_MOTION  0x400u
#define RX_SDL_EVENT_MOUSE_DOWN    0x401u
#define RX_SDL_EVENT_MOUSE_UP      0x402u
#define RX_SDL_EVENT_WINDOW_FIRST  0x200u
#define RX_SDL_EVENT_WINDOW_LAST   0x2FFu

typedef struct {
    int  (*init)(uint32_t);
    void *(*create_window)(const char *, int, int, uint64_t);
    void *(*create_renderer)(void *, const char *);
    void *(*create_texture)(void *, uint32_t, int, int, int);
    int  (*update_texture)(void *, const void *, const void *, int);
    int  (*render_clear)(void *);
    int  (*render_texture)(void *, void *, const void *, const void *);
    void (*render_present)(void *);
    int  (*poll_event)(void *);
    int  (*get_window_size)(void *, int *, int *);
    void (*destroy_texture)(void *);
    void (*destroy_renderer)(void *);
    void (*destroy_window)(void *);
} RxSdl;

static RxSdl rx_sdl;
static int rx_sdl_state = 0;   /* 0 = untried, 1 = loaded+inited, -1 = unavailable */

static int rx_sdl_ready(void) {
    if (rx_sdl_state != 0) return rx_sdl_state == 1;
    rx_sdl_state = -1;
    void *lib = dlopen("libSDL3.so.0", RTLD_NOW);
    if (!lib) return 0;
    rx_sdl.init            = (int (*)(uint32_t))dlsym(lib, "SDL_Init");
    rx_sdl.create_window   = (void *(*)(const char *, int, int, uint64_t))dlsym(lib, "SDL_CreateWindow");
    rx_sdl.create_renderer = (void *(*)(void *, const char *))dlsym(lib, "SDL_CreateRenderer");
    rx_sdl.create_texture  = (void *(*)(void *, uint32_t, int, int, int))dlsym(lib, "SDL_CreateTexture");
    rx_sdl.update_texture  = (int (*)(void *, const void *, const void *, int))dlsym(lib, "SDL_UpdateTexture");
    rx_sdl.render_clear    = (int (*)(void *))dlsym(lib, "SDL_RenderClear");
    rx_sdl.render_texture  = (int (*)(void *, void *, const void *, const void *))dlsym(lib, "SDL_RenderTexture");
    rx_sdl.render_present  = (void (*)(void *))dlsym(lib, "SDL_RenderPresent");
    rx_sdl.poll_event      = (int (*)(void *))dlsym(lib, "SDL_PollEvent");
    rx_sdl.get_window_size = (int (*)(void *, int *, int *))dlsym(lib, "SDL_GetWindowSize");
    rx_sdl.destroy_texture = (void (*)(void *))dlsym(lib, "SDL_DestroyTexture");
    rx_sdl.destroy_renderer = (void (*)(void *))dlsym(lib, "SDL_DestroyRenderer");
    rx_sdl.destroy_window  = (void (*)(void *))dlsym(lib, "SDL_DestroyWindow");
    if (!rx_sdl.init || !rx_sdl.create_window || !rx_sdl.create_renderer ||
        !rx_sdl.create_texture || !rx_sdl.update_texture || !rx_sdl.render_clear ||
        !rx_sdl.render_texture || !rx_sdl.render_present || !rx_sdl.poll_event ||
        !rx_sdl.get_window_size || !rx_sdl.destroy_texture ||
        !rx_sdl.destroy_renderer || !rx_sdl.destroy_window) {
        return 0;
    }
    if (!rx_sdl.init(RX_SDL_INIT_VIDEO)) return 0;
    rx_sdl_state = 1;
    return 1;
}

/* ---- lifecycle ---- */

/* forward declarations (window_open tears down via drop; the SDL pump
 * feeds the ring via push_event) */
void ruxen_canvas_host_drop(int64_t self);
int64_t ruxen_canvas_push_event(int64_t self, int64_t kind, double a, double b);

/* Create a host with a width*height framebuffer, initially fully
 * transparent. Returns the handle as int64_t, or 0 on bad dimensions /
 * allocation failure. */
int64_t ruxen_canvas_host_new(int64_t width, int64_t height) {
    if (width <= 0 || height <= 0 || width > 16384 || height > 16384) {
        return 0;
    }
    RxHost *h = (RxHost *)calloc(1, sizeof(RxHost));
    if (!h) return 0;
    h->width  = (int32_t)width;
    h->height = (int32_t)height;
    h->pixels = (uint32_t *)calloc((size_t)(width * height), sizeof(uint32_t));
    if (!h->pixels) {
        free(h);
        return 0;
    }
    return (int64_t)h;
}

/* True when the handle is null — lets the Ruxen side turn an allocation
 * failure in ruxen_canvas_host_new into a proper Err instead of a zombie
 * object. */
int64_t ruxen_canvas_host_is_null(int64_t self) {
    return self == 0 ? 1 : 0;
}

/* Open a host with a LIVE OS window over it (SDL3 via dlopen). Returns 0
 * when SDL or a display is unavailable, or on bad dimensions — the caller
 * falls back to the headless path. The framebuffer is identical to the
 * headless one; end_frame additionally presents it to the window. */
int64_t ruxen_canvas_window_open(int64_t title, int64_t width, int64_t height) {
    const char *t = (const char *)title;
    if (!t) return 0;
    int64_t handle = ruxen_canvas_host_new(width, height);
    if (!handle) return 0;
    RxHost *h = (RxHost *)handle;
    if (!rx_sdl_ready()) {
        ruxen_canvas_host_drop(handle);
        return 0;
    }
    h->sdl_window = rx_sdl.create_window(t, (int)width, (int)height, RX_SDL_WIN_RESIZABLE);
    if (h->sdl_window) {
        h->sdl_renderer = rx_sdl.create_renderer(h->sdl_window, NULL);
    }
    if (h->sdl_renderer) {
        h->sdl_texture = rx_sdl.create_texture(h->sdl_renderer, RX_SDL_ARGB8888,
                                               RX_SDL_TEX_STREAMING, (int)width, (int)height);
    }
    if (!h->sdl_texture) {
        if (h->sdl_renderer) rx_sdl.destroy_renderer(h->sdl_renderer);
        if (h->sdl_window)   rx_sdl.destroy_window(h->sdl_window);
        ruxen_canvas_host_drop(handle);
        return 0;
    }
    h->windowed = 1;
    return handle;
}

/* True when this host presents to a live OS window (vs headless). */
int64_t ruxen_canvas_host_is_windowed(int64_t self) {
    RxHost *h = (RxHost *)self;
    return (h && h->windowed) ? 1 : 0;
}

/* Tear the host down. Called from the Ruxen side's drop — deterministic,
 * no GC. */
void ruxen_canvas_host_drop(int64_t self) {
    RxHost *h = (RxHost *)self;
    if (!h) return;
    if (h->windowed) {
        if (h->sdl_texture)  rx_sdl.destroy_texture(h->sdl_texture);
        if (h->sdl_renderer) rx_sdl.destroy_renderer(h->sdl_renderer);
        if (h->sdl_window)   rx_sdl.destroy_window(h->sdl_window);
    }
    free(h->pixels);
    free(h);
}

int64_t ruxen_canvas_host_width(int64_t self) {
    RxHost *h = (RxHost *)self;
    return h ? h->width : 0;
}

int64_t ruxen_canvas_host_height(int64_t self) {
    RxHost *h = (RxHost *)self;
    return h ? h->height : 0;
}

/* Read one pixel back as packed 0xAARRGGBB. Returns -1 when out of bounds.
 * This is the observation hook every pin test uses. */
int64_t ruxen_canvas_read_pixel(int64_t self, int64_t x, int64_t y) {
    RxHost *h = (RxHost *)self;
    if (!h || x < 0 || y < 0 || x >= h->width || y >= h->height) {
        return -1;
    }
    return (int64_t)h->pixels[y * h->width + x];
}

/* ---- frame discipline ---- */

int64_t ruxen_canvas_begin_frame(int64_t self) {
    RxHost *h = (RxHost *)self;
    if (!h) return RXC_ERR_BAD_ARGS;
    if (h->in_frame) return RXC_ERR_IN_FRAME;
    h->in_frame = 1;
    return RXC_OK;
}

/* Present the frame: headless hosts have nothing to flip; windowed hosts
 * upload the framebuffer to the SDL texture and present it. */
int64_t ruxen_canvas_end_frame(int64_t self) {
    RxHost *h = (RxHost *)self;
    if (!h) return RXC_ERR_BAD_ARGS;
    if (!h->in_frame) return RXC_ERR_NO_FRAME;
    h->in_frame = 0;
    if (h->windowed) {
        if (!rx_sdl.update_texture(h->sdl_texture, NULL, h->pixels, h->width * 4)) {
            return RXC_ERR_PRESENT;
        }
        rx_sdl.render_clear(h->sdl_renderer);
        if (!rx_sdl.render_texture(h->sdl_renderer, h->sdl_texture, NULL, NULL)) {
            return RXC_ERR_PRESENT;
        }
        rx_sdl.render_present(h->sdl_renderer);
    }
    return RXC_OK;
}

/* ---- drawing ---- */

static int rxc_check_color(int64_t r, int64_t g, int64_t b, int64_t a) {
    return r >= 0 && r <= 255 && g >= 0 && g <= 255 &&
           b >= 0 && b <= 255 && a >= 0 && a <= 255;
}

static uint32_t rxc_pack(int64_t r, int64_t g, int64_t b, int64_t a) {
    return ((uint32_t)a << 24) | ((uint32_t)r << 16) |
           ((uint32_t)g << 8)  |  (uint32_t)b;
}

/* Source-over blend of src (non-premultiplied) onto dst. */
static uint32_t rxc_blend(uint32_t dst, uint32_t src) {
    uint32_t sa = (src >> 24) & 0xFF;
    if (sa == 255) return src;
    if (sa == 0)   return dst;
    uint32_t da = (dst >> 24) & 0xFF;
    uint32_t inv = 255 - sa;
    uint32_t out_a = sa + (da * inv + 127) / 255;
    if (out_a == 0) return 0;
    uint32_t channels[3];
    int shift;
    for (shift = 0; shift < 3; shift++) {
        uint32_t sc = (src >> (16 - 8 * shift)) & 0xFF;
        uint32_t dc = (dst >> (16 - 8 * shift)) & 0xFF;
        /* blend premultiplied, then un-premultiply by out_a */
        uint32_t num = sc * sa * 255 + dc * da * inv;
        channels[shift] = num / (out_a * 255);
        if (channels[shift] > 255) channels[shift] = 255;
    }
    return (out_a << 24) | (channels[0] << 16) | (channels[1] << 8) | channels[2];
}

/* Clear the whole surface to a solid color (replaces, no blending —
 * matching SkCanvas::clear semantics). */
int64_t ruxen_canvas_clear(int64_t self, int64_t r, int64_t g, int64_t b, int64_t a) {
    RxHost *h = (RxHost *)self;
    if (!h || !rxc_check_color(r, g, b, a)) return RXC_ERR_BAD_ARGS;
    if (!h->in_frame) return RXC_ERR_NO_FRAME;
    uint32_t px = rxc_pack(r, g, b, a);
    int64_t n = (int64_t)h->width * h->height;
    for (int64_t i = 0; i < n; i++) h->pixels[i] = px;
    return RXC_OK;
}

/* Fill an axis-aligned rect with source-over blending, clipped to the
 * surface. Geometry comes in as doubles (Ruxen Float32 widens at the call);
 * the filled pixel box is [floor(x), floor(x+w)) x [floor(y), floor(y+h)). */
int64_t ruxen_canvas_draw_rect(int64_t self, double x, double y, double w, double hgt,
                               int64_t r, int64_t g, int64_t b, int64_t a) {
    RxHost *h = (RxHost *)self;
    if (!h || !rxc_check_color(r, g, b, a)) return RXC_ERR_BAD_ARGS;
    if (!h->in_frame) return RXC_ERR_NO_FRAME;
    if (rxc_is_nan(w) || rxc_is_nan(hgt)) return RXC_ERR_BAD_ARGS;
    if (!(w > 0.0) || !(hgt > 0.0)) return RXC_OK;     /* empty: nothing to draw */
    if (!rxc_finite_pixels(x) || !rxc_finite_pixels(y) ||
        !rxc_finite_pixels(w) || !rxc_finite_pixels(hgt)) {
        return RXC_ERR_BAD_ARGS;
    }

    int64_t x0 = rxc_floor_to_i64(x);
    int64_t y0 = rxc_floor_to_i64(y);
    int64_t x1 = rxc_floor_to_i64(x + w);
    int64_t y1 = rxc_floor_to_i64(y + hgt);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > h->width)  x1 = h->width;
    if (y1 > h->height) y1 = h->height;

    uint32_t src = rxc_pack(r, g, b, a);
    for (int64_t py = y0; py < y1; py++) {
        uint32_t *row = h->pixels + py * h->width;
        for (int64_t px = x0; px < x1; px++) {
            row[px] = rxc_blend(row[px], src);
        }
    }
    return RXC_OK;
}

/* ---- text: embedded 5x7 bitmap font ---- */
/* Classic 5x7 ASCII font (0x20..0x7E), column-major: 5 bytes per glyph,
 * bit 0 of each byte is the top row. Good enough for the counter-app
 * slice; Skia paragraph / HarfBuzz shaping replaces this later. */

#define RXC_GLYPH_W   5
#define RXC_GLYPH_H   7
#define RXC_ADVANCE   6  /* glyph width + 1px spacing */

static const uint8_t rxc_font5x7[95][5] = {
    {0x00,0x00,0x00,0x00,0x00}, /* ' ' */
    {0x00,0x00,0x5F,0x00,0x00}, /* '!' */
    {0x00,0x07,0x00,0x07,0x00}, /* '"' */
    {0x14,0x7F,0x14,0x7F,0x14}, /* '#' */
    {0x24,0x2A,0x7F,0x2A,0x12}, /* '$' */
    {0x23,0x13,0x08,0x64,0x62}, /* '%' */
    {0x36,0x49,0x55,0x22,0x50}, /* '&' */
    {0x00,0x05,0x03,0x00,0x00}, /* ''' */
    {0x00,0x1C,0x22,0x41,0x00}, /* '(' */
    {0x00,0x41,0x22,0x1C,0x00}, /* ')' */
    {0x14,0x08,0x3E,0x08,0x14}, /* '*' */
    {0x08,0x08,0x3E,0x08,0x08}, /* '+' */
    {0x00,0x50,0x30,0x00,0x00}, /* ',' */
    {0x08,0x08,0x08,0x08,0x08}, /* '-' */
    {0x00,0x60,0x60,0x00,0x00}, /* '.' */
    {0x20,0x10,0x08,0x04,0x02}, /* '/' */
    {0x3E,0x51,0x49,0x45,0x3E}, /* '0' */
    {0x00,0x42,0x7F,0x40,0x00}, /* '1' */
    {0x42,0x61,0x51,0x49,0x46}, /* '2' */
    {0x21,0x41,0x45,0x4B,0x31}, /* '3' */
    {0x18,0x14,0x12,0x7F,0x10}, /* '4' */
    {0x27,0x45,0x45,0x45,0x39}, /* '5' */
    {0x3C,0x4A,0x49,0x49,0x30}, /* '6' */
    {0x01,0x71,0x09,0x05,0x03}, /* '7' */
    {0x36,0x49,0x49,0x49,0x36}, /* '8' */
    {0x06,0x49,0x49,0x29,0x1E}, /* '9' */
    {0x00,0x36,0x36,0x00,0x00}, /* ':' */
    {0x00,0x56,0x36,0x00,0x00}, /* ';' */
    {0x08,0x14,0x22,0x41,0x00}, /* '<' */
    {0x14,0x14,0x14,0x14,0x14}, /* '=' */
    {0x00,0x41,0x22,0x14,0x08}, /* '>' */
    {0x02,0x01,0x51,0x09,0x06}, /* '?' */
    {0x32,0x49,0x79,0x41,0x3E}, /* '@' */
    {0x7E,0x11,0x11,0x11,0x7E}, /* 'A' */
    {0x7F,0x49,0x49,0x49,0x36}, /* 'B' */
    {0x3E,0x41,0x41,0x41,0x22}, /* 'C' */
    {0x7F,0x41,0x41,0x22,0x1C}, /* 'D' */
    {0x7F,0x49,0x49,0x49,0x41}, /* 'E' */
    {0x7F,0x09,0x09,0x09,0x01}, /* 'F' */
    {0x3E,0x41,0x49,0x49,0x7A}, /* 'G' */
    {0x7F,0x08,0x08,0x08,0x7F}, /* 'H' */
    {0x00,0x41,0x7F,0x41,0x00}, /* 'I' */
    {0x20,0x40,0x41,0x3F,0x01}, /* 'J' */
    {0x7F,0x08,0x14,0x22,0x41}, /* 'K' */
    {0x7F,0x40,0x40,0x40,0x40}, /* 'L' */
    {0x7F,0x02,0x0C,0x02,0x7F}, /* 'M' */
    {0x7F,0x04,0x08,0x10,0x7F}, /* 'N' */
    {0x3E,0x41,0x41,0x41,0x3E}, /* 'O' */
    {0x7F,0x09,0x09,0x09,0x06}, /* 'P' */
    {0x3E,0x41,0x51,0x21,0x5E}, /* 'Q' */
    {0x7F,0x09,0x19,0x29,0x46}, /* 'R' */
    {0x46,0x49,0x49,0x49,0x31}, /* 'S' */
    {0x01,0x01,0x7F,0x01,0x01}, /* 'T' */
    {0x3F,0x40,0x40,0x40,0x3F}, /* 'U' */
    {0x1F,0x20,0x40,0x20,0x1F}, /* 'V' */
    {0x3F,0x40,0x38,0x40,0x3F}, /* 'W' */
    {0x63,0x14,0x08,0x14,0x63}, /* 'X' */
    {0x07,0x08,0x70,0x08,0x07}, /* 'Y' */
    {0x61,0x51,0x49,0x45,0x43}, /* 'Z' */
    {0x00,0x7F,0x41,0x41,0x00}, /* '[' */
    {0x02,0x04,0x08,0x10,0x20}, /* '\' */
    {0x00,0x41,0x41,0x7F,0x00}, /* ']' */
    {0x04,0x02,0x01,0x02,0x04}, /* '^' */
    {0x40,0x40,0x40,0x40,0x40}, /* '_' */
    {0x00,0x01,0x02,0x04,0x00}, /* '`' */
    {0x20,0x54,0x54,0x54,0x78}, /* 'a' */
    {0x7F,0x48,0x44,0x44,0x38}, /* 'b' */
    {0x38,0x44,0x44,0x44,0x20}, /* 'c' */
    {0x38,0x44,0x44,0x48,0x7F}, /* 'd' */
    {0x38,0x54,0x54,0x54,0x18}, /* 'e' */
    {0x08,0x7E,0x09,0x01,0x02}, /* 'f' */
    {0x0C,0x52,0x52,0x52,0x3E}, /* 'g' */
    {0x7F,0x08,0x04,0x04,0x78}, /* 'h' */
    {0x00,0x44,0x7D,0x40,0x00}, /* 'i' */
    {0x20,0x40,0x44,0x3D,0x00}, /* 'j' */
    {0x7F,0x10,0x28,0x44,0x00}, /* 'k' */
    {0x00,0x41,0x7F,0x40,0x00}, /* 'l' */
    {0x7C,0x04,0x18,0x04,0x78}, /* 'm' */
    {0x7C,0x08,0x04,0x04,0x78}, /* 'n' */
    {0x38,0x44,0x44,0x44,0x38}, /* 'o' */
    {0x7C,0x14,0x14,0x14,0x08}, /* 'p' */
    {0x08,0x14,0x14,0x18,0x7C}, /* 'q' */
    {0x7C,0x08,0x04,0x04,0x08}, /* 'r' */
    {0x48,0x54,0x54,0x54,0x20}, /* 's' */
    {0x04,0x3F,0x44,0x40,0x20}, /* 't' */
    {0x3C,0x40,0x40,0x20,0x7C}, /* 'u' */
    {0x1C,0x20,0x40,0x20,0x1C}, /* 'v' */
    {0x3C,0x40,0x30,0x40,0x3C}, /* 'w' */
    {0x44,0x28,0x10,0x28,0x44}, /* 'x' */
    {0x0C,0x50,0x50,0x50,0x3C}, /* 'y' */
    {0x44,0x64,0x54,0x4C,0x44}, /* 'z' */
    {0x00,0x08,0x36,0x41,0x00}, /* '{' */
    {0x00,0x00,0x7F,0x00,0x00}, /* '|' */
    {0x00,0x41,0x36,0x08,0x00}, /* '}' */
    {0x10,0x08,0x08,0x10,0x08}, /* '~' */
};

/* Width in pixels of an n-character single line at this font's one size.
 * Each glyph advances RXC_ADVANCE px; no trailing gap. The CHARACTER COUNT
 * crosses the FFI (not the string): the metric stays defined here, and the
 * call uses only integer arguments. */
int64_t ruxen_canvas_measure_text_n(int64_t self, int64_t n) {
    (void)self;
    if (n <= 0) return 0;
    return n * RXC_ADVANCE - 1;
}

/* The font's line height (ascent above the baseline), in pixels. */
int64_t ruxen_canvas_text_height(int64_t self) {
    (void)self;
    return RXC_GLYPH_H;
}

/* Draw a single line of text. (x, y) is the BASELINE origin: glyphs occupy
 * the 7 rows above y. Characters outside printable ASCII render as a
 * replacement box. Source-over blended, clipped to the surface. */
int64_t ruxen_canvas_draw_text(int64_t self, int64_t text, double x, double y,
                               int64_t r, int64_t g, int64_t b, int64_t a) {
    RxHost *h = (RxHost *)self;
    const char *s = (const char *)text;
    if (!h || !s || !rxc_check_color(r, g, b, a)) return RXC_ERR_BAD_ARGS;
    if (!h->in_frame) return RXC_ERR_NO_FRAME;
    if (!rxc_finite_pixels(x) || !rxc_finite_pixels(y)) return RXC_ERR_BAD_ARGS;

    uint32_t src = rxc_pack(r, g, b, a);
    int64_t pen_x = rxc_floor_to_i64(x);
    int64_t top   = rxc_floor_to_i64(y) - RXC_GLYPH_H;

    for (; *s; s++, pen_x += RXC_ADVANCE) {
        unsigned char ch = (unsigned char)*s;
        const uint8_t *glyph;
        uint8_t box[5] = {0x7F, 0x41, 0x41, 0x41, 0x7F}; /* replacement box */
        if (ch >= 0x20 && ch <= 0x7E) {
            glyph = rxc_font5x7[ch - 0x20];
        } else {
            glyph = box;
        }
        for (int col = 0; col < RXC_GLYPH_W; col++) {
            int64_t px = pen_x + col;
            if (px < 0 || px >= h->width) continue;
            for (int row = 0; row < RXC_GLYPH_H; row++) {
                if (!((glyph[col] >> row) & 1)) continue;
                int64_t py = top + row;
                if (py < 0 || py >= h->height) continue;
                uint32_t *p = h->pixels + py * h->width + px;
                *p = rxc_blend(*p, src);
            }
        }
    }
    return RXC_OK;
}

/* ---- event queue ---- */
/* The platform pump (SDL later; tests today) pushes events in; the Ruxen
 * side polls them out one at a time. poll pops the next event into the
 * `pending` slot and returns its kind (-1 when the queue is empty); the
 * payload accessors then read from `pending`. */

int64_t ruxen_canvas_push_event(int64_t self, int64_t kind, double a, double b) {
    RxHost *h = (RxHost *)self;
    if (!h || kind < 0 || kind > RXC_EVENT_KIND_MAX) return RXC_ERR_BAD_ARGS;
    if (h->ev_count >= RXC_EVENT_CAP) return RXC_ERR_QUEUE_FULL;
    int32_t tail = (h->ev_head + h->ev_count) % RXC_EVENT_CAP;
    h->events[tail].kind = (int32_t)kind;
    h->events[tail].a = a;
    h->events[tail].b = b;
    h->ev_count++;
    return RXC_OK;
}

/* Translate pending SDL events into the ring. Event-struct payload
 * offsets (x: +28, y: +32 as f32; keycode: +28 as u32) were verified
 * empirically against SDL 3. Window events are detected by type range and
 * checked against the actual window size, so individual window-event
 * constants don't need to be hardcoded. */
static void rx_pump_sdl(RxHost *h) {
    if (!h->windowed) return;
    unsigned char ev[256];
    while (rx_sdl.poll_event(ev)) {
        uint32_t type;
        memcpy(&type, ev, sizeof type);
        if (type == RX_SDL_EVENT_QUIT) {
            ruxen_canvas_push_event((int64_t)h, 5, 0, 0);              /* CloseRequested */
        } else if (type == RX_SDL_EVENT_KEY_DOWN) {
            uint32_t key;
            memcpy(&key, ev + 28, sizeof key);
            ruxen_canvas_push_event((int64_t)h, 3, (double)key, 0);    /* KeyDown */
        } else if (type == RX_SDL_EVENT_MOUSE_MOTION ||
                   type == RX_SDL_EVENT_MOUSE_DOWN ||
                   type == RX_SDL_EVENT_MOUSE_UP) {
            float x, y;
            memcpy(&x, ev + 28, sizeof x);
            memcpy(&y, ev + 32, sizeof y);
            int64_t kind = type == RX_SDL_EVENT_MOUSE_MOTION ? 0
                         : type == RX_SDL_EVENT_MOUSE_DOWN   ? 1 : 2;
            ruxen_canvas_push_event((int64_t)h, kind, (double)x, (double)y);
        } else if (type >= RX_SDL_EVENT_WINDOW_FIRST && type <= RX_SDL_EVENT_WINDOW_LAST) {
            int ww = 0, hh = 0;
            rx_sdl.get_window_size(h->sdl_window, &ww, &hh);
            if (ww > 0 && hh > 0 && (ww != h->width || hh != h->height)) {
                /* resize: fresh transparent framebuffer + texture; the
                 * Resize event tells L2 to repaint */
                uint32_t *np = (uint32_t *)calloc((size_t)ww * hh, sizeof(uint32_t));
                void *nt = rx_sdl.create_texture(h->sdl_renderer, RX_SDL_ARGB8888,
                                                 RX_SDL_TEX_STREAMING, ww, hh);
                if (np && nt) {
                    free(h->pixels);
                    rx_sdl.destroy_texture(h->sdl_texture);
                    h->pixels = np;
                    h->sdl_texture = nt;
                    h->width = ww;
                    h->height = hh;
                    ruxen_canvas_push_event((int64_t)h, 4, (double)ww, (double)hh);
                } else {
                    free(np);
                    if (nt) rx_sdl.destroy_texture(nt);
                }
            }
        }
        /* other event types: not part of the L1 surface yet */
    }
}

int64_t ruxen_canvas_poll_event(int64_t self) {
    RxHost *h = (RxHost *)self;
    if (!h) return -1;
    rx_pump_sdl(h);
    if (h->ev_count == 0) return -1;
    h->pending = h->events[h->ev_head];
    h->ev_head = (h->ev_head + 1) % RXC_EVENT_CAP;
    h->ev_count--;
    return h->pending.kind;
}

double ruxen_canvas_event_a(int64_t self) {
    RxHost *h = (RxHost *)self;
    return h ? h->pending.a : 0.0;
}

double ruxen_canvas_event_b(int64_t self) {
    RxHost *h = (RxHost *)self;
    return h ? h->pending.b : 0.0;
}

/* Integer-typed companions: the Ruxen side currently can't lower Int<->
 * Float casts inside class methods, so the conversions happen here. The
 * double-typed entry points above remain the canonical ABI for the
 * eventual Float32 event payloads. */

int64_t ruxen_canvas_push_event_i(int64_t self, int64_t kind, int64_t a, int64_t b) {
    return ruxen_canvas_push_event(self, kind, (double)a, (double)b);
}

int64_t ruxen_canvas_event_ai(int64_t self) {
    RxHost *h = (RxHost *)self;
    return h ? (int64_t)h->pending.a : 0;
}

int64_t ruxen_canvas_event_bi(int64_t self) {
    RxHost *h = (RxHost *)self;
    return h ? (int64_t)h->pending.b : 0;
}
