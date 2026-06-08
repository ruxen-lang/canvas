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

- _(none currently — the GPU surface backend is decided in `docs/GPU.md`:
  Metal on Apple, OpenGL on Linux/Windows behind one context seam, Vulkan
  deferred. Implementation is a later cycle.)_

## Later cycles

- Full canvas surface: `draw_path`, `draw_image`, transforms, clips, layers.
- Text i18n / accessibility (HarfBuzz shaping + paragraph layout via a separate
  `HarfBuzzSharp` native lib + ICU).
- Platform matrix: macOS/Windows/Linux → Android/iOS → web (WASM + canvas).

## Remaining — tracked checklist

Audited 2026-06-08 against `src/**`, `runtime/**`, and CHANGELOG `[Unreleased]`.
Ordered by what unblocks `quiver`'s widget library soonest. `→ ruxen #X` marks a
cross-repo dependency on a language fix (see `../ruxen/docs/TASKS.md`).

### Unblocked now (current language is sufficient — additive FFI, 4-step discipline)

- [x] **`draw_path` — arbitrary Skia paths** (`sk_path_*`: moveTo/lineTo/quadTo/
      cubicTo/arcTo/close, fill + stroke, fill-rule). The single highest-value
      missing primitive: it's what L2 needs for icons, custom containers, and any
      non-rect/rrect shape. Done: `Path2D` builder + `Canvas#draw_path` /
      `#stroke_path`; pin tests in `tests/canvas_path.rx`.
- [x] **Layers — `save_layer` / `save_layer_alpha`** (`sk_canvas_save_layer` /
      `sk_canvas_save_layer_alpha`): offscreen compositing for group opacity +
      blended overlays (fade transitions, translucent panels). Done:
      `Canvas#save_layer` / `#save_layer_alpha` return the layer save count and
      pair with the existing `#restore`; pin tests in `tests/canvas_layers.rx`.
- [ ] **Configurable font *family*** — `draw_text_sized` resizes the shared
      default typeface today; add family selection (`sk_typeface_*` /
      `sk_fontmgr_*`) so widgets can pick a font, not just a size.
- [ ] **Multi-window** — one window per process today; lift to N windows for
      real apps (engine-level, not blocking the widget library).

### Needs an architecture decision (design doc first, then implement)

- [ ] **GPU surface backend (Ganesh)** — CPU raster only today. **Decision
      recorded in `docs/GPU.md`** (Metal on Apple, OpenGL on Linux/Windows
      behind one context seam, Vulkan deferred; `gr_*` C API already in the
      fetched lib). The ADR is done; the *binding/implementation* is a later
      cycle and slots behind the unchanged `ruxen_canvas_*` ABI.

### Later cycles (large, sequenced)

- [ ] **Text i18n / shaping** — HarfBuzz + ICU + paragraph layout (separate
      `HarfBuzzSharp` native lib). Gates real international text in L2.
- [ ] **Accessibility** — platform a11y trees.
- [ ] **Platform matrix** — Windows → Android/iOS → web (WASM + canvas).

### Compiler-imposed deviations to revert (blocked on ruxen)

- [ ] `Event` pointer coords are `Int` logical pixels — enum **float payloads
      miscompile**; the C ABI already carries doubles. Revert to float once
      fixed. **→ ruxen** (file a `Q##` if not already tracked).
- [ ] `measure_text` forwards a **char count**, not the string, over the FFI —
      a borrowed `&String` into an FFI call passes the wrong pointer. Revert to
      real advance width once fixed. **→ ruxen.**
