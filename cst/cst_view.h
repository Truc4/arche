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

/* Does the subtree contain an SN_ERROR node? */
int cv_has_error(CstView v);

#endif /* CST_VIEW_H */
