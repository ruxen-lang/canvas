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

## Milestone 2 — real Skia raster backend ✅ (in progress on `feat/skia-backend`)

`canvas` now renders with actual Skia, behind the same `ruxen_canvas_*` ABI,
with the software raster kept as a fallback (`docs/SKIA.md`):

1. **Skia brought in by fetch + dlopen, not vendor + link** — `fetch_skia.sh`
   SHA-pins `libSkiaSharp` (the prebuilt Skia behind Avalonia/Uno/MAUI, flat
   `sk_*` C API); the shim `dlopen`s it like SDL2. ✅
2. Skia draws straight into the existing `0xAARRGGBB` framebuffer
   (`sk_surface_new_raster_direct`, `kBGRA_8888`), so the SDL presenter is
   untouched. `clear`, `draw_rect` routed through Skia. ✅
3. Skia-native primitives: `draw_circle`/`stroke_circle`, `draw_round_rect`/
   `stroke_round_rect`, `draw_rrect`/`stroke_rrect` (per-corner radii),
   `draw_line` — the building blocks for quiver widgets. ✅
4. Antialiased Skia text (`draw_text`/`measure_text`/`text_height`). ✅
5. `skia_available?` / `skia_active?` capability probes; Skia-only ops return a
   clear `Err` when the library is absent. ✅

**Next in this cycle:** gradients (`sk_shader`) + drop-shadows
(`sk_imagefilter`/`sk_maskfilter`) — APIs confirmed present in the lib;
configurable font size/family; then the GPU (Ganesh GL) surface.

## Resolved decisions

- **Skia build/link** — ✅ **fetch prebuilt `libSkiaSharp` + dlopen** (no link,
  no dev package, no source build). See `docs/SKIA.md`.
- **SDL2 vs SDL3** — SDL2 runtime via dlopen for the live-window presenter.

## Open decisions

- **GPU surface backend** — GL vs Vulkan vs Metal per platform (the `gr_`
  Ganesh C API is present in the fetched lib for when we move off CPU raster).

## Later cycles

- Full canvas surface: `draw_path`, `draw_image`, transforms, clips, layers.
- Text i18n / accessibility (HarfBuzz shaping + paragraph layout via a separate
  `HarfBuzzSharp` native lib + ICU).
- Platform matrix: macOS/Windows/Linux → Android/iOS → web (WASM + canvas).
