#include "lower.h"
#include <stdlib.h>
#include <string.h>

/* =========================
   Type mapping
   ========================= */

/* Recognize a fixed-width integer type name (byte, i8/u8 .. i64/u64, i128/u128).
 * Returns 1 and fills width/signed on match. */
int ast_parse_int_width(const char *s, int *width, int *is_signed) {
	if (!s)
		return 0;
	if (strcmp(s, "byte") == 0) {
		*width = 8;
		*is_signed = 0;
		return 1;
	}
	if (s[0] != 'i' && s[0] != 'u')
		return 0;
	int sign = (s[0] == 'i') ? 1 : 0;
	const char *num = s + 1;
	int w;
	if (strcmp(num, "8") == 0)
		w = 8;
	else if (strcmp(num, "16") == 0)
		w = 16;
	else if (strcmp(num, "32") == 0)
		w = 32;
	else if (strcmp(num, "64") == 0)
		w = 64;
	else if (strcmp(num, "128") == 0)
		w = 128;
	else
		return 0;
	*width = w;
	*is_signed = sign;
	return 1;
}

static AstType map_type_str(const char *resolved_type) {
	AstType t = {0};
	if (!resolved_type) {
		t.tag = AST_TYPE_UNKNOWN;
		return t;
	}
	int w, sg;
	if (strcmp(resolved_type, "int") == 0) {
		t.tag = AST_TYPE_INT;
		t.int_width = 32;
		t.int_signed = 1;
	} else if (ast_parse_int_width(resolved_type, &w, &sg)) {
		t.tag = AST_TYPE_INT;
		t.int_width = w;
		t.int_signed = sg;
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
	} else if (strcmp(resolved_type, "opaque") == 0) {
		t.tag = AST_TYPE_OPAQUE;
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
	case TYPE_ARCHETYPE:
		t->tag = AST_TYPE_ARCHETYPE;
		break;
	case TYPE_OPAQUE:
		t->tag = AST_TYPE_OPAQUE;
		break;
	case TYPE_TYPE:
		/* The meta-type is compile-time only; a type alias carrying it is erased before
		 * lowering, so this should never reach codegen. Leave AST_TYPE_UNKNOWN defensively. */
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
	case STMT_BIND: {
		BindStmt *ls = &stmt->data.bind_stmt;
		/* A local type alias (`V :: float`) is compile-time only — the alias is erased and its
		 * references resolve to the backing. It declares no runtime value, so emit a harmless
		 * no-op expression statement (`0;`) in its place. */
		if (ls->is_type_alias) {
			s->kind = AST_STMT_EXPR;
			AstExpr *zero = ast_expr_create(AST_EXPR_LITERAL);
			zero->data.literal.lexeme = malloc(2);
			strcpy(zero->data.literal.lexeme, "0");
			s->data.expr_stmt.expr = zero;
			break;
		}
		s->kind = AST_STMT_BIND;
		if (ls->name_count > 0 && ls->names) {
			/* multi-value let: copy names[] */
			s->data.bind_stmt.name_count = ls->name_count;
			s->data.bind_stmt.names = calloc(ls->name_count, sizeof(char *));
			for (int i = 0; i < ls->name_count; i++) {
				s->data.bind_stmt.names[i] = malloc(strlen(ls->names[i]) + 1);
				strcpy(s->data.bind_stmt.names[i], ls->names[i]);
			}
		} else {
			/* single-var let: normalize to names[0] */
			s->data.bind_stmt.name_count = 1;
			s->data.bind_stmt.names = calloc(1, sizeof(char *));
			s->data.bind_stmt.names[0] = malloc(strlen(ls->name) + 1);
			strcpy(s->data.bind_stmt.names[0], ls->name);
		}
		s->data.bind_stmt.type = lower_type_ref(ls->type);
		s->data.bind_stmt.value = lower_expr(ls->value);
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
			afs->var_name = malloc(strlen(fs->var_name) + 1);
			strcpy(afs->var_name, fs->var_name);
		}
		afs->iterable = lower_expr(fs->iterable);
		afs->init = lower_stmt(fs->init);
		afs->cond = lower_expr(fs->condition);
		afs->incr = lower_stmt(fs->increment);
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
		s->data.return_stmt.count = stmt->data.return_stmt.count;
		s->data.return_stmt.values = calloc(stmt->data.return_stmt.count, sizeof(AstExpr *));
		for (int i = 0; i < stmt->data.return_stmt.count; i++)
			s->data.return_stmt.values[i] = lower_expr(stmt->data.return_stmt.values[i]);
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
	case STMT_EACH_FIELD: {
		s->kind = AST_STMT_EACH_FIELD;
		EachFieldStmt *ef = &stmt->data.each_field;
		s->data.each_field.binding_name = malloc(strlen(ef->binding_name) + 1);
		strcpy(s->data.each_field.binding_name, ef->binding_name);
		s->data.each_field.filter_type = lower_type_ref(ef->filter_type);
		s->data.each_field.arch_param_name = malloc(strlen(ef->arch_param_name) + 1);
		strcpy(s->data.each_field.arch_param_name, ef->arch_param_name);
		s->data.each_field.body_count = ef->body_count;
		s->data.each_field.body = calloc(ef->body_count, sizeof(AstStmt *));
		for (int i = 0; i < ef->body_count; i++) {
			s->data.each_field.body[i] = lower_stmt(ef->body[i]);
		}
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
	ap->is_move = p->is_move;
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
   Tuple desugaring (CST -> AST)
   =========================
   Tuples exist only in the CST. Lowering flattens every tuple field
   `pos: (x, y)` into scalar columns `pos_x`, `pos_y`, and rewrites system
   parameters and bodies accordingly, so the AST (and every later pass) never
   sees a tuple. The registry records each tuple field's components, collected
   from the CST's archetype declarations. */

typedef struct {
	char *base;      /* tuple field name, e.g. "pos" (borrowed from CST) */
	char **comp;     /* component names, e.g. {"x","y"} (borrowed) */
	TypeRef **ctype; /* component types (borrowed) */
	int n;
} TupleFieldInfo;

static TupleFieldInfo *g_tuples = NULL;
static int g_tuple_count = 0;
static int g_tuple_cap = 0;

static void build_tuple_registry(Program *cst) {
	g_tuples = NULL;
	g_tuple_count = 0;
	g_tuple_cap = 0;
	for (int i = 0; i < cst->decl_count; i++) {
		if (cst->decls[i]->kind != DECL_ARCHETYPE)
			continue;
		ArchetypeDecl *arch = cst->decls[i]->data.archetype;
		for (int f = 0; f < arch->field_count; f++) {
			FieldDecl *fd = arch->fields[f];
			if (!fd->type || fd->type->kind != TYPE_TUPLE)
				continue;
			int seen = 0;
			for (int k = 0; k < g_tuple_count; k++)
				if (strcmp(g_tuples[k].base, fd->name) == 0) {
					seen = 1;
					break;
				}
			if (seen)
				continue;
			if (g_tuple_count == g_tuple_cap) {
				g_tuple_cap = g_tuple_cap ? g_tuple_cap * 2 : 8;
				g_tuples = realloc(g_tuples, g_tuple_cap * sizeof(TupleFieldInfo));
			}
			TupleFieldInfo *ti = &g_tuples[g_tuple_count++];
			ti->base = fd->name;
			ti->n = fd->type->data.tuple.field_count;
			ti->comp = fd->type->data.tuple.field_names;
			ti->ctype = fd->type->data.tuple.field_types;
		}
	}
}

static TupleFieldInfo *lookup_tuple(const char *name) {
	for (int i = 0; i < g_tuple_count; i++)
		if (strcmp(g_tuples[i].base, name) == 0)
			return &g_tuples[i];
	return NULL;
}

/* Rewrite `base.comp` (AST_EXPR_FIELD over NAME base) into NAME `base_comp`. */
static void tuple_rewrite_expr(AstExpr *e, const char *base) {
	if (!e)
		return;
	switch (e->kind) {
	case AST_EXPR_FIELD:
		tuple_rewrite_expr(e->data.field.base, base);
		if (e->data.field.base && e->data.field.base->kind == AST_EXPR_NAME &&
		    strcmp(e->data.field.base->data.name.name, base) == 0) {
			const char *sub = e->data.field.field_name;
			char *combined = malloc(strlen(base) + strlen(sub) + 2);
			sprintf(combined, "%s_%s", base, sub);
			e->kind = AST_EXPR_NAME;
			e->data.name.name = combined; /* old base node intentionally leaked */
		}
		break;
	case AST_EXPR_INDEX:
		tuple_rewrite_expr(e->data.index.base, base);
		for (int i = 0; i < e->data.index.index_count; i++)
			tuple_rewrite_expr(e->data.index.indices[i], base);
		break;
	case AST_EXPR_BINARY:
		tuple_rewrite_expr(e->data.binary.left, base);
		tuple_rewrite_expr(e->data.binary.right, base);
		break;
	case AST_EXPR_UNARY:
		tuple_rewrite_expr(e->data.unary.operand, base);
		break;
	case AST_EXPR_CALL:
		tuple_rewrite_expr(e->data.call.callee, base);
		for (int i = 0; i < e->data.call.arg_count; i++)
			tuple_rewrite_expr(e->data.call.args[i], base);
		break;
	default:
		break;
	}
}

static void tuple_rewrite_stmt(AstStmt *s, const char *base) {
	if (!s)
		return;
	switch (s->kind) {
	case AST_STMT_BIND:
		tuple_rewrite_expr(s->data.bind_stmt.value, base);
		break;
	case AST_STMT_ASSIGN:
		tuple_rewrite_expr(s->data.assign_stmt.target, base);
		tuple_rewrite_expr(s->data.assign_stmt.value, base);
		break;
	case AST_STMT_FOR:
		tuple_rewrite_stmt(s->data.for_stmt.init, base);
		tuple_rewrite_expr(s->data.for_stmt.cond, base);
		tuple_rewrite_stmt(s->data.for_stmt.incr, base);
		for (int i = 0; i < s->data.for_stmt.body_count; i++)
			tuple_rewrite_stmt(s->data.for_stmt.body[i], base);
		break;
	case AST_STMT_IF:
		tuple_rewrite_expr(s->data.if_stmt.cond, base);
		for (int i = 0; i < s->data.if_stmt.then_count; i++)
			tuple_rewrite_stmt(s->data.if_stmt.then_body[i], base);
		for (int i = 0; i < s->data.if_stmt.else_count; i++)
			tuple_rewrite_stmt(s->data.if_stmt.else_body[i], base);
		break;
	case AST_STMT_EXPR:
		tuple_rewrite_expr(s->data.expr_stmt.expr, base);
		break;
	case AST_STMT_RETURN:
		for (int i = 0; i < s->data.return_stmt.count; i++)
			tuple_rewrite_expr(s->data.return_stmt.values[i], base);
		break;
	default:
		break;
	}
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
		/* Flatten tuple fields: `pos: (x, y)` -> columns `pos_x`, `pos_y`. */
		int fcount = 0;
		for (int i = 0; i < arch->field_count; i++) {
			FieldDecl *fd = arch->fields[i];
			fcount += (fd->type && fd->type->kind == TYPE_TUPLE) ? fd->type->data.tuple.field_count : 1;
		}
		aarch->field_count = fcount;
		aarch->fields = calloc(fcount, sizeof(AstField *));
		int fi = 0;
		for (int i = 0; i < arch->field_count; i++) {
			FieldDecl *fd = arch->fields[i];
			if (fd->type && fd->type->kind == TYPE_TUPLE) {
				for (int j = 0; j < fd->type->data.tuple.field_count; j++) {
					AstField *af = ast_field_create(fd->kind, NULL, NULL);
					af->loc = fd->loc;
					char nm[256];
					snprintf(nm, sizeof(nm), "%s_%s", fd->name, fd->type->data.tuple.field_names[j]);
					af->name = malloc(strlen(nm) + 1);
					strcpy(af->name, nm);
					af->type = lower_type_ref(fd->type->data.tuple.field_types[j]);
					aarch->fields[fi++] = af;
				}
			} else {
				aarch->fields[fi++] = lower_field(fd);
			}
		}
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

		/* Lower the body first; tuple params then desugar against it. */
		asys->stmt_count = sys->statement_count;
		asys->stmts = calloc(sys->statement_count, sizeof(AstStmt *));
		for (int i = 0; i < sys->statement_count; i++)
			asys->stmts[i] = lower_stmt(sys->statements[i]);

		/* A param naming a tuple field expands into one scalar param per
		   component (`pos` -> `pos_x`, `pos_y`); its `pos.x` accesses in the body
		   are rewritten to the flattened names, matching the flattened columns. */
		int pcount = 0;
		for (int i = 0; i < sys->param_count; i++) {
			TupleFieldInfo *ti = lookup_tuple(sys->params[i]->name);
			pcount += ti ? ti->n : 1;
		}
		asys->param_count = pcount;
		asys->params = calloc(pcount, sizeof(AstParam *));
		int pi = 0;
		for (int i = 0; i < sys->param_count; i++) {
			Parameter *cp = sys->params[i];
			TupleFieldInfo *ti = lookup_tuple(cp->name);
			if (!ti) {
				asys->params[pi++] = lower_param(cp);
				continue;
			}
			for (int s = 0; s < asys->stmt_count; s++)
				tuple_rewrite_stmt(asys->stmts[s], cp->name);
			for (int j = 0; j < ti->n; j++) {
				char nm[256];
				snprintf(nm, sizeof(nm), "%s_%s", cp->name, ti->comp[j]);
				AstParam *ap = ast_param_create(NULL, NULL);
				ap->loc = cp->loc;
				ap->is_move = cp->is_move;
				ap->name = malloc(strlen(nm) + 1);
				strcpy(ap->name, nm);
				ap->type = lower_type_ref(ti->ctype[j]);
				asys->params[pi++] = ap;
			}
		}
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
		afunc->return_type_count = func->return_type_count;
		afunc->return_types = calloc(func->return_type_count, sizeof(AstType *));
		for (int i = 0; i < func->return_type_count; i++)
			afunc->return_types[i] = lower_type_ref(func->return_types[i]);
		afunc->stmt_count = func->statement_count;
		afunc->stmts = calloc(func->statement_count, sizeof(AstStmt *));
		for (int i = 0; i < func->statement_count; i++)
			afunc->stmts[i] = lower_stmt(func->statements[i]);
		ad->data.func = afunc;
		break;
	}
	case DECL_FUNC_GROUP: {
		ad = ast_decl_create(AST_DECL_FUNC_GROUP);
		ad->loc = decl->loc;
		FuncGroup *g = decl->data.func_group;
		AstFuncGroupDecl *ag = calloc(1, sizeof(AstFuncGroupDecl));
		ag->loc = g->loc;
		ag->name = malloc(strlen(g->name) + 1);
		strcpy(ag->name, g->name);
		ag->member_count = g->member_count;
		ag->member_names = calloc(g->member_count, sizeof(char *));
		for (int i = 0; i < g->member_count; i++) {
			ag->member_names[i] = malloc(strlen(g->member_names[i]) + 1);
			strcpy(ag->member_names[i], g->member_names[i]);
		}
		ad->data.func_group = ag;
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

	/* Collect tuple field definitions before lowering so archetype fields and
	   system params/bodies can be desugared into flat scalar columns. */
	build_tuple_registry(cst);

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
