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
3. Event stream: pointer move/down/up, scroll, resize, close, **control keys**
   (`KeyDown`) + **printable text** (`TextInput`, via `SDL_TEXTINPUT`) — over a C
   ring buffer with `push_event`/`poll_event`. Windowed pointer coords are mapped
   window-points → design coords by the show factor; windows are resizable and
   emit `Event.Resize` (GPU surface re-created at the new backing size). ✅
4. A pin test per newly-bound method (46 tests across `tests/`). ✅

**Explicitly out of this slice:** mobile/web, the full canvas surface
(paths/images/clips), text shaping/i18n beyond one basic font, packaging.

## Milestone 2 — real Skia raster backend ✅ (in progress on `feat/skia-backend`)

`canvas` now renders with actual Skia, behind the same `ruxen_canvas_*` ABI,
with the software raster kept as a fallback (`docs/SKIA.md`):

1. **Skia brought in by fetch + dlopen, not vendor + link** — `fetch_skia.sh`
   SHA-pins `libSkiaSharp` (the prebuilt Skia behind Avalonia/Uno/MAUI, flat
   `sk_*` C API); the shim `dlopen`s it like SDL2. ✅ **Host-aware:** Linux
   `.so` + macOS universal `.dylib` (`SkiaSharp.NativeAssets.macOS`), both
   SHA-pinned. Skia is now **active locally on macOS arm64**, so the full
   Skia-only surface is pixel-verified on-host (not just the software branch). ✅
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

## Prod-parity Phase 1 — engine essentials (E1) + desktop services (E2)

The tier that unblocks quiver's animation (F4) and text editing (F3). Each item
is the usual additive 4-step FFI binding + a pin; live-window/GPU paths pin
headless (capability + fallback) and pixel-verify via a standalone
`examples/*_verify.c` where a real window/GPU is required.

### E1 — engine essentials

- [x] **Frame pacing / vsync seam** (ADR: `docs/decisions/frame-pacing.md`). A
      monotonic-ns timebase (`Window#ticks_ns` / `#ticks_ms`,
      `clock_gettime(CLOCK_MONOTONIC)`), a paced-present wait to an ABSOLUTE
      target tick (`Window#wait_frame` — fills an early frame's slack, never adds
      latency to a late one; no-op-by-design where the Metal/GL present already
      blocks on vsync), and a refresh-rate hint (`Window.refresh_rate` via
      `SDL_GetDesktopDisplayMode`, clean `Err` headless — never a bogus `Ok(0)`).
      NO callback/timer system — just the timebase + paced present, so L2's
      animation ticker has a real timebase. Pins: `tests/frame_pacing.rx`
      (monotonicity, both wait_frame branches, ms=ns/1e6, refresh-rate Err
      contract).
- [x] **Transforms — `skew` + `concat`** (full 2D matrix), composing with the
      existing `save`/`restore`/`translate`/`scale`/`rotate`. `Canvas#skew(sx,sy)`
      (`sk_canvas_skew`) and `Canvas#concat(a,b,c,d,e,f)` (the 6-value top-two-rows
      affine; `sk_canvas_concat`). **Binding gotcha pinned:** this `libSkiaSharp`'s
      `sk_canvas_concat` takes a 4x4 SkM44 in COLUMN-MAJOR order (16 floats), NOT a
      3x3 `sk_matrix_t` — empirically determined via `sk_canvas_get_matrix`
      round-trip (a 3x3 silently no-ops). Pixel-pinned in `tests/canvas_transforms.rx`
      (skew shears a rect into a parallelogram at predicted pixels; concat applies a
      2x-scale + (6,6)-translate matrix and the rect lands in the mapped box).
- [x] **Blend modes** — `Canvas#set_blend_mode(mode)`, a small stable int enum
      (0 src-over / 1 clear / 2 src / 3 multiply / 4 screen) mapped to SkBlendMode
      in the shim (`sk_paint_set_blendmode`). It is host PAINT STATE (Skia paints
      carry the mode), applied to subsequent shape-fill / line / path draws and
      RESET to source-over each `begin_frame` so it never leaks across frames.
      `Canvas.blend_*` constants name the ints. Out-of-range is `Err`. Pixel-pinned
      in `tests/canvas_blend.rx` (multiply red*green→black; screen→yellow;
      per-frame reset; validation).
- [x] **Blur image filter + drop-shadow generalization** — `Canvas#save_layer_blur(sigma)`
      pushes an offscreen layer whose paint carries a Gaussian blur image filter
      (`sk_imagefilter_new_blur` + `sk_paint_set_imagefilter`), so EVERYTHING drawn
      into the layer is blurred when `restore` composites it down. The general blur
      primitive — it blurs arbitrary content (shapes, text, paths), generalizing
      the rrect-only `draw_round_rect_shadow` (frosted panels, blurred backdrops,
      soft shadows of any shape). `sigma <= 0` is `Err`; Skia-only. Pixel-pinned in
      `tests/canvas_blur.rx` (ink spreads past a hard edge under blur; a control
      without the layer keeps the same pixel hard; sigma validation).
- [ ] **Dash path effect** — **BLOCKED on the fetched binary** (see Phase-1.5).

### E2 — desktop services core

- [x] **Clipboard** — `Window.clipboard_text -> Result[String, String]` /
      `Window.set_clipboard_text(s)` via `SDL_GetClipboardText` /
      `SDL_SetClipboardText`. **Works headless** — SDL's clipboard round-trips a
      set->get under the dummy video driver with no live window (verified), so the
      harness pins a REAL round-trip, not just the Err contract. C→Ruxen String
      return uses `ruxen_string_from` (a Ruxen String IS a malloc'd `char*`), so the
      returned text is Ruxen-owned (the SDL copy is `SDL_free`'d); under the forked
      harness the dummy driver is forced for fork-safety (the real Cocoa pasteboard
      is unsafe post-`fork()`). Pinned in `tests/clipboard.rx` (set->get round-trip,
      overwrite replaces, set/get availability agree).
- [x] **IME composition events** — `Event.TextEditing(start, length)` (the
      composition cursor + selection) from `SDL_TEXTEDITING`, the in-progress
      (uncommitted) marked text CJK / diacritic input needs beyond the committed
      `TextInput`. The marked-text STRING is a side-channel: each TextEditing event
      self-carries it in the ring slot (`RxEvent.text[32]`, SDL's composition cap),
      COPIED at push time so no SDL pointer dangles, and read back via
      `Window#composition_text` (a Ruxen-owned String via `ruxen_string_from`)
      right after polling. `Window#push_composition` injects one (carrying the
      string, which plain `push_event` can't); the live SDL pump and a
      `window_pump_test_textediting` seam share the same `push_event_text` path.
      The `Event` variant is APPENDED last so prior tag values stay stable. The
      full sound subset landed (multi-byte CJK round-trip pinned) — no Q-candidate
      needed. Pinned in `tests/ime_editing.rx`.
- [ ] **Mouse cursors** — `Window#set_cursor(kind)`.

## Phase 1.5 — deferred (explicit checklist; NOT implemented in Phase 1)

- [ ] **Dash path effect** (`sk_patheffect_create_dash`). **Blocked on the fetched
      `libSkiaSharp` binary:** `nm -gU` on the fetched dylib shows **zero
      `sk_patheffect_*` symbols** — the prebuilt does not export the path-effect C
      API at all (same class as the absent SkParagraph/SkShaper). Binding it would
      either fail to resolve or (worse) resolve to a NULL-returning stub that lies
      in its capability probe — exactly the "silent no-op / silently-wrong" path
      the repo's FFI invariants forbid. **Remediation:** point
      `runtime/fetch_skia.sh` at a `libSkiaSharp` build that exports
      `sk_patheffect_create_dash` + `sk_patheffect_unref` (and verify
      `sk_paint_set_patheffect` is present), add a new per-RID SHA pin, then the
      binding is the usual additive 4-step + a pin (dashed line has gaps at
      predicted pixels). Until that binary is wired, dash stays unimplemented.
- [ ] **Render-to-texture / raster cache.**
- [ ] **Drag-and-drop.**
- [ ] **File dialogs.**
- [ ] **Fullscreen / minimize-maximize / DPI-change events.**
- [ ] **Vulkan** (additive behind the GPU seam, per `docs/GPU.md`).
- [ ] **Per-window / multi-monitor refresh rate** (Phase-1 `refresh_rate` is
      display-0 only).

## Later cycles

- Full canvas surface: `draw_path`, `draw_image`, transforms, clips, layers.
- Text i18n / accessibility (HarfBuzz shaping + paragraph layout via a separate
  `HarfBuzzSharp` native lib + ICU).
- Platform matrix: macOS/Windows/Linux → Android/iOS → web (WASM + canvas).

## Remaining — tracked checklist

Audited 2026-06-08 against `src/**`, `runtime/**`, and CHANGELOG `[Unreleased]`.
Ordered by what unblocks `quiver`'s widget library soonest. `→ ruxen #X` marks a
cross-repo dependency on a language fix (see `../ruxen/docs/TASKS.md`).

### Public-API ergonomics (ruxen language-feature adoption)

- [x] **`Window#frame` / `Canvas#frame` — resource-bracket block API** (adopts
      ruxen Ruby-blocks). `window.frame do |c: &var Canvas| … end` brackets
      begin → draw → end → present; headless windows skip present so the harness
      pins it. Blockless → explicit `Err` (never a silent frame). `examples/
      counter.rx` converted. Pins in `tests/canvas_frame.rx` + `tests/window.rx`.
- [x] **`alias` adoption** — three genuine method synonyms (`Rect#overlaps`,
      `Canvas#fill_rect`, `Canvas#line_height`), each a pure resolver synonym,
      both-name pinned. Field/`?`-name/operator alias forms are out (E1120/E1123).
- [x] **Bare string literals replace `String.from("…")`** across src/tests/
      examples (literal args only; `String.from(var)` left intact).

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
- [x] **Configurable font *family*** — family selection via
      `sk_typeface_create_from_name` (+ `sk_fontstyle_*`), so widgets can pick a
      font, not just a size. Done: `Canvas#draw_text_font` / `#measure_text_font`
      / `#text_height_font`; a process-wide family cache; an unknown family
      falls back to the default typeface. Pin tests in `tests/canvas_fonts.rx`.
- [x] **Rich text — word-wrapped, aligned paragraphs** — multi-line wrapping
      labels / text blocks. The fetched `libSkiaSharp` has no SkParagraph/
      SkShaper, so wrapping is done in the shim (greedy whitespace word-wrap on
      the bound Skia font measure+draw — no new native lib). Done:
      `Canvas#draw_paragraph` (returns total height; align 0/1/2) /
      `#measure_paragraph` / `#measure_paragraph_width`; Skia-only (clean `Err`
      when absent); a long unbreakable word renders without looping. Pixel-
      verified in `tests/canvas_paragraph.rx`. **Scope: Latin word-wrap**; proper
      shaping (bidi/complex scripts) is the HarfBuzz/ICU follow-up below.
- [x] **Multi-window** — N independent on-screen windows per process
      (`docs/MULTIWINDOW.md`). The single-window globals in `runtime/sdl_window.c`
      became a fixed-size table of `RxWin` slots keyed by the owning `RxHost`; each
      `ruxen_canvas_window_*` entry point resolves its slot by `self`, so a
      single-window app touches one slot and is byte-for-byte backward compatible.
      SDL's one process-wide event queue is demuxed per `windowID` in the pump, so
      each window's input lands only in its own ring. New per-window teardown
      (`ruxen_canvas_window_destroy_for`, behind `Window#hide`). Headless pin tests
      (`tests/multiwindow.rx`: independent canvases + event rings, interleaved FIFO,
      deterministic N-window teardown); live two-window present + demux proven on a
      real display by `examples/multiwindow_verify.c` (`PASS`), not harness-
      verifiable (forked/headless harness). **Deferred:** concurrent live GL/Metal
      multi-window is architected (each slot carries its own GL/Metal state) but
      pixel-verified only for the raster path here; the GL proc-loader stays a
      process-global current-context seam (`Q-candidate`, below).

### Needs an architecture decision (design doc first, then implement)

- [~] **GPU surface backend (Ganesh)** — **decision in `docs/GPU.md`** (Metal
      on Apple, OpenGL on Linux/Windows behind one context seam, GL-first;
      Vulkan deferred). **GL backend landed** (the top rung of the
      backend-selection ladder, behind the unchanged `ruxen_canvas_*` ABI):
      - SDL GL context seam in `runtime/sdl_window.c`
        (`ruxen_canvas_window_create_gl` / `_gl_present` / `_gl_get_proc` /
        `_gl_drawable_size`), resolved in the same dlopen tier as the rest of
        SDL.
      - `GrDirectContext` + GPU-backed `SkSurface` over that GL context in
        `runtime/skia_shim.c` (`gr_glinterface_*`, `gr_direct_context_make_gl`,
        `gr_backendrendertarget_new_gl`, `sk_surface_new_backend_render_target`
        — OPTIONAL loader tier, a miss disables only the GPU rung).
      - Probes `Canvas#gpu_available?` / `#gpu_active?`; `Window#show_gpu`
        attempts GPU then falls back cleanly to the raster window path.
      - **CPU raster fallback preserved** as the test oracle; the GPU path is
        selected at runtime, never a replacement. Full GPU **pixel**
        verification is deferred to a GL-capable desktop (this host + CI are
        headless — same posture as the Skia-on-Linux-CI note in `docs/SKIA.md`);
        the smoke pin (`tests/gpu_backend.rx`) asserts the capability + clean
        fallback contract.
      - **GL-on-macOS status (with Skia now active):** `gpu_available?` is
        **true** on macOS — the Ganesh GL symbols resolve (this was previously
        blocked by a non-existent `gr_direct_context_unref` binding, now fixed to
        `gr_recording_context_unref`). A live GL **surface** still does not come
        up here because there is no SDL2 / display on this host, so `gpu_active?`
        stays false and drawing falls back cleanly. macOS GL is deprecated by
        Apple anyway — **Metal is the real Apple path**.
      - **Metal (Apple) LANDED — with headless GPU pixel verification.** The
        macOS `libSkiaSharp.dylib` is `SK_METAL=1`, so Metal is real. An
        **offscreen** Metal `SkSurface` (`gr_direct_context_make_metal` + a BGRA
        `sk_surface_new_render_target`; device+queue via `Metal.framework` + the
        objc runtime through `dlopen`) renders on the GPU with **no window**
        (`MTLCreateSystemDefaultDevice` is headless), and `sk_surface_read_pixels`
        reads the result back into the framebuffer on `end_frame` — the **first
        GPU backend pixel-verified locally**. `Canvas#enable_gpu_offscreen` is the
        no-window entry; `gpu_metal_available?` + `gpu_backend_kind == 2`
        (`RX_GPU_KIND_METAL`) report it; selection/teardown reuse the GL seam.
        Pixel proof is `examples/metal_offscreen_verify.c` (a standalone `PASS`,
        blue rect read back byte-exact) — NOT an in-harness draw, because Apple
        forbids Metal across `fork()`-without-`exec()` and the test harness forks
        per case (`MTLCompilerService` unreachable post-fork). The in-harness
        `tests/gpu_backend.rx` pins capability + clean fallback.
      - **On-screen windowed Metal (`CAMetalLayer`) LANDED.** SDL `SDL_WINDOW_METAL`
        → `SDL_Metal_CreateView`/`GetLayer` → `CAMetalLayer`; per frame acquire
        the next drawable, wrap its texture (`gr_backendrendertarget_new_metal`),
        build a per-frame `SkSurface`, draw, flush+submit, and present the
        drawable. `Window#show_gpu` ladder = on-screen Metal → GL window → raster;
        `Window#present` routes by backend. **Verified on a real display** by
        `examples/metal_window_verify.c` (`PASS`); not harness-verifiable (CF/Metal
        after `fork()` is blocked, harness forks per case → falls back to a raster
        window there). Also fixed the **host-aware SDL loader** (`load_sdl` only
        tried the Linux SO name, so macOS always fell back headless).
      - **Still deferred:** Vulkan — additive behind the same seam, per the ADR.

### Later cycles (large, sequenced)

- [~] **Text shaping / i18n** — proper shaping (kerning, ligatures, RTL/complex)
      via **HarfBuzz-direct + Skia glyph render** (`docs/SHAPING.md`). **Landed:**
      `HarfBuzzSharp.NativeAssets.macOS` fetched + SHA-pinned alongside Skia; the
      shim shapes one run (`hb_shape`) and renders the positioned glyphs with
      Skia's textblob API (libSkiaSharp has no SkParagraph/SkShaper, but DOES have
      `sk_textblob_*`). `Canvas#draw_text_shaped` / `#measure_text_shaped` /
      `#shaping_available?`; kerning + ligatures pixel-verified
      (`tests/canvas_shaping.rx`, `examples/shape_kerning_verify.c`). Plus
      **shaped paragraphs** — `Canvas#draw_paragraph_shaped` /
      `#measure_paragraph_shaped`: the greedy word-wrap now measures + renders
      each line through the shaped glyph path (a `"ffi"` line is narrower than the
      naive `f+f+i`; `tests/canvas_paragraph_shaped.rx`). **Deferred follow-ups:**
      ICU bidi / line-break / grapheme segmentation (the shaped wrap is still
      greedy-whitespace, not ICU line-break); per-line multi-run shaping + font
      fallback; family→file resolution; font/run caching. These are what "full
      international text in L2" still needs.
- [ ] **Accessibility** — platform a11y trees.
- [ ] **Platform matrix** — Windows → Android/iOS → web (WASM + canvas).

### Compiler-imposed deviations to revert (blocked on ruxen) — ALL REVERTED ✅

- [x] **`Event` coords are `Float32` again** (2026-06-09). The `Int`-logical-pixel
      workaround was reverted once ruxen **Q28** (f32 field/payload width-blind
      store) and **Q31** (enum under-allocation crashing repeated float-payload
      construction) were fixed and verified on the installed toolchain. Pointer /
      Scroll / Resize payloads are `Float32`; decode reads the C double accessors
      (`event_a`/`event_b`, newly declared) and `push_event` routes coords through
      the double `ruxen_canvas_push_event`; `KeyDown`/`TextInput` stay `Int`
      (keycode/codepoint). Sub-pixel round-trip pinned in
      `tests/subpixel_events.rx`; suite 143 green; the settings example runs a
      live windowed loop on the new decode. One residual ruxen quirk found while
      reverting — `Float32 == <negative Int literal>` miscompares (**Q33**, the
      payload value itself is correct) — worked around in one test with an
      as-Int compare.
- [x] **`measure_text` real `&String` advance width** — already the live path
      (ruxen **Q29** audit: borrowed `&String` over FFI was never broken; the
      char-count story described the legacy fallback). The dead
      `measure_text_n_raw` binding had zero callers and its Ruxen-side
      declaration is now deleted.
