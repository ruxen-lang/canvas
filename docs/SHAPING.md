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
- **Shaped paragraphs** — `Canvas#draw_paragraph_shaped` /
  `#measure_paragraph_shaped`: the greedy whitespace word-wrap measures **and**
  renders each wrapped line through the shaped glyph path, so wrapping +
  alignment use kerned/ligature widths. The shaper (HarfBuzz face/font + Skia
  typeface) is built once per paragraph; each line's width comes from shaping its
  byte sub-range (`hb_buffer_add_utf8` item-offset/length).
  `tests/canvas_paragraph_shaped.rx` proves it: a `"ffi"` paragraph line is
  strictly narrower than the naive per-char sum `f+f+i` (the ligature), and the
  paragraph line width equals the shaped run width (wrap measured shaped, not
  naive).
- `examples/shape_kerning_verify.c` — a standalone, committed proof
  (`cc -O2 -o m examples/shape_kerning_verify.c && ./m` → `PASS`; AV 1.8px
  tighter, ffi 1.8px tighter, textblob inked).

**Deferred (the follow-ups to "full" international text):**
- **Line-break + grapheme segmentation (ICU) — LANDED (Phase 3).** No new
  dependency: the system `libicucore` (macOS) is dlopen'd (bare `ubrk_*` names, a
  suffix-scan forward-compat hedge). `Canvas#grapheme_count` /
  `#grapheme_boundary_at` expose grapheme boundaries (UTF-8 byte offsets) for L2's
  caret/selection; `Canvas#draw_paragraph_icu` wraps at ICU line-break
  opportunities + font fallback, so CJK (no spaces) wraps at character boundaries.
  See `docs/decisions/text-fallback.md`. The SHAPED-paragraph path
  (`draw_paragraph_shaped`) still uses greedy-whitespace wrap — the ICU-wrap path
  is the separate `draw_paragraph_icu` entry; unifying them is a follow-up.
- **Bidi (ICU ubidi) — LANDED single-line (Phase 3).** `Canvas#draw_text_bidi`
  reorders a mixed-direction line into visual order via `ubidi` (present in
  libicucore) and shapes each directional run with its resolved direction. See
  `docs/decisions/text-fallback.md`. **Remainder:** per-line reordering of a WRAPPED
  bidi paragraph and an explicit RTL base direction (the single-line LTR-base case
  is done; we never render visually-wrong RTL).
- **Font fallback (CJK / emoji) — LANDED (Phase 3).** `Canvas#draw_text_fallback`
  itemizes a string into runs by font coverage and renders each uncovered run
  (CJK / emoji) with a system-font-manager-matched typeface
  (`sk_fontmgr_match_family_style_character`) via Skia's direct glyph mapping — so
  non-Latin text shows real glyphs, not tofu. No new dependency (the fontmgr is in
  `libSkiaSharp`); no font file needed for fallback runs. See
  `docs/decisions/text-fallback.md`. Complex-script SHAPING (Indic/Arabic) inside a
  fallback font still needs the font file for HarfBuzz — the documented remainder.
- **Per-line multi-run shaping — LANDED (Phase 3).**
  `Canvas#draw_text_shaped_multi` itemizes a line into runs by font coverage:
  base-font runs are HarfBuzz-shaped (kerning/ligatures), uncovered runs (CJK/emoji)
  use the system fallback. So a mixed Latin+CJK line shapes the Latin AND renders
  the CJK. See `docs/decisions/text-fallback.md`. Integrating this into the
  word-wrapped `draw_paragraph_shaped` layout (so wrapped shaped lines also split
  per-font) is a follow-up; per-run SCRIPT/direction boundary splitting beyond
  coverage is the bidi item.
- **Family → file resolution.** Today a shaped run takes a font **file path**;
  resolving a family name to a file (via the system font manager) is a small
  follow-up.
- **Font/run caching — LANDED (Phase 3).** A bounded, process-wide shaped-run
  MEASURE cache (keyed by font+size+direction × run-bytes hash) skips HarfBuzz when
  a label is re-measured across frames; `Canvas#shape_cache_hits` proves the hit.
  A fallback-typeface cache keyed by Unicode block landed with font fallback. See
  `docs/decisions/text-fallback.md`. (Caching the per-call HarfBuzz face/font + Skia
  typeface objects themselves — vs the measured widths — is a further follow-up.)

## Updating the pinned HarfBuzz version

Edit `HARFBUZZ_VER` + the per-package nupkg SHA + the per-member dylib SHA in
`runtime/fetch_skia.sh` together (same procedure as Skia). Get the checksums by
downloading the nupkg and `shasum -a 256` the package and the extracted
`runtimes/osx/native/libHarfBuzzSharp.dylib`.
