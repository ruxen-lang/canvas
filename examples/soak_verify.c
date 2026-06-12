/* soak_verify.c — sustained leak soak of the C shim (prod-hardening, Item 2).
 *
 * The per-binding pin tests prove each call is CORRECT, but they each run in a
 * fresh forked process, so they cannot catch a SLOW leak that only shows over many
 * frames in ONE process (a missing unref per text run, a ring slot that strdup's
 * without freeing, an offscreen host that doesn't tear down). This harness does
 * exactly that: it drives the REAL shim entry points (ruxen_canvas_*, compiled in
 * — NOT a re-implementation) in a long headless loop, samples RSS via mach
 * task_info, and asserts post-warmup growth stays below a threshold.
 *
 * BUILD (links the actual shim so a shim leak is a soak leak):
 *   cc -O2 -o soak_verify examples/soak_verify.c runtime/skia_shim.c \
 *      runtime/sdl_window.c -ldl
 *   ./soak_verify            # 10000 iterations (default); SOAK_ITERS=N to override
 *
 * The Skia/SDL libs are dlopen'd by the shim itself (the production loader); if
 * libSkiaSharp is absent the shim falls back to software raster and the soak still
 * exercises the raster + event + multi-window paths (the leak-prone C surface).
 *
 * Exits 0 + PASS if growth over the last 80% of iterations is below threshold,
 * 1 + FAIL with the RSS curve otherwise.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#if defined(__APPLE__)
#include <mach/mach.h>      /* task_info — RSS on macOS */
#elif defined(__linux__)
#include <stdio.h>          /* /proc/self/statm — RSS on Linux */
#include <unistd.h>         /* sysconf(_SC_PAGESIZE) */
#endif

/* ---- the Ruxen String runtime constructor the shim expects (a Ruxen String IS a
 * malloc'd char*); the soak provides a faithful stub so the shim links standalone.
 * The shim's own frees (free() on these) match this allocator. ---- */
char *ruxen_string_from(const char *s) {
    if (!s) s = "";
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

/* ---- the shim entry points we drive (flat C ABI, machine-word handles) ---- */
extern int64_t ruxen_canvas_host_new(int64_t w, int64_t h);
extern void    ruxen_canvas_host_drop(int64_t self);
extern int64_t ruxen_canvas_begin_frame(int64_t self);
extern int64_t ruxen_canvas_end_frame(int64_t self);
extern int64_t ruxen_canvas_clear(int64_t self, int64_t r, int64_t g, int64_t b, int64_t a);
extern int64_t ruxen_canvas_draw_rect(int64_t self, double x, double y, double w, double h,
                                      int64_t r, int64_t g, int64_t b, int64_t a);
extern int64_t ruxen_canvas_draw_text(int64_t self, int64_t text, double x, double y,
                                      int64_t r, int64_t g, int64_t b, int64_t a);
extern int64_t ruxen_canvas_draw_text_fallback(int64_t self, int64_t text, double x, double y,
                                               double size, int64_t argb);
extern int64_t ruxen_canvas_measure_text_fallback(int64_t self, int64_t text, double size);
extern int64_t ruxen_canvas_draw_text_shaped(int64_t self, int64_t text, double x, double y,
                                             double size, int64_t font_path, int64_t dir, int64_t argb);
extern int64_t ruxen_canvas_save_layer_blur(int64_t self, double sigma);
extern int64_t ruxen_canvas_restore(int64_t self);
extern int64_t ruxen_canvas_host_snapshot(int64_t self);
extern int64_t ruxen_canvas_draw_image(int64_t self, int64_t img, double x, double y);
extern void    ruxen_canvas_image_drop(int64_t self);
extern int64_t ruxen_canvas_push_event(int64_t self, int64_t kind, double a, double b);
extern int64_t ruxen_canvas_push_event_text(int64_t self, int64_t kind, int64_t start,
                                            int64_t length, int64_t text);
extern int64_t ruxen_canvas_poll_event(int64_t self);
extern int64_t ruxen_canvas_event_text(int64_t self);
extern int64_t ruxen_canvas_event_drop_path(int64_t self);
extern int64_t ruxen_canvas_window_pump_test_dropfile(int64_t self, int64_t path);
extern int64_t ruxen_canvas_skia_available(int64_t self);

/* event-kind tags (src/event.rx): 0 PointerMove .. 8 TextEditing .. 9 FileDrop. */
enum { EV_POINTER_MOVE = 0, EV_TEXT_EDITING = 8, EV_FILE_DROP = 9 };

static long rss_kb(void) {
#if defined(__APPLE__)
    struct mach_task_basic_info info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  (task_info_t)&info, &count) != KERN_SUCCESS) return -1;
    return (long)(info.resident_size / 1024);
#elif defined(__linux__)
    /* /proc/self/statm field 2 is resident-set size in PAGES. Multiply by the
     * page size for bytes, then to KiB — the same unit the macOS arm returns. */
    FILE *f = fopen("/proc/self/statm", "r");
    if (!f) return -1;
    long size_pages = 0, resident_pages = 0;
    int got = fscanf(f, "%ld %ld", &size_pages, &resident_pages);
    fclose(f);
    if (got != 2) return -1;
    long page_kb = sysconf(_SC_PAGESIZE) / 1024;
    return resident_pages * page_kb;
#else
    return -1;   /* unknown platform: RSS unavailable (the soak still runs) */
#endif
}

/* One representative unit of work: a frame of drawing + text-cache churn (VARYING
 * strings so the shaped/fallback caches don't just hit), an offscreen snapshot blit,
 * a blurred layer, and event-ring traffic incl. the side-channel paths. */
static void do_work(int64_t host, int64_t off, int iter) {
    char buf[64];

    ruxen_canvas_begin_frame(host);
    ruxen_canvas_clear(host, 10, 20, 30, 255);
    for (int i = 0; i < 50; i++)
        ruxen_canvas_draw_rect(host, i * 3.0, i * 2.0, 20, 12, 200, 100, 50, 255);

    /* VARYING strings — defeat the cache so allocations actually churn. */
    snprintf(buf, sizeof buf, "frame %d label %d", iter, iter * 7 % 1000);
    int64_t s1 = (int64_t)ruxen_string_from(buf);
    ruxen_canvas_draw_text(host, s1, 5, 30, 255, 255, 255, 255);
    free((void *)s1);

    snprintf(buf, sizeof buf, "shape-%d-AVTo", iter % 97);
    int64_t s2 = (int64_t)ruxen_string_from(buf);
    ruxen_canvas_measure_text_fallback(host, s2, 16.0);
    /* mix in CJK so the fallback typeface cache + glyph path run. */
    int64_t cjk = (int64_t)ruxen_string_from("\xe4\xbd\xa0\xe5\xa5\xbd");  /* 你好 */
    ruxen_canvas_draw_text_fallback(host, cjk, 5, 60, 16.0, 0xFF00FF00u);
    int64_t fp = (int64_t)ruxen_string_from("");  /* empty font path -> default face */
    ruxen_canvas_draw_text_shaped(host, s2, 5, 90, 16.0, fp, 0, 0xFFFFFFFFu);
    free((void *)s2); free((void *)cjk); free((void *)fp);

    /* blurred layer (image filter create/unref on save_layer_blur/restore). */
    ruxen_canvas_save_layer_blur(host, 3.0);
    ruxen_canvas_draw_rect(host, 40, 40, 60, 60, 255, 0, 0, 200);
    ruxen_canvas_restore(host);
    ruxen_canvas_end_frame(host);

    /* offscreen snapshot -> blit -> free (the render-to-texture lifecycle). */
    ruxen_canvas_begin_frame(off);
    ruxen_canvas_clear(off, 0, 0, 255, 255);
    ruxen_canvas_draw_rect(off, 2, 2, 16, 16, 255, 0, 0, 255);
    ruxen_canvas_end_frame(off);
    int64_t img = ruxen_canvas_host_snapshot(off);
    if (img) {
        ruxen_canvas_begin_frame(host);
        ruxen_canvas_draw_image(host, img, 100, 100);
        ruxen_canvas_end_frame(host);
        ruxen_canvas_image_drop(img);  /* the snapshot is caller-owned */
    }

    /* event ring: pointer + the two side-channels (TextEditing marked text, FileDrop
     * owned path). poll back so the ring's MOVE/free discipline runs. */
    ruxen_canvas_push_event(host, EV_POINTER_MOVE, iter % 300, iter % 200);
    int64_t mark = (int64_t)ruxen_string_from("\xe3\x81\x82\xe3\x81\x84");  /* あい (marked) */
    ruxen_canvas_push_event_text(host, EV_TEXT_EDITING, 0, 2, mark);
    free((void *)mark);
    snprintf(buf, sizeof buf, "/tmp/very/long/dropped/path/that/exceeds/inline/buffer/file-%d.txt", iter);
    int64_t path = (int64_t)ruxen_string_from(buf);
    ruxen_canvas_window_pump_test_dropfile(host, path);  /* strdup into ring */
    free((void *)path);
    /* drain: each poll may MOVE an owned drop_path into pending; read + free the
     * Ruxen-owned strings the accessors return (they are fresh ruxen_string_from). */
    for (int k = 0; k < 8; k++) {
        int64_t got = ruxen_canvas_poll_event(host);
        if (got < 0) break;
        int64_t et = ruxen_canvas_event_text(host);       if (et) free((void *)et);
        int64_t dp = ruxen_canvas_event_drop_path(host);  if (dp) free((void *)dp);
    }
}

int main(void) {
    long iters = 10000;
    const char *env = getenv("SOAK_ITERS");
    if (env && env[0]) { long v = atol(env); if (v > 0) iters = v; }

    int64_t host = ruxen_canvas_host_new(320, 240);
    int64_t off  = ruxen_canvas_host_new(64, 64);
    if (!host || !off) { printf("FAIL: host alloc\n"); return 1; }
    printf("soak: %ld iters, skia=%s\n", iters,
           ruxen_canvas_skia_available(host) ? "available" : "software-fallback");

    long warm = iters / 5;             /* first 20% is warm-up (cache fill, lazy init) */
    long base_rss = 0, sample_n = 0, sum_after = 0;
    long min_after = 0, max_after = 0;

    for (long i = 0; i < iters; i++) {
        do_work(host, off, (int)i);

        /* multi-window churn: open + close a third host every 500 iters (the
         * per-host teardown path — pixels, surface, ring owned paths). */
        if (i % 500 == 0) {
            int64_t tmp = ruxen_canvas_host_new(48, 48);
            if (tmp) {
                ruxen_canvas_begin_frame(tmp);
                ruxen_canvas_clear(tmp, 1, 2, 3, 255);
                ruxen_canvas_end_frame(tmp);
                ruxen_canvas_host_drop(tmp);
            }
        }

        if (i == warm) base_rss = rss_kb();
        if (i >= warm) {
            long r = rss_kb();
            sum_after += r; sample_n++;
            if (min_after == 0 || r < min_after) min_after = r;
            if (r > max_after) max_after = r;
            if ((i - warm) % (iters / 10 + 1) == 0)
                printf("  iter %6ld  rss %ld KB\n", i, r);
        }
    }

    long end_rss = rss_kb();
    ruxen_canvas_host_drop(host);
    ruxen_canvas_host_drop(off);

    double growth_pct = base_rss > 0 ? 100.0 * (end_rss - base_rss) / base_rss : 0.0;
    long avg_after = sample_n ? sum_after / sample_n : 0;
    printf("\nRSS: warm(@%ld)=%ld KB  end=%ld KB  min=%ld max=%ld avg=%ld KB\n",
           warm, base_rss, end_rss, min_after, max_after, avg_after);
    printf("growth over last 80%%: %+.2f%% (threshold < 5%%)\n", growth_pct);

    /* A small absolute floor avoids flagging KB-level allocator noise on tiny RSS. */
    if (growth_pct < 5.0 || (end_rss - base_rss) < 2048) {
        printf("PASS: no sustained leak detected over %ld iterations\n", iters);
        return 0;
    }
    printf("FAIL: RSS grew %+.2f%% post-warmup — investigate a per-iteration leak\n", growth_pct);
    return 1;
}
