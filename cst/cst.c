#include "cst.h"
#include <stdlib.h>
#include <string.h>

/* =========================
   AstProgram / Declarations
   ========================= */

AstProgram *ast_program_create(void) {
	AstProgram *prog = malloc(sizeof(AstProgram));
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
	proc->return_types = NULL;
	proc->return_type_count = 0;
	proc->is_extern = 0;
	proc->is_unsafe = 0;
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

FuncDecl *func_decl_create(char *name) {
	FuncDecl *func = malloc(sizeof(FuncDecl));
	func->name = name;
	func->return_types = NULL;
	func->return_type_count = 0;
	func->params = NULL;
	func->param_count = 0;
	func->is_extern = 0;
	func->is_unsafe = 0;
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
	constant->type_value = NULL;
	constant->decl_type = NULL;
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
	param->is_own = 0;
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
	Expression *expr = calloc(1, sizeof(Expression));
	expr->type = type;
	expr->loc.line = 1;
	expr->loc.column = 1;
	expr->resolved_type = NULL;
	return expr;
}

/* =========================
   Destructors
   ========================= */

void ast_program_free(AstProgram *prog) {
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
	for (int i = 0; i < decl->allow_slug_count; i++)
		free(decl->allow_slugs[i]);
	free(decl->allow_slugs);
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
			type_ref_free(c->type_value);
			type_ref_free(c->decl_type);
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
	for (int i = 0; i < proc->return_type_count; i++)
		type_ref_free(proc->return_types[i]);
	free(proc->return_types);
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
	for (int i = 0; i < func->return_type_count; i++)
		type_ref_free(func->return_types[i]);
	free(func->return_types);
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
	case TYPE_OPAQUE:
		break;
	case TYPE_TYPE:
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
	case STMT_BIND:
		free(stmt->data.bind_stmt.name);
		type_ref_free(stmt->data.bind_stmt.type);
		expression_free(stmt->data.bind_stmt.value);
		type_ref_free(stmt->data.bind_stmt.type_value);
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
	case STMT_BREAK:
		break;
	case STMT_RETURN:
		for (int i = 0; i < stmt->data.return_stmt.count; i++)
			expression_free(stmt->data.return_stmt.values[i]);
		free(stmt->data.return_stmt.values);
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
