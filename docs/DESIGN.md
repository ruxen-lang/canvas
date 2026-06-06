# canvas — Design (L1 engine)

**Status:** Design derived from the approved *Ruxen GUI* top-level design.
Only the **first vertical slice** (the minimal canvas FFI for the desktop
counter app) proceeds to implementation next.

## Goal

Provide the L1 **engine** for the Ruxen GUI stack: a thin, type-safe Ruxen
surface over proven C rendering/windowing libraries, exposed *upward only* to
the L2 framework ([`quiver`](../../quiver)). `canvas` builds **no renderer of
its own** — it binds Skia and SDL.

## Non-goals

- No widgets, layout, reactivity, or DSL — those live in L2 (`quiver`).
- No bespoke renderer (bind Skia).
- No native-widget wrapping. The stack is **draw-everything** (Flutter-style):
  L2 draws *all* widgets onto this canvas for pixel-identical output across
  platforms.

## Why this layer exists

The incumbents (Flutter, React Native, Electron) persist because the *language*
was never the hard part — **rendering consistency, text/accessibility, and
platform integration** are. So we bind the proven C libraries for exactly those
concerns and keep all of Ruxen's value-add (type safety, no-GC ownership,
reactive API) in the safe L2 layer above.

Ruxen is well positioned here:
- **Compiled-native** (Cranelift dev/JIT + LLVM release, cross-compilation) — no
  browser engine to ship.
- **Memory-safe, no GC** — ownership + borrow checking + drop elaboration give
  deterministic, allocation-light frames (the differentiator vs Dart's GC).
- **First-class C FFI** — the entire Ruxen stdlib is `lib "C"` bindings; this is
  the same bridge `canvas` uses to reach Skia/SDL.

## What it binds

| C library | Role |
|---|---|
| **SDL2 / SDL3** | windowing, input, event pump, GL/Metal context (desktop + mobile) |
| **Skia** | the 2D canvas (Flutter's renderer): GPU rendering + text shaping/i18n via bundled HarfBuzz/FreeType |

*Skia from day one* — it is literally Flutter's renderer and gives pixel-perfect
consistency plus world-class text.

## Surface exposed upward (only)

- `Window` / surface — created over SDL's context, backed by a Skia surface.
- An **event stream** — pointer/keyboard input, resize, lifecycle (`Event`).
- A `Canvas` — `begin_frame`/`end_frame`, `clear`, `draw_rect`, `draw_path`,
  `draw_text`, `draw_image`, transforms/clips, … (the growing subset).

L2 designs its `Canvas` API against the **full intended** surface from the
start; L1 fills in coverage over time via the incremental-FFI discipline
([`FFI.md`](FFI.md)).

## Ownership / teardown

A `Window` owns its Skia surface and SDL window; a `Canvas` borrows the
surface for a frame. Dropping a `Window` tears everything down deterministically
— no GC, no leaks. This is where the no-GC ownership model pays off as smooth,
jank-free frames.

## Risks

- **Skia build/link complexity** — C++ + large binary; the L1 build story is the
  main engineering risk. Mitigated by vendoring + the incremental shim (bind
  only what's used) and by deciding prebuilt-vs-source in the build spec
  ([`ROADMAP.md`](ROADMAP.md)).
