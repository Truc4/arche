#ifndef CST_VIEW_H
#define CST_VIEW_H

/* Typed views over the CST — the "abstract syntax" as zero-copy lenses, not a
 * separate tree (rust-analyzer model). A view is just a (node, source) pair; a
 * NULL node means "absent". Text accessors return borrowed slices into the
 * source buffer, so views allocate nothing.
 *
 * These generic accessors are the substrate; construct-specific typed accessors
 * (cv_proc_name, cv_bind_value, …) are thin wrappers built on top as consumers
 * need them. */

#include "syntax_tree.h"
#include <stddef.h>

typedef struct {
	const SyntaxNode *node; /* NULL = absent */
	const char *src;
} CstView;

typedef struct {
	const char *ptr; /* borrowed slice into the source buffer; NOT NUL-terminated */
	size_t len;      /* 0 / ptr NULL when absent */
} CvText;

CstView cv_root(const SyntaxNode *root, const char *src);

static inline int cv_present(CstView v) {
	return v.node != NULL;
}
static inline SyntaxNodeKind cv_kind(CstView v) {
	return v.node->kind;
}
static inline uint32_t cv_id(CstView v) {
	return v.node->id;
}

/* The node's full source span as a borrowed slice (an identifier node's text). */
CvText cv_text(CstView v);
int cv_text_eq(CstView v, const char *s);

/* Direct-child navigation. */
int cv_node_count(CstView v);                               /* number of child *nodes* */
CstView cv_node_at(CstView v, int index);                   /* nth child node (any kind) */
CstView cv_child(CstView v, SyntaxNodeKind kind);           /* first child node of `kind` */
CstView cv_child_at(CstView v, SyntaxNodeKind kind, int n); /* nth child node of `kind` */
int cv_count(CstView v, SyntaxNodeKind kind);               /* count of child nodes of `kind` */

/* First direct child *token* of `kind`; .ptr is NULL when absent. */
CvText cv_token(CstView v, TokenKind kind);
int cv_has_token(CstView v, TokenKind kind);

/* Child navigation by syntactic role (expression vs type position), mirroring how
 * the AST reconstruction reads a node. "expr" = SN_LITERAL_EXPR..SN_PAREN_EXPR,
 * "type" = SN_TYPE_REF..SN_TYPE_HANDLE. Absent → NULL node. */
CstView cv_expr_at(CstView v, int idx); /* nth child node in an expression position */
CstView cv_type_at(CstView v, int idx); /* nth child node in a type position */
int cv_type_count(CstView v);           /* number of child nodes in type position */

/* A token leaf's position: 1-based line/column + byte offset/length. line==0 ⇒ absent. */
typedef struct {
	int line;
	int column;
	uint32_t offset;
	uint32_t length;
} CvPos;

/* Position of the first / last token leaf in v's subtree, in source order. line==0 if none.
 * For a simple name node first==last (the identifier); the end column is column+length. */
CvPos cv_first_token_pos(CstView v);
CvPos cv_last_token_pos(CstView v);

/* Position of the first / nth direct-child token of `kind`. line==0 if absent. */
CvPos cv_token_pos(CstView v, TokenKind kind);
CvPos cv_token_pos_at(CstView v, TokenKind kind, int n);

/* Does the subtree contain an SN_ERROR node? */
int cv_has_error(CstView v);

/* Doc comments. Built on the lexer's single doc-comment classifier (no prefix
 * re-checks here). Each returned CvText is the comment line with its marker and
 * one optional leading space stripped — a borrowed slice into the source, NOT
 * NUL-terminated. Returns the count written to out[] (capped at max).
 *
 * cv_decl_doc_lines: the contiguous run of outer `///` comments immediately
 *   preceding `decl` among `root`'s children, in source order. The run is broken
 *   by a blank line, a plain `//` comment, or any intervening node — so only docs
 *   physically attached to the declaration are returned.
 * cv_module_doc_lines: the leading run of inner `//!` comments before the file's
 *   first node.
 * When `out_lines` is non-NULL, out_lines[i] receives the 1-based source line of
 *   the i-th returned comment (so callers can report exact fence positions). */
int cv_decl_doc_lines(CstView root, CstView decl, CvText *out, int *out_lines, int max);
int cv_module_doc_lines(CstView root, CvText *out, int max);

#endif /* CST_VIEW_H */
