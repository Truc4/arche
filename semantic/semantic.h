#ifndef SEMANTIC_H
#define SEMANTIC_H

#include "../cst/cst.h"
#include "sem_model.h"

/* Semantic analysis context */
typedef struct SemanticContext SemanticContext;

/* Create and analyze a program */
SemanticContext *semantic_analyze(Program *prog);

/* CST-driven semantic analysis (migration path; gated by ARCHE_SEM_CST in main.c).
 * Walks the lossless CST + registered module CSTs to build the analyzable program,
 * keying the side model by CST node id — identical contract to semantic_analyze. */
SemanticContext *semantic_analyze_cst(const SyntaxNode *root, const char *src);

/* Register a `use`-module's CST so semantic_analyze_cst can inline it (parallel to
 * lower_add_module). Call once per module before semantic_analyze_cst. */
void semantic_add_module(const char *name, const SyntaxNode *root, const char *src);

void semantic_context_free(SemanticContext *ctx);

/* The resolved-type side model (keyed by CST node id); read by lowering. */
SemModel *sem_context_model(SemanticContext *ctx);

/* Resolve a (possibly nominal-alias) type name through the alias chain to its
 * backing; returns `name` unchanged if not an alias. */
const char *semantic_resolve_type_alias(SemanticContext *ctx, const char *name);

/* Error checking */
int semantic_has_errors(SemanticContext *ctx);
int semantic_error_count(const SemanticContext *ctx);

/* Archetype queries */
int semantic_archetype_exists(SemanticContext *ctx, const char *name);
int semantic_field_exists(SemanticContext *ctx, const char *archetype_name, const char *field_name);
FieldKind semantic_field_kind(SemanticContext *ctx, const char *archetype_name, const char *field_name);
const char *semantic_field_type_name(SemanticContext *ctx, const char *archetype_name, const char *field_name);

/* Constant queries */
const char *semantic_get_const_value(SemanticContext *ctx, const char *const_name);

/* Lint configuration. Both lints are enabled by default. CLI flags
 * (--Wno-proc-could-be-func / --Wno-proc-no-effect) disable them;
 * --Werror=proc-could-be-func / --Werror=proc-no-effect promote each
 * warning to a hard error. */
void semantic_set_lint_proc_could_be_func(int enabled, int werror);
void semantic_set_lint_proc_no_effect(int enabled, int werror);

#endif /* SEMANTIC_H */
