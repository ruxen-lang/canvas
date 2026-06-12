# ADR: Packaging — how an end-user app ships canvas's native dylibs

Status: **Accepted (prod-hardening, 2026-06-11).** The dev story (fetch +
`~/.cache` + dlopen) is unchanged; this ADR adds the PRODUCTION story: how a
shipped `.app` carries `libSkiaSharp` + `libHarfBuzzSharp` so it runs on a machine
that never ran `fetch_skia.sh`. Implemented by `scripts/bundle_libs.sh` (copy +
SHA-verify into the bundle) plus a small executable-relative entry in the shim's
dlopen search order.
Date: 2026-06-11
Relates to: `docs/SKIA.md` (fetch + dlopen rationale), `runtime/fetch_skia.sh`
(the SHA-pinned fetch), `runtime/skia_shim.c` (the dlopen loaders).

## Context

`canvas` links NOTHING native at build time. At runtime the shim `dlopen`s three
native libraries:

| Library | Size | Provisioning today | License |
|---|---|---|---|
| `libSkiaSharp.dylib` | ~15 MB | `fetch_skia.sh` → `~/.cache/ruxen-canvas`, SHA-pinned | BSD-3-Clause (Skia) + MIT (SkiaSharp glue) |
| `libHarfBuzzSharp.dylib` | ~2.6 MB | `fetch_skia.sh` → `~/.cache/ruxen-canvas`, SHA-pinned | MIT (HarfBuzz "Old MIT") |
| `libSDL2-2.0.0.dylib` | system | system / Homebrew, dlopen by name | zlib |

The shim's load search order (skia_shim.c) is, in order:

1. `$RUXEN_CANVAS_SKIA` / `$RUXEN_CANVAS_HARFBUZZ` — explicit absolute path.
2. `$RUXEN_CANVAS_CACHE/<basename>` — the cache dir override.
3. `$HOME/.cache/ruxen-canvas/<basename>` — the fetch script's default install.
4. the system loader (bare basename: DYLD path / Homebrew / ldconfig).

This is perfect for **development** (run the fetch script once, every app on the
machine finds the cached lib). It does NOT work for a **shipped app handed to a
user who never ran the fetch script** — none of (1)–(4) point at the dylibs the
developer would carry inside the `.app`. That gap is what this ADR closes.

## Decision

### 1. Ship the dylibs inside the app bundle's `Frameworks/` directory

The macOS-canonical place for a redistributed dylib is
`MyApp.app/Contents/Frameworks/`. `scripts/bundle_libs.sh` copies the two fetched,
SHA-verified dylibs there. This is the same layout an Avalonia / .NET app uses for
the very same `libSkiaSharp`.

### 2. The shim gains ONE executable-relative search entry (no env var needed)

So a bundled app "just works" with zero environment setup, the loader's search
order gains a new step BEFORE the cache/system fallbacks:

> **(1b) executable-relative:** `<dir-of-main-executable>/../Frameworks/<basename>`
> and `<dir-of-main-executable>/<basename>`.

The executable directory is found with `_NSGetExecutablePath` (macOS) — no
`@rpath` link magic, consistent with the dlopen-everything design (we never link
these, so we can't rely on the linker's `@rpath`). The order becomes:

1. `$RUXEN_CANVAS_SKIA` (explicit override — still wins, for ops/debugging).
1b. `<exe>/../Frameworks` then `<exe>/` (the bundled-app path — NEW).
2. `$RUXEN_CANVAS_CACHE`.
3. `$HOME/.cache/ruxen-canvas`.
4. the system loader.

Rationale for (1b) before the cache: a SHIPPED app must prefer the dylib it
carries (a known, SHA-verified version) over whatever a dev `~/.cache` happens to
hold, so the app's behavior doesn't depend on the end user's home directory. The
explicit `$RUXEN_CANVAS_SKIA` override still trumps everything (it is the operator
escape hatch + the error-injection seam).

### 3. SHA verification happens at BUILD/BUNDLE time, not load time

`bundle_libs.sh` verifies each dylib's sha256 against the pins in `fetch_skia.sh`
BEFORE copying it into the bundle, and refuses on mismatch. We deliberately do NOT
re-hash the ~15 MB dylib on every process start (a load-time cost on a hot path for
no real security gain — the bundle is already code-signed + notarized for
distribution, which is the real integrity boundary on macOS). The pins remain the
single source of truth (one table, in `fetch_skia.sh`); the bundle script reads
them so there is no second copy to drift.

### 4. Licensing — the redistributed binaries

Shipping these binaries triggers their license notice requirements. The bundle
script emits a `THIRD_PARTY_LICENSES` note alongside the copied dylibs naming each:

- **Skia** — BSD-3-Clause (Google). Requires the copyright + license text in
  distributed binary form.
- **SkiaSharp** glue — MIT (Microsoft / Mono).
- **HarfBuzz** — the "Old MIT" license. Requires the notice.
- **SDL2** (if also bundled rather than relied on as a system lib) — zlib.

The app vendor is responsible for surfacing these in their About/acknowledgements;
the bundle script makes the obligation explicit so it is not forgotten. canvas
itself is dual MIT/Apache-2.0 (`LICENSE-MIT` / `LICENSE-APACHE`).

## Consequences

- `scripts/bundle_libs.sh <App.app>` (or a plain dir): SHA-verifies the two fetched
  dylibs against the `fetch_skia.sh` pins, copies them into
  `<App.app>/Contents/Frameworks/`, and writes `THIRD_PARTY_LICENSES`. Idempotent.
- `runtime/skia_shim.c`: a new `rx_exe_relative_dir()` helper + an executable-
  relative probe inserted into both the Skia and HarfBuzz dlopen search orders.
  Backward-compatible: dev machines with a populated `~/.cache` are unaffected (the
  exe-relative probe simply misses and falls through to the cache).
- **Filed remainder (not this host):** Linux (`.so` next to the binary / an
  AppImage's `usr/lib`) and Windows (`.dll` next to the `.exe`) bundling reuse the
  same exe-relative probe — `_NSGetExecutablePath` is the macOS arm of a
  `rx_exe_relative_dir()` that gets `/proc/self/exe` (Linux) /
  `GetModuleFileName` (Windows) arms when those platforms land. SDL2 stays a
  system/Homebrew dependency for now (zlib-licensed, trivially bundlable later).

## Per-OS packaging notes (Phase 4 — desktop platform matrix)

The fetch + SHA-pin + dlopen model is now host-aware across macOS / Linux /
Windows (`fetch_skia.sh` selects the RID; the shim's basename arrays cover
`.dylib`/`.so`/`.dll`). Two OS-specific packaging facts a shipper must know:

- **Linux — `libSkiaSharp.so` has a SYSTEM runtime dependency on
  `libfontconfig.so.1`** (it uses fontconfig for system-font enumeration; the
  macOS universal dylib uses Core Text and has no such dep). Without fontconfig
  on the target, `dlopen(libSkiaSharp.so)` fails and Skia is silently inactive
  (`skia_available? false`, clean software-raster fallback). A shipped Linux app
  must therefore depend on the distro's fontconfig package (and, for non-Latin
  text, a font with the needed coverage — e.g. `fonts-noto-cjk` for CJK fallback;
  the engine resolves CJK/emoji via Skia's fontmgr, which reads the system font
  set). The Phase-4 Linux verification environment (`Dockerfile.linux-verify`,
  the `ubuntu-latest` CI job) installs `libfontconfig1` + `fonts-noto-cjk` for
  exactly this reason. `libfreetype6` arrives transitively.
- **Windows — EXPERIMENTAL.** The blobs are SHA-pinned (`win-x64` / `win-arm64`
  `libSkiaSharp.dll` / `libHarfBuzzSharp.dll`) and the loader has a
  LoadLibrary/GetProcAddress seam (`runtime/rx_dlopen.h`), but the path is
  compiles-untested-until-CI. The Windows DLLs additionally need the VC++ runtime
  present on the target (standard for the SkiaSharp build); the bundling story
  (DLLs beside the `.exe`, `GetModuleFileName` exe-relative probe) is the filed
  remainder above, to be finished when the Windows CI job is promoted past
  compile-only.
- **The exe-relative bundling probe stays macOS-only** for now
  (`_NSGetExecutablePath`); the Linux `/proc/self/exe` and Windows
  `GetModuleFileName` arms remain the filed remainder. On Linux/Windows today the
  blobs are found via `$RUXEN_CANVAS_CACHE` / `$HOME/.cache/ruxen-canvas` / the
  system loader path, not an exe-relative bundle dir.
