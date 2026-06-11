#ifndef SEM_MODEL_H
#define SEM_MODEL_H

/* Semantic side model: resolved facts keyed by syntax tree node id, kept OUT of the
 * syntax tree (the tree stays immutable). Semantic writes here; lowering reads
 * here. The model is the single home for resolved type info — the tree is
 * never mutated by analysis.
 *
 * The callee_name/ref_name string channels are OWNED by the model (copied in, freed at teardown). */

#include "../syntax/syntax_tree.h"
#include "sem_ids.h"
#include "sem_types.h"
#include <stdint.h>

typedef struct {
	TypeId *expr_type_id;     /* Phase 3: interned identity of an expression (the nominal-preferring type),
	                             keyed by node id. The home for an expression's type — read by tycheck +
	                             lowering; rendered via tyid_display at the LSP hover edge. */
	uint8_t *bind_alias;      /* indexed by syntax tree node id; 1 if a bind is a compile-time type alias */
	uint8_t *implicit_move;   /* indexed by syntax tree node id; 1 if a bare move-only name in an ownership-
	                             taking position is an IMPLICIT move. Semantic decides this (it has the
	                             types); lowering materializes an explicit `move` HIR node so codegen
	                             has a single move path (no syntax re-derivation downstream). */
	uint8_t *policy_elided;   /* indexed by an index/slice op node id; 1 if the bounds prover proved it
	                             in-bounds ⇒ no failure policy applies. THE single provability verdict:
	                             read by the analyzer (no ghost inlay) and by lowering→codegen (elide the
	                             policy macro). Neither re-derives. */
	const char **callee_name; /* indexed by an SN_CALL_EXPR node id; the call's resolved callee name —
	                             for `mod.f` the qualify pass's mangled/foreign link name, else the bare
	                             name. Lets a syntax-tree-driven resolver read what the qualify pass
	                             resolved, instead of re-deriving module export lookups. */
	const char **ref_name;    /* indexed by a NAME/FIELD/INDEX/SLICE node id; the resolved leftmost
	                             name (module-inlined references are prefixed in the AST, e.g.
	                             `csv.load_cols`). Lets view-driven analysis look names up under the
	                             identity the AST resolved them to, not the bare token. */
	DefId *callee_def;        /* indexed by an SN_CALL_EXPR node id; the DefId the callee_name resolves
	                             to in the decl table, or DEFID_NONE (extern/libc/builtin/unresolved).
	                             The id-keyed twin of callee_name — analysis traverses this, not the
	                             string. Populated post-resolution (sem_bind_defs). */
	DefId *ref_def;           /* indexed by a NAME node id; the DefId ref_name resolves to (a func/proc
	                             named as a value, e.g. a callback arg), or DEFID_NONE. */
	int cap;
} SemModel;

SemModel *sem_model_new(void);
void sem_model_free(SemModel *m); /* frees the tables, not the borrowed strings */

/* Phase 3 interned-type channel (keyed by node id). */
void sem_model_set_expr_type_id(SemModel *m, uint32_t node_id, TypeId t);
TypeId sem_model_expr_type_id(const SemModel *m, uint32_t node_id);
int sem_model_cap(const SemModel *m);

void sem_model_set_bind_alias(SemModel *m, uint32_t node_id);
int sem_model_bind_alias(const SemModel *m, uint32_t node_id);

/* A bare move-only name handed to an ownership-taking position (own param / bind / assign RHS) is an
 * implicit move. Semantic records it; lowering wraps the HIR expr in an explicit `move`. */
void sem_model_set_implicit_move(SemModel *m, uint32_t node_id);
int sem_model_implicit_move(const SemModel *m, uint32_t node_id);

void sem_model_set_policy_elided(SemModel *m, uint32_t node_id);
int sem_model_policy_elided(const SemModel *m, uint32_t node_id);

/* The resolved callee name for an SN_CALL_EXPR node (qualify-mangled for `mod.f`, else the bare
 * name). NULL if not recorded (e.g. a non-module qualified field call). Model owns its copy. */
void sem_model_set_callee_name(SemModel *m, uint32_t node_id, const char *name);
const char *sem_model_callee_name(const SemModel *m, uint32_t node_id);

void sem_model_set_ref_name(SemModel *m, uint32_t node_id, const char *name);
const char *sem_model_ref_name(const SemModel *m, uint32_t node_id);

/* Id-keyed resolution channels (the twins of callee_name/ref_name). Get returns DEFID_NONE when
 * unset or out of range. */
void sem_model_set_callee_def(SemModel *m, uint32_t node_id, DefId d);
DefId sem_model_callee_def(const SemModel *m, uint32_t node_id);
void sem_model_set_ref_def(SemModel *m, uint32_t node_id, DefId d);
DefId sem_model_ref_def(const SemModel *m, uint32_t node_id);

#endif /* SEM_MODEL_H */
