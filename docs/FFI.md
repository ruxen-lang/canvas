# Incremental-FFI discipline

The **full Skia C library is vendored in-repo**, but only a **curated subset of
methods is exposed via FFI**, grown method-by-method. Nothing is missing from
the vendored library — only the *binding surface* is incremental. This keeps the
binary/link surface small and every bound call tested.

## Adding a capability — the mechanical 4 steps

1. **Wrap** the `SkCanvas` / `SkParagraph` / … call in the C shim
   (`runtime/skia_shim.c`), behind a flat, Ruxen-friendly C-ABI signature
   prefixed `ruxen_canvas_*`.
2. **Declare** it in L1's `lib "C"` block in `src/lib.rx`.
3. **Expose** it as an L1 `Canvas` (or `Window`) method.
4. **Use** it from L2 (`quiver`).

## Rules

- **L2's `Canvas` API is designed against the full intended surface from the
  start.** L1 fills in coverage over time. A not-yet-bound method returns an
  `Err`/clear failure — never a silent no-op.
- **Every newly-bound Skia method gets a pin test.** (Consistent with the Ruxen
  project's test discipline: no feature lands without a test.)
- **Symbols are prefixed `ruxen_canvas_*`** so they never collide with the host
  runtime or other packages.
- **All pointers cross the ABI as machine-word integers**; `void` returns map to
  no return value — matching how the Ruxen runtime ABI table treats pointers.

## Returning a String from C to Ruxen (the clipboard pattern)

A Ruxen `String` **is** a `malloc`'d, NUL-terminated `char*` (no length header;
`library/std/string/runtime/string.c`). So a C shim function can return a Ruxen
`String` by returning a `char*` **allocated with the Ruxen allocator** — call the
runtime constructor `ruxen_string_from(const char *)`, which `malloc`s a copy.

This is load-bearing for OWNERSHIP: never return a pointer allocated by a foreign
allocator (e.g. `SDL_GetClipboardText`'s SDL-`malloc`'d buffer) directly as a
`String` — the Ruxen owner would free it with the *wrong* allocator on drop. Copy
through `ruxen_string_from`, then release the foreign buffer (`SDL_free`).

Used by `ruxen_canvas_clipboard_get` (E2). Declare the constructor as
`extern char *ruxen_string_from(const char *s);` in the shim, and the `lib`
declaration as `def self.clipboard_get as "…" -> String`. A failure path returns
`0` (NULL) and the Ruxen wrapper gates on a separate `*_available` probe rather
than constructing a String from NULL.

## C/C++ boundary

Skia's public surface is C++. `skia_shim.c` (and/or `.cpp`) is the single
C/C++ boundary; the Ruxen side only ever sees the flat C functions declared in
the shim. Keep the shim's signatures stable — changing one is an ABI change.

## Build / link (open)

Whether Skia is vendored as **prebuilt binaries per platform** or **built from
source** is decided in the L1 build spec; see [`ROADMAP.md`](ROADMAP.md). Either
way, link names are declared in `Ruxen.toml`'s `[system_libs]`.
