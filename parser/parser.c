#include "parser.h"
#include "../cst/syntax_tree.h"
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
	 * drained into the next CST node's leading_trivia at the start of its parse,
	 * and any same-line entries are stolen back as the just-finished node's
	 * trailing_trivia after parse. */
	Trivia *pending_trivia;
	int pending_count;
	int pending_cap;
	int recursion_depth;
	/* Inside a `#foreign` region (banner = rest of file, or a `#foreign { ... }` block). While set,
	 * a bodiless proc parses as a foreign value-form (SN_PROC_EXPR) rather than a proc type. */
	int in_foreign;
	/* Lossless CST builder. Built alongside the AST purely by appending; never
	 * affects parse control flow, so CST bugs can't change compiler output. */
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
	/* CST: emit the token being consumed (the current lookahead) as a leaf, in
	 * source order. Skips the priming/end sentinel (EOF) and error tokens (whose
	 * `start` points at a message literal, not into source). */
	if (parser->builder && parser->current.kind != TOK_EOF && parser->current.kind != TOK_ERROR &&
	    parser->current.start != NULL) {
		uint32_t off = (uint32_t)(parser->current.start - parser->lexer->src);
		cst_builder_token(parser->builder, parser->current.kind, off, (uint32_t)parser->current.length,
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
		/* CST: comments are real leaves too, keeping the tree lossless. */
		if (parser->builder && parser->current.start != NULL) {
			uint32_t coff = (uint32_t)(parser->current.start - parser->lexer->src);
			cst_builder_token(parser->builder, TOK_COMMENT, coff, (uint32_t)parser->current.length,
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

/* Discard accumulated pending trivia. Comments are already emitted as CST leaves
 * in advance(); the pending list only tracked comments/blank-lines for the old
 * AST-node trivia (consumed by the legacy formatter). Draining keeps the list
 * bounded during a parse without affecting the CST. */
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
		case TOK_SYS:
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

/* ===== CST builder helpers (no-ops when no builder is attached) ===== */
static int cst_cp(Parser *parser) {
	return parser->builder ? cst_builder_checkpoint(parser->builder) : 0;
}
static SyntaxNode *cst_wrap(Parser *parser, int checkpoint, SyntaxNodeKind kind) {
	if (parser->builder)
		return cst_builder_wrap(parser->builder, checkpoint, kind);
	return NULL;
}

/* True if everything emitted since `checkpoint` already collapsed to a single
 * node (e.g. a parenthesised expression wrapped itself), so the caller should
 * not wrap it again. */
static int cst_single_node(Parser *parser, int checkpoint) {
	CstBuilder *b = parser->builder;
	return b && b->count == checkpoint + 1 && b->items[checkpoint].tag == SE_NODE;
}

/* Kind of the single CST node emitted since `checkpoint`, or SN_ERROR if the region
 * isn't exactly one node. Lets statement parsing tell a bare-name target (SN_NAME_EXPR)
 * from a field/index/call lvalue without an AST. */
static SyntaxNodeKind cst_single_node_kind(Parser *parser, int checkpoint) {
	CstBuilder *b = parser->builder;
	if (b && b->count == checkpoint + 1 && b->items[checkpoint].tag == SE_NODE)
		return b->items[checkpoint].as.node->kind;
	return SN_ERROR;
}

/* ========== FORWARD DECLARATIONS ========== */

/* The parser builds ONLY the lossless CST: every parse_* function consumes tokens,
 * emits CST node wraps, and returns success (1) / failure (0). It builds no abstract
 * AST — that is reconstructed from the CST by cst_to_program. A few parse decisions
 * still depend on the *form* of a just-parsed sub-construct (a bare-name LHS, a `type`
 * meta-type, a shaped-array element type); those are threaded back through small
 * out-params instead of inspecting a built node. */

/* Form of a parsed type, for the few callers that branch on it. `cst_kind` is the
 * CST wrap kind; `is_type_meta` marks the bare `type` meta-keyword (which shares
 * SN_TYPE_REF with ordinary names but drives const/bind RHS parsing). */
typedef struct {
	SyntaxNodeKind cst_kind;
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

/* Wrapper: every type position becomes a type node in the CST, tagged by the
 * specific form so identifiers within are classified as types and consumers can
 * tell arrays/tuples/handles apart. All call sites go through here. */
static int parse_type(Parser *parser) {
	TypeForm form;
	return parse_type_form(parser, &form);
}

static int parse_type_form(Parser *parser, TypeForm *out) {
	int cp = cst_cp(parser);
	int ok = parse_type_inner(parser, out);
	cst_wrap(parser, cp, out->cst_kind);
	return ok;
}

/* `out->cst_kind` receives the CST type-node kind for the form parsed, from parse
 * context. Pre-set to SN_TYPE_REF; overridden for array/shaped/handle forms. */
static int parse_type_inner(Parser *parser, TypeForm *out) {
	out->cst_kind = SN_TYPE_REF;
	out->is_type_meta = 0;

	/* A proc/func type in annotation position: `h: proc(in)(out)`, `f: func(in) -> T`. The
	 * `type` meta is implied (a callable signature denotes a type). Bodiless — no `{...}` here. */
	if (check(parser, TOK_PROC)) {
		advance(parser);
		out->cst_kind = SN_TYPE_PROC;
		out->is_type_meta = 1;
		return parse_proc_sig(parser, 0);
	}
	if (check(parser, TOK_FUNC)) {
		advance(parser);
		out->cst_kind = SN_TYPE_FUNC;
		out->is_type_meta = 1;
		return parse_func_sig(parser, 0);
	}

	if (!check(parser, TOK_IDENT)) {
		error(parser, "Expected type name");
		return 0;
	}

	int is_handle = (parser->current.length == 6 && strncmp(parser->current.start, "handle", 6) == 0);
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

	/* handle<ArchetypeName> — a generation-checked reference to a row in a table.
	 * Legacy handle(ArchetypeName) is still accepted during migration. */
	if (is_handle && (check(parser, TOK_LT) || check(parser, TOK_LPAREN))) {
		int angle = check(parser, TOK_LT);
		advance(parser); /* consume < or ( */
		if (!check(parser, TOK_IDENT)) {
			error(parser, "Expected archetype name after 'handle<'");
			return 0;
		}
		advance(parser);
		if (angle) {
			if (!match(parser, TOK_GT)) {
				error(parser, "Expected '>' after archetype name in handle type");
				return 0;
			}
		} else if (!match(parser, TOK_RPAREN)) {
			error(parser, "Expected ')' after archetype name in handle type");
			return 0;
		}
		out->cst_kind = SN_TYPE_HANDLE;
		return 1;
	}

	/* `archetype` / `opaque` bare-category names parse like an ordinary type name (the
	 * CST records the keyword token; semantic interprets it). */

	if (check(parser, TOK_LBRACKET)) {
		advance(parser); /* consume [ */
		if (check(parser, TOK_RBRACKET)) {
			/* float[] → TYPE_ARRAY */
			advance(parser);
			out->cst_kind = SN_TYPE_ARRAY;
			return 1;
		}
		if (!check(parser, TOK_NUMBER)) {
			error(parser, "Expected ']' or integer size after '['");
			while (!check(parser, TOK_RBRACKET) && !check(parser, TOK_EOF)) {
				advance(parser);
			}
			if (check(parser, TOK_RBRACKET)) {
				advance(parser);
			}
			return 1;
		}
		advance(parser); /* size */
		if (!match(parser, TOK_RBRACKET)) {
			error(parser, "Expected ']' after array size");
			return 1;
		}
		out->cst_kind = SN_TYPE_SHAPED_ARRAY;
		/* chain: float[5][5] */
		while (check(parser, TOK_LBRACKET)) {
			advance(parser);
			if (!check(parser, TOK_NUMBER)) {
				error(parser, "Expected integer size after '['");
				while (!check(parser, TOK_RBRACKET) && !check(parser, TOK_EOF)) {
					advance(parser);
				}
				if (check(parser, TOK_RBRACKET)) {
					advance(parser);
				}
				return 1;
			}
			advance(parser); /* size */
			if (!match(parser, TOK_RBRACKET)) {
				error(parser, "Expected ']' after array size");
				return 1;
			}
		}
		return 1;
	}

	return 1;
}

/* Parse a tuple name-group `(a, b, …) :: T` after the leading name was consumed. The
 * suffixes are part of the name (minting `name_a`, `name_b`, …), `T` is the shared type. */
static int parse_tuple_name_group(Parser *parser) {
	advance(parser); /* consume '(' */
	while (!check(parser, TOK_RPAREN) && !check(parser, TOK_EOF)) {
		if (!check(parser, TOK_IDENT)) {
			error(parser, "Expected a name in tuple group `(a, b, …)`");
			break;
		}
		advance(parser);
		if (!match(parser, TOK_COMMA))
			break;
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

/* Parse one archetype field/component, wrapping its CST. Returns 1 on success,
 * 0 on a malformed component (so the body loop stops). */
static int parse_arch_field(Parser *parser) {
	if (!check(parser, TOK_IDENT)) {
		error(parser, "Expected field name");
		return 0;
	}
	int field_decl_name_cp = cst_cp(parser);
	advance(parser);
	cst_wrap(parser, field_decl_name_cp, SN_FIELD_NAME);

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

		int param_cp = cst_cp(parser);

		match(parser, TOK_OWN); /* optional `own` qualifier (CST records the token) */

		if (!check(parser, TOK_IDENT)) {
			error(parser, "Expected parameter name");
			return 0;
		}
		int param_name_cp = cst_cp(parser);
		advance(parser);
		cst_wrap(parser, param_name_cp, SN_PARAM_NAME);

		if (!match(parser, TOK_COLON)) {
			error(parser, "Expected ':' after parameter name");
			return 0;
		}

		if (!parse_type(parser))
			return 0;

		cst_wrap(parser, param_cp, SN_PARAM);
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
				int out_param_cp = cst_cp(parser);

				if (check(parser, TOK_OWN)) {
					error(parser, "an out-parameter is owned by definition — drop `own`");
					return 0;
				}

				if (!check(parser, TOK_IDENT)) {
					error(parser, "Expected out-parameter name");
					return 0;
				}
				int out_param_name_cp = cst_cp(parser);
				advance(parser);
				cst_wrap(parser, out_param_name_cp, SN_PARAM_NAME);

				if (!match(parser, TOK_COLON)) {
					error(parser, "Expected ':' after out-parameter name");
					return 0;
				}

				if (!parse_type(parser))
					return 0;

				cst_wrap(parser, out_param_cp, SN_OUT_PARAM);
			} while (match(parser, TOK_COMMA) && !check(parser, TOK_RPAREN));
		}
		if (!match(parser, TOK_RPAREN)) {
			error(parser, "Expected ')' after out-parameters");
			return 0;
		}
	}
	return 1;
}

/* A func's single return type: `-> T`. Optional only for a bare extern (absent ⇒ void);
 * mandatory for an ordinary func. */
static int parse_func_return(Parser *parser, int is_extern) {
	if (match(parser, TOK_ARROW)) {
		if (check(parser, TOK_LPAREN)) {
			error(parser, "a func has exactly one return type — use a `proc` with an out-parameter list "
			              "`(out)` for multiple results or an in-place fill");
			return 0;
		}
		if (!parse_type(parser))
			return 0;
	} else if (!is_extern) {
		error(parser, "Expected '->'");
		return 0;
	}
	return 1;
}

/* sys parameters are bare component names (no `: T`) between an already-consumed '(' and the
 * closing ')' (matched by the caller). */
static int parse_sys_param_list_body(Parser *parser) {
	if (check(parser, TOK_RPAREN))
		return 1;
	do {
		int param_cp = cst_cp(parser);
		if (!check(parser, TOK_IDENT)) {
			error(parser, "Expected parameter name");
			return 0;
		}

		int param_name_cp = cst_cp(parser);
		advance(parser);
		cst_wrap(parser, param_name_cp, SN_PARAM_NAME);

		cst_wrap(parser, param_cp, SN_PARAM);
	} while (match(parser, TOK_COMMA) && !check(parser, TOK_RPAREN));
	return 1;
}

/* A `{ statement* }` body. Identical across proc / sys / func. */
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

static int parse_static_decl(Parser *parser, SyntaxNodeKind *out_kind) {
	if (!check(parser, TOK_IDENT))
		return 0;
	advance(parser); /* the binding name — a bare leading IDENT for every top-level form */

	/* Pool allocation: `Name[C](N){V}` — capacity in `[]`, optional initial live-count in
	 * `()`, optional field-init block `{}`. Top-level position implies static storage; the
	 * name references the archetype shape whose singleton pool to allocate. */
	if (check(parser, TOK_LBRACKET)) {
		*out_kind = SN_STATIC_DECL;
		advance(parser); /* '[' */
		if (!parse_expression(parser))
			return 0;
		if (!match(parser, TOK_RBRACKET)) {
			error(parser, "Expected ']' after pool capacity in `Name[C]`");
			return 0;
		}
		if (match(parser, TOK_LPAREN)) {
			if (!parse_expression(parser))
				return 0;
			if (!match(parser, TOK_RPAREN))
				error(parser, "Expected ')' after pool live-count in `Name[C](N)`");
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
		match(parser, TOK_SEMI); /* recorded; formatter drops it for static decls */
		return 1;
	}

	/* Tuple group: `pos (x, y) :: float` — the parenthesized suffixes mint flat names
	 * (`pos_x`, `pos_y`); the shared type follows `::`. */
	if (check(parser, TOK_LPAREN)) {
		*out_kind = SN_CONST_DECL;
		if (!parse_tuple_name_group(parser))
			return 0;
		if (check(parser, TOK_SEMI))
			advance(parser);
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
			match(parser, TOK_SEMI); /* recorded; formatter drops it for static decls */
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
			match(parser, TOK_SEMI);
			return 1;
		}

		/* `name : T` with no second separator → mutable static storage, zero-initialized (the
		 * implicit `= 0` of the unified form). T may be a sized array (a buffer) or a scalar. */
		if (have_type && !check(parser, TOK_COLON)) {
			*out_kind = SN_STATIC_DECL;
			match(parser, TOK_SEMI); /* recorded; formatter drops it for static decls */
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
			if (!parse_expression(parser))
				return 0;
		}
		if (check(parser, TOK_SEMI))
			advance(parser);
		return 1;
	}

	return 0;
}

/* `out_kind` receives the CST node kind for the declaration form, from parse
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
			int child_cp = cst_cp(parser);
			SyntaxNodeKind child_kind = SN_ERROR;
			if (!parse_decl(parser, &child_kind)) {
				synchronize(parser);
				cst_wrap(parser, child_cp, SN_ERROR);
				continue;
			}
			cst_wrap(parser, child_cp, child_kind);
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

static int parse_decl(Parser *parser, SyntaxNodeKind *out_kind) {
	*out_kind = SN_ERROR;

	/* Declaration-site decorators. Two recognized forms:
	 *
	 *   @allow_pure_proc           — legacy, proc-only: suppresses proc-could-be-func.
	 *   @allow(<diagnostic-slug>)  — general lint suppression, any decl kind.
	 *
	 * Multiple decorators may be stacked. Tokens are stored in the CST at decl
	 * level; `cst_to_program` reads them — `cv_has_token(d, TOK_AT)` for the
	 * legacy flag and a slug scan for `@allow(...)` entries. */
	while (parser->current.kind == TOK_AT) {
		advance(parser); /* consume '@' */
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
		error(parser, "Unknown decorator (recognized: @allow_pure_proc, @allow(<slug>), @drop(<type>), @intrinsic)");
		return 0;
	}

	switch (parser->current.kind) {
	/* Unified grammar: declarations are bindings `name :: <form>`. proc/func/sys/arche (and
	 * `extern proc`) are RHS value/type forms (see parse_primary_expr / parse_type_inner), reached
	 * via the IDENT-led default case below. There are no keyword-led top-level declarations. */
	case TOK_USE: {
		/* `#import` is a region in the `#module`/`#file`/`#foreign` family. Two forms:
		 *   bare   `#import io`            one module (trailing `;` optional)
		 *   block  `#import { io net … }`  a group of modules (whitespace/`,`/`;` separated)
		 * Both produce an SN_USE_DECL carrying one IDENT per module; consumers iterate them. */
		advance(parser); /* consume '#import' */
		if (check(parser, TOK_LBRACE)) {
			advance(parser); /* consume '{' */
			if (check(parser, TOK_RBRACE)) {
				error(parser, "empty `#import { }` — list one or more module names");
				return 0;
			}
			while (!check(parser, TOK_RBRACE) && !check(parser, TOK_EOF)) {
				if (!check(parser, TOK_IDENT)) {
					error(parser, "expected a module name in `#import { ... }` (names are whitespace-separated)");
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
		while (check(parser, TOK_IDENT)) {
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
	default:
		/* Every top-level declaration is an IDENT-led binding (const, pool alloc, or static
		 * buffer) — see parse_static_decl. */
		if (check(parser, TOK_IDENT)) {
			if (parse_static_decl(parser, out_kind))
				return 1;
		}
		error(parser, "Expected declaration");
		return 0;
	}
}

/* ========== EXPRESSION PARSING ========== */

/* After a postfix `[` is matched, parse either an index list `a, b, …` or a slice `lo:hi` (lo/hi
 * each optional: `[:hi]`, `[lo:]`, `[:]`). Sets *out_slice=1 for the slice form. Consumes the `]`.
 * A `:` inside the brackets is what marks a slice. */
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
		} else { /* index: optional comma list */
			while (match(parser, TOK_COMMA) && !check(parser, TOK_RBRACKET))
				if (!parse_expression(parser))
					return 0;
		}
	}
	if (!match(parser, TOK_RBRACKET)) {
		error(parser, "Expected ']'");
		return 0;
	}
	return 1;
}

/* `out_kind` receives the SyntaxNodeKind for the primary expression form parsed,
 * derived from parse context (not from a built AST node). The caller wraps the
 * CST node with it. Left untouched when the primary already wrapped itself (paren). */
static int parse_primary_expr(Parser *parser, SyntaxNodeKind *out_kind) {
	int prim_start = cst_cp(parser);
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

	/* Unified-grammar RHS forms — proc/func/sys/archetype as value literals or type forms.
	 * The name is the binding LHS, so none of these consume a name. Top-level keyword-led decls
	 * are still handled by parse_decl; these fire only in expression/RHS position. */
	if (check(parser, TOK_PROC) || check(parser, TOK_FUNC)) {
		int is_proc = check(parser, TOK_PROC);
		advance(parser); /* consume 'proc' / 'func' */
		if (check(parser, TOK_LBRACE))
			return parse_group_form(parser, out_kind);
		/* Inside a `#foreign` region a bodiless proc is a foreign (FFI) value-form, not a proc
		 * type — drive that off the parser's region state. Funcs are never foreign (FFI bodies
		 * are procs), so a bodiless func is always a func type. */
		return is_proc ? parse_proc_form(parser, parser->in_foreign, out_kind) : parse_func_form(parser, out_kind);
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
	if (check(parser, TOK_SYS)) {
		advance(parser); /* consume 'sys' */
		*out_kind = SN_SYS_EXPR;
		if (!match(parser, TOK_LPAREN)) {
			error(parser, "Expected '(' after 'sys'");
			return 0;
		}
		if (!parse_sys_param_list_body(parser))
			return 0;
		if (!match(parser, TOK_RPAREN)) {
			error(parser, "Expected ')'");
			return 0;
		}
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
				int v_cp = cst_cp(parser);
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
				cst_wrap(parser, v_cp, SN_ENUM_VARIANT);
			} while (match(parser, TOK_COMMA) && !check(parser, TOK_RBRACE));
		}
		if (!match(parser, TOK_RBRACE)) {
			error(parser, "Expected '}' to close enum");
			return 0;
		}
		return 1;
	}

	if (check(parser, TOK_IDENT)) {
		int prim_name_cp = cst_cp(parser);
		int is_table = cur_ident_is(parser, "table", 5);
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

		/* field access (and optional trailing index): `a.b.c[i]` */
		if (match(parser, TOK_DOT)) {
			if (!check(parser, TOK_IDENT)) {
				error(parser, "Expected field name after '.'");
				return 0;
			}
			int field_name_cp = cst_cp(parser);
			advance(parser);
			cst_wrap(parser, field_name_cp, SN_FIELD_NAME);

			while (match(parser, TOK_DOT)) {
				if (!check(parser, TOK_IDENT)) {
					error(parser, "Expected field name after '.'");
					return 0;
				}
				int chained_field_cp = cst_cp(parser);
				advance(parser);
				cst_wrap(parser, chained_field_cp, SN_FIELD_NAME);
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

		/* indexing `a[i]` or sub-slice `a[lo:hi]` */
		if (match(parser, TOK_LBRACKET)) {
			int is_slice;
			if (!parse_bracket_index_or_slice(parser, &is_slice))
				return 0;
			*out_kind = is_slice ? SN_SLICE_EXPR : SN_INDEX_EXPR;
			return 1;
		}

		/* function call: `f(args)` — wrap the callee name, then consume '(' as a sibling. */
		if (check(parser, TOK_LPAREN)) {
			cst_wrap(parser, prim_name_cp, SN_CALLEE_NAME);
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

		*out_kind = SN_NAME_EXPR;
		return 1;
	}

	if (match(parser, TOK_LPAREN)) {
		parse_expression(parser);
		match(parser, TOK_COMMA); /* tolerate a trailing comma — trailing commas are valid in every list */
		if (!match(parser, TOK_RPAREN)) {
			error(parser, "Expected ')' after expression");
		}
		cst_wrap(parser, prim_start, SN_PAREN_EXPR);
		return 1;
	}

	error(parser, "Expected expression");
	return 0;
}

/* Prefix unary operators: `-x` (negate) and `!x` (logical not). Binds tighter
 * than binary operators, looser than postfix (calls/indexing in primary). */
static int parse_unary_expr(Parser *parser) {
	int u_cp = cst_cp(parser);
	/* `move <expr>` / `copy <expr>` — call-site ownership markers; transparent to the
	 * grammar (the CST records the keyword token, read back by cst_to_program). */
	if (check(parser, TOK_MOVE) || check(parser, TOK_COPY)) {
		advance(parser);
		if (!parse_unary_expr(parser))
			return 0;
		cst_wrap(parser, u_cp, SN_UNARY_EXPR);
		return 1;
	}
	if (check(parser, TOK_MINUS) || check(parser, TOK_BANG)) {
		advance(parser);
		if (!parse_unary_expr(parser)) /* allow -(-x), !!x */
			return 0;
		cst_wrap(parser, u_cp, SN_UNARY_EXPR);
		return 1;
	}
	/* Primary expression: wrap it by the kind it parsed into (tracked by parse
	 * context in `prim_kind`), unless it already collapsed to a single node (a
	 * parenthesised expr wraps itself). */
	int p_cp = cst_cp(parser);
	SyntaxNodeKind prim_kind = SN_NAME_EXPR;
	if (!parse_primary_expr(parser, &prim_kind))
		return 0;
	if (!cst_single_node(parser, p_cp))
		cst_wrap(parser, p_cp, prim_kind);
	return 1;
}

/* Binary-operator precedence: higher binds tighter, -1 if not a binary op.
   All operators are left-associative.
     3: * /        2: + -        1: < > <= >= == != */
static int binop_prec(TokenKind k) {
	switch (k) {
	case TOK_STAR:
	case TOK_SLASH:
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
	default:
		return -1;
	}
}

/* Precedence climbing: extend `left` with binary operators whose precedence is
   >= min_prec, so e.g. `2 + 3 * 2` builds (2 + (3 * 2)) rather than a flat fold. */
/* `left_cp` is the CST checkpoint taken before `left` was parsed, so each fold can
 * retroactively wrap [left_cp .. end-of-right] into a SN_BINARY_EXPR — left-assoc
 * nesting falls out because the previous fold collapses to one node at left_cp. */
static int parse_binary_rhs(Parser *parser, int ok_left, int left_cp, int min_prec) {
	if (!ok_left)
		return 0;

	for (;;) {
		int prec = binop_prec(parser->current.kind);
		if (prec < min_prec)
			return 1;

		advance(parser); /* consume the operator token */

		int right_cp = cst_cp(parser);
		if (!parse_unary_expr(parser))
			return 0;

		/* Left-associative: only fold a strictly tighter operator into `right`. */
		while (binop_prec(parser->current.kind) > prec) {
			if (!parse_binary_rhs(parser, 1, right_cp, prec + 1))
				return 0;
		}

		/* Retroactively wrap [left_cp .. end-of-right] into a binary node; the next
		 * fold reuses left_cp so left-assoc nesting falls out. */
		cst_wrap(parser, left_cp, SN_BINARY_EXPR);
	}
}

static int parse_binary_expr(Parser *parser) {
	int left_cp = cst_cp(parser);
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
	 * already consumed by the caller and is recorded in the CST. */
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
	 * from the CST tokens; here we only consume + drive the CST. */
	if (match(parser, TOK_COLON)) {
		if (check(parser, TOK_EQ)) {
			advance(parser); /* `:=` — inferred variable */
			if (!parse_expression(parser))
				return 0;
		} else if (check(parser, TOK_COLON)) {
			advance(parser); /* `::` — inferred-meta local constant */
			if (!parse_expression(parser))
				return 0;
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
	int target_cp = cst_cp(parser);
	if (!parse_expression(parser))
		return 0;

	/* Bare binding: a plain name (the target collapsed to a single SN_NAME_EXPR) then `:`
	 * (`x := e` / `x: T [= e]`) or `,` (`a, b := e`). The name token is already in the CST. */
	if (cst_single_node_kind(parser, target_cp) == SN_NAME_EXPR &&
	    (check(parser, TOK_COLON) || check(parser, TOK_COMMA))) {
		return parse_binding_tail(parser, out_kind);
	}

	/* Proc-call statement: `foo(in)(out)`. The call `foo(in)` was parsed above as a CALL_EXPR; a
	 * following `(` opens the out-argument list — caller-provided places written in place. Each
	 * out-arg is `name` (existing place), `name:` (declare, type from the out-param), or `name: T`
	 * (declare, typed). A proc is an action, so this is a statement, never a value. */
	if (cst_single_node_kind(parser, target_cp) == SN_CALL_EXPR && check(parser, TOK_LPAREN)) {
		advance(parser); /* consume out-list `(` */
		if (!check(parser, TOK_RPAREN)) {
			do {
				int out_arg_cp = cst_cp(parser);
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
				cst_wrap(parser, out_arg_cp, SN_OUT_ARG);
			} while (match(parser, TOK_COMMA) && !check(parser, TOK_RPAREN));
		}
		if (!match(parser, TOK_RPAREN)) {
			error(parser, "Expected ')' after out-arguments");
			return 0;
		}
		*out_kind = SN_PROC_CALL_STMT;
		return 1;
	}

	/* Assignment: `lvalue = / += / -= / *= / /= expr`. */
	if (check(parser, TOK_EQ) || check(parser, TOK_PLUS_EQ) || check(parser, TOK_MINUS_EQ) ||
	    check(parser, TOK_STAR_EQ) || check(parser, TOK_SLASH_EQ)) {
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
	int stmt_cp = cst_cp(parser);
	int ok = 0; /* 1 once a statement has been parsed (drives the cleanup wrap) */
	/* CST wrap kind for the statement, tracked by parse context (not a built AST node).
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
	 * cleanup. Comments survive in the CST as their own leaves regardless. */
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
		/* `return;` — naked early exit (valid in a proc/sys); or `return e1, …, en` — a list of
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

	/* check for run statement */
	if (check(parser, TOK_RUN)) {
		advance(parser); /* consume 'run' */

		if (!check(parser, TOK_IDENT)) {
			error(parser, "Expected system name");
			parser->recursion_depth--;
			goto cleanup;
		}
		advance(parser);

		if (!match(parser, TOK_SEMI)) {
			error(parser, "Expected ';'");
		}

		stmt_kind = SN_RUN_STMT;
		ok = 1;
		goto cleanup;
	}

	if (check(parser, TOK_MATCH)) {
		advance(parser);               /* consume 'match' */
		if (!parse_expression(parser)) /* scrutinee */
			goto cleanup;
		if (!match(parser, TOK_LBRACE)) {
			error(parser, "Expected '{' after match scrutinee");
			goto cleanup;
		}
		while (!check(parser, TOK_RBRACE) && !check(parser, TOK_EOF)) {
			int arm_cp = cst_cp(parser);
			/* pattern: an enum variant / `_` ident, or an int/string/char literal */
			if (check(parser, TOK_IDENT) || check(parser, TOK_NUMBER) || check(parser, TOK_STRING) ||
			    check(parser, TOK_CHAR_LIT)) {
				advance(parser);
			} else {
				error(parser, "Expected match pattern (variant, literal, or '_')");
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
			cst_wrap(parser, arm_cp, SN_MATCH_ARM);
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
			 * their own statement nodes so the CST header is structured (not flat). */
			if (!check(parser, TOK_SEMI)) {
				int init_cp = cst_cp(parser);
				SyntaxNodeKind init_kind = SN_EXPR_STMT;
				if (!parse_simple_statement(parser, &init_kind))
					goto cleanup;
				cst_wrap(parser, init_cp, init_kind);
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
				int incr_cp = cst_cp(parser);
				SyntaxNodeKind incr_kind = SN_EXPR_STMT;
				if (!parse_simple_statement(parser, &incr_kind))
					goto cleanup;
				cst_wrap(parser, incr_cp, incr_kind);
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
			/* `for x in <expr>` does not exist: iteration is a `sys` (over archetypes) or a
			 * C-style `for (init; cond; incr)`. Reject the range-for form at parse time. */
			error(parser, "for-in is not supported — iterate with a `sys` (over an archetype) or a "
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
		cst_wrap(parser, stmt_cp, stmt_kind);
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
	parser->builder = cst_builder_new();
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
		/* Drain pending trivia (comments/blank lines). The parser builds only the CST now,
		 * where comments live as their own leaves, so there's no AST node to attach it to. */
		Trivia *leading = NULL;
		int leading_count = 0;
		take_pending_as_leading(parser, &leading, &leading_count);
		free(leading);
		(void)leading_count;

		int decl_cp = cst_cp(parser);
		SyntaxNodeKind decl_kind = SN_ERROR;
		if (!parse_decl(parser, &decl_kind)) {
			synchronize(parser);
			cst_wrap(parser, decl_cp, SN_ERROR);
			continue;
		}
		cst_wrap(parser, decl_cp, decl_kind);
	}

	ParseResult result;
	result.ast = NULL; /* the parser produces only the lossless CST; cst_to_program builds the AST */
	result.errors = parser->errors;
	result.error_count = parser->error_count;
	result.comments = NULL;
	result.comment_count = 0;
	/* Close out the lossless CST and hand ownership to the caller. */
	result.cst_root = parser->builder ? cst_builder_finish(parser->builder) : NULL;
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
		cst_builder_free(parser->builder);
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
		syntax_node_free(result->cst_root);
		result->errors = NULL;
		result->error_count = 0;
		result->comments = NULL;
		result->comment_count = 0;
		result->cst_root = NULL;
	}
}
