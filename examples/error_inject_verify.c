/* error_inject_verify.c — error-injection proof of honest degradation
 * (prod-hardening, Item 3). Links the REAL shim and forces two failure classes,
 * asserting the shim degrades cleanly instead of crashing or drawing wrong:
 *
 *  (a) MISSING SKIA DYLIB — set RUXEN_CANVAS_SKIA to a nonexistent path BEFORE the
 *      lazy loader runs, so rx_skia()->available stays 0 (the production fallback).
 *      Assert: skia_available == 0; clear/draw_rect STILL produce byte-exact opaque
 *      fills via the software raster (the contract L2 relies on); a Skia-ONLY op
 *      (save_layer_blur) returns the NO_SKIA error code, never a silent no-op.
 *
 *  (c) ABSURD INPUT — host_new with zero / negative / gigantic (overflow-bait)
 *      dimensions must be REJECTED (return 0), never OOM-crash or integer-overflow
 *      a wrap-around allocation. (The shim caps each axis at 16384 BEFORE the
 *      width*height multiply, so 16384*16384*4 = 1 GiB fits size_t — no wrap.)
 *
 * (The GPU ladder forced-failure — (b) — stays a Ruxen pin in tests/gpu_backend.rx:
 * gpu_available?/gpu_active? are total and fall back cleanly. It needs no C driver.)
 *
 * BUILD:
 *   cc -O2 -o error_inject_verify examples/error_inject_verify.c \
 *      runtime/skia_shim.c runtime/sdl_window.c -ldl
 *   ./error_inject_verify     # PASS / FAIL; exits nonzero on any failed assertion.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

char *ruxen_string_from(const char *s) {
    if (!s) s = "";
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

extern int64_t ruxen_canvas_host_new(int64_t w, int64_t h);
extern void    ruxen_canvas_host_drop(int64_t self);
extern int64_t ruxen_canvas_begin_frame(int64_t self);
extern int64_t ruxen_canvas_end_frame(int64_t self);
extern int64_t ruxen_canvas_clear(int64_t self, int64_t r, int64_t g, int64_t b, int64_t a);
extern int64_t ruxen_canvas_draw_rect(int64_t self, double x, double y, double w, double h,
                                      int64_t r, int64_t g, int64_t b, int64_t a);
extern int64_t ruxen_canvas_read_pixel(int64_t self, int64_t x, int64_t y);
extern int64_t ruxen_canvas_save_layer_blur(int64_t self, double sigma);
extern int64_t ruxen_canvas_skia_available(int64_t self);

#define RXC_OK          0
#define RXC_ERR_NO_SKIA 8

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  ok  : %s\n", msg); } \
    else      { printf("  FAIL: %s\n", msg); failures++; } } while (0)

int main(void) {
    /* (a) Force the missing-dylib path: point the loader at a nonexistent file
     * BEFORE any shim call so the lazy rx_skia() singleton resolves to "absent".
     * This is the production override seam (RUXEN_CANVAS_SKIA), reused as the
     * test-only fault injector — documented, default-off. */
    setenv("RUXEN_CANVAS_SKIA", "/nonexistent/path/libSkiaSharp.dylib", 1);
    /* Also blank the cache + HOME-derived fallbacks so the loader can't find the
     * real cached lib and quietly succeed (we WANT the absent path here). */
    setenv("RUXEN_CANVAS_CACHE", "/nonexistent/cache", 1);
    setenv("HOME", "/nonexistent/home", 1);

    int64_t h = ruxen_canvas_host_new(8, 4);
    if (!h) { printf("FAIL: host_new(8,4) returned 0\n"); return 1; }

    printf("(a) missing-dylib fallback:\n");
    CHECK(ruxen_canvas_skia_available(h) == 0,
          "skia_available == 0 with the loader pointed at a nonexistent dylib");

    /* Software-raster fill must still be byte-exact opaque. */
    ruxen_canvas_begin_frame(h);
    ruxen_canvas_clear(h, 0x12, 0x34, 0x56, 0xFF);
    ruxen_canvas_draw_rect(h, 2, 1, 3, 2, 0xAB, 0xCD, 0xEF, 0xFF);
    ruxen_canvas_end_frame(h);
    int64_t bg = ruxen_canvas_read_pixel(h, 0, 0);   /* clear color */
    int64_t fg = ruxen_canvas_read_pixel(h, 3, 1);   /* inside the rect */
    CHECK(bg == (int64_t)0xFF123456, "clear fills exact opaque color in software fallback");
    CHECK(fg == (int64_t)0xFFABCDEF, "draw_rect fills exact opaque color in software fallback");

    /* A Skia-ONLY op must Err NO_SKIA (not crash, not silently succeed). The
     * layer family returns the save-count (>=0) on success and a NEGATIVE -RXC_ERR_*
     * on failure (the Ruxen wrapper Canvas#layer_result negates it back), so the
     * absent-Skia contract is -RXC_ERR_NO_SKIA == -8, NOT +8. */
    ruxen_canvas_begin_frame(h);
    int64_t blur_rc = ruxen_canvas_save_layer_blur(h, 3.0);
    ruxen_canvas_end_frame(h);
    CHECK(blur_rc == -RXC_ERR_NO_SKIA,
          "save_layer_blur returns -NO_SKIA (-8) when Skia is absent — clean Err, no silent no-op");
    ruxen_canvas_host_drop(h);

    printf("(c) absurd-input rejection (no overflow / OOM crash):\n");
    CHECK(ruxen_canvas_host_new(0, 10)        == 0, "host_new(0,10) rejected");
    CHECK(ruxen_canvas_host_new(10, 0)        == 0, "host_new(10,0) rejected");
    CHECK(ruxen_canvas_host_new(-1, 10)       == 0, "host_new(-1,10) rejected");
    CHECK(ruxen_canvas_host_new(10, -5)       == 0, "host_new(10,-5) rejected");
    CHECK(ruxen_canvas_host_new(16385, 10)    == 0, "host_new(16385,10) over-cap rejected");
    CHECK(ruxen_canvas_host_new(100000, 100000) == 0, "host_new(1e5,1e5) gigantic rejected (no OOM)");
    /* The classic overflow bait: dims whose product wraps a 32-bit size but is
     * caught by the per-axis cap before the multiply. */
    CHECK(ruxen_canvas_host_new(65536, 65536) == 0, "host_new(65536,65536) overflow-bait rejected");
    /* The boundary that IS allowed must still succeed (the cap isn't off-by-one). */
    int64_t big = ruxen_canvas_host_new(16384, 16384);
    CHECK(big != 0, "host_new(16384,16384) at the cap succeeds (1 GiB, fits size_t)");
    if (big) ruxen_canvas_host_drop(big);

    if (failures == 0) { printf("\nPASS: honest degradation under missing-dylib + absurd-input\n"); return 0; }
    printf("\nFAIL: %d assertion(s) failed\n", failures);
    return 1;
}
