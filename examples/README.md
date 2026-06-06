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

## Interactive counter (real window, live input)

`counter.rx` is the full loop quiver will automate: open → show → poll
events → redraw → present → pace. Click the button to increment, close
the window to exit; with no display it renders ~60 headless frames and
exits.

```bash
cp /path/to/canvas/examples/counter.rx /tmp/counter_app/src/main.rx
cd /tmp/counter_app && RUXEN_ALLOW_EXTERNAL_PATH=1 ruxen run
```
