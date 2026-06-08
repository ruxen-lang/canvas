# Changelog

All notable changes to `canvas` are documented here. Format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); this project adheres
to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- **GPU surface backend — Metal (Apple), with HEADLESS GPU pixel verification.**
  An additive rung behind the same `rx_gpu_context` seam / backend ladder /
  probes, with the **unchanged `ruxen_canvas_*` ABI** (`docs/GPU.md`):
  - **Offscreen, no-window GPU rendering.** `MTLCreateSystemDefaultDevice`
    returns the system GPU with no display, so an offscreen Metal-backed
    `SkSurface` (`gr_direct_context_make_metal` + a BGRA
    `sk_surface_new_render_target`, Skia owning the `MTLTexture` — no
    `CAMetalLayer`) renders on the GPU and `sk_surface_read_pixels` copies the
    result back into the framebuffer. The **first GPU backend pixel-verified
    locally**: `Canvas#enable_gpu_offscreen` → draw → `end_frame` (flush+submit,
    then readback) → `read_pixel` sees real GPU output.
  - **Device/queue via dlopen, no link-time dep.** Metal device + command queue
    come from `Metal.framework` + the Obj-C runtime
    (`MTLCreateSystemDefaultDevice`, `[device newCommandQueue]` through
    `objc_msgSend`/`sel_registerName`), a process-wide singleton — same
    fetch/dlopen discipline as Skia/SDL.
  - **Seam/ladder intact.** `rx_host_canvas` already routes `is_gpu` hosts to the
    GPU canvas; Metal reuses it, so every `ruxen_canvas_*` draw op is unchanged.
    New `gpu_backend_kind` slot (`Canvas#gpu_backend_kind`: 0 none / 1 GL /
    2 Metal) + `gpu_metal_available?`. Any failure falls back cleanly to raster
    (never half-GPU, never wrong pixels). Teardown: surface → `GrDirectContext`
    (`gr_recording_context_unref`); device/queue not per-host.
  - **Pixel proof is a standalone example, not an in-harness draw.** Apple
    forbids Metal across `fork()`-without-`exec()`; the test harness forks per
    case and Metal's shader-compiler XPC service is unreachable post-fork (a
    shader-compiling draw dies in the forked child). So
    `examples/metal_offscreen_verify.c` is the committed, runnable proof
    (`cc -O2 -o m examples/metal_offscreen_verify.c && ./m` → `PASS`, blue rect
    read back byte-exact `0xFF0080FF`); the in-harness `tests/gpu_backend.rx`
    pins capability + clean fallback only. Full windowed Metal (`CAMetalLayer`
    via `SDL_Metal_*`) stays deferred (no display on this host).
- **Skia is now ACTIVE on macOS (real local pixel verification).**
  `runtime/fetch_skia.sh` is host-aware: on macOS it fetches + SHA-256-pins the
  **`SkiaSharp.NativeAssets.macOS`** package and installs its **universal**
  `runtimes/osx/native/libSkiaSharp.dylib` (arm64 + x86_64) into the same
  `$HOME/.cache/ruxen-canvas/` cache; Linux keeps fetching the Linux `.so`
  (no CI change). The shim's loader (`rx_skia_dlopen`) probes both
  `libSkiaSharp.{dylib,so}` basenames, native-name-first per platform. Result:
  `skia_available?` / `skia_active?` report **true** on this macOS host, so the
  entire Skia-only surface (`draw_path`, gradients, soft shadows, circles /
  rounded-rects, sized text, configurable font family, image decode/scale,
  transforms / clips, offscreen layers + group opacity) now runs its **real
  Skia branch** locally and is pixel-verified — not only the software-fallback /
  `Err` branch. The software fallback remains intact and is still exercised when
  the binary is absent (Skia is not mandatory). The macOS dylib links
  `Metal.framework` (`SK_METAL=1`), which unblocks the Metal backend next
  (`docs/GPU.md`).
- **GPU surface backend (Ganesh GL)** — the top rung of the backend-selection
  ladder, behind the **unchanged `ruxen_canvas_*` ABI** (`docs/GPU.md`,
  GL-first; Metal/Vulkan deferred):
  - **GL context seam** in `runtime/sdl_window.c`: the SDL GL entry points
    (`SDL_GL_CreateContext` / `_MakeCurrent` / `_GetProcAddress` /
    `_SwapWindow` / `_GetDrawableSize`) resolved in the same dlopen tier as the
    rest of SDL, exposed as `ruxen_canvas_window_create_gl` / `_gl_present` /
    `_gl_get_proc` / `_gl_drawable_size` / `_is_gl`. All fail cleanly and
    bounded on a headless / no-SDL / no-GL host; the raster path never depends
    on them.
  - **`GrDirectContext` + GPU-backed `SkSurface`** over that GL context in
    `runtime/skia_shim.c` via the Ganesh C symbols the prebuilt `libSkiaSharp`
    already exports — `gr_glinterface_assemble_gl_interface` /
    `gr_glinterface_create_native_interface`, `gr_direct_context_make_gl`,
    `gr_backendrendertarget_new_gl`, `sk_surface_new_backend_render_target`
    (an **OPTIONAL** loader tier: a missing symbol sets `gpu_gl_ok = 0` and
    disables only the GPU rung — the raster backend is untouched).
  - **Draw routing is automatic and ABI-stable:** every `ruxen_canvas_*` draw
    op funnels through `rx_host_canvas`, which returns the GPU canvas when the
    host is in GPU mode — so no drawing signature moves. `end_frame` flushes +
    submits the `GrDirectContext`; `Window#present` swaps the GL back buffer
    when `gpu_active?`, else blits the raster framebuffer.
  - **Capability probes** `Canvas#gpu_available?` (process can reach Ganesh GL)
    / `#gpu_active?` (this canvas has a live GPU surface), mirroring
    `skia_available?` / `skia_active?`. `Window#show_gpu` attempts the GPU
    backend (GL window + context + GPU surface) and **falls back cleanly to the
    raster show path** on any failure — a GPU op that can't run falls back,
    never produces silently-wrong pixels.
  - **CPU raster fallback preserved** as the deterministic test oracle; GPU is
    selected at runtime, never a replacement. Teardown order is explicit (GPU
    surface → backend-render-target → `GrDirectContext` → GL interface, before
    the GL context + window are destroyed).
  - Pin: `tests/gpu_backend.rx` asserts the **capability + clean-fallback**
    contract (probes are total and safe; an offscreen canvas is never
    GPU-active; raster readback stays byte-exact when GPU is unavailable;
    `show_gpu` either brings a GPU/raster window up or stays headless, drawing
    works on every branch). **NOTE:** full GPU **pixel** verification is
    deferred to a GL-capable desktop — this host and CI are headless with no
    usable GL surface, the same posture as the Skia-on-Linux-CI note in
    `docs/SKIA.md`.
- **Configurable font family** — pick a typeface by family name (widgets can
  choose a font, not just a size):
  - `Canvas#draw_text_font(text, x, y, size, family, color)`,
    `#measure_text_font(text, size, family)`, `#text_height_font(size, family)`
    — the `*_sized` text ops plus a family name.
  - A missing/uninstalled `family` **gracefully falls back to the default
    typeface** (an absent font never breaks rendering — not an error). Resolved
    families are cached process-wide (one `sk_font_t` per family, resized in
    place like the default font; freed never, the same singleton model).
  - `draw_text_font` is Skia-only (a clear `Err` when the backend is absent —
    a family is meaningless for the 5x7 bitmap face); `measure_text_font` /
    `text_height_font` always return a usable number, falling back to the
    bitmap metrics when Skia is absent (`runtime/skia_shim.c`
    `ruxen_canvas_*_font`, `sk_typeface_create_from_name` / `sk_fontstyle_*` /
    `sk_typeface_unref`).
  - Pin tests: `tests/canvas_fonts.rx` (two distinct families measure a string
    differently; an absent family measures like the default; positive line
    height; ink drawn / clean `Err` when Skia is inactive).
- **Offscreen layers** — `Canvas#save_layer` / `#save_layer_alpha`, composited
  down by the existing `#restore` (group opacity + blended overlays: fade
  transitions, translucent panels, scrolling lists):
  - `save_layer` pushes a whole-canvas offscreen layer onto the same save stack
    as `save`; `save_layer_alpha(alpha)` (0..255) applies a uniform group
    opacity to the layer's content. Both return the layer's save count (for
    `restore_to`) on `Ok`; `restore` composites the layer down.
  - Strictly Skia-only — a clear `Err` when the backend is absent (unlike the
    matrix/clip save ops, a layer can't no-op in software without producing
    wrong pixels). Over the flat-Int ABI the count (>= 1) is the success value
    and a negative `-RXC_ERR_*` is the failure channel
    (`runtime/skia_shim.c` `ruxen_canvas_save_layer` / `_save_layer_alpha`,
    `sk_canvas_save_layer` / `sk_canvas_save_layer_alpha`).
  - Pin tests: `tests/canvas_layers.rx` (a plain layer round-trips its content;
    a 50%-alpha layer dims opaque red to a mid-range red, by pixel readback;
    clean `Err` when Skia is inactive).
- **Arbitrary paths** — `draw_path` / `stroke_path` over a `Path2D` builder
  (the highest-value missing L1 primitive: icons, custom containers, any
  non-rect/rrect shape):
  - `Path2D.create` allocates a Skia path (owns it, freed deterministically on
    drop); builder ops `move_to` / `line_to` / `quad_to` / `cubic_to` /
    `arc_to` (SVG-style elliptical arc) / `close`, plus `even_odd` / `winding`
    fill-rule selection.
  - `Canvas#draw_path(path, color)` fills and `#stroke_path(path, width, color)`
    strokes the path (antialiased). Skia backend only — a clear `Err` when the
    library is absent, never a silent no-op.
  - Shim builds any coordinate arrays internally; only the int64 path handle and
    scalar device-pixel coords cross the FFI (`runtime/skia_shim.c`
    `ruxen_canvas_path_*` / `_draw_path`, `sk_path_*` / `sk_canvas_draw_path`).
  - Pin tests: `tests/canvas_path.rx` (filled triangle + stroked square outline
    by offscreen pixel readback; clean `Err` when Skia is inactive).
- **GPU surface backend ADR** — `docs/GPU.md` records the GL-vs-Vulkan-vs-Metal
  decision (Metal on Apple, OpenGL on Linux/Windows behind one context seam,
  Vulkan deferred), grounded in the fetch+dlopen model and Ganesh `gr_*` C API,
  and how it preserves the `ruxen_canvas_*` ABI + the CPU-raster fallback.
  Design only — no GPU implementation.
- **Images** — decode and draw PNG / JPEG / WebP:
  - `Image.load(path)` decodes a file into an `Image` (owns its pixels, freed
    deterministically on drop); `Image#width` / `#height`. `Err` when the file
    is missing/undecodable or the Skia backend is unavailable.
  - `Canvas#draw_image` (natural size), `#draw_image_rect` (scaled to a rect,
    linear sampling), and `#draw_image_rect_src` (a sub-region → rect, for
    sprite sheets / atlases). Skia backend only.
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
- **`save_layer_alpha` group opacity now works on real Skia.** This Skia build
  does not export `sk_canvas_save_layer_alpha` (the convenience wrapper was
  removed upstream; only `sk_canvas_save_layer` / `_rec` exist), so the call was
  bound to a non-existent symbol and returned `Err` whenever Skia was actually
  active — masked until now because the binary was never present locally.
  Reimplemented via `sk_canvas_save_layer` with an alpha-carrying paint
  (`SkCanvas` applies the layer paint's alpha as whole-layer opacity); the
  group-opacity readback pin (`tests/canvas_layers.rx`) now passes through real
  Skia.
- **GPU-GL capability probe could never be true.** The GL rung required a
  `gr_direct_context_unref` symbol that does not exist in this Skia C API (a
  `GrDirectContext` *is-a* `GrRecordingContext`; the real release is
  `gr_recording_context_unref`). Rebound to `gr_recording_context_unref`
  (upcasting the context), so `gpu_available?` now reports true where the Ganesh
  GL symbols resolve. Both bugs were surfaced by bringing Skia live on macOS and
  verified against the fetched binary with `nm` + the SkiaSharp C header.
- `Color.white`/`black`/`transparent` named constructors restored — the
  zero-arg struct-static and closure-inference compiler bugs they were
  blocked on are fixed upstream (ruxen `18df435`).
