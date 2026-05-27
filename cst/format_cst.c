#include "format_cst.h"
#include <stdlib.h>

/* Flatten the CST's token leaves (in source order) so the printer can make
 * spacing decisions from adjacent token kinds. */
typedef struct {
	TokenKind kind;
	const char *text;
	int len;
	int line;
	SyntaxNodeKind parent; /* node directly containing this token — for structure-aware spacing */
} Leaf;

typedef struct {
	Leaf *items;
	int count, cap;
} Leaves;

static void push_leaf(Leaves *ls, TokenKind k, const char *text, int len, int line, SyntaxNodeKind parent) {
	if (ls->count >= ls->cap) {
		ls->cap = ls->cap ? ls->cap * 2 : 256;
		ls->items = realloc(ls->items, (size_t)ls->cap * sizeof(Leaf));
	}
	ls->items[ls->count].kind = k;
	ls->items[ls->count].text = text;
	ls->items[ls->count].len = len;
	ls->items[ls->count].line = line;
	ls->items[ls->count].parent = parent;
	ls->count++;
}

static void collect(const SyntaxNode *n, const char *src, Leaves *ls) {
	for (int i = 0; i < n->child_count; i++) {
		const SyntaxElem *e = &n->children[i];
		if (e->tag == SE_NODE) {
			collect(e->as.node, src, ls);
		} else {
			push_leaf(ls, e->as.token.kind, src + e->as.token.offset, (int)e->as.token.length, e->as.token.line,
			          n->kind);
		}
	}
}

static int is_line_comment(const Leaf *l) {
	return l->kind == TOK_COMMENT && l->len >= 2 && l->text[0] == '/' && l->text[1] == '/';
}

/* No space before this token on the same line. `next` is the following token kind
 * so compound colon operators (`:=`, `::`) — which arche lexes as two tokens — can
 * be spaced as units: `result := e`, `name :: T`, but `a: int` for a type colon. */
static int no_space_before(TokenKind t, TokenKind prev, TokenKind next) {
	switch (t) {
	case TOK_SEMI:
	case TOK_COMMA:
	case TOK_RPAREN:
	case TOK_RBRACKET:
	case TOK_DOT:
		return 1;
	case TOK_COLON:
		if (prev == TOK_COLON)
			return 1; /* second `:` of `::` glues to the first */
		if (next == TOK_EQ || next == TOK_COLON)
			return 0; /* first colon of a `:=` / `::` operator: space before it */
		return 1;     /* type-annotation colon: `a: int` */
	case TOK_EQ:
		return prev == TOK_COLON; /* `=` of `:=` glues to the `:` */
	case TOK_LPAREN:
	case TOK_LBRACKET:
		/* call / index: hug an identifier or a closing bracket */
		return prev == TOK_IDENT || prev == TOK_RPAREN || prev == TOK_RBRACKET;
	default:
		break;
	}
	switch (prev) {
	case TOK_DOT:
	case TOK_LPAREN:
	case TOK_LBRACKET:
	case TOK_AT:
		return 1;
	default:
		return 0;
	}
}

void format_cst(FILE *out, const SyntaxNode *root, const char *src) {
	if (!root)
		return;
	Leaves ls = {NULL, 0, 0};
	collect(root, src, &ls);

	int indent = 0;
	int started = 0;
	TokenKind prev = TOK_EOF;
	SyntaxNodeKind prev_parent = SN_SOURCE_FILE;
	int prev_line = 0;
	int force_nl = 0; /* a line comment forces the next token onto a new line */

	for (int i = 0; i < ls.count; i++) {
		Leaf *l = &ls.items[i];
		TokenKind next = (i + 1 < ls.count) ? ls.items[i + 1].kind : TOK_EOF;

		if (!started) {
			started = 1;
		} else {
			/* one archetype field per line: a comma directly in the archetype body */
			int arch_field_break = (prev == TOK_COMMA && prev_parent == SN_ARCHETYPE_DECL);
			/* `;` ends a statement → newline. The two `;` in a `for (init; cond; incr)` header are
			 * direct children of the for-statement node; they separate clauses, not statements, so
			 * we don't force a break. Instead the author's layout is preserved (like blank lines
			 * below): an inline header stays inline, a hand-split one keeps its breaks. */
			int for_header_semi = (prev == TOK_SEMI && prev_parent == SN_FOR_STMT);
			int want_nl = force_nl || arch_field_break || l->kind == TOK_RBRACE || prev == TOK_LBRACE ||
			              (prev == TOK_SEMI && !for_header_semi) || prev == TOK_RBRACE;
			if (for_header_semi && l->line > prev_line)
				want_nl = 1; /* the author split the header across lines — keep it */
			/* a type/generic/table reference is compact: handle<X>, float[5], table<P> */
			int compact = (l->parent == prev_parent && (l->parent == SN_TYPE_REF || l->parent == SN_TYPE_ARRAY ||
			                                            l->parent == SN_TYPE_SHAPED_ARRAY ||
			                                            l->parent == SN_TYPE_HANDLE || l->parent == SN_NAME_EXPR));
			if (l->kind == TOK_RBRACE && indent > 0)
				indent--;
			if (want_nl) {
				fputc('\n', out);
				if (l->line - prev_line >= 2) /* preserve one blank line */
					fputc('\n', out);
				for (int t = 0; t < indent; t++)
					fputs("  ", out);
			} else if (!compact && !no_space_before(l->kind, prev, next)) {
				fputc(' ', out);
			}
		}

		fwrite(l->text, 1, (size_t)l->len, out);

		if (l->kind == TOK_LBRACE)
			indent++;
		prev = l->kind;
		prev_parent = l->parent;
		prev_line = l->line;
		force_nl = is_line_comment(l);
	}
	fputc('\n', out);
	free(ls.items);
}
