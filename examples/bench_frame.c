/* bench_frame.c — perf BASELINE for a representative canvas frame (prod-hardening,
 * Item 4). RECORDED, NOT GATED: it prints median/p95 frame walltime + the text-cache
 * hit-rate so docs/PERF.md has a number to diff against later. It does NOT assert a
 * threshold — perf assertions on a shared/loaded machine flake (and this runs under
 * whatever else the host is doing), so the discipline is "record + compare", not
 * "fail the build on a slow run". See docs/PERF.md.
 *
 * The representative frame (the brief): clear + 100 draw_rect + 20 text runs
 * (10 plain draw_text + 10 shaped draw_text_shaped) + 1 image blit (an offscreen
 * snapshot). Measured over 1000 frames on the headless raster backend, and again on
 * an offscreen GPU (Metal) surface if one comes up on this host.
 *
 * BUILD (links the real shim):
 *   cc -O2 -o bench_frame examples/bench_frame.c runtime/skia_shim.c \
 *      runtime/sdl_window.c -ldl
 *   ./bench_frame                 # 1000 frames (BENCH_FRAMES=N to override)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

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
extern int64_t ruxen_canvas_draw_text(int64_t self, int64_t text, double x, double y,
                                      int64_t r, int64_t g, int64_t b, int64_t a);
extern int64_t ruxen_canvas_draw_text_shaped_multi(int64_t self, int64_t text, double x, double y,
                                             double size, int64_t font_path, int64_t dir, int64_t argb);
extern int64_t ruxen_canvas_measure_text_shaped_multi(int64_t self, int64_t text, double size,
                                                int64_t font_path, int64_t dir);
extern int64_t ruxen_canvas_host_snapshot(int64_t self);
extern int64_t ruxen_canvas_draw_image(int64_t self, int64_t img, double x, double y);
extern void    ruxen_canvas_image_drop(int64_t self);
extern int64_t ruxen_canvas_shape_cache_hits(int64_t self);
extern int64_t ruxen_canvas_skia_available(int64_t self);
extern int64_t ruxen_canvas_host_enable_gpu_offscreen(int64_t self);
extern int64_t ruxen_canvas_gpu_active(int64_t self);
extern int64_t ruxen_canvas_gpu_backend_kind(int64_t self);

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1.0e6;
}

static int cmp_d(const void *a, const void *b) {
    double da = *(const double *)a, db = *(const double *)b;
    return (da > db) - (da < db);
}

/* The same shaped label re-drawn each frame is the L2 steady-state case (a static
 * caption centered every frame) — so the shaped run cache should HIT after warm-up.
 * The plain text runs vary so they exercise the non-cached path. */
static int64_t s_shaped, s_fontpath, s_plain[10];

static int s_have_font = 0;

static void build_strings(void) {
    s_shaped = (int64_t)ruxen_string_from("Steady Caption AVTo");
    /* HarfBuzz shaping needs a real font FILE (an empty path is BAD_ARGS); use the
     * same Arial the shaping pins use, so the shaped path + run cache actually run.
     * If it's absent the bench still measures the rest of the frame (s_have_font 0
     * skips the shaped runs). */
    const char *arial = "/System/Library/Fonts/Supplemental/Arial.ttf";
    FILE *f = fopen(arial, "rb");
    if (f) { fclose(f); s_have_font = 1; s_fontpath = (int64_t)ruxen_string_from(arial); }
    else   { s_fontpath = (int64_t)ruxen_string_from(""); }
    char b[32];
    for (int i = 0; i < 10; i++) { snprintf(b, sizeof b, "row %d value", i); s_plain[i] = (int64_t)ruxen_string_from(b); }
}

/* One representative frame; `img` is a pre-made offscreen snapshot to blit. */
static void one_frame(int64_t host, int64_t img) {
    ruxen_canvas_begin_frame(host);
    ruxen_canvas_clear(host, 12, 18, 28, 255);
    for (int i = 0; i < 100; i++) {
        double x = (i * 7) % 300, y = (i * 11) % 200;
        ruxen_canvas_draw_rect(host, x, y, 18, 10, 60 + i % 180, 90, 200 - i % 150, 255);
    }
    for (int i = 0; i < 10; i++)
        ruxen_canvas_draw_text(host, s_plain[i], 6, 20 + i * 12, 230, 230, 230, 255);
    if (s_have_font) {
        for (int i = 0; i < 10; i++) {
            ruxen_canvas_measure_text_shaped_multi(host, s_shaped, 16.0, s_fontpath, 0);  /* cache-hit path */
            ruxen_canvas_draw_text_shaped_multi(host, s_shaped, 6, 150 + i * 6, 16.0, s_fontpath, 0, 0xFFE0E0E0u);
        }
    }
    if (img) ruxen_canvas_draw_image(host, img, 240, 160);
    ruxen_canvas_end_frame(host);
}

static void bench(const char *label, int64_t host, int64_t img, long frames) {
    double *t = (double *)malloc(sizeof(double) * frames);
    if (!t) return;
    /* warm-up (cache fill, lazy GPU/Skia init) — not measured. */
    for (int i = 0; i < 50; i++) one_frame(host, img);
    int64_t hits_before = ruxen_canvas_shape_cache_hits(host);

    for (long i = 0; i < frames; i++) {
        double a = now_ms();
        one_frame(host, img);
        t[i] = now_ms() - a;
    }
    int64_t hits_after = ruxen_canvas_shape_cache_hits(host);

    qsort(t, frames, sizeof(double), cmp_d);
    double med = t[frames / 2];
    double p95 = t[(long)(frames * 0.95)];
    double sum = 0; for (long i = 0; i < frames; i++) sum += t[i];
    /* Each frame issues 10 measure_text_shaped_multi + 10 draw_text_shaped_multi,
     * and BOTH route through the cached shaper measure (the draw measures to
     * position), so 20 cache lookups/frame. At steady state (same label) every
     * lookup is a hit -> ~100%. */
    long lookups = frames * 20;
    long hits = (long)(hits_after - hits_before);
    double hitrate = lookups ? 100.0 * hits / lookups : 0.0;

    printf("[%s] frames=%ld  median=%.4f ms  p95=%.4f ms  mean=%.4f ms  shape-cache hit-rate=%.1f%% (%ld/%ld)\n",
           label, frames, med, p95, sum / frames, hitrate, hits, lookups);
    free(t);
}

int main(void) {
    long frames = 1000;
    const char *env = getenv("BENCH_FRAMES");
    if (env && env[0]) { long v = atol(env); if (v > 0) frames = v; }

    build_strings();

    /* an offscreen snapshot to blit each frame (the image-blit part of the brief). */
    int64_t off = ruxen_canvas_host_new(48, 48);
    ruxen_canvas_begin_frame(off);
    ruxen_canvas_clear(off, 0, 0, 255, 255);
    ruxen_canvas_draw_rect(off, 4, 4, 40, 40, 255, 80, 0, 255);
    ruxen_canvas_end_frame(off);
    int64_t img = ruxen_canvas_host_snapshot(off);

    /* raster backend (the deterministic path; always available). */
    int64_t raster = ruxen_canvas_host_new(320, 240);
    printf("skia=%s\n", ruxen_canvas_skia_available(raster) ? "available" : "software-fallback");
    bench("raster", raster, img, frames);

    /* GPU offscreen (Metal) if it comes up on this host. */
    int64_t gpu = ruxen_canvas_host_new(320, 240);
    ruxen_canvas_host_enable_gpu_offscreen(gpu);
    if (ruxen_canvas_gpu_active(gpu)) {
        printf("gpu offscreen active (backend_kind=%lld)\n", (long long)ruxen_canvas_gpu_backend_kind(gpu));
        bench("gpu-offscreen", gpu, img, frames);
    } else {
        printf("[gpu-offscreen] not active on this host — raster-only baseline (this is fine)\n");
    }

    if (img) ruxen_canvas_image_drop(img);
    ruxen_canvas_host_drop(raster);
    ruxen_canvas_host_drop(gpu);
    ruxen_canvas_host_drop(off);
    return 0;
}
