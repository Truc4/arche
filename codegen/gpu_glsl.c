/* GLSL compute-shader emission for `@gpu` maps — the GPU lowering of a `map` kernel.
 *
 * A `map` is already a loopless, branch-free, per-element kernel; that is exactly a compute shader's
 * `main()`. This pass turns each `@gpu` map into a Vulkan-style GLSL compute shader: one `std430` SSBO
 * per column, the row count in a push constant, and one invocation per element guarded by the standard
 * grid bound check. `select(cond, a, b)` lowers to a GLSL ternary (a branch-free GPU select).
 *
 * v1 supports the common case — float columns, arithmetic, comparisons, and `select`. Anything else
 * (non-float columns, consts/singletons, other calls) makes the map non-emittable; it is skipped (its
 * CPU path is untouched) and reported. See docs/DECISIONS_gpu.md. */

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
static int is_column(HirMapDecl *map, const char *name) {
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

/* Emit a map-body expression as GLSL. `want_bool` requests a boolean (for a `select` condition);
 * otherwise a `float` value is produced (a comparison in value position is wrapped `float(...)` so it
 * stays well-typed, matching arche's "comparison is 0/1" semantics). Clears `b->ok` on anything the v1
 * emitter can't lower. */
static void emit_expr(GBuf *b, HirMapDecl *map, HirExpr *e, int want_bool) {
	if (!b->ok || !e) {
		b->ok = 0;
		return;
	}
	switch (e->kind) {
	case HIR_EXPR_NAME:
		if (is_column(map, e->data.name.name)) {
			gb_putf(b, "%s[i]", e->data.name.name); /* a column read at this element */
		} else {
			b->ok = 0; /* a const / singleton / unknown scalar — not supported in v1 */
		}
		break;
	case HIR_EXPR_LITERAL: {
		const char *lx = e->data.literal.lexeme ? e->data.literal.lexeme : "0";
		gb_putf(b, "%s", lx);
		if (!strchr(lx, '.') && !strchr(lx, 'e') && !strchr(lx, 'E'))
			gb_putf(b, ".0"); /* float-ize an integer literal for a float column */
		break;
	}
	case HIR_EXPR_UNARY:
		gb_putf(b, "(-(");
		emit_expr(b, map, e->data.unary.operand, 0);
		gb_putf(b, "))");
		break;
	case HIR_EXPR_BINARY: {
		Operator op = e->data.binary.op;
		const char *o = binop_glsl(op);
		if (!o || op == OP_MOD || op == OP_AND || op == OP_OR) {
			b->ok = 0;
			break;
		}
		int cmp = op_is_compare(op);
		if (cmp && !want_bool)
			gb_putf(b, "float("); /* comparison used as a 0/1 value */
		gb_putf(b, "(");
		emit_expr(b, map, e->data.binary.left, 0);
		gb_putf(b, " %s ", o);
		emit_expr(b, map, e->data.binary.right, 0);
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
			emit_expr(b, map, e->data.call.args[0], 1); /* condition → bool */
			gb_putf(b, ") ? (");
			emit_expr(b, map, e->data.call.args[1], 0);
			gb_putf(b, ") : (");
			emit_expr(b, map, e->data.call.args[2], 0);
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
static void emit_assign(GBuf *b, HirMapDecl *map, HirAssignStmt *a) {
	if (!a->target || a->target->kind != HIR_EXPR_NAME || !is_column(map, a->target->data.name.name)) {
		b->ok = 0;
		return;
	}
	const char *t = a->target->data.name.name;
	gb_putf(b, "  %s[i] = ", t);
	if (a->op != OP_NONE) {
		const char *o = binop_glsl(a->op);
		if (!o || op_is_compare(a->op) || a->op == OP_MOD) {
			b->ok = 0;
			return;
		}
		gb_putf(b, "%s[i] %s (", t, o);
		emit_expr(b, map, a->value, 0);
		gb_putf(b, ")");
	} else {
		emit_expr(b, map, a->value, 0);
	}
	gb_putf(b, ";\n");
}

static void emit_stmts(GBuf *b, HirMapDecl *map, HirStmt **stmts, int count) {
	for (int i = 0; i < count && b->ok; i++) {
		HirStmt *s = stmts[i];
		if (!s)
			continue;
		if (s->kind == HIR_STMT_ASSIGN)
			emit_assign(b, map, &s->data.assign_stmt);
		else if (s->kind == HIR_STMT_BLOCK)
			emit_stmts(b, map, s->data.block.stmts, s->data.block.count);
		else
			b->ok = 0; /* control flow shouldn't reach here (the map whitelist forbids it), but be safe */
	}
}

/* Does archetype `arch` contain every column the map needs, and are they all float? (v1 constraint.) */
static int arch_matches_float(HirMapDecl *map, HirArchetypeDecl *arch) {
	for (int p = 0; p < map->param_count; p++) {
		const char *pn = map->params[p]->name;
		int found = 0;
		for (int f = 0; f < arch->field_count; f++) {
			if (arch->fields[f]->kind == FIELD_COLUMN && strcmp(arch->fields[f]->name, pn) == 0) {
				if (!arch->fields[f]->type || arch->fields[f]->type->tag != HIR_TYPE_FLOAT)
					return 0; /* v1: float columns only */
				found = 1;
				break;
			}
		}
		if (!found)
			return 0;
	}
	return 1;
}

/* Build the full shader text for (map, arch). Returns a malloc'd string on success, NULL if the body is
 * not GPU-emittable in v1. */
static char *build_shader(HirMapDecl *map, HirArchetypeDecl *arch) {
	GBuf body;
	gb_init(&body);
	emit_stmts(&body, map, map->stmts, map->stmt_count);
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
		gb_putf(&out, "layout(std430, binding = %d) buffer B_%s { float %s[]; };\n", binding++, pn, pn);
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

int arche_gpu_emit(HirProgram *prog, const char *out_dir, int *out_count) {
	if (!prog)
		return 0;
	int written = 0, gpu_maps = 0;

	/* Index archetype decls once. */
	HirArchetypeDecl **archs = calloc(prog->decl_count ? (size_t)prog->decl_count : 1, sizeof(*archs));
	int narch = 0;
	for (int i = 0; i < prog->decl_count; i++)
		if (prog->decls[i] && prog->decls[i]->kind == HIR_DECL_ARCHETYPE && prog->decls[i]->data.archetype)
			archs[narch++] = prog->decls[i]->data.archetype;

	for (int i = 0; i < prog->decl_count; i++) {
		HirDecl *d = prog->decls[i];
		if (!d || d->kind != HIR_DECL_MAP || !d->data.map || !d->data.map->is_gpu)
			continue;
		HirMapDecl *map = d->data.map;
		gpu_maps++;
		int emitted_for_map = 0;
		for (int a = 0; a < narch; a++) {
			if (!arch_matches_float(map, archs[a]))
				continue;
			char *src = build_shader(map, archs[a]);
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
			        "arche: note: `@gpu` map `%s` not GPU-emittable in v1 (needs float columns, "
			        "arithmetic/select only); CPU path unaffected.\n",
			        map->name);
	}
	free(archs);
	if (out_count)
		*out_count = gpu_maps;
	return written;
}
