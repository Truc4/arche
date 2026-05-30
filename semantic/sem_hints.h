#ifndef SEM_HINTS_H
#define SEM_HINTS_H

/* Editor-facing inferred facts that are NOT in the CST and can't be a local
 * syntactic lens — they need whole-program resolution. Kept OUT of the syntax
 * tree, keyed by CST node id, parallel to SemModel. Read by the analyzer's
 * "explicit view"; not used by the compile pipeline.
 *
 * Currently: the callee parameter a call argument binds to (name + `own`-ness),
 * which requires resolving the call to a specific signature. Strings are OWNED
 * (copied in; freed by sem_hints_free).
 *
 * (Implicit move/copy belongs here too, but the language requires explicit
 * move/copy today — see the implicit-move TODO in semantic.c — so there is no
 * implicit decision to record yet.) */

#include "../cst/syntax_tree.h"
#include <stdint.h>

typedef struct {
	const char **param_name; /* indexed by CST node id; the call arg's resolved param name, or NULL */
	uint8_t *param_is_own;   /* indexed by CST node id; 1 if that parameter is `own` */
	uint8_t *effect_call;    /* indexed by CST node id; 1 if this call node is a bare effectful (proc/
	                            extern) call whose out-list is implied — editor shows a ghost `()` */
	int cap;
} SemHints;

SemHints *sem_hints_new(void);
void sem_hints_free(SemHints *h);

/* Record that the call argument at `node_id` binds to a parameter named `name`
 * (copied) which is `own` iff is_own. */
void sem_hints_set_param(SemHints *h, uint32_t node_id, const char *name, int is_own);
const char *sem_hints_param_name(const SemHints *h, uint32_t node_id);
int sem_hints_param_is_own(const SemHints *h, uint32_t node_id);

/* Record / query that the call at `node_id` is a bare effectful (proc/extern) call — its empty
 * out-list `()` is implied and the editor should render it so readers see the potential effect. */
void sem_hints_set_effect_call(SemHints *h, uint32_t node_id);
int sem_hints_is_effect_call(const SemHints *h, uint32_t node_id);

#endif /* SEM_HINTS_H */
