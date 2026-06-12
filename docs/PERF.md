# canvas — performance baseline

**This is a RECORDED baseline, NOT a gated threshold.** Perf numbers on a shared,
multi-tenant developer machine flake (background load, thermal state, frequency
scaling), so this file is a number to *diff against later* — not an assertion that
fails the build. `scripts/check.sh` runs the bench in report-only mode. If a future
change moves these numbers materially, that is a signal to investigate, not an
automatic failure.

Re-record with:

```bash
cc -O2 -o /tmp/bench_frame examples/bench_frame.c runtime/skia_shim.c \
   runtime/sdl_window.c -ldl
/tmp/bench_frame                  # 1000 frames; BENCH_FRAMES=N to override
```

## The representative frame (the bench workload)

Per frame, on a 320×240 surface: `clear` + 100 `draw_rect` + 20 text runs
(10 plain `draw_text` + 10 shaped `draw_text_shaped_multi`, the same static label
each frame — the L2 "centered caption every frame" steady-state case) + 1 image
blit of a 48×48 offscreen snapshot. Measured over 1000 frames after a 50-frame
warm-up (cache fill + lazy backend init, not counted). The shaped runs use
`/System/Library/Fonts/Supplemental/Arial.ttf` (the same font the shaping pins use).

## Baseline — 2026-06-11

- **Host:** Apple M4, macOS 26.5 (build 25F71), arm64.
- **Skia:** `libSkiaSharp` 3.119.4 active (the fetched/dlopen'd backend).
- **Build:** `cc -O2` linking the real shim (`skia_shim.c` + `sdl_window.c`).

| Backend | median | p95 | mean | shape-cache hit-rate |
|---|---|---|---|---|
| raster (software-direct Skia) | ~2.2 ms | ~6.6 ms | ~2.6 ms | 100.0% (20000/20000) |
| gpu-offscreen (Metal, backend_kind=2) | ~3.5 ms | ~10.6 ms | ~4.4 ms | 100.0% (20000/20000) |

(Median/p95/mean are the spread across three back-to-back 1000-frame runs; the
tens-of-percent run-to-run variation on p95 is exactly why this is not gated.)

### Reading the numbers

- **Shape-cache hit-rate is 100% at steady state.** Each frame issues 10 shaped
  measures + 10 shaped draws, and both route through the cached shaper measure
  (the draw measures to position glyphs), so 20 cache lookups/frame; re-shaping the
  same label hits every time and skips HarfBuzz. This is the cache's job — it keeps
  the L2 "re-center a static caption every frame" pattern off the shaper hot path.
- **GPU-offscreen is SLOWER than raster here, and that is expected for this
  workload.** The offscreen Metal path rebuilds a per-frame `SkSurface` over a fresh
  render target and flush+submits each frame; for a small (320×240) frame with
  little fill, that fixed GPU-submit overhead dominates the tiny draw cost the GPU
  would otherwise win. The GPU pays off at large fills / large surfaces / real
  windowed present (vsync-bound), not a tiny offscreen readback loop. Recorded so
  the trade-off is visible, not to recommend GPU-offscreen for small frames.
- The numbers are wall-clock CPU-submit time, not presented frames — there is no
  vsync wait here (headless), so these are raw draw+submit costs.
