# Changelog

All notable changes to `canvas` are documented here. Format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); this project adheres
to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
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
- `Color.white`/`black`/`transparent` named constructors deferred (zero-arg
  `def self.` methods on structs currently miscompile).
- `measure_text` crosses the character count (not the string) over the FFI.
