#ifndef SEM_DECLS_H
#define SEM_DECLS_H

/* AST-kill: the resolved per-decl signature side-table (`DeclSummary`) that supersedes walking the
 * legacy `AstProgram` during analysis and typechecking. Shared by semantic.c (which builds it) and
 * tycheck.c (which reads it). Bodies are navigated via the `body_node` SyntaxView, never stored. */

#include "../syntax/syntax_view.h"
#include "../syntax/type_ref.h"

typedef struct SemanticContext SemanticContext;

typedef struct {
	char *name;    /* owned, resolved; may be NULL for unnamed positions */
	TypeRef *type; /* pooled root, may be NULL */
	int is_own;
	SourceLoc loc;
} ParamSummary;

typedef struct {
	char *name;    /* owned, resolved */
	TypeRef *type; /* pooled root */
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
	TypeRef **return_types; /* func returns (pooled roots) */
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
	SyntaxView const_value;    /* the RHS value node view (for resolve + loc), NULL node if none */
	int const_value_kind;      /* ExpressionType of the RHS value (EXPR_LITERAL/EXPR_NAME/…), or -1 */
	char *const_value_lexeme;  /* owned; the literal lexeme if the RHS is a literal, else NULL */
	char *const_value_name;    /* owned; the bare-name RHS (for the value/alias fixpoint), else NULL */
	SourceLoc const_value_loc; /* diagnostics + registry loc */
	TypeRef *const_type_value; /* pooled; the `: type :` RHS type-form (alias/tuple/callable), else NULL */
	TypeRef *const_decl_type;  /* pooled; the explicit declared type (`PI : float : x`), else NULL */
	int is_transparent;        /* `k :: alias T` tier-1 transparent */
	/* static */
	int static_kind;      /* STATIC_KIND_* or -1 if not a static decl */
	TypeRef *static_type; /* ARRAY: element type; SCALAR: scalar type (pooled); else NULL */
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
	int is_drop;
	char *drop_type;
	char **allow_slugs;
	int allow_slug_count;
} DeclSummary;

/* DeclTable accessors (defined in semantic.c) for out-of-file readers like tycheck. */
int semantic_decl_count(const SemanticContext *ctx);
const DeclSummary *semantic_decl_at(const SemanticContext *ctx, int i);
const DeclSummary *semantic_find_callable_sig(const SemanticContext *ctx, const char *name);

/* The resolved callee name of an SN_CALL_EXPR view (qualify-mangled for `mod.f`, else the bare
 * SN_CALLEE_NAME), or NULL for a non-module qualified field call. Caller frees. */
char *semantic_call_callee_name(SemanticContext *ctx, SyntaxView call);

/* A pooled TypeRef built from a type-node view (lives in the context type pool). NULL if absent. */
TypeRef *semantic_type_from_view(SemanticContext *ctx, SyntaxView t);

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
