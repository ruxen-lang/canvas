# ADR: Frame pacing / vsync seam — the engine timebase + paced present

Status: **Accepted — clock + paced-present seam implemented; callback/timer system out of scope**
Date: 2026-06-10
Relates to: `docs/GPU.md` (the Metal/GL/raster backend ladder this paces on top of).

## Context

Today the examples sleep a blind `16ms` between frames (`Window#sleep_ms(16)`),
which is wrong three ways: it ignores how long the frame's own work took (so the
real cadence is `work + 16ms`, not 16ms), it has no relationship to the display's
actual refresh interval, and it gives L2 (`quiver`) no monotonic timebase to
drive animation against. quiver's future animation ticker (roadmap F4) needs a
real clock and a real "wait until the next frame boundary" primitive, not a fixed
sleep.

The constraints that bound this decision are the same ones the rest of the engine
lives under:

- **The three backends pace differently, and two of them already block on the
  hardware.** On the on-screen Metal path, acquiring the CAMetalLayer's next
  drawable (`[layer nextDrawable]`, in `begin_frame` → `rx_metal_window_begin_frame`)
  blocks when the swapchain is full — **that is vsync**, enforced by the OS
  compositor, and it already happens on this codebase. On the GL window path,
  `SDL_GL_SetSwapInterval(1)` (already resolved in `sdl_window.c`) makes the buffer
  swap block on vblank. Only the **raster / headless** path has no hardware clock
  to block on, so it is the one that needs a software-clocked sleep to a target
  interval.
- **No new dependency.** `skia_shim.c` already `#include <time.h>` and already
  calls `nanosleep` for `sleep_ms`. A `clock_gettime(CLOCK_MONOTONIC)` timebase
  costs zero new link/dlopen surface.
- **The test harness forks per case and runs headless.** A pacing primitive must
  be pin-able headless on its arithmetic (clock monotonicity, "remaining = target
  − now" math), never requiring a live display or a real vblank to verify.
- **No abstraction for hypothetical futures** (repo rule 4). The brief is explicit:
  build the *timebase + paced present*, NOT a callback/timer/scheduler system.
  quiver owns the animation loop; the engine owns the clock and the frame-boundary
  wait.

## Decision

Add exactly three things, all additive behind the existing ABI:

### 1. A monotonic nanosecond clock — `ruxen_canvas_ticks_ns`

Bound in `skia_shim.c` over `clock_gettime(CLOCK_MONOTONIC, …)`, returning an
`int64_t` nanosecond count from an unspecified-but-fixed epoch (process start /
boot — only *differences* are meaningful, which is all a frame clock needs). It
takes the host handle for ABI uniformity but ignores it (the clock is process-wide,
exactly like `sleep_ms`). `CLOCK_MONOTONIC` is the right choice over wall-clock:
it never jumps backward on NTP adjustment or DST, so a frame delta is always
`>= 0`.

Surfaced as:
- `Window#ticks_ns -> Int` — raw nanoseconds.
- `Window#ticks_ms -> Int` — `ticks_ns / 1_000_000`, the convenience most loops
  want.

**Micro-call (recorded here):** nanoseconds is the stored unit, not milliseconds.
A frame at 120 Hz is 8.33ms — sub-millisecond precision matters for pacing math,
and `int64` ns does not overflow for ~292 years. `ticks_ms` is a pure divide on
top, never the storage unit.

### 2. A paced-present wait — `Window#wait_frame(target_ns)`

The frame-boundary primitive. The caller captures `start = ticks_ns` at the top
of a frame, does its work + present, then calls `wait_frame(start + interval_ns)`.
`wait_frame` computes `remaining = target_ns − ticks_ns` and, if positive, sleeps
exactly that long (`ruxen_canvas_wait_until_ns` over `nanosleep`). If the frame
already overran the target (`remaining <= 0`) it returns immediately — pacing
never *adds* latency to an already-late frame; it only fills the slack of an
early one.

**Micro-call:** `wait_frame` takes an ABSOLUTE target tick, not a duration. An
absolute target makes the cadence self-correcting: the caller advances
`target += interval` each frame, so a frame that finishes 2ms early and one that
finishes 1ms late both converge back onto the same grid, with no drift
accumulation. A duration-based `wait(remaining_ms)` would have to recompute the
remainder caller-side and would drift by the cost of the call itself.

**Relationship to hardware vsync (documented, not re-implemented):** on the Metal
and GL on-screen paths the present already blocks on the swapchain / vblank, so
`wait_frame` is a *no-op-by-design* there (the frame is already paced; `remaining`
will be `<= 0` because the blocking present consumed the interval). `wait_frame`
is the software clock for the **raster / headless** path, and the uniform seam L2
calls regardless of backend — it does the right thing on each. We do NOT try to
defeat or double-count the hardware block; the contract is "this call guarantees
the frame is no *shorter* than the target interval," which composes correctly with
a present that already enforces a minimum.

### 3. Display refresh rate — `Window#refresh_rate`

`SDL_GetDesktopDisplayMode` (a new OPTIONAL SDL symbol in `sdl_window.c`) reads the
display mode's `refresh_rate` (Hz) for display 0. Returns `Ok(hz)` when SDL + a
display are available, `Err` when headless / no SDL (the harness, CI). This lets
an app pick its target interval (`1e9 / hz` ns) from the real display instead of
hardcoding 60. It is a *hint*, not a pacing mechanism — the actual pacing is
`wait_frame`; `refresh_rate` only tells you what interval to aim at.

**Micro-call:** `refresh_rate` returns `Result[Int, String]`, not a sentinel `0`.
A refresh rate of 0 is the SDL "unspecified" value, and conflating "unknown" with
a valid-looking number invites a divide-by-zero when computing the interval. An
explicit `Err` forces the caller to choose a fallback (typically 60).

## Options considered

- **A: keep the blind `sleep_ms(16)`.** Rejected — it is the bug. No timebase, no
  work-aware cadence, wrong on any non-60Hz display.
- **B (chosen): monotonic clock + absolute-target `wait_frame` + refresh-rate hint.**
  Minimal, headless-pinnable, composes with the hardware vsync the GPU paths
  already have.
- **C: a callback/timer/scheduler subsystem in the engine.** Explicitly out of
  scope — that is quiver's animation loop to own. The engine must not grow a
  scheduler; it provides the clock and the wait, and L2 builds the loop. (Repo
  rule 4: build the thing today's code needs, not a framework for a hypothetical.)
- **SDL_GetTicks vs clock_gettime for the clock.** `SDL_GetTicks` is millisecond
  resolution and requires SDL to be loaded (it is unavailable headless / when SDL
  isn't fetched). `clock_gettime(CLOCK_MONOTONIC)` is nanosecond, always available
  on the POSIX hosts canvas targets, and keeps the clock in the always-present
  `skia_shim.c` tier rather than the optional SDL tier — so the timebase works
  even on a host with no SDL at all. Chosen.

## Consequences

Positive:
- L2 gets a real monotonic timebase (`ticks_ns`/`ticks_ms`) and a real
  frame-boundary wait (`wait_frame`) to build its animation ticker on, with the
  refresh rate available to size the interval.
- The clock + wait are pin-able headless on pure arithmetic (monotonic
  non-decreasing; `wait_frame(now)` returns immediately; `wait_frame(now + dt)`
  consumes `>= dt`), so the harness verifies the contract with no display.
- Nothing about the existing draw / present / event ABI changes; this is three
  new methods.

Costs / notes:
- `wait_frame`'s sleep is `nanosleep`, which guarantees a *minimum* sleep, not an
  exact one (the OS may over-sleep by a scheduler tick). For a 2D UI engine that
  is correct: we never want to present *before* the boundary; a few hundred µs of
  over-sleep is invisible. Sub-microsecond busy-wait precision is a game-engine
  concern, not ours, and is not built.
- `refresh_rate` is display-0 only. Multi-monitor / per-window refresh is a
  Phase-1.5 concern (filed in ROADMAP), not needed for the single-window animation
  timebase this unblocks.
