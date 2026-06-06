#include "syntax_view.h"
#include <string.h>

static SyntaxView none(void) {
	SyntaxView v = {NULL, NULL};
	return v;
}

SyntaxView sv_root(const SyntaxNode *root, const char *src) {
	SyntaxView v = {root, src};
	return v;
}

SynText sv_text(SyntaxView v) {
	SynText t = {NULL, 0};
	if (v.node) {
		t.ptr = v.src + v.node->offset;
		t.len = v.node->length;
	}
	return t;
}

int sv_text_eq(SyntaxView v, const char *s) {
	SynText t = sv_text(v);
	size_t n = strlen(s);
	return t.ptr && t.len == n && memcmp(t.ptr, s, n) == 0;
}

int sv_node_count(SyntaxView v) {
	int c = 0;
	for (int i = 0; i < v.node->child_count; i++)
		if (v.node->children[i].tag == SE_NODE)
			c++;
	return c;
}

int sv_node_count_deep(SyntaxView v) {
	int c = 0;
	for (int i = 0; i < v.node->child_count; i++)
		if (v.node->children[i].tag == SE_NODE) {
			c++;
			c += sv_node_count_deep((SyntaxView){v.node->children[i].as.node, v.src});
		}
	return c;
}

SyntaxView sv_node_at(SyntaxView v, int index) {
	int c = 0;
	for (int i = 0; i < v.node->child_count; i++) {
		if (v.node->children[i].tag != SE_NODE)
			continue;
		if (c == index) {
			SyntaxView r = {v.node->children[i].as.node, v.src};
			return r;
		}
		c++;
	}
	return none();
}

SyntaxView sv_child_at(SyntaxView v, SyntaxNodeKind kind, int n) {
	int c = 0;
	for (int i = 0; i < v.node->child_count; i++) {
		if (v.node->children[i].tag != SE_NODE || v.node->children[i].as.node->kind != kind)
			continue;
		if (c == n) {
			SyntaxView r = {v.node->children[i].as.node, v.src};
			return r;
		}
		c++;
	}
	return none();
}

SyntaxView sv_child(SyntaxView v, SyntaxNodeKind kind) {
	return sv_child_at(v, kind, 0);
}

int sv_count(SyntaxView v, SyntaxNodeKind kind) {
	int c = 0;
	for (int i = 0; i < v.node->child_count; i++)
		if (v.node->children[i].tag == SE_NODE && v.node->children[i].as.node->kind == kind)
			c++;
	return c;
}

SynText sv_token(SyntaxView v, TokenKind kind) {
	SynText t = {NULL, 0};
	for (int i = 0; i < v.node->child_count; i++) {
		if (v.node->children[i].tag == SE_TOKEN && v.node->children[i].as.token.kind == kind) {
			t.ptr = v.src + v.node->children[i].as.token.offset;
			t.len = v.node->children[i].as.token.length;
			return t;
		}
	}
	return t;
}

int sv_has_token(SyntaxView v, TokenKind kind) {
	return sv_token(v, kind).ptr != NULL;
}

static int is_expr_kind(SyntaxNodeKind k) {
	return k >= SN_LITERAL_EXPR && k <= SN_PAREN_EXPR;
}
static int is_type_kind(SyntaxNodeKind k) {
	return k >= SN_TYPE_REF && k <= SN_TYPE_FUNC;
}

SyntaxView sv_expr_at(SyntaxView v, int idx) {
	int c = 0;
	for (int i = 0; i < v.node->child_count; i++) {
		if (v.node->children[i].tag != SE_NODE)
			continue;
		if (is_expr_kind(v.node->children[i].as.node->kind)) {
			if (c == idx) {
				SyntaxView r = {v.node->children[i].as.node, v.src};
				return r;
			}
			c++;
		}
	}
	return none();
}

SyntaxView sv_type_at(SyntaxView v, int idx) {
	int c = 0;
	for (int i = 0; i < v.node->child_count; i++) {
		if (v.node->children[i].tag != SE_NODE)
			continue;
		if (is_type_kind(v.node->children[i].as.node->kind)) {
			if (c == idx) {
				SyntaxView r = {v.node->children[i].as.node, v.src};
				return r;
			}
			c++;
		}
	}
	return none();
}

int sv_type_count(SyntaxView v) {
	int c = 0;
	for (int i = 0; i < v.node->child_count; i++)
		if (v.node->children[i].tag == SE_NODE && is_type_kind(v.node->children[i].as.node->kind))
			c++;
	return c;
}

/* Walk leaves in source order; keep the first (or, when want_last, the last) token. */
static CvPos token_pos_edge(SyntaxView v, int want_last) {
	CvPos best = {0, 0, 0, 0};
	if (!v.node)
		return best;
	for (int i = 0; i < v.node->child_count; i++) {
		const SyntaxElem *e = &v.node->children[i];
		if (e->tag == SE_NODE) {
			SyntaxView c = {e->as.node, v.src};
			CvPos sub = token_pos_edge(c, want_last);
			if (sub.line) {
				best = sub;
				if (!want_last)
					return best; /* first token found: done */
			}
		} else {
			CvPos p = {e->as.token.line, e->as.token.column, e->as.token.offset, e->as.token.length};
			if (!want_last)
				return p; /* first token in source order */
			best = p;     /* keep advancing to the last */
		}
	}
	return best;
}

CvPos sv_first_token_pos(SyntaxView v) {
	return token_pos_edge(v, 0);
}
CvPos sv_last_token_pos(SyntaxView v) {
	return token_pos_edge(v, 1);
}

CvPos sv_token_pos_at(SyntaxView v, TokenKind kind, int n) {
	CvPos none = {0, 0, 0, 0};
	if (!v.node)
		return none;
	int c = 0;
	for (int i = 0; i < v.node->child_count; i++) {
		const SyntaxElem *e = &v.node->children[i];
		if (e->tag != SE_TOKEN || e->as.token.kind != kind)
			continue;
		if (c == n) {
			CvPos p = {e->as.token.line, e->as.token.column, e->as.token.offset, e->as.token.length};
			return p;
		}
		c++;
	}
	return none;
}

CvPos sv_token_pos(SyntaxView v, TokenKind kind) {
	return sv_token_pos_at(v, kind, 0);
}

int sv_has_error(SyntaxView v) {
	if (!v.node)
		return 0;
	if (v.node->kind == SN_ERROR)
		return 1;
	for (int i = 0; i < v.node->child_count; i++) {
		if (v.node->children[i].tag == SE_NODE) {
			SyntaxView c = {v.node->children[i].as.node, v.src};
			if (sv_has_error(c))
				return 1;
		}
	}
	return 0;
}

/* Strip a doc marker (`marker_len` bytes) and one optional leading space from a
 * comment leaf's source span; return the borrowed remainder slice. */
static SynText doc_strip(const char *text, size_t len, size_t marker_len) {
	SynText t = {NULL, 0};
	if (len < marker_len)
		return t;
	const char *p = text + marker_len;
	size_t n = len - marker_len;
	if (n > 0 && *p == ' ') {
		p++;
		n--;
	}
	t.ptr = p;
	t.len = n;
	return t;
}

/* A comment leaf gathered during the in-order walk. */
typedef struct {
	int line;
	const char *ptr;
	uint32_t len;
} CmtLeaf;

/* Gather, in source order, the most recent comment leaves with offset < limit
 * (i.e. everything lexically before the declaration). The parser may absorb a
 * doc comment that follows a complete declaration as a trailing leaf *inside*
 * that declaration's node rather than as a root-level sibling — so attachment
 * must be decided by source position over a flat list, not by tree adjacency.
 * The buffer keeps the most recent `cap` leaves (a doc run is short, so the tail
 * is all that matters). */
static void gather_comments_before(const SyntaxNode *node, const char *src, uint32_t limit, CmtLeaf *buf, int cap,
                                   int *count) {
	for (int i = 0; i < node->child_count; i++) {
		const SyntaxElem *e = &node->children[i];
		if (e->tag == SE_TOKEN) {
			if (e->as.token.kind == TOK_COMMENT && e->as.token.offset < limit) {
				if (*count < cap) {
					buf[*count].line = e->as.token.line;
					buf[*count].ptr = src + e->as.token.offset;
					buf[*count].len = e->as.token.length;
					(*count)++;
				} else { /* keep most recent: drop the oldest */
					for (int k = 1; k < cap; k++)
						buf[k - 1] = buf[k];
					buf[cap - 1].line = e->as.token.line;
					buf[cap - 1].ptr = src + e->as.token.offset;
					buf[cap - 1].len = e->as.token.length;
				}
			}
		} else if (e->as.node->offset < limit) {
			/* only descend into subtrees that begin before the declaration */
			gather_comments_before(e->as.node, src, limit, buf, cap, count);
		}
	}
}

int sv_decl_doc_lines(SyntaxView root, SyntaxView decl, SynText *out, int *out_lines, int max) {
	if (!root.node || !decl.node || !out || max <= 0)
		return 0;

	CvPos dp = sv_first_token_pos(decl);
	if (dp.line == 0)
		return 0;

#define CV_DOC_GATHER_CAP 1024
	CmtLeaf buf[CV_DOC_GATHER_CAP];
	int total = 0;
	gather_comments_before(root.node, root.src, decl.node->offset, buf, CV_DOC_GATHER_CAP, &total);

	/* Walk the gathered comments from the tail: the contiguous run of `///`
	 * comments ending on the line immediately above the declaration. A blank
	 * line, a plain `//`, or anything else breaks the run. */
	int start = total;
	int expect = dp.line;
	for (int j = total - 1; j >= 0; j--) {
		if (!arche_is_doc_comment(buf[j].ptr, buf[j].len))
			break;
		if (buf[j].line != expect - 1)
			break; /* blank-line gap */
		expect = buf[j].line;
		start = j;
	}

	int count = 0;
	for (int j = start; j < total && count < max; j++) {
		if (out_lines)
			out_lines[count] = buf[j].line;
		out[count++] = doc_strip(buf[j].ptr, buf[j].len, 3);
	}
	return count;
#undef CV_DOC_GATHER_CAP
}

int sv_module_doc_lines(SyntaxView root, SynText *out, int max) {
	if (!root.node || !out || max <= 0)
		return 0;
	int count = 0;
	int expect = 0; /* 0 = no line seen yet; otherwise subsequent lines must be consecutive */
	for (int i = 0; i < root.node->child_count && count < max; i++) {
		const SyntaxElem *e = &root.node->children[i];
		if (e->tag == SE_NODE)
			break; /* first declaration ends the leading module-doc region */
		if (e->tag != SE_TOKEN || e->as.token.kind != TOK_COMMENT)
			break;
		const char *t = root.src + e->as.token.offset;
		if (!arche_is_inner_doc_comment(t, e->as.token.length))
			break;
		if (expect != 0 && e->as.token.line != expect + 1)
			break; /* blank-line gap */
		expect = e->as.token.line;
		out[count++] = doc_strip(t, e->as.token.length, 3);
	}
	return count;
}
