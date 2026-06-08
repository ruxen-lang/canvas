/*
 * rx_canvas_internal.h — shared internals of the canvas C shim.
 *
 * skia_shim.c (the deterministic software raster backend) includes this.
 * runtime/sdl_window.c (the live-window presenter) deliberately does NOT —
 * it reads only the leading width/height/pixels fields through its own
 * layout-prefix view, so the two translation units stay decoupled. If the
 * leading field order of RxHost changes, update RxHostPrefix there.
 *
 * Everything here is internal to the shim — the Ruxen side only ever sees
 * the flat ruxen_canvas_* functions (docs/FFI.md).
 */
#ifndef RX_CANVAS_INTERNAL_H
#define RX_CANVAS_INTERNAL_H

#include <stdint.h>

/* ---- status codes shared with src/lib.rx (keep in sync!) ---- */

#define RXC_OK             0
#define RXC_ERR_BAD_ARGS   1  /* invalid dimensions / null handle / bad channel */
#define RXC_ERR_NO_FRAME   2  /* draw call outside begin_frame/end_frame */
#define RXC_ERR_IN_FRAME   3  /* begin_frame while a frame is already open */
#define RXC_ERR_QUEUE_FULL 4  /* event ring buffer is full */
#define RXC_ERR_PRESENT    5  /* presenting to the platform window failed /
                                 no platform window available */
/* 6 (NO_SDL) and 7 (BUSY) are reserved by runtime/sdl_window.c. */
#define RXC_ERR_NO_SKIA    8  /* op requires the Skia backend, which is not
                                 loaded (run runtime/fetch_skia.sh) */

/* ---- events ---- */

#define RXC_EVENT_CAP 256
#define RXC_EVENT_KIND_MAX 7  /* TextInput — keep in sync with Event in src/lib.rx */

typedef struct {
    int32_t kind;   /* event-kind tag; see the Rxc module in src/lib.rx */
    double  a;      /* x / keycode / width  (event-kind dependent) */
    double  b;      /* y / unused  / height (event-kind dependent) */
} RxEvent;

/* ---- the host object ----
 *
 * NOT thread-safe: an RxHost has exactly one owner (the Ruxen Canvas /
 * Window) and all calls — in particular poll_event followed by the
 * pending-payload accessors — must come from one thread.
 *
 * width/height/pixels MUST stay the leading fields in this order — they
 * are sdl_window.c's RxHostPrefix view. */
typedef struct RxHost {
    int32_t   width;
    int32_t   height;
    /* width*height, packed 0xAARRGGBB. Alpha convention depends on the active
     * backend: the software raster path stores STRAIGHT (non-premultiplied)
     * alpha; when the Skia backend is live the buffer holds PREMULTIPLIED alpha
     * (the surface is created kBGRA_8888 / kPremul). The two agree for opaque
     * pixels (a=255), which is why opaque draws read back identically under
     * both. The SDL presenter only ever displays it, so it is agnostic. */
    uint32_t *pixels;
    int32_t   in_frame;    /* begin_frame/end_frame discipline flag */

    /* event ring buffer (filled by the platform pump or by tests) */
    RxEvent   events[RXC_EVENT_CAP];
    int32_t   ev_head;
    int32_t   ev_count;
    RxEvent   pending;     /* the event most recently popped by poll */

    /* Skia raster-direct state (trailing — invisible to sdl_window.c's
     * RxHostPrefix view). Kept as void pointers so this header stays decoupled
     * from skia_capi.h; skia_shim.c casts them to the sk_surface_t / sk_canvas_t
     * handle types. The surface wraps `pixels` for the host's lifetime; NULL
     * when Skia is unavailable or surface creation failed (sk_tried records the
     * attempt so we don't retry every draw). */
    void     *sk_surface;
    void     *sk_canvas;
    int32_t   sk_tried;

    /* GPU (Ganesh GL) backend state (trailing — invisible to sdl_window.c's
     * RxHostPrefix view; docs/GPU.md). Set only when this host was put into GPU
     * mode (a GL window + context were created for it and the GPU surface was
     * built). gpu_requested gates whether rx_host_canvas even ATTEMPTS the GPU
     * rung, so existing offscreen/raster hosts are wholly unaffected.
     *   gr_context  — the GrDirectContext over the window's GL context
     *   gr_target   — the GrBackendRenderTarget wrapping the default FBO
     *   gpu_surface — the GPU-backed SkSurface (its canvas is sk_canvas when GPU
     *                 is active); released BEFORE gr_context, both before the GL
     *                 context is deleted (teardown order, docs/GPU.md).
     * gl_interface  — the GrGLInterface (released last of the GPU objects). */
    void     *gr_context;
    void     *gr_target;
    void     *gpu_surface;
    void     *gl_interface;
    int32_t   gpu_requested;  /* caller asked for the GPU backend on this host */
    int32_t   gpu_tried;      /* GPU surface creation attempted (success or not) */
    int32_t   is_gpu;         /* 1 iff sk_canvas currently targets the GPU surface */

    /* Which GPU backend is live for this host (RX_GPU_KIND_*; 0 = raster). The
     * Metal rung reuses gr_context (its GrDirectContext) + gpu_surface (an
     * OFFSCREEN GPU surface) but has no gr_target / gl_interface. For the Metal
     * offscreen path, gpu_offscreen marks that end_frame must read the GPU
     * surface's pixels back into `pixels` so read_pixel observes real GPU output
     * (the headless pixel-verification path — docs/GPU.md). The Metal device +
     * command queue are a process-wide singleton (rx_metal), NOT per-host. */
    int32_t   gpu_backend_kind;
    int32_t   gpu_offscreen;  /* 1 iff this GPU host renders offscreen + reads back */
    /* 1 iff this GPU host renders to an ON-SCREEN Metal CAMetalLayer: each frame
     * acquires the layer's next drawable, builds a per-frame GPU surface over the
     * drawable's texture, flush+submits, and presents the drawable. The surface +
     * backend-render-target are per-frame (a fresh drawable each frame), unlike
     * the offscreen path's persistent surface. docs/GPU.md. */
    int32_t   gpu_windowed;

    /* ---- HiDPI design->backing content scale (docs/GPU.md) ----
     * The app draws at DESIGN coordinates (e.g. Window.open(320,360) -> draws a
     * 320x360 UI), but the windowed surface is sized to the native BACKING pixels
     * (e.g. 640x720 on a 2x display). To render design content crisp at native
     * resolution, begin_frame applies a base transform scale = surface_size /
     * design_size as the FIRST canvas transform, so all design-coord draws fill
     * the backing surface and Skia rasterizes glyphs/shapes at native density.
     *
     * design_w/design_h == 0 means "no content scale" — the default for offscreen
     * / test surfaces, which draw at explicit pixel sizes (scale 1). Windowed
     * hosts set this to their logical (design) size at enable; an offscreen host
     * can set it explicitly to make the content-scale logic headless-testable
     * (a backing-sized framebuffer with a smaller design size). */
    int32_t   design_w;
    int32_t   design_h;
} RxHost;

/* the ring-buffer feeder, defined in skia_shim.c, used by the pump */
int64_t ruxen_canvas_push_event(int64_t self, int64_t kind, double a, double b);

#endif /* RX_CANVAS_INTERNAL_H */
