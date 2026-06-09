# Multi-window

`canvas` supports **N independent on-screen windows per process**. This document
records the architecture, what is verified vs. deferred, and the one language gap
it surfaced.

## The problem

`canvas` was one-window-per-process. The framebuffer, Skia/GPU surface, and event
ring were already **per-`RxHost`** (each `Window` → its own `Canvas` → its own
`RxHost`, allocated by `ruxen_canvas_host_new`). The single-window assumption lived
entirely in `runtime/sdl_window.c`, as a set of process-global statics
(`s_win`/`s_ren`/`s_tex`/`s_glctx`/`s_mtl_*`/`s_owner`/`s_scale`/…) keyed implicitly
by one `s_owner` host pointer. Every `ruxen_canvas_window_*` entry point already
took `self` (the host) and gated on `s_owner == self`.

## The design

The single set of `s_*` window globals became a **fixed-size table of `RxWin`
slots**, one per live window, keyed by the owning `RxHost` pointer:

```c
#define RX_MAX_WINDOWS 16
typedef struct {
    void *owner;            /* the RxHost; NULL = free slot */
    void *win, *ren, *tex;  /* raster path */
    uint32_t win_id;        /* SDL_GetWindowID — the event-demux key */
    int tex_w, tex_h, scale, out_w, out_h;
    void *glctx;            /* GL path */
    void *mtl_view, *mtl_layer, *mtl_device, *mtl_queue, *mtl_drawable;  /* Metal path */
} RxWin;
static RxWin s_wins[RX_MAX_WINDOWS];
```

- **No heap, no language feature.** A fixed array (capacity 16) — bounded, simple,
  no allocator, nothing the Ruxen v1 compiler can't already express. `RxWin` is
  pure C internal to the shim; the Ruxen side is unchanged (windows still cross the
  ABI as the host integer handle, never a raw pointer).
- `rx_win_for(owner)` finds a host's slot, `rx_win_alloc(owner)` claims a free one,
  `rx_win_by_id(id)` finds the slot for an SDL `windowID`, `rx_win_teardown(slot)`
  frees one slot. Every entry point resolves its slot from `self` at the top.
- **The SDL library handle (`s_lib`) and its dlsym'd function pointers stay
  process-global** — SDL is loaded and `SDL_Init`'d once. Only the per-window OS
  objects move into a slot.

### Backward compatibility

A single-window app hits exactly one slot. The entry-point signatures are
**unchanged** (ABI stable). All 136 pre-existing tests pass untouched; the only
semantic generalizations are the two **no-arg** legacy functions:

- `ruxen_canvas_window_is_shown()` → "at least one window is shown".
- `ruxen_canvas_window_destroy()` → tears down **all** windows (the safe superset
  of the old "destroy the window" for single-window callers).

### Event demux (the one genuinely new piece)

`SDL_PollEvent` is a **single process-wide queue** shared by all windows; each
window-associated event carries the originating window's SDL `windowID` (a `Uint32`
at byte offset 8 of every such event struct — mouse motion/button/wheel, keyboard,
text-input, window events). `ruxen_canvas_window_pump(self)` now:

1. drains the whole SDL queue once,
2. reads each event's `windowID` and routes the translated `RxEvent` to the owning
   slot's host ring (`rx_win_by_id`),
3. returns the count of events delivered to `self`'s window (so a single-window app
   sees the same number it always did).

`SDL_QUIT` has no `windowID` (it is app-global); it delivers `CloseRequested` to
**every** live window so each can tear down. Because the queue is shared, whichever
window pumps first in a frame drains everything — other windows' events are routed
to their rings and surface when those windows poll. No window's events are dropped.

### Per-window teardown

`ruxen_canvas_window_destroy_for(self)` (new flat binding, host param) tears down
only that host's window. `Window#hide` routes through it, so hiding one window
leaves the others up. `ruxen_canvas_window_note_host_dropped(self)` (called from
`host_drop`) frees only the dropping host's slot — preserving the GPU teardown
order (Skia GPU surface + gr context released by `host_drop` before the GL/Metal
context is deleted, then the window).

## Verified vs. deferred

- **Verified, headless (`tests/multiwindow.rx`):** N windows each with an
  independent canvas/framebuffer; interleaved frames don't bleed; per-window event
  rings are isolated; interleaved pushes keep each ring's FIFO order; deterministic
  teardown of many concurrent windows. (The event-ring isolation is the headless
  half of the demux contract — the per-host ring is what the demux routes into.)
- **Verified, live display (`examples/multiwindow_verify.c`, `PASS`):** two
  independent on-screen SDL windows (ids 1 & 2) presenting red and blue
  independently via the raster path, with the **exact** windowID-demux routing the
  pump uses. Not harness-verifiable (the test harness forks per case and runs
  headless).
- **Deferred:** concurrent **live GL/Metal** multi-window. The architecture
  supports it — each `RxWin` slot carries its own `glctx` / `mtl_*` state, and the
  selection ladder (`Window#show_gpu_scaled`) allocates a slot per host — but it is
  pixel-verified only for the raster path on this host. See the language gap below.

## Language gap (Q-candidate for the ruxen ledger)

`ruxen_canvas_window_gl_get_proc(name)` is the GL function loader Skia's
`gr_glinterface_assemble_*` calls back into. It takes **only `name`, no `self`**,
because SDL's GL proc resolution is per-process against the *currently-current* GL
context. With multiple concurrent GL windows, only one context is current at a time,
so this loader is a process-global current-context seam — fine for building each
context's `GrGLInterface` once at create (the current behavior, preserved), but it
means truly concurrent multi-GL-context rendering would need an explicit
make-current per window per frame. This is a GPU-backend refinement, not a blocker:
on Apple the real path is Metal, and the raster multi-window path is complete. File
as a `Q##` candidate if/when concurrent live GL multi-window is scheduled.
