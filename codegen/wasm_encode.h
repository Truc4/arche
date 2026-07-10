// wasm_encode.h — a minimal WebAssembly binary-module writer for Arche's direct wasm backend (wasmgen).
// No dependencies beyond libc. Emits a single self-contained module (no linker/relocations): build up the
// type/import/function/memory/global/export/start/code/data sections, then wm_finish() serializes.
#ifndef ARCHE_WASM_ENCODE_H
#define ARCHE_WASM_ENCODE_H
#include <stddef.h>
#include <stdint.h>

// ---- growable byte buffer ----
typedef struct {
	uint8_t *data;
	size_t len, cap;
} WasmBuf;
void wb_init(WasmBuf *b);
void wb_free(WasmBuf *b);
void wb_byte(WasmBuf *b, uint8_t v);
void wb_bytes(WasmBuf *b, const void *p, size_t n);
void wb_u(WasmBuf *b, uint64_t v);       // unsigned LEB128
void wb_i(WasmBuf *b, int64_t v);        // signed LEB128
void wb_name(WasmBuf *b, const char *s); // length-prefixed utf8

// ---- wasm value types / opcodes used across the emitter ----
#define WT_I32 0x7f
#define WT_I64 0x7e
#define WT_F32 0x7d
#define WT_F64 0x7c
// export kinds
#define WK_FUNC 0x00
#define WK_MEM 0x02
#define WK_GLOBAL 0x03

// ---- module builder ----
typedef struct {
	WasmBuf types;
	int type_count;
	WasmBuf imports;
	int import_count; // all imports are funcs, in order → func indices 0..import_count-1
	WasmBuf funcs;
	int func_count; // defined funcs' type indices; indices start at import_count
	WasmBuf globals;
	int global_count;
	WasmBuf exports;
	int export_count;
	WasmBuf code;
	int code_count; // defined func bodies (parallel to funcs)
	WasmBuf data;
	int data_count;
	int has_memory, mem_min;
	int has_start, start_idx;
} WasmModule;

void wm_init(WasmModule *m);
void wm_free(WasmModule *m);

// A function type: params[np], results[nr] as WT_*. Returns the type index (deduped-caller's job).
int wm_type(WasmModule *m, const uint8_t *params, int np, const uint8_t *results, int nr);
// Import a function from module "env" named `field` with signature `type_idx`. Returns its func index.
int wm_import_func(WasmModule *m, const char *field, int type_idx);
void wm_memory(WasmModule *m, int min_pages); // declare (linear) memory
void wm_export_mem(WasmModule *m, const char *name);
// A mutable/const i32 global with a constant init. Returns global index.
int wm_global_i32(WasmModule *m, int is_mutable, int32_t init);
void wm_export_global(WasmModule *m, const char *name, int global_idx);
// Add a defined function of signature `type_idx`. Returns its func index. Add its body next via wm_code().
int wm_func(WasmModule *m, int type_idx);
// A function body: `local_types[nlg]`/`local_counts[nlg]` grouped locals, then `body` (which MUST end in 0x0b).
void wm_code(WasmModule *m, const uint8_t *local_types, const uint32_t *local_counts, int nlg, const WasmBuf *body);
void wm_export_func(WasmModule *m, const char *name, int func_idx);
void wm_start(WasmModule *m, int func_idx);
// An active data segment writing `bytes` at linear-memory `offset`.
void wm_data(WasmModule *m, uint32_t offset, const void *bytes, size_t n);

void wm_finish(WasmModule *m, WasmBuf *out); // serialize the whole module into `out`

#endif
