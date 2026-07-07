// wasmgen.c — direct HIR→wasm backend (see wasmgen.h). A peer of codegen.c: same HirProgram input, emits a
// self-contained wasm module instead of LLVM IR text (no LLVM/clang/wasm-ld). Built incrementally:
//   M0 ✓ encoder (wasm_encode.c) · pipeline ✓ wired into compile.c.
//   M1 (here, first slice): lower a run-once `system` whose body calls `fmt.printf("literal")` → emit each
//      string as a data segment + a call to a host `arche_print(ptr,len)` import (host formats/prints). This
//      is the "print a string" milestone — no arithmetic/loops/pools yet; those grow next.
#include "wasmgen.h"
#include "wasm_encode.h"
#include "../hir/hir.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  WasmModule m;
  WasmBuf body;      // the accumulating _start body (instructions)
  WasmBuf strings;   // packed string bytes, emitted as one data segment
  int strings_base;  // linear-memory offset where `strings` is placed
  int f_print;       // func index of the imported arche_print
} Wg;

// Append a string's bytes to the data pool (processing common C escapes), return its (offset, len).
static void wg_intern_string(Wg *g, const char *raw, int rawlen, int *out_off, int *out_len) {
  int off = g->strings_base + (int)g->strings.len;
  int n = 0;
  for (int i = 0; i < rawlen; i++) {
    char c = raw[i];
    if (c == '\\' && i + 1 < rawlen) {
      char e = raw[++i];
      c = e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e == '0' ? '\0' : e; // \\, \" → the char
    }
    wb_byte(&g->strings, (uint8_t)c);
    n++;
  }
  *out_off = off;
  *out_len = n;
}

// A call to fmt.printf? (callee is a NAME/FIELD naming "printf"). First arg is the format string.
static int is_printf_call(const HirExpr *e) {
  if (!e || e->kind != HIR_EXPR_CALL) return 0;
  const HirExpr *c = e->data.call.callee;
  if (!c) return 0;
  if (c->kind == HIR_EXPR_NAME && c->data.name.name && strstr(c->data.name.name, "printf")) return 1;
  if (c->kind == HIR_EXPR_FIELD && c->data.field.field_name && strstr(c->data.field.field_name, "printf")) return 1;
  return 0;
}

// Emit one statement's effects into the _start body. First slice: only EXPR-stmt printf(string).
static void wg_emit_stmt(Wg *g, const HirStmt *s) {
  if (!s) return;
  if (s->kind == HIR_STMT_EXPR) {
    const HirExpr *e = s->data.expr_stmt.expr;
    if (is_printf_call(e) && e->data.call.arg_count >= 1 && e->data.call.args[0]->kind == HIR_EXPR_STRING) {
      int off, len;
      wg_intern_string(g, e->data.call.args[0]->data.string.value, e->data.call.args[0]->data.string.length, &off, &len);
      wb_byte(&g->body, 0x41); wb_i(&g->body, off);          // i32.const off
      wb_byte(&g->body, 0x41); wb_i(&g->body, len);          // i32.const len
      wb_byte(&g->body, 0x10); wb_u(&g->body, g->f_print);   // call arche_print
    }
  }
}

int wasmgen_generate(HirProgram *ast, SemanticContext *sem, const char *out_path) {
  (void)sem;
  Wg g;
  memset(&g, 0, sizeof(g));
  wm_init(&g.m);
  wm_memory(&g.m, 2);
  wm_export_mem(&g.m, "memory");
  wb_init(&g.body);
  wb_init(&g.strings);
  g.strings_base = 1024; // static data region

  // import env.arche_print : (i32 ptr, i32 len) -> ()  (the host prints/formats)
  uint8_t p2[] = { WT_I32, WT_I32 };
  int t_print = wm_type(&g.m, p2, 2, NULL, 0);
  g.f_print = wm_import_func(&g.m, "arche_print", t_print);

  // Walk decls: emit each run-once `system` body (a single-`#run` schedule of one system). The full
  // ScheduleTree walk comes later; for now run-once systems in declaration order == the common case.
  int has_run = 0;
  for (int i = 0; i < ast->decl_count; i++) if (ast->decls[i]->kind == HIR_DECL_RUN) has_run = 1;
  if (has_run) {
    for (int i = 0; i < ast->decl_count; i++) {
      HirDecl *d = ast->decls[i];
      if (d->kind != HIR_DECL_KERNEL) continue;
      HirKernelDecl *k = d->data.kernel;
      if (k->kind != HIR_KERNEL_SYSTEM || k->param_count != 0) continue; // run-once systems
      for (int j = 0; j < k->stmt_count; j++) wg_emit_stmt(&g, k->stmts[j]);
    }
  }
  wb_byte(&g.body, 0x0b); // end

  // _start : () -> ()  with the accumulated body.
  int t_void = wm_type(&g.m, NULL, 0, NULL, 0);
  int f_start = wm_func(&g.m, t_void);
  wm_code(&g.m, NULL, NULL, 0, &g.body);
  wm_export_func(&g.m, "_start", f_start);

  if (g.strings.len) wm_data(&g.m, g.strings_base, g.strings.data, g.strings.len);

  WasmBuf out;
  wm_finish(&g.m, &out);
  FILE *f = fopen(out_path, "wb");
  int ok = 0;
  if (f) { ok = fwrite(out.data, 1, out.len, f) == out.len; fclose(f); }
  wm_free(&g.m); wb_free(&g.body); wb_free(&g.strings); wb_free(&out);
  return ok;
}
