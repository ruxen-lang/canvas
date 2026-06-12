# canvas examples

## Render a scene and view it in a desktop window

The library is headless (deterministic software raster backend), so "seeing"
a render is: draw → dump the framebuffer → present it.

```bash
# 1. scaffold a scratch app that depends on canvas
mkdir -p /tmp/canvas_demo/src && cd /tmp/canvas_demo
cat > Ruxen.toml <<'TOML'
[package]
name = "canvas_demo"
version = "0.1.0"
edition = "2026"

[dependencies]
canvas = { path = "/path/to/canvas" }
TOML
cp /path/to/canvas/examples/demo.rx src/main.rx

# 2. draw the scene and dump it as out.ppm
RUXEN_ALLOW_EXTERNAL_PATH=1 ruxen run

# 3. present it in a desktop window (SDL3 loaded via dlopen — no SDL dev
#    package needed, just the runtime libSDL3.so.0)
gcc -O2 -o view /path/to/canvas/examples/view.c
./view out.ppm          # stays open until closed / keypress
./view out.ppm 5        # auto-close after ~5 seconds
```

`demo.rx` draws a header bar, three color swatches under a translucent
blended overlay, a button with text, and a footer — exercising clear,
draw_rect (incl. alpha blending), draw_text, and the event stream.

`view.c` is also a sketch of the eventual presenter: the same SDL window +
texture upload the GPU backend will hang off the `ruxen_canvas_*` ABI.

## Buttons & styling (Skia primitives)

`buttons.rx` showcases the building blocks quiver (L2) will compose into real
widgets: filled and outlined rounded-rect buttons, **per-corner radii** (pills
and top-rounded tabs), circles with stroked rings, lines, and
horizontally-centered antialiased Skia text (centering uses `measure_text` +
`text_height`, which report Skia's true metrics). Renders to `out.ppm`.

```bash
mkdir -p /tmp/canvas_buttons/src && cd /tmp/canvas_buttons
cat > Ruxen.toml <<'TOML'
[package]
name = "canvas_buttons"
version = "0.1.0"
edition = "2026"

[dependencies]
canvas = { path = "/path/to/canvas" }
TOML
cp /path/to/canvas/examples/buttons.rx src/main.rx
RUXEN_ALLOW_EXTERNAL_PATH=1 ruxen run        # -> /tmp/canvas_buttons/out.ppm
convert out.ppm buttons.png                  # or: ./view out.ppm
```

Requires the Skia backend for the rounded/curved shapes — run
`runtime/fetch_skia.sh` first (see `../docs/SKIA.md`); the example prints which
backend it used.

## Interactive counter (real window, live input)

`counter.rx` is the full loop quiver will automate: open → show → poll
events → redraw → present → pace. Click the button to increment, close
the window to exit; with no display it renders ~60 headless frames and
exits.

```bash
cp /path/to/canvas/examples/counter.rx /tmp/counter_app/src/main.rx
cd /tmp/counter_app && RUXEN_ALLOW_EXTERNAL_PATH=1 ruxen run
```

## Standalone `*_verify.c` live proofs (manual, on a real desktop)

Some engine paths need a real window / GPU / display / human click and so CANNOT
run in the forked, headless test harness. Each is a self-contained C file (dlopen,
no link deps) that replicates the shim's exact call sequence and prints `PASS` /
`SKIP`. They are MANUAL — compiled and run by a human on a desktop, never wired
into anything automated (the harness pins the headless contract instead).

```bash
cc -O2 -o metal_window_verify  examples/metal_window_verify.c   && ./metal_window_verify
cc -O2 -o window_mgmt_verify    examples/window_mgmt_verify.c   && ./window_mgmt_verify
cc -O2 -o file_dialog_verify    examples/file_dialog_verify.c   && ./file_dialog_verify
cc -O2 -framework CoreGraphics -o a11y_verify examples/a11y_verify.c && ./a11y_verify
```

`file_dialog_verify.c` drives a real macOS **NSOpenPanel** then **NSSavePanel**
through the objc runtime (the Phase-2 `Window.open_file_dialog` /
`save_file_dialog` path) — pick a file / a save location and it prints the chosen
paths. A modal needs a human click, so it is manual by nature; the automated bar is
the headless `Err` contract in `tests/file_dialog.rx` plus this file compiling.

`a11y_verify.c` (Phase-3 / Prod-hardening) brings up a live Metal window, builds the
engine's stored a11y nodes as **NSAccessibility CHILD elements**
(`Window.sync_a11y_children`), and ASSERTS each one round-trips its role + label.
It then attempts the external Accessibility-client round-trip
(`AXUIElementCreateApplication(getpid())`); if TCC consent / a live GUI session is
missing it prints a precise MANUAL-STEP and asserts the element-level proof — it
never fakes the OS-walk. Run with VoiceOver (Cmd-F5) to hear the children.

## Prod-hardening harnesses (link the real shim)

Unlike the `*_verify.c` above (self-contained dlopen mirrors), these link the
ACTUAL shim (`runtime/skia_shim.c` + `runtime/sdl_window.c`) so they exercise the
real C code paths. `scripts/check.sh` runs the soak (gated, short) + bench
(report-only) for you; run them directly with:

```bash
cc -O2 -o soak_verify         examples/soak_verify.c          runtime/skia_shim.c runtime/sdl_window.c -ldl && ./soak_verify
cc -O2 -o error_inject_verify examples/error_inject_verify.c  runtime/skia_shim.c runtime/sdl_window.c -ldl && ./error_inject_verify
cc -O2 -o bench_frame         examples/bench_frame.c          runtime/skia_shim.c runtime/sdl_window.c -ldl && ./bench_frame
```

- `soak_verify.c` — sustained ≥10k-frame leak soak (RSS via mach `task_info`),
  asserts post-warmup growth < 5%. `SOAK_ITERS=N` tunes length.
- `error_inject_verify.c` — forces the missing-Skia path (via `RUXEN_CANVAS_SKIA`)
  and absurd-input rejection, asserting honest degradation (clean Err / no
  overflow-OOM), never a silent no-op.
- `bench_frame.c` — perf baseline (median/p95 frame walltime + shape-cache
  hit-rate) on raster + offscreen Metal; numbers recorded in `docs/PERF.md`
  (report-only, never gated). `BENCH_FRAMES=N` tunes length.
