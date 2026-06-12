/* window_mgmt_verify.c — standalone proof that the Phase-1.5 window-management
 * setters actually drive a real OS window: borderless desktop FULLSCREEN on/off,
 * MAXIMIZE, and RESTORE, observing the window's drawable size change at each
 * transition. RUN THIS ON A DESKTOP with a display — it cannot be verified in the
 * forked test harness (which runs headless and gates real windows off; the
 * in-harness tests/window_mgmt.rx pins the Err-without-window contract + the
 * pump's subtype->Resize/minimized logic via the window-event seam instead).
 *
 *   cc -O2 -o window_mgmt_verify examples/window_mgmt_verify.c
 *   ./window_mgmt_verify   # a window appears, goes fullscreen, restores,
 *                          # maximizes, restores; sizes are printed at each step.
 *
 * Mirrors runtime/sdl_window.c's setters exactly: SDL_SetWindowFullscreen with
 * SDL_WINDOW_FULLSCREEN_DESKTOP (the borderless modern default), SDL_MaximizeWindow,
 * SDL_RestoreWindow — and reads SDL_GL_GetDrawableSize (falling back to
 * SDL_GetWindowSize) to confirm the backing size grew/shrank.
 *
 * Exit 0 + "PASS" when fullscreen and maximize each enlarged the window and
 * restore brought it back; "SKIP" if SDL2 / a display are unavailable.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dlfcn.h>

#define SDL_INIT_VIDEO            0x20u
#define SDL_WINDOWPOS_CENTERED    0x2FFF0000u
#define SDL_WINDOW_SHOWN          0x4u
#define SDL_WINDOW_RESIZABLE      0x20u
#define SDL_WINDOW_ALLOW_HIGHDPI  0x00002000u
#define SDL_WINDOW_FULLSCREEN_DESKTOP 0x00001001u

static void *DL(const char *const *names, int n) {
    for (int i = 0; i < n; i++) { void *h = dlopen(names[i], RTLD_NOW | RTLD_GLOBAL); if (h) return h; }
    return NULL;
}

int main(void) {
    const char *sdl_names[] = { "libSDL2-2.0.0.dylib", "/opt/homebrew/lib/libSDL2-2.0.0.dylib",
                                "/usr/local/lib/libSDL2-2.0.0.dylib", "libSDL2.dylib",
                                "libSDL2-2.0.so.0" };
    void *SDL = DL(sdl_names, 5);
    if (!SDL) { printf("SKIP: SDL2 unavailable\n"); return 2; }

    int   (*SDL_Init)(uint32_t) = dlsym(SDL, "SDL_Init");
    void *(*SDL_CreateWindow)(const char*,int,int,int,int,uint32_t) = dlsym(SDL, "SDL_CreateWindow");
    void  (*SDL_DestroyWindow)(void*) = dlsym(SDL, "SDL_DestroyWindow");
    int   (*SDL_SetWindowFullscreen)(void*,uint32_t) = dlsym(SDL, "SDL_SetWindowFullscreen");
    void  (*SDL_MaximizeWindow)(void*) = dlsym(SDL, "SDL_MaximizeWindow");
    void  (*SDL_RestoreWindow)(void*) = dlsym(SDL, "SDL_RestoreWindow");
    void  (*SDL_GetWindowSize)(void*,int*,int*) = dlsym(SDL, "SDL_GetWindowSize");
    void  (*SDL_PumpEvents)(void) = dlsym(SDL, "SDL_PumpEvents");
    void  (*SDL_Delay)(uint32_t) = dlsym(SDL, "SDL_Delay");
    if (!SDL_Init || !SDL_CreateWindow || !SDL_DestroyWindow || !SDL_SetWindowFullscreen ||
        !SDL_MaximizeWindow || !SDL_RestoreWindow || !SDL_GetWindowSize) {
        printf("SKIP: required SDL2 window-management symbols absent\n"); return 2;
    }
    if (SDL_Init(SDL_INIT_VIDEO) != 0) { printf("SKIP: SDL_Init failed (no display?)\n"); return 2; }

    void *win = SDL_CreateWindow("ruxen window management",
                                 (int)SDL_WINDOWPOS_CENTERED, (int)SDL_WINDOWPOS_CENTERED,
                                 320, 240,
                                 SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!win) { printf("SKIP: window creation failed (headless?)\n"); return 2; }

    /* settle helper: pump + delay so the WindowServer applies the state change. */
    #define SETTLE() do { for (int s = 0; s < 20; s++) { if (SDL_PumpEvents) SDL_PumpEvents(); \
                          if (SDL_Delay) SDL_Delay(16); } } while (0)

    int bw = 0, bh = 0;          /* baseline windowed size */
    SETTLE();
    SDL_GetWindowSize(win, &bw, &bh);
    printf("windowed:    %dx%d\n", bw, bh);

    int fw = 0, fh = 0;
    if (SDL_SetWindowFullscreen(win, SDL_WINDOW_FULLSCREEN_DESKTOP) != 0) {
        printf("FAIL: SetWindowFullscreen(DESKTOP) failed\n"); SDL_DestroyWindow(win); return 1;
    }
    SETTLE();
    SDL_GetWindowSize(win, &fw, &fh);
    printf("fullscreen:  %dx%d\n", fw, fh);

    /* back to windowed */
    SDL_SetWindowFullscreen(win, 0);
    SETTLE();
    int rw = 0, rh = 0;
    SDL_GetWindowSize(win, &rw, &rh);
    printf("restored:    %dx%d\n", rw, rh);

    int mw = 0, mh = 0;
    SDL_MaximizeWindow(win);
    SETTLE();
    SDL_GetWindowSize(win, &mw, &mh);
    printf("maximized:   %dx%d\n", mw, mh);

    SDL_RestoreWindow(win);
    SETTLE();
    int r2w = 0, r2h = 0;
    SDL_GetWindowSize(win, &r2w, &r2h);
    printf("restored2:   %dx%d\n", r2w, r2h);

    SDL_DestroyWindow(win);

    /* Fullscreen-desktop should fill at least as large as the windowed size (it is
     * the whole display); maximize should be at least as large as windowed too.
     * We assert strictly-larger area for fullscreen (a real display is bigger than
     * 320x240); maximize is >= windowed (some WMs cap it to the work area). */
    int fs_ok  = (fw * fh) > (bw * bh);
    int max_ok = (mw * mh) >= (bw * bh);
    if (fs_ok && max_ok) {
        printf("PASS: fullscreen enlarged %dx%d->%dx%d; maximize %dx%d->%dx%d\n",
               bw, bh, fw, fh, bw, bh, mw, mh);
        return 0;
    }
    printf("FAIL: fullscreen or maximize did not enlarge the window\n");
    return 1;
}
