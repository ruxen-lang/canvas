# Skia in canvas

`canvas` renders with **Skia** — the same 2D engine behind Chrome, Flutter,
and (via this exact prebuilt) Avalonia / Uno / .NET MAUI. This note explains
how Skia is brought in, the integration model, and how to advance the binding.

## How Skia is brought in: fetch, don't vendor, don't link

- **Binary:** `libSkiaSharp.so`, the prebuilt native library from the NuGet
  package `SkiaSharp.NativeAssets.Linux`. It exports Skia's flat **`sk_*` C
  API** (812 functions) — no C++ at the boundary.
- **Fetched, not committed:** `runtime/fetch_skia.sh` downloads a pinned
  package version, verifies the SHA-256 of both the package and the extracted
  `.so`, and installs it to `$HOME/.cache/ruxen-canvas/libSkiaSharp.so`
  (override the cache dir with `$RUXEN_CANVAS_CACHE`). The `.so` is ~11 MB and
  is never checked into this public repo — only the tiny C-API header
  (`runtime/skia/skia_capi.h`) is committed.
- **dlopen, not link:** `runtime/skia_shim.c` `dlopen()`s the library and
  resolves the symbol table at runtime, exactly as `runtime/sdl_window.c` does
  for SDL2. There is **no** `-lSkiaSharp`, no dev package, no link-time
  dependency. The shim stays plain C compiled by `gcc`.

```
runtime/
  fetch_skia.sh         # SHA-pinned download + extract -> ~/.cache/ruxen-canvas/
  skia/skia_capi.h      # committed: opaque types, enums, fn-ptr typedefs, RxSkia
  skia_shim.c           # dlopen loader (rx_skia) + draws into RxHost.pixels
```

### Bootstrapping a checkout

```bash
runtime/fetch_skia.sh          # once; idempotent, re-runs are a no-op
ruxen test                     # the shim finds the .so via the cache path
```

The shim looks for the library in this order:

1. `$RUXEN_CANVAS_SKIA` — explicit absolute path to a `libSkiaSharp.so`
2. `$RUXEN_CANVAS_CACHE/libSkiaSharp.so`
3. `$HOME/.cache/ruxen-canvas/libSkiaSharp.so` (where `fetch_skia.sh` installs)
4. `libSkiaSharp.so` via the system loader (`LD_LIBRARY_PATH`, ldconfig)

If none resolve, `rx_skia()->available` is 0 and the shim falls back to its
deterministic **software raster** backend — the build never breaks for lack of
the binary; Skia-only primitives return a clear `Err` instead.

## Integration model: Skia draws into our own framebuffer

`RxHost` owns a `width*height` buffer of `0xAARRGGBB` pixels. That packing is,
on a little-endian host, byte order **B,G,R,A** — i.e. Skia's
`kBGRA_8888_SkColorType`. So the shim wraps that exact buffer with
`sk_surface_new_raster_direct` and Skia rasterizes **straight into it**:

- `runtime/sdl_window.c` keeps presenting `RxHost.pixels` to the OS window
  unchanged — it never needs to know Skia exists (it reads only the leading
  `width/height/pixels` prefix of `RxHost`).
- `ruxen_canvas_read_pixel` keeps reading the same buffer, so pin tests observe
  Skia's output directly.
- Skia state (`sk_surface`/`sk_canvas`/typeface/font) lives in **trailing**
  `RxHost` fields, invisible to the SDL presenter.

The ABI of `skia_capi.h` (the `sk_imageinfo_t` layout, the `BGRA_8888`/`PREMUL`
enum values) is pinned by an empirical probe: a red rect drawn into a
raster-direct `0xAARRGGBB` buffer reads back byte-exact `0xFFFF0000`. If a
future Skia bump changes a value, that probe (and the pin tests) catch it.

## What this Skia covers

Everything a full 2D GUI framework needs: paths (béziers/arcs), gradients and
shaders, images and codecs, blurs / drop-shadows / image filters, clips and
transforms, pictures, runtime SkSL shaders, PDF/SVG export, and a GPU (Ganesh
GL/Vulkan/Metal) backend for later. Text: fonts, glyphs, simple text, and
measurement via the system font manager (fontconfig/freetype). The one thing
**not** in this C API is complex-script shaping + paragraph layout
(`skshaper`/`skparagraph`); that arrives later as a separate `HarfBuzzSharp`
native library when i18n text needs it.

## Advancing the binding (the 4-step discipline still holds)

To expose a new Skia capability (see also `docs/FFI.md`):

1. Add the `sk_*` function-pointer field to `RxSkia` in `skia/skia_capi.h` and
   resolve it in `rx_skia()`'s loader in `skia_shim.c`.
2. Wrap it in a flat `ruxen_canvas_*` C function in `skia_shim.c` (fall back to
   software, or return `Err` if Skia-only).
3. Declare + expose it as a `Canvas` method in `src/lib.rx`.
4. Use it from L2 (`quiver`), and add a pin test.

## Updating the pinned version

Edit `SKIA_VER`, `NUPKG_SHA256`, and the per-RID `.so` SHA in
`runtime/fetch_skia.sh` together. Get the checksums with:

```bash
curl -sL -o pkg.nupkg \
  https://api.nuget.org/v3-flatcontainer/skiasharp.nativeassets.linux/<VER>/skiasharp.nativeassets.linux.<VER>.nupkg
sha256sum pkg.nupkg
unzip -p pkg.nupkg runtimes/linux-x64/native/libSkiaSharp.so | sha256sum
```
