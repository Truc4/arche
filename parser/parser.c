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
	Token *comments;
	size_t comment_count;
	size_t comment_cap;
	int recursion_depth;
};

/* ========== UTILITY FUNCTIONS ========== */

static void advance(Parser *parser) {
	parser->previous = parser->current;
	parser->current = lexer_next_token(parser->lexer);

	/* Capture comment tokens instead of skipping */
	int comment_loop_count = 0;
	const int MAX_COMMENT_LOOP = 1000;
	while (parser->current.kind == TOK_COMMENT) {
		if (++comment_loop_count > MAX_COMMENT_LOOP) {
			parser->had_error = 1;
			parser->current.kind = TOK_EOF;
			break;
		}
		if (parser->comment_count >= parser->comment_cap) {
			parser->comment_cap = (parser->comment_cap == 0) ? 16 : parser->comment_cap * 2;
			parser->comments = realloc(parser->comments, parser->comment_cap * sizeof(Token));
		}
		parser->comments[parser->comment_count++] = parser->current;
		parser->current = lexer_next_token(parser->lexer);
	}

	if (parser->current.kind == TOK_ERROR) {
		parser->had_error = 1;
	}
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

	TypeRef *type = type_name_create(name);
	type->loc.line = parser->previous.line;
	type->loc.column = parser->previous.column;

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

	if (!match(parser, TOK_COLON)) {
		error(parser, "Expected ':'");
		free(name_copy);
		*out_count = 0;
		return NULL;
	}

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
		int field_count = 0;
		FieldDecl **fields = parse_arch_field_expanded(parser, &field_count);
		if (!fields || field_count == 0) {
			synchronize(parser);
			continue;
		}

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

	if (match(parser, TOK_EXTERN)) {
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

	Decl *decl = decl_create(DECL_SYS);
	decl->data.sys = sys;
	return decl;
}

/* ========== FUNCTION PARSING ========== */

static Decl *parse_func_decl(Parser *parser) {
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

	if (!match(parser, TOK_LPAREN)) {
		error(parser, "Expected '('");
		return NULL;
	}

	FuncDecl *func = func_decl_create(name, NULL);
	func->loc.line = parser->previous.line;
	func->loc.column = parser->previous.column;

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

	TypeRef *return_type = parse_type(parser);
	if (!return_type)
		return NULL;
	func->return_type = return_type;

	if (!match(parser, TOK_LBRACE)) {
		error(parser, "Expected '{'");
		return NULL;
	}

	/* parse return expression */
	Expression *return_expr = parse_expression(parser);
	if (return_expr) {
		Statement *stmt = statement_create(STMT_EXPR);
		stmt->data.expr_stmt.expr = return_expr;
		func->statements = realloc(func->statements, (func->statement_count + 1) * sizeof(Statement *));
		func->statements[func->statement_count++] = stmt;
	}

	if (!match(parser, TOK_RBRACE)) {
		error(parser, "Expected '}'");
	}

	Decl *decl = decl_create(DECL_FUNC);
	decl->data.func = func;
	return decl;
}

/* ========== DECLARATION PARSING ========== */

/* ========== WORLD PARSING ========== */

static Decl *parse_decl(Parser *parser) {

	switch (parser->current.kind) {
	case TOK_ARCHETYPE:
		return parse_archetype_decl(parser);
	case TOK_EXTERN:
	case TOK_PROC:
		return parse_proc_decl(parser);
	case TOK_SYS:
		return parse_sys_decl(parser);
	case TOK_FUNC:
		return parse_func_decl(parser);
	default:
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
		char *lexeme = token_text(parser->current);
		advance(parser);

		Expression *expr = expression_create(EXPR_LITERAL);
		expr->loc.line = parser->previous.line;
		expr->loc.column = parser->previous.column;
		expr->data.literal.lexeme = lexeme;
		return expr;
	}

	if (check(parser, TOK_CHAR_LIT)) {
		char char_buf[32];
		snprintf(char_buf, sizeof(char_buf), "%d", parser->current.int_val);
		char *lexeme = malloc(strlen(char_buf) + 1);
		strcpy(lexeme, char_buf);
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
			} while (match(parser, TOK_COMMA));
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

		/* check for alloc expression */
		if (strcmp(name, "alloc") == 0) {
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

				/* Parse optional field init block: { field: value, ... } */
				if (match(parser, TOK_LBRACE)) {
					if (!check(parser, TOK_RBRACE)) {
						/* Parse field initialization pairs */
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

							/* Reallocate and add the new field */
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

static Expression *parse_binary_expr_with_left(Parser *parser, Expression *left) {
	/* Continue parsing binary expression from a given left operand */
	if (!left)
		return NULL;

	while (check(parser, TOK_PLUS) || check(parser, TOK_MINUS) || check(parser, TOK_STAR) || check(parser, TOK_SLASH) ||
	       check(parser, TOK_EQ_EQ) || check(parser, TOK_BANG_EQ) || check(parser, TOK_LT) || check(parser, TOK_GT) ||
	       check(parser, TOK_LT_EQ) || check(parser, TOK_GT_EQ)) {

		TokenKind op_kind = parser->current.kind;
		advance(parser);

		Expression *right = parse_primary_expr(parser);
		if (!right)
			return NULL;

		Operator op;
		switch (op_kind) {
		case TOK_PLUS:
			op = OP_ADD;
			break;
		case TOK_MINUS:
			op = OP_SUB;
			break;
		case TOK_STAR:
			op = OP_MUL;
			break;
		case TOK_SLASH:
			op = OP_DIV;
			break;
		case TOK_EQ_EQ:
			op = OP_EQ;
			break;
		case TOK_BANG_EQ:
			op = OP_NEQ;
			break;
		case TOK_LT:
			op = OP_LT;
			break;
		case TOK_GT:
			op = OP_GT;
			break;
		case TOK_LT_EQ:
			op = OP_LTE;
			break;
		case TOK_GT_EQ:
			op = OP_GTE;
			break;
		default:
			op = OP_ADD;
			break;
		}

		Expression *binary = expression_create(EXPR_BINARY);
		binary->loc.line = left->loc.line;
		binary->loc.column = left->loc.column;
		binary->data.binary.op = op;
		binary->data.binary.left = left;
		binary->data.binary.right = right;

		left = binary;
	}

	return left;
}

static Expression *parse_binary_expr(Parser *parser) {
	Expression *left = parse_primary_expr(parser);
	return parse_binary_expr_with_left(parser, left);
}

static Expression *parse_expression(Parser *parser) {
	return parse_binary_expr(parser);
}

/* ========== STATEMENT PARSING ========== */

static Statement *parse_statement(Parser *parser) {
	if (match(parser, TOK_SEMI)) {
		return NULL; /* empty statement */
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
		return stmt;
	}

	/* check for run statement */
	if (check(parser, TOK_IDENT)) {
		const char *text = parser->current.start;
		size_t len = parser->current.length;
		if (len == 3 && strncmp(text, "run", 3) == 0) {
			advance(parser); /* consume 'run' */

			if (!check(parser, TOK_IDENT)) {
				error(parser, "Expected system name");
				return NULL;
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
			return stmt;
		}
	}

	if (match(parser, TOK_LET)) {
		int let_line = parser->previous.line;
		int let_column = parser->previous.column;

		if (!check(parser, TOK_IDENT)) {
			error(parser, "Expected variable name after 'let'");
			return NULL;
		}

		char *name = token_text(parser->current);
		advance(parser);

		TypeRef *type = NULL;
		if (match(parser, TOK_COLON)) {
			type = parse_type(parser);
			if (!type)
				return NULL;
		}

		Expression *value = NULL;
		if (match(parser, TOK_EQ)) {
			value = parse_expression(parser);
			if (!value)
				return NULL;
		} else if (!type) {
			error(parser, "Expected '=' or type annotation after variable name");
			return NULL;
		}

		if (!match(parser, TOK_SEMI)) {
			error(parser, "Expected ';' after let statement");
		}

		Statement *stmt = statement_create(STMT_LET);
		stmt->loc.line = let_line;
		stmt->loc.column = let_column;
		stmt->data.let_stmt.name = name;
		stmt->data.let_stmt.type = type;
		stmt->data.let_stmt.value = value;
		return stmt;
	}

	if (match(parser, TOK_IF)) {
		int if_line = parser->previous.line;
		int if_column = parser->previous.column;

		if (!match(parser, TOK_LPAREN)) {
			error(parser, "Expected '(' after 'if'");
			return NULL;
		}

		Expression *cond = parse_expression(parser);
		if (!cond)
			return NULL;

		if (!match(parser, TOK_RPAREN)) {
			error(parser, "Expected ')' after if condition");
			return NULL;
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

		return stmt;
	}

	if (match(parser, TOK_FOR)) {
		int for_line = parser->previous.line;
		int for_column = parser->previous.column;

		Statement *stmt = statement_create(STMT_FOR);
		stmt->loc.line = for_line;
		stmt->loc.column = for_column;
		stmt->data.for_stmt.var_name = NULL;
		stmt->data.for_stmt.iterable = NULL;
		stmt->data.for_stmt.condition = NULL;
		stmt->data.for_stmt.body = NULL;
		stmt->data.for_stmt.body_count = 0;

		/* Check for infinite for: for { } */
		if (match(parser, TOK_LBRACE)) {
			/* Infinite loop - no var_name, iterable, or condition (already set to NULL above) */
		} else {
			/* Range-based for: for var in iterable { } */
			if (!check(parser, TOK_IDENT)) {
				error(parser, "Expected variable name after 'for'");
				return NULL;
			}

			char *var_name = token_text(parser->current);
			advance(parser);

			if (!match(parser, TOK_IN)) {
				error(parser, "Expected 'in' in for loop");
				return NULL;
			}

			if (!check(parser, TOK_IDENT)) {
				error(parser, "Expected iterable after 'in'");
				return NULL;
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
				return NULL;
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

		return stmt;
	}

	/* try to parse as assignment */
	Expression *target = parse_expression(parser);
	if (!target)
		return NULL;

	/* check for assignment operators */
	if (check(parser, TOK_EQ) || check(parser, TOK_PLUS_EQ) || check(parser, TOK_MINUS_EQ) ||
	    check(parser, TOK_STAR_EQ) || check(parser, TOK_SLASH_EQ)) {

		/* capture the operator before consuming it */
		TokenKind op_token = parser->current.kind;
		advance(parser); /* consume assignment operator */

		Expression *value = parse_expression(parser);
		if (!value)
			return NULL;

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
		return stmt;
	}

	/* otherwise it was an expression statement */
	if (!match(parser, TOK_SEMI)) {
		error(parser, "Expected ';' after expression statement");
	}

	Statement *stmt = statement_create(STMT_EXPR);
	stmt->loc.line = target->loc.line;
	stmt->loc.column = target->loc.column;
	stmt->data.expr_stmt.expr = target;
	return stmt;
}

/* ========== MAIN PARSER ========== */

static void parser_init(Parser *parser, Lexer *lexer) {
	parser->lexer = lexer;
	parser->had_error = 0;
	parser->panic_mode = 0;
	parser->errors = NULL;
	parser->error_count = 0;
	parser->error_cap = 0;
	parser->comments = NULL;
	parser->comment_count = 0;
	parser->comment_cap = 0;
	parser->recursion_depth = 0;
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
		Decl *decl = parse_decl(parser);
		if (!decl) {
			synchronize(parser);
			continue;
		}

		prog->decls = realloc(prog->decls, (prog->decl_count + 1) * sizeof(Decl *));
		prog->decls[prog->decl_count++] = decl;
	}

	ParseResult result;
	result.ast = prog;
	result.errors = parser->errors;
	result.error_count = parser->error_count;
	result.comments = parser->comments;
	result.comment_count = parser->comment_count;
	parser->errors = NULL;
	parser->error_count = 0;
	parser->comments = NULL;
	parser->comment_count = 0;
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
		free(parser->comments);
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
