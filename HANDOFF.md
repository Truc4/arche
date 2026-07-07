# arche — session handoff (2026-07-07)

> Next session is on a different PC. **These changes are uncommitted — commit + push before switching machines.**
> Full context + the plan live in the sibling repo: **`../arche-playground/HANDOFF.md`**.

## What changed here (Part A: devices ship their browser host, compiler-collected)

A device now ships its browser glue (`host.js`) next to `backend.arche`, symmetric with the native `.c` shim;
the compiler collects + emits it for wasm builds. So apps stop copy-pasting per-device JS.

- `compile/module_resolve.{c,h}` — new `add_js_host` resolver callback (parallel to `add_c_shim`); collects
  `.js` from a device folder / its selected variant subfolder.
- `compile/compile.c` — `g_js_hosts[]` + `compile_add_js_host`; a `copy_file` helper; after a **wasm** link it
  emits `<outbase>.hosts.js` (collected hosts, concatenated) and copies `runtime/arche-web.js` next to the
  `.wasm`. Native builds ignore `g_js_hosts`.
- `runtime/arche-web.js` (NEW) — self-contained browser runtime (bundled WASI + `archeHosts` seam assembly +
  reactor/command drive). Staged to `build/runtime/` by the Makefile so `arche_resource_dir(RUNTIME)` finds it.
- `Makefile` — rule + `all`-dep to stage `runtime/arche-web.js` → `build/runtime/arche-web.js`.
- `arche_analyzer.c` — resolver initializer got the trailing `NULL` for the new callback.

Verified: `arche build --arch=wasm32 --select screen=dom -o X.wasm src.arche` emits `X.wasm` + `X.hosts.js` +
`arche-web.js`; browser runs it with no hand-written host.

## Still TODO in this repo (Part B — editor)
- `stdlib/term/host.js` (NEW) — browser host for stdlib `term`'s `term_raw_enable/term_raw_restore/
  term_read_key` seams (keydown→byte queue→`read_key` shifts next; returns 0 when empty; enable/restore
  no-op). `term` is single-impl, so `host.js` goes in the TOP `stdlib/term/` folder (always collected;
  emitted only for wasm).
- (In arche-playground) `screen_be_present` seam for native pacing; `src/editor.arche` already compiles.

## Also uncommitted from earlier (direct wasm backend)
`codegen/wasm_encode.{c,h}`, `codegen/wasmgen.{c,h}`, `codegen/arche_wasm_main.c`, `codegen/wasm_stubs.c`,
`compile/compile.c` hooks, `tests/wasm/{coverage.mjs,run-one.mjs}` — the LLVM-free HIR→wasm backend
(`ARCHE_WASMGEN=1`, ~47% of language tests). See `../arche-playground/HANDOFF.md`.
