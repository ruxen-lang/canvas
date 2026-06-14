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

export function makeCanvasImports(CanvasKit, makeSurface, opts = {}) {
  const hosts = new Map(); // handle -> { surface, canvas, paint }
  let nextHandle = 1;
  const N = (x) => (typeof x === "bigint" ? Number(x) : x);
  const color = (r, g, b, a) => CanvasKit.Color(N(r), N(g), N(b), N(a));
  // draw_round_rect passes a packed 0xAARRGGBB int (Color.to_argb).
  const argbColor = (argb) => {
    const v = N(argb) >>> 0;
    return CanvasKit.Color((v >> 16) & 0xff, (v >> 8) & 0xff, v & 0xff, (v >> 24) & 0xff);
  };

  // Wasm linear memory + an optional Font, wired after instantiation via the
  // returned setters — draw_text needs to read the `&String` (a char* offset
  // into wasm memory) and render glyphs.
  let memory = opts.memory || null;
  let font = opts.font || null;
  const readCString = (ptr) => {
    const p = N(ptr) >>> 0;
    if (!memory || !p) return "";
    const bytes = new Uint8Array(memory.buffer, p);
    let end = 0;
    while (bytes[end] !== 0) end++;
    return new TextDecoder("utf-8").decode(bytes.subarray(0, end));
  };

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
      // Real canvas FFI: host_new returns a RawHost (class → pointer → wasm i32),
      // so return a plain Number, not BigInt. The handle is a small int index.
      return handle;
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
    // Rounded rect — fill when stroke_w <= 0, else stroke. argb is 0xAARRGGBB.
    ruxen_canvas_draw_round_rect(h, x, y, w, ht, radius, stroke_w, argb) {
      const { canvas, paint } = host(h);
      const sw = N(stroke_w);
      paint.setColor(argbColor(argb));
      if (sw > 0) { paint.setStyle(CanvasKit.PaintStyle.Stroke); paint.setStrokeWidth(sw); }
      else { paint.setStyle(CanvasKit.PaintStyle.Fill); }
      const r = N(radius);
      const rrect = CanvasKit.RRectXY(
        CanvasKit.LTRBRect(N(x), N(y), N(x) + N(w), N(y) + N(ht)), r, r);
      canvas.drawRRect(rrect, paint);
      return 0n;
    },
    // Text — `text` is a &String (char* offset into wasm memory). Needs a font.
    ruxen_canvas_draw_text(h, text, x, y, r, g, b, a) {
      const { canvas, paint } = host(h);
      if (!font) return 0n; // no font wired → skip (shapes still render)
      const s = readCString(text);
      paint.setStyle(CanvasKit.PaintStyle.Fill);
      paint.setColor(color(r, g, b, a));
      canvas.drawText(s, N(x), N(y), paint, font);
      return 0n;
    },
    ruxen_canvas_save(h) { host(h).canvas.save(); return 0n; },
    ruxen_canvas_restore(h) { host(h).canvas.restore(); return 0n; },
    ruxen_canvas_translate(h, dx, dy) { host(h).canvas.translate(N(dx), N(dy)); return 0n; },
    ruxen_canvas_clip_rect(h, x, y, w, ht) {
      host(h).canvas.clipRect(
        CanvasKit.LTRBRect(N(x), N(y), N(x) + N(w), N(y) + N(ht)),
        CanvasKit.ClipOp.Intersect, true);
      return 0n;
    },
    // Frame end — commit drawing to the surface.
    ruxen_canvas_end_frame(h) {
      host(h).surface.flush();
      return 0n;
    },
  };

  // `hosts` is exposed so a host (test/demo) can reach a surface by handle to
  // snapshot or present it after `render()` returns the handle. `setMemory` and
  // `setFont` wire the wasm memory + a CanvasKit Font after instantiation (for
  // draw_text). Unimplemented `ruxen_*` host imports (sync/io/time) are filled
  // with no-op stubs by `stubMissing` so the module instantiates.
  const setMemory = (m) => { memory = m; };
  const setFont = (f) => { font = f; };
  return { env, hosts, setMemory, setFont };
}

/// Fill any module imports not implemented by `env` with no-op stubs returning
/// 0n (sync mutexes, io puts, time — irrelevant to a single-threaded one-shot
/// render). Pass the WebAssembly.Module so we can enumerate its imports.
export function stubMissing(CanvasKit, env, wasmModule) {
  for (const i of WebAssembly.Module.imports(wasmModule)) {
    if (i.module === "env" && i.kind === "function" && !(i.name in env)) {
      env[i.name] = (...args) => {
        // ruxen_puts(ptr) — surface text to the console if we can.
        return 0n;
      };
    }
  }
  return env;
}
