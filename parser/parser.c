#include "parser.h"
#include "../syntax/syntax_tree.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ========== PARSER STRUCT DEFINITION ========== */

struct Parser {
	Lexer *lexer;
	Token current;
	Token previous;
	int had_error;
	int panic_mode;
	ParseError *errors;
	size_t error_count;
	size_t error_cap;
	/* Trivia (comments + blank-line runs) collected between syntactic tokens.
	 * Each TOK_COMMENT seen during advance() lands here; blank-line gaps detected
	 * between the previous-syntactic-token's line and the next syntactic token's
	 * line also land here as TRIVIA_BLANK_LINES entries. The pending list is
	 * drained into the next syntax tree node's leading_trivia at the start of its parse,
	 * and any same-line entries are stolen back as the just-finished node's
	 * trailing_trivia after parse. */
	Trivia *pending_trivia;
	int pending_count;
	int pending_cap;
	int recursion_depth;
	/* Rust-style restriction: while set, a bare `IDENT {` is NOT an entity literal — it's a control-flow
	 * header (the `match` scrutinee, whose `{` opens the arm block). Lifted inside `(`/`[` groups. */
	int no_brace_lit;
	/* Inside a `#foreign` region (banner = rest of file, or a `#foreign { ... }` block). While set,
	 * a bodiless proc parses as a foreign value-form (SN_PROC_EXPR) rather than a proc type. */
	int in_foreign;
	/* Lossless syntax tree builder. Built alongside the AST purely by appending; never
	 * affects parse control flow, so syntax tree bugs can't change compiler output. */
	CstBuilder *builder;
};

/* ========== UTILITY FUNCTIONS ========== */

static void append_pending_trivia(Parser *parser, Trivia tr) {
	if (parser->pending_count >= parser->pending_cap) {
		parser->pending_cap = parser->pending_cap ? parser->pending_cap * 2 : 8;
		parser->pending_trivia = realloc(parser->pending_trivia, parser->pending_cap * sizeof(Trivia));
	}
	parser->pending_trivia[parser->pending_count++] = tr;
}

/* Line of the most-recent reference point: last pending-trivia entry, else
 * the previous syntactic token. Returns 0 only when neither exists (file
 * start, before any tokens or trivia have been seen). */
static int trivia_anchor_line(Parser *parser, int prev_line) {
	if (parser->pending_count > 0) {
		Trivia *last = &parser->pending_trivia[parser->pending_count - 1];
		int line = last->line;
		if (last->kind == TRIVIA_BLANK_LINES)
			line += last->blank_count;
		return line;
	}
	return prev_line;
}

static void maybe_append_blank_trivia(Parser *parser, int anchor_line, int next_line) {
	if (anchor_line == 0)
		return; /* file start — no leading blanks tracked */
	if (next_line - anchor_line <= 1)
		return;
	Trivia tr;
	tr.kind = TRIVIA_BLANK_LINES;
	tr.start = NULL;
	tr.length = 0;
	tr.line = anchor_line + 1;
	tr.column = 1;
	tr.blank_count = next_line - anchor_line - 1;
	append_pending_trivia(parser, tr);
}

static void advance(Parser *parser) {
	/* syntax tree: emit the token being consumed (the current lookahead) as a leaf, in
	 * source order. Skips the priming/end sentinel (EOF) and error tokens (whose
	 * `start` points at a message literal, not into source). */
	if (parser->builder && parser->current.kind != TOK_EOF && parser->current.kind != TOK_ERROR &&
	    parser->current.start != NULL) {
		uint32_t off = (uint32_t)(parser->current.start - parser->lexer->src);
		syntax_builder_token(parser->builder, parser->current.kind, off, (uint32_t)parser->current.length,
		                     parser->current.line, parser->current.column);
	}

	parser->previous = parser->current;
	int prev_line = parser->previous.line;
	parser->current = lexer_next_token(parser->lexer);

	int comment_loop_count = 0;
	const int MAX_COMMENT_LOOP = 1000;
	while (parser->current.kind == TOK_COMMENT) {
		if (++comment_loop_count > MAX_COMMENT_LOOP) {
			parser->had_error = 1;
			parser->current.kind = TOK_EOF;
			break;
		}
		/* syntax tree: comments are real leaves too, keeping the tree lossless. */
		if (parser->builder && parser->current.start != NULL) {
			uint32_t coff = (uint32_t)(parser->current.start - parser->lexer->src);
			syntax_builder_token(parser->builder, TOK_COMMENT, coff, (uint32_t)parser->current.length,
			                     parser->current.line, parser->current.column);
		}
		maybe_append_blank_trivia(parser, trivia_anchor_line(parser, prev_line), parser->current.line);
		Trivia ctr;
		ctr.kind = TRIVIA_LINE_COMMENT;
		ctr.start = parser->current.start;
		ctr.length = parser->current.length;
		ctr.line = parser->current.line;
		ctr.column = parser->current.column;
		ctr.blank_count = 0;
		append_pending_trivia(parser, ctr);
		parser->current = lexer_next_token(parser->lexer);
	}

	if (parser->current.kind != TOK_EOF) {
		maybe_append_blank_trivia(parser, trivia_anchor_line(parser, prev_line), parser->current.line);
	}

	if (parser->current.kind == TOK_ERROR) {
		parser->had_error = 1;
	}
}

/* Discard accumulated pending trivia. Comments are already emitted as syntax tree leaves
 * in advance(); the pending list only tracked comments/blank-lines for the old
 * AST-node trivia (consumed by the legacy formatter). Draining keeps the list
 * bounded during a parse without affecting the syntax tree. */
static void drain_pending_trivia(Parser *parser) {
	parser->pending_count = 0;
}

/* Drain pending trivia into the given (leading_trivia, leading_count) slots.
 * The caller takes ownership of the allocated array. */
static void take_pending_as_leading(Parser *parser, Trivia **out_trivia, int *out_count) {
	if (parser->pending_count == 0) {
		*out_trivia = NULL;
		*out_count = 0;
		return;
	}
	*out_trivia = malloc(parser->pending_count * sizeof(Trivia));
	memcpy(*out_trivia, parser->pending_trivia, parser->pending_count * sizeof(Trivia));
	*out_count = parser->pending_count;
	parser->pending_count = 0;
}

static int check(Parser *parser, TokenKind kind) {
	return parser->current.kind == kind;
}

static int match(Parser *parser, TokenKind kind) {
	if (!check(parser, kind))
		return 0;
	advance(parser);
	return 1;
}

static void error(Parser *parser, const char *msg) {
	if (parser->panic_mode)
		return;
	parser->panic_mode = 1;
	parser->had_error = 1;

	/* Append error to errors array */
	if (parser->error_count >= parser->error_cap) {
		parser->error_cap = (parser->error_cap == 0) ? 16 : parser->error_cap * 2;
		parser->errors = realloc(parser->errors, parser->error_cap * sizeof(ParseError));
	}

	/* Format and allocate error message */
	char buf[512];
	snprintf(buf, sizeof(buf), "%s", msg);
	parser->errors[parser->error_count].message = malloc(strlen(buf) + 1);
	strcpy(parser->errors[parser->error_count].message, buf);
	parser->errors[parser->error_count].line = parser->current.line;
	parser->errors[parser->error_count].column = parser->current.column;
	parser->error_count++;
}

static void synchronize(Parser *parser) {
	parser->panic_mode = 0;
	int sync_loop_count = 0;
	const int MAX_SYNC_LOOP = 1000;

	/* Always consume the offending token first, so every synchronize() call makes
	 * forward progress. Otherwise a caller loop `if (!stmt) { synchronize(); continue; }`
	 * can re-parse the same token forever (the early returns below would advance 0 tokens),
	 * spinning and growing memory without bound — the `if (x;)` RAM-explosion bug. */
	if (parser->current.kind != TOK_EOF)
		advance(parser);

	while (parser->current.kind != TOK_EOF) {
		if (++sync_loop_count > MAX_SYNC_LOOP) {
			parser->had_error = 1;
			parser->current.kind = TOK_EOF;
			return;
		}
		if (parser->previous.kind == TOK_SEMI)
			return;

		switch (parser->current.kind) {
		case TOK_ARCHETYPE:
		case TOK_PROC:
		case TOK_MAP:
		case TOK_SYSTEM:
		case TOK_FUNC:
		case TOK_LET:
		case TOK_FOR:
			return;
		default:
			break;
		}

		advance(parser);
	}
}

/* ===== syntax tree builder helpers (no-ops when no builder is attached) ===== */
static int syntax_cp(Parser *parser) {
	return parser->builder ? syntax_builder_checkpoint(parser->builder) : 0;
}
static SyntaxNode *syntax_wrap(Parser *parser, int checkpoint, SyntaxNodeKind kind) {
	if (parser->builder)
		return syntax_builder_wrap(parser->builder, checkpoint, kind);
	return NULL;
}

/* True if everything emitted since `checkpoint` already collapsed to a single
 * node (e.g. a parenthesised expression wrapped itself), so the caller should
 * not wrap it again. */
static int syntax_single_node(Parser *parser, int checkpoint) {
	CstBuilder *b = parser->builder;
	return b && b->count == checkpoint + 1 && b->items[checkpoint].tag == SE_NODE;
}

/* Kind of the single syntax tree node emitted since `checkpoint`, or SN_ERROR if the region
 * isn't exactly one node. Lets statement parsing tell a bare-name target (SN_NAME_EXPR)
 * from a field/index/call lvalue without an AST. */
static SyntaxNodeKind syntax_single_node_kind(Parser *parser, int checkpoint) {
	CstBuilder *b = parser->builder;
	if (b && b->count == checkpoint + 1 && b->items[checkpoint].tag == SE_NODE)
		return b->items[checkpoint].as.node->kind;
	return SN_ERROR;
}

/* ========== FORWARD DECLARATIONS ========== */

/* The parser builds ONLY the lossless syntax tree: every parse_* function consumes tokens,
 * emits syntax tree node wraps, and returns success (1) / failure (0). It builds no abstract
 * AST — that is reconstructed from the syntax tree by cst_to_program. A few parse decisions
 * still depend on the *form* of a just-parsed sub-construct (a bare-name LHS, a `type`
 * meta-type, a shaped-array element type); those are threaded back through small
 * out-params instead of inspecting a built node. */

/* Form of a parsed type, for the few callers that branch on it. `syntax_kind` is the
 * syntax tree wrap kind; `is_type_meta` marks the bare `type` meta-keyword (which shares
 * SN_TYPE_REF with ordinary names but drives const/bind RHS parsing). */
typedef struct {
	SyntaxNodeKind syntax_kind;
	int is_type_meta;
} TypeForm;

static int parse_decl(Parser *parser, SyntaxNodeKind *out_kind);
static int parse_statement(Parser *parser);
static int parse_expression(Parser *parser);
static int parse_type(Parser *parser);
static int parse_type_form(Parser *parser, TypeForm *out);
static int parse_type_inner(Parser *parser, TypeForm *out);
/* Signature parsers (defined with the shared sub-parsers below); forward-declared here so a
 * proc/func type form can appear in type-annotation position (`h: proc()(w: writer)`). */
static int parse_proc_sig(Parser *parser, int is_extern);
static int parse_func_sig(Parser *parser, int is_extern);

/* ========== TYPE PARSING ========== */

/* Wrapper: every type position becomes a type node in the syntax tree, tagged by the
 * specific form so identifiers within are classified as types and consumers can
 * tell arrays/tuples/handles apart. All call sites go through here. */
static int parse_type(Parser *parser) {
	TypeForm form;
	return parse_type_form(parser, &form);
}

static int parse_type_form(Parser *parser, TypeForm *out) {
	int cp = syntax_cp(parser);
	int ok = parse_type_inner(parser, out);
	syntax_wrap(parser, cp, out->syntax_kind);
	return ok;
}

/* `out->syntax_kind` receives the syntax tree type-node kind for the form parsed, from parse
 * context. Pre-set to SN_TYPE_REF; overridden for array/shaped/handle forms. */
static int parse_type_inner(Parser *parser, TypeForm *out) {
	out->syntax_kind = SN_TYPE_REF;
	out->is_type_meta = 0;

	/* A proc/func type in annotation position: `h: proc(in)(out)`, `f: func(in) -> T`. The
	 * `type` meta is implied (a callable signature denotes a type). Bodiless — no `{...}` here. */
	if (check(parser, TOK_PROC)) {
		advance(parser);
		out->syntax_kind = SN_TYPE_PROC;
		out->is_type_meta = 1;
		return parse_proc_sig(parser, 0);
	}
	if (check(parser, TOK_FUNC)) {
		advance(parser);
		out->syntax_kind = SN_TYPE_FUNC;
		out->is_type_meta = 1;
		return parse_func_sig(parser, 0);
	}
	/* `system` as a TYPE — a system reference (the payload of the Schedule `run` leaf). The `system`
	 * keyword in type position denotes the category of systems; the actual identity is a compile-time
	 * reference captured at construction. Wrapped as SN_TYPE_REF carrying the `system` token. */
	if (check(parser, TOK_SYSTEM)) {
		advance(parser);
		out->syntax_kind = SN_TYPE_REF;
		out->is_type_meta = 1;
		return 1;
	}

	/* Prefix array/slice type: `[]T` (slice, runtime length) or `[N]T` / `[a][b]T` (fixed-size).
	 * The element type FOLLOWS the brackets. Indexing (`a[i]`) is a separate production, unaffected.
	 * The node holds the same IDENT + NUMBER tokens as before — downstream reads them by kind, not
	 * position — so only the surface order changed (postfix `T[N]` → prefix `[N]T`). */
	if (check(parser, TOK_LBRACKET)) {
		int any_number = 0;
		while (check(parser, TOK_LBRACKET)) {
			advance(parser); /* '[' */
			if (check(parser, TOK_RBRACKET)) {
				advance(parser); /* ']' — slice dimension */
			} else if (check(parser, TOK_NUMBER)) {
				advance(parser); /* size */
				any_number = 1;
				if (!match(parser, TOK_RBRACKET)) {
					error(parser, "Expected ']' after array size");
					return 1;
				}
			} else {
				error(parser, "Expected ']' or integer size after '['");
				while (!check(parser, TOK_RBRACKET) && !check(parser, TOK_EOF))
					advance(parser);
				if (check(parser, TOK_RBRACKET))
					advance(parser);
			}
		}
		/* element type name (a primitive or name, optionally qualified `mod.Name`) */
		if (!check(parser, TOK_IDENT)) {
			error(parser, "Expected element type name after array brackets (e.g. `[]int`)");
			return 0;
		}
		advance(parser);              /* element name */
		if (check(parser, TOK_DOT)) { /* qualified `mod.Name` element */
			advance(parser);
			if (!check(parser, TOK_IDENT)) {
				error(parser, "Expected type name after '.' in qualified element type");
				return 0;
			}
			advance(parser);
		}
		out->syntax_kind = any_number ? SN_TYPE_SHAPED_ARRAY : SN_TYPE_ARRAY;
		return 1;
	}

	if (!check(parser, TOK_IDENT)) {
		error(parser, "Expected type name");
		return 0;
	}

	int is_handle = (parser->current.length == 6 && strncmp(parser->current.start, "handle", 6) == 0);
	int is_eff = (parser->current.length == 3 && strncmp(parser->current.start, "Eff", 3) == 0);
	int is_type_kw = (parser->current.length == 4 && strncmp(parser->current.start, "type", 4) == 0);
	int is_archetype = (parser->current.length == 9 && strncmp(parser->current.start, "archetype", 9) == 0);
	int is_opaque = (parser->current.length == 6 && strncmp(parser->current.start, "opaque", 6) == 0);
	advance(parser);

	/* Bare-category names `archetype` / `opaque` are leaf types (no array/handle suffix):
	 * `archetype` is a parameter category; `opaque` is a pointer-width C-owned cell. */
	if (is_archetype || is_opaque)
		return 1;

	/* Qualified type `mod.Name` (e.g. `io.file`, `net.socket`): the leading IDENT is a module, the
	 * member is the type. "A binding is a binding" — this is the same `mod.name` access used for
	 * values; it resolves to the module's type symbol in lowering/semantic. A leaf (no array suffix). */
	if (!is_handle && !is_type_kw && check(parser, TOK_DOT)) {
		advance(parser); /* '.' */
		if (!check(parser, TOK_IDENT)) {
			error(parser, "Expected type name after '.' in qualified type (e.g. io.file)");
			return 0;
		}
		advance(parser); /* the member type name */
		return 1;
	}

	/* `type`: the meta-type (type-of-types). Appears as the declared type in the alias
	 * longhand `foo : type : float` and (later) generic params. Compile-time only. */
	if (is_type_kw) {
		out->is_type_meta = 1;
		return 1;
	}

	/* handle<ArchetypeName> — a generation-checked reference to a row in a table. */
	if (is_handle && check(parser, TOK_LT)) {
		advance(parser); /* consume < */
		if (!check(parser, TOK_IDENT)) {
			error(parser, "Expected archetype name after 'handle<'");
			return 0;
		}
		advance(parser);
		if (!match(parser, TOK_GT)) {
			error(parser, "Expected '>' after archetype name in handle type");
			return 0;
		}
		out->syntax_kind = SN_TYPE_HANDLE;
		return 1;
	}

	/* Eff(T…) — a not-yet-run effect value (the flat effect model §3). The parenthesized list is the
	 * out-slot types it yields when run. A built-in type constructor (no generics); the paren form is
	 * deliberately distinct from handle<…>'s angle brackets. Each out-slot is a child type node. */
	if (is_eff && check(parser, TOK_LPAREN)) {
		advance(parser); /* ( */
		if (!check(parser, TOK_RPAREN)) {
			do {
				if (!parse_type(parser))
					return 0;
			} while (match(parser, TOK_COMMA));
		}
		if (!match(parser, TOK_RPAREN)) {
			error(parser, "Expected ')' to close the Eff(...) out-slot type list");
			return 0;
		}
		out->syntax_kind = SN_TYPE_EFF;
		return 1;
	}

	/* `archetype` / `opaque` bare-category names parse like an ordinary type name (the syntax tree
	 * records the keyword token; semantic interprets it). The array/slice suffix is NO LONGER
	 * accepted here — array types are PREFIX (`[]T` / `[N]T`), parsed at the top of this function. */

	return 1;
}

/* Parse a tuple name-group `(a, b, …) :: T` after the leading name was consumed. The
 * suffixes are part of the name (minting `name_a`, `name_b`, …), `T` is the shared type. */
static int parse_tuple_name_group(Parser *parser) {
	advance(parser); /* consume '(' */
	int name_count = 0;
	while (!check(parser, TOK_RPAREN) && !check(parser, TOK_EOF)) {
		if (!check(parser, TOK_IDENT)) {
			error(parser, "Expected a name in tuple group `(a, b, …)`");
			break;
		}
		advance(parser);
		name_count++;
		if (!match(parser, TOK_COMMA))
			break;
	}
	if (name_count == 0) {
		error(parser, "Empty tuple group `()`: a tuple needs at least one name, e.g. `pos (x, y) :: T`");
		return 0;
	}
	if (!match(parser, TOK_RPAREN)) {
		error(parser, "Expected ')' to close tuple name group");
		return 0;
	}
	if (!match(parser, TOK_COLON) || !match(parser, TOK_COLON)) {
		error(parser, "Expected `::` and a shared type after tuple name group `(a, b) :: T`");
		return 0;
	}
	if (!parse_type(parser))
		return 0;
	return 1;
}

/* ========== ARCHETYPE PARSING ========== */

/* Parse one archetype field/component, wrapping its syntax tree. Returns 1 on success,
 * 0 on a malformed component (so the body loop stops). */
static int parse_arch_field(Parser *parser) {
	if (!check(parser, TOK_IDENT)) {
		error(parser, "Expected field name");
		return 0;
	}
	int field_decl_name_cp = syntax_cp(parser);
	advance(parser);
	syntax_wrap(parser, field_decl_name_cp, SN_FIELD_NAME);

	/* Tuple group component: `pos (x, y) :: float` — the suffixes mint flat `pos_x`/`pos_y`
	 * of the shared type. */
	if (check(parser, TOK_LPAREN)) {
		if (!parse_tuple_name_group(parser))
			return 0;
		match(parser, TOK_COMMA); /* optional trailing comma */
		return 1;
	}

	/* A component is a type. A bare `a` references the type `a` (`arche Foo { a, b }` is a
	 * set of nominal component types). `name :: type` defines one inline (mints the nominal
	 * type `name` and includes it). The old single-colon accessor `name: type` is rejected —
	 * `foo : bar` names nothing. */
	if (!check(parser, TOK_COLON)) {
		match(parser, TOK_COMMA); /* optional trailing comma */
		return 1;
	}
	advance(parser); /* consume first ':' */
	if (check(parser, TOK_COLON)) {
		advance(parser); /* second ':' — `name :: type`, inferred meta-type */
	} else {
		/* The only other accepted form is the explicit meta-type longhand `name : type : type`.
		 * A bare single colon (`name: type`) names nothing and is rejected. */
		int is_meta =
		    (check(parser, TOK_IDENT) && parser->current.length == 4 && strncmp(parser->current.start, "type", 4) == 0);
		if (!is_meta) {
			error(parser, "archetype components are types: write `name :: type` to define a component, "
			              "or a bare type name to reference one (`name: type` is not valid)");
			return 0;
		}
		advance(parser); /* consume the meta-type `type` */
		if (!match(parser, TOK_COLON)) {
			error(parser, "expected `:` after `type`: write `name : type : T`");
			return 0;
		}
	}

	/* Inline scalar component definition `name :: type` / `name : type : type` (tuple groups use
	 * `name (a, b) :: T`). The component type follows; a value RHS (e.g. `hp :: 10`) is rejected by
	 * parse_type, which requires a type name. */
	if (!parse_type(parser))
		return 0;

	match(parser, TOK_COMMA); /* optional trailing comma */
	return 1;
}

/* ========== SHARED SIGNATURE / BODY SUB-PARSERS ========== */
/* These factor the param-list, out-list, return-type, and block-body parsing out of the
 * keyword-led decl parsers so the same code backs both `proc name(...)` (legacy) and the
 * unified `name :: proc(...)` RHS forms. Extraction is behaviour-preserving — every error
 * message is kept verbatim. */

/* Parse the comma-separated in-parameters between an already-consumed '(' and the closing
 * ')' (which the caller matches). `...` (variadic) is accepted only when is_extern. Shared
 * verbatim by proc and func signatures. */
static int parse_param_list_body(Parser *parser, int is_extern) {
	if (check(parser, TOK_RPAREN))
		return 1;
	do {
		/* `...` variadic marker — extern only, last param only. */
		if (check(parser, TOK_DOTDOTDOT)) {
			if (!is_extern)
				error(parser, "`...` variadic marker is only valid in extern signatures");
			advance(parser);
			break;
		}

		int param_cp = syntax_cp(parser);

		match(parser, TOK_OWN); /* optional `own` qualifier (syntax tree records the token) */

		if (!check(parser, TOK_IDENT)) {
			error(parser, "Expected parameter name");
			return 0;
		}
		int param_name_cp = syntax_cp(parser);
		advance(parser);
		syntax_wrap(parser, param_name_cp, SN_PARAM_NAME);

		if (!match(parser, TOK_COLON)) {
			error(parser, "Expected ':' after parameter name");
			return 0;
		}

		if (!parse_type(parser))
			return 0;

		syntax_wrap(parser, param_cp, SN_PARAM);
	} while (match(parser, TOK_COMMA) && !check(parser, TOK_RPAREN));
	return 1;
}

/* A proc declares its results as an OPTIONAL second `(...)` out-parameter list — it has no
 * return type (`->` is a func-only marker). Parses the `->`-rejection guard plus the optional
 * list. The caller has already consumed the in-param list's ')'. */
static int parse_proc_out_list(Parser *parser) {
	if (check(parser, TOK_ARROW)) {
		error(parser, "a proc has no return type (`->` is a func-only marker) — declare results as out-parameters in a "
		              "`(...)` list");
		return 0;
	}
	if (match(parser, TOK_LPAREN)) {
		if (!check(parser, TOK_RPAREN)) {
			do {
				int out_param_cp = syntax_cp(parser);

				if (check(parser, TOK_OWN)) {
					error(parser, "an out-parameter is owned by definition — drop `own`");
					return 0;
				}

				if (!check(parser, TOK_IDENT)) {
					error(parser, "Expected out-parameter name");
					return 0;
				}
				int out_param_name_cp = syntax_cp(parser);
				advance(parser);
				syntax_wrap(parser, out_param_name_cp, SN_PARAM_NAME);

				if (!match(parser, TOK_COLON)) {
					error(parser, "Expected ':' after out-parameter name");
					return 0;
				}

				if (!parse_type(parser))
					return 0;

				syntax_wrap(parser, out_param_cp, SN_OUT_PARAM);
			} while (match(parser, TOK_COMMA) && !check(parser, TOK_RPAREN));
		}
		if (!match(parser, TOK_RPAREN)) {
			error(parser, "Expected ')' after out-parameters");
			return 0;
		}
	}
	return 1;
}

/* A func's results: either a single `-> T` return, or an out-parameter list `(out, …)` for multiple
 * results / an in-place fill (the form `proc` used to carry; `proc` is now foreign-only, so a func is
 * the one non-foreign callable and owns both shapes). A bare extern may have neither (⇒ void). */
static int parse_func_return(Parser *parser, int is_extern) {
	if (match(parser, TOK_ARROW)) {
		if (check(parser, TOK_LPAREN)) {
			error(parser, "a `->` return is a single type; for multiple results drop the `->` and write an "
			              "out-parameter list `(out, …)` instead");
			return 0;
		}
		if (!parse_type(parser))
			return 0;
	} else if (check(parser, TOK_LPAREN)) {
		return parse_proc_out_list(parser); /* a func may produce results via an out-param list */
	} else if (!is_extern) {
		error(parser, "Expected '->' or an out-parameter list '(...)'");
		return 0;
	}
	return 1;
}

/* query columns: bare component names (no `: T`) inside `{ … }`. Assumes `query` is already consumed;
 * this parses the brace list. It is the SAME production for a standalone `Name :: query {…}` decl and
 * for an inline `map(query {…})`, so the column grammar is identical wherever a query appears. */
static int parse_query_columns(Parser *parser) {
	if (!match(parser, TOK_LBRACE)) {
		error(parser, "Expected '{' after 'query'");
		return 0;
	}
	if (!check(parser, TOK_RBRACE)) {
		do {
			if (check(parser, TOK_RBRACE)) /* trailing comma */
				break;
			int param_cp = syntax_cp(parser);
			if (!check(parser, TOK_IDENT)) {
				error(parser, "Expected a column name");
				return 0;
			}

			int param_name_cp = syntax_cp(parser);
			advance(parser);
			syntax_wrap(parser, param_name_cp, SN_PARAM_NAME);

			syntax_wrap(parser, param_cp, SN_PARAM);
		} while (match(parser, TOK_COMMA) && !check(parser, TOK_RBRACE));
	}
	if (!match(parser, TOK_RBRACE)) {
		error(parser, "Expected '}' to close query");
		return 0;
	}
	return 1;
}

/* A `{ statement* }` body. Identical across proc / map / func. */
static int parse_block_body(Parser *parser) {
	if (!match(parser, TOK_LBRACE)) {
		error(parser, "Expected '{'");
		return 0;
	}

	while (!check(parser, TOK_RBRACE) && !check(parser, TOK_EOF)) {
		if (!parse_statement(parser))
			synchronize(parser);
	}

	if (!match(parser, TOK_RBRACE)) {
		error(parser, "Expected '}'");
	}

	return 1;
}

/* A proc signature `(in)(out?)` after the `proc` keyword has been consumed. Shared by the
 * value-literal form (parse_proc_form) and the type form (parse_type_inner). */
static int parse_proc_sig(Parser *parser, int is_extern) {
	if (!match(parser, TOK_LPAREN)) {
		error(parser, "Expected '(' after 'proc'");
		return 0;
	}
	if (!parse_param_list_body(parser, is_extern))
		return 0;
	if (!match(parser, TOK_RPAREN)) {
		error(parser, "Expected ')' after parameters");
		return 0;
	}
	return parse_proc_out_list(parser);
}

/* A func signature `(in) -> T` after the `func` keyword has been consumed. */
static int parse_func_sig(Parser *parser, int is_extern) {
	if (!match(parser, TOK_LPAREN)) {
		error(parser, "Expected '(' after 'func'");
		return 0;
	}
	if (!parse_param_list_body(parser, is_extern))
		return 0;
	if (!match(parser, TOK_RPAREN)) {
		error(parser, "Expected ')' after parameters");
		return 0;
	}
	return parse_func_return(parser, is_extern);
}

/* A proc as an RHS value/type form (the `proc` keyword already consumed). A body `{...}` (or
 * `extern`) makes it a value literal (SN_PROC_EXPR); a bare bodiless signature is a proc type
 * (SN_TYPE_PROC). The binding LHS supplies the name. */
static int parse_proc_form(Parser *parser, int is_extern, SyntaxNodeKind *out_kind) {
	if (!parse_proc_sig(parser, is_extern))
		return 0;
	if (is_extern && check(parser, TOK_LBRACE)) {
		error(parser, "a proc inside a `#foreign` region is an FFI declaration and has no body — "
		              "remove the `{ ... }` (or move it out of the `#foreign` region)");
		return 0;
	}
	if (!is_extern && check(parser, TOK_LBRACE)) {
		*out_kind = SN_PROC_EXPR;
		return parse_block_body(parser);
	}
	*out_kind = is_extern ? SN_PROC_EXPR : SN_TYPE_PROC;
	return 1;
}

/* A func as an RHS value/type form (the `func` keyword already consumed). Body ⇒ SN_FUNC_EXPR
 * value; bodiless ⇒ SN_TYPE_FUNC type. */
static int parse_func_form(Parser *parser, SyntaxNodeKind *out_kind) {
	if (!parse_func_sig(parser, 0))
		return 0;
	if (check(parser, TOK_LBRACE)) {
		*out_kind = SN_FUNC_EXPR;
		return parse_block_body(parser);
	}
	*out_kind = SN_TYPE_FUNC;
	return 1;
}

/* A policy form `(in) -> T { body }` (the `policy` keyword already consumed). Structurally a
 * pure func; tagged as a failure-policy decl, its category supplied by an `@policy(...)`
 * decorator on the binding. Always bodied — there is no bodiless policy type. */
static int parse_policy_form(Parser *parser, SyntaxNodeKind *out_kind) {
	/* A policy is a MACRO: it mutates its operands in place, so the return type is OPTIONAL (a
	 * mutate-form policy has none). `(len, i)` / `(a, b)` then a body. */
	if (!match(parser, TOK_LPAREN)) {
		error(parser, "Expected '(' after 'policy'");
		return 0;
	}
	if (!parse_param_list_body(parser, 0))
		return 0;
	if (!match(parser, TOK_RPAREN)) {
		error(parser, "Expected ')' after policy parameters");
		return 0;
	}
	if (check(parser, TOK_ARROW)) { /* optional explicit return type */
		advance(parser);
		if (!parse_type(parser))
			return 0;
	}
	if (!check(parser, TOK_LBRACE)) {
		error(parser, "a `policy` must have a body `{ ... }`");
		return 0;
	}
	*out_kind = SN_POLICY_EXPR;
	return parse_block_body(parser);
}

/* An Odin-style overload group `{ a, b, c }` (the `proc`/`func` keyword already consumed). */
static int parse_group_form(Parser *parser, SyntaxNodeKind *out_kind) {
	*out_kind = SN_GROUP_EXPR;
	advance(parser); /* consume '{' (caller verified) */
	if (check(parser, TOK_RBRACE)) {
		error(parser, "group must have at least one member");
		return 0;
	}
	while (1) {
		if (!check(parser, TOK_IDENT)) {
			error(parser, "Expected member name in group");
			return 0;
		}
		advance(parser);
		if (!match(parser, TOK_COMMA))
			break;
		if (check(parser, TOK_RBRACE)) /* optional trailing comma */
			break;
	}
	if (!match(parser, TOK_RBRACE)) {
		error(parser, "Expected '}' or ',' in group member list");
		return 0;
	}
	return 1;
}

/* ========== DECLARATION PARSING ========== */

/* ========== WORLD PARSING ========== */

/* True if the current token is the identifier matching `kw` (length `len`). */
static int cur_ident_is(Parser *parser, const char *kw, size_t len) {
	return check(parser, TOK_IDENT) && parser->current.length == len && strncmp(parser->current.start, kw, len) == 0;
}

/* A top-level (and tuple-group) declaration is `;`-terminated, EXCEPT a brace body self-terminates
 * (Jai-style): if the last consumed token was `}`, the `;` is optional. Requiring it on non-brace
 * decls removes the grammar ambiguity where a bare-name RHS would absorb a following prefix-pool `[`
 * (`val :: float` then `[4]Node` — without a terminator, `float[4]` reads as indexing). */
static void require_decl_terminator(Parser *parser) {
	if (match(parser, TOK_SEMI))
		return;
	if (parser->previous.kind == TOK_RBRACE)
		return; /* a `{ … }` body self-terminates — `;` optional after `}` */
	error(parser, "expected `;` to end the declaration");
}

static int parse_static_decl(Parser *parser, SyntaxNodeKind *out_kind) {
	/* Prefix pool allocation: `[C]Name(N){V} ?handler` — capacity LEADS (`[C]`), then the
	 * (possibly qualified `lib.Particle`) archetype name, then optional initial live-count `(N)`,
	 * field-init block `{V}`, and overflow `?handler`. Top-level position implies static storage;
	 * the name references the archetype shape whose singleton pool to allocate. */
	if (check(parser, TOK_LBRACKET)) {
		*out_kind = SN_STATIC_DECL;
		advance(parser);               /* '[' */
		if (!parse_expression(parser)) /* capacity expression */
			return 0;
		if (!match(parser, TOK_RBRACKET)) {
			error(parser, "Expected ']' after pool capacity in `[C]Name`");
			return 0;
		}
		if (check(parser, TOK_ARCHETYPE)) {
			/* Anonymous storage `[C]arche { cols }` — the shape has no name; a query is its accessor. */
			int arch_cp = syntax_cp(parser);
			advance(parser); /* consume 'arche' */
			if (!match(parser, TOK_LBRACE)) {
				error(parser, "Expected '{' after 'archetype'");
				return 0;
			}
			while (!check(parser, TOK_RBRACE) && !check(parser, TOK_EOF)) {
				drain_pending_trivia(parser);
				if (!parse_arch_field(parser))
					break;
			}
			if (!match(parser, TOK_RBRACE)) {
				error(parser, "Expected '}' after archetype fields");
				return 0;
			}
			syntax_wrap(parser, arch_cp, SN_ARCH_EXPR);
		} else if (!check(parser, TOK_IDENT)) {
			error(parser, "Expected an archetype name or `arche {…}` after pool capacity (e.g. `[8]Particle`)");
			return 0;
		} else {
			advance(parser);                 /* archetype name head */
			while (check(parser, TOK_DOT)) { /* qualified `lib.Particle` */
				advance(parser);
				if (!check(parser, TOK_IDENT)) {
					error(parser, "expected an identifier after `.` in a qualified pool name");
					return 0;
				}
				advance(parser); /* the next name segment */
			}
		}
		if (match(parser, TOK_LPAREN)) {
			if (!parse_expression(parser))
				return 0;
			if (!match(parser, TOK_RPAREN))
				error(parser, "Expected ')' after pool live-count in `[C]Name(N)`");
		}
		if (match(parser, TOK_LBRACE)) {
			if (!check(parser, TOK_RBRACE)) {
				do {
					if (!check(parser, TOK_IDENT)) {
						error(parser, "Expected field name in pool init block");
						break;
					}
					advance(parser);
					if (!match(parser, TOK_COLON)) {
						error(parser, "Expected ':' after field name in pool init block");
						break;
					}
					if (!parse_expression(parser)) {
						error(parser, "Expected expression after ':' in pool init block");
						break;
					}
				} while (match(parser, TOK_COMMA) && !check(parser, TOK_RBRACE));
			}
			if (!match(parser, TOK_RBRACE))
				error(parser, "Expected '}' after pool init block");
		}
		/* `Name[C] ?handler` — the pool's overflow handler policy (storage-level, not the archetype
		 * schema). Wrapped in SN_POLICY_REF; every `insert(Name,…)` defaults to it. `?` (handler), not
		 * `!` (panic) — a full pool is an expected condition. */
		if (check(parser, TOK_QUESTION)) {
			int pol_cp = syntax_cp(parser);
			advance(parser); /* '?' */
			if (!check(parser, TOK_IDENT)) {
				error(parser, "Expected a handler name after '?' on a pool decl (e.g. `[8]Foo ?evict_oldest`)");
				return 0;
			}
			advance(parser); /* the handler ident */
			syntax_wrap(parser, pol_cp, SN_POLICY_REF);
		}
		require_decl_terminator(parser); /* required; `;` optional after a `}` body */
		return 1;
	}

	if (!check(parser, TOK_IDENT))
		return 0;
	advance(parser); /* the binding name — a bare leading IDENT for every non-pool top-level form */

	/* Tuple group: `pos (x, y) :: float` — the parenthesized suffixes mint flat names
	 * (`pos_x`, `pos_y`); the shared type follows `::`. */
	if (check(parser, TOK_LPAREN)) {
		*out_kind = SN_CONST_DECL;
		if (!parse_tuple_name_group(parser))
			return 0;
		require_decl_terminator(parser);
		return 1;
	}

	/* `name : …` binding. The second separator carries mutability (the unified grammar):
	 *   `name :: value` / `name : T : value`  — const (value or nominal type alias).
	 *   `name : T`      (no second separator)  — mutable static buffer (a sized array).
	 *   `name := value` / `name : T = value`   — mutable initialized global (not lowered yet).
	 * Const stays SN_CONST_DECL; storage forms become SN_STATIC_DECL. */
	if (check(parser, TOK_COLON)) {
		advance(parser); /* first ':' */

		/* `name := value` — mutable initialized global, inferred type. */
		if (check(parser, TOK_EQ)) {
			*out_kind = SN_STATIC_DECL;
			advance(parser); /* '=' */
			if (!parse_expression(parser))
				return 0;
			require_decl_terminator(parser); /* required; `;` optional after a `}` body */
			return 1;
		}

		int decl_type_is_meta = 0;
		TypeForm form = {0};
		int have_type = 0;
		if (!check(parser, TOK_COLON)) {
			/* explicit declared type before the second separator */
			if (!parse_type_form(parser, &form))
				return 0;
			have_type = 1;
			decl_type_is_meta = form.is_type_meta;
		}

		/* `name : T = value` — mutable initialized typed global. */
		if (check(parser, TOK_EQ)) {
			*out_kind = SN_STATIC_DECL;
			advance(parser); /* '=' */
			if (!parse_expression(parser))
				return 0;
			require_decl_terminator(parser);
			return 1;
		}

		/* `name : T` with no second separator → mutable static storage, zero-initialized (the
		 * implicit `= 0` of the unified form). T may be a sized array (a buffer) or a scalar. */
		if (have_type && !check(parser, TOK_COLON)) {
			*out_kind = SN_STATIC_DECL;
			require_decl_terminator(parser); /* required; `;` optional after a `}` body */
			return 1;
		}

		/* Const: `name :: value` or `name : T : value`. */
		*out_kind = SN_CONST_DECL;
		if (!match(parser, TOK_COLON)) {
			error(parser, "expected `:` and a value: write `name :: value` or `name : T : value`");
			return 0;
		}
		/* When the declared meta-type is `type`, the RHS is a type form (a nominal alias); parse
		 * it as a type. Otherwise the RHS is a value expression (the elided `::` form lands here
		 * too — semantic classifies it by what it denotes). */
		if (decl_type_is_meta) {
			if (!parse_type(parser))
				return 0;
		} else {
			/* `name :: alias T` — consume the transparent-alias marker; the backing then parses as
			 * the value name (which denotes a type), exactly like the bare `name :: T` alias form. */
			if (parser->current.kind == TOK_ALIAS)
				advance(parser);
			if (!parse_expression(parser))
				return 0;
		}
		require_decl_terminator(parser);
		return 1;
	}

	return 0;
}

/* `out_kind` receives the syntax tree node kind for the declaration form, from parse
 * context. Pre-set to SN_ERROR; each leaf parser / branch sets the real kind. */
/* A region marker — `#module` / `#file` / `#foreign` — in one of two forms:
 *   banner  `#foreign`             applies to every following decl, to end of file
 *   block   `#foreign { decls }`   applies only to the decls inside the braces
 * The marker token is current on entry. `#foreign` toggles parser->in_foreign so bodiless
 * procs in scope parse as foreign value-forms (not proc types); `#module`/`#file` visibility
 * narrowing is applied later, positionally, by the decl-collection loops. Emits SN_REGION;
 * for the block form its children are the braced decls (the collection loops recurse in). */
static int parse_region(Parser *parser, SyntaxNodeKind *out_kind) {
	int is_foreign = check(parser, TOK_HASH_FOREIGN);
	advance(parser); /* consume the marker token */
	*out_kind = SN_REGION;

	if (check(parser, TOK_LBRACE)) {
		/* Block form: the attribute is scoped to the braced decl list. */
		advance(parser); /* consume '{' */
		int saved = parser->in_foreign;
		if (is_foreign)
			parser->in_foreign = 1;
		while (!check(parser, TOK_RBRACE) && !check(parser, TOK_EOF)) {
			Trivia *leading = NULL;
			int leading_count = 0;
			take_pending_as_leading(parser, &leading, &leading_count);
			free(leading);
			(void)leading_count;
			int child_cp = syntax_cp(parser);
			SyntaxNodeKind child_kind = SN_ERROR;
			if (!parse_decl(parser, &child_kind)) {
				synchronize(parser);
				syntax_wrap(parser, child_cp, SN_ERROR);
				continue;
			}
			syntax_wrap(parser, child_cp, child_kind);
		}
		parser->in_foreign = saved;
		if (!match(parser, TOK_RBRACE)) {
			error(parser, "Expected '}' to close the region block");
			return 0;
		}
		return 1;
	}

	/* Banner form: sticky to end of file. */
	if (is_foreign)
		parser->in_foreign = 1;
	return 1;
}

/* `#link { "X11" "Xext" }` — a region of quoted system-library names. Unlike `#foreign`, its
 * payload is STRING items (not decls), so it parses like the `#import { ... }` block but accepts
 * only TOK_STRING. It carries no declarations and no visibility/foreign effect — it is pure link
 * metadata, collected program-wide at link time (see semantic_collect_link_libs) and appended to the
 * cc command as `-l<name>`. Emitted as SN_REGION; consumers distinguish it via TOK_HASH_LINK. Block
 * form only (a bare banner `#link X11` is rejected — libs must be quoted). */
static int parse_link_region(Parser *parser, SyntaxNodeKind *out_kind) {
	advance(parser); /* consume '#link' */
	*out_kind = SN_REGION;
	if (!match(parser, TOK_LBRACE)) {
		error(parser, "expected `{ \"lib\" ... }` after `#link`");
		return 0;
	}
	if (check(parser, TOK_RBRACE)) {
		error(parser, "empty `#link { }` — list one or more quoted system-library names");
		return 0;
	}
	while (!check(parser, TOK_RBRACE) && !check(parser, TOK_EOF)) {
		if (!check(parser, TOK_STRING)) {
			error(parser, "expected a quoted system-library name in `#link { ... }` (e.g. \"X11\")");
			return 0;
		}
		advance(parser);
	}
	if (!match(parser, TOK_RBRACE)) {
		error(parser, "Expected '}' to close `#link { ... }`");
		return 0;
	}
	return 1;
}

/* `#run <expr>` or `#run { e1, e2, … }` — the program's Schedule value(s) the runtime executes. The
 * block form is region-style (trailing comma allowed) and runs its entries in order (an implicit `seq`);
 * the bare form is a single expression (e.g. `forever(seq({ run(a), run(b) }))`). Either way the children
 * are EXPRESSION nodes folded at lowering. */
static int parse_run_region(Parser *parser, SyntaxNodeKind *out_kind) {
	advance(parser); /* consume '#run' */
	*out_kind = SN_RUN_DECL;
	if (check(parser, TOK_LBRACE)) {
		advance(parser); /* consume '{' */
		if (check(parser, TOK_RBRACE)) {
			error(parser, "empty `#run { }` — list one or more Schedule expressions");
			return 0;
		}
		while (!check(parser, TOK_RBRACE) && !check(parser, TOK_EOF)) {
			if (!parse_expression(parser))
				return 0;
			if (!match(parser, TOK_COMMA))
				break;
		}
		if (!match(parser, TOK_RBRACE)) {
			error(parser, "expected '}' to close `#run { ... }`");
			return 0;
		}
		return 1;
	}
	if (!parse_expression(parser)) {
		error(parser, "expected a Schedule expression after `#run`");
		return 0;
	}
	return 1;
}

static int parse_decl(Parser *parser, SyntaxNodeKind *out_kind) {
	*out_kind = SN_ERROR;

	/* Declaration-site decorators. Two recognized forms:
	 *
	 *   @allow_pure_proc           — legacy, proc-only: suppresses proc-could-be-func.
	 *   @allow(<diagnostic-slug>)  — general lint suppression, any decl kind.
	 *
	 * Multiple decorators may be stacked. Tokens are stored in the syntax tree at decl
	 * level; `cst_to_program` reads them — `sv_has_token(d, TOK_AT)` for the
	 * legacy flag and a slug scan for `@allow(...)` entries. */
	while (parser->current.kind == TOK_AT) {
		advance(parser); /* consume '@' */
		/* `@policy(<category>)` — role tag on a `policy` decl, naming the op category it serves:
		 * `bounds` (index/slice), `pool` (insert), or `divide`. The category — not the signature —
		 * disambiguates the (int,int)->int collision between bounds and divide. `policy` lexes as
		 * the TOK_POLICY keyword (not TOK_IDENT), so it is matched before the ident guard.
		 * Recorded on the decl as tokens; lowering reads the category ident. */
		if (check(parser, TOK_POLICY)) {
			advance(parser); /* consume 'policy' */
			if (!check(parser, TOK_LPAREN)) {
				error(parser, "Expected '(' after @policy — name the category, e.g. @policy(bounds)");
				return 0;
			}
			advance(parser); /* consume '(' */
			if (!check(parser, TOK_IDENT)) {
				error(parser, "Expected a category (bounds, pool, or divide) inside @policy(...)");
				return 0;
			}
			advance(parser); /* consume the category name */
			if (!check(parser, TOK_RPAREN)) {
				error(parser, "Expected ')' to close @policy(...)");
				return 0;
			}
			advance(parser); /* consume ')' */
			continue;
		}
		if (!check(parser, TOK_IDENT)) {
			error(parser, "Expected decorator name after '@'");
			return 0;
		}
		if (cur_ident_is(parser, "allow_pure_proc", 15)) {
			advance(parser);
			continue;
		}
		if (cur_ident_is(parser, "intrinsic", 9)) {
			/* `@intrinsic` marks a (foreign) decl whose calls the backend lowers to a built-in
			 * instruction (e.g. the raw `syscall`) instead of an ordinary call. No arguments —
			 * recognition is by this marker on the decl, not by the symbol's (mangleable) name. */
			advance(parser);
			continue;
		}
		if (cur_ident_is(parser, "gpu", 3)) {
			/* `@gpu` marks a `map` decl for GPU compute dispatch: the schedule emits a compute shader for it
			 * and dispatches on the GPU (falling back to CPU). No arguments — the marker lives on the decl
			 * (replaces the retired `run map @gpu` site, now that dispatch is by bare name in `#run`). */
			advance(parser);
			continue;
		}
		if (cur_ident_is(parser, "resident", 8)) {
			/* `@resident` marks a pool decl whose columns stay GPU-resident across `@gpu` dispatches
			 * (uploaded once, reused, downloaded only at a `gpu.sync(Pool)`). No arguments — a marker on
			 * the decl, read in lowering (syntax_decl_has_resident_decorator). */
			advance(parser);
			continue;
		}
		if (cur_ident_is(parser, "default", 7)) {
			/* `@default(<kind>, <category>, <policy>)` — a STANDALONE top-level directive setting the
			 * program's failure-policy default for one (effect-kind, op-category) cell. <kind> is the
			 * `proc`/`func` keyword; <category> (bounds/divide/pool) and <policy> are idents. Not a
			 * decorator on a following decl: it wraps as its own SN_DEFAULT_DECL node. Lowering reads
			 * the three argument tokens. */
			advance(parser); /* consume 'default' */
			if (!check(parser, TOK_LPAREN)) {
				error(parser, "Expected '(' after @default — e.g. @default(proc, bounds, abort)");
				return 0;
			}
			advance(parser); /* '(' */
			if (!check(parser, TOK_PROC) && !check(parser, TOK_FUNC)) {
				error(parser, "Expected an effect kind (proc or func) as the first @default argument");
				return 0;
			}
			advance(parser); /* kind */
			if (!match(parser, TOK_COMMA)) {
				error(parser, "Expected ',' after the effect kind in @default(...)");
				return 0;
			}
			if (!check(parser, TOK_IDENT)) {
				error(parser, "Expected a category (bounds, divide, or pool) in @default(...)");
				return 0;
			}
			advance(parser); /* category */
			if (!match(parser, TOK_COMMA)) {
				error(parser, "Expected ',' after the category in @default(...)");
				return 0;
			}
			if (!check(parser, TOK_IDENT)) {
				error(parser, "Expected a policy name in @default(...)");
				return 0;
			}
			advance(parser); /* policy */
			if (!check(parser, TOK_RPAREN)) {
				error(parser, "Expected ')' to close @default(...)");
				return 0;
			}
			advance(parser); /* ')' */
			*out_kind = SN_DEFAULT_DECL;
			return 1;
		}
		if (cur_ident_is(parser, "drop", 4)) {
			/* `@drop(<OpaqueType>)` decl decorator — marks the decorated proc as the destructor
			 * for that opaque type (RAII). The type is named explicitly (not inferred) and must
			 * match the proc's single `own` parameter. Recorded on the decl via a token scan in
			 * cst_build_decl; validated/registered in semantic. */
			advance(parser); /* consume 'drop' */
			if (!check(parser, TOK_LPAREN)) {
				error(parser, "Expected '(' after @drop — name the opaque type, e.g. @drop(socket)");
				return 0;
			}
			advance(parser); /* consume '(' */
			if (!check(parser, TOK_IDENT)) {
				error(parser, "Expected an opaque type name inside @drop(...)");
				return 0;
			}
			advance(parser); /* consume the type name */
			if (!check(parser, TOK_RPAREN)) {
				error(parser, "Expected ')' to close @drop(...)");
				return 0;
			}
			advance(parser); /* consume ')' */
			continue;
		}
		if (cur_ident_is(parser, "syscall", 7)) {
			/* `@syscall(N)` decl decorator — marks a `#foreign` proc as a typed direct syscall number N.
			 * Calls emit the raw syscall asm with the proc's in-params as args (buffers ptrtoint'd); a
			 * buffer the kernel writes is declared in-out (same name in the out-list), so the write is
			 * honest rather than scribbling through a read-only borrow. N is read by a token scan in lower. */
			advance(parser); /* consume 'syscall' */
			if (!check(parser, TOK_LPAREN)) {
				error(parser, "Expected '(' after @syscall — give the syscall number, e.g. @syscall(0)");
				return 0;
			}
			advance(parser); /* consume '(' */
			if (!check(parser, TOK_NUMBER)) {
				error(parser, "Expected a syscall number inside @syscall(...)");
				return 0;
			}
			advance(parser); /* consume the number */
			if (!check(parser, TOK_RPAREN)) {
				error(parser, "Expected ')' to close @syscall(...)");
				return 0;
			}
			advance(parser); /* consume ')' */
			continue;
		}
		if (cur_ident_is(parser, "allow", 5)) {
			advance(parser);
			if (!check(parser, TOK_LPAREN)) {
				error(parser, "Expected '(' after @allow");
				return 0;
			}
			advance(parser); /* consume '(' */
			if (!check(parser, TOK_IDENT)) {
				error(parser, "Expected diagnostic slug inside @allow(...)");
				return 0;
			}
			advance(parser); /* consume slug */
			if (!check(parser, TOK_RPAREN)) {
				error(parser, "Expected ')' to close @allow(...)");
				return 0;
			}
			advance(parser); /* consume ')' */
			continue;
		}
		if (cur_ident_is(parser, "implements", 10)) {
			/* `@implements(<device>.<req>, …)` — driver-side binding: the decorated decl is this
			 * driver's implementation of each named device requirement. Recorded on the decl as
			 * tokens; lowering renames each requirement to this decl's name (`foo` → `bar`). */
			advance(parser); /* consume 'implements' */
			if (!check(parser, TOK_LPAREN)) {
				error(parser,
				      "Expected '(' after @implements — name a device requirement, e.g. @implements(physics.foo)");
				return 0;
			}
			advance(parser); /* consume '(' */
			do {
				if (!check(parser, TOK_IDENT)) {
					error(parser, "Expected a device requirement name inside @implements(...)");
					return 0;
				}
				advance(parser); /* requirement leading segment */
				while (check(parser, TOK_DOT)) {
					advance(parser); /* '.' */
					if (!check(parser, TOK_IDENT)) {
						error(parser, "Expected an identifier after `.` in @implements(...)");
						return 0;
					}
					advance(parser);
				}
			} while (match(parser, TOK_COMMA));
			if (!check(parser, TOK_RPAREN)) {
				error(parser, "Expected ')' to close @implements(...)");
				return 0;
			}
			advance(parser); /* consume ')' */
			continue;
		}
		error(parser, "Unknown decorator (recognized: @allow_pure_proc, @allow(<slug>), @drop(<type>), @intrinsic, "
		              "@gpu, @implements(<device>.<req>, …), @policy(<category>))");
		return 0;
	}

	switch (parser->current.kind) {
	/* Unified grammar: declarations are bindings `name :: <form>`. proc/func/map/arche (and
	 * `extern proc`) are RHS value/type forms (see parse_primary_expr / parse_type_inner), reached
	 * via the IDENT-led default case below. There are no keyword-led top-level declarations. */
	case TOK_USE: {
		/* `#import` is a region in the `#module`/`#file`/`#foreign` family. Two forms:
		 *   bare   `#import io`            one module (trailing `;` optional)
		 *   block  `#import { io net … }`  a group of modules (whitespace/`,`/`;` separated)
		 * An element is either a bare IDENT (a DEVICE, by name) or a STRING literal (a plain MODULE,
		 * by path: `#import { router "./util" }`). Both produce an SN_USE_DECL carrying one IDENT or
		 * STRING token per import; consumers iterate them. */
		advance(parser); /* consume '#import' */
		if (check(parser, TOK_LBRACE)) {
			advance(parser); /* consume '{' */
			if (check(parser, TOK_RBRACE)) {
				error(parser, "empty `#import { }` — list one or more module names");
				return 0;
			}
			while (!check(parser, TOK_RBRACE) && !check(parser, TOK_EOF)) {
				if (!check(parser, TOK_IDENT) && !check(parser, TOK_STRING)) {
					error(parser, "expected a device name or a \"path\" string in `#import { ... }`");
					return 0;
				}
				advance(parser);
			}
			if (!match(parser, TOK_RBRACE)) {
				error(parser, "Expected '}' to close `#import { ... }`");
				return 0;
			}
			*out_kind = SN_USE_DECL;
			return 1;
		}
		/* Bare `#import` is a banner region — like `#foreign`/`#module`/`#file`, it runs to end of
		 * file: every item below it must be a module name. So importing alongside other code REQUIRES
		 * the block form `#import { ... }`; a bare `#import` followed by anything but module names
		 * (to EOF) is an error. No single-module special case. */
		int n = 0;
		while (check(parser, TOK_IDENT) || check(parser, TOK_STRING)) {
			advance(parser);
			n++;
		}
		if (n == 0) {
			error(parser, "Expected a module name after `#import`");
			return 0;
		}
		if (!check(parser, TOK_EOF)) {
			error(parser, "a bare `#import` region runs to end of file — everything below it must be a "
			              "module name; use `#import { ... }` to import alongside other declarations");
			return 0;
		}
		*out_kind = SN_USE_DECL;
		return 1;
	}
	case TOK_HASH_MODULE:
	case TOK_HASH_FILE:
	case TOK_HASH_FOREIGN:
		/* Region marker (`#module` / `#file` / `#foreign`) — banner or `{ ... }` block. */
		return parse_region(parser, out_kind);
	case TOK_HASH_LINK:
		/* `#link { "lib" ... }` — system libraries to link (block form only). */
		return parse_link_region(parser, out_kind);
	case TOK_HASH_RUN:
		/* `#run <expr>` — the program's Schedule value. */
		return parse_run_region(parser, out_kind);
	default:
		/* Top-level declarations: an IDENT-led binding (const / static buffer) or a prefix pool
		 * alloc (`[C]Name…`, which leads with `[`) — see parse_static_decl. */
		if (check(parser, TOK_IDENT) || check(parser, TOK_LBRACKET)) {
			if (parse_static_decl(parser, out_kind))
				return 1;
		}
		error(parser, "Expected declaration");
		return 0;
	}
}

/* ========== EXPRESSION PARSING ========== */

/* After a postfix `[` is matched, parse a SINGLE index `a` or a slice `lo:hi` (lo/hi each optional:
 * `[:hi]`, `[lo:]`, `[:]`). Sets *out_slice=1 for the slice form. Consumes the `]`. A `:` inside the
 * brackets marks a slice. Multi-dimensional access is CHAINED — `a[i][j]`, one index per bracket —
 * which lets each index carry its own failure policy (`a[i] !clamp [j] !abort`); the former comma
 * form `a[i, j]` couldn't, and is now a clear error. */
static int parse_bracket_index_or_slice(Parser *parser, int *out_slice) {
	*out_slice = 0;
	if (check(parser, TOK_COLON)) { /* [:hi] / [:] — no lo */
		*out_slice = 1;
		advance(parser);
		if (!check(parser, TOK_RBRACKET))
			if (!parse_expression(parser))
				return 0;
	} else {
		if (!parse_expression(parser))
			return 0;
		if (check(parser, TOK_COLON)) { /* [lo:hi] / [lo:] */
			*out_slice = 1;
			advance(parser);
			if (!check(parser, TOK_RBRACKET))
				if (!parse_expression(parser))
					return 0;
		} else if (check(parser, TOK_COMMA)) {
			error(parser, "multi-dimensional indexing is chained `a[i][j]` (one index per bracket), not "
			              "comma `a[i, j]` — so each index can take its own failure policy");
			return 0;
		}
	}
	if (!match(parser, TOK_RBRACKET)) {
		error(parser, "Expected ']'");
		return 0;
	}
	return 1;
}

/* After a map/system/each selector, an optional bare `eff` permission marker may
 * precede the `{` body: `system (Q) eff { … }`. `eff` is a CONTEXTUAL keyword (it is
 * a permission only here; elsewhere it is an ordinary identifier — e.g. an `Eff` local
 * named `eff`), so we match the ident text and wrap it as an SN_EFF marker the lowerer
 * detects. Absent ⇒ the kernel is pure (running effects is then a hard error). */
static int parse_opt_eff(Parser *parser) {
	if (cur_ident_is(parser, "eff", 3)) {
		int e_cp = syntax_cp(parser);
		advance(parser); /* consume 'eff' */
		syntax_wrap(parser, e_cp, SN_EFF);
		return 1;
	}
	return 0;
}

/* Optional `as w` row-binder inside a fan's parens (`map (query {…} as w) eff`) — binds the matched row's
 * generation-checked handle to `w` (a `handle(driver)` local) for `delete(w)(ok:)` / relationship filters.
 * `as` is a contextual keyword. Returns 1 if a binder was parsed. */
static int parse_opt_row_bind(Parser *parser) {
	if (cur_ident_is(parser, "as", 2)) {
		advance(parser); /* consume 'as' */
		if (!check(parser, TOK_IDENT)) {
			error(parser, "Expected a name after 'as' — `map (query {…} as w) eff` binds the matched row as `w`");
			return 0;
		}
		int b_cp = syntax_cp(parser);
		advance(parser); /* the binder name */
		syntax_wrap(parser, b_cp, SN_QUERY_BIND);
		return 1;
	}
	return 0;
}

/* Optional `(writes)` permission list after a kernel selector — the bound columns the body may assign:
 * `map (Movers) (pos, vel) { … }`. A comma list of bare column names, each wrapped SN_WRITE_PARAM. It sits
 * between the selector `)` and the optional `eff`. Returns 1 if a list was parsed. */
static int parse_opt_writes(Parser *parser) {
	if (!check(parser, TOK_LPAREN))
		return 0;
	advance(parser); /* consume '(' */
	if (!check(parser, TOK_RPAREN)) {
		do {
			if (!check(parser, TOK_IDENT)) {
				error(parser, "Expected a column name in the `(writes)` list — `map (Movers) (pos, vel) { … }`");
				return 0;
			}
			int w_cp = syntax_cp(parser);
			advance(parser); /* the column name */
			syntax_wrap(parser, w_cp, SN_WRITE_PARAM);
		} while (match(parser, TOK_COMMA) && !check(parser, TOK_RPAREN));
	}
	if (!match(parser, TOK_RPAREN)) {
		error(parser, "Expected ')' to close the `(writes)` list");
		return 0;
	}
	return 1;
}

/* `out_kind` receives the SyntaxNodeKind for the primary expression form parsed,
 * derived from parse context (not from a built AST node). The caller wraps the
 * syntax tree node with it. Left untouched when the primary already wrapped itself (paren). */
static int parse_primary_expr(Parser *parser, SyntaxNodeKind *out_kind) {
	int prim_start = syntax_cp(parser);
	if (check(parser, TOK_NUMBER)) {
		advance(parser);
		*out_kind = SN_LITERAL_EXPR;
		return 1;
	}

	if (check(parser, TOK_STRING)) {
		advance(parser);
		*out_kind = SN_STRING_EXPR;
		return 1;
	}

	if (check(parser, TOK_CHAR_LIT)) {
		advance(parser);
		*out_kind = SN_LITERAL_EXPR;
		return 1;
	}

	if (check(parser, TOK_LBRACE)) {
		advance(parser);
		*out_kind = SN_ARRAY_LIT_EXPR;
		if (!check(parser, TOK_RBRACE)) {
			do {
				if (!parse_expression(parser))
					return 0;
			} while (match(parser, TOK_COMMA) && !check(parser, TOK_RBRACE));
		}
		if (!match(parser, TOK_RBRACE)) {
			error(parser, "Expected '}' after array literal");
			return 0;
		}
		return 1;
	}

	/* Unified-grammar RHS forms — proc/func/map/archetype as value literals or type forms.
	 * The name is the binding LHS, so none of these consume a name. Top-level keyword-led decls
	 * are still handled by parse_decl; these fire only in expression/RHS position. */
	if (check(parser, TOK_PROC) || check(parser, TOK_FUNC)) {
		int is_proc = check(parser, TOK_PROC);
		advance(parser); /* consume 'proc' / 'func' */
		if (check(parser, TOK_LBRACE)) {
			return parse_group_form(parser, out_kind);
		}
		/* Inside a `#foreign` region a bodiless proc is a foreign (FFI) value-form, not a proc
		 * type — drive that off the parser's region state. Funcs are never foreign (FFI bodies
		 * are procs), so a bodiless func is always a func type. */
		return is_proc ? parse_proc_form(parser, parser->in_foreign, out_kind) : parse_func_form(parser, out_kind);
	}
	if (check(parser, TOK_POLICY)) {
		advance(parser); /* consume 'policy' */
		return parse_policy_form(parser, out_kind);
	}
	if (check(parser, TOK_ARCHETYPE)) {
		advance(parser); /* consume 'archetype' */
		*out_kind = SN_ARCH_EXPR;
		if (!match(parser, TOK_LBRACE)) {
			error(parser, "Expected '{' after 'archetype'");
			return 0;
		}
		while (!check(parser, TOK_RBRACE) && !check(parser, TOK_EOF)) {
			drain_pending_trivia(parser);
			if (!parse_arch_field(parser))
				break;
		}
		if (!match(parser, TOK_RBRACE)) {
			error(parser, "Expected '}'");
		}
		return 1;
	}
	if (check(parser, TOK_QUERY)) {
		advance(parser); /* consume 'query' */
		*out_kind = SN_QUERY_EXPR;
		return parse_query_columns(parser);
	}
	if (check(parser, TOK_MAP)) {
		advance(parser); /* consume 'map' */
		*out_kind = SN_MAP_EXPR;
		if (!match(parser, TOK_LPAREN)) {
			error(parser, "Expected '(' after 'map'");
			return 0;
		}
		/* map runs over a query THING in its parens: an inline `query {…}` literal (wrapped as a child
		 * SN_QUERY_EXPR) or a named query (an SN_QUERY_REF leaf). No bare column list. */
		if (check(parser, TOK_QUERY)) {
			int q_cp = syntax_cp(parser);
			advance(parser); /* consume 'query' */
			if (!parse_query_columns(parser))
				return 0;
			syntax_wrap(parser, q_cp, SN_QUERY_EXPR);
		} else if (check(parser, TOK_IDENT)) {
			int ref_cp = syntax_cp(parser);
			advance(parser); /* the query name */
			syntax_wrap(parser, ref_cp, SN_QUERY_REF);
		} else {
			error(parser, "Expected a query in `map(...)` — a name `map(Movers)` or a literal `map(query {…})`");
			return 0;
		}
		/* optional `as w` row-binder (only the effectful per-entity fan binds a row handle). */
		int map_has_bind = parse_opt_row_bind(parser);
		if (check(parser, TOK_COMMA)) {
			error(parser, "maps don't support joins — a map runs over ONE query; to combine pools, nest a "
			              "`map (…) eff` inside another (cross-pool work is explicit nesting, not a join)");
			return 0;
		}
		if (!match(parser, TOK_RPAREN)) {
			error(parser, "Expected ')'");
			return 0;
		}
		/* optional `(writes)` permission list — the bound columns this map assigns. */
		parse_opt_writes(parser);
		/* `map (Q) eff` is the EFFECTFUL per-entity fan — it routes to the SN_EACH_EXPR machinery (per-row
		 * body with control flow + effects), the same kernel the removed `each` keyword produced. A plain
		 * `map` stays the pure branch-free column transform (SN_MAP_EXPR, E0046-restricted). */
		if (parse_opt_eff(parser)) {
			*out_kind = SN_EACH_EXPR;
		} else if (map_has_bind) {
			error(parser, "the `as` row-binder requires the `eff` permission — it binds a handle for effectful "
			              "row ops (`delete(w)`): write `map (query {…} as w) eff { … }`");
			return 0;
		}
		return parse_block_body(parser);
	}
	if (check(parser, TOK_SYSTEM)) {
		advance(parser); /* consume 'system' */
		*out_kind = SN_SYSTEM_EXPR;
		/* `system { body }` runs once. `system(Q) { body }` fans over a query. `system(Q1, Q2) { body }` is
		 * a JOIN — a comma-separated tuple of source-agnostic queries, each wrapped as its own child. */
		if (check(parser, TOK_LPAREN)) {
			advance(parser); /* consume '(' */
			do {
				if (check(parser, TOK_QUERY)) {
					int q_cp = syntax_cp(parser);
					advance(parser); /* consume 'query' */
					if (!parse_query_columns(parser))
						return 0;
					syntax_wrap(parser, q_cp, SN_QUERY_EXPR);
				} else if (check(parser, TOK_IDENT)) {
					int ref_cp = syntax_cp(parser);
					advance(parser); /* the query name */
					syntax_wrap(parser, ref_cp, SN_QUERY_REF);
				} else {
					error(parser, "Expected a query in `system(...)` — a name `system(Drawables)` or a literal "
					              "`system(query {…})`");
					return 0;
				}
			} while (match(parser, TOK_COMMA));
			if (!match(parser, TOK_RPAREN)) {
				error(parser, "Expected ')'");
				return 0;
			}
			/* optional `(writes)` permission list — the bound columns this system assigns. */
			parse_opt_writes(parser);
		}
		parse_opt_eff(parser);
		return parse_block_body(parser);
	}
	if (check(parser, TOK_ENUM)) {
		advance(parser); /* consume 'enum' */
		*out_kind = SN_ENUM_EXPR;
		if (!match(parser, TOK_LBRACE)) {
			error(parser, "Expected '{' after 'enum'");
			return 0;
		}
		if (!check(parser, TOK_RBRACE)) {
			do {
				if (check(parser, TOK_RBRACE)) /* trailing comma */
					break;
				int v_cp = syntax_cp(parser);
				if (!check(parser, TOK_IDENT)) {
					error(parser, "Expected enum variant name");
					return 0;
				}
				advance(parser); /* variant name */
				if (match(parser, TOK_EQ)) {
					if (!check(parser, TOK_NUMBER)) {
						error(parser, "Expected integer after '=' in enum variant");
						return 0;
					}
					advance(parser); /* explicit value */
				}
				syntax_wrap(parser, v_cp, SN_ENUM_VARIANT);
			} while (match(parser, TOK_COMMA) && !check(parser, TOK_RBRACE));
		}
		if (!match(parser, TOK_RBRACE)) {
			error(parser, "Expected '}' to close enum");
			return 0;
		}
		return 1;
	}
	if (check(parser, TOK_IDENT)) {
		int prim_name_cp = syntax_cp(parser);
		int is_table = cur_ident_is(parser, "table", 5);
		/* `sum` is a CONTEXTUAL keyword (not a hard token — `sum` is far too common an identifier):
		 * recognized only as the value-form `Name :: sum { … }`. Checked before the entity-literal `{`. */
		int is_sum = cur_ident_is(parser, "sum", 3);
		/* `reduce`/`scan` take a monoid operator as their FIRST argument — `+`, `*`, or a named op
		 * (`min`/`max`). The operator forms (`+`/`*`) aren't expressions, so the call-arg parse below
		 * accepts an operator token there and wraps it as a literal carrying the op text. */
		int is_collective = cur_ident_is(parser, "reduce", 6) || cur_ident_is(parser, "scan", 4);
		advance(parser);

		/* table<Name> in value position: the singleton table for shape Name. */
		if (is_table && check(parser, TOK_LT)) {
			advance(parser); /* consume < */
			if (!check(parser, TOK_IDENT)) {
				error(parser, "Expected archetype name in 'table<'");
				return 0;
			}
			advance(parser);
			if (!match(parser, TOK_GT)) {
				error(parser, "Expected '>' after archetype name in 'table<...>'");
				return 0;
			}
			*out_kind = SN_NAME_EXPR;
			return 1;
		}

		/* `sum { variant(types), … }` — a tagged-union type definition. Checked before the entity
		 * literal (which also leads `IDENT {`): a sum body is variant constructors, not `field: val`. */
		if (is_sum && check(parser, TOK_LBRACE)) {
			*out_kind = SN_SUM_EXPR;
			advance(parser); /* consume '{' */
			while (!check(parser, TOK_RBRACE) && !check(parser, TOK_EOF)) {
				int v_cp = syntax_cp(parser);
				if (!check(parser, TOK_IDENT)) {
					error(parser, "Expected sum variant name");
					return 0;
				}
				advance(parser); /* variant name */
				if (match(parser, TOK_LPAREN)) {
					if (!check(parser, TOK_RPAREN)) {
						do {
							if (!parse_type(parser))
								return 0;
						} while (match(parser, TOK_COMMA));
					}
					if (!match(parser, TOK_RPAREN)) {
						error(parser, "Expected ')' to close sum variant payload");
						return 0;
					}
				}
				syntax_wrap(parser, v_cp, SN_SUM_VARIANT);
				match(parser, TOK_COMMA); /* optional separator */
			}
			if (!match(parser, TOK_RBRACE)) {
				error(parser, "Expected '}' to close sum");
				return 0;
			}
			return 1;
		}

		/* entity literal `Name { field: val, ... }` — a row value of archetype/query `Name` with NAMED
		 * fields (order-free). The leading name is a bare IDENT token; each field name is wrapped
		 * SN_FIELD_NAME so lowering tells the type-name apart from the fields. Suppressed in a `match`
		 * scrutinee (no_brace_lit), where the `{` opens the arm block, not an entity. */
		if (check(parser, TOK_LBRACE) && !parser->no_brace_lit) {
			*out_kind = SN_ENTITY_EXPR;
			advance(parser); /* consume '{' */
			if (!check(parser, TOK_RBRACE)) {
				do {
					if (check(parser, TOK_RBRACE)) /* trailing comma */
						break;
					if (!check(parser, TOK_IDENT)) {
						error(parser, "Expected field name in entity literal");
						return 0;
					}
					int field_name_cp = syntax_cp(parser);
					advance(parser);
					syntax_wrap(parser, field_name_cp, SN_FIELD_NAME);
					if (!match(parser, TOK_COLON)) {
						error(parser, "Expected ':' after field name in entity literal");
						return 0;
					}
					/* A tuple-group column takes a tuple value `(a, b)`; `(a, b)` isn't an expression (a paren
					 * holds one), so parse a comma list as SN_TUPLE_LIT when it has >1 element. `(e)` stays a
					 * normal parenthesized expression. */
					if (check(parser, TOK_LPAREN)) {
						int tup_cp = syntax_cp(parser);
						advance(parser); /* '(' */
						int nelem = 0;
						if (!check(parser, TOK_RPAREN)) {
							do {
								if (!parse_expression(parser))
									return 0;
								nelem++;
							} while (match(parser, TOK_COMMA) && !check(parser, TOK_RPAREN));
						}
						if (!match(parser, TOK_RPAREN)) {
							error(parser, "Expected ')' to close tuple value");
							return 0;
						}
						syntax_wrap(parser, tup_cp, nelem > 1 ? SN_TUPLE_LIT : SN_PAREN_EXPR);
					} else if (!parse_expression(parser)) {
						return 0;
					}
				} while (match(parser, TOK_COMMA) && !check(parser, TOK_RBRACE));
			}
			if (!match(parser, TOK_RBRACE)) {
				error(parser, "Expected '}' to close entity literal");
				return 0;
			}
			return 1;
		}

		/* field access (and optional trailing index): `a.b.c[i]` */
		if (match(parser, TOK_DOT)) {
			if (!check(parser, TOK_IDENT)) {
				error(parser, "Expected field name after '.'");
				return 0;
			}
			int field_name_cp = syntax_cp(parser);
			advance(parser);
			syntax_wrap(parser, field_name_cp, SN_FIELD_NAME);

			while (match(parser, TOK_DOT)) {
				if (!check(parser, TOK_IDENT)) {
					error(parser, "Expected field name after '.'");
					return 0;
				}
				int chained_field_cp = syntax_cp(parser);
				advance(parser);
				syntax_wrap(parser, chained_field_cp, SN_FIELD_NAME);
			}

			if (match(parser, TOK_LBRACKET)) {
				int is_slice;
				if (!parse_bracket_index_or_slice(parser, &is_slice))
					return 0;
				*out_kind = is_slice ? SN_SLICE_EXPR : SN_INDEX_EXPR;
				return 1;
			}

			/* Qualified call: `mod.name(args)`. The callee is the field-access (base IDENT +
			 * SN_FIELD_NAME); args are the expr-node children. No SN_CALLEE_NAME wrap — lowering
			 * detects a field callee by the presence of SN_FIELD_NAME children. */
			if (check(parser, TOK_LPAREN)) {
				advance(parser); /* consume '(' */
				if (!check(parser, TOK_RPAREN)) {
					do {
						if (!parse_expression(parser))
							return 0;
					} while (match(parser, TOK_COMMA) && !check(parser, TOK_RPAREN));
				}
				if (!match(parser, TOK_RPAREN)) {
					error(parser, "Expected ')' after arguments");
					return 0;
				}
				*out_kind = SN_CALL_EXPR;
				return 1;
			}

			*out_kind = SN_FIELD_EXPR;
			return 1;
		}

		/* indexing `a[i]` (multi-dim is chained `a[i][j]`, one index per bracket), or sub-slice
		 * `a[lo:hi]`. */
		if (match(parser, TOK_LBRACKET)) {
			int is_slice;
			if (!parse_bracket_index_or_slice(parser, &is_slice))
				return 0;
			*out_kind = is_slice ? SN_SLICE_EXPR : SN_INDEX_EXPR;
			return 1;
		}

		/* function call: `f(args)` — wrap the callee name, then consume '(' as a sibling. */
		if (check(parser, TOK_LPAREN)) {
			syntax_wrap(parser, prim_name_cp, SN_CALLEE_NAME);
			advance(parser); /* consume '(' */
			if (!check(parser, TOK_RPAREN)) {
				/* `reduce`/`scan` first arg = monoid operator. Only `+`/`*` need special handling — they
				 * are operator tokens, not expressions, so wrap one as a literal whose lexeme IS the op
				 * text. A named op (`min`/`max`) is a plain IDENT that parses as a normal name argument.
				 * The op is validated in semantic (so a user identifier named `reduce` isn't broken here). */
				if (is_collective && (check(parser, TOK_PLUS) || check(parser, TOK_STAR))) {
					int op_cp = syntax_cp(parser);
					advance(parser);
					syntax_wrap(parser, op_cp, SN_LITERAL_EXPR);
					match(parser, TOK_COMMA);
				}
				do {
					if (check(parser, TOK_RPAREN))
						break;
					if (!parse_expression(parser))
						return 0;
				} while (match(parser, TOK_COMMA) && !check(parser, TOK_RPAREN));
			}
			if (!match(parser, TOK_RPAREN)) {
				error(parser, "Expected ')' after arguments");
				return 0;
			}
			*out_kind = SN_CALL_EXPR;
			return 1;
		}

		*out_kind = SN_NAME_EXPR;
		return 1;
	}

	if (match(parser, TOK_LPAREN)) {
		parse_expression(parser);
		match(parser, TOK_COMMA); /* tolerate a trailing comma — trailing commas are valid in every list */
		if (!match(parser, TOK_RPAREN)) {
			error(parser, "Expected ')' after expression");
		}
		syntax_wrap(parser, prim_start, SN_PAREN_EXPR);
		return 1;
	}

	error(parser, "Expected expression");
	return 0;
}

/* Prefix unary operators: `-x` (negate) and `!x` (logical not). Binds tighter
 * than binary operators, looser than postfix (calls/indexing in primary). */
/* Consume an optional postfix failure-policy `!policy` / `?policy` on a just-parsed index/slice/call,
 * wrapping it as an SN_POLICY_REF child of that node. With chained indexing this runs after EACH `[i]`
 * so every index carries its own policy — `foo[i] !clamp [j] !abort`. */
static int parse_opt_policy(Parser *parser, SyntaxNodeKind kind) {
	if ((kind == SN_INDEX_EXPR || kind == SN_SLICE_EXPR || kind == SN_CALL_EXPR) &&
	    (check(parser, TOK_BANG) || check(parser, TOK_QUESTION))) {
		int pol_cp = syntax_cp(parser);
		advance(parser); /* consume '!' or '?' */
		if (!check(parser, TOK_IDENT)) {
			error(parser, "Expected a policy name after the sigil (e.g. `a[i] !clamp`, `insert(P,x) ?reject`)");
			return 0;
		}
		advance(parser); /* consume the policy ident */
		syntax_wrap(parser, pol_cp, SN_POLICY_REF);
	}
	return 1;
}

static int parse_unary_expr(Parser *parser) {
	int u_cp = syntax_cp(parser);
	/* `move <expr>` / `copy <expr>` — call-site ownership markers; transparent to the
	 * grammar (the syntax tree records the keyword token, read back by cst_to_program). */
	if (check(parser, TOK_MOVE) || check(parser, TOK_COPY)) {
		advance(parser);
		if (!parse_unary_expr(parser))
			return 0;
		syntax_wrap(parser, u_cp, SN_UNARY_EXPR);
		return 1;
	}
	if (check(parser, TOK_MINUS) || check(parser, TOK_BANG)) {
		advance(parser);
		if (!parse_unary_expr(parser)) /* allow -(-x), !!x */
			return 0;
		syntax_wrap(parser, u_cp, SN_UNARY_EXPR);
		return 1;
	}
	/* Primary expression: wrap it by the kind it parsed into (tracked by parse
	 * context in `prim_kind`), unless it already collapsed to a single node (a
	 * parenthesised expr wraps itself). */
	int p_cp = syntax_cp(parser);
	SyntaxNodeKind prim_kind = SN_NAME_EXPR;
	if (!parse_primary_expr(parser, &prim_kind))
		return 0;
	/* Policy on the FIRST postfix group (`a[i] !clamp`, `insert(P,x) ?reject`) — emitted before the
	 * chaining loop so it's wrapped INTO the first index/call node. */
	if (!parse_opt_policy(parser, prim_kind))
		return 0;
	/* General postfix CHAINING past the first group: a `.field` or `[index]`/`[lo:hi]` that follows a
	 * closed `]`/`)` NESTS — the node parsed so far becomes the base of the next (`a[i].b` → FIELD over
	 * INDEX, `f().g` → FIELD over CALL, `f()[i]` → INDEX over CALL, `a.b[i].c` → FIELD over INDEX). The
	 * FIRST group stays flat (parse_primary_expr), preserving `a.b.c`, `a.b.c[i]`, and qualified calls
	 * `mod.f(args)` whose flat SN_FIELD_NAME children lowering relies on. A trailing `(` is deliberately
	 * NOT chained: `call(args)(outs)` is out-param binding, not a call-on-call, and a call on a postfix
	 * result isn't a target here. Re-wrapping from `p_cp` each step encloses the prior node as base. */
	while ((prim_kind == SN_INDEX_EXPR || prim_kind == SN_SLICE_EXPR || prim_kind == SN_CALL_EXPR ||
	        prim_kind == SN_FIELD_EXPR) &&
	       (check(parser, TOK_DOT) || check(parser, TOK_LBRACKET))) {
		syntax_wrap(parser, p_cp, prim_kind); /* close the node parsed so far → it becomes the base */
		if (check(parser, TOK_DOT)) {
			do {
				advance(parser); /* consume '.' */
				if (!check(parser, TOK_IDENT)) {
					error(parser, "Expected field name after '.'");
					return 0;
				}
				int fcp = syntax_cp(parser);
				advance(parser); /* consume the field ident */
				syntax_wrap(parser, fcp, SN_FIELD_NAME);
			} while (check(parser, TOK_DOT));
			prim_kind = SN_FIELD_EXPR;
		} else {             /* TOK_LBRACKET */
			advance(parser); /* consume '[' */
			int is_slice;
			if (!parse_bracket_index_or_slice(parser, &is_slice))
				return 0;
			prim_kind = is_slice ? SN_SLICE_EXPR : SN_INDEX_EXPR;
			/* this index's own failure policy — `foo[i] !clamp [j] !abort` */
			if (!parse_opt_policy(parser, prim_kind))
				return 0;
		}
	}
	if (!syntax_single_node(parser, p_cp))
		syntax_wrap(parser, p_cp, prim_kind);
	return 1;
}

/* Binary-operator precedence: higher binds tighter, -1 if not a binary op.
   All operators are left-associative.
     3: * /        2: + -        1: < > <= >= == != */
static int binop_prec(TokenKind k) {
	switch (k) {
	case TOK_STAR:
	case TOK_SLASH:
	case TOK_PERCENT:
		return 5;
	case TOK_PLUS:
	case TOK_MINUS:
		return 4;
	case TOK_LT:
	case TOK_GT:
	case TOK_LT_EQ:
	case TOK_GT_EQ:
	case TOK_EQ_EQ:
	case TOK_BANG_EQ:
		return 3;
	case TOK_AMP_AMP:
		return 2;
	case TOK_PIPE_PIPE:
		return 1;
	case TOK_PIPE_GT:
		return 1; /* |> binds loosest: `ext(a, b) |> fin` groups the call as the left operand */
	default:
		return -1;
	}
}

/* Precedence climbing: extend `left` with binary operators whose precedence is
   >= min_prec, so e.g. `2 + 3 * 2` builds (2 + (3 * 2)) rather than a flat fold. */
/* `left_cp` is the syntax tree checkpoint taken before `left` was parsed, so each fold can
 * retroactively wrap [left_cp .. end-of-right] into a SN_BINARY_EXPR — left-assoc
 * nesting falls out because the previous fold collapses to one node at left_cp. */
static int parse_binary_rhs(Parser *parser, int ok_left, int left_cp, int min_prec) {
	if (!ok_left)
		return 0;

	for (;;) {
		int prec = binop_prec(parser->current.kind);
		if (prec < min_prec)
			return 1;

		TokenKind op = parser->current.kind; /* remembered for the `/ !policy` check below */
		advance(parser);                     /* consume the operator token */

		int right_cp = syntax_cp(parser);
		if (!parse_unary_expr(parser))
			return 0;

		/* Left-associative: only fold a strictly tighter operator into `right`. */
		while (binop_prec(parser->current.kind) > prec) {
			if (!parse_binary_rhs(parser, 1, right_cp, prec + 1))
				return 0;
		}

		/* Divide/mod failure policy: `a / b !policy` / `a % b !policy` (div-by-zero). Parsed at the
		 * operator level (not the postfix level, where it would wrongly attach to `b`); the SN_POLICY_REF
		 * is wrapped before the binary node closes, so it becomes a child of the op. */
		if ((op == TOK_SLASH || op == TOK_PERCENT) && check(parser, TOK_BANG)) {
			int pol_cp = syntax_cp(parser);
			advance(parser); /* consume '!' */
			if (!check(parser, TOK_IDENT)) {
				error(parser, "Expected a policy name after '!' (e.g. `a / b !zero`)");
				return 0;
			}
			advance(parser); /* consume the policy ident */
			syntax_wrap(parser, pol_cp, SN_POLICY_REF);
		}

		/* Retroactively wrap [left_cp .. end-of-right] into a binary node; the next
		 * fold reuses left_cp so left-assoc nesting falls out. */
		syntax_wrap(parser, left_cp, SN_BINARY_EXPR);
	}
}

static int parse_binary_expr(Parser *parser) {
	int left_cp = syntax_cp(parser);
	int ok = parse_unary_expr(parser);
	return parse_binary_rhs(parser, ok, left_cp, 1);
}

static int parse_expression(Parser *parser) {
	return parse_binary_expr(parser);
}

/* ========== STATEMENT PARSING ========== */

/* Parse the tail of a binding statement after its first target name has been consumed.
 * `name` (owned) is the first target; the current token is at `,` (multi-bind), `:`
 * (`x := e` or `x: T [= e]`), or `=` (legacy `x = e`). Returns STMT_BIND / STMT_MULTI_BIND,
 * or NULL on error. There is no `let` keyword — bindings are recognized by this shape. */
static int parse_binding_tail(Parser *parser, SyntaxNodeKind *out_kind) {
	/* Multi-value binding: `a, b, c := expr` (or legacy `= expr`). The first name was
	 * already consumed by the caller and is recorded in the syntax tree. */
	if (match(parser, TOK_COMMA)) {
		*out_kind = SN_MULTI_BIND_STMT;
		while (!check(parser, TOK_COLON) && !check(parser, TOK_EQ) && !check(parser, TOK_EOF)) {
			if (!check(parser, TOK_IDENT)) {
				error(parser, "Expected variable name in multi-value binding");
				return 0;
			}
			advance(parser);
			if (!match(parser, TOK_COMMA))
				break;
		}
		if (match(parser, TOK_COLON)) {
			if (!match(parser, TOK_EQ)) {
				error(parser, "Expected '=' after ':' in multi-value binding");
				return 0;
			}
		} else if (!match(parser, TOK_EQ)) {
			error(parser, "Expected ':=' or '=' in multi-value binding");
			return 0;
		}
		if (!parse_expression(parser))
			return 0;
		return 1;
	}

	/* Single binding. The universal form is `name : [type] (: | =) value`:
	 *   `x := e`        — variable, inferred type
	 *   `x : T = e`     — variable, explicit type
	 *   `x : T`         — variable, explicit type, no init (zero-init)
	 *   `x :: e`        — constant, inferred type (value const, or a type alias if e denotes a type)
	 *   `x : T : e`     — constant, explicit type/meta (`x : type : T` is a local type alias)
	 *   `x = e`         — legacy assignment-style binding
	 * `:` separator ⇒ constant; `=` ⇒ variable. cst_to_program re-derives all of this
	 * from the syntax tree tokens; here we only consume + drive the syntax tree. */
	if (match(parser, TOK_COLON)) {
		if (check(parser, TOK_EQ)) {
			advance(parser); /* `:=` — inferred variable */
			int rhs_cp = syntax_cp(parser);
			if (!parse_expression(parser))
				return 0;
			if (syntax_single_node_kind(parser, rhs_cp) == SN_ARCH_EXPR) {
				error(parser, "archetypes must be declared at global scope — move this `arche { … }` shape "
				              "out of the proc/block (anonymous `arche { … }` literals in expressions are fine)");
				return 0;
			}
		} else if (check(parser, TOK_COLON)) {
			advance(parser); /* `::` — inferred-meta local constant */
			/* `k :: alias T` — consume the transparent-alias marker; backing parses as the value. */
			if (parser->current.kind == TOK_ALIAS)
				advance(parser);
			int rhs_cp = syntax_cp(parser);
			if (!parse_expression(parser))
				return 0;
			if (syntax_single_node_kind(parser, rhs_cp) == SN_ARCH_EXPR) {
				error(parser, "archetypes must be declared at global scope — move this `arche { … }` shape "
				              "out of the proc/block (anonymous `arche { … }` literals in expressions are fine)");
				return 0;
			}
		} else {
			TypeForm tf;
			if (!parse_type_form(parser, &tf)) /* explicit declared type / meta `T` */
				return 0;
			if (match(parser, TOK_COLON)) {
				/* `x : T : value` — explicit-meta/typed local constant. */
				if (tf.is_type_meta) {
					/* `x : type : <type>` — a local type alias; the RHS is a type. */
					if (!parse_type(parser))
						return 0;
				} else {
					if (!parse_expression(parser)) /* typed value const */
						return 0;
				}
			} else if (match(parser, TOK_EQ)) {
				if (!parse_expression(parser)) /* `x : T = e` variable */
					return 0;
			}
			/* else `x : T` — variable, no initializer */
		}
	} else if (match(parser, TOK_EQ)) {
		if (!parse_expression(parser))
			return 0;
	} else {
		error(parser, "Expected ':' or '=' after variable name");
		return 0;
	}
	*out_kind = SN_BIND_STMT;
	return 1;
}

/* Parse a "simple statement" — a binding (`x := e`, `x: T [= e]`, `a, b := e`), an assignment
 * (`lvalue op= e`), or an expression — WITHOUT consuming a terminator. The caller consumes
 * whatever terminates it (`;` for a statement, `;`/`)` for the parts of a `for` header). This
 * is the one place these forms are parsed; `for` no longer hand-rolls its own. */
static int parse_simple_statement(Parser *parser, SyntaxNodeKind *out_kind) {
	int target_cp = syntax_cp(parser);
	if (!parse_expression(parser))
		return 0;

	/* Bare binding: a plain name (the target collapsed to a single SN_NAME_EXPR) then `:`
	 * (`x := e` / `x: T [= e]`) or `,` (`a, b := e`). The name token is already in the syntax tree. */
	if (syntax_single_node_kind(parser, target_cp) == SN_NAME_EXPR &&
	    (check(parser, TOK_COLON) || check(parser, TOK_COMMA))) {
		return parse_binding_tail(parser, out_kind);
	}

	/* Proc-call statement: `foo(in)(out)`. The call `foo(in)` was parsed above as a CALL_EXPR; a
	 * following `(` opens the out-argument list — caller-provided places written in place. Each
	 * out-arg is `name` (existing place), `name:` (declare, type from the out-param), or `name: T`
	 * (declare, typed). A proc is an action, so this is a statement, never a value. */
	if (syntax_single_node_kind(parser, target_cp) == SN_CALL_EXPR && check(parser, TOK_LPAREN)) {
		advance(parser); /* consume out-list `(` */
		if (!check(parser, TOK_RPAREN)) {
			do {
				int out_arg_cp = syntax_cp(parser);
				if (!check(parser, TOK_IDENT)) {
					error(parser, "Expected out-argument name");
					return 0;
				}
				advance(parser); /* the out-arg name IDENT stays a direct token child of SN_OUT_ARG */
				if (match(parser, TOK_COLON)) {
					/* `name:` — declare, type inferred from the out-param; `name: T` — declare typed. */
					if (!check(parser, TOK_RPAREN) && !check(parser, TOK_COMMA)) {
						if (!parse_type(parser))
							return 0;
					}
				}
				syntax_wrap(parser, out_arg_cp, SN_OUT_ARG);
			} while (match(parser, TOK_COMMA) && !check(parser, TOK_RPAREN));
		}
		if (!match(parser, TOK_RPAREN)) {
			error(parser, "Expected ')' after out-arguments");
			return 0;
		}
		*out_kind = SN_PROC_CALL_STMT;
		return 1;
	}

	/* Assignment: `lvalue = / += / -= / *= / /= / %= expr`. */
	if (check(parser, TOK_EQ) || check(parser, TOK_PLUS_EQ) || check(parser, TOK_MINUS_EQ) ||
	    check(parser, TOK_STAR_EQ) || check(parser, TOK_SLASH_EQ) || check(parser, TOK_PERCENT_EQ)) {
		advance(parser);
		if (!parse_expression(parser))
			return 0;
		*out_kind = SN_ASSIGN_STMT;
		return 1;
	}

	/* Otherwise an expression statement. */
	*out_kind = SN_EXPR_STMT;
	return 1;
}

static int parse_statement(Parser *parser) {
	int stmt_cp = syntax_cp(parser);
	int ok = 0; /* 1 once a statement has been parsed (drives the cleanup wrap) */
	/* syntax tree wrap kind for the statement, tracked by parse context (not a built AST node).
	 * Each branch that produces a statement sets it before `goto cleanup`. */
	SyntaxNodeKind stmt_kind = SN_ERROR;
	Trivia *leading = NULL;
	int leading_count = 0;
	/* Prevent stack overflow from unbounded recursion */
	const int MAX_RECURSION_DEPTH = 1000;
	if (parser->recursion_depth > MAX_RECURSION_DEPTH) {
		error(parser, "Recursion limit exceeded");
		goto cleanup;
	}
	parser->recursion_depth++;

	/* Drain pending trivia (comments/blank lines) to keep comment association tidy; the
	 * parser no longer builds an AST to attach it to, so the drained trivia is freed at
	 * cleanup. Comments survive in the syntax tree as their own leaves regardless. */
	take_pending_as_leading(parser, &leading, &leading_count);

	if (match(parser, TOK_SEMI)) {
		goto cleanup; /* empty statement */
	}

	if (match(parser, TOK_BREAK)) {
		if (!match(parser, TOK_SEMI)) {
			error(parser, "Expected ';' after break");
		}
		stmt_kind = SN_BREAK_STMT;
		ok = 1;
		goto cleanup;
	}

	if (match(parser, TOK_CONTINUE)) {
		if (!match(parser, TOK_SEMI)) {
			error(parser, "Expected ';' after continue");
		}
		stmt_kind = SN_CONTINUE_STMT;
		ok = 1;
		goto cleanup;
	}

	if (match(parser, TOK_EACH_FIELD)) {
		if (!check(parser, TOK_IDENT)) {
			error(parser, "Expected binding name after 'each_field'");
			goto cleanup;
		}
		advance(parser);

		if (match(parser, TOK_COLON)) {
			if (!parse_type(parser))
				goto cleanup;
		}

		if (!match(parser, TOK_IN)) {
			error(parser, "Expected 'in' after each_field binding");
			goto cleanup;
		}

		if (!check(parser, TOK_IDENT)) {
			error(parser, "Expected archetype parameter name after 'in'");
			goto cleanup;
		}
		advance(parser);

		if (!match(parser, TOK_LBRACE)) {
			error(parser, "Expected '{' to start each_field body");
			goto cleanup;
		}

		while (!check(parser, TOK_RBRACE) && !check(parser, TOK_EOF)) {
			if (!parse_statement(parser))
				break;
		}

		if (!match(parser, TOK_RBRACE)) {
			error(parser, "Expected '}' to close each_field body");
		}

		stmt_kind = SN_EACH_FIELD_STMT;
		ok = 1;
		goto cleanup;
	}

	if (match(parser, TOK_RETURN)) {
		/* `return;` — naked early exit (valid in a proc/map); or `return e1, …, en` — a list of
		 * returned values (a single return is just count 1, for a func). The expression list is
		 * present only when the next token isn't `;`. */
		if (!check(parser, TOK_SEMI)) {
			if (!parse_expression(parser)) {
				error(parser, "Expected expression after 'return'");
				goto cleanup;
			}
			while (match(parser, TOK_COMMA) && !check(parser, TOK_SEMI)) {
				if (!parse_expression(parser)) {
					error(parser, "Expected expression after ',' in return statement");
					goto cleanup;
				}
			}
		}

		if (!match(parser, TOK_SEMI)) {
			error(parser, "Expected ';' after return statement");
		}

		stmt_kind = SN_RETURN_STMT;
		ok = 1;
		goto cleanup;
	}

	/* The `run <map>` STATEMENT is RETIRED — a map/each/system is dispatched ONLY by naming it in `#run`
	 * (a system body never dispatches). `run` is no longer a keyword; `run X;` now parses as two
	 * identifiers and errors. GPU dispatch moved to a `@gpu` decorator on the map decl. */

	if (check(parser, TOK_MATCH)) {
		advance(parser); /* consume 'match' */
		int prev_nbl = parser->no_brace_lit;
		parser->no_brace_lit = 1;                /* a bare `IDENT {` here is the arm block, not an entity */
		int scrut_ok = parse_expression(parser); /* scrutinee */
		parser->no_brace_lit = prev_nbl;
		if (!scrut_ok)
			goto cleanup;
		if (!match(parser, TOK_LBRACE)) {
			error(parser, "Expected '{' after match scrutinee");
			goto cleanup;
		}
		while (!check(parser, TOK_RBRACE) && !check(parser, TOK_EOF)) {
			int arm_cp = syntax_cp(parser);
			/* pattern: a QUALIFIED enum case `Enum.case` (a value is always written the same way —
			 * qualified by its descriptor), the `_` wildcard, or an int/string/char literal. A bare case
			 * name is NOT a value and is rejected — match it `Enum.case`. */
			if (check(parser, TOK_NUMBER) || check(parser, TOK_STRING) || check(parser, TOK_CHAR_LIT)) {
				advance(parser); /* literal value pattern */
			} else if (check(parser, TOK_IDENT)) {
				int is_wild = (parser->current.length == 1 && parser->current.start[0] == '_');
				advance(parser); /* the descriptor name (or `_`) */
				if (check(parser, TOK_DOT)) {
					advance(parser); /* `.` */
					if (!check(parser, TOK_IDENT)) {
						error(parser, "Expected an enum case after '.' in a match pattern");
						break;
					}
					advance(parser); /* the case */
				} else if (!is_wild) {
					error(parser,
					      "a match arm names an enum value — qualify it `Enum.case` (a bare case name is not a value)");
					break;
				}
			} else {
				error(parser, "Expected match pattern (`Enum.case`, literal, or '_')");
				break;
			}
			if (!match(parser, TOK_COLON)) {
				error(parser, "Expected ':' after match pattern");
				break;
			}
			/* body: a `{ … }` block or a single statement */
			if (check(parser, TOK_LBRACE)) {
				if (!parse_block_body(parser))
					break;
			} else if (!parse_statement(parser)) {
				synchronize(parser);
			}
			syntax_wrap(parser, arm_cp, SN_MATCH_ARM);
			match(parser, TOK_COMMA); /* optional separator between arms */
		}
		if (!match(parser, TOK_RBRACE))
			error(parser, "Expected '}' to close match");
		stmt_kind = SN_MATCH_STMT;
		ok = 1;
		goto cleanup;
	}

	/* A standalone `{ … }` block statement (a nested scope). A statement that starts with `{` is a
	 * block, not an array-literal expression-statement (a bare array literal has no effect). */
	if (check(parser, TOK_LBRACE)) {
		if (!parse_block_body(parser))
			goto cleanup;
		stmt_kind = SN_BLOCK;
		ok = 1;
		goto cleanup;
	}

	/* Paren-based multi-bind: `(x, y:, n: int) = expr;` — a target with a trailing `:`
	 * (optionally a type) declares a new variable; a bare name assigns to an existing one. */
	if (match(parser, TOK_LPAREN)) {
		/* Parse binding targets: `name` (assign) or `name:` / `name: T` (declare). */
		int loop_count = 0;
		const int MAX_TARGETS = 1000;
		while (!check(parser, TOK_RPAREN) && !check(parser, TOK_EOF)) {
			if (++loop_count > MAX_TARGETS) {
				error(parser, "Too many binding targets");
				break;
			}
			if (!check(parser, TOK_IDENT)) {
				error(parser, "Expected variable name in binding");
				break;
			}
			advance(parser);
			if (match(parser, TOK_COLON)) {
				if (!check(parser, TOK_COMMA) && !check(parser, TOK_RPAREN)) {
					if (!parse_type(parser))
						goto cleanup;
				}
			}
			if (!match(parser, TOK_COMMA))
				break;
		}

		if (!match(parser, TOK_RPAREN)) {
			error(parser, "Expected ')' after binding targets");
			goto cleanup;
		}

		if (!match(parser, TOK_EQ)) {
			error(parser, "Expected '=' after binding targets");
			goto cleanup;
		}

		if (!parse_expression(parser))
			goto cleanup;

		if (!match(parser, TOK_SEMI)) {
			error(parser, "Expected ';' after binding statement");
		}

		stmt_kind = SN_MULTI_BIND_STMT;
		ok = 1;
		goto cleanup;
	}

	/* `let` was removed — bindings are recognized by shape (handled in the assignment/expr
	 * fallback below). A leftover `let` is a clear error so old code is caught, not silently
	 * misparsed. */
	if (check(parser, TOK_LET)) {
		error(parser, "`let` was removed — write a bare binding: `x := expr`, `x: T = expr`, or `a, b := expr`");
		advance(parser);
		goto cleanup;
	}

	if (match(parser, TOK_IF)) {
		if (!match(parser, TOK_LPAREN)) {
			error(parser, "Expected '(' after 'if'");
			goto cleanup;
		}
		if (!parse_expression(parser)) {
			goto cleanup;
		}
		if (!match(parser, TOK_RPAREN)) {
			error(parser, "Expected ')' after if condition");
			goto cleanup;
		}

		if (match(parser, TOK_LBRACE)) {
			while (!check(parser, TOK_RBRACE) && !check(parser, TOK_EOF)) {
				if (!parse_statement(parser))
					synchronize(parser);
			}
			if (!match(parser, TOK_RBRACE)) {
				error(parser, "Expected '}' after if body");
			}
		} else {
			parse_statement(parser); /* braceless: exactly one statement */
		}

		if (match(parser, TOK_ELSE)) {
			if (match(parser, TOK_LBRACE)) {
				while (!check(parser, TOK_RBRACE) && !check(parser, TOK_EOF)) {
					if (!parse_statement(parser))
						synchronize(parser);
				}
				if (!match(parser, TOK_RBRACE)) {
					error(parser, "Expected '}' after else body");
				}
			} else {
				parse_statement(parser); /* braceless else */
			}
		}

		stmt_kind = SN_IF_STMT;
		ok = 1;
		goto cleanup;
	}

	if (match(parser, TOK_FOR)) {
		/* Check for infinite for: for { } */
		if (match(parser, TOK_LBRACE)) {
			/* infinite loop — no header */
		} else if (match(parser, TOK_LPAREN)) {
			/* for (init; cond; incr) { } — all three optional. init/incr are wrapped as
			 * their own statement nodes so the syntax tree header is structured (not flat). */
			if (!check(parser, TOK_SEMI)) {
				int init_cp = syntax_cp(parser);
				SyntaxNodeKind init_kind = SN_EXPR_STMT;
				if (!parse_simple_statement(parser, &init_kind))
					goto cleanup;
				syntax_wrap(parser, init_cp, init_kind);
			}
			if (!match(parser, TOK_SEMI)) {
				error(parser, "Expected ';' in for loop");
				goto cleanup;
			}
			if (!check(parser, TOK_SEMI)) {
				if (!parse_expression(parser))
					goto cleanup;
			}
			if (!match(parser, TOK_SEMI)) {
				error(parser, "Expected ';' in for loop");
				goto cleanup;
			}
			if (!check(parser, TOK_RPAREN)) {
				int incr_cp = syntax_cp(parser);
				SyntaxNodeKind incr_kind = SN_EXPR_STMT;
				if (!parse_simple_statement(parser, &incr_kind))
					goto cleanup;
				syntax_wrap(parser, incr_cp, incr_kind);
			}
			if (!match(parser, TOK_RPAREN)) {
				error(parser, "Expected ')' after for clause");
				goto cleanup;
			}
			if (!match(parser, TOK_LBRACE)) {
				error(parser, "Expected '{'");
				goto cleanup;
			}
		} else {
			/* `for x in <expr>` does not exist: iteration is a `map` (over archetypes) or a
			 * C-style `for (init; cond; incr)`. Reject the range-for form at parse time. */
			error(parser, "for-in is not supported — iterate with a `map` (over an archetype) or a "
			              "C-style `for (init; cond; incr)`");
			goto cleanup;
		}

		while (!check(parser, TOK_RBRACE) && !check(parser, TOK_EOF)) {
			if (!parse_statement(parser))
				synchronize(parser);
		}
		if (!match(parser, TOK_RBRACE)) {
			error(parser, "Expected '}'");
		}

		stmt_kind = SN_FOR_STMT;
		ok = 1;
		goto cleanup;
	}

	/* A binding, assignment, or expression statement — parsed by the shared helper, then
	 * terminated by `;`. */
	ok = parse_simple_statement(parser, &stmt_kind);
	if (ok && !match(parser, TOK_SEMI)) {
		error(parser, "Expected ';' after statement");
	}

cleanup:
	parser->recursion_depth--;
	free(leading);
	if (ok)
		syntax_wrap(parser, stmt_cp, stmt_kind);
	return ok;
}

/* ========== MAIN PARSER ========== */

static void parser_init(Parser *parser, Lexer *lexer) {
	parser->lexer = lexer;
	parser->had_error = 0;
	parser->panic_mode = 0;
	parser->errors = NULL;
	parser->error_count = 0;
	parser->error_cap = 0;
	parser->pending_trivia = NULL;
	parser->pending_count = 0;
	parser->pending_cap = 0;
	parser->recursion_depth = 0;
	memset(&parser->previous, 0, sizeof(Token));
	parser->current.line = 0;
	parser->builder = syntax_builder_new();
	advance(parser);
}

ParseResult parse_program(Parser *parser) {
	int decl_loop_count = 0;
	const int MAX_DECL_LOOP = 10000;

	while (!check(parser, TOK_EOF)) {
		if (++decl_loop_count > MAX_DECL_LOOP) {
			parser->had_error = 1;
			break;
		}
		/* Drain pending trivia (comments/blank lines). The parser builds only the syntax tree now,
		 * where comments live as their own leaves, so there's no AST node to attach it to. */
		Trivia *leading = NULL;
		int leading_count = 0;
		take_pending_as_leading(parser, &leading, &leading_count);
		free(leading);
		(void)leading_count;

		int decl_cp = syntax_cp(parser);
		SyntaxNodeKind decl_kind = SN_ERROR;
		if (!parse_decl(parser, &decl_kind)) {
			synchronize(parser);
			syntax_wrap(parser, decl_cp, SN_ERROR);
			continue;
		}
		syntax_wrap(parser, decl_cp, decl_kind);
	}

	ParseResult result;
	result.errors = parser->errors;
	result.error_count = parser->error_count;
	result.comments = NULL;
	result.comment_count = 0;
	/* Close out the lossless syntax tree and hand ownership to the caller. */
	result.syntax_root = parser->builder ? syntax_builder_finish(parser->builder) : NULL;
	parser->builder = NULL;
	parser->errors = NULL;
	parser->error_count = 0;
	return result;
}

Parser *parser_create(Lexer *lexer) {
	Parser *parser = calloc(1, sizeof(Parser));
	parser_init(parser, lexer);
	return parser;
}

void parser_free(Parser *parser) {
	if (parser) {
		for (size_t i = 0; i < parser->error_count; i++) {
			free(parser->errors[i].message);
		}
		free(parser->errors);
		free(parser->pending_trivia);
		/* Non-NULL only if parse_program never ran to claim the tree. */
		syntax_builder_free(parser->builder);
		free(parser);
	}
}

ParseResult parse_source(const char *src) {
	Lexer lexer;
	lexer_init(&lexer, src);
	Parser *parser = parser_create(&lexer);
	ParseResult result = parse_program(parser);
	parser_free(parser);
	lexer_free(&lexer);
	return result;
}

void parse_result_free(ParseResult *result) {
	if (result) {
		for (size_t i = 0; i < result->error_count; i++) {
			free(result->errors[i].message);
		}
		free(result->errors);
		free(result->comments);
		syntax_node_free(result->syntax_root);
		result->errors = NULL;
		result->error_count = 0;
		result->comments = NULL;
		result->comment_count = 0;
		result->syntax_root = NULL;
	}
}
