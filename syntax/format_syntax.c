#include "format_syntax.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* Flatten the syntax tree's token leaves (in source order) so the printer can make
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
			/* Non-brace top-level decls are `;`-terminated now (required unless the body ends in `}`),
			 * so keep the terminator rather than dropping it. */
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
	case TOK_BANG:
		/* The `proc!` panic marker hugs its keyword (`proc!`); a unary `!` falls through to the
		 * prev-based defaults below so `(!x)` / `= !x` are unchanged. */
		if (prev == TOK_PROC)
			return 1;
		break;
	case TOK_LPAREN:
	case TOK_LBRACKET:
		/* call / index: hug an identifier or a closing bracket. A `proc`/`func` keyword
		 * hugs its param list too (`proc(...)`, `func(...)`) — the keyword reads as the
		 * callable, like an identifier before a call's `(`. A `proc!` marker hugs the param
		 * list the same way (`proc!(...)`); this also tightens unary `!(expr)`. */
		return prev == TOK_IDENT || prev == TOK_RPAREN || prev == TOK_RBRACKET || prev == TOK_PROC ||
		       prev == TOK_FUNC || prev == TOK_BANG;
	default:
		break;
	}
	switch (prev) {
	case TOK_DOT:
	case TOK_LPAREN:
	case TOK_LBRACKET:
	case TOK_AT:
		return 1;
	case TOK_BANG:
	case TOK_QUESTION:
		return 1; /* a policy name hugs its sigil: `!clamp`, `?reject` (and unary `!x`) */
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

/* The `{ … }` wrapping a `match` body. It breaks one-arm-per-line like a block brace, but — Go's
 * `switch`-style — does NOT add an indent level: arm patterns align with `match`, and only each arm's
 * own body block indents under it. So it's a block brace for line-breaking but skipped for indent. */
static int is_match_brace(const Leaf *l) {
	return (l->kind == TOK_LBRACE || l->kind == TOK_RBRACE) && l->parent == SN_MATCH_STMT;
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

/* ===== Separator-alignment pass (gofmt-style `:` / `::` / `:=` / `=` columns) =====
 *
 * The token printer above streams into a memory buffer; this second pass then lines up
 * separators across a run of consecutive, equally-indented lines — like gofmt aligns the
 * `=` in a `var ( … )` block or the types in a struct. A blank line, an indent change, or a
 * separator-free line ends the run. The run is re-lexed (not text-scanned) so a `:`/`=`
 * inside a string, a comment, or a `==`/`<=`/`+=` operator is never mistaken for a column;
 * only separators at the line's own bracket depth count (a call-internal `foo(a: 1)` colon
 * is left alone). */

typedef struct {
	char *data;
	size_t len, cap;
} Buf;

static void buf_reserve(Buf *b, size_t extra) {
	if (b->len + extra <= b->cap)
		return;
	size_t need = b->len + extra;
	b->cap = b->cap ? b->cap : 1024;
	while (b->cap < need)
		b->cap *= 2;
	b->data = realloc(b->data, b->cap);
}
static void bputc(Buf *b, char c) {
	buf_reserve(b, 1);
	b->data[b->len++] = c;
}
static void bwrite(Buf *b, const char *s, size_t n) {
	if (!n)
		return;
	buf_reserve(b, n);
	memcpy(b->data + b->len, s, n);
	b->len += n;
}
static void bpad(Buf *b, size_t n) {
	if (!n)
		return;
	buf_reserve(b, n);
	memset(b->data + b->len, ' ', n);
	b->len += n;
}

/* One source line with its two alignment columns. The separators split a declaration into fixed
 * semantic slots, NOT by ordinal position: the binding separator aligns under another line's binding,
 * never under a `name : type` colon — because `:` is the type (first column) and the binding is
 * always the second. So each line has at most a TYPE colon and a BIND separator; either may be absent
 * (`x := 1` has only a bind; a struct field `a: int` has only a type). The bind column is further
 * split by KIND — a colon-led binding (`:=`/`::`) never shares a column with a `=` assignment (see
 * same_shape / bind_is_colon), so a binding's `:` never lands under an assignment's `=`. */
typedef struct {
	const char *text; /* start of line in the formatted buffer (no trailing '\n') */
	size_t len;
	int leading; /* leading-space count — the group key */
	int has_type, type_start, type_end;
	int has_bind, bind_start, bind_end;
} AlignLine;

static int alignable(const AlignLine *L) {
	return L->has_type || L->has_bind;
}

/* The bind separator is colon-led (`:=` / `::` — a binding/declaration) vs `=` (an assignment).
 * These are different kinds and must NOT share a column: aligning a `:=` with a `=` would anchor on
 * the separator start and park the binding's `:` directly under the assignment's `=`. */
static int bind_is_colon(const AlignLine *L) {
	return L->has_bind && L->bind_start < (int)L->len && L->text[L->bind_start] == ':';
}

/* Lines align only with same-shaped neighbours: a `x := 1` (bind-only) does not align with a
 * `y : int = 2` (typed bind), so an absent slot never opens a gap. A `:=`/`::` binding does not align
 * with a `=` assignment (different bind kinds — see bind_is_colon). A shape change ends the run, just
 * like a blank line — the gofmt rule. */
static int same_shape(const AlignLine *a, const AlignLine *b) {
	if (a->has_type != b->has_type || a->has_bind != b->has_bind)
		return 0;
	if (a->has_bind && bind_is_colon(a) != bind_is_colon(b))
		return 0;
	return 1;
}

/* Route a separator unit into the line's TYPE or BIND column. A leading single `:` is a type
 * annotation; everything else (`=`, `:=`, `::`, or a `:` that follows a type colon — a value colon)
 * is the binding. Separators past the bind fold into its trailing cell. */
static void line_add_sep(AlignLine *L, int single_colon, int s, int e) {
	if (!L->has_type && !L->has_bind && single_colon) {
		L->has_type = 1, L->type_start = s, L->type_end = e;
	} else if (!L->has_bind) {
		L->has_bind = 1, L->bind_start = s, L->bind_end = e;
	}
}

/* Slot `si` (0 = TYPE, 1 = BIND) of line L: its separator span [*sep_start,*sep_end) and the byte
 * column where its cell begins (just past the prior present slot). Returns 0 if L lacks the slot. */
static int line_slot(const AlignLine *L, int si, int *sep_start, int *sep_end, int *cell_from) {
	if (si == 0) {
		if (!L->has_type)
			return 0;
		*sep_start = L->type_start, *sep_end = L->type_end, *cell_from = L->leading;
	} else {
		if (!L->has_bind)
			return 0;
		*sep_start = L->bind_start, *sep_end = L->bind_end;
		*cell_from = L->has_type ? L->type_end : L->leading;
	}
	return 1;
}

/* Trim spaces off both ends of [from,to) in L's text. */
static void trim_slice(const AlignLine *L, int from, int to, int *os, int *oe) {
	while (from < to && L->text[from] == ' ')
		from++;
	while (to > from && L->text[to - 1] == ' ')
		to--;
	*os = from, *oe = to;
}

static void align_and_write(FILE *out, Buf *b) {
	if (b->len == 0)
		return;
	int line_count = 0;
	for (size_t i = 0; i < b->len; i++)
		if (b->data[i] == '\n')
			line_count++;
	if (line_count == 0) {
		fwrite(b->data, 1, b->len, out);
		return;
	}

	AlignLine *lines = calloc((size_t)line_count, sizeof(AlignLine));
	int li = 0;
	size_t start = 0;
	for (size_t i = 0; i < b->len; i++) {
		if (b->data[i] != '\n')
			continue;
		AlignLine *L = &lines[li++];
		L->text = b->data + start;
		L->len = i - start;
		int lead = 0;
		while ((size_t)lead < L->len && L->text[lead] == ' ')
			lead++;
		L->leading = lead;
		start = i + 1;
	}

	/* Re-lex the formatted text to locate genuine separators. */
	buf_reserve(b, 1);
	b->data[b->len] = '\0'; /* NUL terminator past len, not counted */
	TokenBuffer tb = lexer_tokenize(b->data);
	int *start_depth = malloc((size_t)line_count * sizeof(int));
	for (int i = 0; i < line_count; i++)
		start_depth[i] = -1;
	int depth = 0;
	for (size_t ti = 0; ti < tb.count; ti++) {
		Token *t = &tb.tokens[ti];
		if (t->kind == TOK_EOF)
			break;
		int ln = t->line - 1;
		if (ln < 0 || ln >= line_count)
			continue;
		if (start_depth[ln] < 0)
			start_depth[ln] = depth; /* this line's structural level */
		int is_sep = 0, single_colon = 0, s = t->column - 1, e = s + (int)t->length;
		if (t->kind == TOK_COLON) {
			is_sep = 1;
			single_colon = 1;
			/* fold a contiguous `::` or `:=` into a single separator unit (not a single colon) */
			if (ti + 1 < tb.count) {
				Token *n = &tb.tokens[ti + 1];
				if ((n->kind == TOK_COLON || n->kind == TOK_EQ) && n->line == t->line &&
				    n->column == t->column + (int)t->length) {
					e = (n->column - 1) + (int)n->length;
					single_colon = 0;
					ti++;
				}
			}
		} else if (t->kind == TOK_EQ) {
			is_sep = 1; /* a bare `=`; `==`/`<=`/`+=` are distinct token kinds */
		}
		if (is_sep && depth == start_depth[ln])
			line_add_sep(&lines[ln], single_colon, s, e);
		switch (t->kind) {
		case TOK_LPAREN:
		case TOK_LBRACE:
		case TOK_LBRACKET:
			depth++;
			break;
		case TOK_RPAREN:
		case TOK_RBRACE:
		case TOK_RBRACKET:
			if (depth > 0)
				depth--;
			break;
		default:
			break;
		}
	}
	token_buffer_free(&tb);

	Buf *rebuilt = calloc((size_t)line_count, sizeof(Buf));
	int *has_rebuild = calloc((size_t)line_count, sizeof(int));

	int g = 0;
	while (g < line_count) {
		if (!alignable(&lines[g])) {
			g++;
			continue;
		}
		int h = g + 1;
		while (h < line_count && alignable(&lines[h]) && lines[h].leading == lines[g].leading &&
		       same_shape(&lines[h], &lines[g]))
			h++;
		if (h - g >= 2) {
			for (int k = g; k < h; k++) {
				bpad(&rebuilt[k], (size_t)lines[k].leading);
				has_rebuild[k] = 1;
			}
			/* Two fixed columns, left to right: TYPE then BIND. A line lacking a column is
			 * skipped for it but still pads its cell to reach the next column it does have. */
			for (int si = 0; si < 2; si++) {
				size_t target = 0;
				for (int k = g; k < h; k++) {
					int ss, se, cf;
					if (!line_slot(&lines[k], si, &ss, &se, &cf))
						continue;
					int cs, ce;
					trim_slice(&lines[k], cf, ss, &cs, &ce);
					size_t cur = rebuilt[k].len + (size_t)(ce - cs) + 1; /* +1 = min one space */
					if (cur > target)
						target = cur;
				}
				for (int k = g; k < h; k++) {
					int ss, se, cf;
					if (!line_slot(&lines[k], si, &ss, &se, &cf))
						continue;
					int cs, ce;
					trim_slice(&lines[k], cf, ss, &cs, &ce);
					bwrite(&rebuilt[k], lines[k].text + cs, (size_t)(ce - cs));
					bpad(&rebuilt[k], target - rebuilt[k].len);
					bwrite(&rebuilt[k], lines[k].text + ss, (size_t)(se - ss));
					bputc(&rebuilt[k], ' ');
				}
			}
			for (int k = g; k < h; k++) {
				AlignLine *L = &lines[k];
				int ts = L->has_bind ? L->bind_end : L->type_end;
				while ((size_t)ts < L->len && L->text[ts] == ' ')
					ts++;
				bwrite(&rebuilt[k], L->text + ts, L->len - (size_t)ts);
				while (rebuilt[k].len > 0 && rebuilt[k].data[rebuilt[k].len - 1] == ' ')
					rebuilt[k].len--; /* drop trailing pad (e.g. empty tail) */
			}
		}
		g = h;
	}

	for (int k = 0; k < line_count; k++) {
		if (has_rebuild[k])
			fwrite(rebuilt[k].data, 1, rebuilt[k].len, out);
		else
			fwrite(lines[k].text, 1, lines[k].len, out);
		fputc('\n', out);
		free(rebuilt[k].data);
	}
	free(rebuilt);
	free(has_rebuild);
	free(start_depth);
	free(lines);
}

void format_syntax(FILE *out, const SyntaxNode *root, const char *src) {
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

	Buf ob = {NULL, 0, 0}; /* printer streams here; align_and_write() flushes it to `out` */
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

		/* A `;` right after a top-level `}` is the OPTIONAL decl terminator (the `}` self-terminates the
		 * decl — `M :: { … }`, `f :: proc() { … }`). Drop it rather than strand it on its own line. */
		if (l->kind == TOK_SEMI && prev == TOK_RBRACE && depth == 0)
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
			if (l->kind == TOK_SEMI && prev == TOK_RBRACE)
				want_nl = 0; /* a REQUIRED `;` after a `}` (in-body `b := { … };`) hugs it, no new line */
			if (block_close && !is_match_brace(l) && indent > 0)
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
			bputc(&ob, ','); /* attaches to the last item, before the closer's newline */
			col += 1;
		}
		if (nl) {
			bputc(&ob, '\n');
			if (l->line - prev_line >= 2) /* preserve one blank line */
				bputc(&ob, '\n');
			col = 0;
			for (int t = 0; t < eff_indent; t++) {
				bpad(&ob, 2);
				col += 2;
			}
		} else if (space) {
			bputc(&ob, ' ');
			col += 1;
		}

		bwrite(&ob, l->text, (size_t)l->len);
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
			} else if (l->kind == TOK_LBRACE && is_block_brace(l) && !is_match_brace(l)) {
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
	bputc(&ob, '\n');
	align_and_write(out, &ob);
	free(ob.data);
	free(deco_end);
	free(ls.items);
}
