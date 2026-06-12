# Test fixtures

Small binary inputs the canvas pin suite reads (project-relative paths, so they
resolve identically on macOS and Linux under `ruxen test`).

- `red.png`, `twopix.png`, `halves.png` — tiny images for the `canvas_image` pins
  (decode + draw + sample).
- `LiberationSans.ttf` — a portable font for the SHAPING / BIDI pins
  (`canvas_shaping`, `canvas_bidi`, `canvas_paragraph_shaped`, `canvas_shape_cache`,
  `canvas_shaped_multi`). It replaced the previously-hardcoded macOS path
  `/System/Library/Fonts/Supplemental/Arial.ttf`, which does not exist on Linux —
  so those pins ran only on macOS before Phase 4. Liberation Sans has the coverage
  every shaping/bidi pin needs: the **ffi ligature** (the paragraph-shaped pin
  asserts shaped `"ffi"` is strictly narrower than naive `f+f+i`), **kerning**
  (the AV-tightening pin), and **Hebrew** (the bidi RTL-reorder pin). The CJK
  font-FALLBACK pin (`canvas_fallback`) does NOT use this file — it resolves CJK
  through Skia's system fontmgr, which needs a CJK system font on the host
  (PingFang on macOS; `fonts-noto-cjk` in the Linux verification environment).

  Liberation Sans is licensed under the **SIL Open Font License v1.1** (the
  Liberation fonts, by Red Hat / Google) — freely redistributable, so committing
  it as a test fixture is fine.
