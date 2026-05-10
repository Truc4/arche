#include "lower.h"
#include <stdlib.h>
#include <string.h>

/* =========================
   Type mapping
   ========================= */

static AstType map_type_str(const char *resolved_type) {
	AstType t = {0};
	if (!resolved_type) {
		t.tag = AST_TYPE_UNKNOWN;
		return t;
	}
	if (strcmp(resolved_type, "int") == 0) {
		t.tag = AST_TYPE_INT;
	} else if (strcmp(resolved_type, "float") == 0 || strcmp(resolved_type, "double") == 0) {
		t.tag = AST_TYPE_FLOAT;
	} else if (strcmp(resolved_type, "char") == 0) {
		t.tag = AST_TYPE_CHAR;
	} else if (strcmp(resolved_type, "void") == 0) {
		t.tag = AST_TYPE_VOID;
	} else if (strcmp(resolved_type, "char_array") == 0) {
		t.tag = AST_TYPE_CHAR_ARRAY;
	} else if (strcmp(resolved_type, "handle") == 0) {
		t.tag = AST_TYPE_HANDLE;
	} else {
		/* archetype or other named type — pointer into CST, safe since CST outlives AST */
		t.tag = AST_TYPE_NAMED;
		t.name = resolved_type;
	}
	return t;
}

static AstType *lower_type_ref(TypeRef *tr) {
	if (!tr)
		return NULL;
	AstType *t = ast_type_create(AST_TYPE_UNKNOWN);
	switch (tr->kind) {
	case TYPE_NAME:
		*t = map_type_str(tr->data.name);
		break;
	case TYPE_ARRAY:
		t->tag = AST_TYPE_ARRAY;
		t->elem = lower_type_ref(tr->data.array.element_type);
		break;
	case TYPE_SHAPED_ARRAY:
		t->tag = AST_TYPE_SHAPED_ARRAY;
		t->elem = lower_type_ref(tr->data.shaped_array.element_type);
		t->rank = tr->data.shaped_array.rank;
		break;
	case TYPE_TUPLE: {
		t->tag = AST_TYPE_TUPLE;
		t->field_count = tr->data.tuple.field_count;
		t->fields = calloc(t->field_count, sizeof(AstTupleField));
		for (int i = 0; i < t->field_count; i++) {
			t->fields[i].name = tr->data.tuple.field_names[i];
			t->fields[i].type = lower_type_ref(tr->data.tuple.field_types[i]);
		}
		break;
	}
	case TYPE_HANDLE:
		t->tag = AST_TYPE_HANDLE;
		t->name = tr->data.handle.archetype_name;
		break;
	}
	return t;
}

/* =========================
   Expression lowering
   ========================= */

static AstExpr *lower_expr(Expression *expr) {
	if (!expr)
		return NULL;
	AstExpr *e = ast_expr_create(AST_EXPR_LITERAL);
	e->loc = expr->loc;
	e->resolved = map_type_str(expr->resolved_type);

	switch (expr->type) {
	case EXPR_LITERAL: {
		e->kind = AST_EXPR_LITERAL;
		e->data.literal.lexeme = malloc(strlen(expr->data.literal.lexeme) + 1);
		strcpy(e->data.literal.lexeme, expr->data.literal.lexeme);
		break;
	}
	case EXPR_NAME: {
		e->kind = AST_EXPR_NAME;
		e->data.name.name = malloc(strlen(expr->data.name.name) + 1);
		strcpy(e->data.name.name, expr->data.name.name);
		break;
	}
	case EXPR_FIELD: {
		e->kind = AST_EXPR_FIELD;
		e->data.field.base = lower_expr(expr->data.field.base);
		e->data.field.field_name = malloc(strlen(expr->data.field.field_name) + 1);
		strcpy(e->data.field.field_name, expr->data.field.field_name);
		break;
	}
	case EXPR_INDEX: {
		e->kind = AST_EXPR_INDEX;
		e->data.index.base = lower_expr(expr->data.index.base);
		e->data.index.index_count = expr->data.index.index_count;
		e->data.index.indices = calloc(e->data.index.index_count, sizeof(AstExpr *));
		for (int i = 0; i < expr->data.index.index_count; i++)
			e->data.index.indices[i] = lower_expr(expr->data.index.indices[i]);
		break;
	}
	case EXPR_BINARY: {
		e->kind = AST_EXPR_BINARY;
		e->data.binary.op = expr->data.binary.op;
		e->data.binary.left = lower_expr(expr->data.binary.left);
		e->data.binary.right = lower_expr(expr->data.binary.right);
		break;
	}
	case EXPR_UNARY: {
		e->kind = AST_EXPR_UNARY;
		e->data.unary.op = expr->data.unary.op;
		e->data.unary.operand = lower_expr(expr->data.unary.operand);
		break;
	}
	case EXPR_CALL: {
		e->kind = AST_EXPR_CALL;
		e->data.call.callee = lower_expr(expr->data.call.callee);
		e->data.call.arg_count = expr->data.call.arg_count;
		e->data.call.args = calloc(e->data.call.arg_count, sizeof(AstExpr *));
		for (int i = 0; i < expr->data.call.arg_count; i++)
			e->data.call.args[i] = lower_expr(expr->data.call.args[i]);
		break;
	}
	case EXPR_ALLOC: {
		e->kind = AST_EXPR_ALLOC;
		e->data.alloc.archetype_name = malloc(strlen(expr->data.alloc.archetype_name) + 1);
		strcpy(e->data.alloc.archetype_name, expr->data.alloc.archetype_name);
		e->data.alloc.field_count = expr->data.alloc.field_count;
		e->data.alloc.field_names = calloc(e->data.alloc.field_count, sizeof(char *));
		e->data.alloc.field_values = calloc(e->data.alloc.field_count, sizeof(AstExpr *));
		for (int i = 0; i < expr->data.alloc.field_count; i++) {
			if (expr->data.alloc.field_names[i]) {
				e->data.alloc.field_names[i] = malloc(strlen(expr->data.alloc.field_names[i]) + 1);
				strcpy(e->data.alloc.field_names[i], expr->data.alloc.field_names[i]);
			}
			e->data.alloc.field_values[i] = lower_expr(expr->data.alloc.field_values[i]);
		}
		e->data.alloc.init_length = lower_expr(expr->data.alloc.init_length);
		break;
	}
	case EXPR_ARRAY_LITERAL: {
		e->kind = AST_EXPR_ARRAY_LITERAL;
		e->data.array_literal.element_count = expr->data.array_literal.element_count;
		e->data.array_literal.elements = calloc(e->data.array_literal.element_count, sizeof(AstExpr *));
		for (int i = 0; i < expr->data.array_literal.element_count; i++)
			e->data.array_literal.elements[i] = lower_expr(expr->data.array_literal.elements[i]);
		break;
	}
	case EXPR_STRING: {
		e->kind = AST_EXPR_STRING;
		e->data.string.length = expr->data.string.length;
		if (expr->data.string.value) {
			e->data.string.value = malloc(expr->data.string.length + 1);
			memcpy(e->data.string.value, expr->data.string.value, expr->data.string.length + 1);
		}
		break;
	}
	}
	return e;
}

/* =========================
   Statement lowering
   ========================= */

static AstStmt *lower_stmt(Statement *stmt);

static AstStmt **lower_body(Statement **body, int count) {
	if (count == 0)
		return NULL;
	AstStmt **out = calloc(count, sizeof(AstStmt *));
	for (int i = 0; i < count; i++)
		out[i] = lower_stmt(body[i]);
	return out;
}

static AstStmt *lower_stmt(Statement *stmt) {
	if (!stmt)
		return NULL;
	AstStmt *s = ast_stmt_create(AST_STMT_EXPR);
	s->loc = stmt->loc;

	switch (stmt->type) {
	case STMT_LET: {
		s->kind = AST_STMT_LET;
		LetStmt *ls = &stmt->data.let_stmt;
		if (ls->name_count > 0 && ls->names) {
			/* multi-value let: copy names[] */
			s->data.let_stmt.name_count = ls->name_count;
			s->data.let_stmt.names = calloc(ls->name_count, sizeof(char *));
			for (int i = 0; i < ls->name_count; i++) {
				s->data.let_stmt.names[i] = malloc(strlen(ls->names[i]) + 1);
				strcpy(s->data.let_stmt.names[i], ls->names[i]);
			}
		} else {
			/* single-var let: normalize to names[0] */
			s->data.let_stmt.name_count = 1;
			s->data.let_stmt.names = calloc(1, sizeof(char *));
			s->data.let_stmt.names[0] = malloc(strlen(ls->name) + 1);
			strcpy(s->data.let_stmt.names[0], ls->name);
		}
		s->data.let_stmt.type = lower_type_ref(ls->type);
		s->data.let_stmt.value = lower_expr(ls->value);
		break;
	}
	case STMT_ASSIGN: {
		s->kind = AST_STMT_ASSIGN;
		s->data.assign_stmt.op = stmt->data.assign_stmt.op;
		s->data.assign_stmt.target = lower_expr(stmt->data.assign_stmt.target);
		s->data.assign_stmt.value = lower_expr(stmt->data.assign_stmt.value);
		break;
	}
	case STMT_FOR: {
		s->kind = AST_STMT_FOR;
		ForStmt *fs = &stmt->data.for_stmt;
		AstForStmt *afs = &s->data.for_stmt;

		if (fs->var_name) {
			afs->kind = AST_FOR_RANGE;
			afs->range.var_name = malloc(strlen(fs->var_name) + 1);
			strcpy(afs->range.var_name, fs->var_name);
			afs->range.iterable = lower_expr(fs->iterable);
		} else if (fs->init || fs->increment) {
			afs->kind = AST_FOR_C_STYLE;
			afs->c_style.init = lower_stmt(fs->init);
			afs->c_style.cond = lower_expr(fs->condition);
			afs->c_style.incr = lower_stmt(fs->increment);
		} else if (fs->condition) {
			afs->kind = AST_FOR_WHILE;
			afs->while_loop.cond = lower_expr(fs->condition);
		} else {
			afs->kind = AST_FOR_INFINITE;
		}
		afs->body_count = fs->body_count;
		afs->body = lower_body(fs->body, fs->body_count);
		break;
	}
	case STMT_IF: {
		s->kind = AST_STMT_IF;
		IfStmt *is = &stmt->data.if_stmt;
		s->data.if_stmt.cond = lower_expr(is->cond);
		s->data.if_stmt.then_count = is->then_count;
		s->data.if_stmt.then_body = lower_body(is->then_body, is->then_count);
		s->data.if_stmt.else_count = is->else_count;
		s->data.if_stmt.else_body = lower_body(is->else_body, is->else_count);
		break;
	}
	case STMT_BREAK:
		s->kind = AST_STMT_BREAK;
		break;
	case STMT_RUN: {
		s->kind = AST_STMT_RUN;
		RunStmt *rs = &stmt->data.run_stmt;
		s->data.run_stmt.system_name = malloc(strlen(rs->system_name) + 1);
		strcpy(s->data.run_stmt.system_name, rs->system_name);
		if (rs->world_name) {
			s->data.run_stmt.world_name = malloc(strlen(rs->world_name) + 1);
			strcpy(s->data.run_stmt.world_name, rs->world_name);
		}
		break;
	}
	case STMT_EXPR: {
		s->kind = AST_STMT_EXPR;
		s->data.expr_stmt.expr = lower_expr(stmt->data.expr_stmt.expr);
		break;
	}
	case STMT_FREE: {
		s->kind = AST_STMT_FREE;
		s->data.free_stmt.value = lower_expr(stmt->data.free_stmt.value);
		break;
	}
	case STMT_RETURN: {
		s->kind = AST_STMT_RETURN;
		s->data.return_stmt.value = lower_expr(stmt->data.return_stmt.value);
		break;
	}
	case STMT_MULTI_BIND: {
		s->kind = AST_STMT_MULTI_BIND;
		MultiBindStmt *mb = &stmt->data.multi_bind;
		s->data.multi_bind.target_count = mb->target_count;
		s->data.multi_bind.from_shorthand = mb->from_shorthand;
		s->data.multi_bind.targets = calloc(mb->target_count, sizeof(AstBindingTarget));
		for (int i = 0; i < mb->target_count; i++) {
			s->data.multi_bind.targets[i].is_new = mb->targets[i].is_new;
			s->data.multi_bind.targets[i].name = malloc(strlen(mb->targets[i].name) + 1);
			strcpy(s->data.multi_bind.targets[i].name, mb->targets[i].name);
			s->data.multi_bind.targets[i].type = lower_type_ref(mb->targets[i].type);
		}
		s->data.multi_bind.value = lower_expr(mb->value);
		break;
	}
	}
	return s;
}

/* =========================
   Param / field lowering
   ========================= */

static AstParam *lower_param(Parameter *p) {
	AstParam *ap = ast_param_create(NULL, NULL);
	ap->loc = p->loc;
	ap->is_out = p->is_out;
	ap->name = malloc(strlen(p->name) + 1);
	strcpy(ap->name, p->name);
	ap->type = lower_type_ref(p->type);
	return ap;
}

static AstField *lower_field(FieldDecl *f) {
	AstField *af = ast_field_create(f->kind, NULL, NULL);
	af->loc = f->loc;
	af->name = malloc(strlen(f->name) + 1);
	strcpy(af->name, f->name);
	af->type = lower_type_ref(f->type);
	return af;
}

/* =========================
   Declaration lowering
   ========================= */

static AstDecl *lower_decl(Decl *decl) {
	AstDecl *ad = NULL;

	switch (decl->kind) {
	case DECL_USE:
		return NULL; /* stripped */

	case DECL_WORLD: {
		ad = ast_decl_create(AST_DECL_WORLD);
		ad->loc = decl->loc;
		ad->data.world = calloc(1, sizeof(AstWorldDecl));
		ad->data.world->name = malloc(strlen(decl->data.world->name) + 1);
		strcpy(ad->data.world->name, decl->data.world->name);
		break;
	}
	case DECL_ARCHETYPE: {
		ad = ast_decl_create(AST_DECL_ARCHETYPE);
		ad->loc = decl->loc;
		ArchetypeDecl *arch = decl->data.archetype;
		AstArchetypeDecl *aarch = calloc(1, sizeof(AstArchetypeDecl));
		aarch->name = malloc(strlen(arch->name) + 1);
		strcpy(aarch->name, arch->name);
		aarch->field_count = arch->field_count;
		aarch->fields = calloc(arch->field_count, sizeof(AstField *));
		for (int i = 0; i < arch->field_count; i++)
			aarch->fields[i] = lower_field(arch->fields[i]);
		ad->data.archetype = aarch;
		break;
	}
	case DECL_PROC: {
		ad = ast_decl_create(AST_DECL_PROC);
		ad->loc = decl->loc;
		ProcDecl *proc = decl->data.proc;
		AstProcDecl *aproc = calloc(1, sizeof(AstProcDecl));
		aproc->loc = proc->loc;
		aproc->is_extern = proc->is_extern;
		aproc->name = malloc(strlen(proc->name) + 1);
		strcpy(aproc->name, proc->name);
		aproc->param_count = proc->param_count;
		aproc->params = calloc(proc->param_count, sizeof(AstParam *));
		for (int i = 0; i < proc->param_count; i++)
			aproc->params[i] = lower_param(proc->params[i]);
		aproc->stmt_count = proc->statement_count;
		aproc->stmts = calloc(proc->statement_count, sizeof(AstStmt *));
		for (int i = 0; i < proc->statement_count; i++)
			aproc->stmts[i] = lower_stmt(proc->statements[i]);
		ad->data.proc = aproc;
		break;
	}
	case DECL_SYS: {
		ad = ast_decl_create(AST_DECL_SYS);
		ad->loc = decl->loc;
		SysDecl *sys = decl->data.sys;
		AstSysDecl *asys = calloc(1, sizeof(AstSysDecl));
		asys->loc = sys->loc;
		asys->name = malloc(strlen(sys->name) + 1);
		strcpy(asys->name, sys->name);
		asys->param_count = sys->param_count;
		asys->params = calloc(sys->param_count, sizeof(AstParam *));
		for (int i = 0; i < sys->param_count; i++)
			asys->params[i] = lower_param(sys->params[i]);
		asys->stmt_count = sys->statement_count;
		asys->stmts = calloc(sys->statement_count, sizeof(AstStmt *));
		for (int i = 0; i < sys->statement_count; i++)
			asys->stmts[i] = lower_stmt(sys->statements[i]);
		ad->data.sys = asys;
		break;
	}
	case DECL_FUNC: {
		ad = ast_decl_create(AST_DECL_FUNC);
		ad->loc = decl->loc;
		FuncDecl *func = decl->data.func;
		AstFuncDecl *afunc = calloc(1, sizeof(AstFuncDecl));
		afunc->loc = func->loc;
		afunc->is_extern = func->is_extern;
		afunc->name = malloc(strlen(func->name) + 1);
		strcpy(afunc->name, func->name);
		afunc->param_count = func->param_count;
		afunc->params = calloc(func->param_count, sizeof(AstParam *));
		for (int i = 0; i < func->param_count; i++)
			afunc->params[i] = lower_param(func->params[i]);
		afunc->return_type = lower_type_ref(func->return_type);
		afunc->stmt_count = func->statement_count;
		afunc->stmts = calloc(func->statement_count, sizeof(AstStmt *));
		for (int i = 0; i < func->statement_count; i++)
			afunc->stmts[i] = lower_stmt(func->statements[i]);
		ad->data.func = afunc;
		break;
	}
	case DECL_STATIC: {
		ad = ast_decl_create(AST_DECL_STATIC);
		ad->loc = decl->loc;
		StaticDecl *sd = decl->data.static_decl;
		AstStaticDecl *asd = calloc(1, sizeof(AstStaticDecl));
		if (sd->kind == STATIC_KIND_ARCHETYPE) {
			asd->kind = AST_STATIC_ARCHETYPE;
			asd->archetype.archetype_name = malloc(strlen(sd->archetype.archetype_name) + 1);
			strcpy(asd->archetype.archetype_name, sd->archetype.archetype_name);
			asd->archetype.field_count = sd->archetype.field_count;
			asd->archetype.field_names = calloc(sd->archetype.field_count, sizeof(char *));
			asd->archetype.field_values = calloc(sd->archetype.field_count, sizeof(AstExpr *));
			for (int i = 0; i < sd->archetype.field_count; i++) {
				if (sd->archetype.field_names[i]) {
					asd->archetype.field_names[i] = malloc(strlen(sd->archetype.field_names[i]) + 1);
					strcpy(asd->archetype.field_names[i], sd->archetype.field_names[i]);
				}
				asd->archetype.field_values[i] = lower_expr(sd->archetype.field_values[i]);
			}
			asd->archetype.init_length = lower_expr(sd->archetype.init_length);
		} else {
			asd->kind = AST_STATIC_ARRAY;
			asd->array.name = malloc(strlen(sd->array.name) + 1);
			strcpy(asd->array.name, sd->array.name);
			asd->array.element_type = lower_type_ref(sd->array.element_type);
			asd->array.size = sd->array.size;
		}
		ad->data.static_decl = asd;
		break;
	}
	case DECL_CONST: {
		ad = ast_decl_create(AST_DECL_CONST);
		ad->loc = decl->loc;
		ConstDecl *cd = decl->data.constant;
		AstConstDecl *acd = calloc(1, sizeof(AstConstDecl));
		acd->name = malloc(strlen(cd->name) + 1);
		strcpy(acd->name, cd->name);
		acd->value = lower_expr(cd->value);
		ad->data.constant = acd;
		break;
	}
	}
	return ad;
}

/* =========================
   Top-level entry point
   ========================= */

AstProgram *lower_cst_to_ast(Program *cst) {
	if (!cst)
		return NULL;

	AstProgram *ast = ast_program_create();
	ast->loc = cst->loc;

	/* count non-USE decls */
	int out_count = 0;
	for (int i = 0; i < cst->decl_count; i++) {
		if (cst->decls[i]->kind != DECL_USE)
			out_count++;
	}

	ast->decls = calloc(out_count, sizeof(AstDecl *));
	ast->decl_count = 0;

	for (int i = 0; i < cst->decl_count; i++) {
		AstDecl *ad = lower_decl(cst->decls[i]);
		if (ad)
			ast->decls[ast->decl_count++] = ad;
	}

	return ast;
}
