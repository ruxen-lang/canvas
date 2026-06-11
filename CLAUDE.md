# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

`canvas` is **L1 (the engine)** of the Ruxen GUI stack: cross-platform windowing, input, and a Skia-backed 2D canvas, written in Ruxen (`.rx`) with a C shim. It is exposed *upward only* to the L2 framework (`quiver`, a sibling package). The Ruxen language repo lives at `../ruxen` (tutorials in `../ruxen/docs/tutorial/`).

## Commands

The toolchain is the `ruxen` CLI (`~/.ruxen/bin/ruxen`).

- `ruxen check` — type-check without codegen (fastest feedback loop)
- `ruxen build` — compile the library
- `ruxen test` — run all tests in `tests/**.rx`
- `ruxen test <stem>` — run a single test file by its FILE STEM (the basename
  without the `tests/` prefix or `.rx` suffix). E.g. `ruxen test accessibility`
  runs `tests/accessibility.rx`. **NOT** `ruxen test tests/accessibility.rx` —
  that path form matches nothing ("no test files found"). The filter is the stem.
- `ruxen fmt` — format `.rx` files in-place
- `ruxen explain E0001` — explain a compiler error code

### The pre-commit gate

- `scripts/check.sh` — **THE one-command local gate; run it before committing.**
  Stages, in order: (1) the C shim compiles warnings-clean (`-Wall -Wextra`, with
  only the load-bearing `objc_msgSend` cast idiom excepted); (2) `ruxen test` (the
  full pin suite); (3) the leak soak in short mode (`SOAK_ITERS`, default 2000 —
  gated: a detected leak fails); (4) the perf bench (REPORT-ONLY, never gated —
  shared-machine perf flakes, see `docs/PERF.md`). Exits nonzero on any gated
  failure. Tunables: `SOAK_ITERS=N`, `SKIP_BENCH=1`, `CHECK_CC=...`.

## Test framework

Tests are plain `.rx` files under `tests/` — no naming convention or attribute. Each file starts with `Tester.describe(name) do |t: &var Tester| ... end` (the explicit `|t: &var Tester|` binding is required), with `t.it(...) do ... end` cases and `t.expect(actual).to_eq(expected)` matchers. Each `it` runs in a forked child process. See `../ruxen/docs/tutorial/19-writing-and-running-tests.md`.

**Test discipline:** every newly-bound FFI method gets a pin test; no feature lands without a test.

## Architecture

The stack mirrors Flutter's layering (docs/ARCHITECTURE.md):

- **L0** — Ruxen language (unchanged, general-purpose)
- **L1** — `canvas` (this package): binds SDL2 (windowing/input/event pump) + Skia (2D rendering/text). **The only layer permitted to use `unsafe` / C FFI / platform-specific code.**
- **L2** — `quiver`: signals, layout, widgets — 100% safe Ruxen
- **L3** — apps

`canvas` builds no renderer of its own. The surface exposed upward is exactly: `Window` (owns SDL window + Skia surface, deterministic teardown on drop), `Event` (input/lifecycle stream), and `Canvas` (`begin_frame`/`end_frame`, `clear`, `draw_rect`, `draw_text`, … growing).

### Incremental-FFI discipline (docs/FFI.md) — the core workflow

Skia is vendored in full, but the *binding surface* grows method-by-method. To add a capability, follow the mechanical 4 steps:

1. **Wrap** the Skia/SDL call in `runtime/skia_shim.c` as a flat C-ABI function prefixed `ruxen_canvas_*`
2. **Declare** it in the `lib "C"` block in `src/lib.rx`
3. **Expose** it as a method on `Canvas` (or `Window`) in `src/lib.rx`
4. **Use** it from L2 (`quiver`)

Rules:
- A not-yet-bound method returns an `Err` with a clear message — never a silent no-op.
- All pointers cross the ABI as machine-word integers; `void` returns map to no return value.
- `skia_shim.c` is the single C/C++ boundary; its signatures are ABI — changing one is an ABI change.
- Symbols are always prefixed `ruxen_canvas_*`.

### Layout

- `src/lib.rx` — the entire Ruxen API surface (`Color`, `Rect`, `Window`, `Canvas`, `Event`)
- `runtime/skia_shim.c` — the C shim (only place that touches SDL/Skia directly)
- `Ruxen.toml` — `[system_libs]` declares the link names (`SDL2`, `skia`)
- `docs/` — DESIGN (why/boundary), ARCHITECTURE (L0–L3 model), FFI (4-step discipline), ROADMAP (milestones)

### Current milestone

Milestone 1 (docs/ROADMAP.md): the minimal counter-app slice — SDL window + Skia surface (`Window.open`), `begin_frame`/`end_frame`/`clear`/`draw_rect`/`draw_text`, pointer/resize/close events, a pin test per bound method. Mobile/web, paths/images/clips, and full text shaping are explicitly out of scope for this slice.

## Conventions

- Update `CHANGELOG.md` (Keep a Changelog format) for notable changes.
- Doc comments use `##`.

## Task tracking & keeping context current

- **Canonical task list: [`docs/ROADMAP.md`](docs/ROADMAP.md)** — milestones and
  what's in/out of each slice. This is the single place open canvas work is
  tracked; the workspace umbrella (`../CLAUDE.md`) links it but doesn't duplicate it.
- Every change updates docs in the same commit: tick/extend `docs/ROADMAP.md`,
  add a `CHANGELOG.md` entry, and — since this is the FFI layer — keep `docs/FFI.md`
  and the `lib "C"` surface in `src/lib.rx` in sync (a newly bound method gets a
  pin test, per the test discipline above).
- A compiler/toolchain quirk hit while binding Skia/SDL is a **language task**:
  add a `Q##` entry to `../ruxen/docs/dev/gui-stack-v1-issues.md` (repro + severity)
  and list it in `../ruxen/docs/TASKS.md` — don't just silently work around it.
- A `Stop` hook in `.claude/settings.json` reminds you of the above when a session
  touched source.
