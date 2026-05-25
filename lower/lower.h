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

#endif /* LOWER_H */
