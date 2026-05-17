#ifndef SEMANTIC_H
#define SEMANTIC_H

#include "../cst/cst.h"

/* Semantic analysis context */
typedef struct SemanticContext SemanticContext;

/* Create and analyze a program */
SemanticContext *semantic_analyze(Program *prog);
void semantic_context_free(SemanticContext *ctx);

/* Error checking */
int semantic_has_errors(SemanticContext *ctx);
int semantic_error_count(const SemanticContext *ctx);

/* Extern type queries */
int semantic_has_extern_type(const SemanticContext *ctx, const char *name);
int semantic_extern_type_capacity(const SemanticContext *ctx, const char *name);
int semantic_extern_type_count(const SemanticContext *ctx);
const char *semantic_extern_type_name_at(const SemanticContext *ctx, int index);

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
