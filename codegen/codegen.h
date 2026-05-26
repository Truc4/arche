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
void codegen_free(CodegenContext *ctx);

#endif /* CODEGEN_H */
