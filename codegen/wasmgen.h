// wasmgen.h — Arche's DIRECT wasm backend: a peer of codegen.c that lowers a HirProgram straight to a
// self-contained WebAssembly module (no LLVM / clang / wasm-ld). Activated for `--arch=wasm32` when
// ARCHE_WASMGEN is set (until a proper --backend flag lands). Writes the `.wasm` to out_path.
#ifndef ARCHE_WASMGEN_H
#define ARCHE_WASMGEN_H
typedef struct HirProgram HirProgram;
typedef struct SemanticContext SemanticContext;
// Returns 1 on success, 0 on failure.
int wasmgen_generate(HirProgram *ast, SemanticContext *sem, const char *out_path);
#endif
