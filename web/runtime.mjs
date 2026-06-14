// Canvas web backend runtime (docs/WEB_BACKEND.md, first slice).
//
// Maps Ruxen's `ruxen_canvas_*` wasm host imports to CanvasKit (Skia/wasm). The
// same core serves the browser demo (surface from a <canvas> element) and the
// headless test (offscreen raster surface) — the caller supplies `makeSurface`.
//
// ABI notes: every `ruxen_canvas_*` here returns Ruxen `Int` (wasm i64 → JS
// BigInt), so imports return BigInt. Args declared `Int` arrive as BigInt and
// `Float` as a plain number; `N()` normalizes both. The `RawHost` handle is an
// opaque integer indexing a JS-side table of live surfaces.

export function makeCanvasImports(CanvasKit, makeSurface) {
  const hosts = new Map(); // handle -> { surface, canvas, paint }
  let nextHandle = 1;
  const N = (x) => (typeof x === "bigint" ? Number(x) : x);
  const color = (r, g, b, a) => CanvasKit.Color(N(r), N(g), N(b), N(a));
  const host = (h) => hosts.get(N(h));

  const env = {
    // Create a surface (w×h) and return its opaque handle.
    ruxen_canvas_host_new(w, h) {
      const surface = makeSurface(N(w), N(h));
      const canvas = surface.getCanvas();
      const paint = new CanvasKit.Paint();
      paint.setAntiAlias(true);
      const handle = nextHandle++;
      hosts.set(handle, { surface, canvas, paint });
      return BigInt(handle);
    },
    // Frame start — nothing to do for a retained surface yet.
    ruxen_canvas_begin_frame(_h) {
      return 0n;
    },
    ruxen_canvas_clear(h, r, g, b, a) {
      host(h).canvas.clear(color(r, g, b, a));
      return 0n;
    },
    ruxen_canvas_draw_rect(h, x, y, w, ht, r, g, b, a) {
      const { canvas, paint } = host(h);
      paint.setStyle(CanvasKit.PaintStyle.Fill);
      paint.setColor(color(r, g, b, a));
      canvas.drawRect(CanvasKit.LTRBRect(N(x), N(y), N(x) + N(w), N(y) + N(ht)), paint);
      return 0n;
    },
    // Frame end — commit drawing to the surface.
    ruxen_canvas_end_frame(h) {
      host(h).surface.flush();
      return 0n;
    },
  };

  // `hosts` is exposed so a host (test/demo) can reach a surface by handle to
  // snapshot or present it after `render()` returns the handle.
  return { env, hosts };
}
