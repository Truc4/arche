#include "tycheck.h"

#include "sem_diagnostics.h"
#include "sem_model.h"
#include "sem_types.h"

#include "../cst/cst.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Phase A skeleton. Build-out shape:
 *
 *   tycheck_run walks ctx->prog->decls. For each function/proc body, it walks
 *   the statement list. The only rule encoded today is STMT_RETURN: each
 *   returned expression must match the corresponding declared return type.
 *
 *   synth(e) computes the type of an expression bottom-up. Phase A handles
 *   EXPR_LITERAL + EXPR_STRING; everything else returns TYID_UNKNOWN.
 *
 *   check(e, expected, where) reports E0200 if synth(e) != expected and both
 *   are known. Fail-open: when either side is unknown, no diagnostic. */

typedef struct {
	SemanticContext *ctx;
	TypeArena *arena;
	AstProgram *prog;
	SemModel *model;
	int loop_depth; /* PC2: 0 outside any loop; incremented on STMT_FOR enter */
} TyCtx;

/* Find a func/proc declaration by name in the program. Returns NULL when the
 * callee is a builtin (print, insert, syscall, …) or genuinely undefined —
 * undefined-symbol diagnostics are emitted by the earlier semantic pass; we
 * stay quiet here. Linear walk matches semantic.c's existing pattern. */
typedef enum { CALLEE_NONE, CALLEE_FUNC, CALLEE_PROC } CalleeKind;
typedef struct {
	CalleeKind kind;
	union {
		FuncDecl *func;
		ProcDecl *proc;
	} u;
} CalleeRef;

static CalleeRef find_callee(AstProgram *prog, const char *name) {
	CalleeRef r = {CALLEE_NONE, {NULL}};
	if (!prog || !name)
		return r;
	for (int i = 0; i < prog->decl_count; i++) {
		Decl *d = prog->decls[i];
		if (!d)
			continue;
		if (d->kind == DECL_FUNC && d->data.func && d->data.func->name && strcmp(d->data.func->name, name) == 0) {
			r.kind = CALLEE_FUNC;
			r.u.func = d->data.func;
			return r;
		}
		if (d->kind == DECL_PROC && d->data.proc && d->data.proc->name && strcmp(d->data.proc->name, name) == 0) {
			r.kind = CALLEE_PROC;
			r.u.proc = d->data.proc;
			return r;
		}
	}
	return r;
}

/* Width-int family: byte, i8/i16/i32/i64/i128, u8/u16/u32/u64/u128. An untyped
 * integer literal can flow into any of them (Rust/Go-style untyped constant). */
static int is_width_int_name(const char *s) {
	if (!s)
		return 0;
	if (strcmp(s, "byte") == 0)
		return 1;
	if (s[0] != 'i' && s[0] != 'u')
		return 0;
	const char *n = s + 1;
	return strcmp(n, "8") == 0 || strcmp(n, "16") == 0 || strcmp(n, "32") == 0 || strcmp(n, "64") == 0 ||
	       strcmp(n, "128") == 0;
}

/* Map a type-name string (as resolved by semantic.c into SemModel.expr_type) to
 * a TypeId. Nominal aliases (`file :: opaque`) are resolved through their chain
 * BEFORE we intern, so a `file`-typed param and an `opaque` variable both land
 * on the same TypeId.
 *
 * Phase B caveat: this collapses ALL alias distinctness (a distinct-nominal
 * `socket :: opaque` becomes the same TypeId as `file :: opaque`). That's a
 * known limitation; the existing semantic.c handles nominal distinctness for
 * the cases it cares about (extern handle params) via separate checks. */
static TypeId tyid_from_typeref(TypeArena *arena, SemanticContext *ctx, const TypeRef *tr);

static TypeId tyid_from_name(TypeArena *arena, SemanticContext *ctx, const char *n) {
	if (!n || !n[0])
		return TYID_UNKNOWN;
	/* A named callable-type alias resolves to its structural signature TypeId (proc/func types
	 * are matched by shape, not name). */
	if (ctx) {
		TypeRef *ct = semantic_callable_type_alias(ctx, n);
		if (ct)
			return tyid_from_typeref(arena, ctx, ct);
	}
	/* Normalize title-case aliases (Int/Float/Char/Str/Void) to lowercase before
	 * alias resolution. Matches semantic.c's normalize_type_name behavior. */
	if (strcmp(n, "Int") == 0)
		n = "int";
	else if (strcmp(n, "Float") == 0)
		n = "float";
	else if (strcmp(n, "Char") == 0)
		n = "char";
	else if (strcmp(n, "Str") == 0)
		n = "str";
	else if (strcmp(n, "Void") == 0)
		n = "void";
	const char *r = ctx ? semantic_resolve_type_alias(ctx, n) : n;
	if (!r || !r[0])
		return TYID_UNKNOWN;
	if (strcmp(r, "int") == 0)
		return tyid_of_prim(arena, PRIM_INT);
	if (strcmp(r, "float") == 0)
		return tyid_of_prim(arena, PRIM_FLOAT);
	if (strcmp(r, "char") == 0)
		return tyid_of_prim(arena, PRIM_CHAR);
	if (strcmp(r, "str") == 0)
		return tyid_of_prim(arena, PRIM_STR);
	if (strcmp(r, "bool") == 0)
		return tyid_of_prim(arena, PRIM_BOOL);
	if (strcmp(r, "void") == 0)
		return tyid_of_prim(arena, PRIM_VOID);
	if (strcmp(r, "opaque") == 0)
		return tyid_of_nominal(arena, "opaque");
	/* semantic.c uses "char_array" for string literals in SemModel — fold to STR
	 * so a function param `str` matches a name-typed variable whose value came
	 * from a string literal. */
	if (strcmp(r, "char_array") == 0)
		return tyid_of_prim(arena, PRIM_STR);
	return tyid_of_nominal(arena, r);
}

/* Map a declared TypeRef (from FuncDecl.return_types et al) into a TypeId.
 * Names normalize to primitives where they match; everything else becomes a
 * nominal. Width-ints (i64, byte, …) are nominals — kept distinct so a `var: i32`
 * doesn't silently flow into an `i64` param. Untyped integer literals get
 * special-case acceptance in check_literal_fits(). */
static TypeId tyid_from_typeref(TypeArena *arena, SemanticContext *ctx, const TypeRef *tr) {
	if (!tr)
		return TYID_UNKNOWN;
	switch (tr->kind) {
	case TYPE_NAME:
		return tyid_from_name(arena, ctx, tr->data.name);
	case TYPE_PROC:
	case TYPE_FUNC: {
		/* Callable type → structural func TypeId (interned; name never in the key). */
		int np = tr->data.callable.param_count, nr = tr->data.callable.result_count;
		TypeId pbuf[32], rbuf[8];
		TypeId *params = np > 32 ? malloc((size_t)np * sizeof(TypeId)) : pbuf;
		TypeId *rets = nr > 8 ? malloc((size_t)nr * sizeof(TypeId)) : rbuf;
		for (int i = 0; i < np; i++)
			params[i] = tyid_from_typeref(arena, ctx, tr->data.callable.param_types[i]);
		for (int i = 0; i < nr; i++)
			rets[i] = tyid_from_typeref(arena, ctx, tr->data.callable.result_types[i]);
		TypeId out = tyid_of_func(arena, params, np, rets, nr, tr->data.callable.is_proc);
		if (params != pbuf)
			free(params);
		if (rets != rbuf)
			free(rets);
		return out;
	}
	default:
		/* Phase B fills in array/tuple/handle/archetype. */
		return TYID_UNKNOWN;
	}
}

/* Untyped-literal flexibility (Rust/Go model). When `e` is a literal expression
 * whose value can plausibly inhabit `expected`, accept. Examples:
 *   - integer literal `5` flows into int, byte, i8..i128, u8..u128, float
 *   - char literal `'x'` flows into char and any integer (codepoint as int)
 *   - float literal `1.5` flows into float (only)
 * For named-type expected (an archetype, alias, or nominal), no relaxation. */
static int check_literal_fits(const TypeArena *arena, Expression *e, TypeId expected) {
	if (!e || e->type != EXPR_LITERAL)
		return 0;
	const char *lex = e->data.literal.lexeme;
	if (!lex)
		return 0;
	int is_float_lit = (lex[0] != '"' && lex[0] != '\'' && (strchr(lex, '.') || strchr(lex, 'e') || strchr(lex, 'E')));
	int is_int_lit = (lex[0] != '"' && lex[0] != '\'' && !is_float_lit);
	int is_char_lit = (lex[0] == '\'');

	TyKind k = tyid_kind(arena, expected);
	if (k == TYK_PRIM) {
		/* Compare against the prim kind via display — the arena's internals stay
		 * private. tyid_display gives the same string for the same prim. */
		char want[16];
		tyid_display(arena, expected, want, sizeof(want));
		if (is_int_lit) {
			/* arche follows C/Go: int literal flows into int, float, OR char (a
			 * codepoint). `buf[0] = 65` is canonical for assigning chars by code. */
			if (strcmp(want, "int") == 0 || strcmp(want, "float") == 0 || strcmp(want, "char") == 0)
				return 1;
		}
		if (is_char_lit) {
			if (strcmp(want, "char") == 0 || strcmp(want, "int") == 0)
				return 1;
		}
		if (is_float_lit) {
			if (strcmp(want, "float") == 0)
				return 1;
		}
	}
	if (k == TYK_NOMINAL) {
		/* Width-int names: untyped int literal and char literal both flow in
		 * (an int literal as a width-int, a char literal by codepoint). */
		char want[32];
		tyid_display(arena, expected, want, sizeof(want));
		if (is_width_int_name(want) && (is_int_lit || is_char_lit))
			return 1;
	}
	return 0;
}

static TypeId synth(TyCtx *cx, Expression *e);
static void check(TyCtx *cx, Expression *e, TypeId expected, const char *where);
static int prim_binop_compatible(const TypeArena *arena, TypeId a, TypeId b);

/* Encode the call-typing rule: arity check + per-arg type check + return type.
 * Looks up the callee in the program's decl table. Returns the callee's first
 * return type, or TYID_UNKNOWN for builtins / undefined names (the latter is
 * caught earlier by undefined-symbol diagnostics). */
static TypeId synth_call(TyCtx *cx, Expression *e) {
	if (!e || !e->data.call.callee || e->data.call.callee->type != EXPR_NAME)
		return TYID_UNKNOWN;
	const char *name = e->data.call.callee->data.name.name;
	CalleeRef cr = find_callee(cx->prog, name);
	if (cr.kind == CALLEE_NONE)
		return TYID_UNKNOWN; /* builtin or undefined — quiet here */

	Parameter **params;
	int param_count;
	TypeRef **rets;
	int ret_count;
	int is_variadic;
	if (cr.kind == CALLEE_FUNC) {
		params = cr.u.func->params;
		param_count = cr.u.func->param_count;
		rets = cr.u.func->return_types;
		ret_count = cr.u.func->return_type_count;
		is_variadic = cr.u.func->is_variadic;
	} else {
		params = cr.u.proc->params;
		param_count = cr.u.proc->param_count;
		rets = NULL; /* a proc is not a value — its outputs are out-params, not a return type */
		ret_count = 0;
		is_variadic = cr.u.proc->is_variadic;
	}

	int arg_count = e->data.call.arg_count;

	/* Variadic externs (printf etc., declared `extern foo(fmt: char[], ...)`) require
	 * at least the fixed-arity prefix; trailing args go unchecked. Fixed-arity callees
	 * must match exactly. */
	if (is_variadic) {
		if (arg_count < param_count) {
			sem_emit_wrong_arity(cx->ctx, e->loc, name, param_count, arg_count);
			return ret_count > 0 ? tyid_from_typeref(cx->arena, cx->ctx, rets[0]) : TYID_UNKNOWN;
		}
	} else if (param_count != arg_count) {
		sem_emit_wrong_arity(cx->ctx, e->loc, name, param_count, arg_count);
		return ret_count > 0 ? tyid_from_typeref(cx->arena, cx->ctx, rets[0]) : TYID_UNKNOWN;
	}

	for (int i = 0; i < arg_count && i < param_count; i++) {
		TypeId expected = tyid_from_typeref(cx->arena, cx->ctx, params[i]->type);
		char where[64];
		snprintf(where, sizeof(where), "argument %d of '%s'", i + 1, name);
		check(cx, e->data.call.args[i], expected, where);
	}

	return ret_count > 0 ? tyid_from_typeref(cx->arena, cx->ctx, rets[0]) : tyid_of_prim(cx->arena, PRIM_VOID);
}

/* The structural callable TypeId of a proc/func decl (its signature as a value): params +
 * (proc out-params | func return), with the is_proc discriminator. */
static TypeId tyid_of_callee(TyCtx *cx, CalleeRef cr) {
	TypeId pbuf[32], rbuf[8];
	int pc = 0, rc = 0, is_proc = 0;
	if (cr.kind == CALLEE_FUNC) {
		FuncDecl *f = cr.u.func;
		pc = f->param_count > 32 ? 32 : f->param_count;
		rc = f->return_type_count > 8 ? 8 : f->return_type_count;
		for (int i = 0; i < pc; i++)
			pbuf[i] = tyid_from_typeref(cx->arena, cx->ctx, f->params[i]->type);
		for (int i = 0; i < rc; i++)
			rbuf[i] = tyid_from_typeref(cx->arena, cx->ctx, f->return_types[i]);
	} else if (cr.kind == CALLEE_PROC) {
		ProcDecl *p = cr.u.proc;
		is_proc = 1;
		pc = p->param_count > 32 ? 32 : p->param_count;
		rc = p->out_param_count > 8 ? 8 : p->out_param_count;
		for (int i = 0; i < pc; i++)
			pbuf[i] = tyid_from_typeref(cx->arena, cx->ctx, p->params[i]->type);
		for (int i = 0; i < rc; i++)
			rbuf[i] = tyid_from_typeref(cx->arena, cx->ctx, p->out_params[i]->type);
	} else {
		return TYID_UNKNOWN;
	}
	return tyid_of_func(cx->arena, pbuf, pc, rbuf, rc, is_proc);
}

static TypeId synth(TyCtx *cx, Expression *e) {
	if (!e)
		return TYID_UNKNOWN;
	switch (e->type) {
	case EXPR_LITERAL: {
		const char *lex = e->data.literal.lexeme;
		if (!lex)
			return TYID_UNKNOWN;
		if (lex[0] == '"')
			return tyid_of_prim(cx->arena, PRIM_STR);
		if (lex[0] == '\'')
			return tyid_of_prim(cx->arena, PRIM_CHAR);
		if (strchr(lex, '.') || strchr(lex, 'e') || strchr(lex, 'E'))
			return tyid_of_prim(cx->arena, PRIM_FLOAT);
		return tyid_of_prim(cx->arena, PRIM_INT);
	}
	case EXPR_STRING:
		return tyid_of_prim(cx->arena, PRIM_STR);
	case EXPR_CALL:
		return synth_call(cx, e);
	case EXPR_BINARY: {
		Expression *l = e->data.binary.left;
		Expression *r = e->data.binary.right;
		TypeId lt = synth(cx, l);
		TypeId rt = synth(cx, r);
		/* Catch only the truly disjoint operand pairings (str + numeric, archetype
		 * + numeric, etc.). arche today does C-style numeric widening and
		 * column-broadcast (`field * field`); rejecting those would break a lot of
		 * real code. Tighten once arche has an explicit `as` cast. */
		if (!tyid_is_unknown(lt) && !tyid_is_unknown(rt) && !tyid_equal(lt, rt) &&
		    !prim_binop_compatible(cx->arena, lt, rt)) {
			char want[64];
			char have[64];
			tyid_display(cx->arena, lt, want, sizeof(want));
			tyid_display(cx->arena, rt, have, sizeof(have));
			sem_emit_type_mismatch(cx->ctx, r->loc, "binary operand", want, have);
		}
		/* Comparison ops yield int (arche has no bool); arithmetic returns the
		 * "wider" operand type (float dominates int). */
		switch (e->data.binary.op) {
		case OP_EQ:
		case OP_NEQ:
		case OP_LT:
		case OP_GT:
		case OP_LTE:
		case OP_GTE:
		case OP_AND:
		case OP_OR:
			return tyid_of_prim(cx->arena, PRIM_INT);
		default:
			if (tyid_equal(lt, tyid_of_prim(cx->arena, PRIM_FLOAT)) ||
			    tyid_equal(rt, tyid_of_prim(cx->arena, PRIM_FLOAT)))
				return tyid_of_prim(cx->arena, PRIM_FLOAT);
			return lt;
		}
	}
	case EXPR_INDEX: {
		/* Catch the audit case `5[0]`: indexing a literal of non-string type is
		 * always wrong. Wider not-indexable checks (e.g. `int_var[0]`) need a
		 * collection-aware type lookup — semantic.c's resolve_expression_type
		 * doesn't track "this is an array vs a scalar" cleanly today, so we
		 * stay conservative. */
		Expression *base = e->data.index.base;
		if (base && base->type == EXPR_LITERAL) {
			TypeId bt = synth(cx, base);
			TyKind bk = tyid_kind(cx->arena, bt);
			if (bk == TYK_PRIM) {
				char bn[32];
				tyid_display(cx->arena, bt, bn, sizeof(bn));
				if (strcmp(bn, "str") != 0)
					sem_emit_not_indexable(cx->ctx, e->loc, bn);
			}
		}
		if (cx->model && e->cst_id) {
			const char *s = sem_model_expr_type(cx->model, e->cst_id - 1);
			if (s)
				return tyid_from_name(cx->arena, cx->ctx, s);
		}
		return TYID_UNKNOWN;
	}
	case EXPR_NAME:
	case EXPR_FIELD: {
		/* A bare name denoting a proc/func is a first-class callable value: its type is the
		 * structural signature (so `h: Handler = some_proc` is checked, callbacks type, etc.). */
		if (e->type == EXPR_NAME) {
			CalleeRef cr = find_callee(cx->prog, e->data.name.name);
			if (cr.kind != CALLEE_NONE)
				return tyid_of_callee(cx, cr);
		}
		/* Bridge to semantic.c's resolved types: every expression with a cst_id has
		 * its type recorded in SemModel during analyze_expression. We map the
		 * string back to a TypeId. */
		if (cx->model && e->cst_id) {
			const char *s = sem_model_expr_type(cx->model, e->cst_id - 1);
			if (s)
				return tyid_from_name(cx->arena, cx->ctx, s);
		}
		return TYID_UNKNOWN;
	}
	default:
		/* Phase B continues: encode the rest of the rulebook (EXPR_NAME via
		 * symbol-table bridge, EXPR_FIELD, EXPR_INDEX, EXPR_BINARY, …). Until then,
		 * every other shape is unknown so `check` lets it pass. */
		return TYID_UNKNOWN;
	}
}

/* Compatibility for ASSIGNMENT contexts (let/assign/return/call-arg). arche has
 * no `as` cast yet, so int/char/width-ints all coerce silently into each other
 * at storage boundaries (e.g. `buf[0] = 65` is canonical). But int↔float at an
 * assignment boundary is rejected — a typed binding's declared kind matters.
 * Strict typing applies between str↔scalar and across distinct nominals. */
static int prim_silently_compatible(const TypeArena *arena, TypeId a, TypeId b) {
	char an[32];
	char bn[32];
	tyid_display(arena, a, an, sizeof(an));
	tyid_display(arena, b, bn, sizeof(bn));
	int a_intish = (strcmp(an, "int") == 0 || strcmp(an, "char") == 0 || is_width_int_name(an));
	int b_intish = (strcmp(bn, "int") == 0 || strcmp(bn, "char") == 0 || is_width_int_name(bn));
	if (a_intish && b_intish)
		return 1;
	/* opaque flows freely with int-shaped values (handles are pointer-width ints). */
	if ((strcmp(an, "opaque") == 0 && (b_intish || strcmp(bn, "opaque") == 0)) ||
	    (strcmp(bn, "opaque") == 0 && (a_intish || strcmp(an, "opaque") == 0)))
		return 1;
	return 0;
}

/* Compatibility for ARITHMETIC + COMPARISON. Designed to catch the audit cases
 * (str+int, str-numeric) without rejecting arche's broader mixing — which today
 * includes float+int (C widening), char arithmetic, column-broadcast (`field *
 * field`), and handle/opaque comparison with int. The rule: incompatible only
 * when ONE side is a string and the other is a scalar (or vice versa). All
 * other pairings let through. Tighten when arche adopts an `as` cast operator. */
static int prim_binop_compatible(const TypeArena *arena, TypeId a, TypeId b) {
	char an[32];
	char bn[32];
	tyid_display(arena, a, an, sizeof(an));
	tyid_display(arena, b, bn, sizeof(bn));
	int a_str = (strcmp(an, "str") == 0);
	int b_str = (strcmp(bn, "str") == 0);
	if (a_str != b_str) /* exactly one side is str → incompatible */
		return 0;
	return 1;
}

static void check(TyCtx *cx, Expression *e, TypeId expected, const char *where) {
	if (!e || tyid_is_unknown(expected))
		return;
	/* Untyped-literal flexibility: an integer literal fits any int-family type,
	 * a char literal fits char or int, a float literal fits float. Bypass the
	 * stricter equality check below. */
	if (check_literal_fits(cx->arena, e, expected))
		return;
	TypeId got = synth(cx, e);
	if (tyid_is_unknown(got))
		return; /* fail-open: rule not encoded yet for this shape */
	if (tyid_equal(got, expected))
		return;
	if (prim_silently_compatible(cx->arena, got, expected))
		return;
	char want[128];
	char have[128];
	tyid_display(cx->arena, expected, want, sizeof(want));
	tyid_display(cx->arena, got, have, sizeof(have));
	sem_emit_type_mismatch(cx->ctx, e->loc, where, want, have);
}

/* Walk an expression to fire tycheck rules on sub-expressions (mainly
 * EXPR_CALL, which itself recurses through check/synth). For most expression
 * kinds we just synth and discard — synth has the side effect of emitting
 * diagnostics for call-arity / arg-type failures. */
static void visit_expr(TyCtx *cx, Expression *e) {
	if (!e)
		return;
	(void)synth(cx, e);
	/* Drive recursion through the AST shape so calls nested inside non-call
	 * expressions still get walked. synth(EXPR_CALL) already recurses via
	 * check() on each arg; for other kinds we visit sub-exprs explicitly. */
	switch (e->type) {
	case EXPR_BINARY:
		visit_expr(cx, e->data.binary.left);
		visit_expr(cx, e->data.binary.right);
		break;
	case EXPR_UNARY:
		visit_expr(cx, e->data.unary.operand);
		break;
	case EXPR_FIELD:
		visit_expr(cx, e->data.field.base);
		break;
	case EXPR_INDEX:
		visit_expr(cx, e->data.index.base);
		for (int i = 0; i < e->data.index.index_count; i++)
			visit_expr(cx, e->data.index.indices[i]);
		break;
	case EXPR_CALL:
		visit_expr(cx, e->data.call.callee);
		/* args are already walked by synth_call via check() — don't double-walk */
		break;
	case EXPR_ARRAY_LITERAL:
		for (int i = 0; i < e->data.array_literal.element_count; i++)
			visit_expr(cx, e->data.array_literal.elements[i]);
		break;
	default:
		break;
	}
}

/* Walk a statement; visit every contained expression + recurse into bodies. */
static void visit_stmt(TyCtx *cx, Statement *stmt, FuncDecl *fn);
static void visit_stmts(TyCtx *cx, Statement **stmts, int count, FuncDecl *fn) {
	for (int i = 0; i < count; i++)
		visit_stmt(cx, stmts[i], fn);
}

static void visit_stmt(TyCtx *cx, Statement *stmt, FuncDecl *fn) {
	if (!stmt)
		return;
	switch (stmt->type) {
	case STMT_BIND: {
		Expression *value = stmt->data.bind_stmt.value;
		if (value)
			visit_expr(cx, value);
		/* Typed bind `x : T = e` or `x : T : e`: check the RHS against the
		 * declared type T. Inferred binds (`x := e`) have no type to check
		 * against — synth determines T from e implicitly. */
		if (value && stmt->data.bind_stmt.type) {
			TypeId expected = tyid_from_typeref(cx->arena, cx->ctx, stmt->data.bind_stmt.type);
			check(cx, value, expected, "binding");
		}
		break;
	}
	case STMT_ASSIGN: {
		Expression *target = stmt->data.assign_stmt.target;
		Expression *value = stmt->data.assign_stmt.value;
		if (target)
			visit_expr(cx, target);
		if (value)
			visit_expr(cx, value);
		/* `x = e` (or `f.x = e`, `a[i] = e`) — check the RHS against the LHS's
		 * declared type. Fail-open if target's type is unknown. */
		if (target && value) {
			TypeId expected = synth(cx, target);
			check(cx, value, expected, "assignment");
		}
		break;
	}
	case STMT_MULTI_BIND:
		if (stmt->data.multi_bind.value)
			visit_expr(cx, stmt->data.multi_bind.value);
		break;
	case STMT_EXPR:
		visit_expr(cx, stmt->data.expr_stmt.expr);
		break;
	case STMT_RETURN: {
		for (int i = 0; i < stmt->data.return_stmt.count; i++)
			visit_expr(cx, stmt->data.return_stmt.values[i]);
		if (!fn)
			return; /* return outside a func body — semantic.c handles structurally */
		int decl_count = fn->return_type_count;
		int got_count = stmt->data.return_stmt.count;
		if (decl_count != got_count) {
			sem_emit_wrong_return_arity(cx->ctx, stmt->loc, fn->name ? fn->name : "<anon>", decl_count, got_count);
		}
		/* Pair up the values that align, regardless of count mismatch — extra/missing
		 * are reported above; aligned positions still get value-type checked. */
		int n = decl_count < got_count ? decl_count : got_count;
		for (int i = 0; i < n; i++) {
			TypeId expected = tyid_from_typeref(cx->arena, cx->ctx, fn->return_types[i]);
			check(cx, stmt->data.return_stmt.values[i], expected, "return value");
		}
		break;
	}
	case STMT_IF:
		visit_expr(cx, stmt->data.if_stmt.cond);
		visit_stmts(cx, stmt->data.if_stmt.then_body, stmt->data.if_stmt.then_count, fn);
		visit_stmts(cx, stmt->data.if_stmt.else_body, stmt->data.if_stmt.else_count, fn);
		break;
	case STMT_FOR:
		if (stmt->data.for_stmt.iterable)
			visit_expr(cx, stmt->data.for_stmt.iterable);
		if (stmt->data.for_stmt.condition)
			visit_expr(cx, stmt->data.for_stmt.condition);
		if (stmt->data.for_stmt.init)
			visit_stmt(cx, stmt->data.for_stmt.init, fn);
		if (stmt->data.for_stmt.increment)
			visit_stmt(cx, stmt->data.for_stmt.increment, fn);
		cx->loop_depth++;
		visit_stmts(cx, stmt->data.for_stmt.body, stmt->data.for_stmt.body_count, fn);
		cx->loop_depth--;
		break;
	case STMT_BREAK:
		if (cx->loop_depth == 0)
			sem_emit_break_outside_loop(cx->ctx, stmt->loc);
		break;
	case STMT_EACH_FIELD:
		visit_stmts(cx, stmt->data.each_field.body, stmt->data.each_field.body_count, fn);
		break;
	default:
		break;
	}
}

void tycheck_run(SemanticContext *ctx) {
	if (!ctx)
		return;
	AstProgram *prog = semantic_context_program(ctx);
	if (!prog)
		return;

	TyCtx cx;
	cx.ctx = ctx;
	cx.arena = ty_arena_new();
	cx.prog = prog;
	cx.model = sem_context_model(ctx);
	cx.loop_depth = 0;

	for (int i = 0; i < prog->decl_count; i++) {
		Decl *d = prog->decls[i];
		if (!d)
			continue;
		/* PC3: detect duplicate top-level decls (proc/func of same name). Today's
		 * compiler bumps this to LLVM ("invalid redefinition of function 'foo'");
		 * caught upstream here so the user sees E0031 with a clean message at the
		 * declaration site. */
		const char *my_name = NULL;
		const char *my_kind = NULL;
		if (d->kind == DECL_FUNC && d->data.func) {
			my_name = d->data.func->name;
			my_kind = "func";
		} else if (d->kind == DECL_PROC && d->data.proc) {
			my_name = d->data.proc->name;
			my_kind = "proc";
		}
		if (my_name) {
			for (int j = 0; j < i; j++) {
				Decl *e = prog->decls[j];
				if (!e)
					continue;
				const char *other = NULL;
				if (e->kind == DECL_FUNC && e->data.func)
					other = e->data.func->name;
				else if (e->kind == DECL_PROC && e->data.proc)
					other = e->data.proc->name;
				if (other && strcmp(other, my_name) == 0) {
					sem_emit_duplicate_decl(cx.ctx, d->loc, my_kind, my_name);
					break;
				}
			}
		}

		if (d->kind == DECL_FUNC && d->data.func) {
			visit_stmts(&cx, d->data.func->statements, d->data.func->statement_count, d->data.func);
		} else if (d->kind == DECL_PROC && d->data.proc) {
			/* Procs return too; we pass NULL for `fn` because the return-value rule
			 * is encoded against FuncDecl today. Extend in Phase B when procs need it. */
			visit_stmts(&cx, d->data.proc->statements, d->data.proc->statement_count, NULL);
		}
		/* Phase B: also visit DECL_FUNC_GROUP members. */
	}

	ty_arena_free(cx.arena);
}
