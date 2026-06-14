// Headless render of the REAL quiver counter app (quiver/examples/counter) on
// wasm via CanvasKit. Wires the ruxen_canvas_* draw imports (runtime.mjs) plus
// functional single-threaded stubs for the other host imports (mutex/sharedsync
// = JS boxes so State works; puts = console; window/event/gpu = noop), loads a
// font for draw_text, calls render(), and snapshots — proving a quiver UI paints.

import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";
import CanvasKitInit from "canvaskit-wasm";
import { makeCanvasImports } from "./runtime.mjs";
import { importSignatures, zeroFor } from "./wasm_sigs.mjs";

const here = dirname(fileURLToPath(import.meta.url));
const repo = join(here, "../..");
const CanvasKit = await CanvasKitInit({ locateFile: (f) => join(here, "node_modules/canvaskit-wasm/bin", f) });

const fontData = readFileSync(join(repo, "canvas/tests/fixtures/LiberationSans.ttf"));
const typeface = CanvasKit.Typeface.MakeFreeTypeFaceFromData(fontData.buffer);
const font = new CanvasKit.Font(typeface, 16);

const wasmPath = join(repo, "quiver/examples/counter/target/wasm32-unknown-unknown/debug/counter");
const wasmModule = new WebAssembly.Module(readFileSync(wasmPath));

const makeSurface = (w, h) => CanvasKit.MakeSurface(w || 200, h || 96);
const { env, hosts, setMemory } = makeCanvasImports(CanvasKit, makeSurface, { font });

// Functional single-threaded Mutex/SharedSync as JS boxes (keyed by handle), so
// quiver's State (SharedSync[Mutex[T]]) actually stores/retrieves values. Handle
// widths follow the wasm sigs: mutex/sharedsync handles are i32 (Number), the
// stored value is i64 (BigInt).
const box = new Map(); let boxId = 1;
const N2 = (x) => (typeof x === "bigint" ? Number(x) : x);
Object.assign(env, {
  ruxen_mutex_new: (v) => { const id = boxId++; box.set(id, v); return id; },        // ->i32
  ruxen_mutex_lock: (m) => N2(m),                                                    // guard==mutex (->i32)
  ruxen_mutex_guard_get: (g) => box.get(N2(g)) ?? 0n,                                // ->i64
  ruxen_mutex_guard_set: (g, v) => { box.set(N2(g), v); },                           // ->void
  ruxen_mutex_guard_drop: () => {},                                                  // ->void
  ruxen_sharedsync_new: (inner) => { const id = boxId++; box.set(id, inner); return id; }, // ->i32
  ruxen_sharedsync_get: (s) => box.get(N2(s)) ?? 0n,                                 // ->i64 (inner mutex handle)
  ruxen_puts: (ptr) => { const s = readCounterStr(ptr); if (s) console.log("[puts]", s); },
  ruxen_wasm_panic: (ptr) => { console.error("RUXEN PANIC:", readCounterStr(ptr)); },
  // Formatter (string interpolation) — stubbed: static text still renders;
  // interpolated dyn_text is empty for now (real fmt wiring is a follow-up).
  Formatter_new: () => 0n,
  Formatter_write_str: () => {},
  Formatter_buffer: () => 0n,
});

// Read a NUL-terminated UTF-8 string from wasm memory (for puts debug).
let mem = null;
const readCounterStr = (ptr) => {
  const p = N2(ptr) >>> 0;
  if (!mem || !p) return "";
  const u = new Uint8Array(mem.buffer, p); let e = 0; while (u[e]) e++;
  return new TextDecoder().decode(u.subarray(0, e));
};

// Every remaining import → a noop returning the correct zero for its result
// type (sig-driven: i64→0n, i32/f32/f64→0, void→undefined). No more guessing.
const sigs = importSignatures(readFileSync(wasmPath));
let stubbed = [];
for (const i of WebAssembly.Module.imports(wasmModule)) {
  if (i.module === "env" && i.kind === "function" && !(i.name in env)) {
    stubbed.push(i.name);
    const z = zeroFor(sigs.get(i.name)?.results);
    env[i.name] = () => z;
  }
}

// instantiate(Module, imports) resolves to the Instance directly (unlike the
// bytes overload which gives { module, instance }).
const instance = await WebAssembly.instantiate(wasmModule, { env });
setMemory(instance.exports.memory);
mem = instance.exports.memory; // for the puts debug reader

console.log("calling render()…  (noop-stubbed:", stubbed.join(", "), ")");
let rc;
try { rc = Number(instance.exports.render()); }
catch (e) { console.log("render() TRAP:", e.message); process.exit(1); }
console.log("render() =", rc);

// Inspect the produced surface: count non-cleared pixels (anything drawn).
const surface = hosts.get(1)?.surface;
if (!surface) { console.log("no surface created — render() didn't open a canvas"); process.exit(1); }
const info = { width: 200, height: 96, colorType: CanvasKit.ColorType.RGBA_8888,
  alphaType: CanvasKit.AlphaType.Unpremul, colorSpace: CanvasKit.ColorSpace.SRGB };
const px = surface.getCanvas().readPixels(0, 0, info); // Uint8Array 200*96*4
let nonWhite = 0, distinct = new Set();
for (let i = 0; i < px.length; i += 4) {
  const k = px[i] << 16 | px[i + 1] << 8 | px[i + 2];
  // content = anything that isn't the (near-white) background. Counting
  // "non-black" was a false positive: a white bg is non-black on every pixel.
  if (!(px[i] > 240 && px[i + 1] > 240 && px[i + 2] > 240)) nonWhite++;
  distinct.add(k);
}
const ok = nonWhite > 100 && distinct.size > 2; // real widgets, not a stray pixel
console.log(`drawn: ${nonWhite} non-white (content) px of ${200 * 96}, ${distinct.size} distinct colors`);
console.log(ok
  ? "✓ quiver painted a UI to the surface (widgets rendered)"
  : "✗ frame is essentially empty (background only)");
process.exit(ok ? 0 : 1);
