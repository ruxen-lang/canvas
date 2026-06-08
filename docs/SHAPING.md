# Text shaping in canvas

Proper text shaping — **kerning, ligatures, and (with direction) RTL / complex
scripts** — on top of the active Skia backend. This note explains the obtainable
architecture, what landed, and what is deferred.

## Why not SkParagraph / SkShaper

The fetched `libSkiaSharp` ships **no** `SkParagraph` and **no** `SkShaper`
(verified by `nm` — zero `sk_paragraph_*` / `sk_shaper_*` / shaper symbols).
Those modules need a separate HarfBuzzSharp + ICU build that this prebuilt does
not include. So we do **not** depend on them.

## The obtainable architecture: HarfBuzz-direct (shape) + Skia glyph-draw (render)

Two facts make a clean path possible without rebuilding Skia:

1. **`libSkiaSharp` DOES expose positioned-glyph rendering** — the `sk_textblob`
   builder (`sk_textblob_builder_alloc_run_pos`), `sk_canvas_draw_text_blob`, and
   `sk_typeface_create_from_file`. So Skia can render a glyph run we positioned.
2. **HarfBuzzSharp is fetchable** — `HarfBuzzSharp.NativeAssets.macOS` on NuGet
   (HarfBuzz's flat `hb_*` C API), a 2.5 MB universal dylib. `runtime/fetch_skia.sh`
   SHA-pins + installs it alongside `libSkiaSharp.dylib`; the shim `dlopen`s it
   (`rx_hb()`), exactly like Skia/SDL — no link-time dependency.

So the pipeline is:

```
text + font file
   │  HarfBuzz: hb_blob_create_from_file_or_fail → hb_face → hb_font
   │           hb_buffer_add_utf8 → (set direction) → hb_shape
   ▼  → glyph ids + positioned advances (kerning, ligatures, RTL)
positioned glyph run
   │  Skia: sk_typeface_create_from_file (SAME font file → matching glyph ids)
   │        sk_textblob_builder_alloc_run_pos → write (glyphId, x, y)
   │        sk_canvas_draw_text_blob
   ▼
rendered, shaped text
```

HarfBuzz advances are scaled by `hb_font_set_scale(font, size*64, size*64)`, so
they come back as 26.6 fixed-point — divide by 64 for device pixels.

## The bound surface

Following the incremental-FFI discipline (`docs/FFI.md`), `ruxen_canvas_*` /
`Canvas`:

- `Canvas#draw_text_shaped(text, x, y, size, font_path, direction, color)
  -> Result[Int, String]` — shape one run and draw it at baseline `(x, y)`;
  returns the shaped run's advance **width** (px) for layout/centering.
  `direction`: `0` auto / `1` LTR / `2` RTL.
- `Canvas#measure_text_shaped(text, size, font_path, direction) -> Int` — the
  shaped width without drawing.
- `Canvas#shaping_available? -> Bool` — Skia's glyph-render API **and** the
  HarfBuzz shaper both loaded.

Shaping is **Skia + HarfBuzz only**: a clean `Err` / `0` when either is absent,
and the non-shaped `draw_text` / `draw_paragraph` path is the fallback (never a
silent wrong render). The font is selected by **file path** this round.

## What landed vs deferred

**Landed (verified locally on real Skia + HarfBuzz):**
- A single shaped run with **kerning** and **ligatures** (`tests/canvas_shaping.rx`:
  `"AV"` shaped < `"A"` + `"V"`; `"ffi"` < 3× `"f"`), and direction LTR/RTL/auto.
- `examples/shape_kerning_verify.c` — a standalone, committed proof
  (`cc -O2 -o m examples/shape_kerning_verify.c && ./m` → `PASS`; AV 1.8px
  tighter, ffi 1.8px tighter, textblob inked).

**Deferred (the follow-ups to "full" international text):**
- **Bidi + line-break + grapheme segmentation (ICU).** Multi-directional
  paragraphs and Unicode line-breaking need ICU (large, build-heavy). This round
  shapes a single run with an explicit/auto direction; full bidi reordering of
  mixed-direction text is not yet done.
- **Multi-run paragraph integration.** Wiring shaped runs into
  `draw_paragraph`'s wrapping (so wrapped lines are shaped, and wrap points come
  from real line-break tables) is the natural next step.
- **Family → file resolution.** Today a shaped run takes a font **file path**;
  resolving a family name to a file (via the system font manager) is a small
  follow-up.
- **Font/run caching.** The current path builds the HarfBuzz face/font and Skia
  typeface per call; a per-(path,size) cache (like the family-font cache) is a
  performance follow-up, not a correctness one.

## Updating the pinned HarfBuzz version

Edit `HARFBUZZ_VER` + the per-package nupkg SHA + the per-member dylib SHA in
`runtime/fetch_skia.sh` together (same procedure as Skia). Get the checksums by
downloading the nupkg and `shasum -a 256` the package and the extracted
`runtimes/osx/native/libHarfBuzzSharp.dylib`.
