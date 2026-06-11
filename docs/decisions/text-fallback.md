# ADR: International text — font fallback, multi-run shaping, ICU segmentation, bidi

Status: **Accepted** — font fallback + color emoji + per-line multi-run + ICU
segmentation implemented on the pinned binaries with NO new native dependency;
bidi run-reorder landed for the LTR-base case, deep mixed-direction reordering
filed as the Phase-3 remainder.
Date: 2026-06-11
Relates to: `docs/SHAPING.md` (the HarfBuzz-direct + Skia-glyph-render pipeline
this builds on), `docs/ROADMAP.md` (Phase 3 — text i18n completion).

## Context

`docs/SHAPING.md` landed single-run shaping (kerning/ligatures, LTR/RTL/auto) and
shaped paragraphs, but the shaped path shapes a line as ONE HarfBuzz run in ONE
font selected by file path. That renders **tofu** (`.notdef` boxes) for any
codepoint the chosen font lacks — i.e. all CJK and emoji when the font is a Latin
face — and cannot wrap CJK (which has no spaces) because the wrap is
greedy-whitespace. The deferred-follow-ups list in SHAPING.md ("ICU bidi /
line-break / grapheme; per-line multi-run shaping + font fallback; font/run
caching") is exactly the gap to "full international text in L2".

The standing constraints that bound every decision here:

- **No new link dependency, fetch-and-dlopen only** (the canvas FFI rule). A new
  native library is acceptable ONLY if it is fetched + SHA-pinned like Skia /
  HarfBuzz, and only if a no-new-dep path genuinely does not exist.
- **Capability probes never lie.** A missing backend yields a clean `Err` /
  `false`, never tofu-pretending-to-be-text and never a silent no-op.
- **Additive, append-only.** New `ruxen_canvas_*` entry points + new loader
  symbols; the existing shaped/non-shaped paths stay byte-for-byte unchanged so
  every prior pin stays green.
- **The harness forks per case.** Pure-Skia/HarfBuzz/ICU work is fork-safe (no
  Cocoa/Metal/WindowServer), so the fallback + segmentation pins run IN the
  harness headless — unlike the windowed a11y / dialog paths.

## Symbol verification (the load-bearing claim — verify before building)

Ran on the pinned `~/.cache/ruxen-canvas/libSkiaSharp.dylib` (3.119.4) via
`nm -gU`, and on `/usr/lib/libicucore.A.dylib` via a `dlopen` + `dlsym` probe
(`nm` CANNOT see libicucore — it has no on-disk file on modern macOS, it lives
only in the dyld shared cache, so the symbol check MUST be a runtime dlsym probe,
not `nm`).

**Skia font fallback surface — ALL PRESENT:**

| Symbol | Role |
|---|---|
| `sk_fontmgr_ref_default` | the system font manager (Core Text on macOS) |
| `sk_fontmgr_match_family_style_character` | **the canonical per-character fallback** — give it a codepoint, it returns a typeface that covers it |
| `sk_fontmgr_match_family_style` | family+style → typeface |
| `sk_typeface_unichar_to_glyph` | coverage test — glyph 0 (`.notdef`) means "not covered" |
| `sk_typeface_get_family_name` | report which family the fallback picked |
| `sk_typeface_count_glyphs`, `sk_typeface_unref` | lifecycle |
| `sk_font_new_with_values`, `sk_font_set_typeface` | build a sized font over a fallback typeface |

**ICU on macOS — the no-new-dep finding.** `dlopen("/usr/lib/libicucore.A.dylib")`
SUCCEEDS, and these resolve **bare, with NO version suffix** on this host
(macOS 15 / Darwin 25.5):

```
ubrk_open  ubrk_setText  ubrk_first  ubrk_next  ubrk_following  ubrk_preceding
ubrk_close ubrk_isBoundary  ubrk_getRuleStatus
ubidi_open ubidi_setPara  ubidi_getDirection  ubidi_countRuns  ubidi_getVisualRun
ubidi_getLogicalRun  ubidi_close  ubidi_getParaLevel  ubidi_reorderVisual
u_strFromUTF8  u_strToUTF8
```

The suffixed variants (`ubrk_open_74` etc.) are NOT exported on this host — Apple
re-exports the unsuffixed names from libicucore. So **ICU segmentation + bidi is
reachable with NO new dependency.**

## Decisions

### 1. Font fallback: Skia fontmgr per-character match, not a hand-rolled font list

Use `sk_fontmgr_match_family_style_character(fontmgr, family, style, bcp47, n, codepoint)`
— it asks Core Text (the system) which installed font covers a codepoint, so CJK
resolves to PingFang/Hiragino and emoji to Apple Color Emoji **without us
shipping or naming any font**. The alternative (hard-coding a fallback chain of
family names per script) is brittle across OS versions and locales and is exactly
what the system font manager exists to avoid. Coverage of the PRIMARY font is
tested with `sk_typeface_unichar_to_glyph` (glyph 0 = miss); only on a miss do we
consult the fontmgr.

**Itemization:** decode the UTF-8 string to codepoints; for each, test the base
typeface's coverage; group MAXIMAL runs of consecutive codepoints with the same
resolved typeface (base when covered, else the fontmgr match — and consecutive
misses that resolve to the SAME fallback typeface coalesce into one run). Shape +
render each run through the existing HarfBuzz→textblob path with that run's
typeface, advancing the pen x across runs. This is the per-line multi-run upgrade
and the fallback upgrade in one mechanism.

### 2. Color emoji: rely on Skia's automatic color-glyph rendering

The fontmgr fallback reaches Apple Color Emoji (an sbix/COLR font). `libSkiaSharp`
renders color glyph tables automatically when the typeface is a color font and
the paint is a normal fill — no special "color textblob" API is needed (verified
by the multi-color pixel pin). We set `sk_font_set_edging` to antialias and draw
the textblob exactly as for Latin; Skia detects the color table. HONEST-SCOPE: if
a future host's libSkiaSharp lacked color-table support the pin would catch it as
a monochrome render and we would file it; on the pinned binary it works.

### 3. ICU segmentation: dlopen libicucore (no new dep), bare names, suffix-scan fallback

A new `RxICU` loader mirrors `RxHB`: `dlopen("/usr/lib/libicucore.A.dylib")`,
resolve the bare `ubrk_*` / `ubidi_*` / `u_str*` names. **Suffix-resolution
strategy (documented for portability):** try bare first; if a name misses, scan a
small suffix range (`_70` .. `_78`, covering ICU 70-78 ≈ macOS 13-16+) and take
the first hit. On THIS host the bare names resolve, so the scan is dormant; it is
the forward-compat hedge, not a runtime cost. If a host exported neither, ICU
features report unavailable cleanly and the greedy-whitespace wrap stays the
fallback (never a wrong wrap).

ICU is used for two things:
- **Line-break opportunities** — `ubrk_open(UBRK_LINE, ...)` over the paragraph,
  so wrapping respects Unicode line-break rules and CJK (no-space) text wraps at
  character boundaries instead of overflowing the column.
- **Grapheme boundaries** — `ubrk_open(UBRK_CHARACTER, ...)` exposed as a flat
  Int API (`grapheme_count`, `grapheme_boundary_at`) for L2's caret/selection, so
  an emoji+ZWJ sequence counts as ONE grapheme.

ICU wants UTF-16; we convert the UTF-8 input with `u_strFromUTF8` into a bounded
stack buffer (heap for long inputs), run the break iterator, and map UTF-16
offsets back to UTF-8 byte offsets for the wrap / API.

### 4. Caching: extend the family cache; add a bounded shaped-run LRU

- **Fallback typeface cache** — keyed by (codepoint-bucket, style), a small
  bounded table of resolved fallback typefaces, so a CJK paragraph does not call
  the (Core-Text-backed, slow) fontmgr per character. Bucketing by Unicode block
  (codepoint >> 8) is enough granularity: a run of Han characters shares a bucket
  and one cache slot.
- **Shaped-run cache** — keyed by (FNV-1a hash of the run bytes, typeface, size),
  a bounded LRU holding the shaped glyph ids + advances, flushed by a generation
  counter. Repeated shaping of the same label across frames then HITS. The pin
  asserts the HIT (a probe counter), not a wall-clock number — a perf number is
  not a stable test, a hit-count is.

### 5. Bidi: LTR-base run-reorder now, deep mixed-direction filed precisely

`ubidi_*` is present, so we run-split a line into directional runs
(`ubidi_setPara` + `ubidi_countRuns` + `ubidi_getVisualRun`) and reorder runs to
visual order before shaping each with its run direction. This is sound for an
LTR-base paragraph with embedded RTL (and vice-versa at the run level). Full
nested-level reordering with line-relative reordering of wrapped bidi lines is
the deep case; if it proves to need more than the run-reorder, it is filed as the
Phase-3 bidi remainder rather than faked. We do NOT render visually-wrong RTL.

## Consequences

- New `ruxen_canvas_*`: `draw_text_fallback` / `measure_text_fallback` /
  `last_fallback_family` (item 1+2), the multi-run shaped paragraph reuses the
  same itemizer (item 3), `grapheme_count` / `grapheme_boundary_at` /
  `icu_available` (item 4 ICU), plus the ICU-backed wrap behind the existing
  shaped-paragraph entry (gated by an ICU-available check, greedy fallback
  otherwise). All additive; the existing shaped/non-shaped pins are untouched.
- No new fetched library: Skia fontmgr + libicucore are already on the host.
  `runtime/fetch_skia.sh` is unchanged.
- The fallback/segmentation pins run headless in the forked harness (pure
  Skia/HarfBuzz/ICU, no windowing), so they are real in-harness pixel/contract
  pins, not example-only.
