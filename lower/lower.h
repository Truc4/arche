#ifndef LOWER_H
#define LOWER_H

#include "../ast/ast.h"
#include "../cst/cst.h"
#include "../semantic/sem_model.h"

/* Provide the resolved-type side model lowering should read (keyed by CST node
 * id). Call before lower_cst_to_ast; if unset, lowering falls back to the type
 * carried on each expression. */
void lower_set_model(const SemModel *model);

/* Translate a fully semantically-analyzed CST into a clean AstProgram.
   CST must outlive the returned AstProgram (AST_TYPE_NAMED names point into CST). */
AstProgram *lower_cst_to_ast(Program *cst);

/* CST-driven lowering (reads the lossless CST + side model). Gated by
   ARCHE_LOWER_CST until validated IR-identical against the Program-based path. */
AstProgram *lower_cst_to_ast_v2(const SyntaxNode *root, const char *src);

/* Register a `use`d module's CST so lower_cst_to_ast_v2 can inline+prefix it
 * (the CST-path equivalent of main.c's resolve_uses). The CST + src must outlive
 * lowering. Call once per `use` before lower_cst_to_ast_v2. */
void lower_add_module(const char *name, const SyntaxNode *root, const char *src);

struct SemanticContext;                          /* file-scope tag (defined in semantic.c) */
void lower_set_sem(struct SemanticContext *ctx); /* for type-alias resolution */

#endif /* LOWER_H */
