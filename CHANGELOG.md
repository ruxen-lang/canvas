# Changelog

All notable changes to `canvas` are documented here. Format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); this project adheres
to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- **Canvas transforms + clipping** (the foundation for scrolling, nested
  layout, and overflow/masking in L2):
  - `Canvas#save` / `#restore` / `#restore_to` / `#save_count` — the
    matrix+clip state stack.
  - `Canvas#translate` / `#scale` / `#rotate` — coordinate-system transforms.
  - `Canvas#clip_rect` / `#clip_round_rect` — intersect the clip (antialiased;
    rounded masks). Scope with `save`/`restore`.
  - State is **reset at `begin_frame`** so a transform/clip never leaks into the
    next frame. Applied on the Skia backend; a no-op under the software fallback
    (drawing lands untransformed) so `save`/`restore` stays balanced either way.
- **Gradient fills, soft shadows, and sized text** (L1 styling):
  - `Canvas#fill_rect_gradient` / `#fill_round_rect_gradient` (2-stop linear,
    e.g. vertical button backgrounds) and `#fill_circle_radial` (radial:
    centre colour → rim). The shim builds the colour/point arrays so none
    crosses the FFI.
  - `Canvas#draw_round_rect_shadow` — a soft (blurred) rounded rectangle for
    drop shadows (via a Skia blur mask filter).
  - `Canvas#draw_text_sized` / `#measure_text_sized` / `#text_height_sized` —
    text at an explicit pixel size (the shared font is resized per call). Font
    *family* selection is still to come.
  - All Skia-only (clear `Err` when the library is absent); demonstrated in
    `examples/buttons.rx` (gradient + shadowed buttons, radial circle, sized
    heading).

- **Skia vendoring infrastructure** — `canvas` will render with real Skia
  (the prebuilt `libSkiaSharp` that ships behind Avalonia / Uno / .NET MAUI,
  exposing Skia's flat `sk_*` C API). It is **fetched, not committed, and
  dlopen'd, not linked**:
  - `runtime/fetch_skia.sh` — SHA-256-pinned download of
    `SkiaSharp.NativeAssets.Linux` 3.119.4; verifies both the package and the
    extracted `.so`; installs to `$HOME/.cache/ruxen-canvas/` (idempotent).
  - `runtime/skia/skia_capi.h` — the committed minimal C-API surface (opaque
    types, ABI-pinned enums/structs, function-pointer table). The 11 MB `.so`
    is never checked in; only this header is.
  - `docs/SKIA.md` — the vendoring + integration model (Skia rasterizes
    straight into the existing `0xAARRGGBB` `RxHost.pixels` buffer, so the
    SDL presenter is untouched), and the 4-step discipline for growing the
    binding.
- **Skia raster backend live for `clear` + `draw_rect`** — when libSkiaSharp
  is present, both now render through a real Skia `sk_surface_new_raster_direct`
  surface wrapping the host framebuffer (`kBGRA_8888`/premultiplied); when it is
  absent the deterministic software path still runs, so the build never breaks.
  `Canvas#skia_available?` reports library load; `Canvas#skia_active?` reports
  whether *this* canvas is genuinely rendering through Skia. Opaque draws are
  byte-identical across both backends (pin-tested).
- **Skia-native shape primitives** (antialiased; the building blocks for quiver
  widgets) — `Canvas#draw_circle`/`stroke_circle`, `draw_round_rect`/
  `stroke_round_rect` (uniform corner radius), `draw_rrect`/`stroke_rrect`
  (independent per-corner radii — one-side-only rounding, pills, tabs), and
  `draw_line`. Fill and stroke (border) variants throughout. These are
  Skia-only: with no library loaded they return a clear `Err`
  (`requires the Skia backend`), never a silent no-op.
- **Antialiased Skia text** — `draw_text` now renders with a real Skia font
  (system default typeface) when the backend is active, and `measure_text` /
  `text_height` report Skia's true metrics so measurement matches drawing (for
  centering labels). The embedded 5x7 bitmap font remains the software
  fallback. `measure_text` now takes the actual string (real advance width)
  rather than a character count. With text on the Skia path, a frame is now
  rendered entirely by one backend, so there is no premultiplied-vs-straight
  alpha mismatch between shapes and text.

### Changed
- `src/lib.rx` split into per-type files (`color`/`rect`/`rxc`/`raw_host`/
  `canvas`/`event`/`window`.rx); the 5x7 font table moved to
  `runtime/bitmap_font.h`. No behavior change.
- **Live OS windows** (`runtime/sdl_window.c`): `Window.show` puts a real
  window on screen (SDL2 runtime via dlopen — no dev packages, zero
  link-time deps), `present` blits the canvas after `end_frame`, the pump
  feeds real mouse/keyboard/close input into the same `Event` stream tests
  inject into, `hide` tears down, `Window.shown?` queries, `sleep_ms`
  paces render loops. `Window.open` stays headless until `show` — tests
  and CI never pop windows. One window per process for this slice.
- `examples/counter.rx` — interactive clickable counter driving the full
  open/show/poll/draw/present loop.
- **Milestone 1 — the minimal canvas slice**, implemented over a
  deterministic software raster backend in `runtime/skia_shim.c` that
  implements the exact `ruxen_canvas_*` ABI the GPU (Skia/SDL) backend will
  slot in behind:
  - `Color` (`rgb`, `to_argb`) and `Rect` (`right`/`bottom`/`is_empty`/
    half-open `contains`/`intersects`) value types.
  - `Canvas.create` offscreen canvases: framebuffer lifecycle, `width`/
    `height`, and the `read_pixel` pin-test hook (packed `0xAARRGGBB`).
  - Frame discipline: `begin_frame`/`end_frame` pairing enforced; drawing
    outside a frame is an explicit `Err`, never a silent no-op.
  - `clear` (replace, no blending) and `draw_rect` (half-open pixel box,
    source-over blending, surface clipping).
  - `draw_text`/`measure_text` over an embedded classic 5x7 ASCII bitmap
    font: baseline origin, 6px advance, replacement box for non-printables.
  - `Window.open` (headless on this backend): owns its `Canvas` + title +
    size, deterministic teardown on drop.
  - The `Event` stream: `push_event`/`poll_event` over a 256-slot C ring
    buffer — every variant round-trips, FIFO, explicit queue-full error.
  - 46 pin tests across `tests/` (one file per bound capability).
- Initial package scaffold: `Ruxen.toml`, API skeleton, C-shim placeholder,
  and full design docs (`DESIGN`, `ARCHITECTURE`, `FFI`, `ROADMAP`).

### Known deviations (compiler-imposed, tracked for revert)
- `Event` pointer coordinates are `Int` logical pixels (enum float payloads
  currently miscompile); the C ABI already carries doubles.
- `measure_text` crosses the character count (not the string) over the FFI
  (forwarding a borrowed `&String` into an FFI call passes the wrong
  pointer).

### Fixed
- `Color.white`/`black`/`transparent` named constructors restored — the
  zero-arg struct-static and closure-inference compiler bugs they were
  blocked on are fixed upstream (ruxen `18df435`).
