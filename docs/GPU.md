# ADR: GPU surface backend (Ganesh) — GL vs Vulkan vs Metal per platform

Status: **Accepted (design only — no implementation in this cycle)**
Date: 2026-06-08
Supersedes: the "Open decision — GPU surface backend" line in `docs/ROADMAP.md`.

This is an architecture decision record. It decides *which* GPU API `canvas`
targets per platform when it moves off CPU raster, and — equally important —
*how* that move is structured so it does not disturb the `ruxen_canvas_*` ABI
or the software-raster fallback. It does **not** bind any `gr_*` symbol; that is
a later cycle gated on this record.

## Context

Today `canvas` renders entirely on the CPU: `runtime/skia_shim.c` wraps the
host's `0xAARRGGBB` framebuffer with `sk_surface_new_raster_direct`
(`kBGRA_8888` / premul) and Skia rasterizes straight into it (`docs/SKIA.md`).
Every drawing primitive the L1 surface exposes — `clear`, `draw_rect`, the
shape/path/gradient/text family — goes through one flat C function per
capability, prefixed `ruxen_canvas_*`, with all pointers crossing the FFI as
machine-word integers (`docs/FFI.md`). The SDL presenter
(`runtime/sdl_window.c`) only ever reads the leading `width/height/pixels`
prefix of `RxHost`; it has no idea Skia exists.

Three forces push toward a GPU surface eventually:

1. **Fill rate.** CPU raster is fine for a counter app and for the pin tests,
   but blurred shadows, large gradients, image scaling, and full-window
   repaints at 60–120 Hz on a HiDPI display are exactly the workloads a CPU
   rasterizer struggles with. Skia's Ganesh backend exists for this.
2. **Compositing cost.** With a GPU surface, Skia renders into a GPU texture and
   the OS compositor presents it directly — we stop round-tripping a
   `width*height*4` buffer through the CPU and the SDL texture upload each frame.
3. **Platform expectations.** The native 2D stacks we are peers with
   (Flutter, Avalonia, Chrome) are GPU-first; staying CPU-only caps us.

The constraints that bound the decision:

- **We fetch + dlopen, we do not link.** `libSkiaSharp` is the prebuilt Skia
  behind Avalonia / Uno / MAUI; `fetch_skia.sh` SHA-pins it and the shim
  `dlopen`s it (`docs/SKIA.md`, `Ruxen.toml [system_libs] = []`). Whatever GPU
  path we pick must be reachable through symbols **that prebuilt actually
  exports** and must not introduce a link-time GPU dependency.
- **`libSkiaSharp` ships the Ganesh C API.** The fetched library exports the
  flat `gr_*` surface: `gr_direct_context_make_gl` / `_make_vulkan` /
  `_make_metal`, `gr_backendrendertarget_new_*`, and
  `sk_surface_new_backend_render_target` (the GPU analogue of
  `sk_surface_new_raster_direct`). So the GPU surface can be created through the
  same dlopen-resolved table pattern `RxSkia` already uses — no new dependency
  shape.
- **Skia does NOT create the GL/Vulkan/Metal *context* for us.** `gr_*` consumes
  a context/device the host already made current. That is the platform-specific
  part, and it belongs to the windowing layer (SDL), not to Skia.
- **The per-platform GPU reality is fixed by the OS, not by us:**
  - **macOS / iOS:** OpenGL is deprecated; **Metal** is the only first-class,
    non-deprecated path. Vulkan exists only via **MoltenVK** (a translation
    layer over Metal) — extra moving parts, no upside over native Metal here.
  - **Linux:** **OpenGL (via GLX/EGL)** is universal and the lowest-friction
    path; **Vulkan** is available and faster on modern drivers but demands far
    more setup (instance/device/queue/swapchain) and driver-quality variance.
  - **Windows:** **OpenGL (WGL)** works everywhere; **Vulkan** and ANGLE-over-
    D3D are alternatives. GL is the common denominator.
- **The software raster path must never go away.** It is the deterministic
  backend the pin tests read pixels from, and the guaranteed fallback when no
  GPU / no `libSkiaSharp` is present (headless CI, minimal containers). Whatever
  we add must slot *behind* the existing ABI with the raster path intact.

## Options

### Option A — One GPU API everywhere (pick GL, or pick Vulkan)

Use a single GPU backend across all desktops.

- **GL-only:** maximally portable (GL works on all three desktops via SDL's GL
  context), one code path, smallest surface. But on macOS GL is **deprecated**
  and capped at 4.1 — we would be building new code on an API Apple is actively
  retiring, with no AA/perf guarantees going forward.
- **Vulkan-only (+ MoltenVK on macOS):** one modern API in principle, but in
  practice three very different driver realities, the heaviest setup
  (instance/device/queue/swapchain/sync), and MoltenVK as a mandatory
  translation layer on Apple. Highest complexity for a 2D canvas whose hot path
  Skia already optimizes internally.

### Option B — Native API per platform behind one internal seam

Target the OS-blessed API on each platform and hide the choice behind a single
internal context-creation seam in the windowing layer:

- macOS/iOS → **Metal** (`gr_direct_context_make_metal`)
- Linux → **OpenGL** (`gr_direct_context_make_gl`, GLX/EGL via SDL)
- Windows → **OpenGL** (`gr_direct_context_make_gl`, WGL via SDL)
  (Vulkan stays a *later, additive* alternative behind the same seam where it
  measurably wins.)

Skia's `gr_*` already abstracts the rendering; the only platform-specific code
is creating + making-current the GL/Metal context, which SDL2 (already our
windowing dlopen target) provides: `SDL_GL_CreateContext` for GL, and a
`CAMetalLayer` from an `SDL_Window` for Metal.

### Option C — Stay CPU-only indefinitely

Keep `sk_surface_new_raster_direct`, never add a GPU surface. Zero new risk,
but forfeits the fill-rate ceiling that motivates this ADR. Acceptable only as
the *current* state, not as the endpoint.

## Decision

**Adopt Option B: native API per platform — Metal on Apple, OpenGL on
Linux/Windows — behind a single internal `rx_gpu_context` seam in the
windowing layer, with Vulkan deferred as a later additive option where it
measurably wins.** Sequence GL first (Linux/Windows) because it shares the most
machinery with what we have and unblocks two of three desktops; add Metal for
macOS next; revisit Vulkan only if profiling on a target platform justifies it.

Rationale:

- It uses **only symbols `libSkiaSharp` already exports** (`gr_*` +
  `sk_surface_new_backend_render_target`), so it stays inside the fetch+dlopen
  model with **no new link-time dependency** and no change to
  `Ruxen.toml [system_libs]`.
- It follows each OS's **non-deprecated, best-supported** API instead of fighting
  it (no building atop macOS's deprecated GL; no mandatory MoltenVK).
- The platform-specific code is **confined to context creation** in
  `sdl_window.c` (which already owns the SDL dlopen and the window), not spread
  through the drawing shim. The drawing shim keeps issuing the same Skia calls
  against whatever `sk_canvas` it is handed.
- It is **incremental**: GL-first gets a working GPU path on two platforms with
  the least new code; Metal and (if ever) Vulkan are additive behind the same
  seam, decided per platform by measurement — consistent with the project's
  "no abstraction for hypothetical futures, generalize at the third caller"
  discipline.

## Consequences

Positive:

- GPU fill rate for shadows/gradients/images/full-window repaint, on the
  OS-preferred API, without leaving the fetch+dlopen model.
- The drawing surface (`ruxen_canvas_*`) is unchanged for L2 — `quiver` and apps
  see the same `Canvas` whether it is CPU- or GPU-backed.
- The deterministic software raster path is preserved verbatim as both the
  pin-test backend and the universal fallback.

Costs / risks to manage when implementing (later cycle):

- **Context lifetime + threading.** A GPU `gr_direct_context` and its surface
  are bound to the thread that made the context current; `RxHost` is already
  documented single-owner / single-thread (`rx_canvas_internal.h`), which this
  relies on. Teardown order matters: the GPU surface must be unref'd before the
  context, and both before the SDL window is destroyed — an explicit ordering in
  `host_drop` / window teardown, the same discipline the raster `surface_unref`
  already follows.
- **Readback for tests.** `ruxen_canvas_read_pixel` reads CPU memory today. A GPU
  surface has no CPU-side `pixels`; pin tests against a GPU backend need an
  explicit `sk_surface_read_pixels` into a staging buffer. The chosen mitigation:
  **pin tests keep running on the raster backend** (they assert Skia's
  rasterization semantics, which are backend-independent); GPU gets a smaller,
  separate readback-based smoke test. No existing pin test changes.
- **Color/alpha.** The raster surface is `kBGRA_8888` / premul. The GPU
  render-target format must match (or be converted at present time) so colors
  read back identically; this is pinned by the same empirical probe approach
  `docs/SKIA.md` already uses.
- **Per-platform context glue is real work** (GL context via SDL on Linux/
  Windows; `CAMetalLayer` wiring on macOS). It is bounded and lives in one file.

Neutral:

- Vulkan is neither adopted nor forbidden — it is a future additive backend
  behind the same seam, to be justified by profiling, not picked up front.

## How it preserves the existing ABI and the CPU-raster fallback

The whole point of the seam is that **nothing above `rx_host_canvas` changes.**

- **The `ruxen_canvas_*` C ABI is frozen.** Today `rx_host_canvas(RxHost*)`
  returns an `sk_canvas_t*` from a raster-direct surface. Under this decision it
  returns an `sk_canvas_t*` from *either* a raster-direct surface *or* a GPU
  backend-render-target surface — chosen once at host creation. Every drawing
  wrapper (`ruxen_canvas_draw_rect`, `_draw_path`, `_draw_text`, …) keeps calling
  the identical `sk->canvas_*` functions on that canvas. Their flat signatures —
  the ABI L1's `lib` blocks declare and L2 calls — do not move. Adding a GPU
  backend is **not** an ABI change; it is an internal surface-construction
  choice behind an unchanged boundary.

- **Backend selection is a runtime fallback chain, mirroring the existing dlopen
  ladder.** `rx_host_canvas` already tries Skia and falls back to software when
  `rx_skia()->available` is 0. The GPU backend is one more rung at the top:
  attempt GPU surface (context made current + `make_gl`/`make_metal` +
  `sk_surface_new_backend_render_target`) → on any failure fall back to
  `sk_surface_new_raster_direct` → on no-Skia fall back to the software
  rasterizer. A headless/no-GPU host transparently lands on raster, exactly as
  today. `skia_active?` / `skia_available?` stay valid; a future
  `gpu_active?` probe is additive.

- **The software raster path is untouched and remains the test oracle.** No GPU
  code runs in the pin-test build; `read_pixel` keeps reading the same CPU
  buffer. The GPU surface is opt-in per host and never the default for offscreen
  test canvases.

- **The SDL presenter contract is unchanged for raster and tightened for GPU.**
  For the raster path, `sdl_window.c` keeps presenting `RxHost.pixels` via its
  `RxHostPrefix` view — no awareness of Skia. For a GPU host, the present step
  becomes a buffer swap on the same window the context was created against; this
  is added behind the existing present hook, not by changing the hook's
  signature.

In short: GPU support enters as a new *top rung* of the existing
backend-selection ladder, created from symbols the fetched `libSkiaSharp`
already exports, with the CPU rasterizer preserved verbatim beneath it and the
`ruxen_canvas_*` ABI — the only thing L2 depends on — held completely stable.
