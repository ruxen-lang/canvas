# Changelog

All notable changes to `canvas` are documented here. Format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); this project adheres
to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- **Per-character font fallback (CJK / emoji) — `Canvas#draw_text_fallback` /
  `#measure_text_fallback` / `#fallback_available?` / `#last_fallback_family`
  (Phase 3 — text i18n).** Any codepoint the base font lacks (all CJK / emoji under
  a Latin face) now renders as REAL GLYPHS instead of tofu boxes. The string is
  itemized into runs by font coverage (`sk_typeface_unichar_to_glyph`, glyph 0 =
  not covered); each uncovered run is matched to a covering typeface by the SYSTEM
  font manager (`sk_fontmgr_match_family_style_character`, Core Text on macOS — so
  CJK → PingFang/Hiragino, emoji → Apple Color Emoji, **without us naming or
  shipping any font**) and rendered through Skia's direct glyph mapping
  (`sk_font_unichar_to_glyph` + `sk_font_get_widths_bounds` → positioned textblob),
  so no font FILE / HarfBuzz is needed for the fallback runs. A process-wide
  fallback-typeface cache keyed by Unicode block keeps a CJK paragraph from calling
  the (Core-Text-backed) fontmgr per character. **NO new native dependency** — the
  system font manager is already in `libSkiaSharp`. Color emoji renders in color
  automatically (Skia detects the color glyph table). Skia + fontmgr only — a clean
  `Err` when the fontmgr surface is absent; the non-fallback shaped/text path is
  untouched. **Binding gotchas pinned** (skia_capi.h): the per-character match
  SEGFAULTS with a NULL style (we pass a normal style), and
  `sk_typeface_get_family_name` RETURNS an owned `sk_string_t*` (the buf-fill form
  returns empty). Pixel-pinned in `tests/canvas_fallback.rx` (a CJK string renders
  non-blank pixels; Latin+CJK measures strictly wider than the Latin prefix; the
  fallback family is reported). ADR: `docs/decisions/text-fallback.md`. **Scope:**
  CJK + emoji render; complex-script SHAPING (Indic/Arabic ligature+reorder) inside
  a fallback font needs the font file for HarfBuzz and is the documented remainder.
- **Native file dialogs (macOS) — `Window.open_file_dialog` /
  `Window.save_file_dialog` (Phase 2).** A real **NSOpenPanel / NSSavePanel** driven
  through the objc runtime via `dlopen` (`objc_getClass` + `objc_msgSend`,
  foreground-app activation, `[panel runModal]`, `[[panel URL] path]`) — no new link
  dependency, the same pattern the Metal device path uses. The chosen path returns
  as a Ruxen-owned String; cancel / unavailable returns the empty string (never
  NULL) which the wrapper maps to `Err`, and `Window.file_dialogs_available?` gates
  the capability. **Honest-scope:** Cocoa is unsafe after `fork()`, and the harness
  forks per case, so the dialogs are gated OFF under the harness and return a clean,
  PROMPT `Err` (never a hang). The live modal (needs a human click) is the manual
  `examples/file_dialog_verify.c` (compile-checked, not automated); the headless Err
  contract is pinned in `tests/file_dialog.rx`. Non-macOS backends (GTK/Win32) are a
  later platform-matrix item — `file_dialogs_available?` is false off macOS, so
  callers degrade cleanly.
- **Per-window / multi-monitor refresh rate — `Window#refresh_rate` (Phase 2).**
  An INSTANCE `Window#refresh_rate -> Result[Int, String]` (distinct from the
  existing static `Window.refresh_rate`, which stays display-0) reports the Hz of
  the display THIS window is on: `SDL_GetWindowDisplayIndex(win)` picks the window's
  monitor, then `SDL_GetDesktopDisplayMode(thatIndex)` reads its rate — so a window
  on a 144 Hz second monitor reports 144, not the primary's 60. Same Err contract
  as the static one (`> 0` → `Ok`; negative `-RXC_ERR_*` → `Err` for no window /
  no SDL / unspecified rate, never a bogus `Ok(0)`). `SDL_GetWindowDisplayIndex` is
  OPTIONAL — a miss falls back to display 0. Pinned in `tests/frame_pacing.rx`.
- **Render-to-texture / raster cache — `Canvas#snapshot` (Phase 2).** Copies a
  canvas's current surface into an immutable `Image` (`sk_surface_new_image_snapshot`),
  which is then drawn into ANY canvas at any offset through the EXISTING `draw_image`
  path — the primitive L2 uses to cache an expensive subtree (draw once offscreen,
  snapshot, blit cheaply per frame) and the basis for future effects. The only new
  ABI is the snapshot (one shim entry + one loader symbol); `Image` / `draw_image`
  are reused wholesale. **API shape:** a standalone offscreen `Canvas.create(w,h)` +
  `snapshot`, not a host current-target switch — an offscreen raster host already
  owns its buffer and reuses the whole Canvas draw API, the least-new-ABI fit.
  **Ownership:** the snapshot is a COPY (does not alias the source pixels; later
  draws into the source, or dropping it, leave the image intact), caller-owned and
  freed via the SAME `Image` drop path as a loaded image. Skia-only — clean `Err`
  on the software-raster fallback. Pixel-pinned in `tests/canvas_snapshot.rx`.
- **KeyDown modifiers — `Window#key_modifiers` + `Window.mod_*` (Phase 2).** A
  `KeyDown` event now carries the held keyboard modifiers (shift / ctrl / alt /
  gui) as a side-channel, the discipline quiver's selection API (Shift+Arrow,
  Ctrl/Cmd+A) was waiting on. **Append-only:** `Event.KeyDown(Int)`'s payload is
  UNCHANGED (still a bare keycode) — the modifier mask rides a new `RxEvent.mods`
  ring-slot field and is read back via `Window#key_modifiers` right after polling,
  exactly like `dropped_file_path` / `composition_text`. The live pump reads
  `SDL_GetModState()` at pump time (robust across the event-struct layout, unlike
  decoding `keysym.mod` at a guessed offset) and folds SDL's `KMOD_*` into the
  stable `RX_MOD_*` bits; `push_event` / `push_event_text` clear the field so the
  mask never leaks across events. `Window.mod_shift` / `mod_ctrl` / `mod_alt` /
  `mod_gui` name the bits (`mod_gui` = Cmd/Super/Win). TextInput is unaffected.
  Pinned via the extended `window_pump_test_keydown` seam (now takes a folded mask
  arg) in `tests/key_modifiers.rx` (plain arrow → 0; Shift+Right carries shift with
  the keycode unchanged; a combined Ctrl+Alt mask; no leak to a following
  TextInput; auto-repeat still filtered).
- **Drag-and-drop (files) — `Event.FileDrop` + `Window#dropped_file_path`
  (Phase 2).** `SDL_DROPFILE` is surfaced as `Event.FileDrop` (NO coordinates —
  SDL2's file-drop gives no cursor position, and we don't invent one); the dropped
  PATH is a side-channel read via `Window#dropped_file_path` right after polling.
  **Memory contract:** SDL owns the dropped-path string (SDL-malloc'd); the pump
  copies it into the ring at pump time, then `SDL_free`s SDL's copy immediately —
  no SDL pointer ever dangles. Because a path is routinely longer than the 32-byte
  inline IME buffer, FileDrop gets its OWN owned heap copy per ring slot
  (`RxEvent.drop_path`): push `strdup`s it, poll MOVES it into `pending` (freeing
  the prior one), and a non-drop push / host_drop frees it — exactly one owner
  always, no truncation, no leak, no double-free. The `SDL_DropEvent` windowID is
  at offset 16 (not the usual 8); the path at offset 8. A multi-file drop arrives
  as several `SDL_DROPFILE` events (one per file). The `Event` variant is appended
  LAST (tag 9) so prior tags stay stable; `RXC_EVENT_KIND_MAX` is 9. `Window#
  push_file_drop` injects one (the live SDL pump and a `window_pump_test_dropfile`
  headless seam share the same path). Pinned in `tests/file_drop.rx` (round-trip; a
  >32-byte path intact; multi-file FIFO; FileDrop/TextEditing side-channels stay
  separate; seam matches live handler).
- **Desktop window management — `Window#set_fullscreen` / `#maximize` /
  `#minimize` / `#restore` / `#set_min_size` / `#set_max_size` (Phase 2).**
  Per-window setters, each resolving its `RxWin` slot by the owning host
  (multi-window correct). `set_fullscreen(on:)` uses `SDL_WINDOW_FULLSCREEN_DESKTOP`
  (borderless desktop fullscreen — no display-mode switch, instant alt-tab, the
  modern default; the exclusive mode is deliberately not used). Each `Ok(nil)` on
  success; `Err` when the window is not shown or the SDL entry point is absent;
  negative min/max size is a bad-args `Err`. **Pump window-event handling:**
  `SDL_WINDOWEVENT_MINIMIZED` sets a per-slot `minimized` flag — while set,
  present / gl_present / metal_present are no-ops (the **minimized-present
  contract**: an occluded drawable is not presented, so the render loop costs
  nothing while minimized); `MAXIMIZED` / `RESTORED` / `DISPLAY_CHANGED` clear it,
  re-derive the backing surface via the existing resize machinery, and emit
  `Event.Resize` in the window's design size. **No new `Event` variant for
  DPI/display change** — `Event.Resize` already carries the design size and
  triggers the surface re-creation a content-scale change needs (the enum tag set
  is unchanged). Pins: `tests/window_mgmt.rx` (Err-without-window + bad-args; the
  minimized flag + each subtype's Resize via the `window_pump_test_window_event`
  seam). Live proof: `examples/window_mgmt_verify.c` (`PASS` on a real display).
- **Dash path effect — `Canvas#draw_dashed_line` (Phase 2).** A stroked line
  with an `[on_len, off_len]` dash pattern and a `phase` offset, via
  `sk_path_effect_create_dash`. **Re-verdict on Phase-1's "binary blocked":** that
  conclusion was a FALSE NEGATIVE from grepping `sk_patheffect_*` (no underscore
  split); the pinned 3.119.4 `libSkiaSharp` exports the real `sk_path_effect_*`
  names (`_create_dash` / `_unref`) plus `sk_paint_set_path_effect`, confirmed by
  `nm -gU`. So dash bound on the EXISTING binary — no `fetch_skia.sh` change, no
  re-fetch, no new SHA. The owned path-effect is created → set on the paint (which
  takes its own ref) → unref'd → the paint deleted (no leak on any path).
  `on_len<=0` / `off_len<0` / `width<=0` is `Err`; Skia-only (clean
  `RXC_ERR_NO_SKIA` if a build lacks the symbols — an honest probe, never a
  NULL-stub). Pixel-pinned in `tests/canvas_dash.rx` (an on-run pixel is inked, an
  off-gap pixel stays background, and a solid-line control inks that same gap
  column). See `docs/ROADMAP.md` Phase 2 for the full symbol table.
- **Mouse cursors — `Window.set_cursor(kind)` (E2).** Stock system cursors via
  `SDL_CreateSystemCursor`, a small int enum (0 arrow / 1 ibeam / 2 hand /
  3 crosshair / 4 resize-h / 5 resize-v — `Window.cursor_*` constants name them),
  cached per process so hover-driven changes are cheap. `Ok(nil)` on success;
  `Err` for an out-of-range kind or when SDL / a real cursor backend is
  unavailable (the dummy driver on a fully headless host reports "not currently
  supported" → clean Err, never a crash). `Window.cursors_available?` probes the
  capability. Cursor state is process-global, so these are class methods. Pinned
  in `tests/cursors.rx` (validation; every valid kind agrees with the availability
  probe; idempotent re-set; constants).
- **IME composition events — `Event.TextEditing(start, length)` (E2).** The
  in-progress (uncommitted) "marked" text from `SDL_TEXTEDITING` — what CJK /
  diacritic input produces while composing, before the committed `TextInput`. The
  two ints are the composition cursor `start` and selection `length`; the marked
  text STRING is a side-channel: each event self-carries it in the ring slot
  (`RxEvent.text[32]`, SDL's composition cap), COPIED at push time so no SDL buffer
  dangles, and read via `Window#composition_text` (a Ruxen-owned String through
  `ruxen_string_from`) immediately after polling. `Window#push_composition` injects
  one with its marked text; the live SDL pump (`SDL_TEXTEDITING` handler) and a
  `window_pump_test_textediting` headless seam share one `push_event_text` path.
  The `Event` variant is APPENDED last so prior tag values stay stable (the C shim
  dispatches by integer tag; existing events pin green). Multi-byte CJK marked text
  round-trips verbatim. Pinned in `tests/ime_editing.rx`.
- **Clipboard — `Window.clipboard_text` / `Window.set_clipboard_text` (E2).** The
  system clipboard via `SDL_GetClipboardText` / `SDL_SetClipboardText` (dlopen
  tier). `clipboard_text -> Result[String, String]` (`Ok(text)`, possibly empty;
  `Err` when no SDL / no clipboard backend); `set_clipboard_text(s) -> Result[nil,
  String]`. **Works without a window** — SDL's clipboard round-trips a set->get
  under the dummy video driver headless (verified), so the harness pins a REAL
  round-trip. **New FFI convention (docs/FFI.md): returning a String from C to
  Ruxen.** A Ruxen String is a `malloc`'d `char*`, so the C side builds the
  returned text with `ruxen_string_from` (Ruxen-owned, freed by Ruxen's allocator)
  and `SDL_free`s the SDL copy — never hand a foreign-allocator pointer to a Ruxen
  String. Under the forked harness the dummy driver is forced for fork-safety (the
  real Cocoa pasteboard is unsafe post-`fork()`). Pinned in `tests/clipboard.rx`.
- **Blur image filter — `Canvas#save_layer_blur(sigma)`.** Pushes an offscreen
  layer whose paint carries a Gaussian blur image filter (`sk_imagefilter_new_blur`
  + `sk_paint_set_imagefilter`), so everything drawn into the layer is blurred when
  the matching `restore` composites it down. The general blur primitive — it blurs
  arbitrary content (shapes, text, paths), generalizing the rrect-only
  `draw_round_rect_shadow`: frosted-glass panels, blurred backdrops, and soft
  shadows of any shape (push the blur layer, draw the shadow-colored shape,
  `restore`, then draw the crisp widget on top). Returns the layer's save count;
  `sigma <= 0` is `Err`; the blur filter is unref'd after `save_layer` takes its
  own ref. Skia-only (clean `Err` when absent — a blurred layer can't be faked in
  software). Pixel-pinned in `tests/canvas_blur.rx` (ink spreads past a hard edge
  under blur; a control without the layer keeps the same pixel hard; sigma
  validation).
- **Blend modes — `Canvas#set_blend_mode(mode)`.** A small stable int enum
  (`0` src-over / `1` clear / `2` src / `3` multiply / `4` screen — `Canvas.blend_*`
  constants name them) mapped to `SkBlendMode` in the shim via
  `sk_paint_set_blendmode`. It is host PAINT STATE, not a per-draw argument (Skia
  paints carry the blend mode, and threading a blend arg through ~20 draw
  signatures would be needless ABI churn): the mode persists until changed and is
  RESET to source-over at the start of each frame (`begin_frame`), so it never
  leaks across frames — the same discipline as the transform/clip reset. Applied
  to the shape-fill / line / path draws (all funnel through the one `rx_make_paint`
  helper) on the active Skia backend; under the software fallback the mode is
  stored but draws stay source-over (no wrong pixels). An out-of-range mode is
  `Err`. Pixel-pinned in `tests/canvas_blend.rx` (multiply darkens red*green to
  black; screen lightens to yellow; the per-frame reset; out-of-range rejection).
- **Transforms — `Canvas#skew` + `Canvas#concat` (full 2D affine).** Completes the
  transform set (`translate`/`scale`/`rotate` already shipped). `skew(sx, sy)`
  (`sk_canvas_skew`) shears the coordinate system; `concat(a, b, c, d, e, f)`
  (`sk_canvas_concat`) concatenates an arbitrary affine given as the matrix's top
  two rows (`a` scaleX, `b` skewX, `c` transX / `d` skewY, `e` scaleY, `f` transY)
  — the general primitive the others are special cases of. Both compose with the
  current matrix and are scoped by `save`/`restore`; Skia-only (no-op under the
  software fallback, like the existing transforms). **Binding gotcha (pinned):**
  this fetched `libSkiaSharp`'s `sk_canvas_concat` consumes a 4x4 SkM44 in
  COLUMN-MAJOR order (16 floats), **not** a 3x3 `sk_matrix_t` — determined
  empirically by round-tripping `sk_canvas_get_matrix` through `sk_canvas_concat`
  (a 3x3 struct silently produces no transform). The shim builds the 16-float
  column-major matrix (scaleX@0, scaleY@5, transX@12, transY@13, skewY@1, skewX@4);
  documented in `runtime/skia/skia_capi.h`. Pixel-pinned in
  `tests/canvas_transforms.rx`.
- **Frame pacing / vsync seam — the engine timebase + paced present**
  (ADR: `docs/decisions/frame-pacing.md`). Replaces the examples' blind
  `sleep_ms(16)` with a real, work-aware cadence and gives L2's future animation
  ticker a monotonic timebase. Three additive bindings:
  - `Window#ticks_ns` / `#ticks_ms` — a monotonic-nanosecond clock over
    `clock_gettime(CLOCK_MONOTONIC)` (NTP/DST-immune, difference-only epoch;
    `int64` ns, no overflow for ~292 years). No new dependency — `skia_shim.c`
    already used `<time.h>`.
  - `Window#wait_frame(target_ns)` — paces a frame to an ABSOLUTE monotonic
    target tick: sleeps the remainder of an early frame, returns immediately on a
    late one (fills slack, never adds latency). Absolute target self-corrects
    (advance `target += interval`; early/late frames converge, no drift). On the
    on-screen Metal/GL paths the present already blocks on vsync, so it is a
    no-op-by-design there; it is the software clock for raster/headless and the
    one uniform pacing seam L2 calls regardless of backend.
  - `Window.refresh_rate -> Result[Int, String]` — the display refresh rate (Hz)
    via `SDL_GetDesktopDisplayMode` (new OPTIONAL SDL symbol), a HINT for sizing
    the interval. `Err` when headless / no SDL / unspecified — never a bogus
    `Ok(0)` a caller would divide by.
  - **Out of scope by design:** no callback/timer/scheduler — that is L2's
    animation loop to own; the engine provides the clock + the frame-boundary
    wait. Pins: `tests/frame_pacing.rx` (monotonicity, both `wait_frame`
    branches, `ms = ns/1e6`, refresh-rate Err contract — all headless).
- **`Window#frame` and `Canvas#frame` — the resource-bracket block API**
  (adopts ruxen's new Ruby-block feature, `docs/decisions/ruby-block-semantics.md`).
  `window.frame do |c: &var Canvas| … end` brackets `begin_frame` → run the block
  on the borrowed canvas → `end_frame` → `present` (present only when the window
  is on screen, so headless windows pin fully in the forked harness). The
  four-call paint dance every render loop did by hand collapses to one block.
  `Canvas#frame` is the offscreen counterpart (no present). A block is REQUIRED:
  a blockless call returns `Err("canvas: frame requires a block")` (guarded via
  `block_defined?`), never a silent empty frame; a `begin_frame`/`end_frame`/
  `present` error short-circuits to `Err` and a failed `begin_frame` means the
  block never runs. Pins: `tests/canvas_frame.rx` (runs-once, blockless Err,
  begin-failure short-circuit, reopen) and `tests/window.rx` (headless bracket +
  blockless Err). `examples/counter.rx` is converted to the new form.
  - **Known ruxen Tier-1 gap surfaced:** a paren-LESS blockless call to an
    optional-block method (`w.frame`) emits one too few MIR args and crashes the
    verifier; the blockless path is reached via `w.frame()` (parens). This is the
    documented limitation in the ruby-block ADR, not a canvas bug.
- **`alias` adoption in the public API** (ruxen's new `alias new old` keyword,
  `docs/decisions/alias-keyword.md`) — three genuine same-signature method
  synonyms, each pinned by calling both names: `Rect#overlaps` → `intersects`,
  `Canvas#fill_rect` → `draw_rect` (this draw fills), `Canvas#line_height` →
  `text_height`. Each is a pure resolver synonym (one method body, zero extra
  codegen). Field targets (`alias w width`) and `?`-suffixed alias names were
  rejected — `alias` can only target a method (E1120), and a `?`-named alias
  misparses at a paren-less call site; operator aliases are staged upstream
  (E1123).

### Changed
- **Bare string literals replace `String.from("…")` across `src/`, `tests/`, and
  `examples/`.** The installed toolchain coerces a bare `"…"` literal (including
  interpolated `"… #{x} …"`) to `String` in every position — params, fields,
  static-method args (`Window.open("Settings", …)`), `Err("…")` into
  `Result[_, String]`, and locals — so `String.from("literal")` is now vestigial.
  ~170 literal sites were rewritten. The remaining 19 `String.from(var)` sites
  (consuming reuses of `let`-bound owned Strings, in `tests/canvas_shaping.rx` +
  `tests/canvas_paragraph_shaped.rx`) are now `var.clone` — post-Q38 a `let x = "…"`
  binds an OWNED String and a consuming call reuses it via `x.clone`; no
  `String.from` survives anywhere in `src/`, `tests/`, or `examples/`. Suite green
  before and after the sweep.
- **`Event` coordinates are `Float32` (sub-pixel) — the `Int` workaround is
  reverted.** `PointerMove`/`PointerDown`/`PointerUp`/`Resize`/`Scroll` payloads
  carry `Float32` logical pixels; `KeyDown`/`TextInput` stay `Int`
  (keycode/codepoint, read via the Int accessor so large keycodes never round
  through f32). Unblocked by ruxen Q28 (f32 field-store width fix) + Q31 (enum
  slot-rounded allocation — repeated float-payload construction no longer
  corrupts the heap), both verified on the installed toolchain. Decode now reads
  the C double accessors `ruxen_canvas_event_a`/`_b` (newly declared in
  `raw_host.rx`) and `push_event` routes coordinate events through the double
  `ruxen_canvas_push_event` (declared as `push_event_d_raw`); the C shim is
  unchanged (it always carried doubles). Sub-pixel precision is pinned by
  `tests/subpixel_events.rx` (2.5/3.25 round-trip exact); Int literals still
  construct/compare via coercion so existing call sites kept working. Known
  ruxen quirk found while reverting: `Float32 == <negative Int literal>`
  miscompares (Q33; the value is correct) — one test compares through `as Int`.

### Removed
- Dead `measure_text_n_raw` declaration (the legacy char-count fallback): zero
  callers since `measure_text` switched to the real borrowed-`&String` path
  (confirmed sound by the ruxen Q29 audit).

### Added
- **Multi-window support — N independent on-screen windows per process**
  (`docs/MULTIWINDOW.md`). `Window.open` can now create and track multiple live
  windows; each owns its own SDL window + renderer/GPU surface and its own event
  ring. The single-window globals in `runtime/sdl_window.c` became a fixed-size
  table of `RxWin` slots keyed by the owning `RxHost` pointer (no heap, capacity
  `RX_MAX_WINDOWS = 16`); every `ruxen_canvas_window_*` entry point looks its
  slot up by `self`, so a single-window app touches exactly one slot and behaves
  **identically** (backward compatible — all prior tests unchanged + green).
  - **Per-windowID event demux.** SDL has one process-wide event queue; the pump
    now reads each event's SDL `windowID` (byte offset 8 of every
    window-associated event, via `SDL_GetWindowID` recorded at create) and routes
    it to the owning window's host ring — so window B's clicks never land in
    window A's stream. `SDL_QUIT` (no windowID) delivers `CloseRequested` to every
    live window.
  - **Per-window teardown** — new flat binding `ruxen_canvas_window_destroy_for`
    (host param) tears down only that window; `Window#hide` now routes through it
    so hiding one window leaves the others up. The legacy no-arg
    `ruxen_canvas_window_destroy` tears down all windows; `note_host_dropped`
    frees only the dropping host's slot.
  - Headless pin tests in `tests/multiwindow.rx` (independent canvases +
    framebuffers, per-window event-ring isolation, interleaved FIFO order,
    deterministic teardown of many concurrent windows). The live two-window
    present + windowID demux is proven on a real display by
    `examples/multiwindow_verify.c` (`PASS`: two windows, ids 1 & 2, red + blue
    presented independently) — not harness-verifiable (the harness forks per case
    and runs headless).
- **Proper text entry — `Event.TextInput(codepoint)`.** Printable typing now
  flows through the OS text path (`SDL_StartTextInput` + `SDL_TEXTINPUT`), which
  is layout/shift-correct and composition-aware, instead of raw keysyms. The
  pump decodes the event's UTF-8 (ASCII fast-path + 2–4-byte sequences) to a
  Unicode codepoint and emits `Event.TextInput(Int)` (kind 7). `Event.KeyDown`
  is now **control keys only** (editing/navigation allowlist) with auto-repeat
  filtered. So: printable characters → `TextInput`; Backspace/arrows/Enter/Delete/
  Tab/Home/End/Esc → `KeyDown` (once per press). Wired through
  `Window#poll_event` / `#push_event`; pinned headless in `tests/text_input.rx`.
- **Scroll + Resize events for windowed apps.**
  - `Event.Scroll(dx, dy)` — mouse-wheel deltas (+y up / away, +x right), emitted
    from `SDL_MOUSEWHEEL`; deltas are wheel "clicks", not coordinates (not
    scaled). L2 routes it to scroll the hovered widget.
  - Windows are created `SDL_WINDOW_RESIZABLE`; the pump emits `Event.Resize(w, h)`
    (in DESIGN coords) from `SDL_WINDOWEVENT` → `RESIZED`. On resize the GPU
    surface is re-created at the new backing size — Metal updates the layer's
    `drawableSize` (the per-frame drawable picks it up); GL invalidates its
    persistent surface so the next frame rebuilds at the new drawable size.
  - Both wired through `Window#poll_event` / `#push_event`
    (`tests/scroll_resize.rx`).
- **On-screen windowed GPU present (Metal swapchain via `CAMetalLayer`).** Turns
  the offscreen Metal path into a real window (`docs/GPU.md`):
  - SDL window (`SDL_WINDOW_METAL`) → `SDL_Metal_CreateView` /
    `SDL_Metal_GetLayer` → `CAMetalLayer` (configured with the system
    `MTLDevice`, BGRA8, `framebufferOnly=NO`). Per frame: acquire the layer's
    next drawable → wrap its `MTLTexture` (`gr_backendrendertarget_new_metal`) →
    `sk_surface_new_backend_render_target` (a **per-frame** surface — each frame
    consumes a fresh drawable) → draw → `flush_and_submit` → present
    (`[queue commandBuffer]` `[presentDrawable:]` `[commit]`).
  - **Backend-selection ladder** in `Window#show_gpu`: on Apple, **on-screen
    Metal → GL window → raster**, each rung falling through cleanly to the next;
    `ruxen_canvas_host_enable_gpu_windowed` gates on acquiring a first drawable,
    so a headless host fails the rung and falls back. `Window#present` routes by
    backend (Metal drawable present / GL swap / raster blit). Reuses the
    `gr_direct_context_make_metal` + device/queue singleton + `gpu_backend_kind`
    seam. ABI unchanged; raster/offscreen paths untouched; any drawable failure
    falls back (never a half-presented frame / wrong pixels). Teardown: surface →
    render-target → context → layer → view → window.
  - **HiDPI / Retina-correct present (fixes blur).** All window paths now use
    `SDL_WINDOW_ALLOW_HIGHDPI` and render at the true **backing pixel** size
    (queried via `SDL_Metal_GetDrawableSize` / `SDL_GL_GetDrawableSize` /
    `SDL_GetRendererOutputSize`) — Metal sets `CAMetalLayer.drawableSize` to the
    backing size and builds the Skia surface at that resolution (crisp by
    construction), presented 1:1. Previously a small logical buffer was stretched
    by our scale **and** upscaled again by Retina (double blur). The input pump's
    logical-point → framebuffer mapping (`/ s_scale`) is already correct since
    SDL reports mouse in logical points. (`examples/metal_window_verify.c` reports
    `logical 320x240 -> backing 640x480 (dpr ~2.0)`.)
  - **Design→backing content scale (auto, no per-app wiring).** Sizing the
    surface to backing pixels isn't enough: an app draws at DESIGN coordinates,
    so without a transform its content lands at design size in a larger backing
    surface (small / cornered). `begin_frame` now applies a base transform
    `scale = backing / design` as the first canvas transform (re-established each
    frame, below the outer per-frame save), so design-coord `draw_*` fill the
    backing surface and Skia rasterizes at native density. Design size = the
    logical framebuffer size, set automatically when a windowed GPU host is
    enabled (Metal + GL). **Additive** — offscreen / test surfaces leave it unset
    (scale 1.0), so the existing suite is untouched. `Canvas#set_design_size`
    opts an offscreen canvas in; `Canvas#content_scale` reports `(sx, sy)`.
    Headless-verified (`tests/hidpi_content_scale.rx`): a 320×240-design draw
    fills a 640×480 backing buffer; text under 2× scale inks more rows.
  - **SDL loader fix (load-bearing):** `load_sdl()` only `dlopen`'d the Linux SO
    name, so SDL never loaded on macOS and every `Window.show` fell back headless.
    Now host-aware — a candidate list (macOS dylib names, Homebrew full paths at
    `/opt/homebrew` + `/usr/local` since `dlopen` doesn't search them, the
    `SDL2.framework`, the Linux SO; `$RUXEN_CANVAS_SDL2` overrides). With SDL2
    installed, `RawHost.sdl_available == 1`.
  - **Deterministic suite under real windows.** macOS forbids CF/Metal/AppKit
    windowing after `fork()`, and the harness forks per case + fans out in
    parallel (real windows race the WindowServer → flaky). So real-window
    creation is gated off under the test harness (detected via `RUXEN_TEST_FORMAT`)
    → the windowing pin tests run on the deterministic headless framebuffer path;
    `RUXEN_CANVAS_WINDOW=1` (or running outside the harness, like the examples)
    forces real windows. The `sdl_available` probe is unaffected.
  - **Verified on a real display** by `examples/metal_window_verify.c`
    (`cc -O2 -o m examples/metal_window_verify.c && ./m` → `PASS`: a Metal window
    shows a GPU-drawn blue rect). **Not verifiable in the harness** (macOS forbids
    CF/Metal/AppKit windowing after `fork()` without `exec()`, and the harness
    forks per case), so the windowed rungs fall back to a raster window there;
    `tests/gpu_backend.rx` pins the capability + ladder + clean-fallback contract.
- **Shaped paragraphs — word-wrapped text with kerning + ligatures.** Combines
  the wrap + shaping work so wrapping labels / text blocks get real shaping
  (`docs/SHAPING.md`):
  - `Canvas#draw_paragraph_shaped(text, x, y, max_width, size, font_path, align,
    direction, color) -> Result[Int, String]` (total height) and
    `#measure_paragraph_shaped(...) -> Int` / `#measure_paragraph_shaped_width`.
    The same greedy whitespace word-wrap as `draw_paragraph`, but each line's
    width (for the wrap decision **and** alignment) is the HarfBuzz-**shaped**
    advance, and each wrapped line is rendered through the shaped glyph path — so
    wrapping, alignment, and ink of a line with `ffi`/`AV` differ from the naive
    per-char path.
  - **Additive / ABI-stable:** `rx_paragraph_layout` gained an optional shaper
    context; the non-shaped `draw_paragraph` / `measure_paragraph` path is
    untouched (passes `NULL`). The shaper (HarfBuzz face/font + Skia typeface) is
    built **once per paragraph**, not per line; a line's width comes from shaping
    its byte sub-range via `hb_buffer_add_utf8` offset/length
    (`runtime/skia_shim.c` `ruxen_canvas_draw_paragraph_shaped` /
    `_measure_paragraph_shaped`). Skia + HarfBuzz only — clean `Err`/0 when
    absent, with the non-shaped paragraph path as the fallback.
  - **Scope:** greedy whitespace wrap (not ICU line-break), one font by file
    path. ICU bidi / line-break and per-line multi-run/font-fallback remain the
    deferred "full international" follow-ups.
  - Verified on real Skia + HarfBuzz (`tests/canvas_paragraph_shaped.rx`): a
    `"ffi"` paragraph line is strictly narrower than the naive `f+f+i` sum (the
    ligature proves the wrap uses shaped widths); the paragraph line width equals
    the shaped run width; a narrower column wraps taller; multi-line shaped ink
    reads back; center/right alignment shift the shaped ink.
- **Proper text shaping — kerning + ligatures (HarfBuzz + Skia glyphs).** The
  last big text gap: real shaping instead of naive per-character placement
  (`docs/SHAPING.md`):
  - The fetched `libSkiaSharp` ships no SkParagraph/SkShaper, but it **does**
    expose positioned-glyph rendering (`sk_textblob_builder_alloc_run_pos`,
    `sk_canvas_draw_text_blob`, `sk_typeface_create_from_file`). So shaping is
    **HarfBuzz-direct (shape) + Skia glyph-draw (render)**: `runtime/fetch_skia.sh`
    now fetches + SHA-pins **`HarfBuzzSharp.NativeAssets.macOS`** (a 2.5 MB
    universal dylib) alongside Skia; the shim `dlopen`s it (`rx_hb()`, OPTIONAL
    tier). Both are fed the **same font file** so glyph ids match. No SkParagraph
    rebuild, no ICU for Latin kerning/ligatures.
  - `Canvas#draw_text_shaped(text, x, y, size, font_path, direction, color)
    -> Result[Int, String]` shapes one run (kerning, ligatures; `direction` 0
    auto / 1 LTR / 2 RTL) and renders it, returning the shaped advance width;
    `#measure_text_shaped(...) -> Int` is the width without drawing;
    `#shaping_available? -> Bool` probes the capability. Skia + HarfBuzz only —
    clean `Err`/0 when absent, with the non-shaped `draw_text`/`draw_paragraph`
    as the fallback (`runtime/skia_shim.c` `ruxen_canvas_draw_text_shaped` /
    `_measure_text_shaped`).
  - **Scope (bounded first increment):** one run, one font by file path. Bidi /
    line-break / grapheme segmentation (ICU), multi-run paragraph integration,
    and family→file resolution are deferred follow-ups (`docs/SHAPING.md`).
  - Verified locally on real Skia + HarfBuzz (`tests/canvas_shaping.rx`): `"AV"`
    shaped < `"A"` + `"V"` (GPOS kerning), `"ffi"` < 3× `"f"` (ffi ligature),
    shaped ink renders within the reported advance, a bad font path fails
    cleanly. `examples/shape_kerning_verify.c` is a standalone committed proof
    (`PASS`: AV 1.8px tighter, ffi 1.8px tighter, textblob inked).
- **Rich text — multi-line, word-wrapped, aligned paragraphs.** Wrapping labels
  and text blocks, the biggest fundamental UI-text gap:
  - `Canvas#draw_paragraph(text, x, y, max_width, size, family, align, color)
    -> Result[Int, String]` returns the laid-out **total height** (so L2 can
    size a text block); `x` is the column's left edge, `y` the first line's
    baseline, lines stack one line-height below. `align`: 0 left / 1 center /
    2 right.
  - `Canvas#measure_paragraph(text, max_width, size, family) -> Int` (wrapped
    block height) + `#measure_paragraph_width(...) -> Int` (widest line) for
    layout, no draw. The shim packs both into one call.
  - **Implemented in the shim, no new native lib.** The fetched `libSkiaSharp`
    ships no SkParagraph/SkShaper (needs a separate HarfBuzzSharp+ICU build), so
    wrapping is greedy whitespace word-wrap on the already-bound Skia font
    measure+draw: append words while the measured line fits `max_width`, else
    break; an explicit `\n` forces a break; a single word wider than the column
    renders on its own line (never loops). Skia-only (clean `Err`/0 when the
    backend is absent — a wrapped paragraph can't be faked on the 5×7 bitmap);
    graceful default-family fallback; real Skia metrics for line height +
    advances (`runtime/skia_shim.c` `ruxen_canvas_draw_paragraph` /
    `_measure_paragraph`).
  - **Scope:** Latin word-wrap (covers the vast majority of UI text). Proper
    international shaping — bidi, ligatures, complex scripts — is deferred to a
    later HarfBuzz/ICU follow-up.
  - Pixel-verified locally on active Skia (`tests/canvas_paragraph.rx`): long
    text wraps to multiple rows; a narrower column wraps taller than a wide one;
    center/right alignment measurably shift the ink right; a long unbreakable
    word still renders. Dual-branch on `skia_active?`.
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
- **Typing produced wrong/duplicate characters + stray spaces.** The pump
  forwarded raw `SDL_KEYDOWN` keysyms as if they were characters — no shift /
  layout, unfiltered auto-repeat, and some keysyms in the printable range
  inserting garbage. Switched to the OS text path (see `Event.TextInput` above):
  printable characters now come from `SDL_TEXTINPUT` (layout/shift-correct UTF-8),
  `KeyDown` is restricted to a control-key allowlist, and key auto-repeat is
  dropped. (ABI note: the `SDL_KeyboardEvent` repeat flag is at offset **13**
  (`state@12`, `repeat@13`), verified against `SDL_events.h`.)
- **Windowed pointer coordinates landed ~N× too low (interaction broken).** With
  `show_gpu_scaled(N)` / `show_scaled(N)` the OS window is N× the design size and
  SDL reports the mouse in window POINTS, but the **Metal/GL windowed create
  paths set `s_scale = 1`** while the pump divides pointer x/y by `s_scale` — so
  coordinates were never divided by N and a click on the top of the UI registered
  ~N× lower (on a different widget). Fixed: both windowed backends now set
  `s_scale = N` (the show factor), so the pump maps window points → design coords
  (`point / N`). `s_scale` is the design→point factor; the Retina backing/dpr is
  tracked separately (render-only) and does not enter the pump (SDL reports points
  under `ALLOW_HIGHDPI`). Headless-pinned (`tests/input_scale.rx`): a window point
  `(200,300)` at scale 2 → design `(100,150)`; a top click `y=20` → `10`.
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
