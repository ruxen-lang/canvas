/* view.c — present a P3 PPM in a desktop window via SDL3, loaded with
 * dlopen (no SDL headers/dev package needed).
 *
 *   gcc -O2 -o view view.c
 *   ./view out.ppm [seconds]
 *
 * Window closes on quit/escape/click-close or after [seconds] (default:
 * stay open until closed).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dlfcn.h>

#define SDL_INIT_VIDEO 0x20u
#define SDL_PIXELFORMAT_ARGB8888 0x16362004u
#define SDL_TEXTUREACCESS_STREAMING 1
#define SDL_EVENT_QUIT 0x100u
#define SDL_EVENT_KEY_DOWN 0x300u

typedef int  (*fn_init)(uint32_t);
typedef void *(*fn_create_window)(const char *, int, int, uint64_t);
typedef void *(*fn_create_renderer)(void *, const char *);
typedef void *(*fn_create_texture)(void *, uint32_t, int, int, int);
typedef int  (*fn_update_texture)(void *, const void *, const void *, int);
typedef int  (*fn_render_clear)(void *);
typedef int  (*fn_render_texture)(void *, void *, const void *, const void *);
typedef void (*fn_render_present)(void *);
typedef int  (*fn_poll_event)(void *);
typedef void (*fn_delay)(uint32_t);
typedef const char *(*fn_get_error)(void);

static uint32_t *load_ppm(const char *path, int *w, int *h) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    char magic[3] = {0};
    int maxv = 0;
    if (fscanf(f, "%2s %d %d %d", magic, w, h, &maxv) != 4 ||
        strcmp(magic, "P3") != 0 || *w <= 0 || *h <= 0) {
        fclose(f);
        return NULL;
    }
    uint32_t *px = malloc((size_t)(*w) * (size_t)(*h) * 4);
    if (!px) { fclose(f); return NULL; }
    for (int64_t i = 0; i < (int64_t)(*w) * (*h); i++) {
        int r, g, b;
        if (fscanf(f, "%d %d %d", &r, &g, &b) != 3) { free(px); fclose(f); return NULL; }
        px[i] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }
    fclose(f);
    return px;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <image.ppm> [seconds]\n", argv[0]); return 2; }
    int w = 0, h = 0;
    uint32_t *px = load_ppm(argv[1], &w, &h);
    if (!px) { fprintf(stderr, "failed to load %s\n", argv[1]); return 1; }
    long secs = argc > 2 ? strtol(argv[2], NULL, 10) : 0;

    void *sdl = dlopen("libSDL3.so.0", RTLD_NOW);
    if (!sdl) { fprintf(stderr, "dlopen libSDL3.so.0: %s\n", dlerror()); return 1; }
#define LOAD(name) dlsym(sdl, name)
    fn_init           SDL_Init           = (fn_init)LOAD("SDL_Init");
    fn_create_window  SDL_CreateWindow   = (fn_create_window)LOAD("SDL_CreateWindow");
    fn_create_renderer SDL_CreateRenderer = (fn_create_renderer)LOAD("SDL_CreateRenderer");
    fn_create_texture SDL_CreateTexture  = (fn_create_texture)LOAD("SDL_CreateTexture");
    fn_update_texture SDL_UpdateTexture  = (fn_update_texture)LOAD("SDL_UpdateTexture");
    fn_render_clear   SDL_RenderClear    = (fn_render_clear)LOAD("SDL_RenderClear");
    fn_render_texture SDL_RenderTexture  = (fn_render_texture)LOAD("SDL_RenderTexture");
    fn_render_present SDL_RenderPresent  = (fn_render_present)LOAD("SDL_RenderPresent");
    fn_poll_event     SDL_PollEvent      = (fn_poll_event)LOAD("SDL_PollEvent");
    fn_delay          SDL_Delay          = (fn_delay)LOAD("SDL_Delay");
    fn_get_error      SDL_GetError       = (fn_get_error)LOAD("SDL_GetError");
    if (!SDL_Init || !SDL_CreateWindow || !SDL_CreateRenderer || !SDL_CreateTexture ||
        !SDL_UpdateTexture || !SDL_RenderTexture || !SDL_RenderPresent || !SDL_PollEvent) {
        fprintf(stderr, "missing SDL3 symbols\n");
        return 1;
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1; }
    void *win = SDL_CreateWindow("Ruxen Canvas — software backend", w * 2, h * 2, 0);
    if (!win) { fprintf(stderr, "CreateWindow: %s\n", SDL_GetError()); return 1; }
    void *ren = SDL_CreateRenderer(win, NULL);
    if (!ren) { fprintf(stderr, "CreateRenderer: %s\n", SDL_GetError()); return 1; }
    void *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
    if (!tex) { fprintf(stderr, "CreateTexture: %s\n", SDL_GetError()); return 1; }
    SDL_UpdateTexture(tex, NULL, px, w * 4);

    printf("window open (%dx%d)%s\n", w * 2, h * 2, secs ? " — auto-closing" : ", close it to exit");
    fflush(stdout);

    unsigned char ev[256];
    long frames = 0;
    for (;;) {
        while (SDL_PollEvent(ev)) {
            uint32_t type = *(uint32_t *)ev;
            if (type == SDL_EVENT_QUIT || type == SDL_EVENT_KEY_DOWN) goto done;
        }
        SDL_RenderClear(ren);
        SDL_RenderTexture(ren, tex, NULL, NULL);
        SDL_RenderPresent(ren);
        SDL_Delay(16);
        frames++;
        if (secs && frames > secs * 60) break;
    }
done:
    printf("closed after %ld frames\n", frames);
    return 0;
}
