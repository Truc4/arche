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
