#include "lower.h"
#include "../cst/cst_view.h"
#include "../semantic/sem_model.h"
#include "../semantic/semantic.h"
#include <stdlib.h>
#include <string.h>

/* Semantic context for CST-driven lowering: resolves nominal type aliases (e.g.
 * `file` -> `opaque`), which the Program path got via in-place erasure. */
static SemanticContext *g_lower_sem = NULL;
void lower_set_sem(struct SemanticContext *ctx) {
	g_lower_sem = ctx;
}

/* Resolved types live in the semantic side model (keyed by CST node id), set by
 * the driver before lowering. When unset (e.g. unit tests that lower directly),
 * we fall back to the type still carried on the expression. */
static const SemModel *g_lower_model = NULL;
void lower_set_model(const SemModel *m) {
	g_lower_model = m;
}

/* The resolved type of an expression: from the side model when available, else
 * the transitional Expression.resolved_type. */
static const char *lower_expr_type(const Expression *expr) {
	if (g_lower_model && expr->cst_id) {
		const char *t = sem_model_expr_type(g_lower_model, expr->cst_id - 1);
		if (t)
			return t;
	}
	return expr->resolved_type;
}

/* Whether a bind is a compile-time type alias: from the side model when set, else
 * the transitional Statement/BindStmt flag. */
static int lower_bind_is_alias(const Statement *stmt, const BindStmt *ls) {
	if (g_lower_model && stmt->cst_id)
		return sem_model_bind_alias(g_lower_model, stmt->cst_id - 1);
	return ls->is_type_alias;
}

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
	e->resolved = map_type_str(lower_expr_type(expr));

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
		if (lower_bind_is_alias(stmt, ls)) {
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
	ap->is_own = p->is_own;
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
				ap->is_own = cp->is_own;
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
		/* Carry the explicit declared type into the AST. The meta-type `type` (a type alias) is
		 * compile-time only and stays erased — only a concrete `name : T : value` keeps its T. */
		if (cd->decl_type && cd->decl_type->kind != TYPE_TYPE)
			acd->type = lower_type_ref(cd->decl_type);
		ad->data.constant = acd;
		break;
	}
	}
	return ad;
}

/* =========================================================================
   CST-driven lowering (alternative to the Program-based path above). Reads the
   lossless CST via cst_view + the semantic side model. Gated by ARCHE_LOWER_CST
   until validated IR-identical; the Program path remains the default. Reuses the
   AST-level helpers above (map_type_str, tuple registry, tuple_rewrite_*).
   ========================================================================= */

static AstExpr *lower_expr_cst(CstView e);
static AstStmt *lower_stmt_cst(CstView s);

/* malloc'd NUL-terminated copy of a view's source text. */
static char *cv_dup(CstView v) {
	CvText t = cv_text(v);
	char *s = malloc(t.len + 1);
	if (t.ptr)
		memcpy(s, t.ptr, t.len);
	s[t.len] = '\0';
	return s;
}

/* malloc'd NUL-terminated copy of a borrowed text slice. */
static char *txt_dup(CvText t) {
	char *s = malloc(t.len + 1);
	if (t.ptr)
		memcpy(s, t.ptr, t.len);
	s[t.len] = '\0';
	return s;
}

/* Lower a CST type node (SN_TYPE_*) to an AstType, mirroring lower_type_ref. */
static AstType *lower_type_cst(CstView t) {
	if (!cv_present(t))
		return NULL;
	AstType *at = ast_type_create(AST_TYPE_UNKNOWN);
	switch (cv_kind(t)) {
	case SN_TYPE_REF: {
		char *raw = txt_dup(cv_token(t, TOK_IDENT));
		const char *r = g_lower_sem ? semantic_resolve_type_alias(g_lower_sem, raw) : raw;
		char *name = malloc(strlen(r) + 1);
		strcpy(name, r);
		free(raw);
		if (strcmp(name, "archetype") == 0)
			at->tag = AST_TYPE_ARCHETYPE;
		else if (strcmp(name, "opaque") == 0)
			at->tag = AST_TYPE_OPAQUE;
		else if (strcmp(name, "type") == 0)
			; /* meta-type erased; leave UNKNOWN defensively */
		else
			*at = map_type_str(name); /* borrows name via AST_TYPE_NAMED below */
		/* map_type_str's AST_TYPE_NAMED .name points at its argument; keep a stable copy. */
		if (at->tag == AST_TYPE_NAMED)
			at->name = name;
		else
			free(name);
		break;
	}
	case SN_TYPE_ARRAY: {
		at->tag = AST_TYPE_ARRAY;
		AstType *elem = ast_type_create(AST_TYPE_UNKNOWN);
		char *en = txt_dup(cv_token(t, TOK_IDENT));
		*elem = map_type_str(en);
		if (elem->tag == AST_TYPE_NAMED)
			elem->name = en;
		else
			free(en);
		at->elem = elem;
		break;
	}
	case SN_TYPE_SHAPED_ARRAY: {
		/* `T[a][b]...` — innermost element is the named type; each `[n]` adds a rank.
		 * Reconstruct nested shaped arrays from the NUMBER tokens. */
		char *en = txt_dup(cv_token(t, TOK_IDENT));
		AstType *elem = ast_type_create(AST_TYPE_UNKNOWN);
		*elem = map_type_str(en);
		if (elem->tag == AST_TYPE_NAMED)
			elem->name = en;
		else
			free(en);
		/* collect ranks (NUMBER tokens) left-to-right */
		int ranks[16], nr = 0;
		for (int i = 0; i < t.node->child_count && nr < 16; i++)
			if (t.node->children[i].tag == SE_TOKEN && t.node->children[i].as.token.kind == TOK_NUMBER) {
				char buf[32];
				int l = (int)t.node->children[i].as.token.length;
				if (l > 31)
					l = 31;
				memcpy(buf, t.src + t.node->children[i].as.token.offset, l);
				buf[l] = '\0';
				ranks[nr++] = atoi(buf);
			}
		AstType *cur = elem;
		for (int i = nr - 1; i >= 0; i--) {
			AstType *sh = ast_type_create(AST_TYPE_SHAPED_ARRAY);
			sh->elem = cur;
			sh->rank = ranks[i];
			cur = sh;
		}
		free(at);
		return cur;
	}
	case SN_TYPE_HANDLE: {
		at->tag = AST_TYPE_HANDLE;
		/* handle<Name> / handle(Name): the archetype name is the second IDENT */
		CvText h = cv_token(t, TOK_IDENT); /* "handle" */
		(void)h;
		/* find the IDENT that isn't "handle" */
		for (int i = 0; i < t.node->child_count; i++)
			if (t.node->children[i].tag == SE_TOKEN && t.node->children[i].as.token.kind == TOK_IDENT) {
				CvText nm = {t.src + t.node->children[i].as.token.offset, t.node->children[i].as.token.length};
				if (!(nm.len == 6 && memcmp(nm.ptr, "handle", 6) == 0)) {
					at->name = txt_dup(nm);
					break;
				}
			}
		break;
	}
	default:
		break;
	}
	return at;
}

/* Resolved type of a CST expression node, from the side model (keyed by node id). */
static AstType cst_expr_type(CstView e) {
	return map_type_str(g_lower_model ? sem_model_expr_type(g_lower_model, cv_id(e)) : NULL);
}

/* Decode a string token's content (quotes + escapes) like parse_primary_expr does. */
static char *cst_decode_string(CvText raw, int *out_len) {
	const char *s = raw.ptr;
	size_t len = raw.len;
	char *value = malloc(len + 1);
	int p = 0;
	for (size_t i = 1; i + 1 < len; i++) {
		if (s[i] == '\\' && i + 2 < len) {
			i++;
			switch (s[i]) {
			case 'n': value[p++] = '\n'; break;
			case 't': value[p++] = '\t'; break;
			case 'r': value[p++] = '\r'; break;
			case '\\': value[p++] = '\\'; break;
			case '"': value[p++] = '"'; break;
			default: value[p++] = s[i]; break;
			}
		} else {
			value[p++] = s[i];
		}
	}
	value[p] = '\0';
	*out_len = p;
	return value;
}

/* The "innermost value expression" child of a node (skips role-only name wrappers). */
static CstView cst_first_expr(CstView v) {
	for (int i = 0; i < v.node->child_count; i++)
		if (v.node->children[i].tag == SE_NODE) {
			SyntaxNodeKind k = v.node->children[i].as.node->kind;
			if (k >= SN_LITERAL_EXPR && k <= SN_PAREN_EXPR) {
				CstView r = {v.node->children[i].as.node, v.src};
				return r;
			}
		}
	CstView none = {NULL, v.src};
	return none;
}

static Operator cst_tok_to_op(TokenKind k) {
	switch (k) {
	case TOK_PLUS: return OP_ADD;
	case TOK_MINUS: return OP_SUB;
	case TOK_STAR: return OP_MUL;
	case TOK_SLASH: return OP_DIV;
	case TOK_EQ_EQ: return OP_EQ;
	case TOK_BANG_EQ: return OP_NEQ;
	case TOK_LT: return OP_LT;
	case TOK_GT: return OP_GT;
	case TOK_LT_EQ: return OP_LTE;
	case TOK_GT_EQ: return OP_GTE;
	default: return OP_NONE;
	}
}

/* nth direct child node that is a type form (SN_TYPE_REF/ARRAY/SHAPED/TUPLE/HANDLE). */
static CstView cv_type_at(CstView v, int idx) {
	int c = 0;
	for (int i = 0; i < v.node->child_count; i++)
		if (v.node->children[i].tag == SE_NODE) {
			SyntaxNodeKind k = v.node->children[i].as.node->kind;
			if (k >= SN_TYPE_REF && k <= SN_TYPE_HANDLE) {
				if (c == idx) {
					CstView r = {v.node->children[i].as.node, v.src};
					return r;
				}
				c++;
			}
		}
	CstView none = {NULL, v.src};
	return none;
}

static int cv_type_count(CstView v) {
	int c = 0;
	for (int i = 0; i < v.node->child_count; i++)
		if (v.node->children[i].tag == SE_NODE) {
			SyntaxNodeKind k = v.node->children[i].as.node->kind;
			if (k >= SN_TYPE_REF && k <= SN_TYPE_HANDLE)
				c++;
		}
	return c;
}

/* nth child node that is an expression kind (skips role/name/type wrappers). */
static CstView cv_node_at_expr(CstView v, int idx) {
	int c = 0;
	for (int i = 0; i < v.node->child_count; i++)
		if (v.node->children[i].tag == SE_NODE) {
			SyntaxNodeKind k = v.node->children[i].as.node->kind;
			if (k >= SN_LITERAL_EXPR && k <= SN_PAREN_EXPR) {
				if (c == idx) {
					CstView r = {v.node->children[i].as.node, v.src};
					return r;
				}
				c++;
			}
		}
	CstView none = {NULL, v.src};
	return none;
}

static AstExpr *lower_expr_cst(CstView e) {
	if (!cv_present(e))
		return NULL;
	AstExpr *ax = ast_expr_create(AST_EXPR_LITERAL);
	ax->resolved = cst_expr_type(e);

	switch (cv_kind(e)) {
	case SN_PAREN_EXPR:
		/* transparent: lower the inner expression */
		return lower_expr_cst(cst_first_expr(e));
	case SN_LITERAL_EXPR:
		ax->kind = AST_EXPR_LITERAL;
		ax->data.literal.lexeme = cv_dup(e);
		break;
	case SN_STRING_EXPR: {
		ax->kind = AST_EXPR_STRING;
		int n = 0;
		ax->data.string.value = cst_decode_string(cv_text(e), &n);
		ax->data.string.length = n;
		break;
	}
	case SN_NAME_EXPR: {
		ax->kind = AST_EXPR_NAME;
		/* table<Name> in value position resolves to the bare archetype name */
		if (cv_has_token(e, TOK_LT)) {
			/* second IDENT is the archetype name */
			char *nm = NULL;
			int seen = 0;
			for (int i = 0; i < e.node->child_count; i++)
				if (e.node->children[i].tag == SE_TOKEN && e.node->children[i].as.token.kind == TOK_IDENT) {
					CvText t = {e.src + e.node->children[i].as.token.offset, e.node->children[i].as.token.length};
					if (seen++) {
						nm = txt_dup(t);
						break;
					}
				}
			ax->data.name.name = nm ? nm : cv_dup(e);
		} else {
			ax->data.name.name = txt_dup(cv_token(e, TOK_IDENT));
		}
		break;
	}
	case SN_FIELD_EXPR: {
		/* `base.f1.f2…[idx]` is flat under one node: base IDENT, then (DOT FIELD_NAME)+,
		 * optionally a trailing `[indices]`. Rebuild the left-assoc AST. */
		AstExpr *base = ast_expr_create(AST_EXPR_NAME);
		base->data.name.name = txt_dup(cv_token(e, TOK_IDENT));
		AstExpr *cur = base;
		int nfields = cv_count(e, SN_FIELD_NAME);
		for (int i = 0; i < nfields; i++) {
			AstExpr *f = ast_expr_create(AST_EXPR_FIELD);
			f->data.field.base = cur;
			f->data.field.field_name = cv_dup(cv_child_at(e, SN_FIELD_NAME, i));
			cur = f;
		}
		/* trailing index over the final field */
		if (cv_has_token(e, TOK_LBRACKET)) {
			AstExpr *idx = ast_expr_create(AST_EXPR_INDEX);
			idx->data.index.base = cur;
			int ic = 0;
			for (int i = 0; i < e.node->child_count; i++)
				if (e.node->children[i].tag == SE_NODE) {
					SyntaxNodeKind k = e.node->children[i].as.node->kind;
					if (k >= SN_LITERAL_EXPR && k <= SN_PAREN_EXPR)
						ic++;
				}
			idx->data.index.indices = calloc(ic ? ic : 1, sizeof(AstExpr *));
			idx->data.index.index_count = 0;
			for (int i = 0; i < e.node->child_count; i++)
				if (e.node->children[i].tag == SE_NODE) {
					SyntaxNodeKind k = e.node->children[i].as.node->kind;
					if (k >= SN_LITERAL_EXPR && k <= SN_PAREN_EXPR) {
						CstView iv = {e.node->children[i].as.node, e.src};
						idx->data.index.indices[idx->data.index.index_count++] = lower_expr_cst(iv);
					}
				}
			cur = idx;
		}
		free(ax);
		cur->resolved = cst_expr_type(e);
		return cur;
	}
	case SN_INDEX_EXPR: {
		ax->kind = AST_EXPR_INDEX;
		AstExpr *base = ast_expr_create(AST_EXPR_NAME);
		base->data.name.name = txt_dup(cv_token(e, TOK_IDENT));
		ax->data.index.base = base;
		int ic = 0;
		for (int i = 0; i < e.node->child_count; i++)
			if (e.node->children[i].tag == SE_NODE) {
				SyntaxNodeKind k = e.node->children[i].as.node->kind;
				if (k >= SN_LITERAL_EXPR && k <= SN_PAREN_EXPR)
					ic++;
			}
		ax->data.index.indices = calloc(ic ? ic : 1, sizeof(AstExpr *));
		ax->data.index.index_count = 0;
		for (int i = 0; i < e.node->child_count; i++)
			if (e.node->children[i].tag == SE_NODE) {
				SyntaxNodeKind k = e.node->children[i].as.node->kind;
				if (k >= SN_LITERAL_EXPR && k <= SN_PAREN_EXPR) {
					CstView iv = {e.node->children[i].as.node, e.src};
					ax->data.index.indices[ax->data.index.index_count++] = lower_expr_cst(iv);
				}
			}
		break;
	}
	case SN_BINARY_EXPR: {
		ax->kind = AST_EXPR_BINARY;
		/* operator token */
		for (int i = 0; i < e.node->child_count; i++)
			if (e.node->children[i].tag == SE_TOKEN) {
				Operator op = cst_tok_to_op(e.node->children[i].as.token.kind);
				if (op != OP_NONE) {
					ax->data.binary.op = op;
					break;
				}
			}
		ax->data.binary.left = lower_expr_cst(cv_node_at_expr(e, 0));
		ax->data.binary.right = lower_expr_cst(cv_node_at_expr(e, 1));
		break;
	}
	case SN_UNARY_EXPR: {
		ax->kind = AST_EXPR_UNARY;
		CvText op = {NULL, 0};
		for (int i = 0; i < e.node->child_count; i++)
			if (e.node->children[i].tag == SE_TOKEN) {
				TokenKind tk = e.node->children[i].as.token.kind;
				if (tk == TOK_MINUS) ax->data.unary.op = UNARY_NEG;
				else if (tk == TOK_BANG) ax->data.unary.op = UNARY_NOT;
				else if (tk == TOK_MOVE) ax->data.unary.op = UNARY_MOVE;
				else if (tk == TOK_COPY) ax->data.unary.op = UNARY_COPY;
				else continue;
				break;
			}
		(void)op;
		ax->data.unary.operand = lower_expr_cst(cst_first_expr(e));
		break;
	}
	case SN_CALL_EXPR: {
		ax->kind = AST_EXPR_CALL;
		AstExpr *callee = ast_expr_create(AST_EXPR_NAME);
		callee->data.name.name = cv_dup(cv_child(e, SN_CALLEE_NAME));
		ax->data.call.callee = callee;
		int ac = 0;
		for (int i = 0; i < e.node->child_count; i++)
			if (e.node->children[i].tag == SE_NODE) {
				SyntaxNodeKind k = e.node->children[i].as.node->kind;
				if (k >= SN_LITERAL_EXPR && k <= SN_PAREN_EXPR)
					ac++;
			}
		ax->data.call.args = calloc(ac ? ac : 1, sizeof(AstExpr *));
		ax->data.call.arg_count = 0;
		for (int i = 0; i < e.node->child_count; i++)
			if (e.node->children[i].tag == SE_NODE) {
				SyntaxNodeKind k = e.node->children[i].as.node->kind;
				if (k >= SN_LITERAL_EXPR && k <= SN_PAREN_EXPR) {
					CstView av = {e.node->children[i].as.node, e.src};
					ax->data.call.args[ax->data.call.arg_count++] = lower_expr_cst(av);
				}
			}
		break;
	}
	default:
		/* SN_ALLOC_EXPR / SN_ARRAY_LIT_EXPR and any unhandled: leave as a placeholder
		 * literal for now (these surface in verify-codegen if exercised). */
		ax->kind = AST_EXPR_LITERAL;
		ax->data.literal.lexeme = cv_dup(e);
		break;
	}
	return ax;
}

/* Lower the statement-kind child nodes of `parent` into an AstStmt array. */
static AstStmt **cst_lower_body(CstView parent, int *out_count) {
	int n = 0;
	for (int i = 0; i < parent.node->child_count; i++)
		if (parent.node->children[i].tag == SE_NODE) {
			SyntaxNodeKind k = parent.node->children[i].as.node->kind;
			if (k >= SN_BIND_STMT && k <= SN_EACH_FIELD_STMT)
				n++;
		}
	*out_count = n;
	if (n == 0)
		return NULL;
	AstStmt **out = calloc(n, sizeof(AstStmt *));
	int j = 0;
	for (int i = 0; i < parent.node->child_count; i++)
		if (parent.node->children[i].tag == SE_NODE) {
			SyntaxNodeKind k = parent.node->children[i].as.node->kind;
			if (k >= SN_BIND_STMT && k <= SN_EACH_FIELD_STMT) {
				CstView sv = {parent.node->children[i].as.node, parent.src};
				out[j++] = lower_stmt_cst(sv);
			}
		}
	return out;
}

static Operator cst_assign_op(TokenKind k) {
	switch (k) {
	case TOK_PLUS_EQ: return OP_ADD;
	case TOK_MINUS_EQ: return OP_SUB;
	case TOK_STAR_EQ: return OP_MUL;
	case TOK_SLASH_EQ: return OP_DIV;
	default: return OP_NONE; /* plain `=` */
	}
}

static AstStmt *lower_stmt_cst(CstView s) {
	AstStmt *as = ast_stmt_create(AST_STMT_EXPR);

	switch (cv_kind(s)) {
	case SN_BIND_STMT: {
		if (g_lower_model && sem_model_bind_alias(g_lower_model, cv_id(s))) {
			as->kind = AST_STMT_EXPR;
			AstExpr *zero = ast_expr_create(AST_EXPR_LITERAL);
			zero->data.literal.lexeme = malloc(2);
			strcpy(zero->data.literal.lexeme, "0");
			as->data.expr_stmt.expr = zero;
			break;
		}
		as->kind = AST_STMT_BIND;
		CstView target = cv_node_at_expr(s, 0);
		as->data.bind_stmt.name_count = 1;
		as->data.bind_stmt.names = calloc(1, sizeof(char *));
		as->data.bind_stmt.names[0] = cv_dup(target);
		as->data.bind_stmt.type = lower_type_cst(cv_type_at(s, 0));
		as->data.bind_stmt.value = lower_expr_cst(cv_node_at_expr(s, 1));
		break;
	}
	case SN_ASSIGN_STMT: {
		as->kind = AST_STMT_ASSIGN;
		for (int i = 0; i < s.node->child_count; i++)
			if (s.node->children[i].tag == SE_TOKEN) {
				TokenKind tk = s.node->children[i].as.token.kind;
				if (tk == TOK_EQ || tk == TOK_PLUS_EQ || tk == TOK_MINUS_EQ || tk == TOK_STAR_EQ ||
				    tk == TOK_SLASH_EQ) {
					as->data.assign_stmt.op = cst_assign_op(tk);
					break;
				}
			}
		as->data.assign_stmt.target = lower_expr_cst(cv_node_at_expr(s, 0));
		as->data.assign_stmt.value = lower_expr_cst(cv_node_at_expr(s, 1));
		break;
	}
	case SN_EXPR_STMT:
		as->kind = AST_STMT_EXPR;
		as->data.expr_stmt.expr = lower_expr_cst(cv_node_at_expr(s, 0));
		break;
	case SN_FREE_STMT:
		as->kind = AST_STMT_FREE;
		as->data.free_stmt.value = lower_expr_cst(cv_node_at_expr(s, 0));
		break;
	case SN_BREAK_STMT:
		as->kind = AST_STMT_BREAK;
		break;
	case SN_RETURN_STMT: {
		as->kind = AST_STMT_RETURN;
		int c = 0;
		for (int i = 0; i < s.node->child_count; i++)
			if (s.node->children[i].tag == SE_NODE) {
				SyntaxNodeKind k = s.node->children[i].as.node->kind;
				if (k >= SN_LITERAL_EXPR && k <= SN_PAREN_EXPR)
					c++;
			}
		as->data.return_stmt.count = c;
		as->data.return_stmt.values = calloc(c ? c : 1, sizeof(AstExpr *));
		for (int i = 0; i < c; i++)
			as->data.return_stmt.values[i] = lower_expr_cst(cv_node_at_expr(s, i));
		break;
	}
	case SN_RUN_STMT: {
		as->kind = AST_STMT_RUN;
		/* `run sys` or `run sys in world`: collect IDENTs (skip the `in` keyword). */
		char *names[2] = {NULL, NULL};
		int ni = 0;
		for (int i = 0; i < s.node->child_count && ni < 2; i++)
			if (s.node->children[i].tag == SE_TOKEN && s.node->children[i].as.token.kind == TOK_IDENT) {
				CvText t = {s.src + s.node->children[i].as.token.offset, s.node->children[i].as.token.length};
				names[ni++] = txt_dup(t);
			}
		as->data.run_stmt.system_name = names[0] ? names[0] : txt_dup((CvText){"", 0});
		as->data.run_stmt.world_name = names[1];
		break;
	}
	case SN_IF_STMT: {
		as->kind = AST_STMT_IF;
		as->data.if_stmt.cond = lower_expr_cst(cv_node_at_expr(s, 0));
		/* then-body = stmt children directly under the IF; else-body under ELSE_CLAUSE. */
		CstView elsec = cv_child(s, SN_ELSE_CLAUSE);
		as->data.if_stmt.then_body = cst_lower_body(s, &as->data.if_stmt.then_count);
		if (cv_present(elsec))
			as->data.if_stmt.else_body = cst_lower_body(elsec, &as->data.if_stmt.else_count);
		break;
	}
	case SN_FOR_STMT: {
		as->kind = AST_STMT_FOR;
		/* range form: `for IDENT in IDENT { body }` */
		int ni = 0;
		char *vname = NULL, *iname = NULL;
		for (int i = 0; i < s.node->child_count; i++)
			if (s.node->children[i].tag == SE_TOKEN && s.node->children[i].as.token.kind == TOK_IDENT) {
				CvText t = {s.src + s.node->children[i].as.token.offset, s.node->children[i].as.token.length};
				if (ni == 0) vname = txt_dup(t);
				else if (ni == 1) iname = txt_dup(t);
				ni++;
			}
		as->data.for_stmt.var_name = vname;
		if (iname) {
			AstExpr *it = ast_expr_create(AST_EXPR_NAME);
			it->data.name.name = iname;
			as->data.for_stmt.iterable = it;
		}
		as->data.for_stmt.body = cst_lower_body(s, &as->data.for_stmt.body_count);
		break;
	}
	case SN_EACH_FIELD_STMT: {
		as->kind = AST_STMT_EACH_FIELD;
		int ni = 0;
		for (int i = 0; i < s.node->child_count; i++)
			if (s.node->children[i].tag == SE_TOKEN && s.node->children[i].as.token.kind == TOK_IDENT) {
				CvText t = {s.src + s.node->children[i].as.token.offset, s.node->children[i].as.token.length};
				if (ni == 0) as->data.each_field.binding_name = txt_dup(t);
				else if (ni == 1) as->data.each_field.arch_param_name = txt_dup(t);
				ni++;
			}
		as->data.each_field.filter_type = lower_type_cst(cv_type_at(s, 0));
		as->data.each_field.body = cst_lower_body(s, &as->data.each_field.body_count);
		break;
	}
	default:
		/* SN_MULTI_BIND_STMT and anything unhandled: placeholder no-op. */
		as->kind = AST_STMT_EXPR;
		as->data.expr_stmt.expr = NULL;
		break;
	}
	return as;
}

static AstParam *lower_param_cst(CstView p) {
	AstParam *ap = ast_param_create(NULL, NULL);
	ap->name = cv_dup(cv_child(p, SN_PARAM_NAME));
	ap->type = lower_type_cst(cv_type_at(p, 0)); /* NULL for sys params */
	ap->is_own = cv_has_token(p, TOK_OWN);
	return ap;
}

static AstDecl *lower_decl_cst(CstView d) {
	switch (cv_kind(d)) {
	case SN_USE_DECL:
		return NULL;
	case SN_WORLD_DECL: {
		AstDecl *ad = ast_decl_create(AST_DECL_WORLD);
		ad->data.world = calloc(1, sizeof(AstWorldDecl));
		ad->data.world->name = txt_dup(cv_token(d, TOK_IDENT));
		return ad;
	}
	case SN_ARCHETYPE_DECL: {
		AstDecl *ad = ast_decl_create(AST_DECL_ARCHETYPE);
		AstArchetypeDecl *aa = calloc(1, sizeof(AstArchetypeDecl));
		aa->name = cv_dup(cv_child(d, SN_TYPE_DEF_NAME));
		int nf = cv_count(d, SN_FIELD_NAME);
		aa->fields = calloc(nf ? nf : 1, sizeof(AstField *));
		aa->field_count = 0;
		for (int i = 0; i < nf; i++) {
			AstField *af = ast_field_create(FIELD_COLUMN, NULL, NULL);
			af->name = cv_dup(cv_child_at(d, SN_FIELD_NAME, i));
			/* field type = the i-th type node in the body (paired positionally) */
			af->type = lower_type_cst(cv_type_at(d, i));
			aa->fields[aa->field_count++] = af;
		}
		ad->data.archetype = aa;
		return ad;
	}
	case SN_PROC_DECL: {
		AstDecl *ad = ast_decl_create(AST_DECL_PROC);
		AstProcDecl *ap = calloc(1, sizeof(AstProcDecl));
		ap->name = cv_dup(cv_child(d, SN_FUNC_DEF_NAME));
		ap->is_extern = cv_has_token(d, TOK_EXTERN);
		int np = cv_count(d, SN_PARAM);
		ap->params = calloc(np ? np : 1, sizeof(AstParam *));
		for (int i = 0; i < np; i++)
			ap->params[i] = lower_param_cst(cv_child_at(d, SN_PARAM, i));
		ap->param_count = np;
		ap->stmts = cst_lower_body(d, &ap->stmt_count);
		ad->data.proc = ap;
		return ad;
	}
	case SN_SYS_DECL: {
		AstDecl *ad = ast_decl_create(AST_DECL_SYS);
		AstSysDecl *as = calloc(1, sizeof(AstSysDecl));
		as->name = cv_dup(cv_child(d, SN_FUNC_DEF_NAME));
		int np = cv_count(d, SN_PARAM);
		as->params = calloc(np ? np : 1, sizeof(AstParam *));
		for (int i = 0; i < np; i++)
			as->params[i] = lower_param_cst(cv_child_at(d, SN_PARAM, i));
		as->param_count = np;
		as->stmts = cst_lower_body(d, &as->stmt_count);
		ad->data.sys = as;
		return ad;
	}
	case SN_FUNC_DECL: {
		AstDecl *ad = ast_decl_create(AST_DECL_FUNC);
		AstFuncDecl *af = calloc(1, sizeof(AstFuncDecl));
		af->name = cv_dup(cv_child(d, SN_FUNC_DEF_NAME));
		af->is_extern = cv_has_token(d, TOK_EXTERN);
		int np = cv_count(d, SN_PARAM);
		af->params = calloc(np ? np : 1, sizeof(AstParam *));
		for (int i = 0; i < np; i++)
			af->params[i] = lower_param_cst(cv_child_at(d, SN_PARAM, i));
		af->param_count = np;
		/* return types: TYPE_REF nodes that are NOT inside params (params are wrapped in SN_PARAM) */
		int nt = cv_type_count(d);
		af->return_types = calloc(nt ? nt : 1, sizeof(AstType *));
		af->return_type_count = 0;
		for (int i = 0; i < nt; i++)
			af->return_types[af->return_type_count++] = lower_type_cst(cv_type_at(d, i));
		af->stmts = cst_lower_body(d, &af->stmt_count);
		ad->data.func = af;
		return ad;
	}
	case SN_CONST_DECL: {
		AstDecl *ad = ast_decl_create(AST_DECL_CONST);
		AstConstDecl *ac = calloc(1, sizeof(AstConstDecl));
		ac->name = txt_dup(cv_token(d, TOK_IDENT));
		ac->value = lower_expr_cst(cv_node_at_expr(d, 0));
		ad->data.constant = ac;
		return ad;
	}
	case SN_STATIC_DECL: {
		AstDecl *ad = ast_decl_create(AST_DECL_STATIC);
		AstStaticDecl *sd = calloc(1, sizeof(AstStaticDecl));
		if (cv_has_token(d, TOK_LPAREN)) {
			/* `static Name(count)` / `static pool<Name>(count)` — archetype allocation */
			sd->kind = AST_STATIC_ARCHETYPE;
			CstView ty = cv_type_at(d, 0);
			char *an = NULL;
			if (cv_present(ty) && cv_has_token(ty, TOK_LT)) {
				int seen = 0; /* pool<Name>: the archetype is the 2nd IDENT */
				for (int i = 0; i < ty.node->child_count; i++)
					if (ty.node->children[i].tag == SE_TOKEN && ty.node->children[i].as.token.kind == TOK_IDENT) {
						CvText t = {ty.src + ty.node->children[i].as.token.offset,
						            ty.node->children[i].as.token.length};
						if (seen++) {
							an = txt_dup(t);
							break;
						}
					}
			} else if (cv_present(ty)) {
				an = txt_dup(cv_token(ty, TOK_IDENT));
			}
			sd->archetype.archetype_name = an ? an : txt_dup((CvText){"", 0});
			sd->archetype.init_length = lower_expr_cst(cv_node_at_expr(d, 0)); /* the (count) */
			sd->archetype.field_count = 0;
		} else {
			/* `static name : T[size]` — static array */
			sd->kind = AST_STATIC_ARRAY;
			sd->array.name = txt_dup(cv_token(d, TOK_IDENT));
			sd->array.element_type = lower_type_cst(cv_type_at(d, 0));
			for (int i = 0; i < d.node->child_count; i++)
				if (d.node->children[i].tag == SE_TOKEN && d.node->children[i].as.token.kind == TOK_NUMBER) {
					char buf[32];
					int l = (int)d.node->children[i].as.token.length;
					if (l > 31)
						l = 31;
					memcpy(buf, d.src + d.node->children[i].as.token.offset, l);
					buf[l] = '\0';
					sd->array.size = atoi(buf);
					break;
				}
		}
		ad->data.static_decl = sd;
		return ad;
	}
	default:
		/* SN_FUNC_GROUP_DECL (+ tuple flattening, field-init blocks) not yet ported. */
		return NULL;
	}
}

/* CST-driven entry, gated by ARCHE_LOWER_CST (validated against the IR goldens). */
AstProgram *lower_cst_to_ast_v2(const SyntaxNode *root, const char *src) {
	if (!root)
		return NULL;
	AstProgram *ast = ast_program_create();
	CstView r = cv_root(root, src);
	int cap = cv_node_count(r);
	ast->decls = calloc(cap ? cap : 1, sizeof(AstDecl *));
	ast->decl_count = 0;
	for (int i = 0; i < root->child_count; i++) {
		if (root->children[i].tag != SE_NODE)
			continue;
		SyntaxNodeKind k = root->children[i].as.node->kind;
		if (k < SN_WORLD_DECL || k > SN_USE_DECL)
			continue;
		CstView dv = {root->children[i].as.node, src};
		AstDecl *ad = lower_decl_cst(dv);
		if (ad)
			ast->decls[ast->decl_count++] = ad;
	}
	return ast;
}

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
