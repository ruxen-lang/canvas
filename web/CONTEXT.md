# web — canvas web backend (wasm + CanvasKit)

Runs canvas in the browser by implementing its `ruxen_canvas_*` FFI as wasm host
imports against CanvasKit (Skia/wasm). Design: `../docs/WEB_BACKEND.md`. First
slice (proven): a Ruxen wasm module's draw calls render pixel-exact via CanvasKit.

## What lives here
- `runtime.mjs` — the reusable core: `makeCanvasImports(CanvasKit, makeSurface)`
  returns the `env` import object mapping each `ruxen_canvas_*` → CanvasKit, plus
  the `hosts` handle table. Backend-agnostic about surface creation (browser vs
  offscreen).
- `demo/draw.rx` — standalone first-slice module: `lib "env"` host-import decls +
  exported `render()` (clear + a rect). Compiled to `demo/draw.wasm` (gitignored).
- `verify.mjs` — headless test: CanvasKit offscreen raster surface in Node, runs
  `render()`, reads back pixels, asserts the frame. `npm run verify`.
- `index.html` — browser demo (serve over http: `npm run serve`, open :8080).
- `package.json` — pins `canvaskit-wasm`; `node_modules/` gitignored.

## Public surface
`makeCanvasImports(CanvasKit, makeSurface) -> { env, hosts }`. The `env` object is
passed as the wasm import object; `hosts` (handle → {surface,canvas,paint}) lets a
host snapshot/present a surface after `render()` returns its handle.

## Depends on
A wasm build of the Ruxen module (built by the ruxen toolchain WITH wasm heap +
host imports — ruxen tier 4.09); `canvaskit-wasm` (npm); Node (test) / a browser.

## Invariants & gotchas
- ABI: `ruxen_canvas_*` return Ruxen `Int` → wasm i64 → JS BigInt, so imports
  return `BigInt` (`0n`, or `BigInt(handle)`). `Int` args arrive as BigInt,
  `Float` as number — `N()` normalizes. The `RawHost` handle is an opaque int
  indexing `hosts`.
- Symbol names mirror the real canvas C shim (`runtime/skia_shim.c`) so this
  runtime carries straight over when the full canvas package compiles to wasm.
- Growing coverage = add the `ruxen_canvas_*` case to `runtime.mjs` + a `verify.mjs`
  pixel assertion, method-by-method (mirrors the native FFI 4-step discipline).
