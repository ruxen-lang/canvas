/*
 * rx_dlopen.h — the dynamic-loader seam for the canvas C shim.
 *
 * The shim brings in Skia / SDL2 / HarfBuzz / ICU / (on Apple) the objc runtime
 * by RUNTIME LOADING, never link-time (docs/SKIA.md, docs/FFI.md). On POSIX that
 * is dlopen/dlsym/dlclose from <dlfcn.h>. On Windows the equivalent is LoadLibrary
 * / GetProcAddress / FreeLibrary from <windows.h>. This header is the ONE place
 * that difference lives: every call site in skia_shim.c / sdl_window.c uses the
 * rx_dlopen / rx_dlsym / rx_dlclose wrappers, so adding a platform is a change
 * here, not a sweep across the shim.
 *
 * The POSIX `flags` argument (RTLD_NOW | RTLD_LOCAL etc.) is carried through on
 * POSIX and IGNORED on Windows (LoadLibrary has no equivalent — it resolves
 * eagerly and the module is process-local by default), so call sites pass the
 * same flags on every platform and the Windows arm discards them.
 *
 * WINDOWS STATUS — EXPERIMENTAL, compiles-untested-until-CI (docs/ROADMAP.md
 * Phase 4): the seam is present and the basenames are wired (SDL2.dll /
 * libSkiaSharp.dll / libHarfBuzzSharp.dll), but no Windows host has run it. The
 * GitHub Actions windows-latest job is allowed-failure and compile-first; do not
 * claim Windows works until that job is green.
 */
#ifndef RX_DLOPEN_H
#define RX_DLOPEN_H

#include <stddef.h>   /* NULL — needed on every platform, not just via <windows.h> */

#if defined(_WIN32)

#include <windows.h>

/* POSIX dlopen flags don't exist on Windows; the call sites still pass them
 * (RTLD_NOW | RTLD_LOCAL etc.), so define them as 0 and let rx_dlopen ignore the
 * combined value. RTLD_GLOBAL likewise has no Windows analogue. */
#ifndef RTLD_NOW
#define RTLD_NOW 0
#endif
#ifndef RTLD_LOCAL
#define RTLD_LOCAL 0
#endif
#ifndef RTLD_GLOBAL
#define RTLD_GLOBAL 0
#endif

/* Open a shared library by name/path. `flags` is ignored on Windows (no RTLD_*
 * analogue). Returns an opaque handle, or NULL on failure — same contract as
 * dlopen, so every call site's NULL-check is unchanged. */
static void *rx_dlopen(const char *path, int flags) {
    (void)flags;
    if (!path) return NULL;
    return (void *)LoadLibraryA(path);
}

/* Resolve a symbol in an open library. Returns a function/data pointer, or NULL.
 * GetProcAddress returns FARPROC; the cast to void* is the documented idiom (the
 * shim casts each result to its exact callee signature, as on POSIX). */
static void *rx_dlsym(void *handle, const char *name) {
    if (!handle || !name) return NULL;
    return (void *)GetProcAddress((HMODULE)handle, name);
}

/* Close an open library (best-effort; the shim's loaded libs are process-lifetime
 * singletons, so this is rarely called — only on a half-init rollback). */
static int rx_dlclose(void *handle) {
    if (!handle) return 0;
    return FreeLibrary((HMODULE)handle) ? 0 : -1;
}

#else  /* POSIX: macOS, Linux, *BSD */

#include <dlfcn.h>

static void *rx_dlopen(const char *path, int flags) { return dlopen(path, flags); }
static void *rx_dlsym(void *handle, const char *name) { return dlsym(handle, name); }
static int   rx_dlclose(void *handle) { return dlclose(handle); }

#endif

#endif /* RX_DLOPEN_H */
