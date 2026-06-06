# canvas

**L1 engine for the Ruxen GUI stack.** Cross-platform windowing, input, and a
Skia-backed 2D canvas, exposed *upward only* to the L2 framework
([`quiver`](../quiver)).

`canvas` is the **only** layer of the GUI stack permitted to use `unsafe` /
C FFI / platform-specific code. Everything above it (`quiver`) is 100% safe,
platform-agnostic Ruxen.

```
L0  Ruxen language        (unchanged — general purpose)
L1  canvas   ← you are here   windowing + input + 2D canvas via C FFI   [unsafe/FFI]
L2  quiver                    signals + block DSL + layout + widgets    [100% safe]
L3  your app
```

## What it binds

| C library | Role |
|---|---|
| **SDL2 / SDL3** | windowing, input, event pump, GL/Metal context (desktop + mobile) |
| **Skia** | the 2D canvas — Flutter's renderer: GPU rendering, world-class text shaping/i18n via bundled HarfBuzz/FreeType, pixel-perfect cross-platform output |

The full Skia C library is **vendored**; only a **curated subset of methods**
is exposed via FFI, grown method-by-method. Nothing is missing from the
vendored library — only the *binding surface* is incremental. See
[`docs/FFI.md`](docs/FFI.md).

## Surface (exposed upward)

- `Window` / surface — created over SDL's context, backed by a Skia surface.
- An **event stream** — pointer/keyboard input, resize, lifecycle (`Event`).
- A `Canvas` — `begin_frame`/`end_frame`, `clear`, `draw_rect`, `draw_text`,
  … (the growing subset).

## Status

**Milestone 1 implemented** over a deterministic **software raster backend**
(`runtime/skia_shim.c`): `Window.open`, the `Event` stream, and a `Canvas`
with `begin_frame`/`end_frame`, `clear`, `draw_rect` (src-over blending +
clipping), `draw_text`/`measure_text` (embedded 5x7 bitmap font), and a
`read_pixel` pin-test hook — 46 pin tests, one file per bound capability.

The software backend implements the exact `ruxen_canvas_*` ABI the GPU
backend will use, so vendored Skia + SDL slot in behind the same signatures
without touching the Ruxen side (build/link decision tracked in
[`docs/ROADMAP.md`](docs/ROADMAP.md)).

## Docs

- [`docs/DESIGN.md`](docs/DESIGN.md) — why this layer exists and its boundary.
- [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) — the L0–L3 model and the L1 invariant.
- [`docs/FFI.md`](docs/FFI.md) — the incremental-FFI discipline (the 4-step process).
- [`docs/ROADMAP.md`](docs/ROADMAP.md) — milestones and open build/link decisions.

## License

Dual-licensed under either of [MIT](LICENSE-MIT) or
[Apache-2.0](LICENSE-APACHE) at your option.
