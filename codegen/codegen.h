#ifndef CODEGEN_H
#define CODEGEN_H

#include "../hir/hir.h"
#include "../semantic/semantic.h"
#include <stdio.h>

/* Code generator context */
typedef struct CodegenContext CodegenContext;

/* Create, generate, and free codegen context */
CodegenContext *codegen_create(HirProgram *ast, SemanticContext *sem_ctx);
void codegen_generate(CodegenContext *ctx, FILE *output);
/* True if codegen emitted a hard error (diagnostics already printed to stderr). The caller must
 * fail the build and not run the emitted IR. */
int codegen_had_error(const CodegenContext *ctx);
void codegen_free(CodegenContext *ctx);

/* Per-unit codegen: restrict this context to emit only `unit`'s bodies (cross-unit callees are
 * declared). Implies per-unit mode. Used by the per-unit driver in compile.c. */
void codegen_set_emit_unit(CodegenContext *ctx, int unit);
/* `--unchecked`: strip implicit runtime bounds checks (unannotated fallible ops become `!undefined`).
 * Explicit policies are still honored. Set before codegen runs. */
void codegen_set_unchecked(int on);
/* 1 if per-unit codegen is requested (ARCHE_PER_UNIT set) — the driver consults this. */
int codegen_per_unit_enabled(void);

#endif /* CODEGEN_H */
