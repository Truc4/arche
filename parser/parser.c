#include "parser.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ========== UTILITY FUNCTIONS ========== */

static void advance(Parser *parser) {
	parser->previous = parser->current;
	parser->current = lexer_next_token(parser->lexer);

	if (parser->current.kind == TOK_ERROR) {
		parser->had_error = 1;
	}
}

static int check(Parser *parser, TokenKind kind) {
	return parser->current.kind == kind;
}

static int match(Parser *parser, TokenKind kind) {
	if (!check(parser, kind)) return 0;
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
	if (parser->panic_mode) return;
	parser->panic_mode = 1;
	parser->had_error = 1;

	fprintf(stderr, "[Line %d, Col %d] Error: %s\n",
		parser->current.line, parser->current.column, msg);
}

static void synchronize(Parser *parser) {
	parser->panic_mode = 0;

	while (parser->current.kind != TOK_EOF) {
		if (parser->previous.kind == TOK_SEMI) return;

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

	return type_name_create(name);
}

/* ========== ARCHETYPE PARSING ========== */

static FieldDecl *parse_arch_field(Parser *parser) {
	FieldKind kind;

	if (match(parser, TOK_META)) {
		kind = FIELD_META;
	} else if (match(parser, TOK_COL)) {
		kind = FIELD_COLUMN;
	} else {
		error(parser, "Expected 'meta' or 'col'");
		return NULL;
	}

	if (!check(parser, TOK_IDENT)) {
		error(parser, "Expected field name");
		return NULL;
	}
	char *name = token_text(parser->current);
	advance(parser);

	if (!match(parser, TOK_COLON)) {
		error(parser, "Expected ':'");
		return NULL;
	}

	TypeRef *type = parse_type(parser);
	if (!type) return NULL;

	/* trailing comma is optional */
	match(parser, TOK_COMMA);

	return field_decl_create(kind, name, type);
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

	while (!check(parser, TOK_RBRACE) && !check(parser, TOK_EOF)) {
		FieldDecl *field = parse_arch_field(parser);
		if (!field) {
			synchronize(parser);
			continue;
		}

		/* grow the fields array */
		arch->fields = realloc(arch->fields, (arch->field_count + 1) * sizeof(FieldDecl *));
		arch->fields[arch->field_count++] = field;
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

	if (!match(parser, TOK_RPAREN)) {
		error(parser, "Expected ')' (procedures don't take parameters yet)");
		return NULL;
	}

	if (!match(parser, TOK_LBRACE)) {
		error(parser, "Expected '{'");
		return NULL;
	}

	ProcDecl *proc = proc_decl_create(name);

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

	/* parse parameters */
	if (!check(parser, TOK_RPAREN)) {
		do {
			if (!check(parser, TOK_IDENT)) {
				error(parser, "Expected parameter name");
				return NULL;
			}

			char *param_name = token_text(parser->current);
			advance(parser);

			Parameter *param = parameter_create(param_name, NULL);
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

	/* parse parameters */
	if (!check(parser, TOK_RPAREN)) {
		do {
			if (!check(parser, TOK_IDENT)) {
				error(parser, "Expected parameter name");
				return NULL;
			}

			char *param_name = token_text(parser->current);
			advance(parser);

			if (!match(parser, TOK_COLON)) {
				error(parser, "Expected ':' after parameter name");
				return NULL;
			}

			TypeRef *param_type = parse_type(parser);
			if (!param_type) return NULL;

			Parameter *param = parameter_create(param_name, param_type);
			func->params = realloc(func->params, (func->param_count + 1) * sizeof(Parameter *));
			func->params[func->param_count++] = param;
		} while (match(parser, TOK_COMMA));
	}

	if (!match(parser, TOK_RPAREN)) {
		error(parser, "Expected ')'");
		return NULL;
	}

	if (!match(parser, TOK_MINUS)) {
		error(parser, "Expected '-' in '->'");
		return NULL;
	}

	if (!match(parser, TOK_GT)) {
		error(parser, "Expected '>' in '->'");
		return NULL;
	}

	TypeRef *return_type = parse_type(parser);
	if (!return_type) return NULL;
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

static Decl *parse_world_decl(Parser *parser) {
	/* 'world' is the current token, consume it */
	advance(parser); /* consume 'world' */

	if (!check(parser, TOK_IDENT)) {
		error(parser, "Expected world name");
		return NULL;
	}
	char *name = token_text(parser->current);
	advance(parser);

	if (!match(parser, TOK_LPAREN)) {
		error(parser, "Expected '('");
		return NULL;
	}

	if (!match(parser, TOK_RPAREN)) {
		error(parser, "Expected ')' (world fields not yet supported)");
		return NULL;
	}

	WorldDecl *world = world_decl_create(name);
	Decl *decl = decl_create(DECL_WORLD);
	decl->data.world = world;
	return decl;
}

static Decl *parse_decl(Parser *parser) {
	/* check for world keyword first */
	if (check(parser, TOK_IDENT)) {
		/* peek ahead to see if this is 'world' */
		const char *text = parser->current.start;
		size_t len = parser->current.length;
		if (len == 5 && strncmp(text, "world", 5) == 0) {
			return parse_world_decl(parser);
		}
	}

	switch (parser->current.kind) {
	case TOK_ARCHETYPE:
		return parse_archetype_decl(parser);
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
		expr->data.literal.lexeme = lexeme;
		return expr;
	}

	if (check(parser, TOK_STRING)) {
		char *lexeme = token_text(parser->current);
		advance(parser);

		Expression *expr = expression_create(EXPR_LITERAL);
		expr->data.literal.lexeme = lexeme;
		return expr;
	}

	if (check(parser, TOK_IDENT)) {
		char *name = token_text(parser->current);
		advance(parser);

		/* check for field access or indexing */
		if (match(parser, TOK_DOT)) {
			if (!check(parser, TOK_IDENT)) {
				error(parser, "Expected field name after '.'");
				free(name);
				return NULL;
			}

			char *field_name = token_text(parser->current);
			advance(parser);

			Expression *base = expression_create(EXPR_NAME);
			base->data.name.name = name;

			Expression *field = expression_create(EXPR_FIELD);
			field->data.field.base = base;
			field->data.field.field_name = field_name;

			/* check for indexing on the field */
			if (match(parser, TOK_LBRACKET)) {
				Expression *index = expression_create(EXPR_INDEX);
				index->data.index.base = field;
				index->data.index.indices = NULL;
				index->data.index.index_count = 0;

				do {
					Expression *idx_expr = parse_expression(parser);
					if (!idx_expr) return NULL;

					index->data.index.indices = realloc(
						index->data.index.indices,
						(index->data.index.index_count + 1) * sizeof(Expression *)
					);
					index->data.index.indices[index->data.index.index_count++] = idx_expr;
				} while (match(parser, TOK_COMMA));

				if (!match(parser, TOK_RBRACKET)) {
					error(parser, "Expected ']'");
					return NULL;
				}

				return index;
			}

			return field;
		}

		/* check for indexing */
		if (match(parser, TOK_LBRACKET)) {
			Expression *index = expression_create(EXPR_INDEX);

			Expression *base = expression_create(EXPR_NAME);
			base->data.name.name = name;
			index->data.index.base = base;
			index->data.index.indices = NULL;
			index->data.index.index_count = 0;

			do {
				Expression *idx_expr = parse_expression(parser);
				if (!idx_expr) return NULL;

				index->data.index.indices = realloc(
					index->data.index.indices,
					(index->data.index.index_count + 1) * sizeof(Expression *)
				);
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
			callee->data.name.name = name;
			expr->data.call.callee = callee;
			expr->data.call.args = NULL;
			expr->data.call.arg_count = 0;

			/* parse arguments */
			if (!check(parser, TOK_RPAREN)) {
				do {
					Expression *arg = parse_expression(parser);
					if (!arg) return NULL;

					expr->data.call.args = realloc(
						expr->data.call.args,
						(expr->data.call.arg_count + 1) * sizeof(Expression *)
					);
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

static Expression *parse_binary_expr(Parser *parser) {
	Expression *left = parse_primary_expr(parser);
	if (!left) return NULL;

	while (check(parser, TOK_PLUS) || check(parser, TOK_MINUS) ||
	       check(parser, TOK_STAR) || check(parser, TOK_SLASH) ||
	       check(parser, TOK_EQ_EQ) || check(parser, TOK_BANG_EQ) ||
	       check(parser, TOK_LT) || check(parser, TOK_GT) ||
	       check(parser, TOK_LT_EQ) || check(parser, TOK_GT_EQ)) {

		TokenKind op_kind = parser->current.kind;
		advance(parser);

		Expression *right = parse_primary_expr(parser);
		if (!right) return NULL;

		Operator op;
		switch (op_kind) {
		case TOK_PLUS:    op = OP_ADD; break;
		case TOK_MINUS:   op = OP_SUB; break;
		case TOK_STAR:    op = OP_MUL; break;
		case TOK_SLASH:   op = OP_DIV; break;
		case TOK_EQ_EQ:   op = OP_EQ; break;
		case TOK_BANG_EQ: op = OP_NEQ; break;
		case TOK_LT:      op = OP_LT; break;
		case TOK_GT:      op = OP_GT; break;
		case TOK_LT_EQ:   op = OP_LTE; break;
		case TOK_GT_EQ:   op = OP_GTE; break;
		default:          op = OP_ADD; break;
		}

		Expression *binary = expression_create(EXPR_BINARY);
		binary->data.binary.op = op;
		binary->data.binary.left = left;
		binary->data.binary.right = right;

		left = binary;
	}

	return left;
}

static Expression *parse_expression(Parser *parser) {
	return parse_binary_expr(parser);
}

/* ========== STATEMENT PARSING ========== */

static Statement *parse_statement(Parser *parser) {
	if (match(parser, TOK_SEMI)) {
		return NULL; /* empty statement */
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

			if (!match(parser, TOK_IN)) {
				error(parser, "Expected 'in'");
				return NULL;
			}

			if (!check(parser, TOK_IDENT)) {
				error(parser, "Expected world name");
				return NULL;
			}
			char *world_name = token_text(parser->current);
			advance(parser);

			if (!match(parser, TOK_SEMI)) {
				error(parser, "Expected ';'");
			}

			Statement *stmt = statement_create(STMT_RUN);
			stmt->data.run_stmt.system_name = system_name;
			stmt->data.run_stmt.world_name = world_name;
			return stmt;
		}
	}

	if (match(parser, TOK_LET)) {
		if (!check(parser, TOK_IDENT)) {
			error(parser, "Expected variable name after 'let'");
			return NULL;
		}

		char *name = token_text(parser->current);
		advance(parser);

		if (!match(parser, TOK_EQ)) {
			error(parser, "Expected '=' after variable name");
			return NULL;
		}

		Expression *value = parse_expression(parser);
		if (!value) return NULL;

		if (!match(parser, TOK_SEMI)) {
			error(parser, "Expected ';' after expression");
		}

		Statement *stmt = statement_create(STMT_LET);
		stmt->data.let_stmt.name = name;
		stmt->data.let_stmt.type = NULL;
		stmt->data.let_stmt.value = value;
		return stmt;
	}

	if (match(parser, TOK_FOR)) {
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
		iterable->data.name.name = iterable_name;

		if (!match(parser, TOK_LBRACE)) {
			error(parser, "Expected '{'");
			return NULL;
		}

		Statement *stmt = statement_create(STMT_FOR);
		stmt->data.for_stmt.var_name = var_name;
		stmt->data.for_stmt.iterable = iterable;
		stmt->data.for_stmt.body = NULL;
		stmt->data.for_stmt.body_count = 0;

		while (!check(parser, TOK_RBRACE) && !check(parser, TOK_EOF)) {
			Statement *body_stmt = parse_statement(parser);
			if (!body_stmt) {
				synchronize(parser);
				continue;
			}

			stmt->data.for_stmt.body = realloc(
				stmt->data.for_stmt.body,
				(stmt->data.for_stmt.body_count + 1) * sizeof(Statement *)
			);
			stmt->data.for_stmt.body[stmt->data.for_stmt.body_count++] = body_stmt;
		}

		if (!match(parser, TOK_RBRACE)) {
			error(parser, "Expected '}'");
		}

		return stmt;
	}

	/* try to parse as assignment */
	Expression *target = parse_expression(parser);
	if (!target) return NULL;

	/* check for assignment operators */
	if (check(parser, TOK_EQ) || check(parser, TOK_PLUS_EQ) ||
	    check(parser, TOK_MINUS_EQ) || check(parser, TOK_STAR_EQ) ||
	    check(parser, TOK_SLASH_EQ)) {

		advance(parser); /* consume assignment operator */

		Expression *value = parse_expression(parser);
		if (!value) return NULL;

		if (!match(parser, TOK_SEMI)) {
			error(parser, "Expected ';' after assignment");
		}

		Statement *stmt = statement_create(STMT_ASSIGN);
		stmt->data.assign_stmt.target = target;
		stmt->data.assign_stmt.value = value;
		return stmt;
	}

	/* otherwise it was an expression statement */
	if (!match(parser, TOK_SEMI)) {
		error(parser, "Expected ';' after expression statement");
	}

	Statement *stmt = statement_create(STMT_EXPR);
	stmt->data.expr_stmt.expr = target;
	return stmt;
}

/* ========== MAIN PARSER ========== */

void parser_init(Parser *parser, Lexer *lexer) {
	parser->lexer = lexer;
	parser->had_error = 0;
	parser->panic_mode = 0;
	advance(parser);
}

Program *parse_program(Parser *parser) {
	Program *prog = program_create();

	while (!check(parser, TOK_EOF) && !parser->had_error) {
		Decl *decl = parse_decl(parser);
		if (!decl) {
			synchronize(parser);
			continue;
		}

		prog->decls = realloc(prog->decls, (prog->decl_count + 1) * sizeof(Decl *));
		prog->decls[prog->decl_count++] = decl;
	}

	return prog;
}
