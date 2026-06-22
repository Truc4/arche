#ifndef SYNTAX_TREE_H
#define SYNTAX_TREE_H

/* Lossless concrete syntax tree (green tree) + builder.
 *
 * Modelled on rust-analyzer's rowan GreenNodeBuilder. Every syntactic token and
 * comment is a leaf carrying an exact byte span; nodes group children and carry
 * the span covering them. Whitespace is not stored — it is the gap between
 * adjacent leaves and is recoverable from the source buffer.
 *
 * The builder is "checkpoint/wrap": tokens are appended as they are consumed, and
 * a range of already-appended children can be retroactively wrapped into a node.
 * A forgotten wrap yields a flatter — but still lossless — tree.
 */

#include "../lexer/lexer.h" /* TokenKind */
#include <stddef.h>
#include <stdint.h>

/* Node kinds. Token leaves reuse the lexer's TokenKind; these name the interior
 * nodes the parser wraps. The set mirrors the grammar so the syntax tree is structurally
 * complete: every construct is a node, not just identifier roles. */
typedef enum {
	SN_SOURCE_FILE,

	/* Declarations */
	SN_WORLD_DECL,
	SN_ARCHETYPE_DECL,
	SN_PROC_DECL,
	SN_MAP_DECL,
	SN_FUNC_DECL,
	SN_FUNC_GROUP_DECL,
	SN_STATIC_DECL,
	SN_CONST_DECL,
	SN_DEFAULT_DECL, /* `@default(<kind>, <category>, <policy>)` — standalone directive setting the
	                  * program's failure-policy default for one (effect-kind, op-category) cell */
	SN_USE_DECL,
	SN_REGION,   /* `#module`/`#file`/`#foreign` region marker — banner (narrows following decls to
	              * end-of-file) or, when it carries a `{ ... }` body, a bounded block of child decls */
	SN_RUN_DECL, /* `#run <expr>` — the program's Schedule value; one expression child */

	/* Structure */
	SN_PARAM_LIST,
	SN_PARAM,
	SN_OUT_PARAM, /* an out-parameter of a proc: `name: T` in the second `(...)` list */
	SN_OUT_ARG,   /* an out-argument at a proc call site: `name`, `name:`, or `name: T` */
	SN_FIELD_DECL,
	SN_RETURN_TYPES, /* the `-> (T, ...)` of a func */
	SN_ARG_LIST,     /* call argument list */

	/* Statements */
	SN_BIND_STMT,
	SN_ASSIGN_STMT,
	SN_FOR_STMT,
	SN_IF_STMT,
	SN_ELSE_CLAUSE,
	SN_BREAK_STMT,
	SN_CONTINUE_STMT,
	SN_RUN_STMT,
	SN_EXPR_STMT,
	SN_RETURN_STMT,
	SN_MULTI_BIND_STMT,
	SN_PROC_CALL_STMT, /* `foo(in)(out)` — an action with out-arguments */
	SN_EACH_FIELD_STMT,
	SN_BLOCK,      /* a standalone `{ … }` block statement (a nested scope) */
	SN_MATCH_STMT, /* `match expr { pat : body, … }` — exhaustive dispatch */
	SN_MATCH_ARM,  /* one arm: a pattern (variant / literal / `_`) + body */

	/* Expressions */
	SN_LITERAL_EXPR,
	SN_NAME_EXPR,
	SN_FIELD_EXPR,
	SN_INDEX_EXPR,
	SN_SLICE_EXPR,
	SN_BINARY_EXPR,
	SN_UNARY_EXPR,
	SN_CALL_EXPR,
	SN_ALLOC_EXPR,
	SN_ARRAY_LIT_EXPR,
	SN_STRING_EXPR,
	SN_ENTITY_EXPR, /* entity literal value: `Name { field: val, ... }` — a row of archetype/query Name */
	SN_TUPLE_LIT,   /* a tuple value `(a, b)` — only as an entity field value for a tuple-group column */
	SN_PAREN_EXPR,
	/* Unified-grammar RHS value forms: the name is the binding LHS, so these carry no
	 * SN_FUNC_DEF_NAME. A bodied (or `extern`) proc/func is a value; a bodiless one is a
	 * type (SN_TYPE_PROC / SN_TYPE_FUNC below). */
	SN_PROC_EXPR,    /* proc value literal: `proc(in)(out){body}` or `extern proc(in)(out)` */
	SN_FUNC_EXPR,    /* func value literal: `func(in)->T{body}` */
	SN_POLICY_EXPR,  /* policy value literal: `policy(in)->T{body}` — a failure-policy decl */
	SN_GROUP_EXPR,   /* Odin-style overload group: `proc{a,b}` / `func{a,b}` */
	SN_ARCH_EXPR,    /* archetype (record type) definition: `archetype{ fields }` */
	SN_MAP_EXPR,     /* map definition: `map(<query>){body}` — runs over a query */
	SN_SYSTEM_EXPR,  /* system definition: `system { body }` — the composer; invoked by `#schedule` */
	SN_EACH_EXPR,    /* each definition: `each(<query>){body}` — the per-element fan (scalars, control flow) */
	SN_QUERY_EXPR,   /* query definition: `query { col, col }` — an archetype-selecting column set */
	SN_ENUM_EXPR,    /* enum type definition: `enum { a, b = 2, c }` */
	SN_ENUM_VARIANT, /* one enum variant: name + optional `= N` */
	SN_SUM_EXPR,     /* sum (tagged-union) type definition: `sum { a(T), b([]Self), c }` */
	SN_SUM_VARIANT,  /* one sum variant: name + optional `(type, ...)` payload type list */

	/* Types (children of / refinements within a type position) */
	SN_TYPE_REF, /* a type position: identifiers within are types */
	SN_TYPE_ARRAY,
	SN_TYPE_SHAPED_ARRAY,
	SN_TYPE_TUPLE,
	SN_TYPE_HANDLE,
	SN_TYPE_EFF,  /* a not-yet-run effect value type: `Eff(T…)` — the parenthesized out-slot types. Kept
	               * inside the SN_TYPE_REF..SN_TYPE_FUNC range so the "is a type node" guards include it. */
	SN_TYPE_PROC, /* a proc type (bodiless signature): `proc(in)(out)` */
	SN_TYPE_FUNC, /* a func type (bodiless signature): `func(in)->T` */

	/* Identifier-role leaves (kept for the highlighter's classification; these
	 * wrap a single identifier token inside the structural nodes above) */
	SN_TYPE_DEF_NAME, /* archetype name at its declaration (a type definition) */
	SN_FUNC_DEF_NAME, /* proc / map / func name at its declaration */
	SN_FIELD_NAME,    /* field-decl name, or the name in a `.field` access */
	SN_PARAM_NAME,    /* parameter name */
	SN_CALLEE_NAME,   /* the callee identifier of a call expression */
	SN_QUERY_REF,     /* a query name naming the query a `map(Name)` runs over */
	SN_ALLOC_TYPE,    /* the archetype name in `alloc Name(...)` */
	SN_NAME_REF,      /* any other identifier reference (a variable) */
	SN_POLICY_REF,    /* `!name` failure-policy marker trailing a fallible op (index/slice/call/pool-cap) */

	/* Error recovery: tokens consumed during synchronize() */
	SN_ERROR,
} SyntaxNodeKind;

typedef enum {
	SE_NODE,
	SE_TOKEN,
} SyntaxElemKind;

typedef struct SyntaxNode SyntaxNode;

/* A child of a node: either a sub-node or a token leaf. */
typedef struct SyntaxElem {
	SyntaxElemKind tag;
	union {
		SyntaxNode *node;
		struct {
			TokenKind kind;
			uint32_t offset; /* byte offset into source */
			uint32_t length; /* byte length */
			int line;        /* 1-based */
			int column;      /* 1-based */
		} token;
	} as;
} SyntaxElem;

struct SyntaxNode {
	SyntaxNodeKind kind;
	uint32_t id;     /* dense post-order id, 0..N-1; the root has the largest. Keys side tables. */
	uint32_t offset; /* span covering all children */
	uint32_t length;
	SyntaxElem *children;
	int child_count;
};

/* Builder. Holds a flat list of the current root-level children; wrap() collapses
 * a sub-range into a node in place. */
typedef struct CstBuilder {
	SyntaxElem *items;
	int count;
	int cap;
} CstBuilder;

CstBuilder *syntax_builder_new(void);
void syntax_builder_free(CstBuilder *b); /* frees the builder; not a finished tree */

/* Append a consumed token / comment as a leaf at the current root level. */
void syntax_builder_token(CstBuilder *b, TokenKind kind, uint32_t offset, uint32_t length, int line, int column);

/* Record the current position so a later wrap() can group children added after it. */
int syntax_builder_checkpoint(CstBuilder *b);

/* Group items[checkpoint .. count) into a single node of `kind`, in place.
 * Returns the created node (whose pointer is stable for the tree's lifetime, so
 * callers may stash it to correlate other structures with this syntax tree node), or
 * NULL if there was nothing to wrap. */
SyntaxNode *syntax_builder_wrap(CstBuilder *b, int checkpoint, SyntaxNodeKind kind);

/* Wrap everything into a SOURCE_FILE node and return it. The builder is consumed
 * (free it with syntax_builder_free). The returned tree is owned by the caller. */
SyntaxNode *syntax_builder_finish(CstBuilder *b);

void syntax_node_free(SyntaxNode *n);

/* Human-readable node-kind name (for tree dumps / debugging). */
const char *syntax_node_kind_name(SyntaxNodeKind kind);

#endif /* SYNTAX_TREE_H */
