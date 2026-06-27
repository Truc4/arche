#include "tycheck.h"

#include "sem_decls.h"
#include "sem_diagnostics.h"
#include "sem_model.h"
#include "sem_types.h"

#include "../syntax/syntax_tree.h"
#include "../syntax/syntax_view.h"
#include "../syntax/type_ref.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* AST-kill: tycheck walks the SyntaxView tree + the resolved DeclTable (semantic_decl_*) + SemModel,
 * exactly like the rest of analysis — no AstProgram. synth(e) computes an expression's TypeId
 * bottom-up; check(e, expected) reports E0200 when synth(e) and `expected` are known and disjoint.
 * Fail-open: an unencoded shape synths to TYID_UNKNOWN and `check` lets it pass. */

typedef struct {
	SemanticContext *ctx;
	TypeArena *arena;
	SemModel *model;
	int loop_depth; /* 0 outside any loop; incremented around a for-body */
} TyCtx;

/* Resolved callee summary for a call/name, or NULL for a builtin / undefined / non-callable. */
static const DeclSummary *find_callee(SemanticContext *ctx, const char *name) {
	return semantic_find_callable_sig(ctx, name);
}

/* Width-int family: byte, i8/i16/i32/i64/i128, u8/u16/u32/u64/u128. An untyped integer literal can
 * flow into any of them (Rust/Go-style untyped constant). */
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

/* The literal lexeme of an SN_LITERAL_EXPR view (caller frees), or NULL for any other shape. */
static char *literal_lexeme(SyntaxView e) {
	if (!sv_present(e) || sv_kind(e) != SN_LITERAL_EXPR)
		return NULL;
	return sem_cv_dup_first_token(e);
}

/* Untyped-literal flexibility (Rust/Go model): a literal whose value can plausibly inhabit
 * `expected` is accepted. */
static int check_literal_fits(const TypeArena *arena, SemanticContext *ctx, SyntaxView e, TypeId expected) {
	char *lex = literal_lexeme(e);
	if (!lex)
		return 0;
	int is_float_lit = (lex[0] != '"' && lex[0] != '\'' && (strchr(lex, '.') || strchr(lex, 'e') || strchr(lex, 'E')));
	int is_int_lit = (lex[0] != '"' && lex[0] != '\'' && !is_float_lit);
	int is_char_lit = (lex[0] == '\'');
	free(lex);

	int ok = 0;
	TyKind k = tyid_kind(arena, expected);
	if (k == TYK_PRIM) {
		char want[16];
		tyid_display(arena, expected, want, sizeof(want));
		if (is_int_lit && (strcmp(want, "int") == 0 || strcmp(want, "float") == 0 || strcmp(want, "char") == 0))
			ok = 1;
		if (is_char_lit && (strcmp(want, "char") == 0 || strcmp(want, "int") == 0))
			ok = 1;
		if (is_float_lit && strcmp(want, "float") == 0)
			ok = 1;
	}
	if (!ok && k == TYK_NOMINAL) {
		char want[32];
		tyid_display(arena, expected, want, sizeof(want));
		if (is_width_int_name(want) && (is_int_lit || is_char_lit))
			ok = 1;
		/* An enum is a closed NAMED set, not a numeric continuum: a bare int literal does not inhabit it
		 * (you must name a case — `color.red` — or convert explicitly). Other int-backed subtypes still
		 * take untyped literals (`x: meters = 5`). */
		if (!ok && ctx && semantic_is_type_alias(ctx, want) && !semantic_is_enum_type(ctx, want)) {
			const char *b = semantic_resolve_type_alias(ctx, want);
			if (b) {
				if (is_int_lit && (strcmp(b, "int") == 0 || strcmp(b, "float") == 0 || strcmp(b, "char") == 0 ||
				                   is_width_int_name(b)))
					ok = 1;
				if (is_char_lit && (strcmp(b, "char") == 0 || strcmp(b, "int") == 0 || is_width_int_name(b)))
					ok = 1;
				if (is_float_lit && strcmp(b, "float") == 0)
					ok = 1;
			}
		}
	}
	return ok;
}

static TypeId synth(TyCtx *cx, SyntaxView e);
static void check(TyCtx *cx, SyntaxView e, TypeId expected, const char *where);
static int prim_binop_compatible(const TypeArena *arena, TypeId a, TypeId b);

/* The structural callable TypeId of a proc/func decl (its signature as a value). */
static TypeId tyid_of_callee(TyCtx *cx, const DeclSummary *d) {
	TypeId pbuf[32], rbuf[8];
	int pc = d->param_count > 32 ? 32 : d->param_count;
	int is_proc = (d->kind == DECL_PROC);
	int rc;
	for (int i = 0; i < pc; i++)
		pbuf[i] = d->params[i].type_id;
	if (is_proc) {
		rc = d->out_param_count > 8 ? 8 : d->out_param_count;
		for (int i = 0; i < rc; i++)
			rbuf[i] = d->out_params[i].type_id;
		return tyid_of_proc(cx->arena, pbuf, pc, rbuf, rc);
	}
	rc = d->return_type_count > 8 ? 8 : d->return_type_count;
	for (int i = 0; i < rc; i++)
		rbuf[i] = d->return_type_ids[i];
	return tyid_of_func(cx->arena, pbuf, pc, rbuf, rc);
}

/* Encode the call-typing rule: arity + per-arg type + return type, against the DeclTable. */
static TypeId synth_call(TyCtx *cx, SyntaxView e) {
	char *name = semantic_call_callee_name(cx->ctx, e);
	if (!name)
		return TYID_UNKNOWN;
	SourceLoc loc = sem_node_loc(e.node);
	const DeclSummary *cr = find_callee(cx->ctx, name);
	if (!cr) {
		/* `T(x)` conversion: the callee names a type, so the expression's type IS that type. */
		int is_prim = strcmp(name, "int") == 0 || strcmp(name, "float") == 0 || strcmp(name, "char") == 0 ||
		              strcmp(name, "str") == 0 || strcmp(name, "void") == 0 || is_width_int_name(name);
		TypeId r = (is_prim || (cx->ctx && semantic_is_type_alias(cx->ctx, name))) ? sem_tyid_of_name(cx->ctx, name)
		                                                                           : TYID_UNKNOWN;
		free(name);
		return r;
	}

	int is_func = (cr->kind == DECL_FUNC);
	int param_count = cr->param_count;
	int ret_count = is_func ? cr->return_type_count : 0;
	int is_variadic = cr->is_variadic;
	int arg_count = sem_expr_count(e);

	if (is_variadic) {
		if (arg_count < param_count) {
			sem_emit_wrong_arity(cx->ctx, loc, name, param_count, arg_count);
			TypeId r = ret_count > 0 ? cr->return_type_ids[0] : TYID_UNKNOWN;
			free(name);
			return r;
		}
	} else if (param_count != arg_count) {
		sem_emit_wrong_arity(cx->ctx, loc, name, param_count, arg_count);
		TypeId r = ret_count > 0 ? cr->return_type_ids[0] : TYID_UNKNOWN;
		free(name);
		return r;
	}

	for (int i = 0; i < arg_count && i < param_count; i++) {
		TypeId expected = cr->params[i].type_id;
		char where[64];
		snprintf(where, sizeof(where), "argument %d of '%s'", i + 1, name);
		check(cx, sem_node_at_expr(e, i), expected, where);
	}

	/* BUILD SITE: an extern under-applied in value position (in-args only) BUILDS an Eff value — its
	 * out-params are the out-slots; a VOID extern (no out-params) builds the empty `Eff()`. Mirrors
	 * call_type_id in semantic.c so tycheck agrees on the type. */
	if (cr->is_extern && cr->kind == DECL_PROC) {
		int oc = cr->out_param_count;
		TypeId obuf[16];
		const char *nbuf[16];
		TypeId *outs = oc > 16 ? malloc((size_t)oc * sizeof(TypeId)) : obuf;
		const char **names = oc > 16 ? malloc((size_t)oc * sizeof(const char *)) : nbuf;
		for (int i = 0; i < oc; i++) {
			outs[i] = cr->out_params[i].type_id;
			names[i] = cr->out_params[i].name; /* infer out-slot names from the extern's out-params */
		}
		TypeId r = tyid_of_eff_named(cx->arena, name, outs, names, oc);
		if (outs != obuf)
			free(outs);
		if (names != nbuf)
			free(names);
		free(name);
		return r;
	}
	TypeId r = ret_count > 0 ? cr->return_type_ids[0] : tyid_of_prim(cx->arena, PRIM_VOID);
	free(name);
	return r;
}

/* The resolved leftmost name of a NAME view: the ref_name channel, else the leading token text. */
static char *name_of(TyCtx *cx, SyntaxView e) {
	const char *rn = cx->model ? sem_model_ref_name(cx->model, sv_id(e)) : NULL;
	if (rn) {
		char *d = malloc(strlen(rn) + 1);
		strcpy(d, rn);
		return d;
	}
	return sem_cv_dup_first_token(e);
}

static TypeId synth(TyCtx *cx, SyntaxView e) {
	if (!sv_present(e))
		return TYID_UNKNOWN;
	switch (sv_kind(e)) {
	case SN_PAREN_EXPR:
		return synth(cx, sem_first_expr(e));
	case SN_LITERAL_EXPR: {
		char *lex = sem_cv_dup_first_token(e);
		TypeId r;
		if (!lex)
			r = TYID_UNKNOWN;
		else if (lex[0] == '"')
			r = tyid_of_prim(cx->arena, PRIM_STR);
		else if (lex[0] == '\'')
			r = tyid_of_prim(cx->arena, PRIM_CHAR);
		else if (strchr(lex, '.') || strchr(lex, 'e') || strchr(lex, 'E'))
			r = tyid_of_prim(cx->arena, PRIM_FLOAT);
		else
			r = tyid_of_prim(cx->arena, PRIM_INT);
		free(lex);
		return r;
	}
	case SN_STRING_EXPR:
		return tyid_of_prim(cx->arena, PRIM_STR);
	case SN_CALL_EXPR:
		return synth_call(cx, e);
	case SN_BINARY_EXPR: {
		SyntaxView l = sem_node_at_expr(e, 0);
		SyntaxView r = sem_node_at_expr(e, 1);
		if (sem_binary_op(e) == OP_FMAP) {
			/* `eff |> fin` — the result Eff yields the FINALIZER's return types (mirrors binary_type_id in
			 * semantic.c). The right operand is a func NAME, not a value; resolve it and read its declared
			 * returns. Without this, tycheck synthesizes the raw left Eff, so a type-changing finalizer
			 * (`i64 -> i32`) trips a false `return value` mismatch (`Eff(i32)` vs `Eff(i64)`). */
			TypeId lt = synth(cx, l);
			if (tyid_kind(cx->arena, lt) != TYK_EFF)
				return lt;
			const char *fname = cx->model ? sem_model_ref_name(cx->model, sv_id(r)) : NULL;
			const DeclSummary *fd = fname ? find_callee(cx->ctx, fname) : NULL;
			if (fd && fd->return_type_count > 0) {
				TypeId rbuf[8];
				int rc = fd->return_type_count > 8 ? 8 : fd->return_type_count;
				for (int i = 0; i < rc; i++)
					rbuf[i] = fd->return_type_ids[i];
				return tyid_of_eff_structural(cx->arena, rbuf, rc);
			}
			return lt;
		}
		TypeId lt = synth(cx, l);
		TypeId rt = synth(cx, r);
		/* A slice/array (an aggregate) is not an arithmetic/comparison operand — `M[i] + 1`, `s == t`.
		 * (This is now reachable because index/slice exprs carry real `[]T`/`[N]T` types.) */
		TyKind lk = tyid_kind(cx->arena, lt), rk = tyid_kind(cx->arena, rt);
		int aggr_operand = lk == TYK_SLICE || lk == TYK_ARRAY || rk == TYK_SLICE || rk == TYK_ARRAY;
		if (!tyid_is_unknown(lt) && !tyid_is_unknown(rt) &&
		    (aggr_operand || (!tyid_equal(lt, rt) && !prim_binop_compatible(cx->arena, lt, rt)))) {
			char want[64];
			char have[64];
			tyid_display(cx->arena, lt, want, sizeof(want));
			tyid_display(cx->arena, rt, have, sizeof(have));
			sem_emit_type_mismatch(cx->ctx, sem_node_loc(r.node), "binary operand", want, have);
		}
		Operator op = sem_binary_op(e);
		if (op >= OP_EQ && op <= OP_GTE)
			return tyid_of_prim(cx->arena, PRIM_INT);
		if (op == OP_AND || op == OP_OR)
			return tyid_of_prim(cx->arena, PRIM_INT);
		/* same-backing operator: take the LEFT operand's nominal subtype (literal carries none). */
		if (cx->ctx) {
			char ln[64];
			char rn[64];
			tyid_display(cx->arena, lt, ln, sizeof(ln));
			tyid_display(cx->arena, rt, rn, sizeof(rn));
			if (tyid_kind(cx->arena, lt) == TYK_NOMINAL && semantic_is_type_alias(cx->ctx, ln) &&
			    !semantic_alias_is_transparent(cx->ctx, ln))
				return lt;
			if (tyid_kind(cx->arena, rt) == TYK_NOMINAL && semantic_is_type_alias(cx->ctx, rn) &&
			    !semantic_alias_is_transparent(cx->ctx, rn))
				return rt;
		}
		if (tyid_equal(lt, tyid_of_prim(cx->arena, PRIM_FLOAT)) || tyid_equal(rt, tyid_of_prim(cx->arena, PRIM_FLOAT)))
			return tyid_of_prim(cx->arena, PRIM_FLOAT);
		return lt;
	}
	case SN_INDEX_EXPR:
	case SN_SLICE_EXPR:
		/* The interned identity (filled post-analysis from the model's nominal-preferring string). */
		return sem_model_expr_type_id(cx->model, sv_id(e));
	case SN_NAME_EXPR:
	case SN_FIELD_EXPR: {
		if (sv_kind(e) == SN_NAME_EXPR) {
			char *nm = name_of(cx, e);
			const DeclSummary *cr = nm ? find_callee(cx->ctx, nm) : NULL;
			free(nm);
			if (cr)
				return tyid_of_callee(cx, cr);
		}
		return sem_model_expr_type_id(cx->model, sv_id(e));
	}
	case SN_UNARY_EXPR:
		return synth(cx, sem_first_expr(e));
	case SN_ARRAY_LIT_EXPR: {
		/* The aggregate's type is `[N]elem` where `elem` is the FIRST element's type (the same first-
		 * element inference the static-decl path uses). Element-homogeneity is enforced in visit_expr so
		 * the diagnostic fires exactly once per literal — synth stays a pure (silent) type producer. */
		int n = sem_expr_count(e);
		if (n == 0)
			return TYID_UNKNOWN;
		TypeId el0 = synth(cx, sem_node_at_expr(e, 0));
		if (tyid_is_unknown(el0))
			return TYID_UNKNOWN;
		return tyid_of_array(cx->arena, el0, n);
	}
	default:
		return TYID_UNKNOWN;
	}
}

/* Compatibility for ASSIGNMENT contexts: int/char/width-ints coerce; int↔float rejected. An opaque
 * NEVER coerces to/from an int — opaque handles are sealed (born only at the FFI boundary, read/
 * written only in C); the only same-opaque case is exact identity, handled by `tyid_equal`. */
static int prim_silently_compatible(const TypeArena *arena, TypeId a, TypeId b) {
	char an[32];
	char bn[32];
	tyid_display(arena, a, an, sizeof(an));
	tyid_display(arena, b, bn, sizeof(bn));
	int a_intish = (strcmp(an, "int") == 0 || strcmp(an, "char") == 0 || is_width_int_name(an));
	int b_intish = (strcmp(bn, "int") == 0 || strcmp(bn, "char") == 0 || is_width_int_name(bn));
	if (a_intish && b_intish)
		return 1;
	return 0;
}

/* Compatibility for ARITHMETIC + COMPARISON: incompatible only when exactly one side is str. */
static int prim_binop_compatible(const TypeArena *arena, TypeId a, TypeId b) {
	char an[32];
	char bn[32];
	tyid_display(arena, a, an, sizeof(an));
	tyid_display(arena, b, bn, sizeof(bn));
	int a_str = (strcmp(an, "str") == 0);
	int b_str = (strcmp(bn, "str") == 0);
	return a_str == b_str;
}

/* Distinct-by-default subtype compatibility at an assignment/param/return boundary. */
static int subtype_check(TyCtx *cx, TypeId got, TypeId expected) {
	if (!cx->ctx)
		return -1;
	char gname[64];
	char ename[64];
	tyid_display(cx->arena, got, gname, sizeof(gname));
	tyid_display(cx->arena, expected, ename, sizeof(ename));
	int e_sub = semantic_is_type_alias(cx->ctx, ename) && !semantic_alias_is_transparent(cx->ctx, ename);
	int g_sub = semantic_is_type_alias(cx->ctx, gname) && !semantic_alias_is_transparent(cx->ctx, gname);
	/* A distinct subtype is usable AS any type in its backing CHAIN, not only the ultimate backing:
	 * `handle :: win` and `win :: opaque` make `handle` usable as `win` (its immediate backing) and as
	 * `opaque`. Check this first — it applies even when `expected` is itself an opaque-backed subtype
	 * (e.g. a `window` handle passed to a foreign proc), which the opaque-identity branches below reject. */
	if (g_sub) {
		const char *step = semantic_alias_backing_step(cx->ctx, gname);
		for (int guard = 0; step && guard <= 64; guard++) {
			if (strcmp(step, ename) == 0)
				return 1;
			step = semantic_alias_backing_step(cx->ctx, step);
		}
	}
	if (e_sub) {
		const char *eb = semantic_resolve_type_alias(cx->ctx, ename);
		if (eb && strcmp(eb, "opaque") == 0) {
			/* expected is an opaque handle: only the SAME opaque is acceptable (exact identity is caught
			 * by `tyid_equal` before this). A different opaque sibling, or any int, is incompatible — no
			 * minting an opaque from an int. */
			return 0;
		}
		return 0;
	}
	if (g_sub) {
		const char *gb = semantic_resolve_type_alias(cx->ctx, gname);
		if (gb && strcmp(gb, "opaque") == 0) {
			/* an opaque handle is usable only as the SAME opaque (exact, handled earlier) or as the bare
			 * `opaque` backing at the FFI seam — never read out as an int. */
			if (strcmp(ename, "opaque") == 0)
				return 1;
			return 0;
		}
		if (gb && strcmp(gb, ename) == 0)
			return 1;
		/* An int-backed distinct subtype (incl. an enum) is usable AS any integer width — `fd` flows
		 * into the `i64` syscall, `count` into an `int`, etc. (one-way; the reverse needs `T(x)`). */
		int gb_int = gb && (strcmp(gb, "int") == 0 || strcmp(gb, "char") == 0 || is_width_int_name(gb));
		int en_int = strcmp(ename, "int") == 0 || strcmp(ename, "char") == 0 || is_width_int_name(ename);
		if (gb_int && en_int)
			return 1;
	}
	return -1;
}

/* An array element is acceptable against the literal's inferred element type under the SAME rules an
 * assignment uses: exact identity, a one-way distinct-subtype fit, or a silent int-family widening.
 * Anything else (int vs float, int vs str, two unrelated nominals) is a genuine mixed-type literal. */
static int element_matches(TyCtx *cx, TypeId got, TypeId expected) {
	if (tyid_equal(got, expected))
		return 1;
	int sub = subtype_check(cx, got, expected);
	if (sub == 1)
		return 1;
	if (sub != 0 && prim_silently_compatible(cx->arena, got, expected))
		return 1;
	return 0;
}

/* True if `t` is a distinct (non-transparent) nominal whose backing is `opaque` — a sealed handle
 * type like `file`/`res`. Such a value's cell can never be written from arche (it is create-once and
 * only flows), so `=`-assigning over it is illegal. */
static int target_is_opaque_nominal(TyCtx *cx, TypeId t) {
	if (!cx->ctx || tyid_kind(cx->arena, t) != TYK_NOMINAL)
		return 0;
	char name[64];
	tyid_display(cx->arena, t, name, sizeof(name));
	/* The raw `opaque` base: a pool-column element type synthesised THROUGH its alias (`Holder.h[0]` where
	 * `h :: res` and `res :: opaque`) resolves to base `opaque`, not the nominal `res`. Catch it directly so
	 * the create-once seal covers columns of opaque, not just bare opaque nominals. */
	if (strcmp(name, "opaque") == 0)
		return 1;
	if (!semantic_is_type_alias(cx->ctx, name) || semantic_alias_is_transparent(cx->ctx, name))
		return 0;
	const char *b = semantic_resolve_type_alias(cx->ctx, name);
	return b && strcmp(b, "opaque") == 0;
}

static void check(TyCtx *cx, SyntaxView e, TypeId expected, const char *where) {
	if (!sv_present(e) || tyid_is_unknown(expected))
		return;
	/* Fail-open on structural kinds (array / shaped / handle / tuple / archetype): the DeclSummary now
	 * carries rich TypeIds for the analysis readers, but expression synthesis does not yet produce
	 * matching rich ids, so checking them would be spurious. This preserves the pre-Phase-3 coverage
	 * (NAME + callable only). Encoding these checks is a deliberate follow-up with golden updates. */
	switch (tyid_kind(cx->arena, expected)) {
	case TYK_SLICE:
	case TYK_ARRAY:
	case TYK_HANDLE:
	case TYK_TUPLE:
	case TYK_ARCHETYPE_CATEGORY:
		return;
	default:
		break;
	}
	if (check_literal_fits(cx->arena, cx->ctx, e, expected))
		return;
	TypeId got = synth(cx, e);
	if (tyid_is_unknown(got))
		return;
	if (tyid_equal(got, expected))
		return;
	/* An Eff position accepts a concrete `Eff#extern(out…)` where the STRUCTURAL `Eff(out…)` is declared
	 * (e.g. a func's `-> Eff(int,int)` return). Out-slots must match; the extern rides in the concrete id. */
	if (tyid_kind(cx->arena, expected) == TYK_EFF && tyid_usable_as(cx->arena, got, expected))
		return;
	int sub = subtype_check(cx, got, expected);
	if (sub == 1)
		return;
	if (sub != 0 && prim_silently_compatible(cx->arena, got, expected))
		return;
	/* A buffer/slice GOT decays to its data POINTER at integer / FFI boundaries (syscall args, extern
	 * C-ABI params, pointer-width slots): accept a slice/array where an integer is expected. This is the
	 * SAME coercion a buffer used before slices became first-class (when it collapsed to `char`, an
	 * intish type) — now expressed at the slice level rather than relying on the collapse. */
	TyKind gk = tyid_kind(cx->arena, got);
	if (gk == TYK_SLICE || gk == TYK_ARRAY) {
		char en[32];
		tyid_display(cx->arena, expected, en, sizeof(en));
		if (strcmp(en, "int") == 0 || strcmp(en, "char") == 0 || strcmp(en, "i64") == 0 || strcmp(en, "opaque") == 0 ||
		    is_width_int_name(en))
			return;
	}
	char want[128];
	char have[128];
	tyid_display(cx->arena, expected, want, sizeof(want));
	tyid_display(cx->arena, got, have, sizeof(have));
	sem_emit_type_mismatch(cx->ctx, sem_node_loc(e.node), where, want, have);
}

/* Walk an expression, firing tycheck rules on it + its sub-expressions. synth has the side effect
 * of emitting call-arity / arg-type / operand diagnostics; recursion reaches nested calls. */
static void visit_expr(TyCtx *cx, SyntaxView e) {
	if (!sv_present(e))
		return;
	(void)synth(cx, e);
	switch (sv_kind(e)) {
	case SN_PAREN_EXPR:
	case SN_UNARY_EXPR:
		visit_expr(cx, sem_first_expr(e));
		break;
	case SN_BINARY_EXPR:
		visit_expr(cx, sem_node_at_expr(e, 0));
		visit_expr(cx, sem_node_at_expr(e, 1));
		break;
	case SN_INDEX_EXPR:
	case SN_SLICE_EXPR:
		for (int i = 0, n = sem_expr_count(e); i < n; i++)
			visit_expr(cx, sem_node_at_expr(e, i));
		break;
	case SN_ARRAY_LIT_EXPR: {
		/* Walk each element, then enforce element-type homogeneity against the first element's type.
		 * `{ 1, "x" }` / `{ 1, 1.5 }` are mixed-type literals (E0200); `{ 1, 2, 3 }` is fine. */
		TypeId elem = TYID_UNKNOWN;
		int have_elem = 0;
		for (int i = 0, n = sem_expr_count(e); i < n; i++) {
			SyntaxView el = sem_node_at_expr(e, i);
			visit_expr(cx, el);
			TypeId et = synth(cx, el);
			if (tyid_is_unknown(et))
				continue;
			if (!have_elem) {
				elem = et;
				have_elem = 1;
				continue;
			}
			if (element_matches(cx, et, elem))
				continue;
			char want[128];
			char have[128];
			tyid_display(cx->arena, elem, want, sizeof(want));
			tyid_display(cx->arena, et, have, sizeof(have));
			sem_emit_type_mismatch(cx->ctx, sem_node_loc(el.node), "array element", want, have);
		}
		break;
	}
	case SN_CALL_EXPR:
		/* args already walked by synth_call via check(); don't double-walk */
		break;
	default:
		break;
	}
}

static void visit_stmt(TyCtx *cx, SyntaxView s, const DeclSummary *fn);

/* Visit every direct statement child of `v` (used for if then/else, each-field, block, match). */
static void visit_child_stmts(TyCtx *cx, SyntaxView v, const DeclSummary *fn) {
	for (int i = 0, n = sem_stmt_count(v); i < n; i++)
		visit_stmt(cx, sem_stmt_at(v, i), fn);
}

static void visit_stmt(TyCtx *cx, SyntaxView s, const DeclSummary *fn) {
	if (!sv_present(s))
		return;
	switch (sv_kind(s)) {
	case SN_BIND_STMT: {
		SyntaxView value = sem_node_at_expr(s, 1);
		SyntaxView type_view = sem_type_at(s, 0);
		if (sv_present(value))
			visit_expr(cx, value);
		/* typed bind `x : T = e` / `x : T : e`: check the RHS against T (untyped `x := e` has none). */
		if (sv_present(value) && sv_present(type_view))
			check(cx, value, sem_intern_view(cx->ctx, type_view), "binding");
		break;
	}
	case SN_ASSIGN_STMT: {
		SyntaxView target = sem_node_at_expr(s, 0);
		SyntaxView value = sem_node_at_expr(s, 1);
		if (sv_present(target))
			visit_expr(cx, target);
		if (sv_present(value))
			visit_expr(cx, value);
		if (sv_present(target) && sv_present(value)) {
			TypeId tt = synth(cx, target);
			/* Column ← array-literal init/scatter (`Slot.slot = { h }`, `Mob.pos.x = {1,2,3}`) MOVES each
			 * element into a pool row — drop-managed storage, not an overwrite of a local. Exempt it from the
			 * opaque create-once seal (which is for `local_opaque = …`); the element check runs below. */
			int col_scatter = (sv_kind(target) == SN_FIELD_EXPR && sv_kind(value) == SN_ARRAY_LIT_EXPR);
			/* An opaque handle is create-once — you may never overwrite an existing one with `=`. */
			if (!col_scatter && target_is_opaque_nominal(cx, tt)) {
				char on[64];
				tyid_display(cx->arena, tt, on, sizeof(on));
				sem_emit_opaque_overwrite(cx->ctx, sem_node_loc(target.node), on);
				break;
			}
			/* Column ← array-literal scatter (`Mob.pos.x = {1, 2, 3}`): each element fills a row, so check
			 * the ELEMENTS against the column type, not the array aggregate against the scalar (which would
			 * spuriously read `[3]i32` vs `float`). Only for a field target (a column); a plain scalar local
			 * `x = {…}` still falls through to the normal (rejecting) check. */
			if (sv_kind(target) == SN_FIELD_EXPR && sv_kind(value) == SN_ARRAY_LIT_EXPR) {
				for (int i = 0, n = sem_expr_count(value); i < n; i++)
					check(cx, sem_node_at_expr(value, i), tt, "column element");
			} else {
				check(cx, value, tt, "assignment");
			}
		}
		break;
	}
	case SN_MULTI_BIND_STMT:
	case SN_PROC_CALL_STMT: {
		/* the value is the sole call/expr child */
		for (int i = 0, n = sem_expr_count(s); i < n; i++) {
			SyntaxView v = sem_node_at_expr(s, i);
			if (sv_kind(v) == SN_CALL_EXPR) {
				visit_expr(cx, v);
				break;
			}
		}
		break;
	}
	case SN_EXPR_STMT:
		visit_expr(cx, sem_node_at_expr(s, 0));
		break;
	case SN_RETURN_STMT: {
		int got_count = sem_expr_count(s);
		for (int i = 0; i < got_count; i++)
			visit_expr(cx, sem_node_at_expr(s, i));
		if (!fn || fn->kind != DECL_FUNC)
			return; /* a proc's outputs are out-params; return-arity rule is func-only today */
		int decl_count = fn->return_type_count;
		if (decl_count != got_count)
			sem_emit_wrong_return_arity(cx->ctx, sem_node_loc(s.node), fn->name ? fn->name : "<anon>", decl_count,
			                            got_count);
		int n = decl_count < got_count ? decl_count : got_count;
		for (int i = 0; i < n; i++)
			check(cx, sem_node_at_expr(s, i), fn->return_type_ids[i], "return value");
		break;
	}
	case SN_IF_STMT:
		visit_expr(cx, sem_node_at_expr(s, 0));
		visit_child_stmts(cx, s, fn); /* then + else statement children */
		break;
	case SN_FOR_STMT: {
		for (int i = 0, n = sem_expr_count(s); i < n; i++)
			visit_expr(cx, sem_node_at_expr(s, i));
		cx->loop_depth++;
		visit_child_stmts(cx, s, fn);
		cx->loop_depth--;
		break;
	}
	case SN_BREAK_STMT:
		if (cx->loop_depth == 0)
			sem_emit_break_outside_loop(cx->ctx, sem_node_loc(s.node));
		break;
	case SN_CONTINUE_STMT:
		if (cx->loop_depth == 0)
			sem_emit_continue_outside_loop(cx->ctx, sem_node_loc(s.node));
		break;
	case SN_EACH_FIELD_STMT:
	case SN_BLOCK:
	case SN_MATCH_STMT:
		visit_child_stmts(cx, s, fn);
		break;
	default:
		break;
	}
}

void tycheck_run(SemanticContext *ctx) {
	if (!ctx)
		return;
	int n = semantic_decl_count(ctx);

	TyCtx cx;
	cx.ctx = ctx;
	cx.arena = sem_context_arena(ctx); /* shared, context-lived arena (owned by SemanticContext) */
	cx.model = sem_context_model(ctx);
	cx.loop_depth = 0;

	for (int i = 0; i < n; i++) {
		const DeclSummary *d = semantic_decl_at(ctx, i);
		if (!d)
			continue;
		/* PC3: duplicate top-level proc/func names — caught here with E0031 before LLVM. A policy is a
		 * separate namespace (invoked via `!name`, never called), so it doesn't clash with a func/proc. */
		if ((d->kind == DECL_FUNC || d->kind == DECL_PROC) && d->name && !d->is_policy) {
			const char *my_kind = d->kind == DECL_FUNC ? "func" : "proc";
			for (int j = 0; j < i; j++) {
				const DeclSummary *e = semantic_decl_at(ctx, j);
				if (!e || (e->kind != DECL_FUNC && e->kind != DECL_PROC) || e->is_policy || !e->name)
					continue;
				/* A stdlib symbol is module-qualified (`os.write`) and never duplicates a global/core/user
				 * name; the two only appear flat together in the codegen-test harness. Skip stdlib pairs. */
				if (d->origin == DECL_ORIGIN_STDLIB || e->origin == DECL_ORIGIN_STDLIB)
					continue;
				if (strcmp(e->name, d->name) == 0) {
					sem_emit_duplicate_decl(cx.ctx, d->loc, my_kind, d->name);
					break;
				}
			}
		}

		/* Type-check the body of every executable decl. Systems/eaches/maps had been SKIPPED — so a type
		 * error inside a `system`/`each`/`map` body (`x: int = "str"`, a bad call arg, `break` outside a
		 * loop) silently compiled. With `proc` going away and bodies moving into systems, they must get the
		 * same statement/expression checks as a `proc`. `fn` (the return-type context) is only a `func`;
		 * systems/eaches/maps have no return, so they pass NULL like a proc. */
		if (d->kind == DECL_FUNC || d->kind == DECL_PROC || d->kind == DECL_SYSTEM || d->kind == DECL_EACH ||
		    d->kind == DECL_MAP) {
			const DeclSummary *fn = d->kind == DECL_FUNC ? d : NULL;
			for (int k = 0, sc = sem_stmt_count(d->body_node); k < sc; k++)
				visit_stmt(&cx, sem_stmt_at(d->body_node, k), fn);
		}

		/* A top-level array/matrix initializer (`XS : [3]int = { … }`, `M :: { {…}, {…} }`) lives outside
		 * any proc body, so the statement walk never reaches it. Walk the literal directly so its element-
		 * homogeneity is checked the same as a local — `{ 1, "x", 3 }` is mixed-type wherever it appears. */
		if (d->kind == DECL_STATIC && d->static_kind == STATIC_KIND_ARRAY && d->static_has_init &&
		    sv_present(d->static_init) && sv_kind(d->static_init) == SN_ARRAY_LIT_EXPR)
			visit_expr(&cx, d->static_init);
	}
	/* The arena is owned by the SemanticContext (freed at its teardown), not here. */
}
