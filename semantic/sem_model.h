#ifndef SEM_MODEL_H
#define SEM_MODEL_H

/* Semantic side model: resolved facts keyed by CST node id, kept OUT of the
 * syntax tree (the tree stays immutable). Semantic writes here; lowering reads
 * here. The model is the single home for resolved type info — the tree is no
 * longer mutated (Expression.resolved_type is unused).
 *
 * Type strings are OWNED by the model: sem_model_set_expr_type copies them in
 * and sem_model_free releases them. Consumers (lowering -> HIR) borrow these
 * pointers, so the model must outlive them (in the compiler, until codegen). */

#include "../cst/syntax_tree.h"
#include <stdint.h>

typedef struct {
	const char **expr_type;    /* indexed by CST node id; NULL where unresolved (RESOLVED backing) */
	const char **expr_nominal; /* indexed by CST node id; the distinct tier-2 subtype name, else NULL.
	                              Lowering reads expr_type (resolved); the typechecker prefers this. */
	uint8_t *bind_alias;       /* indexed by CST node id; 1 if a bind is a compile-time type alias */
	int cap;
} SemModel;

SemModel *sem_model_new(void);
void sem_model_free(SemModel *m); /* frees the tables, not the borrowed strings */

void sem_model_set_expr_type(SemModel *m, uint32_t node_id, const char *type);
const char *sem_model_expr_type(const SemModel *m, uint32_t node_id);

/* The distinct tier-2 subtype name recorded for an expression (e.g. "meters"), or NULL. The
 * typechecker prefers this over expr_type to enforce distinct-by-default; lowering ignores it. */
void sem_model_set_expr_nominal(SemModel *m, uint32_t node_id, const char *name);
const char *sem_model_expr_nominal(const SemModel *m, uint32_t node_id);

void sem_model_set_bind_alias(SemModel *m, uint32_t node_id);
int sem_model_bind_alias(const SemModel *m, uint32_t node_id);

#endif /* SEM_MODEL_H */
