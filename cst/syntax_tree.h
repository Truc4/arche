#ifndef SYNTAX_TREE_H
#define SYNTAX_TREE_H

/* Lossless concrete syntax tree (green tree) + builder.
 *
 * Modelled on rust-analyzer's rowan GreenNodeBuilder. Every syntactic token and
 * comment is a leaf carrying an exact byte span; nodes group children and carry
 * the span covering them. Whitespace is not stored — it is the gap between
 * adjacent leaves and is recoverable from the source buffer (see docs/LOSSLESS_CST.md).
 *
 * The builder is "checkpoint/wrap": tokens are appended as they are consumed, and
 * a range of already-appended children can be retroactively wrapped into a node.
 * A forgotten wrap yields a flatter — but still lossless — tree.
 */

#include "../lexer/lexer.h" /* TokenKind */
#include <stddef.h>
#include <stdint.h>

/* Node kinds. Token leaves reuse the lexer's TokenKind; these name the interior
 * nodes the parser wraps. The set mirrors the grammar so the CST is structurally
 * complete: every construct is a node, not just identifier roles. */
typedef enum {
	SN_SOURCE_FILE,

	/* Declarations */
	SN_WORLD_DECL,
	SN_ARCHETYPE_DECL,
	SN_PROC_DECL,
	SN_SYS_DECL,
	SN_FUNC_DECL,
	SN_FUNC_GROUP_DECL,
	SN_STATIC_DECL,
	SN_CONST_DECL,
	SN_USE_DECL,

	/* Structure */
	SN_PARAM_LIST,
	SN_PARAM,
	SN_OUT_PARAM, /* an out-parameter of a proc: `name: T` in the second `(...)` list */
	SN_FIELD_DECL,
	SN_BLOCK,        /* a `{ ... }` statement body */
	SN_RETURN_TYPES, /* the `-> (T, ...)` of a func */
	SN_ARG_LIST,     /* call argument list */

	/* Statements */
	SN_BIND_STMT,
	SN_ASSIGN_STMT,
	SN_FOR_STMT,
	SN_IF_STMT,
	SN_ELSE_CLAUSE,
	SN_BREAK_STMT,
	SN_RUN_STMT,
	SN_EXPR_STMT,
	SN_RETURN_STMT,
	SN_MULTI_BIND_STMT,
	SN_EACH_FIELD_STMT,

	/* Expressions */
	SN_LITERAL_EXPR,
	SN_NAME_EXPR,
	SN_FIELD_EXPR,
	SN_INDEX_EXPR,
	SN_BINARY_EXPR,
	SN_UNARY_EXPR,
	SN_CALL_EXPR,
	SN_ALLOC_EXPR,
	SN_ARRAY_LIT_EXPR,
	SN_STRING_EXPR,
	SN_PAREN_EXPR,

	/* Types (children of / refinements within a type position) */
	SN_TYPE_REF, /* a type position: identifiers within are types */
	SN_TYPE_ARRAY,
	SN_TYPE_SHAPED_ARRAY,
	SN_TYPE_TUPLE,
	SN_TYPE_HANDLE,

	/* Identifier-role leaves (kept for the highlighter's classification; these
	 * wrap a single identifier token inside the structural nodes above) */
	SN_TYPE_DEF_NAME, /* archetype name at its declaration (a type definition) */
	SN_FUNC_DEF_NAME, /* proc / sys / func name at its declaration */
	SN_FIELD_NAME,    /* field-decl name, or the name in a `.field` access */
	SN_PARAM_NAME,    /* parameter name */
	SN_CALLEE_NAME,   /* the callee identifier of a call expression */
	SN_ALLOC_TYPE,    /* the archetype name in `alloc Name(...)` */
	SN_NAME_REF,      /* any other identifier reference (a variable) */

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

CstBuilder *cst_builder_new(void);
void cst_builder_free(CstBuilder *b); /* frees the builder; not a finished tree */

/* Append a consumed token / comment as a leaf at the current root level. */
void cst_builder_token(CstBuilder *b, TokenKind kind, uint32_t offset, uint32_t length, int line, int column);

/* Record the current position so a later wrap() can group children added after it. */
int cst_builder_checkpoint(CstBuilder *b);

/* Group items[checkpoint .. count) into a single node of `kind`, in place.
 * Returns the created node (whose pointer is stable for the tree's lifetime, so
 * callers may stash it to correlate other structures with this CST node), or
 * NULL if there was nothing to wrap. */
SyntaxNode *cst_builder_wrap(CstBuilder *b, int checkpoint, SyntaxNodeKind kind);

/* Wrap everything into a SOURCE_FILE node and return it. The builder is consumed
 * (free it with cst_builder_free). The returned tree is owned by the caller. */
SyntaxNode *cst_builder_finish(CstBuilder *b);

void syntax_node_free(SyntaxNode *n);

/* Human-readable node-kind name (for tree dumps / debugging). */
const char *syntax_node_kind_name(SyntaxNodeKind kind);

#endif /* SYNTAX_TREE_H */
