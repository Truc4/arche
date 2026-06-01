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
	int decl_start;        /* first token of a top-level (SOURCE_FILE-child) declaration */
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
	ls->items[ls->count].decl_start = 0;
	ls->count++;
}

static void collect(const SyntaxNode *n, const char *src, Leaves *ls) {
	for (int i = 0; i < n->child_count; i++) {
		const SyntaxElem *e = &n->children[i];
		if (e->tag == SE_NODE) {
			/* A direct child node of SOURCE_FILE — or of a `#foreign`/`#module` region block — is a
			 * declaration. Tag its first leaf so the printer breaks before it: needed for decls with
			 * no `;`/`}` terminator (e.g. `file :: opaque` or a bodiless foreign proc), which would
			 * otherwise glue onto the previous declaration. */
			int top = (n->kind == SN_SOURCE_FILE || n->kind == SN_REGION);
			int before = ls->count;
			collect(e->as.node, src, ls);
			if (top && ls->count > before)
				ls->items[before].decl_start = 1;
		} else {
			/* Declarations take no trailing ';' (only statements do). Drop it from a
			 * static/pool decl so the canonical output is ';'-free. */
			if (e->as.token.kind == TOK_SEMI && n->kind == SN_STATIC_DECL)
				continue;
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
		/* call / index: hug an identifier or a closing bracket. A `proc`/`func` keyword
		 * hugs its param list too (`proc(...)`, `func(...)`, `extern proc(...)`) — the
		 * keyword reads as the callable, like an identifier before a call's `(`. */
		return prev == TOK_IDENT || prev == TOK_RPAREN || prev == TOK_RBRACKET || prev == TOK_PROC || prev == TOK_FUNC;
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
			/* The author split a CALL'S argument list across lines — keep the break (like the
			 * for-header / blank-line preservation below), with a continuation indent. Scoped to
			 * the call node only (args/commas are its direct children — there is no SN_ARG_LIST
			 * wrap), so params/archetype/enum/array/index layouts are untouched; and never the
			 * closing `)` (a trailing comma before it must not indent the closer). */
			int list_continuation =
			    (prev == TOK_COMMA && prev_parent == SN_CALL_EXPR && l->line > prev_line && l->kind != TOK_RPAREN);
			/* `;` ends a statement → newline. The two `;` in a `for (init; cond; incr)` header are
			 * direct children of the for-statement node; they separate clauses, not statements, so
			 * we don't force a break. Instead the author's layout is preserved (like blank lines
			 * below): an inline header stays inline, a hand-split one keeps its breaks. */
			int for_header_semi = (prev == TOK_SEMI && prev_parent == SN_FOR_STMT);
			/* `#module` / `#file` / `#foreign` region markers are standalone section banners: break
			 * onto their own line, and break again after them so the following decl starts fresh —
			 * except a `{` (block form) stays on the marker's line: `#foreign {`. */
			int vis_marker = (l->kind == TOK_HASH_MODULE || l->kind == TOK_HASH_FILE || l->kind == TOK_HASH_FOREIGN);
			int after_vis_marker =
			    (prev == TOK_HASH_MODULE || prev == TOK_HASH_FILE || prev == TOK_HASH_FOREIGN) && l->kind != TOK_LBRACE;
			/* `#import { a b c }` formats inline — its braces don't open an indented block. Suppress
			 * the break-after-`{` and break-before-`}` for tokens inside an SN_USE_DECL so the whole
			 * import stays on one line (you write `#import` once and list the modules). */
			int want_nl = force_nl || arch_field_break || list_continuation ||
			              (l->kind == TOK_RBRACE && l->parent != SN_USE_DECL) ||
			              (prev == TOK_LBRACE && prev_parent != SN_USE_DECL) ||
			              (prev == TOK_SEMI && !for_header_semi) || prev == TOK_RBRACE ||
			              vis_marker || after_vis_marker || l->decl_start;
			/* A comment on a NEW source line gets its own line; a trailing comment on the SAME line as
			 * the code it follows stays inline (don't force it down). This override wins over the
			 * statement-break rules above (e.g. a `// note` after `stmt;` stays put). */
			if (l->kind == TOK_COMMENT)
				want_nl = (l->line != prev_line);
			if (for_header_semi && l->line > prev_line)
				want_nl = 1; /* the author split the header across lines — keep it */
			/* a type/generic/table reference is compact: handle<X>, float[5], table<P> */
			int compact = (l->parent == prev_parent && (l->parent == SN_TYPE_REF || l->parent == SN_TYPE_ARRAY ||
			                                            l->parent == SN_TYPE_SHAPED_ARRAY ||
			                                            l->parent == SN_TYPE_HANDLE || l->parent == SN_NAME_EXPR));
			if (l->kind == TOK_RBRACE && l->parent != SN_USE_DECL && indent > 0)
				indent--;
			if (want_nl) {
				fputc('\n', out);
				if (l->line - prev_line >= 2) /* preserve one blank line */
					fputc('\n', out);
				/* continued list items get one extra level so they sit under, not beside, the call */
				int eff_indent = indent + (list_continuation ? 1 : 0);
				for (int t = 0; t < eff_indent; t++)
					fputs("  ", out);
			} else {
				/* No space after a symbolic unary operator (`-1`, `!flag`): the operator and its
				 * operand are direct children of SN_UNARY_EXPR. `move`/`copy` are keyword unaries and
				 * keep their space, so only `-` / `!` are special-cased here. */
				int after_unary_op = (prev_parent == SN_UNARY_EXPR && (prev == TOK_MINUS || prev == TOK_BANG));
				/* Inline `#import { ... }`: keep a space just inside the braces (`{ io net }`), which
				 * the generic call/index brace-hugging rule would otherwise strip. */
				int use_brace_inner = (prev == TOK_LBRACE && prev_parent == SN_USE_DECL) ||
				                      (l->kind == TOK_RBRACE && l->parent == SN_USE_DECL);
				if (use_brace_inner)
					fputc(' ', out);
				else if (!compact && !after_unary_op && !no_space_before(l->kind, prev, next))
					fputc(' ', out);
			}
		}

		fwrite(l->text, 1, (size_t)l->len, out);

		if (l->kind == TOK_LBRACE && l->parent != SN_USE_DECL)
			indent++;
		prev = l->kind;
		prev_parent = l->parent;
		prev_line = l->line;
		force_nl = is_line_comment(l);
	}
	fputc('\n', out);
	free(ls.items);
}
