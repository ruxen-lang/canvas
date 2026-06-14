# Canvas web backend (wasm + CanvasKit)

Status: **in progress** (started 2026-06-14) — first slice.
Depends on: Ruxen wasm heap + host imports (ruxen tier 4.09, landed).

## Goal

Run the canvas+quiver GUI stack in the browser, completing the "compile
everywhere like Flutter" story. The web backend is **not a rewrite of canvas**:
on the wasm target, canvas's `ruxen_canvas_*` FFI symbols become **wasm host
imports**, and a **JS runtime** implements each against **CanvasKit** (Skia
compiled to wasm) attached to an HTML `<canvas>`. quiver (L2, 100% safe,
backend-agnostic) runs unchanged on top.

```
Ruxen wasm module (canvas/quiver draw calls)
   │  calls ruxen_canvas_* (host imports — env.ruxen_canvas_*)
   ▼
JS runtime (web/runtime.mjs)  ── maps each ruxen_canvas_* → CanvasKit
   ▼
CanvasKit (Skia/wasm) → <canvas> pixels
```

Renderer choice: **CanvasKit**. canvas's C shim already wraps Skia (the `sk_*`
C API), so `ruxen_canvas_*` map almost 1:1 to CanvasKit's JS API — highest
fidelity, all features. Cost: a ~7MB CanvasKit payload (fetched + pinned, like
the existing `runtime/fetch_skia.sh`).

## Scope — first slice (this doc's deliverable)

A **minimal vertical proof**, NOT the whole canvas package (compiling all of
canvas at once drags in its full API + stdlib surface — a big-bang risk). The
slice: a small standalone Ruxen module that declares a handful of
`ruxen_canvas_*` host imports + an exported `render()`, driven by the JS/CanvasKit
runtime, drawing one frame — verified headlessly (CanvasKit raster snapshot) and
visually (a browser page). This proves the **wasm → host-import → CanvasKit →
pixels** pipeline and nails the JS-runtime contract.

Out of scope for the slice: the full `ruxen_canvas_*` surface, compiling the real
canvas package, the event pump / `requestAnimationFrame` loop, quiver — all are
follow-ups (see §6).

## The JS-runtime contract

The `RawHost` handle (`ruxen_canvas_host_new(w,h) -> Int`) is an **opaque
integer**: the JS runtime keeps a table `handle → { surface, skcanvas, paint }`.
Every draw import takes the handle first (matching the existing `(self, …)`
shape) and looks it up. i64 args arrive as JS `BigInt`, `Float` as `f64`; the
runtime coerces (`Number(...)`). First-slice symbols:

| Import | CanvasKit mapping |
|--------|-------------------|
| `ruxen_canvas_host_new(w,h) -> handle` | `MakeCanvasSurface(el)` (browser) / `MakeSurface(w,h)` (headless) → store, return handle |
| `ruxen_canvas_begin_frame(h) -> Int` | no-op (return 0); frame start |
| `ruxen_canvas_clear(h, r,g,b,a) -> Int` | `skcanvas.clear(CanvasKit.Color(r,g,b,a))` |
| `ruxen_canvas_draw_rect(h, x,y,w,h, r,g,b,a) -> Int` | `skcanvas.drawRect(LTRBRect, paint(color))` |
| `ruxen_canvas_end_frame(h) -> Int` | `surface.flush()` |

The runtime is target-agnostic about how the surface is created (browser canvas
element vs offscreen raster), so the same `runtime.mjs` core serves both the
browser demo and the headless test.

## First-slice components (`canvas/web/`)

- `web/fetch_canvaskit.sh` — fetch + SHA-pin `canvaskit.wasm` + loader (mirrors
  `runtime/fetch_skia.sh`).
- `web/runtime.mjs` — the reusable `ruxen_canvas_*` → CanvasKit import factory:
  `makeCanvasImports(CanvasKit, makeSurface)` returns the `env` import object.
- `web/demo/draw.rx` — `lib "canvas"`/`lib "env"` host-import decls + `def render()`
  issuing `host_new` → `begin_frame` → `clear` → `draw_rect`×N → `end_frame`.
- `web/index.html` — loads CanvasKit + the compiled `.wasm` + `runtime.mjs`,
  attaches a `<canvas>`, calls `render()` (visual browser demo).
- `web/verify.mjs` — **headless** test: CanvasKit offscreen raster surface in
  Node, instantiate the wasm with the runtime imports, call `render()`,
  `makeImageSnapshot()` → assert key pixels (rect color at its center, cleared
  background). Runnable in CI (Node + the pinned CanvasKit, no browser).

## Build

The slice's `draw.rx` compiles with the installed `ruxen` (wasm target):
`ruxen compile web/demo/draw.rx --target wasm32-unknown-unknown -o draw.wasm`.
Its `ruxen_canvas_*` are undefined → wasm-ld emits them as imports (module from
the `lib` name) → the JS runtime supplies them. No canvas C shim is compiled for
wasm.

## Testing

- **Automated:** `web/verify.mjs` — headless CanvasKit pixel assertions (CI-able).
- **Manual:** `web/index.html` — open in a browser, see the drawn frame.

## §6 Path to full canvas in the browser (after the slice)

1. Grow `ruxen_canvas_*` coverage in `runtime.mjs` (paths, text, images,
   gradients, transforms, clips, layers) — method-by-method, each with a
   `verify.mjs` pixel/call assertion.
2. Compile the **real** canvas package to wasm: skip `runtime/*.c` (native
   Skia/SDL) on the wasm target; ensure canvas's stdlib deps are in the curated
   wasm bootstrap (`RUXEN_WASM_BOOTSTRAP`); window/input via DOM events +
   `requestAnimationFrame` mapped to the `Event` pump + frame pacing.
3. Run **quiver** unchanged on the web canvas backend (its `PaintSurface` calls
   the same `Canvas` methods); port the counter example to the browser.

## Open questions / staged

- CanvasKit load size (~7MB): acceptable for the milestone; a Canvas2D
  lightweight renderer behind the same `runtime.mjs` interface is a possible
  later option (Flutter-style dual renderer).
- Frame pacing: browser is `requestAnimationFrame`-driven (push), canvas's pump
  is pull — reconciled in step 2, not the slice.
- Text shaping/fonts: CanvasKit needs fonts loaded (fetch); first slice uses
  shapes only.
