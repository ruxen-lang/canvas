# canvas — Roadmap

`canvas` is **L1** of the GUI stack. The first milestone is bounded by the
**desktop counter app** (the first vertical slice that proves the whole stack
end-to-end with `quiver`).

## Milestone 0 — scaffold ✅ (current)

- `Ruxen.toml` (library + `[system_libs]` for SDL2/Skia).
- API skeleton: `Window`, `Canvas`, `Color`, `Rect`, `Event`.
- `runtime/skia_shim.c` C-shim placeholder.
- Design docs.

## Milestone 1 — minimal canvas FFI (counter-app slice)

Bind the smallest Skia/SDL surface that lets `quiver` render and run a counter:

1. SDL window → Skia surface on its GL/Metal context (`Window.open`).
2. Canvas FFI: `begin_frame` / `end_frame`, `clear(color)`, `draw_rect`,
   `draw_text` (one font).
3. Event stream: pointer down/up, resize, close.
4. A pin test per newly-bound Skia method.

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
