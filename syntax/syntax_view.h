#ifndef SYNTAX_VIEW_H
#define SYNTAX_VIEW_H

/* Typed views over the syntax tree — the "abstract syntax" as zero-copy lenses, not a
 * separate tree (rust-analyzer model). A view is just a (node, source) pair; a
 * NULL node means "absent". Text accessors return borrowed slices into the
 * source buffer, so views allocate nothing.
 *
 * These generic accessors are the substrate; construct-specific typed accessors
 * (sv_proc_name, sv_bind_value, …) are thin wrappers built on top as consumers
 * need them. */

#include "syntax_tree.h"
#include <stddef.h>

typedef struct {
	const SyntaxNode *node; /* NULL = absent */
	const char *src;
} SyntaxView;

typedef struct {
	const char *ptr; /* borrowed slice into the source buffer; NOT NUL-terminated */
	size_t len;      /* 0 / ptr NULL when absent */
} SynText;

SyntaxView sv_root(const SyntaxNode *root, const char *src);

static inline int sv_present(SyntaxView v) {
	return v.node != NULL;
}
static inline SyntaxNodeKind sv_kind(SyntaxView v) {
	return v.node->kind;
}
static inline uint32_t sv_id(SyntaxView v) {
	return v.node->id;
}

/* The node's full source span as a borrowed slice (an identifier node's text). */
SynText sv_text(SyntaxView v);
int sv_text_eq(SyntaxView v, const char *s);

/* Direct-child navigation. */
int sv_node_count(SyntaxView v);                                  /* number of child *nodes* */
int sv_node_count_deep(SyntaxView v);                             /* count of all descendant nodes (recursive) */
SyntaxView sv_node_at(SyntaxView v, int index);                   /* nth child node (any kind) */
SyntaxView sv_child(SyntaxView v, SyntaxNodeKind kind);           /* first child node of `kind` */
SyntaxView sv_child_at(SyntaxView v, SyntaxNodeKind kind, int n); /* nth child node of `kind` */
int sv_count(SyntaxView v, SyntaxNodeKind kind);                  /* count of child nodes of `kind` */

/* First direct child *token* of `kind`; .ptr is NULL when absent. */
SynText sv_token(SyntaxView v, TokenKind kind);
int sv_has_token(SyntaxView v, TokenKind kind);

/* Child navigation by syntactic role (expression vs type position), mirroring how
 * the AST reconstruction reads a node. "expr" = SN_LITERAL_EXPR..SN_PAREN_EXPR,
 * "type" = SN_TYPE_REF..SN_TYPE_HANDLE. Absent → NULL node. */
SyntaxView sv_expr_at(SyntaxView v, int idx); /* nth child node in an expression position */
SyntaxView sv_type_at(SyntaxView v, int idx); /* nth child node in a type position */
int sv_type_count(SyntaxView v);              /* number of child nodes in type position */

/* A token leaf's position: 1-based line/column + byte offset/length. line==0 ⇒ absent. */
typedef struct {
	int line;
	int column;
	uint32_t offset;
	uint32_t length;
} CvPos;

/* Position of the first / last token leaf in v's subtree, in source order. line==0 if none.
 * For a simple name node first==last (the identifier); the end column is column+length. */
CvPos sv_first_token_pos(SyntaxView v);
CvPos sv_last_token_pos(SyntaxView v);

/* Position of the first / nth direct-child token of `kind`. line==0 if absent. */
CvPos sv_token_pos(SyntaxView v, TokenKind kind);
CvPos sv_token_pos_at(SyntaxView v, TokenKind kind, int n);

/* Does the subtree contain an SN_ERROR node? */
int sv_has_error(SyntaxView v);

/* Doc comments. Built on the lexer's single doc-comment classifier (no prefix
 * re-checks here). Each returned SynText is the comment line with its marker and
 * one optional leading space stripped — a borrowed slice into the source, NOT
 * NUL-terminated. Returns the count written to out[] (capped at max).
 *
 * sv_decl_doc_lines: the contiguous run of outer `///` comments immediately
 *   preceding `decl` among `root`'s children, in source order. The run is broken
 *   by a blank line, a plain `//` comment, or any intervening node — so only docs
 *   physically attached to the declaration are returned.
 * sv_module_doc_lines: the leading run of inner `//!` comments before the file's
 *   first node.
 * When `out_lines` is non-NULL, out_lines[i] receives the 1-based source line of
 *   the i-th returned comment (so callers can report exact fence positions). */
int sv_decl_doc_lines(SyntaxView root, SyntaxView decl, SynText *out, int *out_lines, int max);
int sv_module_doc_lines(SyntaxView root, SynText *out, int max);

#endif /* SYNTAX_VIEW_H */
