#include "ast.h"
#include <stdlib.h>
#include <string.h>

/* =========================
   Program / Declarations
   ========================= */

Program *program_create(void) {
	Program *prog = malloc(sizeof(Program));
	prog->decls = NULL;
	prog->decl_count = 0;
	prog->loc.line = 1;
	prog->loc.column = 1;
	return prog;
}

Decl *decl_create(DeclKind kind) {
	Decl *decl = malloc(sizeof(Decl));
	decl->kind = kind;
	decl->loc.line = 1;
	decl->loc.column = 1;
	return decl;
}

/* =========================
   Archetyped / Proc / Sys / Func
   ========================= */

WorldDecl *world_decl_create(char *name) {
	WorldDecl *world = malloc(sizeof(WorldDecl));
	world->name = name;
	world->field_names = NULL;
	world->field_count = 0;
	world->loc.line = 1;
	world->loc.column = 1;
	return world;
}

ArchetypeDecl *archetype_decl_create(char *name) {
	ArchetypeDecl *arch = malloc(sizeof(ArchetypeDecl));
	arch->name = name;
	arch->fields = NULL;
	arch->field_count = 0;
	arch->loc.line = 1;
	arch->loc.column = 1;
	return arch;
}

ProcDecl *proc_decl_create(char *name) {
	ProcDecl *proc = malloc(sizeof(ProcDecl));
	proc->name = name;
	proc->params = NULL;
	proc->param_count = 0;
	proc->is_extern = 0;
	proc->statements = NULL;
	proc->statement_count = 0;
	proc->loc.line = 1;
	proc->loc.column = 1;
	return proc;
}

SysDecl *sys_decl_create(char *name) {
	SysDecl *sys = malloc(sizeof(SysDecl));
	sys->name = name;
	sys->params = NULL;
	sys->param_count = 0;
	sys->statements = NULL;
	sys->statement_count = 0;
	sys->loc.line = 1;
	sys->loc.column = 1;
	return sys;
}

FuncDecl *func_decl_create(char *name, TypeRef *return_type) {
	FuncDecl *func = malloc(sizeof(FuncDecl));
	func->name = name;
	func->return_type = return_type;
	func->params = NULL;
	func->param_count = 0;
	func->statements = NULL;
	func->statement_count = 0;
	func->loc.line = 1;
	func->loc.column = 1;
	return func;
}

Parameter *parameter_create(char *name, TypeRef *type) {
	Parameter *param = malloc(sizeof(Parameter));
	param->name = name;
	param->type = type;
	param->loc.line = 1;
	param->loc.column = 1;
	return param;
}

FieldDecl *field_decl_create(FieldKind kind, char *name, TypeRef *type) {
	FieldDecl *field = malloc(sizeof(FieldDecl));
	field->kind = kind;
	field->name = name;
	field->type = type;
	field->loc.line = 1;
	field->loc.column = 1;
	return field;
}

/* =========================
   Types
   ========================= */

TypeRef *type_name_create(char *name) {
	TypeRef *type = malloc(sizeof(TypeRef));
	type->kind = TYPE_NAME;
	type->data.name = name;
	type->loc.line = 1;
	type->loc.column = 1;
	return type;
}

TypeRef *type_array_create(TypeRef *element_type) {
	TypeRef *type = malloc(sizeof(TypeRef));
	type->kind = TYPE_ARRAY;
	type->data.array.element_type = element_type;
	type->loc.line = 1;
	type->loc.column = 1;
	return type;
}

TypeRef *type_shaped_array_create(TypeRef *element_type, int rank) {
	TypeRef *type = malloc(sizeof(TypeRef));
	type->kind = TYPE_SHAPED_ARRAY;
	type->data.shaped_array.element_type = element_type;
	type->data.shaped_array.rank = rank;
	type->loc.line = 1;
	type->loc.column = 1;
	return type;
}

/* =========================
   Statements / Expressions
   ========================= */

Statement *statement_create(StatementType type) {
	Statement *stmt = calloc(1, sizeof(Statement));
	stmt->type = type;
	stmt->loc.line = 1;
	stmt->loc.column = 1;
	return stmt;
}

Expression *expression_create(ExpressionType type) {
	Expression *expr = malloc(sizeof(Expression));
	expr->type = type;
	expr->loc.line = 1;
	expr->loc.column = 1;
	return expr;
}

/* =========================
   Destructors
   ========================= */

void program_free(Program *prog) {
	if (!prog)
		return;
	for (int i = 0; i < prog->decl_count; i++) {
		decl_free(prog->decls[i]);
	}
	free(prog->decls);
	free(prog);
}

void decl_free(Decl *decl) {
	if (!decl)
		return;
	switch (decl->kind) {
	case DECL_WORLD:
		world_decl_free(decl->data.world);
		break;
	case DECL_ARCHETYPE:
		archetype_decl_free(decl->data.archetype);
		break;
	case DECL_PROC:
		proc_decl_free(decl->data.proc);
		break;
	case DECL_SYS:
		sys_decl_free(decl->data.sys);
		break;
	case DECL_FUNC:
		func_decl_free(decl->data.func);
		break;
	}
	free(decl);
}

void world_decl_free(WorldDecl *world) {
	if (!world)
		return;
	free(world->name);
	for (int i = 0; i < world->field_count; i++) {
		free(world->field_names[i]);
	}
	free(world->field_names);
	free(world);
}

void archetype_decl_free(ArchetypeDecl *archetype) {
	if (!archetype)
		return;
	free(archetype->name);
	for (int i = 0; i < archetype->field_count; i++) {
		field_decl_free(archetype->fields[i]);
	}
	free(archetype->fields);
	free(archetype);
}

void proc_decl_free(ProcDecl *proc) {
	if (!proc)
		return;
	free(proc->name);
	for (int i = 0; i < proc->param_count; i++) {
		parameter_free(proc->params[i]);
	}
	free(proc->params);
	for (int i = 0; i < proc->statement_count; i++) {
		statement_free(proc->statements[i]);
	}
	free(proc->statements);
	free(proc);
}

void sys_decl_free(SysDecl *sys) {
	if (!sys)
		return;
	free(sys->name);
	for (int i = 0; i < sys->param_count; i++) {
		parameter_free(sys->params[i]);
	}
	free(sys->params);
	for (int i = 0; i < sys->statement_count; i++) {
		statement_free(sys->statements[i]);
	}
	free(sys->statements);
	free(sys);
}

void func_decl_free(FuncDecl *func) {
	if (!func)
		return;
	free(func->name);
	type_ref_free(func->return_type);
	for (int i = 0; i < func->param_count; i++) {
		parameter_free(func->params[i]);
	}
	free(func->params);
	for (int i = 0; i < func->statement_count; i++) {
		statement_free(func->statements[i]);
	}
	free(func->statements);
	free(func);
}

void parameter_free(Parameter *param) {
	if (!param)
		return;
	free(param->name);
	type_ref_free(param->type);
	free(param);
}

void field_decl_free(FieldDecl *field) {
	if (!field)
		return;
	free(field->name);
	type_ref_free(field->type);
	free(field);
}

void type_ref_free(TypeRef *type) {
	if (!type)
		return;
	switch (type->kind) {
	case TYPE_NAME:
		free(type->data.name);
		break;
	case TYPE_ARRAY:
		type_ref_free(type->data.array.element_type);
		break;
	case TYPE_SHAPED_ARRAY:
		type_ref_free(type->data.shaped_array.element_type);
		break;
	case TYPE_TUPLE:
		for (int i = 0; i < type->data.tuple.field_count; i++) {
			free(type->data.tuple.field_names[i]);
			type_ref_free(type->data.tuple.field_types[i]);
		}
		free(type->data.tuple.field_names);
		free(type->data.tuple.field_types);
		break;
	}
	free(type);
}

void statement_free(Statement *stmt) {
	if (!stmt)
		return;
	switch (stmt->type) {
	case STMT_LET:
		free(stmt->data.let_stmt.name);
		type_ref_free(stmt->data.let_stmt.type);
		expression_free(stmt->data.let_stmt.value);
		break;
	case STMT_ASSIGN:
		expression_free(stmt->data.assign_stmt.target);
		expression_free(stmt->data.assign_stmt.value);
		break;
	case STMT_FOR:
		free(stmt->data.for_stmt.var_name);
		if (stmt->data.for_stmt.init)
			statement_free(stmt->data.for_stmt.init);
		expression_free(stmt->data.for_stmt.condition);
		expression_free(stmt->data.for_stmt.iterable);
		if (stmt->data.for_stmt.increment)
			statement_free(stmt->data.for_stmt.increment);
		for (int i = 0; i < stmt->data.for_stmt.body_count; i++) {
			statement_free(stmt->data.for_stmt.body[i]);
		}
		free(stmt->data.for_stmt.body);
		break;
	case STMT_IF:
		expression_free(stmt->data.if_stmt.cond);
		for (int i = 0; i < stmt->data.if_stmt.then_count; i++) {
			statement_free(stmt->data.if_stmt.then_body[i]);
		}
		free(stmt->data.if_stmt.then_body);
		for (int i = 0; i < stmt->data.if_stmt.else_count; i++) {
			statement_free(stmt->data.if_stmt.else_body[i]);
		}
		free(stmt->data.if_stmt.else_body);
		break;
	case STMT_RUN:
		free(stmt->data.run_stmt.system_name);
		free(stmt->data.run_stmt.world_name);
		break;
	case STMT_EXPR:
		expression_free(stmt->data.expr_stmt.expr);
		break;
	case STMT_FREE:
		expression_free(stmt->data.free_stmt.value);
		break;
	}
	free(stmt);
}

void expression_free(Expression *expr) {
	if (!expr)
		return;
	switch (expr->type) {
	case EXPR_LITERAL:
		free((char *)expr->data.literal.lexeme);
		break;
	case EXPR_NAME:
		free(expr->data.name.name);
		break;
	case EXPR_FIELD:
		expression_free(expr->data.field.base);
		free(expr->data.field.field_name);
		break;
	case EXPR_INDEX:
		expression_free(expr->data.index.base);
		for (int i = 0; i < expr->data.index.index_count; i++) {
			expression_free(expr->data.index.indices[i]);
		}
		free(expr->data.index.indices);
		break;
	case EXPR_BINARY:
		expression_free(expr->data.binary.left);
		expression_free(expr->data.binary.right);
		break;
	case EXPR_UNARY:
		expression_free(expr->data.unary.operand);
		break;
	case EXPR_CALL:
		expression_free(expr->data.call.callee);
		for (int i = 0; i < expr->data.call.arg_count; i++) {
			expression_free(expr->data.call.args[i]);
		}
		free(expr->data.call.args);
		break;
	case EXPR_ALLOC:
		free(expr->data.alloc.archetype_name);
		for (int i = 0; i < expr->data.alloc.field_count; i++) {
			free(expr->data.alloc.field_names[i]);
			expression_free(expr->data.alloc.field_values[i]);
		}
		free(expr->data.alloc.field_names);
		free(expr->data.alloc.field_values);
		break;
	case EXPR_ARRAY_LITERAL:
		for (int i = 0; i < expr->data.array_literal.element_count; i++) {
			expression_free(expr->data.array_literal.elements[i]);
		}
		free(expr->data.array_literal.elements);
		break;
	}
	free(expr);
}

/* =========================
   Formatting / Pretty-printing
   ========================= */

#include <stdio.h>

/* Recursion depth guard for type formatting */
#define MAX_FORMAT_DEPTH 100
static int format_type_depth = 0;

static void format_type(FILE *out, TypeRef *type) {
	if (!type)
		return;

	/* Prevent infinite recursion on malformed types */
	if (format_type_depth >= MAX_FORMAT_DEPTH) {
		fprintf(out, "...");
		return;
	}

	format_type_depth++;
	switch (type->kind) {
	case TYPE_NAME:
		fprintf(out, "%s", type->data.name);
		break;
	case TYPE_ARRAY:
		format_type(out, type->data.array.element_type);
		fprintf(out, "[]");
		break;
	case TYPE_SHAPED_ARRAY:
		format_type(out, type->data.shaped_array.element_type);
		fprintf(out, "[%d]", type->data.shaped_array.rank);
		break;
	case TYPE_TUPLE:
		fprintf(out, "(");
		for (int i = 0; i < type->data.tuple.field_count; i++) {
			if (i > 0)
				fprintf(out, ", ");
			fprintf(out, "%s: ", type->data.tuple.field_names[i]);
			format_type(out, type->data.tuple.field_types[i]);
		}
		fprintf(out, ")");
		break;
	}
	format_type_depth--;
}

static void format_expression(FILE *out, Expression *expr);

static void format_expression(FILE *out, Expression *expr) {
	if (!expr)
		return;

	switch (expr->type) {
	case EXPR_LITERAL:
		fprintf(out, "%s", expr->data.literal.lexeme);
		break;
	case EXPR_NAME:
		fprintf(out, "%s", expr->data.name.name);
		break;
	case EXPR_FIELD: {
		format_expression(out, expr->data.field.base);
		fprintf(out, ".%s", expr->data.field.field_name);
		break;
	}
	case EXPR_INDEX: {
		format_expression(out, expr->data.index.base);
		fprintf(out, "[");
		for (int i = 0; i < expr->data.index.index_count; i++) {
			if (i > 0)
				fprintf(out, ", ");
			format_expression(out, expr->data.index.indices[i]);
		}
		fprintf(out, "]");
		break;
	}
	case EXPR_BINARY: {
		const char *op_str = "?";
		switch (expr->data.binary.op) {
		case OP_ADD:
			op_str = "+";
			break;
		case OP_SUB:
			op_str = "-";
			break;
		case OP_MUL:
			op_str = "*";
			break;
		case OP_DIV:
			op_str = "/";
			break;
		case OP_EQ:
			op_str = "==";
			break;
		case OP_NEQ:
			op_str = "!=";
			break;
		case OP_LT:
			op_str = "<";
			break;
		case OP_GT:
			op_str = ">";
			break;
		case OP_LTE:
			op_str = "<=";
			break;
		case OP_GTE:
			op_str = ">=";
			break;
		}
		format_expression(out, expr->data.binary.left);
		fprintf(out, " %s ", op_str);
		format_expression(out, expr->data.binary.right);
		break;
	}
	case EXPR_UNARY: {
		const char *op_str = expr->data.unary.op == UNARY_NEG ? "-" : "!";
		fprintf(out, "%s", op_str);
		format_expression(out, expr->data.unary.operand);
		break;
	}
	case EXPR_CALL: {
		format_expression(out, expr->data.call.callee);
		fprintf(out, "(");
		for (int i = 0; i < expr->data.call.arg_count; i++) {
			if (i > 0)
				fprintf(out, ", ");
			format_expression(out, expr->data.call.args[i]);
		}
		fprintf(out, ")");
		break;
	}
	case EXPR_ALLOC: {
		/* Check if this is a simple allocation (field_names[0] == NULL) or field assignment */
		if (expr->data.alloc.field_count == 1 && expr->data.alloc.field_names[0] == NULL) {
			/* Simple allocation: alloc Particle(count) */
			fprintf(out, "alloc %s(", expr->data.alloc.archetype_name);
			format_expression(out, expr->data.alloc.field_values[0]);
			fprintf(out, ")");
		} else {
			/* Field assignment: Particle.alloc(field1: val1, field2: val2, ...) */
			fprintf(out, "%s.alloc(", expr->data.alloc.archetype_name);
			for (int i = 0; i < expr->data.alloc.field_count; i++) {
				if (i > 0)
					fprintf(out, ", ");
				fprintf(out, "%s: ", expr->data.alloc.field_names[i]);
				format_expression(out, expr->data.alloc.field_values[i]);
			}
			fprintf(out, ")");
		}
		break;
	}
	case EXPR_ARRAY_LITERAL: {
		/* Check if this is an ASCII string (array of 8-255 values) */
		int is_string = 1;
		for (int i = 0; i < expr->data.array_literal.element_count; i++) {
			Expression *elem = expr->data.array_literal.elements[i];
			if (elem->type != EXPR_LITERAL) {
				is_string = 0;
				break;
			}
			/* Check if lexeme is a valid ASCII value (0-255, typically printable or whitespace) */
			const char *lexeme = elem->data.literal.lexeme;
			int val = atoi(lexeme);
			if (val < 0 || val > 255) {
				is_string = 0;
				break;
			}
		}

		if (is_string && expr->data.array_literal.element_count > 0) {
			/* Output as string literal */
			fprintf(out, "\"");
			for (int i = 0; i < expr->data.array_literal.element_count; i++) {
				int val = atoi(expr->data.array_literal.elements[i]->data.literal.lexeme);
				if (val == 10) {
					fprintf(out, "\\n");
				} else if (val == 13) {
					fprintf(out, "\\r");
				} else if (val == 9) {
					fprintf(out, "\\t");
				} else if (val == 34) {
					fprintf(out, "\\\"");
				} else if (val == 92) {
					fprintf(out, "\\\\");
				} else if (val >= 32 && val < 127) {
					fprintf(out, "%c", (char)val);
				} else {
					/* Non-printable character, output as octal */
					fprintf(out, "\\%03o", val);
				}
			}
			fprintf(out, "\"");
		} else {
			/* Output as regular array literal */
			fprintf(out, "{");
			for (int i = 0; i < expr->data.array_literal.element_count; i++) {
				if (i > 0)
					fprintf(out, ", ");
				format_expression(out, expr->data.array_literal.elements[i]);
			}
			fprintf(out, "}");
		}
		break;
	}
	case EXPR_STRING: {
		fprintf(out, "\"");
		if (expr->data.string.value) {
			for (int i = 0; i < expr->data.string.length; i++) {
				unsigned char c = (unsigned char)expr->data.string.value[i];
				if (c == '\n')
					fprintf(out, "\\n");
				else if (c == '\t')
					fprintf(out, "\\t");
				else if (c == '\r')
					fprintf(out, "\\r");
				else if (c == '\\')
					fprintf(out, "\\\\");
				else if (c == '"')
					fprintf(out, "\\\"");
				else if (c >= 32 && c < 127)
					fprintf(out, "%c", c);
				else
					fprintf(out, "\\x%02x", c);
			}
		}
		fprintf(out, "\"");
		break;
	}
	}
}

/* Context for tracking comment output during formatting */
typedef struct {
	Token *comments;
	size_t comment_count;
	size_t comment_idx;
	int last_line;
	const char *src;
} FmtCtx;

static void format_statement(FILE *out, Statement *stmt, int indent);

static void format_statement(FILE *out, Statement *stmt, int indent) {
	if (!stmt)
		return;

	char indent_str[256] = "";
	for (int i = 0; i < indent; i++) {
		strcat(indent_str, "  ");
	}

	switch (stmt->type) {
	case STMT_LET: {
		fprintf(out, "%slet ", indent_str);

		/* Multi-value let */
		if (stmt->data.let_stmt.name_count > 0 && stmt->data.let_stmt.names) {
			for (int i = 0; i < stmt->data.let_stmt.name_count; i++) {
				fprintf(out, "%s", stmt->data.let_stmt.names[i]);
				if (i < stmt->data.let_stmt.name_count - 1) {
					fprintf(out, ", ");
				}
			}
			/* Multi-value always inferred (no type), so := */
			fprintf(out, " := ");
			format_expression(out, stmt->data.let_stmt.value);
		} else {
			/* Single-value let */
			fprintf(out, "%s", stmt->data.let_stmt.name);
			if (stmt->data.let_stmt.type) {
				/* Explicit type: let x: type = value */
				fprintf(out, ": ");
				format_type(out, stmt->data.let_stmt.type);
				if (stmt->data.let_stmt.value) {
					fprintf(out, " = ");
					format_expression(out, stmt->data.let_stmt.value);
				}
			} else if (stmt->data.let_stmt.value) {
				/* Inferred type: let x := value */
				fprintf(out, " := ");
				format_expression(out, stmt->data.let_stmt.value);
			}
		}
		fprintf(out, ";\n");
		break;
	}
	case STMT_ASSIGN: {
		fprintf(out, "%s", indent_str);
		format_expression(out, stmt->data.assign_stmt.target);
		const char *op_str = "=";
		switch (stmt->data.assign_stmt.op) {
		case OP_ADD:
			op_str = "+=";
			break;
		case OP_SUB:
			op_str = "-=";
			break;
		case OP_MUL:
			op_str = "*=";
			break;
		case OP_DIV:
			op_str = "/=";
			break;
		default:
			break;
		}
		fprintf(out, " %s ", op_str);
		format_expression(out, stmt->data.assign_stmt.value);
		fprintf(out, ";\n");
		break;
	}
	case STMT_FOR: {
		fprintf(out, "%sfor", indent_str);
		if (stmt->data.for_stmt.init || stmt->data.for_stmt.increment) {
			/* Parenthesized for loop: for (init; cond; incr) { } */
			fprintf(out, " (");
			if (stmt->data.for_stmt.init) {
				/* Format statement without leading indent/newline */
				if (stmt->data.for_stmt.init->type == STMT_LET) {
					LetStmt *let = &stmt->data.for_stmt.init->data.let_stmt;
					fprintf(out, "let %s", let->name);
					if (let->type) {
						fprintf(out, ": ");
						format_type(out, let->type);
					}
					if (let->value) {
						fprintf(out, " = ");
						format_expression(out, let->value);
					}
				} else if (stmt->data.for_stmt.init->type == STMT_EXPR) {
					format_expression(out, stmt->data.for_stmt.init->data.expr_stmt.expr);
				}
			}
			fprintf(out, "; ");
			if (stmt->data.for_stmt.condition) {
				format_expression(out, stmt->data.for_stmt.condition);
			}
			fprintf(out, "; ");
			if (stmt->data.for_stmt.increment) {
				if (stmt->data.for_stmt.increment->type == STMT_ASSIGN) {
					AssignStmt *assign = &stmt->data.for_stmt.increment->data.assign_stmt;
					format_expression(out, assign->target);
					if (assign->op != OP_NONE) {
						switch (assign->op) {
						case OP_ADD:
							fprintf(out, " += ");
							break;
						case OP_SUB:
							fprintf(out, " -= ");
							break;
						case OP_MUL:
							fprintf(out, " *= ");
							break;
						case OP_DIV:
							fprintf(out, " /= ");
							break;
						default:
							fprintf(out, " = ");
							break;
						}
					} else {
						fprintf(out, " = ");
					}
					format_expression(out, assign->value);
				} else if (stmt->data.for_stmt.increment->type == STMT_EXPR) {
					format_expression(out, stmt->data.for_stmt.increment->data.expr_stmt.expr);
				}
			}
			fprintf(out, ")");
		} else if (!stmt->data.for_stmt.var_name) {
			/* Infinite or condition-based for */
			if (stmt->data.for_stmt.condition) {
				/* Condition-only: for (; cond;) { } */
				fprintf(out, " (;");
				format_expression(out, stmt->data.for_stmt.condition);
				fprintf(out, ";)");
			}
			/* Else infinite: for { } */
		} else {
			/* Range-based: for var_name in iterable { } */
			fprintf(out, " %s in ", stmt->data.for_stmt.var_name);
			format_expression(out, stmt->data.for_stmt.iterable);
		}
		fprintf(out, " {\n");
		for (int i = 0; i < stmt->data.for_stmt.body_count; i++) {
			format_statement(out, stmt->data.for_stmt.body[i], indent + 1);
		}
		fprintf(out, "%s}\n", indent_str);
		break;
	}
	case STMT_IF: {
		fprintf(out, "%sif (", indent_str);
		format_expression(out, stmt->data.if_stmt.cond);
		fprintf(out, ") {\n");
		for (int i = 0; i < stmt->data.if_stmt.then_count; i++) {
			format_statement(out, stmt->data.if_stmt.then_body[i], indent + 1);
		}
		if (stmt->data.if_stmt.else_count > 0) {
			fprintf(out, "%s} else {\n", indent_str);
			for (int i = 0; i < stmt->data.if_stmt.else_count; i++) {
				format_statement(out, stmt->data.if_stmt.else_body[i], indent + 1);
			}
		}
		fprintf(out, "%s}\n", indent_str);
		break;
	}
	case STMT_RUN: {
		if (stmt->data.run_stmt.world_name) {
			fprintf(out, "%srun %s in %s;\n", indent_str, stmt->data.run_stmt.system_name,
			        stmt->data.run_stmt.world_name);
		} else {
			fprintf(out, "%srun %s;\n", indent_str, stmt->data.run_stmt.system_name);
		}
		break;
	}
	case STMT_EXPR: {
		fprintf(out, "%s", indent_str);
		format_expression(out, stmt->data.expr_stmt.expr);
		fprintf(out, ";\n");
		break;
	}
	case STMT_FREE: {
		fprintf(out, "%s?.free(", indent_str);
		format_expression(out, stmt->data.free_stmt.value);
		fprintf(out, ");\n");
		break;
	}
	case STMT_BREAK: {
		fprintf(out, "%sbreak;\n", indent_str);
		break;
	}
	case STMT_RETURN: {
		fprintf(out, "%sreturn ", indent_str);
		format_expression(out, stmt->data.return_stmt.value);
		fprintf(out, ";\n");
		break;
	}
	}
}

/* Get the actual start line of a declaration (from inner payload, not the broken decl->loc) */
static int decl_start_line(Decl *decl) {
	if (!decl)
		return 1;
	switch (decl->kind) {
	case DECL_WORLD:
		return decl->data.world->loc.line;
	case DECL_ARCHETYPE:
		return decl->data.archetype->loc.line;
	case DECL_PROC:
		return decl->data.proc->loc.line;
	case DECL_SYS:
		return decl->data.sys->loc.line;
	case DECL_FUNC:
		return decl->data.func->loc.line;
	}
	return 1;
}

/* Emit any comments that appear before the given line, plus blank lines if needed */
static void flush_before_line(FILE *out, FmtCtx *ctx, int line) {
	if (!ctx || !ctx->comments)
		return;

	/* Emit any comments with line < current line */
	while (ctx->comment_idx < ctx->comment_count && ctx->comments[ctx->comment_idx].line < line) {
		Token *comment = &ctx->comments[ctx->comment_idx];
		/* Output the comment verbatim from the token */
		fprintf(out, "%.*s\n", (int)comment->length, comment->start);
		ctx->last_line = comment->line;
		ctx->comment_idx++;
	}

	/* Emit blank lines for gaps (preserve up to 2 blank lines) */
	if (line > ctx->last_line + 1) {
		/* Add 1 or 2 blank lines depending on gap size */
		int gap = line - ctx->last_line - 1;
		if (gap >= 2) {
			fprintf(out, "\n\n");
		} else {
			fprintf(out, "\n");
		}
	}
	ctx->last_line = line - 1;
}

void format_program(FILE *out, Program *prog, Token *comments, size_t comment_count, const char *src) {
	if (!prog)
		return;

	FmtCtx ctx = {.comments = comments, .comment_count = comment_count, .comment_idx = 0, .last_line = 0, .src = src};

	/* Emit any leading comments before first declaration */
	if (prog->decl_count > 0) {
		int first_line = decl_start_line(prog->decls[0]);
		flush_before_line(out, &ctx, first_line);
	} else {
		/* No declarations, emit all comments */
		for (size_t i = 0; i < comment_count; i++) {
			fprintf(out, "%.*s\n", (int)comments[i].length, comments[i].start);
		}
		return;
	}

	for (int i = 0; i < prog->decl_count; i++) {
		Decl *decl = prog->decls[i];

		/* Emit comments before this declaration */
		flush_before_line(out, &ctx, decl_start_line(decl));

		switch (decl->kind) {
		case DECL_WORLD: {
			WorldDecl *world = decl->data.world;
			fprintf(out, "world %s()\n\n", world->name);
			break;
		}
		case DECL_ARCHETYPE: {
			ArchetypeDecl *arch = decl->data.archetype;
			fprintf(out, "arche %s {\n", arch->name);
			for (int j = 0; j < arch->field_count; j++) {
				FieldDecl *field = arch->fields[j];
				fprintf(out, "  %s: ", field->name);
				format_type(out, field->type);
				fprintf(out, ",\n");
			}
			fprintf(out, "}\n\n");
			break;
		}
		case DECL_PROC: {
			ProcDecl *proc = decl->data.proc;
			if (proc->is_extern)
				fprintf(out, "extern ");
			fprintf(out, "proc %s(", proc->name);
			for (int j = 0; j < proc->param_count; j++) {
				if (j > 0)
					fprintf(out, ", ");
				fprintf(out, "%s: ", proc->params[j]->name);
				format_type(out, proc->params[j]->type);
			}
			if (proc->is_extern) {
				fprintf(out, ");\n");
			} else {
				fprintf(out, ") {\n");
				for (int j = 0; j < proc->statement_count; j++) {
					format_statement(out, proc->statements[j], 1);
				}
				fprintf(out, "}\n\n");
			}
			break;
		}
		case DECL_SYS: {
			SysDecl *sys = decl->data.sys;
			fprintf(out, "sys %s(", sys->name);
			for (int j = 0; j < sys->param_count; j++) {
				if (j > 0)
					fprintf(out, ", ");
				fprintf(out, "%s", sys->params[j]->name);
			}
			fprintf(out, ") {\n");
			for (int j = 0; j < sys->statement_count; j++) {
				format_statement(out, sys->statements[j], 1);
			}
			fprintf(out, "}\n\n");
			break;
		}
		case DECL_FUNC: {
			FuncDecl *func = decl->data.func;
			if (func->is_extern)
				fprintf(out, "extern ");
			fprintf(out, "func %s(", func->name);
			for (int j = 0; j < func->param_count; j++) {
				if (j > 0)
					fprintf(out, ", ");
				if (func->params[j]->is_out)
					fprintf(out, "out ");
				fprintf(out, "%s: ", func->params[j]->name);
				format_type(out, func->params[j]->type);
			}
			fprintf(out, ") -> ");
			format_type(out, func->return_type);
			if (func->is_extern) {
				fprintf(out, ";\n");
			} else {
				fprintf(out, " {\n");
				for (int j = 0; j < func->statement_count; j++) {
					format_statement(out, func->statements[j], 1);
				}
				fprintf(out, "}\n");
			}
			fprintf(out, "\n");
			break;
		}
		case DECL_STATIC: {
			StaticDecl *alloc = decl->data.alloc;
			fprintf(out, "static %s(", alloc->archetype_name);
			if (alloc->field_count > 0) {
				format_expression(out, alloc->field_values[0]);
			}
			if (alloc->init_length) {
				fprintf(out, ", ");
				format_expression(out, alloc->init_length);
			}
			fprintf(out, ")");
			if (alloc->field_count > 1) {
				fprintf(out, " {\n");
				for (int j = 1; j < alloc->field_count; j++) {
					fprintf(out, "  %s: ", alloc->field_names[j]);
					format_expression(out, alloc->field_values[j]);
					fprintf(out, ",\n");
				}
				fprintf(out, "}");
			}
			fprintf(out, ";\n\n");
			break;
		}
		}
	}
}
