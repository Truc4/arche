#include "cst.h"
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
	Decl *decl = calloc(1, sizeof(Decl));
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
	proc->allow_pure_proc = 0;
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

FuncGroup *func_group_create(char *name) {
	FuncGroup *g = malloc(sizeof(FuncGroup));
	g->name = name;
	g->member_names = NULL;
	g->member_count = 0;
	g->loc.line = 0;
	g->loc.column = 0;
	return g;
}

void func_group_free(FuncGroup *group) {
	if (!group)
		return;
	if (group->name)
		free(group->name);
	for (int i = 0; i < group->member_count; i++) {
		if (group->member_names[i])
			free(group->member_names[i]);
	}
	free(group->member_names);
	free(group);
}

ConstDecl *const_decl_create(char *name, Expression *value) {
	ConstDecl *constant = malloc(sizeof(ConstDecl));
	constant->name = name;
	constant->value = value;
	return constant;
}

StaticDecl *static_decl_archetype_create(char *archetype_name) {
	StaticDecl *s = calloc(1, sizeof(StaticDecl));
	s->kind = STATIC_KIND_ARCHETYPE;
	s->archetype.archetype_name = archetype_name;
	return s;
}

StaticDecl *static_decl_array_create(char *name, TypeRef *element_type, int size) {
	StaticDecl *s = malloc(sizeof(StaticDecl));
	s->kind = STATIC_KIND_ARRAY;
	s->array.name = name;
	s->array.element_type = element_type;
	s->array.size = size;
	return s;
}

UseDecl *use_decl_create(char *name) {
	UseDecl *use = malloc(sizeof(UseDecl));
	use->name = name;
	return use;
}

Parameter *parameter_create(char *name, TypeRef *type) {
	Parameter *param = malloc(sizeof(Parameter));
	param->name = name;
	param->type = type;
	param->is_out = 0;
	param->loc.line = 1;
	param->loc.column = 1;
	return param;
}

FieldDecl *field_decl_create(FieldKind kind, char *name, TypeRef *type) {
	FieldDecl *field = calloc(1, sizeof(FieldDecl));
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
	stmt->last_line = 1;
	return stmt;
}

Expression *expression_create(ExpressionType type) {
	Expression *expr = malloc(sizeof(Expression));
	expr->type = type;
	expr->loc.line = 1;
	expr->loc.column = 1;
	expr->resolved_type = NULL;
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
	free(decl->leading_trivia);
	free(decl->trailing_trivia);
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
	case DECL_FUNC_GROUP:
		func_group_free(decl->data.func_group);
		break;
	case DECL_STATIC:
		static_decl_free(decl->data.static_decl);
		break;
	case DECL_CONST: {
		ConstDecl *c = decl->data.constant;
		if (c) {
			free(c->name);
			expression_free(c->value);
			free(c);
		}
		break;
	}
	case DECL_USE: {
		use_decl_free(decl->data.use);
		break;
	}
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
	free(field->leading_trivia);
	free(field->trailing_trivia);
	free(field);
}

void static_decl_free(StaticDecl *s) {
	if (!s)
		return;
	if (s->kind == STATIC_KIND_ARCHETYPE) {
		free(s->archetype.archetype_name);
		for (int i = 0; i < s->archetype.field_count; i++) {
			free(s->archetype.field_names[i]);
			expression_free(s->archetype.field_values[i]);
		}
		free(s->archetype.field_names);
		free(s->archetype.field_values);
		expression_free(s->archetype.init_length);
	} else {
		free(s->array.name);
		type_ref_free(s->array.element_type);
	}
	free(s);
}

void use_decl_free(UseDecl *use) {
	if (!use)
		return;
	free(use->name);
	free(use);
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
	case TYPE_HANDLE:
		break;
	case TYPE_ARCHETYPE:
		break;
	}
	free(type);
}

void statement_free(Statement *stmt) {
	if (!stmt)
		return;
	free(stmt->leading_trivia);
	free(stmt->trailing_trivia);
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
	case STMT_BREAK:
		break;
	case STMT_RETURN:
		expression_free(stmt->data.return_stmt.value);
		break;
	case STMT_MULTI_BIND:
		for (int i = 0; i < stmt->data.multi_bind.target_count; i++) {
			free(stmt->data.multi_bind.targets[i].name);
			if (stmt->data.multi_bind.targets[i].type) {
				type_ref_free(stmt->data.multi_bind.targets[i].type);
			}
		}
		free(stmt->data.multi_bind.targets);
		expression_free(stmt->data.multi_bind.value);
		break;
	case STMT_EACH_FIELD:
		free(stmt->data.each_field.binding_name);
		type_ref_free(stmt->data.each_field.filter_type);
		free(stmt->data.each_field.arch_param_name);
		for (int i = 0; i < stmt->data.each_field.body_count; i++) {
			statement_free(stmt->data.each_field.body[i]);
		}
		free(stmt->data.each_field.body);
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
	case EXPR_STRING:
		free(expr->data.string.value);
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
	case TYPE_HANDLE:
		fprintf(out, "handle(%s)", type->data.handle.archetype_name);
		break;
	case TYPE_ARCHETYPE:
		fprintf(out, "archetype");
		break;
	}
	format_type_depth--;
}

static void format_expression(FILE *out, Expression *expr);

/* Operator precedence for paren-wrapping. Higher binds tighter. Matches the
 * grammar: mul/div > add/sub > comparison. */
static int expr_binary_precedence(Operator op) {
	switch (op) {
	case OP_MUL:
	case OP_DIV:
		return 30;
	case OP_ADD:
	case OP_SUB:
		return 20;
	case OP_EQ:
	case OP_NEQ:
	case OP_LT:
	case OP_GT:
	case OP_LTE:
	case OP_GTE:
		return 10;
	case OP_NONE:
		return 0;
	}
	return 0;
}

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
		/* Precedence-aware paren wrapping. The parser drops the explicit parens,
		 * so the formatter has to re-introduce them whenever an operand is a
		 * binary expression with lower-or-equal precedence than the current op.
		 * Lower precedence on either side changes semantics; equal precedence on
		 * the RIGHT changes semantics for left-associative operators. */
		int this_prec = expr_binary_precedence(expr->data.binary.op);
		Expression *l = expr->data.binary.left;
		Expression *r = expr->data.binary.right;
		int l_needs_parens = (l && l->type == EXPR_BINARY && expr_binary_precedence(l->data.binary.op) < this_prec);
		int r_needs_parens = (r && r->type == EXPR_BINARY && expr_binary_precedence(r->data.binary.op) <= this_prec);
		if (l_needs_parens)
			fprintf(out, "(");
		format_expression(out, l);
		if (l_needs_parens)
			fprintf(out, ")");
		fprintf(out, " %s ", op_str);
		if (r_needs_parens)
			fprintf(out, "(");
		format_expression(out, r);
		if (r_needs_parens)
			fprintf(out, ")");
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

/* Format context. Used only for tracking whether we're at the start of the
 * program (to suppress an initial blank line) — every other piece of formatting
 * state now lives on the CST node itself (leading_trivia / trailing_trivia). */
typedef struct {
	int at_program_start;
} FmtCtx;

/* Emit a node's leading trivia: comments on their own indented lines, blank
 * lines as literal '\n's. If at_program_start, BLANK_LINES entries are
 * suppressed (no leading blanks at file start). */
static void emit_leading_trivia(FILE *out, Trivia *trivia, int count, const char *indent_str, FmtCtx *ctx) {
	for (int i = 0; i < count; i++) {
		Trivia *t = &trivia[i];
		if (t->kind == TRIVIA_BLANK_LINES) {
			/* Suppress only the LEADING blanks of the program (before any
			 * content has been emitted). Once we've emitted a comment or
			 * an actual decl, blank lines are real and should be preserved. */
			if (ctx && ctx->at_program_start)
				continue;
			int n = t->blank_count;
			if (n > 2)
				n = 2; /* normalize: at most one blank line of separation */
			for (int k = 0; k < n; k++)
				fputc('\n', out);
		} else if (t->kind == TRIVIA_LINE_COMMENT || t->kind == TRIVIA_BLOCK_COMMENT) {
			if (indent_str && indent_str[0])
				fputs(indent_str, out);
			fprintf(out, "%.*s\n", (int)t->length, t->start);
			if (ctx)
				ctx->at_program_start = 0;
		}
	}
}

/* Emit a node's trailing trivia inline: each comment prefixed with two spaces,
 * no leading newline (the caller emits its terminator AFTER). */
static void emit_trailing_trivia(FILE *out, Trivia *trivia, int count) {
	for (int i = 0; i < count; i++) {
		Trivia *t = &trivia[i];
		if (t->kind == TRIVIA_LINE_COMMENT || t->kind == TRIVIA_BLOCK_COMMENT) {
			fprintf(out, "  %.*s", (int)t->length, t->start);
		}
	}
}

static void format_statement(FILE *out, Statement *stmt, int indent, FmtCtx *ctx);

static void format_statement(FILE *out, Statement *stmt, int indent, FmtCtx *ctx) {
	if (!stmt)
		return;

	char indent_str[256] = "";
	for (int i = 0; i < indent; i++) {
		strcat(indent_str, "  ");
	}

	emit_leading_trivia(out, stmt->leading_trivia, stmt->leading_count, indent_str, ctx);

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
		fprintf(out, ";");
		emit_trailing_trivia(out, stmt->trailing_trivia, stmt->trailing_count);
		fprintf(out, "\n");
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
		fprintf(out, ";");
		emit_trailing_trivia(out, stmt->trailing_trivia, stmt->trailing_count);
		fprintf(out, "\n");
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
			format_statement(out, stmt->data.for_stmt.body[i], indent + 1, ctx);
		}
		fprintf(out, "%s}\n", indent_str);
		break;
	}
	case STMT_IF: {
		fprintf(out, "%sif (", indent_str);
		format_expression(out, stmt->data.if_stmt.cond);
		fprintf(out, ") {\n");
		for (int i = 0; i < stmt->data.if_stmt.then_count; i++) {
			format_statement(out, stmt->data.if_stmt.then_body[i], indent + 1, ctx);
		}
		if (stmt->data.if_stmt.else_count > 0) {
			fprintf(out, "%s} else {\n", indent_str);
			for (int i = 0; i < stmt->data.if_stmt.else_count; i++) {
				format_statement(out, stmt->data.if_stmt.else_body[i], indent + 1, ctx);
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
		fprintf(out, ";");
		emit_trailing_trivia(out, stmt->trailing_trivia, stmt->trailing_count);
		fprintf(out, "\n");
		break;
	}
	case STMT_FREE: {
		fprintf(out, "%s?.free(", indent_str);
		format_expression(out, stmt->data.free_stmt.value);
		fprintf(out, ");");
		emit_trailing_trivia(out, stmt->trailing_trivia, stmt->trailing_count);
		fprintf(out, "\n");
		break;
	}
	case STMT_BREAK: {
		fprintf(out, "%sbreak;", indent_str);
		emit_trailing_trivia(out, stmt->trailing_trivia, stmt->trailing_count);
		fprintf(out, "\n");
		break;
	}
	case STMT_RETURN: {
		fprintf(out, "%sreturn ", indent_str);
		format_expression(out, stmt->data.return_stmt.value);
		fprintf(out, ";");
		emit_trailing_trivia(out, stmt->trailing_trivia, stmt->trailing_count);
		fprintf(out, "\n");
		break;
	}
	case STMT_MULTI_BIND: {
		if (stmt->data.multi_bind.from_shorthand) {
			fprintf(out, "%slet ", indent_str);
			for (int i = 0; i < stmt->data.multi_bind.target_count; i++) {
				fprintf(out, "%s", stmt->data.multi_bind.targets[i].name);
				if (i < stmt->data.multi_bind.target_count - 1) {
					fprintf(out, ", ");
				}
			}
			fprintf(out, " := ");
		} else {
			fprintf(out, "%s(", indent_str);
			for (int i = 0; i < stmt->data.multi_bind.target_count; i++) {
				if (stmt->data.multi_bind.targets[i].is_new) {
					fprintf(out, "let %s", stmt->data.multi_bind.targets[i].name);
					if (stmt->data.multi_bind.targets[i].type) {
						fprintf(out, ": ");
						format_type(out, stmt->data.multi_bind.targets[i].type);
					} else {
						fprintf(out, ":");
					}
				} else {
					fprintf(out, "%s", stmt->data.multi_bind.targets[i].name);
				}
				if (i < stmt->data.multi_bind.target_count - 1) {
					fprintf(out, ", ");
				}
			}
			fprintf(out, ") = ");
		}
		format_expression(out, stmt->data.multi_bind.value);
		fprintf(out, ";");
		emit_trailing_trivia(out, stmt->trailing_trivia, stmt->trailing_count);
		fprintf(out, "\n");
		break;
	}
	case STMT_EACH_FIELD: {
		fprintf(out, "%seach_field %s", indent_str, stmt->data.each_field.binding_name);
		if (stmt->data.each_field.filter_type) {
			fprintf(out, ": ");
			format_type(out, stmt->data.each_field.filter_type);
		}
		fprintf(out, " in %s {\n", stmt->data.each_field.arch_param_name);
		for (int i = 0; i < stmt->data.each_field.body_count; i++) {
			format_statement(out, stmt->data.each_field.body[i], indent + 1, ctx);
		}
		fprintf(out, "%s}\n", indent_str);
		break;
	}
	}
}

/* If the next pending comment is on `line`, emit it as a trailing inline
 * comment ("  // ...") WITHOUT a leading newline, and consume it. Caller is
 * responsible for emitting the trailing '\n'. Returns 1 if a comment was
 * emitted, 0 otherwise. */
void format_program(FILE *out, Program *prog, Token *comments, size_t comment_count, const char *src) {
	(void)comments;
	(void)comment_count;
	(void)src;
	if (!prog)
		return;

	FmtCtx ctx = {.at_program_start = 1};

	for (int i = 0; i < prog->decl_count; i++) {
		Decl *decl = prog->decls[i];

		emit_leading_trivia(out, decl->leading_trivia, decl->leading_count, "", &ctx);

		switch (decl->kind) {
		case DECL_WORLD: {
			WorldDecl *world = decl->data.world;
			fprintf(out, "world %s()", world->name);
			emit_trailing_trivia(out, decl->trailing_trivia, decl->trailing_count);
			fprintf(out, "\n");
			break;
		}
		case DECL_ARCHETYPE: {
			ArchetypeDecl *arch = decl->data.archetype;
			fprintf(out, "arche %s {\n", arch->name);
			for (int j = 0; j < arch->field_count; j++) {
				FieldDecl *field = arch->fields[j];
				emit_leading_trivia(out, field->leading_trivia, field->leading_count, "  ", &ctx);
				fprintf(out, "  %s: ", field->name);
				format_type(out, field->type);
				fprintf(out, ",");
				emit_trailing_trivia(out, field->trailing_trivia, field->trailing_count);
				fprintf(out, "\n");
			}
			fprintf(out, "}");
			emit_trailing_trivia(out, decl->trailing_trivia, decl->trailing_count);
			fprintf(out, "\n");
			break;
		}
		case DECL_PROC: {
			ProcDecl *proc = decl->data.proc;
			if (proc->allow_pure_proc)
				fprintf(out, "@allow_pure_proc\n");
			if (proc->is_extern)
				fprintf(out, "extern ");
			fprintf(out, "proc %s(", proc->name);
			for (int j = 0; j < proc->param_count; j++) {
				if (j > 0)
					fprintf(out, ", ");
				if (proc->params[j]->is_out)
					fprintf(out, "out ");
				fprintf(out, "%s: ", proc->params[j]->name);
				format_type(out, proc->params[j]->type);
			}
			if (proc->is_extern) {
				fprintf(out, ");");
				emit_trailing_trivia(out, decl->trailing_trivia, decl->trailing_count);
				fprintf(out, "\n");
			} else {
				fprintf(out, ") {\n");
				for (int j = 0; j < proc->statement_count; j++) {
					format_statement(out, proc->statements[j], 1, &ctx);
				}
				fprintf(out, "}");
				emit_trailing_trivia(out, decl->trailing_trivia, decl->trailing_count);
				fprintf(out, "\n");
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
				format_statement(out, sys->statements[j], 1, &ctx);
			}
			fprintf(out, "}");
			emit_trailing_trivia(out, decl->trailing_trivia, decl->trailing_count);
			fprintf(out, "\n");
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
				fprintf(out, ";");
				emit_trailing_trivia(out, decl->trailing_trivia, decl->trailing_count);
				fprintf(out, "\n");
			} else {
				fprintf(out, " {\n");
				for (int j = 0; j < func->statement_count; j++) {
					format_statement(out, func->statements[j], 1, &ctx);
				}
				fprintf(out, "}");
				emit_trailing_trivia(out, decl->trailing_trivia, decl->trailing_count);
				fprintf(out, "\n");
			}
			break;
		}
		case DECL_STATIC: {
			StaticDecl *s = decl->data.static_decl;
			if (s->kind == STATIC_KIND_ARCHETYPE) {
				fprintf(out, "static %s(", s->archetype.archetype_name);
				if (s->archetype.field_count > 0) {
					format_expression(out, s->archetype.field_values[0]);
				}
				if (s->archetype.init_length) {
					fprintf(out, ", ");
					format_expression(out, s->archetype.init_length);
				}
				fprintf(out, ")");
				if (s->archetype.field_count > 1) {
					fprintf(out, " {\n");
					for (int j = 1; j < s->archetype.field_count; j++) {
						fprintf(out, "  %s: ", s->archetype.field_names[j]);
						format_expression(out, s->archetype.field_values[j]);
						fprintf(out, ",\n");
					}
					fprintf(out, "}");
				}
			} else {
				fprintf(out, "static %s: ", s->array.name);
				format_type(out, s->array.element_type);
				fprintf(out, "[%d]", s->array.size);
			}
			fprintf(out, ";");
			emit_trailing_trivia(out, decl->trailing_trivia, decl->trailing_count);
			fprintf(out, "\n");
			break;
		}
		case DECL_FUNC_GROUP: {
			FuncGroup *g = decl->data.func_group;
			fprintf(out, "func %s = { ", g->name);
			for (int j = 0; j < g->member_count; j++) {
				if (j > 0)
					fprintf(out, ", ");
				fprintf(out, "%s", g->member_names[j]);
			}
			fprintf(out, " };");
			emit_trailing_trivia(out, decl->trailing_trivia, decl->trailing_count);
			fprintf(out, "\n");
			break;
		}
		case DECL_USE: {
			fprintf(out, "use %s;", decl->data.use->name);
			emit_trailing_trivia(out, decl->trailing_trivia, decl->trailing_count);
			fprintf(out, "\n");
			break;
		}
		case DECL_CONST: {
			ConstDecl *c = decl->data.constant;
			fprintf(out, "%s :: ", c->name);
			format_expression(out, c->value);
			emit_trailing_trivia(out, decl->trailing_trivia, decl->trailing_count);
			fprintf(out, "\n");
			break;
		}
		}
		ctx.at_program_start = 0;
	}
}
