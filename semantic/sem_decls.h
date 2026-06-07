#ifndef SEM_DECLS_H
#define SEM_DECLS_H

/* AST-kill: the resolved per-decl signature side-table (`DeclSummary`) that supersedes walking the
 * legacy `AstProgram` during analysis and typechecking. Shared by semantic.c (which builds it) and
 * tycheck.c (which reads it). Bodies are navigated via the `body_node` SyntaxView, never stored. */

#include "../syntax/syntax_view.h"
#include "../syntax/type_ref.h"
#include "sem_ids.h"
#include "sem_types.h"

typedef struct SemanticContext SemanticContext;

/* Resolved visibility of a decl — the region-band model made first-class (docs/devices.md:89).
 * VIS_EXPORTED is the zero value so calloc'd (entry-file) decls default to public, which is correct:
 * a decl with no `#module`/`#file` banner is the exported surface. Module decls are stamped explicitly
 * in sem_add_module_decl from the loader's `exported`/`file_local` bands. */
typedef enum {
	VIS_EXPORTED, /* no banner — public/exported surface */
	VIS_UNIT,     /* `#module` — visible across the unit's files, hidden from importers */
	VIS_FILE,     /* `#file` — visible only within its own file */
} Visibility;

typedef struct {
	char *name;           /* owned, resolved; may be NULL for unnamed positions */
	SyntaxView type_node; /* the param's type syntax node (interned into type_id post-analysis) */
	TypeId type_id;       /* interned identity of the param type */
	int is_own;
	SourceLoc loc;
} ParamSummary;

typedef struct {
	char *name;           /* owned, resolved */
	SyntaxView type_node; /* explicit type syntax node, or absent (then the field NAME is its type) */
	TypeId type_id;       /* interned field type */
	FieldKind kind;
	int meta_explicit;
	SourceLoc loc;
} FieldSummary;

typedef struct {
	DeclKind kind;
	char *name;           /* owned, resolved; NULL if the decl has no name */
	SyntaxView node;      /* the decl's syntax view (== AST decl->sn) */
	SyntaxView body_node; /* the node whose direct children are the body statements (the SN_*_EXPR
	                         value-form for a unified `name :: proc(){…}`, else == node) */
	SourceLoc loc;
	/* proc / func / sys signature */
	ParamSummary *params;
	int param_count;
	ParamSummary *out_params; /* proc out-params */
	int out_param_count;
	SyntaxView *return_type_nodes; /* func return type syntax nodes */
	TypeId *return_type_ids;       /* interned return identities */
	int return_type_count;
	int is_extern;
	int is_variadic;
	int allow_pure_proc;
	/* func group */
	char **member_names; /* owned, resolved */
	int member_count;
	/* archetype */
	FieldSummary *fields;
	int field_count;
	/* enum */
	char **enum_variant_names; /* owned */
	long *enum_variant_values;
	int enum_variant_count;
	/* const (`name :: value` / `name : [type] : value`) */
	SyntaxView const_value;     /* the RHS value node view (for resolve + loc), NULL node if none */
	int const_value_kind;       /* ExpressionType of the RHS value (EXPR_LITERAL/EXPR_NAME/…), or -1 */
	char *const_value_lexeme;   /* owned; the literal lexeme if the RHS is a literal, else NULL */
	char *const_value_name;     /* owned; the bare-name RHS (for the value/alias fixpoint), else NULL */
	SourceLoc const_value_loc;  /* diagnostics + registry loc */
	TypeId const_type_value_id; /* interned `: type :` RHS type-form (alias/tuple/callable), else UNKNOWN */
	TypeId const_decl_type_id;  /* interned explicit declared type (`PI : float : x`), else UNKNOWN */
	int is_transparent;         /* `k :: alias T` tier-1 transparent */
	/* static */
	int static_kind;       /* STATIC_KIND_* or -1 if not a static decl */
	TypeId static_type_id; /* ARRAY: element type; SCALAR: scalar type; interned */
	int is_requirement;
	int static_size;                /* ARRAY: declared element count */
	int static_has_init;            /* ARRAY: 1 if an initializer was written */
	SyntaxView static_init;         /* SCALAR: the scalar initializer value view (NULL node = none) */
	SyntaxView *static_fields;      /* POOL: per-field value views (field 0 = count); owned array */
	int static_field_count;         /* POOL: number of field values */
	int static_pool_count;          /* POOL: field 0 as an int if a literal, else -1 */
	int static_init_length_present; /* POOL: 1 if an init_size argument was given */
	/* device / datasheet provenance + suppressions (cross-decl sweeps read these) */
	int from_device_impl;
	int is_datasheet;
	Visibility visibility; /* region-band visibility; VIS_EXPORTED (0) for entry-file decls */
	int unit;              /* owning compilation unit: 0 = entry/root program, >0 = a module (UnitInterface.unit_id) */
	DeclOrigin origin;     /* provenance of the owning unit; DECL_ORIGIN_ENTRY (0) for entry-file decls */
	int is_drop;
	char *drop_type;
	char **allow_slugs;
	int allow_slug_count;
} DeclSummary;

/* The exported surface of one compilation unit (module) — a first-class, persisted interface. Today
 * arche resolves cross-unit `mod.member` references against these (the inlining of bodies is for
 * codegen/monomorphization, NOT resolution). Each `exports` entry is a `"member=identity"` pair: the
 * member name as written at the use site, and the qualified/foreign link identity it resolves to.
 * Owned by the SemanticContext; the registry replaces the old transient, 64-capped `acc` arrays. */
typedef struct {
	int unit_id;     /* >0; stable identity of this unit (decls carry it in DeclSummary.unit) */
	char *unit_name; /* module name */
	char **exports;  /* owned; "member=identity" entries (qualify-pass format) */
	int export_count;
} UnitInterface;

/* The context's interned TypeId arena (Phase 3), shared by analysis + tycheck. */
TypeArena *sem_context_arena(SemanticContext *ctx);

/* Intern a type into the context arena (Phase 3). The shared resolvers used by BOTH the DeclSummary
 * builder and tycheck, so a given type interns to the SAME TypeId everywhere. `sem_tyid_of_name`
 * resolves a type-NAME through the alias chain (tier-2 subtype interns by its own name);
 * `sem_intern_view` builds straight from a syntax type-node view. */
TypeId sem_tyid_of_name(SemanticContext *ctx, const char *name);
TypeId sem_intern_view(SemanticContext *ctx, SyntaxView t);

/* DeclTable accessors (defined in semantic.c) for out-of-file readers like tycheck. */
int semantic_decl_count(const SemanticContext *ctx);
const DeclSummary *semantic_decl_at(const SemanticContext *ctx, int i);
const DeclSummary *semantic_find_callable_sig(const SemanticContext *ctx, const char *name);

/* The resolved callee name of an SN_CALL_EXPR view (qualify-mangled for `mod.f`, else the bare
 * SN_CALLEE_NAME), or NULL for a non-module qualified field call. Caller frees. */
char *semantic_call_callee_name(SemanticContext *ctx, SyntaxView call);

/* Shared syntax-view navigation (defined in semantic.c) so semantic + tycheck read the tree the
 * same way. Indices count only expression / statement children. */
SyntaxView sem_first_expr(SyntaxView v);
SyntaxView sem_node_at_expr(SyntaxView v, int idx);
SyntaxView sem_type_at(SyntaxView v, int idx);
SyntaxView sem_stmt_at(SyntaxView v, int idx);
int sem_stmt_count(SyntaxView v);
int sem_expr_count(SyntaxView v);
Operator sem_binary_op(SyntaxView v);
char *sem_cv_dup_first_token(SyntaxView v);
SourceLoc sem_node_loc(const SyntaxNode *n);

#endif /* SEM_DECLS_H */
