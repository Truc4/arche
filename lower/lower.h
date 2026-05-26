#ifndef LOWER_H
#define LOWER_H

#include "../cst/cst.h"
#include "../hir/hir.h"
#include "../semantic/sem_model.h"

/* Provide the resolved-type side model lowering should read (keyed by CST node
 * id). Call before lower_to_hir. */
void lower_set_model(const SemModel *model);

/* CST-driven lowering: translate the lossless CST (+ semantic side model) into a clean
   HirProgram. The CST + src must outlive the returned HirProgram (HIR_TYPE_NAMED names
   point into the CST). This is the only lowering path. */
HirProgram *lower_to_hir(const SyntaxNode *root, const char *src);

/* Register a `use`d module's CST so lower_to_hir can inline+prefix it
 * (the CST-path equivalent of main.c's resolve_uses). The CST + src must outlive
 * lowering. Call once per `use` before lower_to_hir. */
void lower_add_module(const char *name, const SyntaxNode *root, const char *src);

struct SemanticContext;                          /* file-scope tag (defined in semantic.c) */
void lower_set_sem(struct SemanticContext *ctx); /* for type-alias resolution */

#endif /* LOWER_H */
