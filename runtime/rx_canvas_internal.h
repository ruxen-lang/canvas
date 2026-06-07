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
#define RXC_EVENT_KIND_MAX 5  /* CloseRequested — keep in sync with Event in src/lib.rx */

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
} RxHost;

/* the ring-buffer feeder, defined in skia_shim.c, used by the pump */
int64_t ruxen_canvas_push_event(int64_t self, int64_t kind, double a, double b);

#endif /* RX_CANVAS_INTERNAL_H */
