#include "format_cst.h"
#include <limits.h>
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
		 * hugs its param list too (`proc(...)`, `func(...)`) — the keyword reads as the
		 * callable, like an identifier before a call's `(`. */
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

/* ===== Width-aware list layout (fit-or-break, à la Prettier/rustfmt) ===== */

/* Usable width of one pane of a fullscreen 16:9 nvim split down the middle. A bracketed value
 * list is printed inline if it fits within this column; otherwise it breaks one item per line. */
#define MAX_WIDTH 98

/* A `{ … }` that holds a *value list* (array literal, `#import` names, overload group, archetype
 * fields, enum variants) — as opposed to a statement/decl block (proc body, `#foreign`/`#module`
 * region), which always breaks. Omissions are safe: an unlisted brace keeps the legacy block path. */
static int is_list_brace_parent(SyntaxNodeKind p) {
	return p == SN_ARRAY_LIT_EXPR || p == SN_USE_DECL || p == SN_GROUP_EXPR || p == SN_ARCH_EXPR || p == SN_ENUM_EXPR;
}

/* A `( … )` that holds a genuine comma list — call args, proc/func/sys params + out-params — and so
 * is worth breaking one-per-line. A single-expression paren (`(expr)`), `alloc(...)`, and control-flow
 * headers (`if`/`for`) are excluded: they aren't lists, so breaking them (and adding a trailing
 * comma) would just be noise. (Trailing commas parse fine everywhere now — this is a style choice,
 * not a validity one.) */
static int is_list_paren_parent(SyntaxNodeKind p) {
	switch (p) {
	case SN_CALL_EXPR:
	case SN_PROC_CALL_STMT:
	case SN_PROC_EXPR:
	case SN_FUNC_EXPR:
	case SN_SYS_EXPR:
	case SN_SYS_DECL:
	case SN_TYPE_PROC:
	case SN_TYPE_FUNC:
	case SN_PROC_DECL:
	case SN_FUNC_DECL:
		return 1;
	default:
		return 0;
	}
}

static int is_list_open(const Leaf *l) {
	if (l->kind == TOK_LPAREN)
		return is_list_paren_parent(l->parent);
	if (l->kind == TOK_LBRACE)
		return is_list_brace_parent(l->parent);
	return 0;
}

/* A brace that opens a statement/decl block (handled by the legacy indent path), i.e. any `{`/`}`
 * that is not a value-list brace. */
static int is_block_brace(const Leaf *l) {
	return (l->kind == TOK_LBRACE || l->kind == TOK_RBRACE) && !is_list_brace_parent(l->parent);
}

/* Rendered inline width of the bracket group opening at leaf `i`, through its matching closer.
 * INT_MAX when the group can't be inlined — it contains a line comment or a `;` (a statement, so
 * it must break). Pure over the token stream (whitespace-independent) → the break decision is
 * idempotent. */
static int flat_width(const Leaves *ls, int i) {
	int depth = 0, w = 0;
	TokenKind prev = TOK_EOF;
	for (int k = i; k < ls->count; k++) {
		const Leaf *l = &ls->items[k];
		if (l->kind == TOK_COMMENT || l->kind == TOK_SEMI)
			return INT_MAX;
		if (k > i) {
			TokenKind nx = (k + 1 < ls->count) ? ls->items[k + 1].kind : TOK_EOF;
			if (!no_space_before(l->kind, prev, nx))
				w += 1;
		}
		w += l->len;
		if (l->kind == TOK_LPAREN || l->kind == TOK_LBRACE || l->kind == TOK_LBRACKET) {
			depth++;
		} else if (l->kind == TOK_RPAREN || l->kind == TOK_RBRACE || l->kind == TOK_RBRACKET) {
			if (--depth == 0) {
				if (ls->items[i].kind == TOK_LBRACE)
					w += 2; /* `{ … }` interior spaces */
				return w;
			}
		}
		prev = l->kind;
	}
	return w;
}

/* "Magic trailing comma": the source already ends this bracket group with a comma right before the
 * closer. That is the author's signal to keep the list exploded one-per-line (Prettier/black), so
 * we force a break even when it would fit. (Remove the comma to let it collapse inline.) */
static int has_trailing_comma(const Leaves *ls, int i) {
	int depth = 0;
	for (int k = i; k < ls->count; k++) {
		TokenKind t = ls->items[k].kind;
		if (t == TOK_LPAREN || t == TOK_LBRACE || t == TOK_LBRACKET) {
			depth++;
		} else if (t == TOK_RPAREN || t == TOK_RBRACE || t == TOK_RBRACKET) {
			if (--depth == 0)
				return k > i && ls->items[k - 1].kind == TOK_COMMA;
		}
	}
	return 0;
}

/* One active value-list group while printing. */
typedef struct {
	TokenKind opener; /* '(' or '{' */
	TokenKind closer; /* ')' or '}' */
	SyntaxNodeKind parent;
	int broken; /* doesn't fit on one line → one item per line */
	int depth;  /* bracket depth at which this group's items live */
} Frame;

void format_cst(FILE *out, const SyntaxNode *root, const char *src) {
	if (!root)
		return;
	Leaves ls = {NULL, 0, 0};
	collect(root, src, &ls);

	/* Mark the last token of each decl-level decorator (`@name` or `@name( … )`) so the layout
	 * can break after it — every decorator sits on its own line above the decl it annotates,
	 * rather than inline. `@` is only ever a decorator marker in Arche source. */
	int *deco_end = ls.count ? calloc((size_t)ls.count, sizeof(int)) : NULL;
	for (int i = 0; deco_end && i < ls.count; i++) {
		if (ls.items[i].kind != TOK_AT || i + 1 >= ls.count || ls.items[i + 1].kind != TOK_IDENT)
			continue;
		int last = i + 1; /* `@name` with no argument list ends at the name */
		if (i + 2 < ls.count && ls.items[i + 2].kind == TOK_LPAREN) {
			int d = 0;
			for (int k = i + 2; k < ls.count; k++) {
				if (ls.items[k].kind == TOK_LPAREN)
					d++;
				else if (ls.items[k].kind == TOK_RPAREN && --d == 0) {
					last = k;
					break;
				}
			}
		}
		deco_end[last] = 1;
	}

	int indent = 0;
	int started = 0;
	int col = 0; /* current column (for the width decision) */
	TokenKind prev = TOK_EOF;
	TokenKind prev_noncomment = TOK_EOF; /* last emitted token that wasn't a line comment */
	SyntaxNodeKind prev_parent = SN_SOURCE_FILE;
	int prev_line = 0;
	int force_nl = 0; /* a line comment forces the next token onto a new line */
	Frame fr[128];
	int frn = 0;   /* active value-list groups */
	int depth = 0; /* current bracket nesting depth (all bracket kinds) */

	for (int i = 0; i < ls.count; i++) {
		Leaf *l = &ls.items[i];
		TokenKind next = (i + 1 < ls.count) ? ls.items[i + 1].kind : TOK_EOF;

		/* Closing the innermost active list group? (matched by kind at its own depth). */
		int closes_frame = frn > 0 && l->kind == fr[frn - 1].closer && depth == fr[frn - 1].depth;
		/* Directly inside the innermost list group (its item area)? */
		int in_frame = frn > 0 && depth == fr[frn - 1].depth && !closes_frame;

		/* A trailing comma is a layout artifact: emitted only when a list breaks across lines.
		 * Drop the source's trailing comma when this group renders inline. */
		if (l->kind == TOK_COMMA && in_frame && next == fr[frn - 1].closer && !fr[frn - 1].broken)
			continue;

		int nl = 0, space = 0, eff_indent = indent; /* layout decision for the gap before `l` */
		int add_trailing_comma = 0;

		if (!started) {
			started = 1;
		} else if (closes_frame) {
			Frame *f = &fr[frn - 1];
			if (f->broken) {
				indent--; /* closer dedents to the group's own level */
				eff_indent = indent;
				nl = 1;
				add_trailing_comma = (prev_noncomment !=
				                      TOK_COMMA); /* one trailing comma before the closer (past any trailing comment) */
			} else if (f->closer == TOK_RBRACE) {
				space = 1; /* `{ … }` interior space; `)` hugs */
			}
		} else if (in_frame) {
			Frame *f = &fr[frn - 1];
			int after_open = (prev == f->opener);
			int after_comma = (prev == TOK_COMMA);
			int import_sep = (f->parent == SN_USE_DECL && l->kind == TOK_IDENT && prev == TOK_IDENT);
			if (force_nl) {
				/* Previous token was a line comment — its `//` runs to end of line, so the next
				 * item MUST start a fresh line or it would be swallowed into the comment. */
				nl = 1, eff_indent = indent;
			} else if (l->kind == TOK_COMMENT && l->line == prev_line) {
				/* A trailing comment hugs the item's line. If that item carries no comma (the
				 * last item, with no magic trailing comma in source), synthesize one before the
				 * comment so the exploded list stays one-item-per-line and valid. */
				space = 1;
				add_trailing_comma = f->broken && prev_noncomment != TOK_COMMA && prev_noncomment != f->opener;
			} else if (after_open) {
				if (f->broken)
					nl = 1, eff_indent = indent;
				else if (f->opener == TOK_LBRACE)
					space = 1; /* first item after `{`; `(` hugs */
			} else if (after_comma || import_sep) {
				if (f->broken)
					nl = 1, eff_indent = indent;
				else
					space = 1;
			} else {
				/* item-internal token: normal spacing */
				int after_unary = (prev_parent == SN_UNARY_EXPR && (prev == TOK_MINUS || prev == TOK_BANG));
				int compact = (l->parent == prev_parent && (l->parent == SN_TYPE_REF || l->parent == SN_TYPE_ARRAY ||
				                                            l->parent == SN_TYPE_SHAPED_ARRAY ||
				                                            l->parent == SN_TYPE_HANDLE || l->parent == SN_NAME_EXPR));
				space = (!compact && !after_unary && !no_space_before(l->kind, prev, next));
			}
		} else {
			/* ===== legacy block / statement / decl path ===== */
			int for_header_semi = (prev == TOK_SEMI && prev_parent == SN_FOR_STMT);
			int vis_marker = (l->kind == TOK_HASH_MODULE || l->kind == TOK_HASH_FILE || l->kind == TOK_HASH_FOREIGN);
			int after_vis_marker =
			    (prev == TOK_HASH_MODULE || prev == TOK_HASH_FILE || prev == TOK_HASH_FOREIGN) && l->kind != TOK_LBRACE;
			int block_close = (l->kind == TOK_RBRACE && is_block_brace(l));
			int after_block_open = (prev == TOK_LBRACE);
			int after_deco = (deco_end && i > 0 && deco_end[i - 1]); /* break after each decl-level decorator */
			int want_nl = force_nl || block_close || after_block_open || (prev == TOK_SEMI && !for_header_semi) ||
			              prev == TOK_RBRACE || vis_marker || after_vis_marker || l->decl_start || after_deco;
			if (l->kind == TOK_COMMENT)
				want_nl = (l->line != prev_line);
			if (for_header_semi && l->line > prev_line)
				want_nl = 1; /* author split the header across lines — keep it */
			if (block_close && indent > 0)
				indent--;
			if (want_nl) {
				nl = 1;
				eff_indent = indent;
			} else {
				int after_unary = (prev_parent == SN_UNARY_EXPR && (prev == TOK_MINUS || prev == TOK_BANG));
				int compact = (l->parent == prev_parent && (l->parent == SN_TYPE_REF || l->parent == SN_TYPE_ARRAY ||
				                                            l->parent == SN_TYPE_SHAPED_ARRAY ||
				                                            l->parent == SN_TYPE_HANDLE || l->parent == SN_NAME_EXPR));
				space = (!compact && !after_unary && !no_space_before(l->kind, prev, next));
				if (l->kind == TOK_COMMENT)
					space = 1; /* always a gap before a same-line trailing comment */
			}
		}

		/* Emit the decided gap. */
		if (add_trailing_comma) {
			fputc(',', out); /* attaches to the last item, before the closer's newline */
			col += 1;
		}
		if (nl) {
			fputc('\n', out);
			if (l->line - prev_line >= 2) /* preserve one blank line */
				fputc('\n', out);
			col = 0;
			for (int t = 0; t < eff_indent; t++) {
				fputs("  ", out);
				col += 2;
			}
		} else if (space) {
			fputc(' ', out);
			col += 1;
		}

		fwrite(l->text, 1, (size_t)l->len, out);
		col += l->len;

		/* Bracket bookkeeping: maintain depth, open value-list frames, and block indent. */
		if (closes_frame) {
			frn--;
			depth--;
		} else if (l->kind == TOK_RPAREN || l->kind == TOK_RBRACE || l->kind == TOK_RBRACKET) {
			depth--;
		} else if (l->kind == TOK_LPAREN || l->kind == TOK_LBRACE || l->kind == TOK_LBRACKET) {
			depth++;
			if (is_list_open(l) && frn < 128) {
				int fw = flat_width(&ls, i);
				int broken = (fw == INT_MAX) || (col + fw > MAX_WIDTH) || has_trailing_comma(&ls, i);
				fr[frn].opener = l->kind;
				fr[frn].closer = (l->kind == TOK_LPAREN) ? TOK_RPAREN : TOK_RBRACE;
				fr[frn].parent = l->parent;
				fr[frn].broken = broken;
				fr[frn].depth = depth;
				frn++;
				if (broken)
					indent++;
			} else if (l->kind == TOK_LBRACE && is_block_brace(l)) {
				indent++;
			}
		}

		prev = l->kind;
		if (l->kind != TOK_COMMENT)
			prev_noncomment = l->kind;
		else if (add_trailing_comma)
			prev_noncomment = TOK_COMMA; /* synthesized a comma before this trailing comment */
		prev_parent = l->parent;
		prev_line = l->line;
		force_nl = is_line_comment(l);
	}
	fputc('\n', out);
	free(deco_end);
	free(ls.items);
}
