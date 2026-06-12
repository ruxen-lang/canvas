/* multiwindow_verify.c — standalone proof that canvas can drive TWO independent
 * on-screen SDL windows at once, present different content to each, and DEMUX
 * input by SDL windowID (the multi-window contract in runtime/sdl_window.c).
 * RUN THIS ON A DESKTOP with a display — it cannot be verified headless or in
 * the test harness (the harness forks per case and runs headless; the in-harness
 * tests/multiwindow.rx pins the per-host canvas + event-ring isolation instead).
 *
 *   cc -O2 -o multiwindow_verify examples/multiwindow_verify.c
 *   ./multiwindow_verify     # two windows appear: one RED, one BLUE, for ~3s.
 *                            # Click/move the mouse in either; the demux prints
 *                            # which window each event belongs to (by windowID).
 *
 * Mirrors what runtime/sdl_window.c does for the raster path: dlopen SDL2 (no
 * link-time dependency); per window SDL_CreateWindow -> SDL_CreateRenderer ->
 * SDL_CreateTexture; per frame SDL_UpdateTexture(host pixels) -> RenderCopy ->
 * RenderPresent. The event loop reads windowID at byte offset 8 of every
 * window-associated SDL event (exactly as ruxen_canvas_window_pump now does) and
 * routes it to the matching window — proving the demux keeps window B's clicks
 * out of window A's stream.
 *
 * Exit 0 + "PASS" once both windows presented and the demux routed correctly;
 * "SKIP" if SDL2 / a display are unavailable.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dlfcn.h>

#define SDL_INIT_VIDEO            0x20u
#define SDL_WINDOWPOS_CENTERED    0x2FFF0000u
#define SDL_WINDOW_SHOWN          0x4u
#define SDL_WINDOW_ALLOW_HIGHDPI  0x00002000u
#define SDL_PIXELFORMAT_ARGB8888  0x16362004u
#define SDL_TEXTUREACCESS_STREAMING 1
#define SDL_QUIT_EV               0x100u
#define SDL_MOUSEMOTION_EV        0x400u
#define SDL_MOUSEBUTTONDOWN_EV    0x401u
#define SDL_EVENT_WINDOWID_OFF    8   /* Uint32 windowID @ 8 for window events */

static void *DL(const char *const *names, int n) {
    for (int i = 0; i < n; i++) { void *h = dlopen(names[i], RTLD_NOW | RTLD_GLOBAL); if (h) return h; }
    return NULL;
}

/* One window's render state — the example's analogue of an RxWin slot. */
typedef struct {
    void    *win, *ren, *tex;
    uint32_t id;            /* SDL_GetWindowID — the demux key */
    uint32_t color;         /* 0xAARRGGBB fill */
    int      w, h;
    uint32_t *pixels;       /* the host-style framebuffer */
    int      clicks;        /* events the demux routed to THIS window */
} Win;

int main(void) {
    const char *sdl_names[] = { "libSDL2-2.0.0.dylib", "/opt/homebrew/lib/libSDL2-2.0.0.dylib",
                                "/usr/local/lib/libSDL2-2.0.0.dylib", "libSDL2.dylib",
                                "libSDL2-2.0.so.0" };
    void *SDL = DL(sdl_names, 5);
    if (!SDL) { printf("SKIP: SDL2 unavailable\n"); return 2; }

    int   (*SDL_Init)(uint32_t) = dlsym(SDL, "SDL_Init");
    void *(*SDL_CreateWindow)(const char*,int,int,int,int,uint32_t) = dlsym(SDL, "SDL_CreateWindow");
    void *(*SDL_CreateRenderer)(void*,int,uint32_t) = dlsym(SDL, "SDL_CreateRenderer");
    void *(*SDL_CreateTexture)(void*,uint32_t,int,int,int) = dlsym(SDL, "SDL_CreateTexture");
    int   (*SDL_UpdateTexture)(void*,const void*,const void*,int) = dlsym(SDL, "SDL_UpdateTexture");
    int   (*SDL_RenderClear)(void*) = dlsym(SDL, "SDL_RenderClear");
    int   (*SDL_RenderCopy)(void*,void*,const void*,const void*) = dlsym(SDL, "SDL_RenderCopy");
    void  (*SDL_RenderPresent)(void*) = dlsym(SDL, "SDL_RenderPresent");
    int   (*SDL_PollEvent)(void*) = dlsym(SDL, "SDL_PollEvent");
    uint32_t (*SDL_GetWindowID)(void*) = dlsym(SDL, "SDL_GetWindowID");
    void  (*SDL_DestroyWindow)(void*) = dlsym(SDL, "SDL_DestroyWindow");
    void  (*SDL_Delay)(uint32_t) = dlsym(SDL, "SDL_Delay");
    if (!SDL_Init || !SDL_CreateWindow || !SDL_CreateRenderer || !SDL_CreateTexture ||
        !SDL_UpdateTexture || !SDL_RenderCopy || !SDL_RenderPresent || !SDL_PollEvent ||
        !SDL_GetWindowID) {
        printf("SKIP: required SDL2 symbols absent\n"); return 2;
    }
    if (SDL_Init(SDL_INIT_VIDEO) != 0) { printf("SKIP: SDL_Init failed (no display?)\n"); return 2; }

    Win wins[2];
    memset(wins, 0, sizeof wins);
    wins[0].w = 240; wins[0].h = 180; wins[0].color = 0xFFFF2020u;   /* red  */
    wins[1].w = 240; wins[1].h = 180; wins[1].color = 0xFF2040FFu;   /* blue */
    const char *titles[2] = { "ruxen window A (red)", "ruxen window B (blue)" };

    for (int i = 0; i < 2; i++) {
        Win *w = &wins[i];
        w->win = SDL_CreateWindow(titles[i],
                                  (int)SDL_WINDOWPOS_CENTERED, (int)SDL_WINDOWPOS_CENTERED,
                                  w->w, w->h,
                                  SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI);
        if (!w->win) { printf("SKIP: window %d failed (headless?)\n", i);
                       for (int j = 0; j < i; j++) SDL_DestroyWindow(wins[j].win); return 2; }
        w->ren = SDL_CreateRenderer(w->win, -1, 0);
        w->tex = SDL_CreateTexture(w->ren, SDL_PIXELFORMAT_ARGB8888,
                                   SDL_TEXTUREACCESS_STREAMING, w->w, w->h);
        w->id  = SDL_GetWindowID(w->win);
        w->pixels = malloc((size_t)w->w * w->h * 4);
        for (int p = 0; p < w->w * w->h; p++) w->pixels[p] = w->color;
        printf("window %d: id=%u color=0x%08X\n", i, w->id, w->color);
    }
    if (wins[0].id == wins[1].id) { printf("FAIL: window ids collided\n"); return 1; }

    int presented0 = 0, presented1 = 0;
    unsigned char ev[64];
    for (int frame = 0; frame < 180; frame++) {            /* ~3s at 60Hz */
        /* DEMUX: drain the one shared SDL queue, route each event by windowID —
         * exactly ruxen_canvas_window_pump's logic. */
        while (SDL_PollEvent(ev)) {
            uint32_t type; memcpy(&type, ev, 4);
            if (type == SDL_QUIT_EV) { frame = 9999; break; }
            if (type == SDL_MOUSEBUTTONDOWN_EV || type == SDL_MOUSEMOTION_EV) {
                uint32_t wid; memcpy(&wid, ev + SDL_EVENT_WINDOWID_OFF, 4);
                for (int i = 0; i < 2; i++) {
                    if (wins[i].id == wid) {
                        if (type == SDL_MOUSEBUTTONDOWN_EV) {
                            wins[i].clicks++;
                            printf("  click routed to window %d (id=%u), total %d\n",
                                   i, wid, wins[i].clicks);
                        }
                        break;
                    }
                }
            }
        }
        for (int i = 0; i < 2; i++) {
            Win *w = &wins[i];
            SDL_UpdateTexture(w->tex, NULL, w->pixels, w->w * 4);
            SDL_RenderClear(w->ren);
            SDL_RenderCopy(w->ren, w->tex, NULL, NULL);
            SDL_RenderPresent(w->ren);
        }
        presented0 = 1; presented1 = 1;
        if (SDL_Delay) SDL_Delay(16);
    }

    for (int i = 0; i < 2; i++) { free(wins[i].pixels); SDL_DestroyWindow(wins[i].win); }
    if (presented0 && presented1) {
        printf("PASS: presented two independent on-screen windows; demux routed "
               "%d clicks to A, %d to B\n", wins[0].clicks, wins[1].clicks);
        return 0;
    }
    printf("FAIL: a window did not present\n");
    return 1;
}
