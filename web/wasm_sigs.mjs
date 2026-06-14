// Parse a wasm binary's Type + Import sections to recover each imported
// function's signature (param + result value types). Node exposes import
// names but not signatures; we need result types to return correctly-typed
// stubs (i64→BigInt, i32/f32→Number, f64→Number, void→undefined).

const VT = { 0x7f: "i32", 0x7e: "i64", 0x7d: "f32", 0x7c: "f64" };

export function importSignatures(bytes) {
  const u8 = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
  let p = 8; // skip magic(4) + version(4)
  const u32 = () => { // LEB128 unsigned
    let r = 0, s = 0, b;
    do { b = u8[p++]; r |= (b & 0x7f) << s; s += 7; } while (b & 0x80);
    return r >>> 0;
  };
  const name = () => { const n = u32(); const s = new TextDecoder().decode(u8.subarray(p, p + n)); p += n; return s; };
  const types = []; // index -> { params:[], results:[] }
  const imports = []; // { module, field, results }

  while (p < u8.length) {
    const id = u8[p++];
    const len = u32();
    const end = p + len;
    if (id === 1) { // Type section
      const count = u32();
      for (let i = 0; i < count; i++) {
        if (u8[p++] !== 0x60) break; // functype tag
        const np = u32(); const params = [];
        for (let j = 0; j < np; j++) params.push(VT[u8[p++]] || "?");
        const nr = u32(); const results = [];
        for (let j = 0; j < nr; j++) results.push(VT[u8[p++]] || "?");
        types.push({ params, results });
      }
    } else if (id === 2) { // Import section
      const count = u32();
      for (let i = 0; i < count; i++) {
        const mod = name(); const fld = name(); const kind = u8[p++];
        if (kind === 0x00) { const ti = u32(); imports.push({ module: mod, field: fld, ...types[ti] }); }
        else { // table/mem/global — skip its descriptor
          if (kind === 0x01) { u8[p++]; const fl = u8[p++]; u32(); if (fl) u32(); }
          else if (kind === 0x02) { const fl = u8[p++]; u32(); if (fl) u32(); }
          else if (kind === 0x03) { u8[p++]; u8[p++]; }
        }
      }
    }
    p = end;
  }
  const map = new Map();
  for (const im of imports) map.set(im.field, im);
  return map;
}

// A zero value of the given result-type list (for noop stubs).
export function zeroFor(results) {
  if (!results || results.length === 0) return undefined;
  return results[0] === "i64" ? 0n : 0; // i32/f32/f64 → Number
}
