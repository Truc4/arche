#include "parser.h"
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

/* If pending_trivia[0] is a comment on or before `line`, steal entries up to
 * and including any same-line trailing comment as trailing trivia for the
 * just-finished construct. */
static void steal_trailing_inline(Parser *parser, int line, Trivia **out_trivia, int *out_count) {
	*out_trivia = NULL;
	*out_count = 0;
	if (parser->pending_count == 0)
		return;
	int steal = 0;
	for (int i = 0; i < parser->pending_count; i++) {
		Trivia *t = &parser->pending_trivia[i];
		if (t->kind == TRIVIA_LINE_COMMENT && t->line == line) {
			steal = i + 1;
		} else {
			break;
		}
	}
	if (steal == 0)
		return;
	*out_trivia = malloc(steal * sizeof(Trivia));
	memcpy(*out_trivia, parser->pending_trivia, steal * sizeof(Trivia));
	*out_count = steal;
	int remaining = parser->pending_count - steal;
	if (remaining > 0) {
		memmove(parser->pending_trivia, parser->pending_trivia + steal, remaining * sizeof(Trivia));
	}
	parser->pending_count = remaining;
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

static char *token_text(Token tok) {
	char *text = malloc(tok.length + 1);
	strncpy(text, tok.start, tok.length);
	text[tok.length] = '\0';
	return text;
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

/* ========== FORWARD DECLARATIONS ========== */

static Decl *parse_decl(Parser *parser);
static Statement *parse_statement(Parser *parser);
static Expression *parse_expression(Parser *parser);
static TypeRef *parse_type(Parser *parser);

/* ========== TYPE PARSING ========== */

static TypeRef *parse_type(Parser *parser) {
	if (!check(parser, TOK_IDENT)) {
		error(parser, "Expected type name");
		return NULL;
	}

	char *name = token_text(parser->current);
	advance(parser);

	TypeRef *type = NULL;
	SourceLoc start_loc;
	start_loc.line = parser->previous.line;
	start_loc.column = parser->previous.column;

	/* Bare-category `archetype` parameter type. Only legal where parse_type is
	 * invoked from a parameter slot; semantic validates the context. */
	if (strcmp(name, "archetype") == 0) {
		free(name);
		type = malloc(sizeof(TypeRef));
		type->kind = TYPE_ARCHETYPE;
		type->loc = start_loc;
		return type;
	}

	/* `opaque`: a pointer-width, C-owned cell. A foreign-resource type is a nominal
	 * alias over it (`file :: opaque`); distinctness comes from the alias name. */
	if (strcmp(name, "opaque") == 0) {
		free(name);
		type = malloc(sizeof(TypeRef));
		type->kind = TYPE_OPAQUE;
		type->loc = start_loc;
		return type;
	}

	/* handle<ArchetypeName> — a generation-checked reference to a row in a table.
	 * Legacy handle(ArchetypeName) is still accepted during migration. */
	if (strcmp(name, "handle") == 0 && (check(parser, TOK_LT) || check(parser, TOK_LPAREN))) {
		int angle = check(parser, TOK_LT);
		advance(parser); /* consume < or ( */
		if (!check(parser, TOK_IDENT)) {
			error(parser, "Expected archetype name after 'handle<'");
			return NULL;
		}
		char *arch_name = token_text(parser->current);
		advance(parser);
		if (angle) {
			if (!match(parser, TOK_GT)) {
				error(parser, "Expected '>' after archetype name in handle type");
				return NULL;
			}
		} else if (!match(parser, TOK_RPAREN)) {
			error(parser, "Expected ')' after archetype name in handle type");
			return NULL;
		}
		type = malloc(sizeof(TypeRef));
		type->kind = TYPE_HANDLE;
		type->data.handle.archetype_name = arch_name;
		type->loc = start_loc;
		return type;
	}

	type = type_name_create(name);
	type->loc = start_loc;

	if (check(parser, TOK_LBRACKET)) {
		advance(parser); /* consume [ */
		if (check(parser, TOK_RBRACKET)) {
			/* float[] → TYPE_ARRAY */
			advance(parser);
			TypeRef *arr = type_array_create(type);
			arr->loc = type->loc;
			return arr;
		}
		if (!check(parser, TOK_NUMBER)) {
			error(parser, "Expected ']' or integer size after '['");
			while (!check(parser, TOK_RBRACKET) && !check(parser, TOK_EOF)) {
				advance(parser);
			}
			if (check(parser, TOK_RBRACKET)) {
				advance(parser);
			}
			return type;
		}
		int rank = atoi(token_text(parser->current));
		advance(parser);
		if (!match(parser, TOK_RBRACKET)) {
			error(parser, "Expected ']' after array size");
			return type;
		}
		TypeRef *shaped = type_shaped_array_create(type, rank);
		shaped->loc = type->loc;
		type = shaped;
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
				return type;
			}
			int r = atoi(token_text(parser->current));
			advance(parser);
			if (!match(parser, TOK_RBRACKET)) {
				error(parser, "Expected ']' after array size");
				return type;
			}
			TypeRef *outer = type_shaped_array_create(type, r);
			outer->loc = shaped->loc;
			type = outer;
		}
		return type;
	}

	return type;
}

/* ========== ARCHETYPE PARSING ========== */

static FieldDecl **parse_arch_field_expanded(Parser *parser, int *out_count) {
	/* All fields are columns (no metadata) */
	FieldKind kind = FIELD_COLUMN;

	if (!check(parser, TOK_IDENT)) {
		error(parser, "Expected field name");
		*out_count = 0;
		return NULL;
	}
	char *name = token_text(parser->current);
	char *name_copy = malloc(strlen(name) + 1);
	strcpy(name_copy, name);
	advance(parser);

	/* A component is a type. A bare `a` references the type `a` (`arche Foo { a, b }` is a
	 * set of nominal component types). `name :: type` defines one inline (mints the nominal
	 * type `name` and includes it). The old single-colon accessor `name: type` is rejected —
	 * `foo : bar` names nothing. */
	if (!check(parser, TOK_COLON)) {
		char *type_name_dup = malloc(strlen(name_copy) + 1);
		strcpy(type_name_dup, name_copy);
		TypeRef *t = type_name_create(type_name_dup);
		t->loc.line = parser->previous.line;
		t->loc.column = parser->previous.column;
		FieldDecl *field = field_decl_create(kind, name_copy, t);
		field->loc.line = parser->previous.line;
		field->loc.column = parser->previous.column;
		match(parser, TOK_COMMA); /* optional trailing comma */
		FieldDecl **result = malloc(sizeof(FieldDecl *));
		result[0] = field;
		*out_count = 1;
		return result;
	}
	advance(parser); /* consume first ':' */
	if (!check(parser, TOK_COLON)) {
		error(parser, "archetype components are types: write `name :: type` to define a component, "
		              "or a bare type name to reference one (`name: type` is not valid)");
		free(name_copy);
		*out_count = 0;
		return NULL;
	}
	advance(parser); /* consume second ':' — this was `::`, an inline component definition */

	/* Check for tuple syntax: (x: float, y: float) */
	if (check(parser, TOK_LPAREN)) {
		advance(parser); /* consume ( */
		char **tuple_field_names = NULL;
		TypeRef **tuple_field_types = NULL;
		int tuple_field_count = 0;

		while (!check(parser, TOK_RPAREN) && !check(parser, TOK_EOF)) {
			if (!check(parser, TOK_IDENT)) {
				error(parser, "Expected field name in tuple");
				free(name_copy);
				*out_count = 0;
				return NULL;
			}
			char *field_name = token_text(parser->current);
			char *field_name_copy = malloc(strlen(field_name) + 1);
			strcpy(field_name_copy, field_name);
			advance(parser);

			if (!match(parser, TOK_COLON)) {
				error(parser, "Expected ':' after tuple field name");
				free(name_copy);
				free(field_name_copy);
				*out_count = 0;
				return NULL;
			}

			TypeRef *field_type = parse_type(parser);
			if (!field_type) {
				free(name_copy);
				free(field_name_copy);
				*out_count = 0;
				return NULL;
			}

			/* Collect tuple field info */
			tuple_field_names = realloc(tuple_field_names, (tuple_field_count + 1) * sizeof(char *));
			tuple_field_types = realloc(tuple_field_types, (tuple_field_count + 1) * sizeof(TypeRef *));
			tuple_field_names[tuple_field_count] = field_name_copy;
			tuple_field_types[tuple_field_count] = field_type;
			tuple_field_count++;

			if (!match(parser, TOK_COMMA))
				break;
		}

		if (!match(parser, TOK_RPAREN)) {
			error(parser, "Expected ')' to close tuple type");
			free(name_copy);
			*out_count = 0;
			return NULL;
		}

		/* Create TYPE_TUPLE */
		TypeRef *tuple_type = malloc(sizeof(TypeRef));
		tuple_type->kind = TYPE_TUPLE;
		tuple_type->loc.line = parser->previous.line;
		tuple_type->loc.column = parser->previous.column;
		tuple_type->data.tuple.field_names = tuple_field_names;
		tuple_type->data.tuple.field_types = tuple_field_types;
		tuple_type->data.tuple.field_count = tuple_field_count;

		/* Create single field with tuple type */
		FieldDecl *field = field_decl_create(kind, name_copy, tuple_type);
		field->loc.line = parser->previous.line;
		field->loc.column = parser->previous.column;

		/* trailing comma is optional */
		match(parser, TOK_COMMA);

		FieldDecl **result = malloc(sizeof(FieldDecl *));
		result[0] = field;
		*out_count = 1;
		return result;
	}

	/* Regular (non-tuple) field */
	TypeRef *type = parse_type(parser);
	if (!type) {
		free(name_copy);
		*out_count = 0;
		return NULL;
	}

	/* trailing comma is optional */
	match(parser, TOK_COMMA);

	FieldDecl *field = field_decl_create(kind, name_copy, type);
	field->loc.line = parser->previous.line;
	field->loc.column = parser->previous.column;

	FieldDecl **result = malloc(sizeof(FieldDecl *));
	result[0] = field;
	*out_count = 1;
	return result;
}

static Decl *parse_archetype_decl(Parser *parser) {
	if (!match(parser, TOK_ARCHETYPE)) {
		error(parser, "Expected 'arche'");
		return NULL;
	}

	if (!check(parser, TOK_IDENT)) {
		error(parser, "Expected archetype name");
		return NULL;
	}
	char *name = token_text(parser->current);
	advance(parser);

	if (!match(parser, TOK_LBRACE)) {
		error(parser, "Expected '{'");
		return NULL;
	}

	ArchetypeDecl *arch = archetype_decl_create(name);
	arch->loc.line = parser->previous.line;
	arch->loc.column = parser->previous.column;

	while (!check(parser, TOK_RBRACE) && !check(parser, TOK_EOF)) {
		/* Pending trivia between fields → leading trivia of the next field group. */
		Trivia *fleading = NULL;
		int fleading_count = 0;
		take_pending_as_leading(parser, &fleading, &fleading_count);

		int field_count = 0;
		FieldDecl **fields = parse_arch_field_expanded(parser, &field_count);
		if (!fields || field_count == 0) {
			/* Malformed component. Stop the body loop — global `synchronize` can skip past
			 * the closing `}` to the next decl keyword and park there without advancing,
			 * which would spin this loop (allocating trivia each pass → RAM blowup). The
			 * missing `}` is reported below. */
			free(fleading);
			break;
		}

		/* Attach leading trivia to the FIRST field and trailing-inline to the
		 * LAST field of the expanded group (typically the same field, but tuple
		 * fields expand to many). */
		fields[0]->leading_trivia = fleading;
		fields[0]->leading_count = fleading_count;
		int last_field_line = parser->previous.line;
		steal_trailing_inline(parser, last_field_line, &fields[field_count - 1]->trailing_trivia,
		                      &fields[field_count - 1]->trailing_count);

		/* grow the fields array and add all expanded fields */
		for (int i = 0; i < field_count; i++) {
			arch->fields = realloc(arch->fields, (arch->field_count + 1) * sizeof(FieldDecl *));
			arch->fields[arch->field_count++] = fields[i];
		}
		free(fields);
	}

	if (!match(parser, TOK_RBRACE)) {
		error(parser, "Expected '}'");
	}

	Decl *decl = decl_create(DECL_ARCHETYPE);
	decl->data.archetype = arch;
	return decl;
}

/* ========== PROCEDURE PARSING ========== */

static Decl *parse_proc_decl(Parser *parser) {
	int is_extern = 0;

	if (parser->previous.kind == TOK_EXTERN || match(parser, TOK_EXTERN)) {
		is_extern = 1;
	}

	if (!match(parser, TOK_PROC)) {
		error(parser, "Expected 'proc'");
		return NULL;
	}

	if (!check(parser, TOK_IDENT)) {
		error(parser, "Expected procedure name");
		return NULL;
	}
	char *name = token_text(parser->current);
	advance(parser);

	if (!match(parser, TOK_LPAREN)) {
		error(parser, "Expected '('");
		return NULL;
	}

	ProcDecl *proc = proc_decl_create(name);
	proc->is_extern = is_extern;
	proc->loc.line = parser->previous.line;
	proc->loc.column = parser->previous.column;

	/* Parse parameters */
	if (!check(parser, TOK_RPAREN)) {
		do {
			int param_is_consume = 0;

			if (check(parser, TOK_OUT)) {
				error(parser, "`out` parameters were removed; return the value instead via a "
				              "multi-return signature `-> (T, ...)`");
				advance(parser);
			}
			if (match(parser, TOK_CONSUME)) {
				param_is_consume = 1;
			}

			if (!check(parser, TOK_IDENT)) {
				error(parser, "Expected parameter name");
				return NULL;
			}
			char *param_name = token_text(parser->current);
			advance(parser);
			int param_line = parser->previous.line;
			int param_column = parser->previous.column;

			if (!match(parser, TOK_COLON)) {
				error(parser, "Expected ':' after parameter name");
				return NULL;
			}

			TypeRef *param_type = parse_type(parser);
			if (!param_type)
				return NULL;

			Parameter *param = parameter_create(param_name, param_type);
			param->is_consume = param_is_consume;
			param->loc.line = param_line;
			param->loc.column = param_column;
			proc->params = realloc(proc->params, (proc->param_count + 1) * sizeof(Parameter *));
			proc->params[proc->param_count++] = param;
		} while (match(parser, TOK_COMMA));
	}

	if (!match(parser, TOK_RPAREN)) {
		error(parser, "Expected ')' after parameters");
		return NULL;
	}

	/* For extern procs, no body needed */
	if (is_extern) {
		if (!match(parser, TOK_SEMI)) {
			error(parser, "Expected ';' after extern proc declaration");
		}
		Decl *decl = decl_create(DECL_PROC);
		decl->data.proc = proc;
		return decl;
	}

	/* Regular proc: require body */
	if (!match(parser, TOK_LBRACE)) {
		error(parser, "Expected '{'");
		return NULL;
	}

	while (!check(parser, TOK_RBRACE) && !check(parser, TOK_EOF)) {
		Statement *stmt = parse_statement(parser);
		if (!stmt) {
			synchronize(parser);
			continue;
		}

		proc->statements = realloc(proc->statements, (proc->statement_count + 1) * sizeof(Statement *));
		proc->statements[proc->statement_count++] = stmt;
	}

	if (!match(parser, TOK_RBRACE)) {
		error(parser, "Expected '}'");
	}

	proc->end_line = parser->previous.line;
	Decl *decl = decl_create(DECL_PROC);
	decl->data.proc = proc;
	return decl;
}

/* ========== SYSTEM PARSING ========== */

static Decl *parse_sys_decl(Parser *parser) {
	if (!match(parser, TOK_SYS)) {
		error(parser, "Expected 'sys'");
		return NULL;
	}

	if (!check(parser, TOK_IDENT)) {
		error(parser, "Expected system name");
		return NULL;
	}
	char *name = token_text(parser->current);
	advance(parser);

	if (!match(parser, TOK_LPAREN)) {
		error(parser, "Expected '('");
		return NULL;
	}

	SysDecl *sys = sys_decl_create(name);
	sys->loc.line = parser->previous.line;
	sys->loc.column = parser->previous.column;

	/* parse parameters */
	if (!check(parser, TOK_RPAREN)) {
		do {
			if (!check(parser, TOK_IDENT)) {
				error(parser, "Expected parameter name");
				return NULL;
			}

			char *param_name = token_text(parser->current);
			advance(parser);
			int param_line = parser->previous.line;
			int param_column = parser->previous.column;

			Parameter *param = parameter_create(param_name, NULL);
			param->loc.line = param_line;
			param->loc.column = param_column;
			sys->params = realloc(sys->params, (sys->param_count + 1) * sizeof(Parameter *));
			sys->params[sys->param_count++] = param;
		} while (match(parser, TOK_COMMA));
	}

	if (!match(parser, TOK_RPAREN)) {
		error(parser, "Expected ')'");
		return NULL;
	}

	if (!match(parser, TOK_LBRACE)) {
		error(parser, "Expected '{'");
		return NULL;
	}

	while (!check(parser, TOK_RBRACE) && !check(parser, TOK_EOF)) {
		Statement *stmt = parse_statement(parser);
		if (!stmt) {
			synchronize(parser);
			continue;
		}

		sys->statements = realloc(sys->statements, (sys->statement_count + 1) * sizeof(Statement *));
		sys->statements[sys->statement_count++] = stmt;
	}

	if (!match(parser, TOK_RBRACE)) {
		error(parser, "Expected '}'");
	}

	sys->end_line = parser->previous.line;
	Decl *decl = decl_create(DECL_SYS);
	decl->data.sys = sys;
	return decl;
}

/* ========== FUNCTION PARSING ========== */

static Decl *parse_func_decl(Parser *parser) {
	int is_extern = parser->previous.kind == TOK_EXTERN;

	if (!match(parser, TOK_FUNC)) {
		error(parser, "Expected 'func'");
		return NULL;
	}

	if (!check(parser, TOK_IDENT)) {
		error(parser, "Expected function name");
		return NULL;
	}
	char *name = token_text(parser->current);
	advance(parser);

	/* Group-declaration branch: `func NAME = { IDENT (, IDENT)* };` */
	if (match(parser, TOK_EQ)) {
		if (!match(parser, TOK_LBRACE)) {
			error(parser, "Expected '{' after '=' in func group declaration");
			return NULL;
		}
		FuncGroup *g = func_group_create(name);
		g->loc.line = parser->previous.line;
		g->loc.column = parser->previous.column;
		if (check(parser, TOK_RBRACE)) {
			error(parser, "func group must have at least one member");
			func_group_free(g);
			return NULL;
		}
		while (1) {
			if (!check(parser, TOK_IDENT)) {
				error(parser, "Expected member function name in func group");
				func_group_free(g);
				return NULL;
			}
			char *member = token_text(parser->current);
			advance(parser);
			g->member_names = realloc(g->member_names, (g->member_count + 1) * sizeof(char *));
			g->member_names[g->member_count++] = member;
			if (!match(parser, TOK_COMMA))
				break;
			/* Disallow trailing comma: after a `,` the next token must be IDENT, not `}`. */
			if (check(parser, TOK_RBRACE)) {
				error(parser, "trailing comma not allowed in func group member list");
				func_group_free(g);
				return NULL;
			}
		}
		if (!match(parser, TOK_RBRACE)) {
			error(parser, "Expected '}' or ',' in func group member list");
			func_group_free(g);
			return NULL;
		}
		if (!match(parser, TOK_SEMI)) {
			error(parser, "Expected ';' after func group declaration");
		}
		Decl *decl = decl_create(DECL_FUNC_GROUP);
		decl->data.func_group = g;
		decl->loc = g->loc;
		return decl;
	}

	if (!match(parser, TOK_LPAREN)) {
		error(parser, "Expected '(' or '=' after func name");
		return NULL;
	}

	FuncDecl *func = func_decl_create(name, NULL);
	func->is_extern = is_extern;
	func->loc.line = parser->previous.line;
	func->loc.column = parser->previous.column;

	/* parse parameters */
	if (!check(parser, TOK_RPAREN)) {
		do {
			if (check(parser, TOK_OUT)) {
				error(parser, "`out` parameters were removed; return the value instead via a "
				              "multi-return signature `-> (T, ...)`");
				advance(parser);
			}

			if (!check(parser, TOK_IDENT)) {
				error(parser, "Expected parameter name");
				return NULL;
			}

			char *param_name = token_text(parser->current);
			advance(parser);
			int param_line = parser->previous.line;
			int param_column = parser->previous.column;

			if (!match(parser, TOK_COLON)) {
				error(parser, "Expected ':' after parameter name");
				return NULL;
			}

			TypeRef *param_type = parse_type(parser);
			if (!param_type)
				return NULL;

			Parameter *param = parameter_create(param_name, param_type);
			param->loc.line = param_line;
			param->loc.column = param_column;
			func->params = realloc(func->params, (func->param_count + 1) * sizeof(Parameter *));
			func->params[func->param_count++] = param;
		} while (match(parser, TOK_COMMA));
	}

	if (!match(parser, TOK_RPAREN)) {
		error(parser, "Expected ')'");
		return NULL;
	}

	if (!match(parser, TOK_ARROW)) {
		error(parser, "Expected '->'");
		return NULL;
	}

	/* Return type: single `-> T`, or multi-return `-> (T1, …, Tn)`. In the multi form the
	 * leading array returns are caller-passed buffers the func fills in place; the final
	 * type is the scalar physically returned. return_type tracks that scalar. */
	if (match(parser, TOK_LPAREN)) {
		do {
			TypeRef *rt = parse_type(parser);
			if (!rt)
				return NULL;
			func->return_types =
			    realloc(func->return_types, (func->return_type_count + 1) * sizeof(TypeRef *));
			func->return_types[func->return_type_count++] = rt;
		} while (match(parser, TOK_COMMA));
		if (!match(parser, TOK_RPAREN)) {
			error(parser, "Expected ')' after multi-return type list");
			return NULL;
		}
		if (func->return_type_count < 2) {
			error(parser, "a parenthesized return type must list at least two types");
			return NULL;
		}
		func->return_type = func->return_types[func->return_type_count - 1];
	} else {
		TypeRef *return_type = parse_type(parser);
		if (!return_type)
			return NULL;
		func->return_type = return_type;
	}

	/* For extern funcs, no body needed */
	if (is_extern) {
		if (!match(parser, TOK_SEMI)) {
			error(parser, "Expected ';' after extern func declaration");
		}
		Decl *decl = decl_create(DECL_FUNC);
		decl->data.func = func;
		return decl;
	}

	if (!match(parser, TOK_LBRACE)) {
		error(parser, "Expected '{'");
		return NULL;
	}

	while (!check(parser, TOK_RBRACE) && !check(parser, TOK_EOF)) {
		Statement *stmt = parse_statement(parser);
		if (!stmt) {
			synchronize(parser);
			continue;
		}

		func->statements = realloc(func->statements, (func->statement_count + 1) * sizeof(Statement *));
		func->statements[func->statement_count++] = stmt;
	}

	if (!match(parser, TOK_RBRACE)) {
		error(parser, "Expected '}'");
	}

	func->end_line = parser->previous.line;
	Decl *decl = decl_create(DECL_FUNC);
	decl->data.func = func;
	return decl;
}

/* ========== DECLARATION PARSING ========== */

/* ========== WORLD PARSING ========== */

static Decl *parse_static_decl(Parser *parser) {
	/* Check for const IDENT : : expr ; first */
	if (check(parser, TOK_IDENT) && strcmp(token_text(parser->current), "static") != 0) {
		char *name = token_text(parser->current);
		Token ident_tok = parser->current;
		advance(parser);
		if (check(parser, TOK_COLON)) {
			advance(parser); /* consume first : */
			if (check(parser, TOK_COLON)) {
				advance(parser); /* consume second : */
				if (check(parser, TOK_LPAREN)) {
						/* Tuple type alias: `pos :: (x: int, y: int)` mints flat types pos_x, pos_y.
						 * The RHS is a tuple type, not an expression, so parse it directly. */
						advance(parser); /* consume ( */
						char **fnames = NULL;
						TypeRef **ftypes = NULL;
						int fcount = 0;
						while (!check(parser, TOK_RPAREN) && !check(parser, TOK_EOF)) {
							if (!check(parser, TOK_IDENT)) {
								error(parser, "Expected field name in tuple type");
								break;
							}
							char *fname = token_text(parser->current);
							advance(parser);
							if (!match(parser, TOK_COLON)) {
								error(parser, "Expected ':' after tuple field name");
								free(fname);
								break;
							}
							TypeRef *ftype = parse_type(parser);
							if (!ftype) {
								free(fname);
								break;
							}
							fnames = realloc(fnames, (fcount + 1) * sizeof(char *));
							ftypes = realloc(ftypes, (fcount + 1) * sizeof(TypeRef *));
							fnames[fcount] = fname;
							ftypes[fcount] = ftype;
							fcount++;
							if (!match(parser, TOK_COMMA))
								break;
						}
						match(parser, TOK_RPAREN);
						TypeRef *tt = malloc(sizeof(TypeRef));
						tt->kind = TYPE_TUPLE;
						tt->loc.line = ident_tok.line;
						tt->loc.column = ident_tok.column;
						tt->data.tuple.field_names = fnames;
						tt->data.tuple.field_types = ftypes;
						tt->data.tuple.field_count = fcount;
						Decl *dt = decl_create(DECL_CONST);
						dt->loc.line = ident_tok.line;
						dt->loc.column = ident_tok.column;
						dt->data.constant = const_decl_create(name, NULL);
						dt->data.constant->type_value = tt;
						if (check(parser, TOK_SEMI))
							advance(parser);
						return dt;
					}

					Expression *val = parse_expression(parser);
				Decl *d = decl_create(DECL_CONST);
				d->loc.line = ident_tok.line;
				d->loc.column = ident_tok.column;
				d->data.constant = const_decl_create(name, val);
				if (check(parser, TOK_SEMI)) {
					advance(parser); /* consume optional semicolon */
				}
				return d;
			}
		}
		/* Not a const. This fallthrough path shouldn't happen; return NULL to let caller report error */
		return NULL;
	}

	if (check(parser, TOK_IDENT) && strcmp(token_text(parser->current), "static") == 0) {
		advance(parser);
		if (!check(parser, TOK_IDENT)) {
			error(parser, "Expected name after 'static'");
			return NULL;
		}
		char *name = token_text(parser->current);
		Token name_tok = parser->current;
		advance(parser);

		/* `static table<Name>(...)` — the table-addressed allocation form. The
		 * `table<...>` wrapper just names the shape whose singleton table to
		 * allocate; unwrap it to the archetype name. (Legacy `static Name(...)`
		 * stays valid: only the array form uses ':'.) */
		if ((strcmp(name, "table") == 0 || strcmp(name, "pool") == 0) && check(parser, TOK_LT)) {
			free(name);
			advance(parser); /* consume < */
			if (!check(parser, TOK_IDENT)) {
				error(parser, "Expected archetype name in 'static pool<'");
				return NULL;
			}
			name = token_text(parser->current);
			name_tok = parser->current;
			advance(parser);
			if (!match(parser, TOK_GT)) {
				error(parser, "Expected '>' after archetype name in 'static table<...>'");
				return NULL;
			}
		}

		/* Check if this is a static array (static name: type[size];) or archetype (static Name(n);) */
		if (check(parser, TOK_COLON)) {
			/* Static array declaration */
			advance(parser); /* consume ':' */
			TypeRef *element_type = parse_type(parser);
			if (!element_type) {
				error(parser, "Expected type in static array declaration");
				return NULL;
			}

			/* Validate that type is a shaped array */
			if (element_type->kind != TYPE_SHAPED_ARRAY) {
				error(parser, "Expected sized array type for static array (e.g. char[4194304])");
				type_ref_free(element_type);
				return NULL;
			}

			int size = element_type->data.shaped_array.rank;
			TypeRef *elem_type = element_type->data.shaped_array.element_type;

			if (!match(parser, TOK_SEMI)) {
				error(parser, "Expected ';' after static array declaration");
				type_ref_free(element_type);
				return NULL;
			}

			Decl *decl = decl_create(DECL_STATIC);
			decl->loc.line = name_tok.line;
			decl->loc.column = name_tok.column;
			decl->data.static_decl = static_decl_array_create(name, elem_type, size);

			return decl;
		}

		/* Otherwise, treat as static archetype allocation */
		Decl *decl = decl_create(DECL_STATIC);
		decl->loc.line = name_tok.line;
		decl->loc.column = name_tok.column;

		StaticDecl *s = static_decl_archetype_create(name);

		if (match(parser, TOK_LPAREN)) {
			Expression *capacity = parse_expression(parser);
			if (capacity) {
				s->archetype.field_names = malloc(sizeof(char *));
				s->archetype.field_values = malloc(sizeof(Expression *));
				s->archetype.field_names[0] = NULL;
				s->archetype.field_values[0] = capacity;
				s->archetype.field_count = 1;
			}
			if (match(parser, TOK_COMMA)) {
				s->archetype.init_length = parse_expression(parser);
			}
			if (!match(parser, TOK_RPAREN)) {
				error(parser, "Expected ')' after alloc count");
			}

			if (match(parser, TOK_LBRACE)) {
				if (!check(parser, TOK_RBRACE)) {
					do {
						if (!check(parser, TOK_IDENT)) {
							error(parser, "Expected field name in alloc init");
							break;
						}
						char *field_name = token_text(parser->current);
						advance(parser);

						if (!match(parser, TOK_COLON)) {
							error(parser, "Expected ':' after field name in alloc init");
							break;
						}

						Expression *field_value = parse_expression(parser);
						if (!field_value) {
							error(parser, "Expected expression after ':' in alloc init");
							break;
						}

						s->archetype.field_names =
						    realloc(s->archetype.field_names, (s->archetype.field_count + 1) * sizeof(char *));
						s->archetype.field_values =
						    realloc(s->archetype.field_values, (s->archetype.field_count + 1) * sizeof(Expression *));
						s->archetype.field_names[s->archetype.field_count] = field_name;
						s->archetype.field_values[s->archetype.field_count] = field_value;
						s->archetype.field_count++;
					} while (match(parser, TOK_COMMA) && !check(parser, TOK_RBRACE));
				}

				if (!match(parser, TOK_RBRACE)) {
					error(parser, "Expected '}' after alloc init block");
				}
			}
		}

		if (!match(parser, TOK_SEMI)) {
			error(parser, "Expected ';' after alloc declaration");
		}

		decl->data.static_decl = s;
		return decl;
	}
	return NULL;
}

static Decl *parse_decl(Parser *parser) {

	/* Declaration-site decorators. Currently only @allow_pure_proc is
	 * recognized; it sets a flag on the following proc declaration that
	 * suppresses the proc-could-be-func lint. The decorator must be
	 * followed immediately by a `proc` (or `extern proc`) declaration. */
	int allow_pure_proc_flag = 0;
	if (parser->current.kind == TOK_AT) {
		advance(parser); /* consume '@' */
		if (!check(parser, TOK_IDENT)) {
			error(parser, "Expected decorator name after '@'");
			return NULL;
		}
		char *deco_name = token_text(parser->current);
		if (strcmp(deco_name, "allow_pure_proc") != 0) {
			error(parser, "Unknown decorator (only @allow_pure_proc is supported)");
			free(deco_name);
			return NULL;
		}
		free(deco_name);
		advance(parser);
		allow_pure_proc_flag = 1;
		/* Must be followed by proc or extern proc — guard here so the
		 * switch below doesn't silently consume the flag on a wrong kind. */
		if (parser->current.kind != TOK_PROC && parser->current.kind != TOK_EXTERN) {
			error(parser, "@allow_pure_proc must precede a proc declaration");
			return NULL;
		}
	}

	switch (parser->current.kind) {
	case TOK_ARCHETYPE:
		return parse_archetype_decl(parser);
	case TOK_EXTERN:
		advance(parser); /* consume 'extern' */
		if (check(parser, TOK_FUNC)) {
			if (allow_pure_proc_flag) {
				error(parser, "@allow_pure_proc must precede a proc, not a func");
				return NULL;
			}
			return parse_func_decl(parser);
		} else if (check(parser, TOK_PROC)) {
			Decl *d = parse_proc_decl(parser);
			if (d && d->kind == DECL_PROC && allow_pure_proc_flag)
				d->data.proc->allow_pure_proc = 1;
			return d;
		} else {
			error(parser, "Expected 'func' or 'proc' after 'extern'");
			return NULL;
		}
	case TOK_PROC: {
		Decl *d = parse_proc_decl(parser);
		if (d && d->kind == DECL_PROC && allow_pure_proc_flag)
			d->data.proc->allow_pure_proc = 1;
		return d;
	}
	case TOK_SYS:
		return parse_sys_decl(parser);
	case TOK_FUNC:
		return parse_func_decl(parser);
	case TOK_UNSAFE: {
		advance(parser); /* consume 'unsafe' */
		if (check(parser, TOK_FUNC)) {
			Decl *d = parse_func_decl(parser);
			if (d && d->kind == DECL_FUNC)
				d->data.func->is_unsafe = 1;
			return d;
		} else if (check(parser, TOK_PROC)) {
			Decl *d = parse_proc_decl(parser);
			if (d && d->kind == DECL_PROC)
				d->data.proc->is_unsafe = 1;
			return d;
		}
		error(parser, "Expected 'proc' or 'func' after 'unsafe'");
		return NULL;
	}
	case TOK_USE: {
		advance(parser); /* consume 'use' */
		if (!check(parser, TOK_IDENT)) {
			error(parser, "Expected module name after 'use'");
			return NULL;
		}
		char *mod_name = token_text(parser->current);
		Token use_tok = parser->current;
		advance(parser);

		if (!match(parser, TOK_SEMI)) {
			error(parser, "Expected ';' after use declaration");
			return NULL;
		}

		Decl *decl = decl_create(DECL_USE);
		decl->loc.line = use_tok.line;
		decl->loc.column = use_tok.column;
		decl->data.use = use_decl_create(mod_name);
		return decl;
	}
	default:
		/* INFO: Check for top-level const or alloc */
		if (check(parser, TOK_IDENT)) {
			Decl *static_decl = parse_static_decl(parser);
			if (static_decl) {
				return static_decl;
			}
		}
		error(parser, "Expected declaration");
		return NULL;
	}
}

/* ========== EXPRESSION PARSING ========== */

static Expression *parse_primary_expr(Parser *parser) {
	if (check(parser, TOK_NUMBER)) {
		char *lexeme = token_text(parser->current);
		advance(parser);

		Expression *expr = expression_create(EXPR_LITERAL);
		expr->loc.line = parser->previous.line;
		expr->loc.column = parser->previous.column;
		expr->data.literal.lexeme = lexeme;
		return expr;
	}

	if (check(parser, TOK_STRING)) {
		const char *lexeme = parser->current.start;
		size_t len = parser->current.length;
		int str_line = parser->current.line;
		int str_column = parser->current.column;
		advance(parser);

		/* Extract string content (without quotes) and process escape sequences */
		char *value = malloc(len - 1); /* -2 for quotes, +1 for null */
		int out_pos = 0;

		for (size_t i = 1; i < len - 1; i++) {
			if (lexeme[i] == '\\' && i + 1 < len - 1) {
				i++;
				switch (lexeme[i]) {
				case 'n':
					value[out_pos++] = '\n';
					break;
				case 't':
					value[out_pos++] = '\t';
					break;
				case 'r':
					value[out_pos++] = '\r';
					break;
				case '\\':
					value[out_pos++] = '\\';
					break;
				case '"':
					value[out_pos++] = '"';
					break;
				default:
					value[out_pos++] = lexeme[i];
					break;
				}
			} else {
				value[out_pos++] = lexeme[i];
			}
		}
		value[out_pos] = '\0';

		Expression *expr = expression_create(EXPR_STRING);
		expr->loc.line = str_line;
		expr->loc.column = str_column;
		expr->data.string.value = value;
		expr->data.string.length = out_pos;
		return expr;
	}

	if (check(parser, TOK_CHAR_LIT)) {
		char *lexeme = malloc(parser->current.length + 1);
		strncpy(lexeme, parser->current.start, parser->current.length);
		lexeme[parser->current.length] = '\0';
		advance(parser);

		Expression *expr = expression_create(EXPR_LITERAL);
		expr->loc.line = parser->previous.line;
		expr->loc.column = parser->previous.column;
		expr->data.literal.lexeme = lexeme;
		return expr;
	}

	if (check(parser, TOK_LBRACE)) {
		advance(parser);
		int arr_line = parser->previous.line;
		int arr_column = parser->previous.column;

		Expression *arr_expr = expression_create(EXPR_ARRAY_LITERAL);
		arr_expr->loc.line = arr_line;
		arr_expr->loc.column = arr_column;
		arr_expr->data.array_literal.elements = NULL;
		arr_expr->data.array_literal.element_count = 0;

		if (!check(parser, TOK_RBRACE)) {
			do {
				Expression *elem = parse_expression(parser);
				if (!elem) {
					return NULL;
				}

				arr_expr->data.array_literal.elements =
				    realloc(arr_expr->data.array_literal.elements,
				            (arr_expr->data.array_literal.element_count + 1) * sizeof(Expression *));
				arr_expr->data.array_literal.elements[arr_expr->data.array_literal.element_count++] = elem;
			} while (match(parser, TOK_COMMA) && !check(parser, TOK_RBRACE));
		}

		if (!match(parser, TOK_RBRACE)) {
			error(parser, "Expected '}' after array literal");
			return NULL;
		}

		return arr_expr;
	}

	if (check(parser, TOK_IDENT)) {
		char *name = token_text(parser->current);
		advance(parser);
		int name_line = parser->previous.line;
		int name_column = parser->previous.column;

		/* table<Name> in value position: the singleton table for shape Name.
		 * Resolved as the bare archetype name (insert/delete/run resolve it); the
		 * is_table_ref flag lets the formatter round-trip the `table<...>` form. */
		if (strcmp(name, "table") == 0 && check(parser, TOK_LT)) {
			free(name);
			advance(parser); /* consume < */
			if (!check(parser, TOK_IDENT)) {
				error(parser, "Expected archetype name in 'table<'");
				return NULL;
			}
			char *tname = token_text(parser->current);
			advance(parser);
			if (!match(parser, TOK_GT)) {
				error(parser, "Expected '>' after archetype name in 'table<...>'");
				free(tname);
				return NULL;
			}
			Expression *expr = expression_create(EXPR_NAME);
			expr->loc.line = name_line;
			expr->loc.column = name_column;
			expr->data.name.name = tname;
			expr->data.name.is_table_ref = 1;
			return expr;
		}

		/* INFO: Expression-based alloc parsing preserved for future heap_alloc feature.
		   Currently alloc is only allowed as a top-level declaration (DECL_STATIC).
		   When heap allocation is implemented, uncomment this block and create EXPR_HEAP_ALLOC.
		if (strcmp(name, "static") == 0) {
		    if (!check(parser, TOK_IDENT)) {
		        error(parser, "Expected archetype name after 'alloc'");
		        free(name);
		        return NULL;
		    }
		    char *arch_name = token_text(parser->current);
		    advance(parser);
		    int alloc_line = parser->previous.line;
		    int alloc_column = parser->previous.column;

		    Expression *alloc_expr = expression_create(EXPR_ALLOC);
		    alloc_expr->loc.line = alloc_line;
		    alloc_expr->loc.column = alloc_column;
		    alloc_expr->data.alloc.archetype_name = arch_name;
		    alloc_expr->data.alloc.field_names = NULL;
		    alloc_expr->data.alloc.field_values = NULL;
		    alloc_expr->data.alloc.field_count = 0;
		    alloc_expr->data.alloc.init_length = NULL;

		    if (match(parser, TOK_LPAREN)) {
		        Expression *count = parse_expression(parser);
		        if (count) {
		            alloc_expr->data.alloc.field_names = malloc(sizeof(char *));
		            alloc_expr->data.alloc.field_values = malloc(sizeof(Expression *));
		            alloc_expr->data.alloc.field_names[0] = NULL;
		            alloc_expr->data.alloc.field_values[0] = count;
		            alloc_expr->data.alloc.field_count = 1;
		        }
		        if (!match(parser, TOK_RPAREN)) {
		            error(parser, "Expected ')' after alloc count");
		        }

		        if (match(parser, TOK_LBRACE)) {
		            if (!check(parser, TOK_RBRACE)) {
		                do {
		                    if (!check(parser, TOK_IDENT)) {
		                        error(parser, "Expected field name in alloc init");
		                        break;
		                    }
		                    char *field_name = token_text(parser->current);
		                    advance(parser);

		                    if (!match(parser, TOK_COLON)) {
		                        error(parser, "Expected ':' after field name in alloc init");
		                        break;
		                    }

		                    Expression *field_value = parse_expression(parser);
		                    if (!field_value) {
		                        error(parser, "Expected expression after ':' in alloc init");
		                        break;
		                    }

		                    alloc_expr->data.alloc.field_names =
		                        realloc(alloc_expr->data.alloc.field_names,
		                                (alloc_expr->data.alloc.field_count + 1) * sizeof(char *));
		                    alloc_expr->data.alloc.field_values =
		                        realloc(alloc_expr->data.alloc.field_values,
		                                (alloc_expr->data.alloc.field_count + 1) * sizeof(Expression *));
		                    alloc_expr->data.alloc.field_names[alloc_expr->data.alloc.field_count] = field_name;
		                    alloc_expr->data.alloc.field_values[alloc_expr->data.alloc.field_count] = field_value;
		                    alloc_expr->data.alloc.field_count++;
		                } while (match(parser, TOK_COMMA));
		            }

		            if (!match(parser, TOK_RBRACE)) {
		                error(parser, "Expected '}' after alloc init block");
		            }
		        }
		    }

		    free(name);
		    return alloc_expr;
		}
		*/

		/* check for field access or indexing */
		if (match(parser, TOK_DOT)) {
			if (!check(parser, TOK_IDENT)) {
				error(parser, "Expected field name after '.'");
				free(name);
				return NULL;
			}

			Expression *base = expression_create(EXPR_NAME);
			base->loc.line = name_line;
			base->loc.column = name_column;
			base->data.name.name = name;

			/* Process first field (DOT already consumed) */
			char *field_name = token_text(parser->current);
			advance(parser);

			Expression *field = expression_create(EXPR_FIELD);
			field->loc.line = base->loc.line;
			field->loc.column = base->loc.column;
			field->data.field.base = base;
			field->data.field.field_name = field_name;

			base = field;

			/* Handle chained field access: p.pos.x.y */
			while (match(parser, TOK_DOT)) {
				if (!check(parser, TOK_IDENT)) {
					error(parser, "Expected field name after '.'");
					return NULL;
				}

				field_name = token_text(parser->current);
				advance(parser);

				field = expression_create(EXPR_FIELD);
				field->loc.line = base->loc.line;
				field->loc.column = base->loc.column;
				field->data.field.base = base;
				field->data.field.field_name = field_name;

				base = field;
			}

			/* check for indexing on the final field */
			if (match(parser, TOK_LBRACKET)) {
				Expression *index = expression_create(EXPR_INDEX);
				index->loc.line = base->loc.line;
				index->loc.column = base->loc.column;
				index->data.index.base = base;
				index->data.index.indices = NULL;
				index->data.index.index_count = 0;

				do {
					Expression *idx_expr = parse_expression(parser);
					if (!idx_expr)
						return NULL;

					index->data.index.indices =
					    realloc(index->data.index.indices, (index->data.index.index_count + 1) * sizeof(Expression *));
					index->data.index.indices[index->data.index.index_count++] = idx_expr;
				} while (match(parser, TOK_COMMA));

				if (!match(parser, TOK_RBRACKET)) {
					error(parser, "Expected ']'");
					return NULL;
				}

				return index;
			}

			return base;
		}

		/* check for indexing */
		if (match(parser, TOK_LBRACKET)) {
			Expression *index = expression_create(EXPR_INDEX);

			Expression *base = expression_create(EXPR_NAME);
			base->loc.line = name_line;
			base->loc.column = name_column;
			base->data.name.name = name;
			index->loc.line = base->loc.line;
			index->loc.column = base->loc.column;
			index->data.index.base = base;
			index->data.index.indices = NULL;
			index->data.index.index_count = 0;

			do {
				Expression *idx_expr = parse_expression(parser);
				if (!idx_expr)
					return NULL;

				index->data.index.indices =
				    realloc(index->data.index.indices, (index->data.index.index_count + 1) * sizeof(Expression *));
				index->data.index.indices[index->data.index.index_count++] = idx_expr;
			} while (match(parser, TOK_COMMA));

			if (!match(parser, TOK_RBRACKET)) {
				error(parser, "Expected ']'");
				return NULL;
			}

			return index;
		}

		/* check for function call */
		if (match(parser, TOK_LPAREN)) {
			Expression *expr = expression_create(EXPR_CALL);
			Expression *callee = expression_create(EXPR_NAME);
			callee->loc.line = name_line;
			callee->loc.column = name_column;
			callee->data.name.name = name;
			expr->loc.line = callee->loc.line;
			expr->loc.column = callee->loc.column;
			expr->data.call.callee = callee;
			expr->data.call.args = NULL;
			expr->data.call.arg_count = 0;

			/* parse arguments */
			if (!check(parser, TOK_RPAREN)) {
				do {
					Expression *arg = parse_expression(parser);
					if (!arg)
						return NULL;

					expr->data.call.args =
					    realloc(expr->data.call.args, (expr->data.call.arg_count + 1) * sizeof(Expression *));
					expr->data.call.args[expr->data.call.arg_count++] = arg;
				} while (match(parser, TOK_COMMA));
			}

			if (!match(parser, TOK_RPAREN)) {
				error(parser, "Expected ')' after arguments");
				return NULL;
			}

			return expr;
		}

		Expression *expr = expression_create(EXPR_NAME);
		expr->loc.line = name_line;
		expr->loc.column = name_column;
		expr->data.name.name = name;
		return expr;
	}

	if (match(parser, TOK_LPAREN)) {
		Expression *expr = parse_expression(parser);
		if (!match(parser, TOK_RPAREN)) {
			error(parser, "Expected ')' after expression");
		}
		return expr;
	}

	error(parser, "Expected expression");
	return NULL;
}

/* Prefix unary operators: `-x` (negate) and `!x` (logical not). Binds tighter
 * than binary operators, looser than postfix (calls/indexing in primary). */
static Expression *parse_unary_expr(Parser *parser) {
	/* `move <expr>` — call-site ownership transfer (contextual keyword). The value is
	 * transparent; the checker marks the operand consumed (use-after-move is an error). */
	if (check(parser, TOK_IDENT)) {
		char *tt = token_text(parser->current);
		int is_move = (strcmp(tt, "move") == 0);
		free(tt);
		if (is_move) {
			int line = parser->current.line, col = parser->current.column;
			advance(parser);
			Expression *operand = parse_unary_expr(parser);
			if (!operand)
				return NULL;
			Expression *u = expression_create(EXPR_UNARY);
			u->loc.line = line;
			u->loc.column = col;
			u->data.unary.op = UNARY_MOVE;
			u->data.unary.operand = operand;
			return u;
		}
	}
	if (check(parser, TOK_MINUS) || check(parser, TOK_BANG)) {
		TokenKind op_kind = parser->current.kind;
		int line = parser->current.line;
		int col = parser->current.column;
		advance(parser);
		Expression *operand = parse_unary_expr(parser); /* allow -(-x), !!x */
		if (!operand)
			return NULL;
		Expression *u = expression_create(EXPR_UNARY);
		u->loc.line = line;
		u->loc.column = col;
		u->data.unary.op = (op_kind == TOK_MINUS) ? UNARY_NEG : UNARY_NOT;
		u->data.unary.operand = operand;
		return u;
	}
	return parse_primary_expr(parser);
}

/* Map a binary-operator token to its Operator. Only called for tokens that
   binop_prec() has already classified as binary operators. */
static Operator tok_to_op(TokenKind k) {
	switch (k) {
	case TOK_PLUS:
		return OP_ADD;
	case TOK_MINUS:
		return OP_SUB;
	case TOK_STAR:
		return OP_MUL;
	case TOK_SLASH:
		return OP_DIV;
	case TOK_EQ_EQ:
		return OP_EQ;
	case TOK_BANG_EQ:
		return OP_NEQ;
	case TOK_LT:
		return OP_LT;
	case TOK_GT:
		return OP_GT;
	case TOK_LT_EQ:
		return OP_LTE;
	case TOK_GT_EQ:
		return OP_GTE;
	default:
		return OP_ADD; /* unreachable: guarded by binop_prec */
	}
}

/* Binary-operator precedence: higher binds tighter, -1 if not a binary op.
   All operators are left-associative.
     3: * /        2: + -        1: < > <= >= == != */
static int binop_prec(TokenKind k) {
	switch (k) {
	case TOK_STAR:
	case TOK_SLASH:
		return 3;
	case TOK_PLUS:
	case TOK_MINUS:
		return 2;
	case TOK_LT:
	case TOK_GT:
	case TOK_LT_EQ:
	case TOK_GT_EQ:
	case TOK_EQ_EQ:
	case TOK_BANG_EQ:
		return 1;
	default:
		return -1;
	}
}

/* Precedence climbing: extend `left` with binary operators whose precedence is
   >= min_prec, so e.g. `2 + 3 * 2` builds (2 + (3 * 2)) rather than a flat fold. */
static Expression *parse_binary_rhs(Parser *parser, Expression *left, int min_prec) {
	if (!left)
		return NULL;

	for (;;) {
		int prec = binop_prec(parser->current.kind);
		if (prec < min_prec)
			return left;

		TokenKind op_kind = parser->current.kind;
		advance(parser);

		Expression *right = parse_unary_expr(parser);
		if (!right)
			return NULL;

		/* Left-associative: only fold a strictly tighter operator into `right`. */
		while (binop_prec(parser->current.kind) > prec) {
			right = parse_binary_rhs(parser, right, prec + 1);
			if (!right)
				return NULL;
		}

		Expression *binary = expression_create(EXPR_BINARY);
		binary->loc.line = left->loc.line;
		binary->loc.column = left->loc.column;
		binary->data.binary.op = tok_to_op(op_kind);
		binary->data.binary.left = left;
		binary->data.binary.right = right;

		left = binary;
	}
}

static Expression *parse_binary_expr(Parser *parser) {
	Expression *left = parse_unary_expr(parser);
	return parse_binary_rhs(parser, left, 1);
}

static Expression *parse_expression(Parser *parser) {
	return parse_binary_expr(parser);
}

/* ========== STATEMENT PARSING ========== */

static Statement *parse_statement(Parser *parser) {
	/* Prevent stack overflow from unbounded recursion */
	const int MAX_RECURSION_DEPTH = 1000;
	if (parser->recursion_depth > MAX_RECURSION_DEPTH) {
		error(parser, "Recursion limit exceeded");
		goto cleanup;
	}
	parser->recursion_depth++;
	Statement *result = NULL;

	/* Drain pending trivia: any comments/blank-lines that appeared since the
	 * previous syntactic token become the leading trivia of this statement. */
	Trivia *leading = NULL;
	int leading_count = 0;
	take_pending_as_leading(parser, &leading, &leading_count);

	if (match(parser, TOK_SEMI)) {
		goto cleanup; /* empty statement */
	}

	if (match(parser, TOK_BREAK)) {
		int break_line = parser->previous.line;
		int break_column = parser->previous.column;

		if (!match(parser, TOK_SEMI)) {
			error(parser, "Expected ';' after break");
		}

		Statement *stmt = statement_create(STMT_BREAK);
		stmt->loc.line = break_line;
		stmt->loc.column = break_column;
		result = stmt;
		goto cleanup;
	}

	if (match(parser, TOK_EACH_FIELD)) {
		int ef_line = parser->previous.line;
		int ef_column = parser->previous.column;

		if (!check(parser, TOK_IDENT)) {
			error(parser, "Expected binding name after 'each_field'");
			goto cleanup;
		}
		char *binding = token_text(parser->current);
		advance(parser);

		TypeRef *filter_type = NULL;
		if (match(parser, TOK_COLON)) {
			filter_type = parse_type(parser);
			if (!filter_type) {
				free(binding);
				goto cleanup;
			}
		}

		if (!match(parser, TOK_IN)) {
			error(parser, "Expected 'in' after each_field binding");
			free(binding);
			type_ref_free(filter_type);
			goto cleanup;
		}

		if (!check(parser, TOK_IDENT)) {
			error(parser, "Expected archetype parameter name after 'in'");
			free(binding);
			type_ref_free(filter_type);
			goto cleanup;
		}
		char *arch_name = token_text(parser->current);
		advance(parser);

		if (!match(parser, TOK_LBRACE)) {
			error(parser, "Expected '{' to start each_field body");
			free(binding);
			free(arch_name);
			type_ref_free(filter_type);
			goto cleanup;
		}

		Statement **body = NULL;
		int body_count = 0;
		while (!check(parser, TOK_RBRACE) && !check(parser, TOK_EOF)) {
			Statement *body_stmt = parse_statement(parser);
			if (!body_stmt)
				break;
			body = realloc(body, (body_count + 1) * sizeof(Statement *));
			body[body_count++] = body_stmt;
		}

		if (!match(parser, TOK_RBRACE)) {
			error(parser, "Expected '}' to close each_field body");
		}

		Statement *stmt = statement_create(STMT_EACH_FIELD);
		stmt->loc.line = ef_line;
		stmt->loc.column = ef_column;
		stmt->data.each_field.binding_name = binding;
		stmt->data.each_field.filter_type = filter_type;
		stmt->data.each_field.arch_param_name = arch_name;
		stmt->data.each_field.body = body;
		stmt->data.each_field.body_count = body_count;
		result = stmt;
		goto cleanup;
	}

	if (match(parser, TOK_RETURN)) {
		int return_line = parser->previous.line;
		int return_column = parser->previous.column;

		Expression *value = parse_expression(parser);
		if (!value) {
			error(parser, "Expected expression after 'return'");
			goto cleanup;
		}

		/* Multi-return `return buf, …, n`: collect all values. The leading ones are
		 * caller-passed buffers filled in place (no codegen at the return); only the final
		 * scalar is physically returned. The full list is kept for faithful formatting. */
		Expression **ret_values = malloc(sizeof(Expression *));
		ret_values[0] = value;
		int ret_value_count = 1;
		while (match(parser, TOK_COMMA)) {
			Expression *nv = parse_expression(parser);
			if (!nv) {
				error(parser, "Expected expression after ',' in return statement");
				goto cleanup;
			}
			ret_values = realloc(ret_values, (ret_value_count + 1) * sizeof(Expression *));
			ret_values[ret_value_count++] = nv;
		}
		value = ret_values[ret_value_count - 1]; /* scalar = last */

		if (!match(parser, TOK_SEMI)) {
			error(parser, "Expected ';' after return statement");
		}

		Statement *stmt = statement_create(STMT_RETURN);
		stmt->loc.line = return_line;
		stmt->loc.column = return_column;
		stmt->data.return_stmt.value = value;
		stmt->data.return_stmt.values = ret_values;
		stmt->data.return_stmt.value_count = ret_value_count;
		result = stmt;
		goto cleanup;
	}

	/* check for run statement */
	if (check(parser, TOK_IDENT)) {
		const char *text = parser->current.start;
		size_t len = parser->current.length;
		if (len == 3 && strncmp(text, "run", 3) == 0) {
			advance(parser); /* consume 'run' */

			if (!check(parser, TOK_IDENT)) {
				error(parser, "Expected system name");
				parser->recursion_depth--;
				goto cleanup;
			}
			char *system_name = token_text(parser->current);
			advance(parser);

			if (!match(parser, TOK_SEMI)) {
				error(parser, "Expected ';'");
			}

			Statement *stmt = statement_create(STMT_RUN);
			stmt->loc.line = parser->previous.line;
			stmt->loc.column = parser->previous.column;
			stmt->data.run_stmt.system_name = system_name;
			stmt->data.run_stmt.world_name = NULL;
			result = stmt;
			goto cleanup;
		}
	}

	/* Paren-based multi-bind: (let x:, y) = expr; */
	if (match(parser, TOK_LPAREN)) {

		BindingTarget *targets = NULL;
		int target_count = 0;
		int max_targets = 16;
		targets = malloc(max_targets * sizeof(BindingTarget));

		/* Parse binding targets with loop guard */
		int loop_count = 0;
		const int MAX_TARGETS = 1000;
		while (!check(parser, TOK_RPAREN) && !check(parser, TOK_EOF)) {
			if (++loop_count > MAX_TARGETS) {
				error(parser, "Too many binding targets");
				break;
			}

			int is_new = match(parser, TOK_LET) ? 1 : 0;

			if (!check(parser, TOK_IDENT)) {
				error(parser, "Expected variable name in binding");
				break;
			}

			char *name = token_text(parser->current);
			advance(parser);

			TypeRef *type = NULL;
			if (is_new && match(parser, TOK_COLON)) {
				if (!check(parser, TOK_COMMA) && !check(parser, TOK_RPAREN)) {
					type = parse_type(parser);
				}
			}

			if (target_count >= max_targets) {
				max_targets *= 2;
				targets = realloc(targets, max_targets * sizeof(BindingTarget));
			}

			targets[target_count].name = name;
			targets[target_count].is_new = is_new;
			targets[target_count].type = type;
			target_count++;

			if (!match(parser, TOK_COMMA)) {
				break;
			}
		}

		if (!match(parser, TOK_RPAREN)) {
			error(parser, "Expected ')' after binding targets");
			goto cleanup;
		}

		if (!match(parser, TOK_EQ)) {
			error(parser, "Expected '=' after binding targets");
			goto cleanup;
		}

		Expression *value = parse_expression(parser);
		if (!value) {
			goto cleanup;
		}

		if (!match(parser, TOK_SEMI)) {
			error(parser, "Expected ';' after binding statement");
		}

		Statement *stmt = statement_create(STMT_MULTI_BIND);
		stmt->loc.line = parser->previous.line;
		stmt->loc.column = parser->previous.column;
		stmt->data.multi_bind.targets = targets;
		stmt->data.multi_bind.target_count = target_count;
		stmt->data.multi_bind.value = value;
		stmt->data.multi_bind.from_shorthand = 0;
		result = stmt;
		goto cleanup;
	}

	if (match(parser, TOK_LET)) {
		int let_line = parser->previous.line;
		int let_column = parser->previous.column;

		if (!check(parser, TOK_IDENT)) {
			error(parser, "Expected variable name after 'let'");
			goto cleanup;
		}

		char *name = token_text(parser->current);
		advance(parser);

		/* Check for multi-value let: let a, b, c := expr or let a, b = expr (old) */
		char **names = NULL;
		int name_count = 0;
		if (match(parser, TOK_COMMA)) {
			/* Multi-value let */
			names = malloc(sizeof(char *));
			names[0] = name;
			name_count = 1;

			while (!check(parser, TOK_COLON) && !check(parser, TOK_EQ) && !check(parser, TOK_EOF)) {
				if (!check(parser, TOK_IDENT)) {
					error(parser, "Expected variable name in multi-value let");
					parser->recursion_depth--;
					goto cleanup;
				}
				char *var_name = token_text(parser->current);
				advance(parser);

				names = realloc(names, (name_count + 1) * sizeof(char *));
				names[name_count++] = var_name;

				if (!match(parser, TOK_COMMA)) {
					break;
				}
			}

			/* Support both new syntax (:=) and old syntax (=) */
			if (match(parser, TOK_COLON)) {
				if (!match(parser, TOK_EQ)) {
					error(parser, "Expected '=' after ':' in multi-value let");
					parser->recursion_depth--;
					goto cleanup;
				}
			} else if (!match(parser, TOK_EQ)) {
				error(parser, "Expected ':=' or '=' in multi-value let");
				parser->recursion_depth--;
				goto cleanup;
			}

			Expression *value = parse_expression(parser);
			if (!value) {
				parser->recursion_depth--;
				goto cleanup;
			}

			if (!match(parser, TOK_SEMI)) {
				error(parser, "Expected ';' after let statement");
			}

			/* Convert to STMT_MULTI_BIND with is_new=1 for all targets */
			BindingTarget *targets = malloc(name_count * sizeof(BindingTarget));
			for (int i = 0; i < name_count; i++) {
				targets[i].name = names[i];
				targets[i].is_new = 1;
				targets[i].type = NULL;
			}
			free(names);

			Statement *stmt = statement_create(STMT_MULTI_BIND);
			stmt->loc.line = let_line;
			stmt->loc.column = let_column;
			stmt->data.multi_bind.targets = targets;
			stmt->data.multi_bind.target_count = name_count;
			stmt->data.multi_bind.value = value;
			stmt->data.multi_bind.from_shorthand = 1;
			result = stmt;
			goto cleanup;
		}

		/* Single-value let: let x := expr or let x: type = expr or let x: type or let x = expr (old) */
		TypeRef *type = NULL;
		Expression *value = NULL;

		if (match(parser, TOK_COLON)) {
			/* New/explicit syntax: colon present */
			if (check(parser, TOK_EQ)) {
				/* Inferred: let x := expr */
				advance(parser); /* consume '=' */
				value = parse_expression(parser);
				if (!value) {
					parser->recursion_depth--;
					goto cleanup;
				}
			} else {
				/* Explicit: let x: type [= expr] */
				type = parse_type(parser);
				if (!type) {
					parser->recursion_depth--;
					goto cleanup;
				}

				if (match(parser, TOK_EQ)) {
					value = parse_expression(parser);
					if (!value) {
						parser->recursion_depth--;
						goto cleanup;
					}
				}
			}
		} else if (match(parser, TOK_EQ)) {
			/* Old syntax (backward compat): let x = expr */
			value = parse_expression(parser);
			if (!value) {
				parser->recursion_depth--;
				goto cleanup;
			}
		} else {
			error(parser, "Expected ':' or '=' after variable name");
			goto cleanup;
		}

		if (!match(parser, TOK_SEMI)) {
			error(parser, "Expected ';' after let statement");
		}

		Statement *stmt = statement_create(STMT_LET);
		stmt->loc.line = let_line;
		stmt->loc.column = let_column;
		stmt->data.let_stmt.name = name;
		stmt->data.let_stmt.names = NULL;
		stmt->data.let_stmt.name_count = 0;
		stmt->data.let_stmt.type = type;
		stmt->data.let_stmt.value = value;
		result = stmt;
		goto cleanup;
	}

	if (match(parser, TOK_IF)) {
		int if_line = parser->previous.line;
		int if_column = parser->previous.column;

		if (!match(parser, TOK_LPAREN)) {
			error(parser, "Expected '(' after 'if'");
			goto cleanup;
		}

		Expression *cond = parse_expression(parser);
		if (!cond) {
			goto cleanup;
		}

		if (!match(parser, TOK_RPAREN)) {
			error(parser, "Expected ')' after if condition");
			goto cleanup;
		}

		Statement *stmt = statement_create(STMT_IF);
		stmt->loc.line = if_line;
		stmt->loc.column = if_column;
		stmt->data.if_stmt.cond = cond;
		stmt->data.if_stmt.then_body = NULL;
		stmt->data.if_stmt.then_count = 0;
		stmt->data.if_stmt.else_body = NULL;
		stmt->data.if_stmt.else_count = 0;

		if (match(parser, TOK_LBRACE)) {
			/* Braced block: collect statements until } */
			while (!check(parser, TOK_RBRACE) && !check(parser, TOK_EOF)) {
				Statement *body_stmt = parse_statement(parser);
				if (!body_stmt) {
					synchronize(parser);
					continue;
				}

				stmt->data.if_stmt.then_body =
				    realloc(stmt->data.if_stmt.then_body, (stmt->data.if_stmt.then_count + 1) * sizeof(Statement *));
				stmt->data.if_stmt.then_body[stmt->data.if_stmt.then_count++] = body_stmt;
			}

			if (!match(parser, TOK_RBRACE)) {
				error(parser, "Expected '}' after if body");
			}
		} else {
			/* Braceless: parse exactly one statement */
			Statement *body_stmt = parse_statement(parser);
			if (body_stmt) {
				stmt->data.if_stmt.then_body = malloc(sizeof(Statement *));
				stmt->data.if_stmt.then_body[0] = body_stmt;
				stmt->data.if_stmt.then_count = 1;
			}
		}

		/* Parse else clause if present */
		if (match(parser, TOK_ELSE)) {
			if (match(parser, TOK_LBRACE)) {
				/* Braced else block: collect statements until } */
				while (!check(parser, TOK_RBRACE) && !check(parser, TOK_EOF)) {
					Statement *body_stmt = parse_statement(parser);
					if (!body_stmt) {
						synchronize(parser);
						continue;
					}

					stmt->data.if_stmt.else_body = realloc(stmt->data.if_stmt.else_body,
					                                       (stmt->data.if_stmt.else_count + 1) * sizeof(Statement *));
					stmt->data.if_stmt.else_body[stmt->data.if_stmt.else_count++] = body_stmt;
				}

				if (!match(parser, TOK_RBRACE)) {
					error(parser, "Expected '}' after else body");
				}
			} else {
				/* Braceless else: parse exactly one statement */
				Statement *body_stmt = parse_statement(parser);
				if (body_stmt) {
					stmt->data.if_stmt.else_body = malloc(sizeof(Statement *));
					stmt->data.if_stmt.else_body[0] = body_stmt;
					stmt->data.if_stmt.else_count = 1;
				}
			}
		}

		result = stmt;
		goto cleanup;
	}

	if (match(parser, TOK_FOR)) {
		int for_line = parser->previous.line;
		int for_column = parser->previous.column;

		Statement *stmt = statement_create(STMT_FOR);
		stmt->loc.line = for_line;
		stmt->loc.column = for_column;
		stmt->data.for_stmt.var_name = NULL;
		stmt->data.for_stmt.iterable = NULL;
		stmt->data.for_stmt.init = NULL;
		stmt->data.for_stmt.condition = NULL;
		stmt->data.for_stmt.increment = NULL;
		stmt->data.for_stmt.body = NULL;
		stmt->data.for_stmt.body_count = 0;

		/* Check for infinite for: for { } */
		if (match(parser, TOK_LBRACE)) {
			/* Infinite loop - no var_name, iterable, or condition (already set to NULL above) */
		} else if (match(parser, TOK_LPAREN)) {
			/* for loop: for (init; cond; incr) { } */
			/* All three parts are optional */

			Statement *init = NULL;
			Expression *cond = NULL;

			/* Parse init (can be let statement or expression, or empty) */
			if (check(parser, TOK_LET)) {
				/* Parse let statement without trailing semicolon requirement */
				advance(parser); /* consume LET */
				int let_line = parser->previous.line;
				int let_column = parser->previous.column;

				if (!check(parser, TOK_IDENT)) {
					error(parser, "Expected variable name after 'let'");
					goto cleanup;
				}

				char *name = token_text(parser->current);
				advance(parser);

				TypeRef *type = NULL;
				if (match(parser, TOK_COLON)) {
					type = parse_type(parser);
					if (!type)
						goto cleanup;
				}

				Expression *value = NULL;
				if (match(parser, TOK_EQ)) {
					value = parse_expression(parser);
					if (!value)
						goto cleanup;
				} else if (!type) {
					error(parser, "Expected '=' or type annotation after variable name");
					goto cleanup;
				}

				Statement *init_stmt = statement_create(STMT_LET);
				init_stmt->loc.line = let_line;
				init_stmt->loc.column = let_column;
				init_stmt->data.let_stmt.name = name;
				init_stmt->data.let_stmt.names = NULL;
				init_stmt->data.let_stmt.name_count = 0;
				init_stmt->data.let_stmt.type = type;
				init_stmt->data.let_stmt.value = value;
				init = init_stmt;
			} else if (!check(parser, TOK_SEMI)) {
				/* Parse expression as init */
				Expression *init_expr = parse_expression(parser);
				if (!init_expr)
					goto cleanup;
				/* Wrap expression in statement */
				Statement *init_stmt = statement_create(STMT_EXPR);
				init_stmt->loc = init_expr->loc;
				init_stmt->data.expr_stmt.expr = init_expr;
				init = init_stmt;
			}

			/* Expect and consume first semicolon */
			if (!match(parser, TOK_SEMI)) {
				error(parser, "Expected ';' in for loop");
				goto cleanup;
			}

			/* Parse condition (can be empty) */
			if (!check(parser, TOK_SEMI)) {
				cond = parse_expression(parser);
				if (!cond)
					goto cleanup;
			}

			/* Expect and consume second semicolon (required) */
			if (!match(parser, TOK_SEMI)) {
				error(parser, "Expected ';' in for loop");
				goto cleanup;
			}

			/* Parse increment statement (can be empty) */
			Statement *incr_stmt = NULL;
			if (!check(parser, TOK_RPAREN)) {
				/* Parse as an assignment or expression statement */
				Expression *target = parse_expression(parser);
				if (!target)
					goto cleanup;

				if (check(parser, TOK_EQ) || check(parser, TOK_PLUS_EQ) || check(parser, TOK_MINUS_EQ) ||
				    check(parser, TOK_STAR_EQ) || check(parser, TOK_SLASH_EQ)) {
					/* Assignment statement */
					Operator op = OP_NONE;
					if (match(parser, TOK_EQ)) {
						op = OP_NONE;
					} else if (match(parser, TOK_PLUS_EQ)) {
						op = OP_ADD;
					} else if (match(parser, TOK_MINUS_EQ)) {
						op = OP_SUB;
					} else if (match(parser, TOK_STAR_EQ)) {
						op = OP_MUL;
					} else if (match(parser, TOK_SLASH_EQ)) {
						op = OP_DIV;
					}

					Expression *value = parse_expression(parser);
					if (!value) {
						error(parser, "Expected value in for increment assignment");
						goto cleanup;
					}

					incr_stmt = statement_create(STMT_ASSIGN);
					incr_stmt->loc = target->loc;
					incr_stmt->data.assign_stmt.target = target;
					incr_stmt->data.assign_stmt.value = value;
					incr_stmt->data.assign_stmt.op = op;
				} else {
					/* Expression statement */
					incr_stmt = statement_create(STMT_EXPR);
					incr_stmt->loc = target->loc;
					incr_stmt->data.expr_stmt.expr = target;
				}
			}

			stmt->data.for_stmt.init = init;
			stmt->data.for_stmt.condition = cond;
			stmt->data.for_stmt.increment = incr_stmt;

			if (!match(parser, TOK_RPAREN)) {
				error(parser, "Expected ')' after for clause");
				goto cleanup;
			}

			if (!match(parser, TOK_LBRACE)) {
				error(parser, "Expected '{'");
				goto cleanup;
			}
		} else {
			/* Range-based for: for var in iterable { } */
			if (!check(parser, TOK_IDENT)) {
				error(parser, "Expected variable name after 'for'");
				goto cleanup;
			}

			char *var_name = token_text(parser->current);
			advance(parser);

			if (!match(parser, TOK_IN)) {
				error(parser, "Expected 'in' in for loop");
				goto cleanup;
			}

			if (!check(parser, TOK_IDENT)) {
				error(parser, "Expected iterable after 'in'");
				goto cleanup;
			}

			char *iterable_name = token_text(parser->current);
			advance(parser);

			Expression *iterable = expression_create(EXPR_NAME);
			iterable->loc.line = parser->previous.line;
			iterable->loc.column = parser->previous.column;
			iterable->data.name.name = iterable_name;

			stmt->data.for_stmt.var_name = var_name;
			stmt->data.for_stmt.iterable = iterable;

			if (!match(parser, TOK_LBRACE)) {
				error(parser, "Expected '{'");
				goto cleanup;
			}
		}

		while (!check(parser, TOK_RBRACE) && !check(parser, TOK_EOF)) {
			Statement *body_stmt = parse_statement(parser);
			if (!body_stmt) {
				synchronize(parser);
				continue;
			}

			stmt->data.for_stmt.body =
			    realloc(stmt->data.for_stmt.body, (stmt->data.for_stmt.body_count + 1) * sizeof(Statement *));
			stmt->data.for_stmt.body[stmt->data.for_stmt.body_count++] = body_stmt;
		}

		if (!match(parser, TOK_RBRACE)) {
			error(parser, "Expected '}'");
		}

		result = stmt;
		goto cleanup;
	}

	/* try to parse as assignment */
	Expression *target = parse_expression(parser);
	if (!target)
		goto cleanup;

	/* check for assignment operators */
	if (check(parser, TOK_EQ) || check(parser, TOK_PLUS_EQ) || check(parser, TOK_MINUS_EQ) ||
	    check(parser, TOK_STAR_EQ) || check(parser, TOK_SLASH_EQ)) {

		/* capture the operator before consuming it */
		TokenKind op_token = parser->current.kind;
		advance(parser); /* consume assignment operator */

		Expression *value = parse_expression(parser);
		if (!value)
			goto cleanup;

		if (!match(parser, TOK_SEMI)) {
			error(parser, "Expected ';' after assignment");
		}

		/* map token to Operator enum */
		Operator op = OP_NONE;
		if (op_token == TOK_PLUS_EQ)
			op = OP_ADD;
		else if (op_token == TOK_MINUS_EQ)
			op = OP_SUB;
		else if (op_token == TOK_STAR_EQ)
			op = OP_MUL;
		else if (op_token == TOK_SLASH_EQ)
			op = OP_DIV;

		Statement *stmt = statement_create(STMT_ASSIGN);
		stmt->loc.line = target->loc.line;
		stmt->loc.column = target->loc.column;
		stmt->data.assign_stmt.target = target;
		stmt->data.assign_stmt.value = value;
		stmt->data.assign_stmt.op = op;
		result = stmt;
		goto cleanup;
	}

	/* otherwise it was an expression statement */
	if (!match(parser, TOK_SEMI)) {
		error(parser, "Expected ';' after expression statement");
	}

	Statement *stmt = statement_create(STMT_EXPR);
	stmt->loc.line = target->loc.line;
	stmt->loc.column = target->loc.column;
	stmt->data.expr_stmt.expr = target;
	result = stmt;

cleanup:
	parser->recursion_depth--;
	if (result) {
		result->leading_trivia = leading;
		result->leading_count = leading_count;
		result->last_line = parser->previous.line;
		steal_trailing_inline(parser, result->last_line, &result->trailing_trivia, &result->trailing_count);
	} else {
		free(leading);
	}
	return result;
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
	advance(parser);
}

ParseResult parse_program(Parser *parser) {
	Program *prog = program_create();
	int decl_loop_count = 0;
	const int MAX_DECL_LOOP = 10000;

	while (!check(parser, TOK_EOF)) {
		if (++decl_loop_count > MAX_DECL_LOOP) {
			parser->had_error = 1;
			break;
		}
		/* Pending trivia accumulated since the previous decl (or program start)
		 * becomes the leading trivia of THIS decl. */
		Trivia *leading = NULL;
		int leading_count = 0;
		take_pending_as_leading(parser, &leading, &leading_count);

		Decl *decl = parse_decl(parser);
		if (!decl) {
			free(leading);
			synchronize(parser);
			continue;
		}
		decl->leading_trivia = leading;
		decl->leading_count = leading_count;
		decl->last_line = parser->previous.line;
		/* Inline-trailing comment on the decl's last line is stolen back from
		 * pending so the NEXT decl doesn't pick it up as leading. */
		steal_trailing_inline(parser, decl->last_line, &decl->trailing_trivia, &decl->trailing_count);

		prog->decls = realloc(prog->decls, (prog->decl_count + 1) * sizeof(Decl *));
		prog->decls[prog->decl_count++] = decl;
	}

	ParseResult result;
	result.ast = prog;
	result.errors = parser->errors;
	result.error_count = parser->error_count;
	result.comments = NULL;
	result.comment_count = 0;
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
		result->errors = NULL;
		result->error_count = 0;
		result->comments = NULL;
		result->comment_count = 0;
	}
}
