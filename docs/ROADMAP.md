# canvas — Roadmap

`canvas` is **L1** of the GUI stack. The first milestone is bounded by the
**desktop counter app** (the first vertical slice that proves the whole stack
end-to-end with `quiver`).

## Milestone 0 — scaffold ✅ (current)

- `Ruxen.toml` (library + `[system_libs]` for SDL2/Skia).
- API skeleton: `Window`, `Canvas`, `Color`, `Rect`, `Event`.
- `runtime/skia_shim.c` C-shim placeholder.
- Design docs.

## Milestone 1 — minimal canvas FFI (counter-app slice) ✅ (software backend)

The smallest canvas surface that lets `quiver` render and run a counter,
implemented over the deterministic software raster backend (the exact
`ruxen_canvas_*` ABI the GPU backend will use):

1. `Window.open` → framebuffer-backed window (headless; SDL window + GL/
   Metal surface slot in behind the same ABI later). ✅
2. Canvas FFI: `begin_frame` / `end_frame`, `clear(color)`, `draw_rect`,
   `draw_text` (one embedded 5x7 font) + `measure_text` + `read_pixel`. ✅
3. Event stream: pointer move/down/up, key, resize, close — over a C ring
   buffer with `push_event`/`poll_event`. ✅
4. A pin test per newly-bound method (46 tests across `tests/`). ✅

**Explicitly out of this slice:** mobile/web, the full canvas surface
(paths/images/clips), text shaping/i18n beyond one basic font, packaging.

## Open decisions (resolved in the L1 build spec, not here)

- **Skia build/link** — vendor prebuilt binaries per platform vs build from
  source. (Main engineering risk: C++ + large binary.)
- **SDL2 vs SDL3** — pick the windowing baseline.
- **Surface backend** — GL vs Metal vs Vulkan per platform.

## Later cycles

- Full canvas surface: `draw_path`, `draw_image`, transforms, clips, layers.
- Text / i18n / accessibility (Skia paragraph + HarfBuzz + ICU).
- Platform matrix: macOS/Windows/Linux → Android/iOS → web (WASM + canvas).
