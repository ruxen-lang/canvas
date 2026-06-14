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
// quiver's State (SharedSync[Mutex[T]]) actually stores/retrieves values.
const box = new Map(); let boxId = 1;
const B = (x) => (typeof x === "bigint" ? Number(x) : x);
Object.assign(env, {
  ruxen_mutex_new: (v) => { const id = boxId++; box.set(id, v); return BigInt(id); },
  ruxen_mutex_lock: (m) => m,                     // guard handle == mutex handle
  ruxen_mutex_guard_get: (g) => box.get(B(g)) ?? 0n,
  ruxen_mutex_guard_set: (g, v) => { box.set(B(g), v); return 0n; },
  ruxen_mutex_guard_drop: () => 0n,
  ruxen_sharedsync_new: (inner) => inner,         // transparent wrapper
  ruxen_sharedsync_get: (s) => s,
  ruxen_puts: () => 0n,
  ruxen_env_init: () => 0n,
});

// Any remaining import → noop. Return type must match the wasm signature:
// Bool→i32 and Float→f64 imports want a plain Number; Int(i64)/ptr want BigInt.
const i32ret = new Set([
  "ruxen_canvas_host_is_null", "ruxen_canvas_gpu_active", "ruxen_canvas_window_is_metal",
]);
const f64ret = new Set(["ruxen_canvas_event_a", "ruxen_canvas_event_b"]);
let stubbed = [];
for (const i of WebAssembly.Module.imports(wasmModule)) {
  if (i.module === "env" && i.kind === "function" && !(i.name in env)) {
    stubbed.push(i.name);
    env[i.name] = i32ret.has(i.name) ? () => 0 : f64ret.has(i.name) ? () => 0.0 : () => 0n;
  }
}

// instantiate(Module, imports) resolves to the Instance directly (unlike the
// bytes overload which gives { module, instance }).
const instance = await WebAssembly.instantiate(wasmModule, { env });
setMemory(instance.exports.memory);

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
let nonZero = 0, distinct = new Set();
for (let i = 0; i < px.length; i += 4) {
  const k = px[i] << 16 | px[i + 1] << 8 | px[i + 2];
  if (px[i] || px[i + 1] || px[i + 2]) nonZero++;
  distinct.add(k);
}
console.log(`drawn: ${nonZero} non-black px of ${200 * 96}, ${distinct.size} distinct colors`);
console.log(distinct.size > 1 && nonZero > 0
  ? "✓ quiver painted a UI to the surface (shapes rendered)"
  : "✗ nothing drew");
process.exit(distinct.size > 1 ? 0 : 1);
