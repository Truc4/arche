#include "hir.h"
#include <stdlib.h>
#include <string.h>

HirProgram *hir_program_create(void) {
	HirProgram *prog = calloc(1, sizeof(HirProgram));
	return prog;
}

HirDecl *hir_decl_create(HirDeclKind kind) {
	HirDecl *decl = calloc(1, sizeof(HirDecl));
	decl->kind = kind;
	return decl;
}

HirStmt *hir_stmt_create(HirStmtKind kind) {
	HirStmt *stmt = calloc(1, sizeof(HirStmt));
	stmt->kind = kind;
	return stmt;
}

HirExpr *hir_expr_create(HirExprKind kind) {
	HirExpr *expr = calloc(1, sizeof(HirExpr));
	expr->kind = kind;
	return expr;
}

HirType *hir_type_create(HirTypeTag tag) {
	HirType *type = calloc(1, sizeof(HirType));
	type->tag = tag;
	if (tag == HIR_TYPE_INT) {
		type->int_width = 32;
		type->int_signed = 1;
	}
	return type;
}

HirField *hir_field_create(FieldKind kind, char *name, HirType *type) {
	HirField *field = calloc(1, sizeof(HirField));
	field->kind = kind;
	field->name = name;
	field->type = type;
	return field;
}

HirParam *hir_param_create(char *name, HirType *type) {
	HirParam *param = calloc(1, sizeof(HirParam));
	param->name = name;
	param->type = type;
	return param;
}

/* =========================
   Destructors
   ========================= */

void hir_type_free(HirType *type) {
	if (!type)
		return;
	hir_type_free(type->elem);
	if (type->fields) {
		for (int i = 0; i < type->field_count; i++) {
			hir_type_free(type->fields[i].type);
		}
		free(type->fields);
	}
	free(type);
}

void hir_field_free(HirField *field) {
	if (!field)
		return;
	free(field->name);
	hir_type_free(field->type);
	free(field);
}

void hir_param_free(HirParam *param) {
	if (!param)
		return;
	free(param->name);
	hir_type_free(param->type);
	free(param);
}

void hir_expr_free(HirExpr *expr) {
	if (!expr)
		return;
	switch (expr->kind) {
	case HIR_EXPR_LITERAL:
		free(expr->data.literal.lexeme);
		break;
	case HIR_EXPR_NAME:
		free(expr->data.name.name);
		break;
	case HIR_EXPR_FIELD:
		hir_expr_free(expr->data.field.base);
		free(expr->data.field.field_name);
		break;
	case HIR_EXPR_INDEX:
		hir_expr_free(expr->data.index.base);
		for (int i = 0; i < expr->data.index.index_count; i++)
			hir_expr_free(expr->data.index.indices[i]);
		free(expr->data.index.indices);
		free(expr->data.index.policy);
		break;
	case HIR_EXPR_SLICE:
		hir_expr_free(expr->data.slice.base);
		hir_expr_free(expr->data.slice.lo);
		hir_expr_free(expr->data.slice.hi);
		free(expr->data.slice.policy);
		break;
	case HIR_EXPR_BINARY:
		hir_expr_free(expr->data.binary.left);
		hir_expr_free(expr->data.binary.right);
		break;
	case HIR_EXPR_UNARY:
		hir_expr_free(expr->data.unary.operand);
		break;
	case HIR_EXPR_CALL:
		hir_expr_free(expr->data.call.callee);
		for (int i = 0; i < expr->data.call.arg_count; i++)
			hir_expr_free(expr->data.call.args[i]);
		free(expr->data.call.args);
		break;
	case HIR_EXPR_ALLOC:
		free(expr->data.alloc.archetype_name);
		for (int i = 0; i < expr->data.alloc.field_count; i++) {
			free(expr->data.alloc.field_names[i]);
			hir_expr_free(expr->data.alloc.field_values[i]);
		}
		free(expr->data.alloc.field_names);
		free(expr->data.alloc.field_values);
		hir_expr_free(expr->data.alloc.init_length);
		break;
	case HIR_EXPR_ENTITY_LIT:
		free(expr->data.entity.type_name);
		for (int i = 0; i < expr->data.entity.field_count; i++) {
			free(expr->data.entity.field_names[i]);
			hir_expr_free(expr->data.entity.field_values[i]);
		}
		free(expr->data.entity.field_names);
		free(expr->data.entity.field_values);
		break;
	case HIR_EXPR_ARRAY_LITERAL:
		for (int i = 0; i < expr->data.array_literal.element_count; i++)
			hir_expr_free(expr->data.array_literal.elements[i]);
		free(expr->data.array_literal.elements);
		break;
	case HIR_EXPR_STRING:
		free(expr->data.string.value);
		break;
	}
	free(expr);
}

void hir_stmt_free(HirStmt *stmt) {
	if (!stmt)
		return;
	switch (stmt->kind) {
	case HIR_STMT_BIND:
		for (int i = 0; i < stmt->data.bind_stmt.name_count; i++)
			free(stmt->data.bind_stmt.names[i]);
		free(stmt->data.bind_stmt.names);
		hir_type_free(stmt->data.bind_stmt.type);
		hir_expr_free(stmt->data.bind_stmt.value);
		break;
	case HIR_STMT_ASSIGN:
		hir_expr_free(stmt->data.assign_stmt.target);
		hir_expr_free(stmt->data.assign_stmt.value);
		break;
	case HIR_STMT_FOR:
		free(stmt->data.for_stmt.var_name);
		hir_expr_free(stmt->data.for_stmt.iterable);
		hir_stmt_free(stmt->data.for_stmt.init);
		hir_expr_free(stmt->data.for_stmt.cond);
		hir_stmt_free(stmt->data.for_stmt.incr);
		for (int i = 0; i < stmt->data.for_stmt.body_count; i++)
			hir_stmt_free(stmt->data.for_stmt.body[i]);
		free(stmt->data.for_stmt.body);
		break;
	case HIR_STMT_IF:
		hir_expr_free(stmt->data.if_stmt.cond);
		for (int i = 0; i < stmt->data.if_stmt.then_count; i++)
			hir_stmt_free(stmt->data.if_stmt.then_body[i]);
		free(stmt->data.if_stmt.then_body);
		for (int i = 0; i < stmt->data.if_stmt.else_count; i++)
			hir_stmt_free(stmt->data.if_stmt.else_body[i]);
		free(stmt->data.if_stmt.else_body);
		break;
	case HIR_STMT_BREAK:
	case HIR_STMT_CONTINUE:
		break;
	case HIR_STMT_RUN:
		free(stmt->data.run_stmt.map_name);
		free(stmt->data.run_stmt.world_name);
		break;
	case HIR_STMT_EXPR:
		hir_expr_free(stmt->data.expr_stmt.expr);
		break;
	case HIR_STMT_RETURN:
		for (int i = 0; i < stmt->data.return_stmt.count; i++)
			hir_expr_free(stmt->data.return_stmt.values[i]);
		free(stmt->data.return_stmt.values);
		break;
	case HIR_STMT_MULTI_BIND:
		for (int i = 0; i < stmt->data.multi_bind.target_count; i++) {
			free(stmt->data.multi_bind.targets[i].name);
			hir_type_free(stmt->data.multi_bind.targets[i].type);
		}
		free(stmt->data.multi_bind.targets);
		hir_expr_free(stmt->data.multi_bind.value);
		break;
	case HIR_STMT_EACH_FIELD:
		free(stmt->data.each_field.binding_name);
		hir_type_free(stmt->data.each_field.filter_type);
		free(stmt->data.each_field.arch_param_name);
		for (int i = 0; i < stmt->data.each_field.body_count; i++) {
			hir_stmt_free(stmt->data.each_field.body[i]);
		}
		free(stmt->data.each_field.body);
		break;
	case HIR_STMT_BLOCK:
		for (int i = 0; i < stmt->data.block.count; i++)
			hir_stmt_free(stmt->data.block.stmts[i]);
		free(stmt->data.block.stmts);
		break;
	}
	free(stmt);
}

static void hir_proc_decl_free(HirProcDecl *proc) {
	if (!proc)
		return;
	free(proc->name);
	for (int i = 0; i < proc->param_count; i++)
		hir_param_free(proc->params[i]);
	free(proc->params);
	for (int i = 0; i < proc->out_param_count; i++)
		hir_param_free(proc->out_params[i]);
	free(proc->out_params);
	for (int i = 0; i < proc->stmt_count; i++)
		hir_stmt_free(proc->stmts[i]);
	free(proc->stmts);
	free(proc);
}

static void hir_map_decl_free(HirMapDecl *map) {
	if (!map)
		return;
	free(map->name);
	for (int i = 0; i < map->param_count; i++)
		hir_param_free(map->params[i]);
	free(map->params);
	for (int i = 0; i < map->stmt_count; i++)
		hir_stmt_free(map->stmts[i]);
	free(map->stmts);
	free(map);
}

static void hir_system_decl_free(HirSystemDecl *sys) {
	if (!sys)
		return;
	free(sys->name);
	for (int i = 0; i < sys->stmt_count; i++)
		hir_stmt_free(sys->stmts[i]);
	free(sys->stmts);
	free(sys);
}

static void hir_schedule_decl_free(HirScheduleDecl *sch) {
	if (!sch)
		return;
	for (int i = 0; i < sch->entry_count; i++)
		free(sch->entries[i]);
	free(sch->entries);
	free(sch);
}

void schedule_tree_free(ScheduleTree *t) {
	if (!t)
		return;
	for (int i = 0; i < t->child_count; i++)
		schedule_tree_free(t->children[i]);
	free(t->children);
	free(t->sym);
	free(t);
}

static void hir_run_decl_free(HirRunDecl *run) {
	if (!run)
		return;
	schedule_tree_free(run->tree);
	free(run);
}

static void hir_func_decl_free(HirFuncDecl *func) {
	if (!func)
		return;
	free(func->name);
	for (int i = 0; i < func->param_count; i++)
		hir_param_free(func->params[i]);
	free(func->params);
	for (int i = 0; i < func->return_type_count; i++)
		hir_type_free(func->return_types[i]);
	free(func->return_types);
	for (int i = 0; i < func->stmt_count; i++)
		hir_stmt_free(func->stmts[i]);
	free(func->stmts);
	free(func);
}

static void hir_archetype_decl_free(HirArchetypeDecl *arch) {
	if (!arch)
		return;
	free(arch->name);
	for (int i = 0; i < arch->field_count; i++)
		hir_field_free(arch->fields[i]);
	free(arch->fields);
	free(arch);
}

static void hir_static_decl_free(HirStaticDecl *s) {
	if (!s)
		return;
	if (s->kind == HIR_STATIC_ARCHETYPE) {
		free(s->archetype.archetype_name);
		for (int i = 0; i < s->archetype.field_count; i++) {
			free(s->archetype.field_names[i]);
			hir_expr_free(s->archetype.field_values[i]);
		}
		free(s->archetype.field_names);
		free(s->archetype.field_values);
		hir_expr_free(s->archetype.init_length);
	} else if (s->kind == HIR_STATIC_SCALAR) {
		free(s->scalar.name);
		hir_type_free(s->scalar.type);
		hir_expr_free(s->scalar.init);
	} else {
		free(s->array.name);
		hir_type_free(s->array.element_type);
		hir_expr_free(s->array.init);
	}
	free(s);
}

void hir_decl_free(HirDecl *decl) {
	if (!decl)
		return;
	switch (decl->kind) {
	case HIR_DECL_WORLD:
		if (decl->data.world) {
			free(decl->data.world->name);
			free(decl->data.world);
		}
		break;
	case HIR_DECL_ARCHETYPE:
		hir_archetype_decl_free(decl->data.archetype);
		break;
	case HIR_DECL_PROC:
		hir_proc_decl_free(decl->data.proc);
		break;
	case HIR_DECL_QUERY:
		if (decl->data.query) {
			free(decl->data.query->name);
			for (int i = 0; i < decl->data.query->col_count; i++)
				free(decl->data.query->cols[i]);
			free(decl->data.query->cols);
			free(decl->data.query);
		}
		break;
	case HIR_DECL_MAP:
		hir_map_decl_free(decl->data.map);
		break;
	case HIR_DECL_FUNC:
		hir_func_decl_free(decl->data.func);
		break;
	case HIR_DECL_FUNC_GROUP: {
		HirFuncGroupDecl *g = decl->data.func_group;
		if (g) {
			free(g->name);
			for (int i = 0; i < g->member_count; i++)
				free(g->member_names[i]);
			free(g->member_names);
			free(g);
		}
		break;
	}
	case HIR_DECL_STATIC:
		hir_static_decl_free(decl->data.static_decl);
		break;
	case HIR_DECL_CONST:
		if (decl->data.constant) {
			free(decl->data.constant->name);
			hir_expr_free(decl->data.constant->value);
			hir_type_free(decl->data.constant->type);
			free(decl->data.constant);
		}
		break;
	case HIR_DECL_DEFAULT:
		if (decl->data.default_decl) {
			free(decl->data.default_decl->policy);
			free(decl->data.default_decl);
		}
		break;
	case HIR_DECL_SYSTEM:
		hir_system_decl_free(decl->data.system);
		break;
	case HIR_DECL_SCHEDULE:
		hir_schedule_decl_free(decl->data.schedule);
		break;
	case HIR_DECL_RUN:
		hir_run_decl_free(decl->data.run);
		break;
	}
	free(decl);
}

void hir_program_free(HirProgram *prog) {
	if (!prog)
		return;
	for (int i = 0; i < prog->decl_count; i++)
		hir_decl_free(prog->decls[i]);
	free(prog->decls);
	free(prog);
}
