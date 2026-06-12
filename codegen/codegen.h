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
/* `--incremental`: force per-unit (device-granular) codegen — each compilation unit (driver = unit 0,
 * each imported device = unit N) is opt/llc/cc'd to its own content-hash-cached object, so an
 * unchanged device is reused verbatim. Equivalent to the ARCHE_PER_UNIT env. Set before codegen runs.
 * `on == 0` resets to auto (the env decides), NOT a hard whole-program. */
void codegen_set_per_unit(int on);
/* `--whole-program`: a HARD override forcing whole-program codegen — beats both `codegen_set_per_unit`
 * and the ARCHE_PER_UNIT env. Use for the explicit opt-out (`arche run --whole-program`). */
void codegen_force_whole_program(void);
/* 1 if per-unit codegen is requested (--incremental flag OR the ARCHE_PER_UNIT env) — the driver
 * consults this to choose the device-granular pipeline. */
int codegen_per_unit_enabled(void);

#endif /* CODEGEN_H */
