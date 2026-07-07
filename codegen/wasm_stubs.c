// wasm_stubs.c — WASM-ONLY stubs for native-only symbols the compiler references but never calls on the
// direct-wasm-backend (ARCHE_WASMGEN) path: they sit after compile_source's early `goto cleanup`, so they're
// linked but unreachable. Compiled only into the browser (wasm32-wasi) build of the compiler; inert natively.
#ifdef __wasi__
typedef struct HirProgram HirProgram;

// wasi-libc lacks mkdtemp; the workdir path (per-unit/native IR) is never taken under wasmgen.
char *mkdtemp(char *tmpl) { return tmpl; }

// wasi-libc has no process spawning; the clang/opt/llc shell-outs are unreachable under wasmgen.
int system(const char *cmd) { (void)cmd; return -1; }

// GPU embedding (SPIR-V) is native-only; @gpu is unsupported on wasm and this is never reached.
int arche_gpu_embed(HirProgram *prog, const char *out_c_path, int quiet) {
  (void)prog; (void)out_c_path; (void)quiet; return 0;
}
#endif
