#include "ast.h"
#include <stdlib.h>
#include <string.h>

AstProgram *ast_program_create(void) {
	AstProgram *prog = calloc(1, sizeof(AstProgram));
	return prog;
}

AstDecl *ast_decl_create(AstDeclKind kind) {
	AstDecl *decl = calloc(1, sizeof(AstDecl));
	decl->kind = kind;
	return decl;
}

AstStmt *ast_stmt_create(AstStmtKind kind) {
	AstStmt *stmt = calloc(1, sizeof(AstStmt));
	stmt->kind = kind;
	return stmt;
}

AstExpr *ast_expr_create(AstExprKind kind) {
	AstExpr *expr = calloc(1, sizeof(AstExpr));
	expr->kind = kind;
	return expr;
}

AstType *ast_type_create(AstTypeTag tag) {
	AstType *type = calloc(1, sizeof(AstType));
	type->tag = tag;
	if (tag == AST_TYPE_INT) {
		type->int_width = 32;
		type->int_signed = 1;
	}
	return type;
}

AstField *ast_field_create(FieldKind kind, char *name, AstType *type) {
	AstField *field = calloc(1, sizeof(AstField));
	field->kind = kind;
	field->name = name;
	field->type = type;
	return field;
}

AstParam *ast_param_create(char *name, AstType *type) {
	AstParam *param = calloc(1, sizeof(AstParam));
	param->name = name;
	param->type = type;
	return param;
}

/* =========================
   Destructors
   ========================= */

void ast_type_free(AstType *type) {
	if (!type)
		return;
	ast_type_free(type->elem);
	if (type->fields) {
		for (int i = 0; i < type->field_count; i++) {
			ast_type_free(type->fields[i].type);
		}
		free(type->fields);
	}
	free(type);
}

void ast_field_free(AstField *field) {
	if (!field)
		return;
	free(field->name);
	ast_type_free(field->type);
	free(field);
}

void ast_param_free(AstParam *param) {
	if (!param)
		return;
	free(param->name);
	ast_type_free(param->type);
	free(param);
}

void ast_expr_free(AstExpr *expr) {
	if (!expr)
		return;
	switch (expr->kind) {
	case AST_EXPR_LITERAL:
		free(expr->data.literal.lexeme);
		break;
	case AST_EXPR_NAME:
		free(expr->data.name.name);
		break;
	case AST_EXPR_FIELD:
		ast_expr_free(expr->data.field.base);
		free(expr->data.field.field_name);
		break;
	case AST_EXPR_INDEX:
		ast_expr_free(expr->data.index.base);
		for (int i = 0; i < expr->data.index.index_count; i++)
			ast_expr_free(expr->data.index.indices[i]);
		free(expr->data.index.indices);
		break;
	case AST_EXPR_BINARY:
		ast_expr_free(expr->data.binary.left);
		ast_expr_free(expr->data.binary.right);
		break;
	case AST_EXPR_UNARY:
		ast_expr_free(expr->data.unary.operand);
		break;
	case AST_EXPR_CALL:
		ast_expr_free(expr->data.call.callee);
		for (int i = 0; i < expr->data.call.arg_count; i++)
			ast_expr_free(expr->data.call.args[i]);
		free(expr->data.call.args);
		break;
	case AST_EXPR_ALLOC:
		free(expr->data.alloc.archetype_name);
		for (int i = 0; i < expr->data.alloc.field_count; i++) {
			free(expr->data.alloc.field_names[i]);
			ast_expr_free(expr->data.alloc.field_values[i]);
		}
		free(expr->data.alloc.field_names);
		free(expr->data.alloc.field_values);
		ast_expr_free(expr->data.alloc.init_length);
		break;
	case AST_EXPR_ARRAY_LITERAL:
		for (int i = 0; i < expr->data.array_literal.element_count; i++)
			ast_expr_free(expr->data.array_literal.elements[i]);
		free(expr->data.array_literal.elements);
		break;
	case AST_EXPR_STRING:
		free(expr->data.string.value);
		break;
	}
	free(expr);
}

void ast_stmt_free(AstStmt *stmt) {
	if (!stmt)
		return;
	switch (stmt->kind) {
	case AST_STMT_BIND:
		for (int i = 0; i < stmt->data.bind_stmt.name_count; i++)
			free(stmt->data.bind_stmt.names[i]);
		free(stmt->data.bind_stmt.names);
		ast_type_free(stmt->data.bind_stmt.type);
		ast_expr_free(stmt->data.bind_stmt.value);
		break;
	case AST_STMT_ASSIGN:
		ast_expr_free(stmt->data.assign_stmt.target);
		ast_expr_free(stmt->data.assign_stmt.value);
		break;
	case AST_STMT_FOR:
		free(stmt->data.for_stmt.var_name);
		ast_expr_free(stmt->data.for_stmt.iterable);
		ast_stmt_free(stmt->data.for_stmt.init);
		ast_expr_free(stmt->data.for_stmt.cond);
		ast_stmt_free(stmt->data.for_stmt.incr);
		for (int i = 0; i < stmt->data.for_stmt.body_count; i++)
			ast_stmt_free(stmt->data.for_stmt.body[i]);
		free(stmt->data.for_stmt.body);
		break;
	case AST_STMT_IF:
		ast_expr_free(stmt->data.if_stmt.cond);
		for (int i = 0; i < stmt->data.if_stmt.then_count; i++)
			ast_stmt_free(stmt->data.if_stmt.then_body[i]);
		free(stmt->data.if_stmt.then_body);
		for (int i = 0; i < stmt->data.if_stmt.else_count; i++)
			ast_stmt_free(stmt->data.if_stmt.else_body[i]);
		free(stmt->data.if_stmt.else_body);
		break;
	case AST_STMT_BREAK:
		break;
	case AST_STMT_RUN:
		free(stmt->data.run_stmt.system_name);
		free(stmt->data.run_stmt.world_name);
		break;
	case AST_STMT_EXPR:
		ast_expr_free(stmt->data.expr_stmt.expr);
		break;
	case AST_STMT_FREE:
		ast_expr_free(stmt->data.free_stmt.value);
		break;
	case AST_STMT_RETURN:
		for (int i = 0; i < stmt->data.return_stmt.count; i++)
			ast_expr_free(stmt->data.return_stmt.values[i]);
		free(stmt->data.return_stmt.values);
		break;
	case AST_STMT_MULTI_BIND:
		for (int i = 0; i < stmt->data.multi_bind.target_count; i++) {
			free(stmt->data.multi_bind.targets[i].name);
			ast_type_free(stmt->data.multi_bind.targets[i].type);
		}
		free(stmt->data.multi_bind.targets);
		ast_expr_free(stmt->data.multi_bind.value);
		break;
	case AST_STMT_EACH_FIELD:
		free(stmt->data.each_field.binding_name);
		ast_type_free(stmt->data.each_field.filter_type);
		free(stmt->data.each_field.arch_param_name);
		for (int i = 0; i < stmt->data.each_field.body_count; i++) {
			ast_stmt_free(stmt->data.each_field.body[i]);
		}
		free(stmt->data.each_field.body);
		break;
	}
	free(stmt);
}

static void ast_proc_decl_free(AstProcDecl *proc) {
	if (!proc)
		return;
	free(proc->name);
	for (int i = 0; i < proc->param_count; i++)
		ast_param_free(proc->params[i]);
	free(proc->params);
	for (int i = 0; i < proc->stmt_count; i++)
		ast_stmt_free(proc->stmts[i]);
	free(proc->stmts);
	free(proc);
}

static void ast_sys_decl_free(AstSysDecl *sys) {
	if (!sys)
		return;
	free(sys->name);
	for (int i = 0; i < sys->param_count; i++)
		ast_param_free(sys->params[i]);
	free(sys->params);
	for (int i = 0; i < sys->stmt_count; i++)
		ast_stmt_free(sys->stmts[i]);
	free(sys->stmts);
	free(sys);
}

static void ast_func_decl_free(AstFuncDecl *func) {
	if (!func)
		return;
	free(func->name);
	for (int i = 0; i < func->param_count; i++)
		ast_param_free(func->params[i]);
	free(func->params);
	for (int i = 0; i < func->return_type_count; i++)
		ast_type_free(func->return_types[i]);
	free(func->return_types);
	for (int i = 0; i < func->stmt_count; i++)
		ast_stmt_free(func->stmts[i]);
	free(func->stmts);
	free(func);
}

static void ast_archetype_decl_free(AstArchetypeDecl *arch) {
	if (!arch)
		return;
	free(arch->name);
	for (int i = 0; i < arch->field_count; i++)
		ast_field_free(arch->fields[i]);
	free(arch->fields);
	free(arch);
}

static void ast_static_decl_free(AstStaticDecl *s) {
	if (!s)
		return;
	if (s->kind == AST_STATIC_ARCHETYPE) {
		free(s->archetype.archetype_name);
		for (int i = 0; i < s->archetype.field_count; i++) {
			free(s->archetype.field_names[i]);
			ast_expr_free(s->archetype.field_values[i]);
		}
		free(s->archetype.field_names);
		free(s->archetype.field_values);
		ast_expr_free(s->archetype.init_length);
	} else {
		free(s->array.name);
		ast_type_free(s->array.element_type);
	}
	free(s);
}

void ast_decl_free(AstDecl *decl) {
	if (!decl)
		return;
	switch (decl->kind) {
	case AST_DECL_WORLD:
		if (decl->data.world) {
			free(decl->data.world->name);
			free(decl->data.world);
		}
		break;
	case AST_DECL_ARCHETYPE:
		ast_archetype_decl_free(decl->data.archetype);
		break;
	case AST_DECL_PROC:
		ast_proc_decl_free(decl->data.proc);
		break;
	case AST_DECL_SYS:
		ast_sys_decl_free(decl->data.sys);
		break;
	case AST_DECL_FUNC:
		ast_func_decl_free(decl->data.func);
		break;
	case AST_DECL_FUNC_GROUP: {
		AstFuncGroupDecl *g = decl->data.func_group;
		if (g) {
			free(g->name);
			for (int i = 0; i < g->member_count; i++)
				free(g->member_names[i]);
			free(g->member_names);
			free(g);
		}
		break;
	}
	case AST_DECL_STATIC:
		ast_static_decl_free(decl->data.static_decl);
		break;
	case AST_DECL_CONST:
		if (decl->data.constant) {
			free(decl->data.constant->name);
			ast_expr_free(decl->data.constant->value);
			ast_type_free(decl->data.constant->type);
			free(decl->data.constant);
		}
		break;
	}
	free(decl);
}

void ast_program_free(AstProgram *prog) {
	if (!prog)
		return;
	for (int i = 0; i < prog->decl_count; i++)
		ast_decl_free(prog->decls[i]);
	free(prog->decls);
	free(prog);
}
