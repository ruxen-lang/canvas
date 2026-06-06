# Architecture — the Ruxen GUI stack (and where `canvas` sits)

The stack mirrors Dart/Flutter's layering 1:1, delivered as **packages, not a
language change**. The Ruxen language (L0) stays general-purpose.

| Layer | Package | Flutter analogy | `unsafe`/FFI? |
|---|---|---|---|
| **L0 — Language** | ruxen core (unchanged) | Dart | — |
| **L1 — Engine** | **`canvas`** | `dart:ui` / the engine | **Yes — the only such layer** |
| **L2 — Framework** | [`quiver`](../../quiver) | the Flutter framework | No — 100% safe ruxen |
| **L3 — Apps** | your app | a Flutter app | No |

## The L1 invariant (load-bearing)

> **`canvas` is the only layer permitted to be `unsafe` / FFI /
> platform-specific.** `quiver` (L2) is 100% safe, platform-agnostic Ruxen.

This invariant:
- keeps the language unbloated (no GUI in L0),
- makes the framework portable (L2 has no platform code), and
- is where the no-GC ownership model pays off (deterministic widget/node
  teardown, no GC jank).

## Consistency model

**Draw-everything** (Flutter-style): L2 draws *all* widgets onto L1's `Canvas`
— no native controls — for pixel-identical output across platforms.

## Data flow (one frame)

```
SDL event pump ─► canvas Event stream ─► quiver dispatches to widget handlers
                                              │
                                  signal changes invalidate tracking scopes
                                              │
quiver paints dirty nodes ─► canvas Canvas (Skia) ─► Window surface ─► screen
```

`canvas` owns the lower half (SDL + Skia); `quiver` owns the upper half
(signals, layout, paint/diff, dispatch). The boundary is exactly the `Window` /
`Event` / `Canvas` surface this package exposes.

## Decomposition of the whole program

Each its own spec → plan → build:

1. **L1 engine (`canvas`)** — Skia vendoring + build/link + the C shim + SDL
   windowing/input + the minimal canvas FFI + the incremental-FFI pattern.
2. **L2 core (`quiver`)** — signal arena runtime + `Copy` handles + the
   Ruby-block DSL + layout + paint/diff + event dispatch.
3. **Widget library** — buttons, text, lists, inputs, containers, …
4. **Text / i18n / accessibility** — Skia paragraph/HarfBuzz/ICU; platform a11y.
5. **Platform matrix** — macOS/Windows/Linux → Android/iOS → web (WASM + canvas).
6. **Packaging / distribution** — `.app`/`.apk`/`.ipa`/`.msi`, permissions, lifecycle.

The first implementation plan is a thin vertical through **#1 + #2 only** (the
counter app). The rest are future cycles.
