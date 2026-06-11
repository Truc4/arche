#ifndef SEM_HINTS_H
#define SEM_HINTS_H

/* Editor-facing inferred facts that are NOT in the syntax tree and can't be a local
 * syntactic lens — they need whole-program resolution. Kept OUT of the syntax
 * tree, keyed by syntax tree node id, parallel to SemModel. Read by the analyzer's
 * "explicit view"; not used by the compile pipeline.
 *
 * Currently: the callee parameter a call argument binds to (name + `own`-ness),
 * which requires resolving the call to a specific signature. Strings are OWNED
 * (copied in; freed by sem_hints_free).
 *
 * (Implicit move/copy belongs here too, but the language requires explicit
 * move/copy today — see the implicit-move TODO in semantic.c — so there is no
 * implicit decision to record yet.) */

#include "../syntax/syntax_tree.h"
#include <stdint.h>

typedef struct {
	const char **param_name; /* indexed by syntax tree node id; the call arg's resolved param name, or NULL */
	uint8_t *param_is_own;   /* indexed by syntax tree node id; 1 if that parameter is `own` */
	uint8_t *elided_move;    /* indexed by syntax tree node id; 1 if a bare name was consumed (elided `move`) */
	uint8_t *policy_proven;  /* indexed by syntax tree node id; 1 if a fallible index/slice op was proven in-bounds (no implicit policy) */
	int cap;
} SemHints;

SemHints *sem_hints_new(void);
void sem_hints_free(SemHints *h);

/* Record that the call argument at `node_id` binds to a parameter named `name`
 * (copied) which is `own` iff is_own. */
void sem_hints_set_param(SemHints *h, uint32_t node_id, const char *name, int is_own);
const char *sem_hints_param_name(const SemHints *h, uint32_t node_id);
int sem_hints_param_is_own(const SemHints *h, uint32_t node_id);

/* Record that the bare name at `node_id` was consumed — the source elided an explicit `move`
 * (canonical form is `b := move a` / `f(move a)`). The editor renders an inlay "move" hint. */
void sem_hints_set_elided_move(SemHints *h, uint32_t node_id);
int sem_hints_is_elided_move(const SemHints *h, uint32_t node_id);

/* Record that the fallible index/slice op at `node_id` was proven in-bounds by the bounds prover —
 * it carries no implicit failure policy, so the editor renders no ghost `!clamp`/`!abort` inlay. */
void sem_hints_set_policy_proven(SemHints *h, uint32_t node_id);
int sem_hints_is_policy_proven(const SemHints *h, uint32_t node_id);

#endif /* SEM_HINTS_H */
