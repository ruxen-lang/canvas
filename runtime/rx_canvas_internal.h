/*
 * rx_canvas_internal.h — shared internals of the canvas C shim.
 *
 * Two translation units include this:
 *   skia_shim.c   — the deterministic software raster backend (no platform
 *                   code; presents/pumps through the hooks below).
 *   sdl_window.c  — the live-window platform layer (SDL3 via dlopen); it
 *                   attaches to a host by filling in the hooks.
 *
 * Everything here is internal to the shim pair — the Ruxen side only ever
 * sees the flat ruxen_canvas_* functions (docs/FFI.md).
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
 * pending-payload accessors — must come from one thread. */
typedef struct RxHost {
    int32_t   width;
    int32_t   height;
    uint32_t *pixels;      /* width*height, 0xAARRGGBB, non-premultiplied */
    int32_t   in_frame;    /* begin_frame/end_frame discipline flag */

    /* event ring buffer (filled by the platform pump or by tests) */
    RxEvent   events[RXC_EVENT_CAP];
    int32_t   ev_head;
    int32_t   ev_count;
    RxEvent   pending;     /* the event most recently popped by poll */

    /* platform-window attachment (all 0/NULL on the headless path).
     * skia_shim.c calls the hooks; sdl_window.c fills them in. */
    int32_t   windowed;
    void     *plat_window;
    void     *plat_renderer;
    void     *plat_texture;
    int       (*present)(struct RxHost *);  /* end_frame -> screen; 0 = ok */
    void      (*pump)(struct RxHost *);     /* drain platform events into the ring */
    void      (*close)(struct RxHost *);    /* tear the platform window down */
} RxHost;

/* the ring-buffer feeder, defined in skia_shim.c, used by the pump */
int64_t ruxen_canvas_push_event(int64_t self, int64_t kind, double a, double b);

#endif /* RX_CANVAS_INTERNAL_H */
