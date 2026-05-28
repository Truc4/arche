#include "cst_view.h"
#include <string.h>

static CstView none(void) {
	CstView v = {NULL, NULL};
	return v;
}

CstView cv_root(const SyntaxNode *root, const char *src) {
	CstView v = {root, src};
	return v;
}

CvText cv_text(CstView v) {
	CvText t = {NULL, 0};
	if (v.node) {
		t.ptr = v.src + v.node->offset;
		t.len = v.node->length;
	}
	return t;
}

int cv_text_eq(CstView v, const char *s) {
	CvText t = cv_text(v);
	size_t n = strlen(s);
	return t.ptr && t.len == n && memcmp(t.ptr, s, n) == 0;
}

int cv_node_count(CstView v) {
	int c = 0;
	for (int i = 0; i < v.node->child_count; i++)
		if (v.node->children[i].tag == SE_NODE)
			c++;
	return c;
}

CstView cv_node_at(CstView v, int index) {
	int c = 0;
	for (int i = 0; i < v.node->child_count; i++) {
		if (v.node->children[i].tag != SE_NODE)
			continue;
		if (c == index) {
			CstView r = {v.node->children[i].as.node, v.src};
			return r;
		}
		c++;
	}
	return none();
}

CstView cv_child_at(CstView v, SyntaxNodeKind kind, int n) {
	int c = 0;
	for (int i = 0; i < v.node->child_count; i++) {
		if (v.node->children[i].tag != SE_NODE || v.node->children[i].as.node->kind != kind)
			continue;
		if (c == n) {
			CstView r = {v.node->children[i].as.node, v.src};
			return r;
		}
		c++;
	}
	return none();
}

CstView cv_child(CstView v, SyntaxNodeKind kind) {
	return cv_child_at(v, kind, 0);
}

int cv_count(CstView v, SyntaxNodeKind kind) {
	int c = 0;
	for (int i = 0; i < v.node->child_count; i++)
		if (v.node->children[i].tag == SE_NODE && v.node->children[i].as.node->kind == kind)
			c++;
	return c;
}

CvText cv_token(CstView v, TokenKind kind) {
	CvText t = {NULL, 0};
	for (int i = 0; i < v.node->child_count; i++) {
		if (v.node->children[i].tag == SE_TOKEN && v.node->children[i].as.token.kind == kind) {
			t.ptr = v.src + v.node->children[i].as.token.offset;
			t.len = v.node->children[i].as.token.length;
			return t;
		}
	}
	return t;
}

int cv_has_token(CstView v, TokenKind kind) {
	return cv_token(v, kind).ptr != NULL;
}

static int is_expr_kind(SyntaxNodeKind k) {
	return k >= SN_LITERAL_EXPR && k <= SN_PAREN_EXPR;
}
static int is_type_kind(SyntaxNodeKind k) {
	return k >= SN_TYPE_REF && k <= SN_TYPE_HANDLE;
}

CstView cv_expr_at(CstView v, int idx) {
	int c = 0;
	for (int i = 0; i < v.node->child_count; i++) {
		if (v.node->children[i].tag != SE_NODE)
			continue;
		if (is_expr_kind(v.node->children[i].as.node->kind)) {
			if (c == idx) {
				CstView r = {v.node->children[i].as.node, v.src};
				return r;
			}
			c++;
		}
	}
	return none();
}

CstView cv_type_at(CstView v, int idx) {
	int c = 0;
	for (int i = 0; i < v.node->child_count; i++) {
		if (v.node->children[i].tag != SE_NODE)
			continue;
		if (is_type_kind(v.node->children[i].as.node->kind)) {
			if (c == idx) {
				CstView r = {v.node->children[i].as.node, v.src};
				return r;
			}
			c++;
		}
	}
	return none();
}

int cv_type_count(CstView v) {
	int c = 0;
	for (int i = 0; i < v.node->child_count; i++)
		if (v.node->children[i].tag == SE_NODE && is_type_kind(v.node->children[i].as.node->kind))
			c++;
	return c;
}

/* Walk leaves in source order; keep the first (or, when want_last, the last) token. */
static CvPos token_pos_edge(CstView v, int want_last) {
	CvPos best = {0, 0, 0, 0};
	if (!v.node)
		return best;
	for (int i = 0; i < v.node->child_count; i++) {
		const SyntaxElem *e = &v.node->children[i];
		if (e->tag == SE_NODE) {
			CstView c = {e->as.node, v.src};
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

CvPos cv_first_token_pos(CstView v) {
	return token_pos_edge(v, 0);
}
CvPos cv_last_token_pos(CstView v) {
	return token_pos_edge(v, 1);
}

int cv_has_error(CstView v) {
	if (!v.node)
		return 0;
	if (v.node->kind == SN_ERROR)
		return 1;
	for (int i = 0; i < v.node->child_count; i++) {
		if (v.node->children[i].tag == SE_NODE) {
			CstView c = {v.node->children[i].as.node, v.src};
			if (cv_has_error(c))
				return 1;
		}
	}
	return 0;
}
