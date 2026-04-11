#ifndef SEMANTIC_H
#define SEMANTIC_H

#include "../ast/ast.h"

/* Semantic analysis context */
typedef struct SemanticContext SemanticContext;

/* Create and analyze a program */
SemanticContext *semantic_analyze(Program *prog);
void semantic_context_free(SemanticContext *ctx);

/* Error checking */
int semantic_has_errors(SemanticContext *ctx);

/* Archetype queries */
int semantic_archetype_exists(SemanticContext *ctx, const char *name);
int semantic_field_exists(SemanticContext *ctx, const char *archetype_name, const char *field_name);
FieldKind semantic_field_kind(SemanticContext *ctx, const char *archetype_name, const char *field_name);
const char *semantic_field_type_name(SemanticContext *ctx, const char *archetype_name, const char *field_name);

#endif /* SEMANTIC_H */
