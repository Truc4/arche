#ifndef LOWER_H
#define LOWER_H

#include "../hir/hir.h"
#include "../semantic/sem_model.h"
#include "../syntax/cst.h"

/* Provide the resolved-type side model lowering should read (keyed by syntax tree node
 * id). Call before lower_to_hir. */
void lower_set_model(const SemModel *model);

/* syntax-tree-driven lowering: translate the lossless syntax tree (+ semantic side model) into a clean
   HirProgram. The syntax tree + src must outlive the returned HirProgram (HIR_TYPE_NAMED names
   point into the syntax tree). This is the only lowering path. */
HirProgram *lower_to_hir(const SyntaxNode *root, const char *src);

/* Register a `use`d module's syntax tree so lower_to_hir can inline+prefix it
 * (the syntax-tree-path equivalent of main.c's resolve_uses). The syntax tree + src must outlive
 * lowering. Call once per `use` before lower_to_hir. */
void lower_add_module(const char *name, const SyntaxNode *root, const char *src, const char *filename);
/* Clear registered modules. The registry is a static global; reset it at the start of each
 * compilation or stale entries accumulate and get inlined again (modules can be folders with
 * multiple files, all inlined). */
void lower_reset_modules(void);

struct SemanticContext;                          /* file-scope tag (defined in semantic.c) */
void lower_set_sem(struct SemanticContext *ctx); /* for type-alias resolution */

#endif /* LOWER_H */
