// run-one.mjs <file.wasm> — run one direct-backend wasm under a minimal host (arche_print/arche_printf +
// auto-stubbed env), print its stdout. Used by coverage.mjs as the wasm-side executor (spawned with a
// timeout so a runaway program can't hang the sweep). Compute (_start) only; reactor frame-loop is skipped.
import { readFileSync } from "node:fs";
const dec = new TextDecoder();

// Host-side printf matching C's output: args are 8-byte slots (i32 in the low 4 bytes, or an f64). Handles
// flags/width/precision for d/i/u/x/X/o/f/F/e/E/g/G/c/%. (%s needs a string pointer — deferred, see NEEDS-HUMAN.)
function fmtPrintf(mem, fp, fl, ap) {
  const dv = new DataView(mem.buffer);
  const fmt = dec.decode(new Uint8Array(mem.buffer).subarray(fp, fp + fl));
  let ai = 0;
  const I = () => dv.getInt32(ap + ai++ * 8, true);
  const U = () => dv.getUint32(ap + ai++ * 8, true) >>> 0;
  const F = () => dv.getFloat64(ap + ai++ * 8, true);
  const pad = (str, flags, width) => {
    if (str.length >= width) return str;
    const fill = flags.includes("0") && !flags.includes("-") ? "0" : " ";
    const p = (fill).repeat(width - str.length);
    if (flags.includes("-")) return str + " ".repeat(width - str.length);
    if (fill === "0" && (str[0] === "-" || str[0] === "+")) return str[0] + p + str.slice(1);
    return p + str;
  };
  const sign = (neg, flags) => (neg ? "-" : flags.includes("+") ? "+" : flags.includes(" ") ? " " : "");
  return fmt.replace(/%([-+ 0#]*)(\d+)?(?:\.(\d+))?(?:hh|h|ll|l|L|z|j|t)?([diouxXeEfFgGcs%])/g,
    (_, flags, w, p, spec) => {
      const width = w ? +w : 0, prec = p !== undefined ? +p : undefined;
      if (spec === "%") return "%";
      if (spec === "c") return pad(String.fromCharCode(I() & 0xff), flags, width);
      if (spec === "s") return "%s"; // unsupported (needs a string ptr) — left literal so it's visibly wrong
      if (spec === "d" || spec === "i") { const v = I(); let d = Math.abs(v).toString(); if (prec !== undefined) d = d.padStart(prec, "0"); return pad(sign(v < 0, flags) + d, flags, width); }
      if (spec === "u") return pad(U().toString(), flags, width);
      if (spec === "x" || spec === "X") { let h = U().toString(16); if (spec === "X") h = h.toUpperCase(); if (prec !== undefined) h = h.padStart(prec, "0"); if (flags.includes("#") && h !== "0") h = (spec === "X" ? "0X" : "0x") + h; return pad(h, flags, width); }
      if (spec === "o") { let o = U().toString(8); if (prec !== undefined) o = o.padStart(prec, "0"); return pad(o, flags, width); }
      const f = F();
      if (spec === "f" || spec === "F") { const v = Math.abs(f).toFixed(prec === undefined ? 6 : prec); return pad(sign(f < 0 || Object.is(f, -0), flags) + v, flags, width); }
      if (spec === "e" || spec === "E") { let v = Math.abs(f).toExponential(prec === undefined ? 6 : prec); if (spec === "E") v = v.toUpperCase(); v = v.replace(/e([+-])(\d)$/, "e$10$2"); return pad(sign(f < 0, flags) + v, flags, width); }
      if (spec === "g" || spec === "G") { let v = f.toString(); return pad(v, flags, width); } // approximate
      return _;
    });
}
let mem, out = "";
const real = {
  arche_print: (p, l) => { out += dec.decode(new Uint8Array(mem.buffer, p, l)); },
  arche_printf: (fp, fl, ap) => { out += fmtPrintf(mem, fp, fl, ap); },
};
const env = new Proxy(real, { get: (t, k) => (k in t ? t[k] : () => 0) }); // auto-stub any other env import
try {
  const inst = await WebAssembly.instantiate(await WebAssembly.compile(readFileSync(process.argv[2])), { env });
  mem = inst.exports.memory;
  if (inst.exports._start) inst.exports._start();
  else if (inst.exports.arche_run) inst.exports.arche_run();
  process.stdout.write(out);
} catch (e) { process.stderr.write("WASMERR:" + (e && e.message ? e.message : e)); process.exit(3); }
