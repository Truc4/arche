#include "syntax_tree.h"
#include <stdlib.h>

/* Monotonic node-id counter for one compiler process. Spanning all parses (the
 * main file AND inlined `use` modules, which are separate parse_source calls) in
 * a single id space keeps ids globally unique, so a side model keyed by id never
 * collides across modules. Never reset; ids only grow. */
static uint32_t g_node_id = 0;

CstBuilder *cst_builder_new(void) {
	CstBuilder *b = malloc(sizeof(CstBuilder));
	b->items = NULL;
	b->count = 0;
	b->cap = 0;
	return b;
}

void cst_builder_free(CstBuilder *b) {
	if (!b)
		return;
	/* Free any nodes still sitting in the flat list (e.g. if finish() wasn't
	 * called). Token leaves own nothing. */
	for (int i = 0; i < b->count; i++) {
		if (b->items[i].tag == SE_NODE)
			syntax_node_free(b->items[i].as.node);
	}
	free(b->items);
	free(b);
}

static void builder_push(CstBuilder *b, SyntaxElem elem) {
	if (b->count >= b->cap) {
		b->cap = b->cap ? b->cap * 2 : 32;
		b->items = realloc(b->items, b->cap * sizeof(SyntaxElem));
	}
	b->items[b->count++] = elem;
}

void cst_builder_token(CstBuilder *b, TokenKind kind, uint32_t offset, uint32_t length, int line, int column) {
	SyntaxElem e;
	e.tag = SE_TOKEN;
	e.as.token.kind = kind;
	e.as.token.offset = offset;
	e.as.token.length = length;
	e.as.token.line = line;
	e.as.token.column = column;
	builder_push(b, e);
}

int cst_builder_checkpoint(CstBuilder *b) {
	return b->count;
}

/* Start offset of an element. */
static uint32_t elem_offset(const SyntaxElem *e) {
	return e->tag == SE_NODE ? e->as.node->offset : e->as.token.offset;
}

/* End offset (exclusive) of an element. */
static uint32_t elem_end(const SyntaxElem *e) {
	return e->tag == SE_NODE ? e->as.node->offset + e->as.node->length : e->as.token.offset + e->as.token.length;
}

/* Build a node from items[from .. to) (a copy of those elements). */
static SyntaxNode *make_node(SyntaxElem *items, int from, int to, SyntaxNodeKind kind) {
	SyntaxNode *n = malloc(sizeof(SyntaxNode));
	n->kind = kind;
	n->child_count = to - from;
	if (n->child_count > 0) {
		n->children = malloc(n->child_count * sizeof(SyntaxElem));
		for (int i = 0; i < n->child_count; i++)
			n->children[i] = items[from + i];
		n->offset = elem_offset(&n->children[0]);
		n->length = elem_end(&n->children[n->child_count - 1]) - n->offset;
	} else {
		n->children = NULL;
		n->offset = 0;
		n->length = 0;
	}
	return n;
}

SyntaxNode *cst_builder_wrap(CstBuilder *b, int checkpoint, SyntaxNodeKind kind) {
	if (checkpoint < 0)
		checkpoint = 0;
	if (checkpoint >= b->count)
		return NULL; /* nothing to wrap (e.g. an errored parse that consumed no tokens) */

	SyntaxNode *node = make_node(b->items, checkpoint, b->count, kind);
	node->id = g_node_id++;

	SyntaxElem e;
	e.tag = SE_NODE;
	e.as.node = node;
	b->items[checkpoint] = e;
	b->count = checkpoint + 1;
	return node;
}

SyntaxNode *cst_builder_finish(CstBuilder *b) {
	SyntaxNode *root = make_node(b->items, 0, b->count, SN_SOURCE_FILE);
	root->id = g_node_id++;
	free(b->items);
	free(b);
	return root;
}

const char *syntax_node_kind_name(SyntaxNodeKind kind) {
	switch (kind) {
	case SN_SOURCE_FILE: return "SOURCE_FILE";
	case SN_WORLD_DECL: return "WORLD_DECL";
	case SN_ARCHETYPE_DECL: return "ARCHETYPE_DECL";
	case SN_PROC_DECL: return "PROC_DECL";
	case SN_SYS_DECL: return "SYS_DECL";
	case SN_FUNC_DECL: return "FUNC_DECL";
	case SN_FUNC_GROUP_DECL: return "FUNC_GROUP_DECL";
	case SN_STATIC_DECL: return "STATIC_DECL";
	case SN_CONST_DECL: return "CONST_DECL";
	case SN_USE_DECL: return "USE_DECL";
	case SN_PARAM_LIST: return "PARAM_LIST";
	case SN_PARAM: return "PARAM";
	case SN_FIELD_DECL: return "FIELD_DECL";
	case SN_BLOCK: return "BLOCK";
	case SN_RETURN_TYPES: return "RETURN_TYPES";
	case SN_ARG_LIST: return "ARG_LIST";
	case SN_BIND_STMT: return "BIND_STMT";
	case SN_ASSIGN_STMT: return "ASSIGN_STMT";
	case SN_FOR_STMT: return "FOR_STMT";
	case SN_IF_STMT: return "IF_STMT";
	case SN_ELSE_CLAUSE: return "ELSE_CLAUSE";
	case SN_BREAK_STMT: return "BREAK_STMT";
	case SN_RUN_STMT: return "RUN_STMT";
	case SN_EXPR_STMT: return "EXPR_STMT";
	case SN_FREE_STMT: return "FREE_STMT";
	case SN_RETURN_STMT: return "RETURN_STMT";
	case SN_MULTI_BIND_STMT: return "MULTI_BIND_STMT";
	case SN_EACH_FIELD_STMT: return "EACH_FIELD_STMT";
	case SN_LITERAL_EXPR: return "LITERAL_EXPR";
	case SN_NAME_EXPR: return "NAME_EXPR";
	case SN_FIELD_EXPR: return "FIELD_EXPR";
	case SN_INDEX_EXPR: return "INDEX_EXPR";
	case SN_BINARY_EXPR: return "BINARY_EXPR";
	case SN_UNARY_EXPR: return "UNARY_EXPR";
	case SN_CALL_EXPR: return "CALL_EXPR";
	case SN_ALLOC_EXPR: return "ALLOC_EXPR";
	case SN_ARRAY_LIT_EXPR: return "ARRAY_LIT_EXPR";
	case SN_STRING_EXPR: return "STRING_EXPR";
	case SN_PAREN_EXPR: return "PAREN_EXPR";
	case SN_TYPE_REF: return "TYPE_REF";
	case SN_TYPE_ARRAY: return "TYPE_ARRAY";
	case SN_TYPE_SHAPED_ARRAY: return "TYPE_SHAPED_ARRAY";
	case SN_TYPE_TUPLE: return "TYPE_TUPLE";
	case SN_TYPE_HANDLE: return "TYPE_HANDLE";
	case SN_TYPE_DEF_NAME: return "TYPE_DEF_NAME";
	case SN_FUNC_DEF_NAME: return "FUNC_DEF_NAME";
	case SN_FIELD_NAME: return "FIELD_NAME";
	case SN_PARAM_NAME: return "PARAM_NAME";
	case SN_CALLEE_NAME: return "CALLEE_NAME";
	case SN_ALLOC_TYPE: return "ALLOC_TYPE";
	case SN_NAME_REF: return "NAME_REF";
	case SN_ERROR: return "ERROR";
	}
	return "?";
}

void syntax_node_free(SyntaxNode *n) {
	if (!n)
		return;
	for (int i = 0; i < n->child_count; i++) {
		if (n->children[i].tag == SE_NODE)
			syntax_node_free(n->children[i].as.node);
	}
	free(n->children);
	free(n);
}
