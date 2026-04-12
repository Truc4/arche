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
	Statement *stmt = malloc(sizeof(Statement));
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
	if (!prog) return;
	for (int i = 0; i < prog->decl_count; i++) {
		decl_free(prog->decls[i]);
	}
	free(prog->decls);
	free(prog);
}

void decl_free(Decl *decl) {
	if (!decl) return;
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
	if (!world) return;
	free(world->name);
	for (int i = 0; i < world->field_count; i++) {
		free(world->field_names[i]);
	}
	free(world->field_names);
	free(world);
}

void archetype_decl_free(ArchetypeDecl *archetype) {
	if (!archetype) return;
	free(archetype->name);
	for (int i = 0; i < archetype->field_count; i++) {
		field_decl_free(archetype->fields[i]);
	}
	free(archetype->fields);
	free(archetype);
}

void proc_decl_free(ProcDecl *proc) {
	if (!proc) return;
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
	if (!sys) return;
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
	if (!func) return;
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
	if (!param) return;
	free(param->name);
	type_ref_free(param->type);
	free(param);
}

void field_decl_free(FieldDecl *field) {
	if (!field) return;
	free(field->name);
	type_ref_free(field->type);
	free(field);
}

void type_ref_free(TypeRef *type) {
	if (!type) return;
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
	}
	free(type);
}

void statement_free(Statement *stmt) {
	if (!stmt) return;
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
		expression_free(stmt->data.for_stmt.iterable);
		for (int i = 0; i < stmt->data.for_stmt.body_count; i++) {
			statement_free(stmt->data.for_stmt.body[i]);
		}
		free(stmt->data.for_stmt.body);
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
	if (!expr) return;
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
	}
	free(expr);
}

/* =========================
   Formatting / Pretty-printing
   ========================= */

#include <stdio.h>

static void format_type(FILE *out, TypeRef *type) {
	if (!type) return;
	switch (type->kind) {
	case TYPE_NAME:
		fprintf(out, "%s", type->data.name);
		break;
	case TYPE_ARRAY:
		fprintf(out, "[");
		format_type(out, type->data.array.element_type);
		fprintf(out, "]");
		break;
	case TYPE_SHAPED_ARRAY:
		fprintf(out, "[");
		format_type(out, type->data.shaped_array.element_type);
		fprintf(out, "]%d", type->data.shaped_array.rank);
		break;
	}
}

static void format_expression(FILE *out, Expression *expr);

static void format_expression(FILE *out, Expression *expr) {
	if (!expr) return;

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
			if (i > 0) fprintf(out, ", ");
			format_expression(out, expr->data.index.indices[i]);
		}
		fprintf(out, "]");
		break;
	}
	case EXPR_BINARY: {
		const char *op_str = "?";
		switch (expr->data.binary.op) {
		case OP_ADD: op_str = "+"; break;
		case OP_SUB: op_str = "-"; break;
		case OP_MUL: op_str = "*"; break;
		case OP_DIV: op_str = "/"; break;
		case OP_EQ: op_str = "=="; break;
		case OP_NEQ: op_str = "!="; break;
		case OP_LT: op_str = "<"; break;
		case OP_GT: op_str = ">"; break;
		case OP_LTE: op_str = "<="; break;
		case OP_GTE: op_str = ">="; break;
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
			if (i > 0) fprintf(out, ", ");
			format_expression(out, expr->data.call.args[i]);
		}
		fprintf(out, ")");
		break;
	}
	case EXPR_ALLOC: {
		fprintf(out, "%s.alloc(", expr->data.alloc.archetype_name);
		for (int i = 0; i < expr->data.alloc.field_count; i++) {
			if (i > 0) fprintf(out, ", ");
			fprintf(out, "%s: ", expr->data.alloc.field_names[i]);
			format_expression(out, expr->data.alloc.field_values[i]);
		}
		fprintf(out, ")");
		break;
	}
	}
}

static void format_statement(FILE *out, Statement *stmt, int indent);

static void format_statement(FILE *out, Statement *stmt, int indent) {
	if (!stmt) return;

	char indent_str[256] = "";
	for (int i = 0; i < indent; i++) {
		strcat(indent_str, "  ");
	}

	switch (stmt->type) {
	case STMT_LET: {
		fprintf(out, "%slet %s = ", indent_str, stmt->data.let_stmt.name);
		format_expression(out, stmt->data.let_stmt.value);
		fprintf(out, ";\n");
		break;
	}
	case STMT_ASSIGN: {
		fprintf(out, "%s", indent_str);
		format_expression(out, stmt->data.assign_stmt.target);
		fprintf(out, " = ");
		format_expression(out, stmt->data.assign_stmt.value);
		fprintf(out, ";\n");
		break;
	}
	case STMT_FOR: {
		fprintf(out, "%sfor %s in %s {\n", indent_str,
			stmt->data.for_stmt.var_name, "?");
		for (int i = 0; i < stmt->data.for_stmt.body_count; i++) {
			format_statement(out, stmt->data.for_stmt.body[i], indent + 1);
		}
		fprintf(out, "%s}\n", indent_str);
		break;
	}
	case STMT_RUN: {
		fprintf(out, "%srun %s in %s;\n", indent_str,
			stmt->data.run_stmt.system_name,
			stmt->data.run_stmt.world_name);
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
	}
}

void format_program(FILE *out, Program *prog) {
	if (!prog) return;

	for (int i = 0; i < prog->decl_count; i++) {
		Decl *decl = prog->decls[i];

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
				fprintf(out, "  %s %s: ",
					field->kind == FIELD_META ? "meta" : "col",
					field->name);
				format_type(out, field->type);
				if (j < arch->field_count - 1) {
					fprintf(out, ",");
				}
				fprintf(out, "\n");
			}
			fprintf(out, "}\n\n");
			break;
		}
		case DECL_PROC: {
			ProcDecl *proc = decl->data.proc;
			fprintf(out, "proc %s() {\n", proc->name);
			for (int j = 0; j < proc->statement_count; j++) {
				format_statement(out, proc->statements[j], 1);
			}
			fprintf(out, "}\n\n");
			break;
		}
		case DECL_SYS: {
			SysDecl *sys = decl->data.sys;
			fprintf(out, "sys %s(", sys->name);
			for (int j = 0; j < sys->param_count; j++) {
				if (j > 0) fprintf(out, ", ");
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
			fprintf(out, "func %s(", func->name);
			for (int j = 0; j < func->param_count; j++) {
				if (j > 0) fprintf(out, ", ");
				fprintf(out, "%s: ", func->params[j]->name);
				format_type(out, func->params[j]->type);
			}
			fprintf(out, ") -> ");
			format_type(out, func->return_type);
			fprintf(out, " {\n");
			for (int j = 0; j < func->statement_count; j++) {
				format_statement(out, func->statements[j], 1);
			}
			fprintf(out, "}\n\n");
			break;
		}
		}
	}
}
