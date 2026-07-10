// coverage.mjs [corpus-dir...] — measure how much of the language the DIRECT wasm backend (wasmgen) covers,
// using the NATIVE compiler as the oracle. For each .arche test:
//   1. native:  `arche run <f>`                                   → reference stdout   (skip if it can't run)
//   2. wasm:    `ARCHE_WASMGEN=1 arche build --arch=wasm32 …` then run-one.mjs → wasm stdout
//   3. PASS if the two stdouts match; else classify the failure.
// Coverage = PASS / (tests the native oracle can run). This is the regression harness for this and future
// backends: as wasmgen grows, PASS climbs; a regression flips a PASS to MISMATCH.
//
// Usage:  node tests/wasm/coverage.mjs [dir ...]   (default: tests/unit/language)
import { execFileSync } from "node:child_process";
import { readdirSync, statSync, mkdtempSync, rmSync, writeFileSync, mkdirSync } from "node:fs";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import { tmpdir } from "node:os";

const here = dirname(fileURLToPath(import.meta.url));
const repo = join(here, "..", "..");
const arche = join(repo, "build", "arche");
const runner = join(here, "run-one.mjs");
const roots = process.argv.slice(2).map((p) => (p.startsWith("/") ? p : join(process.cwd(), p)));
if (roots.length === 0) roots.push(join(repo, "tests", "unit", "language"));

function walk(dir, acc) {
  for (const name of readdirSync(dir)) {
    const p = join(dir, name);
    const st = statSync(p);
    if (st.isDirectory()) walk(p, acc);
    else if (name.endsWith(".arche")) acc.push(p);
  }
  return acc;
}

function tryExec(cmd, args, opts = {}) {
  try { return { ok: true, out: execFileSync(cmd, args, { encoding: "utf8", timeout: 8000, stdio: ["ignore", "pipe", "pipe"], ...opts }) }; }
  catch (e) { return { ok: false, out: (e.stdout || "") + "", err: (e.stderr || e.message || "") + "", timedOut: e.code === "ETIMEDOUT" || e.signal === "SIGTERM" }; }
}

const work = mkdtempSync(join(tmpdir(), "wasmcov-"));
const files = roots.flatMap((r) => walk(r, []));
const cat = { PASS: [], MISMATCH: [], WASM_COMPILE_FAIL: [], WASM_RUN_FAIL: [], NATIVE_SKIP: [] };

for (const f of files) {
  // 1) native oracle
  const nat = tryExec(arche, ["run", f]);
  if (!nat.ok) { cat.NATIVE_SKIP.push(f); continue; } // gfx/forever/native-only/error tests aren't oracles
  // 2) wasm compile (direct backend)
  const wasmPath = join(work, "t.wasm");
  const wc = tryExec(arche, ["build", "--arch=wasm32", "-o", wasmPath, f], { env: { ...process.env, ARCHE_WASMGEN: "1" } });
  if (!wc.ok) { cat.WASM_COMPILE_FAIL.push(f); continue; }
  // 3) wasm run
  const wr = tryExec(process.execPath, [runner, wasmPath]);
  if (!wr.ok) { cat.WASM_RUN_FAIL.push(f); continue; }
  (nat.out === wr.out ? cat.PASS : cat.MISMATCH).push(f);
}
rmSync(work, { recursive: true, force: true });

const rel = (f) => f.slice(repo.length + 1);
// Persist full category lists for triage (the console truncates).
const outDir = join(here, ".last");
mkdirSync(outDir, { recursive: true });
for (const k of Object.keys(cat)) writeFileSync(join(outDir, k + ".txt"), cat[k].map(rel).sort().join("\n") + "\n");
const runnable = files.length - cat.NATIVE_SKIP.length;
const pct = runnable ? ((cat.PASS.length / runnable) * 100).toFixed(1) : "0.0";
console.log(`\nDIRECT WASM BACKEND — coverage vs native oracle`);
console.log(`corpus: ${files.length} files under ${roots.map(rel).join(", ")}`);
console.log(`native-runnable oracle set: ${runnable}   (skipped ${cat.NATIVE_SKIP.length}: gfx/forever/native-only/error tests)\n`);
console.log(`  PASS               ${cat.PASS.length}`);
console.log(`  MISMATCH           ${cat.MISMATCH.length}   (compiled but wrong output → unsupported feature)`);
console.log(`  WASM_COMPILE_FAIL  ${cat.WASM_COMPILE_FAIL.length}`);
console.log(`  WASM_RUN_FAIL      ${cat.WASM_RUN_FAIL.length}   (trap / bad module)`);
console.log(`\n  ► wasm backend coverage: ${cat.PASS.length}/${runnable} = ${pct}% of native-runnable tests\n`);
for (const k of ["MISMATCH", "WASM_COMPILE_FAIL", "WASM_RUN_FAIL"]) {
  if (!cat[k].length) continue;
  console.log(`${k} (${cat[k].length}):`);
  for (const f of cat[k].slice(0, 12)) console.log(`   ${rel(f)}`);
  if (cat[k].length > 12) console.log(`   … +${cat[k].length - 12} more`);
}
