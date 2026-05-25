#ifndef SEM_MODEL_H
#define SEM_MODEL_H

/* Semantic side model: resolved facts keyed by CST node id, kept OUT of the
 * syntax tree (the tree stays immutable). During the migration this mirrors what
 * semantic used to write onto Expression.resolved_type; lowering reads it here.
 *
 * Type strings are BORROWED for now (still owned by Expression.resolved_type);
 * ownership moves here once the tree mutation is removed. */

#include "../cst/syntax_tree.h"
#include <stdint.h>

typedef struct {
	const char **expr_type; /* indexed by CST node id; NULL where unresolved */
	int cap;
} SemModel;

SemModel *sem_model_new(void);
void sem_model_free(SemModel *m); /* frees the table, not the borrowed strings */

void sem_model_set_expr_type(SemModel *m, uint32_t node_id, const char *type);
const char *sem_model_expr_type(const SemModel *m, uint32_t node_id);

#endif /* SEM_MODEL_H */
