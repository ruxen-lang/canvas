// Headless verification of the canvas web backend first slice (docs/WEB_BACKEND.md).
//
// Loads CanvasKit in Node, instantiates the Ruxen-compiled draw.wasm with the
// ruxen_canvas_* host imports wired to an OFFSCREEN raster surface, calls the
// exported render(), then reads back pixels and asserts the frame drew correctly
// — proving the wasm → host-import → CanvasKit → pixels pipeline with no browser.

import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";
import CanvasKitInit from "canvaskit-wasm";
import { makeCanvasImports } from "./runtime.mjs";

const here = dirname(fileURLToPath(import.meta.url));
const binDir = join(here, "node_modules/canvaskit-wasm/bin");

const CanvasKit = await CanvasKitInit({ locateFile: (f) => join(binDir, f) });

// Offscreen raster surface — no DOM needed.
const makeSurface = (w, h) => {
  const s = CanvasKit.MakeSurface(w, h);
  if (!s) throw new Error("CanvasKit.MakeSurface returned null");
  return s;
};

const { env, hosts } = makeCanvasImports(CanvasKit, makeSurface);

const wasmBytes = readFileSync(join(here, "demo/draw.wasm"));
const { instance } = await WebAssembly.instantiate(wasmBytes, { env });

const handle = Number(instance.exports.render());
const { surface } = hosts.get(handle);

// Read one pixel (RGBA, unpremultiplied) at (px, py) from the surface's canvas.
const readPixel = (px, py) => {
  const info = {
    width: 1,
    height: 1,
    colorType: CanvasKit.ColorType.RGBA_8888,
    alphaType: CanvasKit.AlphaType.Unpremul,
    colorSpace: CanvasKit.ColorSpace.SRGB,
  };
  const px4 = surface.getCanvas().readPixels(px, py, info);
  return [px4[0], px4[1], px4[2], px4[3]];
};

// Tolerance for AA/colorspace rounding.
const near = (got, want, tol = 6) => got.every((c, i) => Math.abs(c - want[i]) <= tol);

// render() drew: clear (20,20,30,255), then a rect (40,30)-(160,120) in (220,80,60,255).
const center = readPixel(100, 75); // inside the rect
const bg = readPixel(5, 5); // background (cleared, outside the rect)

let ok = true;
const check = (label, got, want) => {
  const pass = near(got, want);
  ok &&= pass;
  console.log(`${pass ? "ok  " : "FAIL"} ${label} = [${got}] (want ~[${want}])`);
};
check("rect center", center, [220, 80, 60, 255]);
check("background", bg, [20, 20, 30, 255]);

console.log(ok ? "web backend slice works: Ruxen draw calls → CanvasKit pixels" : "web backend slice BROKEN");
process.exit(ok ? 0 : 1);
