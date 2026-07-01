/* GLSL compute-shader emission for `@gpu` maps — the GPU lowering of a `map` kernel.
 *
 * A `map` is already a loopless, branch-free, per-element kernel; that is exactly a compute shader's
 * `main()`. This pass turns each `@gpu` map into a Vulkan-style GLSL compute shader: one `std430` SSBO
 * per column, the row count in a push constant, and one invocation per element guarded by the standard
 * grid bound check. `select(cond, a, b)` lowers to a GLSL ternary (a branch-free GPU select).
 *
 * Supports same-typed 32-bit columns (all `float`, or all `int`/`uint`), arithmetic, comparisons,
 * `select`, and inlined compile-time constants. Integer `/` and `%` are emitted only with a statically
 * nonzero divisor (div-by-zero is UB on the GPU). Anything else — mixed or non-32-bit columns, singletons,
 * other calls — makes the map non-emittable; it is skipped (its CPU path is untouched) and reported. */

#include "gpu_glsl.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A small growable text buffer. */
typedef struct {
	char *data;
	size_t len, cap;
	int ok; /* cleared if an unsupported construct is hit → the whole shader is abandoned */
} GBuf;

static void gb_init(GBuf *b) {
	b->cap = 1024;
	b->data = malloc(b->cap);
	b->len = 0;
	b->ok = 1;
	b->data[0] = '\0';
}
static void gb_putf(GBuf *b, const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	char tmp[1024];
	int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	if (b->len + (size_t)n + 1 > b->cap) {
		while (b->len + (size_t)n + 1 > b->cap)
			b->cap *= 2;
		b->data = realloc(b->data, b->cap);
	}
	memcpy(b->data + b->len, tmp, (size_t)n + 1);
	b->len += (size_t)n;
}
static void gb_free(GBuf *b) {
	free(b->data);
}

/* Is `name` one of the map's column parameters? */
static int is_column(HirKernelDecl *map, const char *name) {
	for (int i = 0; i < map->param_count; i++)
		if (map->params[i]->name && strcmp(map->params[i]->name, name) == 0)
			return 1;
	return 0;
}

static const char *binop_glsl(Operator op) {
	switch (op) {
	case OP_ADD:
		return "+";
	case OP_SUB:
		return "-";
	case OP_MUL:
		return "*";
	case OP_DIV:
		return "/";
	case OP_MOD:
		return NULL; /* float `%` not supported in the GPU emitter; callers guard on NULL */
	case OP_EQ:
		return "==";
	case OP_NEQ:
		return "!=";
	case OP_LT:
		return "<";
	case OP_GT:
		return ">";
	case OP_LTE:
		return "<=";
	case OP_GTE:
		return ">=";
	default:
		return NULL;
	}
}
static int op_is_compare(Operator op) {
	return op >= OP_EQ && op <= OP_GTE;
}

/* GPU emit context: the kernel, the program (for resolving compile-time constants), and the shader's
 * SCALAR element type. Every column of a GPU-emittable map is the SAME 32-bit type (float, int, or uint),
 * so one type describes the whole shader — SSBOs, literals, and comparison-as-value casts all render in it. */
typedef struct {
	HirKernelDecl *map;
	HirProgram *prog; /* for resolving `NAME` references to compile-time constants; may be NULL */
	const char *ety;  /* GLSL scalar type: "float", "int", or "uint" */
	int is_float;     /* ety == "float" — controls integer-literal float-izing and division safety */
} EmitCtx;

/* The GLSL scalar type for an arche column type, or NULL if not GPU-emittable in v1 (only 32-bit float /
 * int / uint — other widths need a different SSBO stride and dispatch elem_size, a follow-on). */
static const char *glsl_scalar_type(const HirType *t) {
	if (!t)
		return NULL;
	if (t->tag == HIR_TYPE_FLOAT)
		return "float";
	if (t->tag == HIR_TYPE_INT && (t->int_width == 0 || t->int_width == 32))
		return t->int_signed ? "int" : "uint"; /* arche's default `int` is signed 32-bit */
	return NULL;
}

/* The literal lexeme of a scalar compile-time constant `name` (`STIFFNESS :: 24`), or NULL if `name` is
 * not such a constant (unknown, or a non-literal const the v1 emitter can't inline). */
static const char *gpu_const_lexeme(HirProgram *prog, const char *name) {
	if (!prog || !name)
		return NULL;
	for (int i = 0; i < prog->decl_count; i++) {
		HirDecl *d = prog->decls[i];
		if (d && d->kind == HIR_DECL_CONST && d->data.constant && d->data.constant->name &&
		    strcmp(d->data.constant->name, name) == 0) {
			HirExpr *v = d->data.constant->value;
			return (v && v->kind == HIR_EXPR_LITERAL) ? v->data.literal.lexeme : NULL;
		}
	}
	return NULL;
}

/* Is `e` a statically nonzero INTEGER (literal or int constant)? GPU integer `/` and `%` are UB on a zero
 * divisor and there is no failure policy on the GPU, so we only emit them when the divisor is provably
 * nonzero; otherwise the map stays on the CPU, where its `!` divide policy applies. */
static int gpu_nonzero_int_const(HirProgram *prog, HirExpr *e) {
	if (!e)
		return 0;
	const char *lx = NULL;
	if (e->kind == HIR_EXPR_LITERAL)
		lx = e->data.literal.lexeme;
	else if (e->kind == HIR_EXPR_NAME)
		lx = gpu_const_lexeme(prog, e->data.name.name);
	if (!lx || strchr(lx, '.'))
		return 0; /* absent, or a float — not an integer divisor */
	return strtol(lx, NULL, 0) != 0;
}

/* Emit a scalar literal lexeme in the shader's element type: float-ize a bare integer for a float shader;
 * for an int shader emit the integer as-is and REJECT a fractional lexeme (a type mismatch the homogeneous
 * v1 emitter can't represent). NULL lexeme (unresolved const / singleton) → non-emittable. */
static void emit_scalar_lexeme(GBuf *b, EmitCtx *ec, const char *lx) {
	if (!lx) {
		b->ok = 0;
		return;
	}
	int frac = strchr(lx, '.') || strchr(lx, 'e') || strchr(lx, 'E');
	if (ec->is_float) {
		gb_putf(b, "%s", lx);
		if (!frac)
			gb_putf(b, ".0"); /* float-ize an integer literal for a float column */
	} else if (frac) {
		b->ok = 0; /* a fractional literal in an int shader — unrepresentable in v1 */
	} else {
		gb_putf(b, "%s", lx);
	}
}

/* Emit a map-body expression as GLSL. `want_bool` requests a boolean (for a `select` condition); otherwise
 * a value of the shader's element type is produced (a comparison in value position is wrapped `T(...)` so it
 * stays well-typed, matching arche's "comparison is 0/1" semantics). Clears `b->ok` on anything the v1
 * emitter can't lower. */
static void emit_expr(GBuf *b, EmitCtx *ec, HirExpr *e, int want_bool) {
	if (!b->ok || !e) {
		b->ok = 0;
		return;
	}
	switch (e->kind) {
	case HIR_EXPR_NAME:
		if (is_column(ec->map, e->data.name.name)) {
			gb_putf(b, "%s[i]", e->data.name.name); /* a column read at this element */
		} else {
			/* not a column — inline a compile-time constant (`STIFFNESS`); else unsupported (singleton/…) */
			emit_scalar_lexeme(b, ec, gpu_const_lexeme(ec->prog, e->data.name.name));
		}
		break;
	case HIR_EXPR_LITERAL:
		emit_scalar_lexeme(b, ec, e->data.literal.lexeme ? e->data.literal.lexeme : "0");
		break;
	case HIR_EXPR_UNARY:
		gb_putf(b, "(-(");
		emit_expr(b, ec, e->data.unary.operand, 0);
		gb_putf(b, "))");
		break;
	case HIR_EXPR_BINARY: {
		Operator op = e->data.binary.op;
		const char *o = (op == OP_MOD) ? "%" : binop_glsl(op);
		if (!o) { /* unsupported operator (`&&`, `||`, …) */
			b->ok = 0;
			break;
		}
		if (op == OP_MOD && ec->is_float) { /* float `%` has no GLSL form */
			b->ok = 0;
			break;
		}
		if ((op == OP_DIV || op == OP_MOD) && !ec->is_float &&
		    !gpu_nonzero_int_const(ec->prog, e->data.binary.right)) {
			b->ok = 0; /* unsafe integer div/mod (non-constant divisor) — keep on the CPU */
			break;
		}
		int cmp = op_is_compare(op);
		if (cmp && !want_bool)
			gb_putf(b, "%s(", ec->ety); /* comparison used as a 0/1 value, in the shader's element type */
		gb_putf(b, "(");
		emit_expr(b, ec, e->data.binary.left, 0);
		gb_putf(b, " %s ", o);
		emit_expr(b, ec, e->data.binary.right, 0);
		gb_putf(b, ")");
		if (cmp && !want_bool)
			gb_putf(b, ")");
		break;
	}
	case HIR_EXPR_CALL: {
		const char *fn = e->data.call.callee && e->data.call.callee->kind == HIR_EXPR_NAME
		                     ? e->data.call.callee->data.name.name
		                     : NULL;
		if (fn && strcmp(fn, "select") == 0 && e->data.call.arg_count == 3) {
			gb_putf(b, "((");
			emit_expr(b, ec, e->data.call.args[0], 1); /* condition → bool */
			gb_putf(b, ") ? (");
			emit_expr(b, ec, e->data.call.args[1], 0);
			gb_putf(b, ") : (");
			emit_expr(b, ec, e->data.call.args[2], 0);
			gb_putf(b, "))");
		} else {
			b->ok = 0; /* a func/proc call in a GPU kernel — not supported in v1 */
		}
		break;
	}
	default:
		b->ok = 0;
		break;
	}
}

/* Emit one assignment `col = expr` (or `col op= expr`) as a GLSL statement. */
static void emit_assign(GBuf *b, EmitCtx *ec, HirAssignStmt *a) {
	if (!a->target || a->target->kind != HIR_EXPR_NAME || !is_column(ec->map, a->target->data.name.name)) {
		b->ok = 0;
		return;
	}
	const char *t = a->target->data.name.name;
	gb_putf(b, "  %s[i] = ", t);
	if (a->op != OP_NONE) {
		const char *o = (a->op == OP_MOD) ? "%" : binop_glsl(a->op);
		if (!o || op_is_compare(a->op)) {
			b->ok = 0;
			return;
		}
		if (a->op == OP_MOD && ec->is_float) {
			b->ok = 0;
			return;
		}
		if ((a->op == OP_DIV || a->op == OP_MOD) && !ec->is_float && !gpu_nonzero_int_const(ec->prog, a->value)) {
			b->ok = 0; /* unsafe integer div/mod (non-constant divisor) — keep on the CPU */
			return;
		}
		gb_putf(b, "%s[i] %s (", t, o);
		emit_expr(b, ec, a->value, 0);
		gb_putf(b, ")");
	} else {
		emit_expr(b, ec, a->value, 0);
	}
	gb_putf(b, ";\n");
}

static void emit_stmts(GBuf *b, EmitCtx *ec, HirStmt **stmts, int count) {
	for (int i = 0; i < count && b->ok; i++) {
		HirStmt *s = stmts[i];
		if (!s)
			continue;
		if (s->kind == HIR_STMT_ASSIGN)
			emit_assign(b, ec, &s->data.assign_stmt);
		else if (s->kind == HIR_STMT_BLOCK)
			emit_stmts(b, ec, s->data.block.stmts, s->data.block.count);
		else
			b->ok = 0; /* control flow shouldn't reach here (the map whitelist forbids it), but be safe */
	}
}

/* Every column of `map` present in `arch`, all the SAME 32-bit scalar type → that GLSL type ("float" /
 * "int" / "uint"); NULL if a column is missing, an unsupported type/width, or the map MIXES types (mixed
 * int/float in one map needs per-expression casts — a follow-on). One type describes the whole shader. */
static const char *map_glsl_elem_type(HirKernelDecl *map, HirArchetypeDecl *arch) {
	const char *ety = NULL;
	for (int p = 0; p < map->param_count; p++) {
		const char *pn = map->params[p]->name;
		const char *ct = NULL;
		for (int f = 0; f < arch->field_count; f++)
			if (arch->fields[f]->kind == FIELD_COLUMN && strcmp(arch->fields[f]->name, pn) == 0) {
				ct = glsl_scalar_type(arch->fields[f]->type);
				break;
			}
		if (!ct)
			return NULL; /* missing column or unsupported type/width */
		if (!ety)
			ety = ct;
		else if (strcmp(ety, ct) != 0)
			return NULL; /* homogeneous only in v1 */
	}
	return ety;
}

/* Build the full shader text for (map, arch). Returns a malloc'd string on success, NULL if the body is
 * not GPU-emittable in v1. `prog` supplies compile-time constants referenced in the body. */
char *gpu_glsl_build_src(HirProgram *prog, HirKernelDecl *map, HirArchetypeDecl *arch) {
	const char *ety = map_glsl_elem_type(map, arch);
	if (!ety)
		return NULL;
	EmitCtx ec = {.map = map, .prog = prog, .ety = ety, .is_float = (strcmp(ety, "float") == 0)};

	GBuf body;
	gb_init(&body);
	emit_stmts(&body, &ec, map->stmts, map->stmt_count);
	if (!body.ok || body.len == 0) {
		gb_free(&body);
		return NULL;
	}

	GBuf out;
	gb_init(&out);
	gb_putf(&out, "#version 450\n");
	gb_putf(&out, "// generated from `%s` over archetype `%s` — the GPU lowering of an arche `map`.\n", map->name,
	        arch->name);
	gb_putf(&out, "layout(local_size_x = 64) in;\n");
	int binding = 0;
	for (int p = 0; p < map->param_count; p++) {
		const char *pn = map->params[p]->name;
		gb_putf(&out, "layout(std430, binding = %d) buffer B_%s { %s %s[]; };\n", binding++, pn, ety, pn);
	}
	gb_putf(&out, "layout(push_constant) uniform Params { uint count; };\n");
	gb_putf(&out, "void main() {\n");
	gb_putf(&out, "  uint i = gl_GlobalInvocationID.x;\n");
	gb_putf(&out, "  if (i >= count) return;\n"); /* grid bound guard (uniform, not a data branch) */
	gb_putf(&out, "%s", body.data);
	gb_putf(&out, "}\n");
	gb_free(&body);
	return out.data; /* caller frees */
}

/* Mark a map for GPU emission if any `run <map> @gpu` dispatches it. GPU dispatch is a call-site decision
 * (`run step @gpu`), so the trigger lives on the run statement; this propagates it to the map the emitter
 * walks. Recurses into block statements (a run can sit inside a desugared block). */
static void mark_gpu_runs_in(HirProgram *prog, HirStmt **stmts, int count) {
	for (int i = 0; i < count; i++) {
		HirStmt *s = stmts[i];
		if (!s)
			continue;
		if (s->kind == HIR_STMT_RUN && s->data.run_stmt.is_gpu && s->data.run_stmt.map_name) {
			for (int d = 0; d < prog->decl_count; d++)
				if (prog->decls[d] && prog->decls[d]->kind == HIR_DECL_KERNEL &&
				    prog->decls[d]->data.kernel && prog->decls[d]->data.kernel->kind == HIR_KERNEL_MAP &&
				    prog->decls[d]->data.kernel->name &&
				    strcmp(prog->decls[d]->data.kernel->name, s->data.run_stmt.map_name) == 0)
					prog->decls[d]->data.kernel->is_gpu = 1;
		} else if (s->kind == HIR_STMT_BLOCK) {
			mark_gpu_runs_in(prog, s->data.block.stmts, s->data.block.count);
		}
	}
}

void gpu_glsl_mark_runs(HirProgram *prog) {
	if (!prog)
		return;
	for (int i = 0; i < prog->decl_count; i++) {
		HirDecl *d = prog->decls[i];
		if (d && d->kind == HIR_DECL_PROC && d->data.proc)
			mark_gpu_runs_in(prog, d->data.proc->stmts, d->data.proc->stmt_count);
	}
}

HirArchetypeDecl *gpu_glsl_first_emittable_arch(HirProgram *prog, HirKernelDecl *map) {
	if (!prog || !map)
		return NULL;
	for (int i = 0; i < prog->decl_count; i++) {
		HirDecl *d = prog->decls[i];
		if (d && d->kind == HIR_DECL_ARCHETYPE && d->data.archetype && map_glsl_elem_type(map, d->data.archetype))
			return d->data.archetype;
	}
	return NULL;
}

int arche_gpu_emit(HirProgram *prog, const char *out_dir, int *out_count) {
	if (!prog)
		return 0;
	int written = 0, gpu_maps = 0;

	/* Propagate `run ... @gpu` dispatch markers from proc bodies onto the target maps. */
	gpu_glsl_mark_runs(prog);

	/* Index archetype decls once. */
	HirArchetypeDecl **archs = calloc(prog->decl_count ? (size_t)prog->decl_count : 1, sizeof(*archs));
	int narch = 0;
	for (int i = 0; i < prog->decl_count; i++)
		if (prog->decls[i] && prog->decls[i]->kind == HIR_DECL_ARCHETYPE && prog->decls[i]->data.archetype)
			archs[narch++] = prog->decls[i]->data.archetype;

	for (int i = 0; i < prog->decl_count; i++) {
		HirDecl *d = prog->decls[i];
		if (!d || d->kind != HIR_DECL_KERNEL || !d->data.kernel ||
		    d->data.kernel->kind != HIR_KERNEL_MAP || !d->data.kernel->is_gpu)
			continue;
		HirKernelDecl *map = d->data.kernel;
		gpu_maps++;
		int emitted_for_map = 0;
		for (int a = 0; a < narch; a++) {
			if (!map_glsl_elem_type(map, archs[a]))
				continue;
			char *src = gpu_glsl_build_src(prog, map, archs[a]);
			if (!src)
				continue;
			char path[1024];
			snprintf(path, sizeof(path), "%s/%s__%s.comp", out_dir, map->name, archs[a]->name);
			FILE *fp = fopen(path, "w");
			if (!fp) {
				fprintf(stderr, "arche: could not write GPU shader %s\n", path);
				free(src);
				free(archs);
				return -1;
			}
			fputs(src, fp);
			fclose(fp);
			free(src);
			written++;
			emitted_for_map = 1;
		}
		if (!emitted_for_map)
			fprintf(stderr,
			        "arche: note: `@gpu` map `%s` not GPU-emittable in v1 (needs same-typed 32-bit "
			        "float/int columns, arithmetic/select only); CPU path unaffected.\n",
			        map->name);
	}
	free(archs);
	if (out_count)
		*out_count = gpu_maps;
	return written;
}
