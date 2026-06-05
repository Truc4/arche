#ifndef SEM_MODEL_H
#define SEM_MODEL_H

/* Semantic side model: resolved facts keyed by syntax tree node id, kept OUT of the
 * syntax tree (the tree stays immutable). Semantic writes here; lowering reads
 * here. The model is the single home for resolved type info — the tree is
 * never mutated by analysis.
 *
 * Type strings are OWNED by the model: sem_model_set_expr_type copies them in
 * and sem_model_free releases them. Consumers (lowering -> HIR) borrow these
 * pointers, so the model must outlive them (in the compiler, until codegen). */

#include "../syntax/syntax_tree.h"
#include <stdint.h>

typedef struct {
	const char **expr_type;    /* indexed by syntax tree node id; NULL where unresolved (RESOLVED backing) */
	const char **expr_nominal; /* indexed by syntax tree node id; the distinct tier-2 subtype name, else NULL.
	                              Lowering reads expr_type (resolved); the typechecker prefers this. */
	uint8_t *bind_alias;       /* indexed by syntax tree node id; 1 if a bind is a compile-time type alias */
	uint8_t *implicit_move;    /* indexed by syntax tree node id; 1 if a bare move-only name in an ownership-
	                              taking position is an IMPLICIT move. Semantic decides this (it has the
	                              types); lowering materializes an explicit `move` HIR node so codegen
	                              has a single move path (no syntax re-derivation downstream). */
	const char **callee_name;  /* indexed by an SN_CALL_EXPR node id; the call's resolved callee name —
	                              for `mod.f` the qualify pass's mangled/foreign link name, else the bare
	                              name. Lets a syntax-tree-driven resolver read what the qualify pass
	                              resolved, instead of re-deriving module export lookups. */
	const char **ref_name;     /* indexed by a NAME/FIELD/INDEX/SLICE node id; the resolved leftmost
	                              name (module-inlined references are prefixed in the AST, e.g.
	                              `csv.load_cols`). Lets view-driven analysis look names up under the
	                              identity the AST resolved them to, not the bare token. */
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

/* A bare move-only name handed to an ownership-taking position (own param / bind / assign RHS) is an
 * implicit move. Semantic records it; lowering wraps the HIR expr in an explicit `move`. */
void sem_model_set_implicit_move(SemModel *m, uint32_t node_id);
int sem_model_implicit_move(const SemModel *m, uint32_t node_id);

/* The resolved callee name for an SN_CALL_EXPR node (qualify-mangled for `mod.f`, else the bare
 * name). NULL if not recorded (e.g. a non-module qualified field call). Model owns its copy. */
void sem_model_set_callee_name(SemModel *m, uint32_t node_id, const char *name);
const char *sem_model_callee_name(const SemModel *m, uint32_t node_id);

void sem_model_set_ref_name(SemModel *m, uint32_t node_id, const char *name);
const char *sem_model_ref_name(const SemModel *m, uint32_t node_id);

#endif /* SEM_MODEL_H */
