/*
 * skia_shim.c — C shim bridging Ruxen's `canvas` (L1) FFI to Skia + SDL.
 *
 * This is step (1) of the incremental-FFI discipline (see docs/FFI.md):
 * every capability the L1 `Canvas` exposes is first wrapped here as a flat
 * C function with a stable, Ruxen-friendly C-ABI signature, then declared in
 * a `lib "C"` block in src/lib.rx, then surfaced as a `Canvas` method.
 *
 * Skia's public surface is C++; this shim is the C/C++ boundary. Symbols are
 * prefixed `ruxen_canvas_*` so they never collide with the host runtime.
 *
 * Status: SCAFFOLD — no functions defined yet. The first vertical slice adds:
 *   ruxen_canvas_begin_frame / ruxen_canvas_end_frame
 *   ruxen_canvas_clear / ruxen_canvas_draw_rect / ruxen_canvas_draw_text
 *
 * Build/link of Skia itself (prebuilt-per-platform vs build-from-source) is
 * decided in the L1 build spec — see docs/ROADMAP.md.
 */
