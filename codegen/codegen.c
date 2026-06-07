#define _POSIX_C_SOURCE 200809L
#include "codegen.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* C99 doesn't expose strdup; declare it explicitly. */
char *strdup(const char *s);

/* ========== DATA STRUCTURES ========== */

typedef struct {
	char *name;
	char *llvm_name;              /* allocated SSA value name */
	int type;                     /* 0=i32, 1=i32*, 2=i8* (string), 3=arch*, 4=column ptr, 5=%struct.arche_array* */
	char *arch_name;              /* for type==3 or 4, nullable otherwise */
	int string_len;               /* for type==2 (string), the compile-time length (-1 if unknown) */
	const char *field_type;       /* for type==4 (column ptr), the Arche type name (e.g. "float") */
	const char *handle_archetype; /* if field_type=="handle", the target archetype name (borrowed, like field_type) */
	int bit_width;                /* 32 (default) or 64 for SSA values */
	int is_slice; /* type==6: 1 = T[] fat-pointer slice (runtime len in len_ssa), 0 = bounded T[N] (len = string_len) */
	char *len_ssa;      /* type==6 slice: SSA value (or literal) holding the i64 runtime length; NULL otherwise */
	char *cap_ssa;      /* type==6 slice: i64 backing capacity (`.cap`); NULL ⇒ fall back to length. Part of the
	                     * heapless {ptr,len,cap} model. TODO: not yet threaded through the call ABI (a slice
	                     * param's cap falls back to its len); revisit per the plan's cap optimization note. */
	char *out_aggr_ptr; /* out-ONLY unbounded `char[]`/`T[]` out-param: the `{T*,i64}*` caller slot (%outN).
	                     * When set, assigning a slice/array to this name stores the {ptr,len} back through
	                     * it so the caller recovers the returned view. NULL for ordinary values. */
} ValueInfo;

typedef struct {
	ValueInfo **values;
	int value_count;
} ValueScope;

typedef struct {
	char sys_name[256];
	char arch_name[256];
	char versioned_name[512];
} SysVersion;

struct CodegenContext {
	HirProgram *ast;
	SemanticContext *sem_ctx;
	int had_error; /* set when codegen hits a hard error (e.g. a `run` no shape can satisfy) */

	/* Per-unit codegen (EXPERIMENTAL, behind ARCHE_PER_UNIT) — a correctness/READINESS mode, NOT a
	 * build-speed mode: it proves codegen can split into one LLVM module per compilation unit (with
	 * mangled/external symbols + linkonce_odr shared defs) that llvm-link merges back losslessly. There
	 * is NO per-object cache (removed as speculative), and the front end always analyzes the whole program
	 * every build, so per-unit gives no incremental-build win. It is also behaviorally- but NOT
	 * bit-equivalent to whole-program: per-unit runs `opt -O2` over each isolated module (no cross-module
	 * IPO), whole-program opts the merged module — same semantics, different binary (an opt-sensitive bug
	 * could surface in only one mode; the suite runs in BOTH). When set, `emit_only_unit` >= 0 restricts
	 * func/proc emission to that unit (systems emit once in unit 0; shared decls — archetype types/helpers,
	 * pool storage — emit in every module as linkonce_odr, gated by the always-on ODR verifier in
	 * compile.c). Default off → the whole-program path is unchanged. See the compilation plan. */
	int per_unit;
	int emit_only_unit; /* -1 = emit all units (whole-program / default) */

	/* For tracking allocated values */
	ValueScope *scopes;
	int scope_count;

	/* SSA counter for generating unique names */
	int value_counter;
	int string_counter;

	/* Buffered output */
	char *output_buffer;
	size_t buffer_size;
	size_t buffer_pos;

	/* Global constants buffer (strings, etc.) */
	char *globals_buffer;
	size_t globals_size;
	size_t globals_pos;

	/* SIMD vectorization context */
	int vector_lanes; /* 0 = scalar mode, 4 = AVX2 double (256-bit / 64-bit = 4 lanes) */
	int in_sys;       /* 1 when generating inside a sys function body */

	/* Implicit loop context */
	char implicit_loop_index[64]; /* SSA reg name for current implicit loop ("" = not in loop) */

	/* Loop exit label stack for break statements */
	char **loop_exit_labels;
	int loop_exit_count;
	int loop_exit_capacity;

	/* Loop continue-target label stack (parallel to loop_exit): the increment latch for a C-style
	 * `for`, or the condition label for an infinite/condition `for`. `continue` branches here. */
	char **loop_cont_labels;
	int loop_cont_count;
	int loop_cont_capacity;

	/* System function version mapping: (sys_name, arch_name) -> versioned_name */
	SysVersion *sys_versions;
	int sys_version_count;
	int sys_version_capacity;

	/* Top-level allocations to initialize in main() */
	HirStaticDecl **top_level_allocs;
	int alloc_count;
	int alloc_capacity;

	/* Static arrays (name -> HirStaticDecl mapping, kind == HIR_STATIC_ARRAY) */
	HirStaticDecl **static_arrays;
	int static_array_count;
	int static_array_capacity;

	/* Scalar globals (name -> HirStaticDecl mapping, kind == HIR_STATIC_SCALAR) */
	HirStaticDecl **scalars;
	int scalar_count;
	int scalar_capacity;

	/* Alloca hoisting: collect allocas during function body gen, emit at entry */
	char *alloca_buffer;
	size_t alloca_buf_size;
	size_t alloca_buf_pos;
	int hoisting_allocas;

	/* Return type of the function currently being generated. For a multi-return func this is
	 * the literal aggregate type (e.g. "{i32, i32}"); current_func gives the member types so a
	 * `return a, b` can build the aggregate. */
	const char *current_return_type;
	HirFuncDecl *current_func; /* the func being generated (for multi-return packing) */
	/* The return-type list of the func OR proc being generated — used for multi-return member
	 * types, so the packing works for procs too (which aren't a HirFuncDecl). */
	HirType **current_return_types;
	int current_return_type_count;
	char current_return_type_buf[512];

	/* Loop bound tracking for bounds-check elision. When a C-style `for`
	 * loop has the shape `for (let v = 0; v < BOUND; v += 1)` with BOUND a
	 * compile-time int, we push (v, BOUND) here. A bounds check on an
	 * archetype column access whose index is `v` can then be elided when
	 * BOUND <= the archetype's static capacity. */
	struct {
		char var_name[64];
		int bound; /* exclusive upper bound */
	} loop_bounds[16];
	int loop_bound_count;

	/* Monomorphization state: set during specialized emission of an
	 * archetype-parametric proc. NULL outside such emission. */
	HirArchetypeDecl *current_archetype_param;
	const char *current_arch_param_name; /* proc parameter name bound to the archetype */
	const char *current_arch_param_llvm; /* LLVM register holding the archetype struct pointer */

	/* Active each_field expansion. NULL when not inside one. */
	const char *current_each_field_binding;
	HirField *current_each_field_target;        /* the archetype field f is bound to for THIS iteration */
	int current_each_field_index;               /* compile-time f.index */
	const char *current_each_field_name_global; /* @.efield_name_NN symbol holding the field's name */

	/* Trailing out-pointer args for a non-extern proc call: set by the proc-call multi-bind path,
	 * appended to the `call void` argument list when the call expression is emitted, then cleared. */
	char *pending_out_ptr_vals[16];
	const char *pending_out_ptr_types[16];
	int pending_out_ptr_count;

	/* Memo of emitted monomorphs: per (proc_name, arch_name) → 1.
	 * Used to avoid double-emitting the same specialization. */
	struct {
		char *proc_name;
		char *arch_name;
	} *mono_emitted;
	int mono_emitted_count;
	int mono_emitted_capacity;

	/* Worklist of monomorphizations queued from inside other functions'
	 * emission. Drained at the end of codegen_generate. */
	struct {
		HirProcDecl *proc;
		HirArchetypeDecl *arch;
	} *mono_pending;
	int mono_pending_count;
	int mono_pending_capacity;

	/* Counter for per-field name globals (@.efield_name_NN). */
	int efield_name_counter;

	/* Set when an actual `@llvm.memcpy` call is emitted (e.g. a non-elided `copy x`). The intrinsic
	 * `declare` is then emitted at module end only if used, so a program with no real copy contains
	 * no `llvm.memcpy` at all. */
	int uses_memcpy;

	/* Compile-time callback monomorphization. A proc with a proc/func-typed
	 * (HIR_TYPE_FUNC) param is callback-parametric: it is never emitted directly,
	 * only specialized per call site where each callback arg is a known proc/func
	 * NAME. The callback param carries no runtime value — inside the specialized
	 * body, a call to the callback param name is rewritten to a direct call to the
	 * bound proc via these active bindings. */
	const char *cb_param_names[8]; /* callback param names bound in the current specialization */
	const char *cb_bound_names[8]; /* the proc/func each is bound to (direct-call target) */
	int cb_binding_count;          /* 0 outside a callback specialization */

	/* Memo of emitted callback specializations, keyed by mangled symbol. */
	char **cb_emitted;
	int cb_emitted_count;
	int cb_emitted_capacity;

	/* RAII auto-drop registry: opaque nominal type name (e.g. "file") -> the (already module-
	 * prefixed) destructor proc symbol (e.g. "io_arche_fclose"). Built once from `@drop` HIR
	 * procs. Borrowed string pointers into the HIR. */
	struct {
		const char *type_name;
		const char *dtor;
	} *drop_reg;
	int drop_reg_count;
	int drop_reg_capacity;

	/* Per-function stack of LIVE droppable opaque locals, in declaration order. At each scope
	 * exit, entries at >= that scope's depth that aren't consumed get a dtor call (reverse order)
	 * and are popped; at function/return exit, ALL live entries are dropped (reverse order).
	 * Consumption (move into an own param / return / insert) flips `consumed`. */
	struct {
		char *var_name;   /* the local's source name (for consumption matching) */
		char *slot;       /* the alloca SSA holding the i64 opaque cell */
		const char *dtor; /* destructor symbol to call */
		int scope_depth;  /* value-scope depth at which it was declared */
		int consumed;     /* 1 once moved out / returned / inserted */
	} *drop_live;
	int drop_live_count;
	int drop_live_capacity;
	/* Set right after a `return` emits its terminator + path-local drops; tells the immediately
	 * following pop_value_scope that the current block is already terminated (its drops were
	 * emitted by the return), so it pops without re-emitting. Cleared when a new live block opens
	 * (e.g. an if's exit label), so the fall-through path still auto-drops at function exit. */
	int block_terminated;

	/* Worklist of callback specializations queued from call sites, drained with
	 * the archetype worklist. Each carries the bound names per callback param. */
	struct {
		HirProcDecl *proc;
		char *mangled;
		const char *bound[8]; /* bound proc/func name per callback param (NULL = non-callback param) */
	} *cb_pending;
	int cb_pending_count;
	int cb_pending_capacity;
};

/* ========== SYSTEM VERSION MAPPING ========== */

static void codegen_register_sys_version(CodegenContext *ctx, const char *sys_name, const char *arch_name) {
	if (ctx->sys_version_count >= ctx->sys_version_capacity) {
		ctx->sys_version_capacity = (ctx->sys_version_capacity == 0) ? 16 : ctx->sys_version_capacity * 2;
		ctx->sys_versions = realloc(ctx->sys_versions, ctx->sys_version_capacity * sizeof(ctx->sys_versions[0]));
	}

	SysVersion *entry = &ctx->sys_versions[ctx->sys_version_count];
	strncpy(entry->sys_name, sys_name, sizeof(entry->sys_name) - 1);
	entry->sys_name[sizeof(entry->sys_name) - 1] = '\0';
	strncpy(entry->arch_name, arch_name, sizeof(entry->arch_name) - 1);
	entry->arch_name[sizeof(entry->arch_name) - 1] = '\0';
	snprintf(entry->versioned_name, sizeof(entry->versioned_name), "%s_%s", sys_name, arch_name);
	ctx->sys_version_count++;
}

/* ========== STATIC ARRAY TRACKING ========== */

static void codegen_register_static_array(CodegenContext *ctx, HirStaticDecl *sa) {
	if (ctx->static_array_count >= ctx->static_array_capacity) {
		ctx->static_array_capacity = (ctx->static_array_capacity == 0) ? 16 : ctx->static_array_capacity * 2;
		ctx->static_arrays = realloc(ctx->static_arrays, ctx->static_array_capacity * sizeof(HirStaticDecl *));
	}
	ctx->static_arrays[ctx->static_array_count++] = sa;
}

static HirStaticDecl *codegen_find_static_array(CodegenContext *ctx, const char *name) {
	for (int i = 0; i < ctx->static_array_count; i++) {
		if (strcmp(ctx->static_arrays[i]->array.name, name) == 0) {
			return ctx->static_arrays[i];
		}
	}
	return NULL;
}

static int char_literal_value(const char *lex); /* defined below */

static void codegen_register_scalar(CodegenContext *ctx, HirStaticDecl *sc) {
	if (ctx->scalar_count >= ctx->scalar_capacity) {
		ctx->scalar_capacity = (ctx->scalar_capacity == 0) ? 16 : ctx->scalar_capacity * 2;
		ctx->scalars = realloc(ctx->scalars, ctx->scalar_capacity * sizeof(HirStaticDecl *));
	}
	ctx->scalars[ctx->scalar_count++] = sc;
}

static HirStaticDecl *codegen_find_scalar(CodegenContext *ctx, const char *name) {
	for (int i = 0; i < ctx->scalar_count; i++)
		if (strcmp(ctx->scalars[i]->scalar.name, name) == 0)
			return ctx->scalars[i];
	return NULL;
}

/* Fold a scalar global's initializer to an LLVM constant string (default "0" for the implicit
 * `= 0`). Semantic guarantees the initializer is compile-time-constant: a literal, a value const
 * (inlined via semantic_get_const_value), or an enum variant. */
static void codegen_scalar_init_const(CodegenContext *ctx, HirExpr *init, char *out, size_t out_sz) {
	if (!init) {
		snprintf(out, out_sz, "0");
		return;
	}
	if (init->kind == HIR_EXPR_LITERAL && init->data.literal.lexeme) {
		const char *lx = init->data.literal.lexeme;
		if (lx[0] == '\'')
			snprintf(out, out_sz, "%d", char_literal_value(lx));
		else
			snprintf(out, out_sz, "%s", lx);
		return;
	}
	if (init->kind == HIR_EXPR_NAME) {
		const char *cv = semantic_get_const_value(ctx->sem_ctx, init->data.name.name);
		if (cv) {
			if (cv[0] == '\'')
				snprintf(out, out_sz, "%d", char_literal_value(cv));
			else
				snprintf(out, out_sz, "%s", cv);
			return;
		}
	}
	snprintf(out, out_sz, "0");
}

/* ========== UTILITY FUNCTIONS ========== */

static void buffer_append(CodegenContext *ctx, const char *str) {
	size_t len = strlen(str);
	if (ctx->buffer_pos + len >= ctx->buffer_size) {
		ctx->buffer_size = (ctx->buffer_size + len) * 2;
		ctx->output_buffer = realloc(ctx->output_buffer, ctx->buffer_size);
	}
	strcpy(ctx->output_buffer + ctx->buffer_pos, str);
	ctx->buffer_pos += len;
}

static void buffer_append_fmt(CodegenContext *ctx, const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);

	char temp[1024];
	vsnprintf(temp, sizeof(temp), fmt, args);
	va_end(args);

	buffer_append(ctx, temp);
}

static void emit_alloca(CodegenContext *ctx, const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	char temp[256];
	vsnprintf(temp, sizeof(temp), fmt, args);
	va_end(args);

	if (ctx->hoisting_allocas) {
		size_t len = strlen(temp);
		if (ctx->alloca_buf_pos + len >= ctx->alloca_buf_size) {
			ctx->alloca_buf_size = (ctx->alloca_buf_size + len + 64) * 2;
			ctx->alloca_buffer = realloc(ctx->alloca_buffer, ctx->alloca_buf_size);
		}
		strcpy(ctx->alloca_buffer + ctx->alloca_buf_pos, temp);
		ctx->alloca_buf_pos += len;
	} else {
		buffer_append(ctx, temp);
	}
}

/* Size in bytes of an LLVM scalar/pointer type name, for column layout + row-stride math.
 * Pointer-aware and width-aware (the old `elem_type[0]=='d' ? 8 : 4` heuristic under-sized i64,
 * i128, and pointers — corrupting columns of those types). */
static int llvm_type_sizeof(const char *t) {
	if (!t || !t[0])
		return 4;
	size_t n = strlen(t);
	if (t[n - 1] == '*')
		return 8; /* any pointer is pointer-width (LP64) */
	if (strcmp(t, "double") == 0 || strcmp(t, "i64") == 0)
		return 8;
	if (strcmp(t, "i128") == 0)
		return 16;
	if (strcmp(t, "i16") == 0)
		return 2;
	if (strcmp(t, "i8") == 0 || strcmp(t, "i1") == 0)
		return 1;
	return 4; /* i32 / float / default */
}

static const char *llvm_type_from_arche(const char *arche_type) {
	if (!arche_type)
		return "i32"; /* default to int */

	if (strcmp(arche_type, "float") == 0)
		return "double";
	if (strcmp(arche_type, "int") == 0)
		return "i32";
	if (strcmp(arche_type, "char") == 0)
		return "i8";
	if (strcmp(arche_type, "void") == 0)
		return "void";
	if (strcmp(arche_type, "handle") == 0)
		return "i64";
	if (strcmp(arche_type, "opaque") == 0)
		return "i64"; /* pointer-width cell; holds a C pointer, never interpreted by Arche */

	/* Fixed-width integer types map to LLVM iN (sign lives in the ops, not
	 * the type): byte/u8/i8 -> i8, u16/i16 -> i16, ... i128/u128 -> i128. */
	{
		int w, sg;
		if (hir_parse_int_width(arche_type, &w, &sg)) {
			switch (w) {
			case 8:
				return "i8";
			case 16:
				return "i16";
			case 32:
				return "i32";
			case 64:
				return "i64";
			case 128:
				return "i128";
			}
		}
	}

	/* For custom types (Vec3, archetypes, etc.), use opaque structures */
	static char buf[256];
	snprintf(buf, sizeof(buf), "%%struct.%s", arche_type);
	return buf;
}

static char *gen_value_name(CodegenContext *ctx);

/* LLVM integer type name for a width (8/16/32/64/128). */
static const char *llvm_int_type(int width) {
	switch (width) {
	case 8:
		return "i8";
	case 16:
		return "i16";
	case 64:
		return "i64";
	case 128:
		return "i128";
	default:
		return "i32";
	}
}

/* Convert integer value `val` (LLVM type from `from`) to width `to_w`, emitting
 * sext/zext/trunc as needed. Writes the resulting SSA name (or `val` unchanged)
 * into `out`. `from` may be NULL (treated as i32 signed). */
static void emit_int_convert(CodegenContext *ctx, const char *val, HirType *from, int to_w, char *out) {
	int from_w = (from && from->tag == HIR_TYPE_INT && from->int_width) ? from->int_width : 32;
	if (from && (from->tag == HIR_TYPE_OPAQUE || from->tag == HIR_TYPE_HANDLE))
		from_w = 64; /* opaque cell / handle are pointer-width i64 */
	int from_signed = (from && from->tag == HIR_TYPE_INT) ? from->int_signed : 1;
	if (to_w == 0)
		to_w = 32;
	if (from_w == to_w) {
		strcpy(out, val);
		return;
	}
	/* An integer literal constant (no SSA '%') is width-agnostic in LLVM — emit
	 * it directly at the target width. This implements literal-adopts-context
	 * and avoids truncating/sign-extending a literal that doesn't fit i32
	 * (e.g. `let x: i64 = 3000000000`). */
	if (val && val[0] != '%') {
		strcpy(out, val);
		return;
	}
	const char *fl = llvm_int_type(from_w);
	const char *tl = llvm_int_type(to_w);
	char *r = gen_value_name(ctx);
	if (to_w > from_w)
		buffer_append_fmt(ctx, "  %s = %s %s %s to %s\n", r, from_signed ? "sext" : "zext", fl, val, tl);
	else
		buffer_append_fmt(ctx, "  %s = trunc %s %s to %s\n", r, fl, val, tl);
	strcpy(out, r);
}

/* Coerce an index value to i64 for getelementptr, respecting the index
 * expression's actual width: i64 passes through, i32/narrower sext/zext, i128
 * truncates. `idx_expr` may be NULL (assumed i32 signed). */
static void emit_index_i64(CodegenContext *ctx, const char *idx_val, const HirExpr *idx_expr, char *out) {
	int w = 32, sgn = 1;
	if (idx_expr && idx_expr->resolved.tag == HIR_TYPE_INT && idx_expr->resolved.int_width) {
		w = idx_expr->resolved.int_width;
		sgn = idx_expr->resolved.int_signed;
	}
	if (w == 64) {
		strcpy(out, idx_val);
		return;
	}
	char *r = gen_value_name(ctx);
	if (w < 64)
		buffer_append_fmt(ctx, "  %s = %s %s %s to i64\n", r, sgn ? "sext" : "zext", llvm_int_type(w), idx_val);
	else
		buffer_append_fmt(ctx, "  %s = trunc i128 %s to i64\n", r, idx_val);
	strcpy(out, r);
}

static const char *llvm_vector_type(const char *scalar_type, int lanes) {
	static char buf[64];
	snprintf(buf, sizeof(buf), "<%d x %s>", lanes, scalar_type);
	return buf;
}

static const char *elem_llvm_type(CodegenContext *ctx, const char *arche_type) {
	const char *scalar = llvm_type_from_arche(arche_type);
	if (ctx->vector_lanes > 0)
		return llvm_vector_type(scalar, ctx->vector_lanes);
	return scalar;
}

/* Canonical Arche type-name string for an integer width/signedness.
 * 32-bit signed stays "int" (back-compat); others use iN/uN. */
static const char *int_width_name(int width, int is_signed) {
	if (width == 32 && is_signed)
		return "int";
	switch (width) {
	case 8:
		return is_signed ? "i8" : "u8";
	case 16:
		return is_signed ? "i16" : "u16";
	case 32:
		return is_signed ? "i32" : "u32";
	case 64:
		return is_signed ? "i64" : "u64";
	case 128:
		return is_signed ? "i128" : "u128";
	default:
		return "int";
	}
}

static const char *field_base_type_name(HirType *type) {
	while (type && (type->tag == HIR_TYPE_SHAPED_ARRAY || type->tag == HIR_TYPE_ARRAY))
		type = type->elem;
	if (!type)
		return "int";
	switch (type->tag) {
	case HIR_TYPE_INT:
		return int_width_name(type->int_width, type->int_signed);
	case HIR_TYPE_FLOAT:
		return "float";
	case HIR_TYPE_CHAR:
		return "char";
	case HIR_TYPE_VOID:
		return "void";
	case HIR_TYPE_HANDLE:
		return "handle";
	case HIR_TYPE_OPAQUE:
		return "opaque";
	case HIR_TYPE_NAMED:
		return type->name ? type->name : "int";
	default:
		return "int";
	}
}

static int field_total_elements(HirType *type) {
	if (type && type->tag == HIR_TYPE_SHAPED_ARRAY)
		return type->rank * field_total_elements(type->elem);
	return 1;
}

/* LLVM type of one return value. char[] returns a raw i8* byte view; everything else maps
 * through the normal type lowering (int→iN, float→double, handle/opaque→i64, …). */
static int proc_out_param_is_inout(HirProcDecl *proc, int oi);
static int proc_out_param_is_inout_in(HirProcDecl *proc, int ii);
static const char *extern_proc_cret(HirProcDecl *proc);

static const char *return_member_llvm(HirType *t) {
	if (t && t->tag == HIR_TYPE_ARRAY) {
		/* A `T[]` slice (any element, incl. char) is returned as a fat pointer `{T*, i64}` (the same
		 * (ptr,len) it was threaded in as), so the caller recovers both the data pointer and the
		 * runtime length after a `move`-and-return. */
		const char *e = llvm_type_from_arche(field_base_type_name(t));
		if (strcmp(e, "double") == 0)
			return "{ double*, i64 }";
		if (strcmp(e, "i64") == 0)
			return "{ i64*, i64 }";
		if (strcmp(e, "i16") == 0)
			return "{ i16*, i64 }";
		if (strcmp(e, "i8") == 0)
			return "{ i8*, i64 }";
		return "{ i32*, i64 }";
	}
	if (t && (t->tag == HIR_TYPE_ARRAY || t->tag == HIR_TYPE_SHAPED_ARRAY)) {
		/* An array is returned as an element pointer (by reference) — char[] → i8* (unchanged),
		 * int[] → i32*, etc. The callee returns an `own`/borrowed param's buffer; the caller binds
		 * the result as a bounded type-6 array. (Returning a fresh local is rejected in semantic.) */
		const char *e = llvm_type_from_arche(field_base_type_name(t));
		if (strcmp(e, "i8") == 0)
			return "i8*";
		if (strcmp(e, "i16") == 0)
			return "i16*";
		if (strcmp(e, "i32") == 0)
			return "i32*";
		if (strcmp(e, "i64") == 0)
			return "i64*";
		if (strcmp(e, "double") == 0)
			return "double*";
		return "i8*"; /* fallback */
	}
	return llvm_type_from_arche(field_base_type_name(t));
}

/* The LLVM type of an array crossing the FFI (C ABI) boundary: a BARE element pointer (char[] →
 * i8*, int[] → i32*, …), never the fat-pointer `{T*, i64}` that the internal slice ABI uses. C
 * functions take/return a raw pointer; the runtime byte length is carried separately. */
static const char *extern_member_llvm(HirType *t) {
	if (t && (t->tag == HIR_TYPE_ARRAY || t->tag == HIR_TYPE_SHAPED_ARRAY)) {
		const char *e = llvm_type_from_arche(field_base_type_name(t));
		if (strcmp(e, "i16") == 0)
			return "i16*";
		if (strcmp(e, "i32") == 0)
			return "i32*";
		if (strcmp(e, "i64") == 0)
			return "i64*";
		if (strcmp(e, "double") == 0)
			return "double*";
		return "i8*";
	}
	return llvm_type_from_arche(field_base_type_name(t));
}

/* The LLVM integer type to compare a truthiness condition against 0 (`if`/`for` test). Conditions
 * are arche ints of various widths — `bool` (i8), `int` (i32), an opaque/handle cell (i64), or any
 * iN — so the `icmp ne <T>, 0` MUST use the value's real width (a hardcoded i32 mis-types an i8
 * `bool` condition). Defaults to i32 for anything non-integer (a type error elsewhere). */
static const char *cond_int_type(const HirExpr *cond) {
	if (!cond)
		return "i32";
	if (cond->resolved.tag == HIR_TYPE_OPAQUE || cond->resolved.tag == HIR_TYPE_HANDLE)
		return "i64"; /* non-null cell test */
	const char *a = field_base_type_name((HirType *)&cond->resolved);
	if (a && strcmp(a, "bool") == 0)
		return "i8";
	const char *t = llvm_type_from_arche(a);
	if (strcmp(t, "i1") == 0 || strcmp(t, "i8") == 0 || strcmp(t, "i16") == 0 || strcmp(t, "i32") == 0 ||
	    strcmp(t, "i64") == 0 || strcmp(t, "i128") == 0)
		return t;
	return "i32";
}

/* Fill `out` with the LLVM type for a return-type list: `void` for count 0, the single type for
 * count 1, or a literal aggregate `{T1, T2, …}` for a multi-value return. Shared by funcs and procs
 * (both now carry a `return_types`/`return_type_count` list). */
static void llvm_return_list_type(HirType **return_types, int count, char *out, size_t cap) {
	if (count == 0) {
		snprintf(out, cap, "void");
		return;
	}
	if (count == 1) {
		snprintf(out, cap, "%s", return_member_llvm(return_types[0]));
		return;
	}
	size_t n = 0;
	n += (size_t)snprintf(out + n, cap - n, "{ ");
	for (int i = 0; i < count; i++)
		n += (size_t)snprintf(out + n, cap - n, "%s%s", i ? ", " : "", return_member_llvm(return_types[i]));
	snprintf(out + n, cap - n, " }");
}

static void func_llvm_return_type(HirFuncDecl *f, char *out, size_t cap) {
	llvm_return_list_type(f->return_types, f->return_type_count, out, cap);
}

/* Decode a char-literal lexeme ('a', '\n', …) to its integer code, for emission as an LLVM
 * value (LLVM has no char-literal token — a char is just its integer value). */
static int char_literal_value(const char *lex) {
	if (lex[1] == '\\') {
		switch (lex[2]) {
		case 'n':
			return '\n';
		case 't':
			return '\t';
		case 'r':
			return '\r';
		case '\\':
			return '\\';
		case '\'':
			return '\'';
		case '0':
			return '\0';
		default:
			return lex[2];
		}
	}
	return lex[1];
}

static const char *hir_resolved_type_name(const HirExpr *expr) {
	if (!expr)
		return "int";
	switch (expr->resolved.tag) {
	case HIR_TYPE_INT:
		return int_width_name(expr->resolved.int_width, expr->resolved.int_signed);
	case HIR_TYPE_FLOAT:
		return "float";
	case HIR_TYPE_CHAR:
		return "char";
	case HIR_TYPE_VOID:
		return "void";
	case HIR_TYPE_HANDLE:
		return "handle";
	case HIR_TYPE_OPAQUE:
		return "opaque";
	case HIR_TYPE_NAMED:
		return expr->resolved.name ? expr->resolved.name : "int";
	default:
		return "int";
	}
}

static char *gen_value_name(CodegenContext *ctx) {
	char *name = malloc(32);
	snprintf(name, 32, "%%v%d", ctx->value_counter++);
	return name;
}

/* Emit a string constant global and return its name. `with_nul` appends a trailing `\00` — used
 * ONLY at the FFI boundary (a literal handed to a C function that wants a NUL-terminated `char*`:
 * printf/sprintf, extern char* params). Internally a literal is a bare `[N x i8]` array; its length
 * travels with the value, never via a NUL. */
static char *emit_string_global_impl(CodegenContext *ctx, const char *quoted_str, int with_nul, size_t *out_len) {
	char global_name[64];
	snprintf(global_name, sizeof(global_name), "@.str%d", ctx->string_counter++);

	/* quoted_str is "..." with quotes. Process escape sequences and build LLVM constant */
	char escaped[2048];
	size_t escaped_pos = 0;
	size_t str_len = strlen(quoted_str);

	for (size_t i = 1; i < str_len - 1 && escaped_pos < sizeof(escaped) - 10; i++) {
		char c = quoted_str[i];
		if (c == '\\' && i + 1 < str_len - 1) {
			i++;
			switch (quoted_str[i]) {
			case 'n':
				escaped[escaped_pos++] = '\n';
				break;
			case 't':
				escaped[escaped_pos++] = '\t';
				break;
			case 'r':
				escaped[escaped_pos++] = '\r';
				break;
			case '\\':
				escaped[escaped_pos++] = '\\';
				break;
			case '"':
				escaped[escaped_pos++] = '"';
				break;
			default:
				escaped[escaped_pos++] = quoted_str[i];
				break;
			}
		} else {
			escaped[escaped_pos++] = c;
		}
	}

	/* Build LLVM global constant declaration */
	char global_decl[4096];
	char llvm_escaped[2048] = "";
	size_t llvm_pos = 0;

	for (size_t i = 0; i < escaped_pos && llvm_pos < sizeof(llvm_escaped) - 10; i++) {
		unsigned char c = (unsigned char)escaped[i];
		if (c >= 32 && c < 127 && c != '"' && c != '\\') {
			llvm_escaped[llvm_pos++] = c;
		} else {
			llvm_pos += snprintf(llvm_escaped + llvm_pos, sizeof(llvm_escaped) - llvm_pos, "\\%02X", c);
		}
	}

	if (with_nul)
		snprintf(global_decl, sizeof(global_decl), "%s = private unnamed_addr constant [%zu x i8] c\"%s\\00\"\n",
		         global_name, escaped_pos + 1, llvm_escaped);
	else
		snprintf(global_decl, sizeof(global_decl), "%s = private unnamed_addr constant [%zu x i8] c\"%s\"\n",
		         global_name, escaped_pos, llvm_escaped);

	/* Append to globals buffer */
	size_t decl_len = strlen(global_decl);
	if (ctx->globals_pos + decl_len >= ctx->globals_size) {
		ctx->globals_size = (ctx->globals_size + decl_len) * 2;
		ctx->globals_buffer = realloc(ctx->globals_buffer, ctx->globals_size);
	}
	strcpy(ctx->globals_buffer + ctx->globals_pos, global_decl);
	ctx->globals_pos += decl_len;

	/* Return allocated name */
	if (out_len)
		*out_len = escaped_pos;
	char *ret = malloc(64);
	strcpy(ret, global_name);
	return ret;
}

/* String-literal global: NUL-terminated `[N+1 x i8]`. The trailing NUL is the FFI/C-string
 * provision — it sits PAST the content length (`.length` is N, never counts it), so the array is
 * "normal": indexing is bounded to N, the length travels with the value, and the NUL is only ever
 * consumed at the C boundary (printf/sprintf %s, extern char*, file paths) or copied wholesale by
 * `insert` into a (zero-padded) column. A literal carries its own NUL, so no runtime materialization
 * is needed and byte-buffer FFI params (net_send/recv) are untouched. */
static char *emit_string_global(CodegenContext *ctx, const char *quoted_str) {
	return emit_string_global_impl(ctx, quoted_str, 1, NULL);
}

static void push_value_scope(CodegenContext *ctx) {
	ctx->scopes = realloc(ctx->scopes, (ctx->scope_count + 1) * sizeof(ValueScope));
	ctx->scopes[ctx->scope_count].values = NULL;
	ctx->scopes[ctx->scope_count].value_count = 0;
	ctx->scope_count++;
}

static void drop_exit_scope(CodegenContext *ctx, int depth);
static void drop_pop_scope_only(CodegenContext *ctx, int depth);

static void pop_value_scope(CodegenContext *ctx) {
	if (ctx->scope_count > 0) {
		/* RAII: auto-drop live opaque locals declared at this scope depth (reverse order,
		 * skipping consumed). Skipped when the current block was just terminated by a `return`
		 * (it already emitted those drops on its path) — but the entries are still popped. */
		if (ctx->block_terminated)
			drop_pop_scope_only(ctx, ctx->scope_count);
		else
			drop_exit_scope(ctx, ctx->scope_count);
		ValueScope *scope = &ctx->scopes[ctx->scope_count - 1];
		for (int i = 0; i < scope->value_count; i++) {
			free(scope->values[i]->name);
			free(scope->values[i]->llvm_name);
			free(scope->values[i]->arch_name);
			free(scope->values[i]);
		}
		free(scope->values);
		ctx->scope_count--;
	}
}

/* ===== RAII auto-drop support ===== */

/* The registered destructor symbol for opaque type `type_name`, or NULL. */
static const char *drop_dtor_for_type(CodegenContext *ctx, const char *type_name) {
	if (!type_name)
		return NULL;
	for (int i = 0; i < ctx->drop_reg_count; i++)
		if (strcmp(ctx->drop_reg[i].type_name, type_name) == 0)
			return ctx->drop_reg[i].dtor;
	return NULL;
}

/* Record a live droppable opaque local at the current scope depth. `slot` is the alloca SSA
 * holding the i64 cell; `dtor` the destructor symbol. */
static void drop_track(CodegenContext *ctx, const char *var_name, const char *slot, const char *dtor) {
	if (ctx->drop_live_count >= ctx->drop_live_capacity) {
		ctx->drop_live_capacity = ctx->drop_live_capacity ? ctx->drop_live_capacity * 2 : 8;
		ctx->drop_live = realloc(ctx->drop_live, ctx->drop_live_capacity * sizeof(*ctx->drop_live));
	}
	int k = ctx->drop_live_count++;
	ctx->drop_live[k].var_name = strdup(var_name);
	ctx->drop_live[k].slot = strdup(slot);
	ctx->drop_live[k].dtor = dtor;
	ctx->drop_live[k].scope_depth = ctx->scope_count;
	ctx->drop_live[k].consumed = 0;
}

/* Mark a tracked droppable local consumed (moved out / returned / inserted) — suppresses its
 * auto-drop. Matches the innermost live entry of that name. No-op if not tracked. */
static void drop_mark_consumed(CodegenContext *ctx, const char *var_name) {
	for (int i = ctx->drop_live_count - 1; i >= 0; i--) {
		if (!ctx->drop_live[i].consumed && strcmp(ctx->drop_live[i].var_name, var_name) == 0) {
			ctx->drop_live[i].consumed = 1;
			return;
		}
	}
}

/* Emit a destructor call for one live entry: load the i64 cell and call dtor(cell). */
static void drop_emit_one(CodegenContext *ctx, int idx);

/* Drop & pop all live entries declared at value-scope depth >= `depth`, in reverse declaration
 * order, skipping consumed ones. Called from pop_value_scope (depth = the scope being popped). */
static void drop_exit_scope(CodegenContext *ctx, int depth) {
	for (int i = ctx->drop_live_count - 1; i >= 0; i--) {
		if (ctx->drop_live[i].scope_depth < depth)
			break; /* entries are pushed in order; lower depths sit below */
		if (!ctx->drop_live[i].consumed)
			drop_emit_one(ctx, i);
	}
	/* Pop entries at >= depth. */
	while (ctx->drop_live_count > 0 && ctx->drop_live[ctx->drop_live_count - 1].scope_depth >= depth) {
		ctx->drop_live_count--;
		free(ctx->drop_live[ctx->drop_live_count].var_name);
		free(ctx->drop_live[ctx->drop_live_count].slot);
	}
}

/* Pop entries at >= depth WITHOUT emitting drops (the terminating `return` already emitted them
 * on its path). */
static void drop_pop_scope_only(CodegenContext *ctx, int depth) {
	while (ctx->drop_live_count > 0 && ctx->drop_live[ctx->drop_live_count - 1].scope_depth >= depth) {
		ctx->drop_live_count--;
		free(ctx->drop_live[ctx->drop_live_count].var_name);
		free(ctx->drop_live[ctx->drop_live_count].slot);
	}
}

/* Drop ALL still-live entries (reverse order) for a `return` exit — PATH-LOCAL: it does NOT
 * mark them consumed (they remain live on the other control-flow paths, which auto-drop at their
 * own exits). Sets block_terminated so the immediately-following pop_value_scope (if this return
 * is the last statement) pops without emitting a second, dead drop. */
static void drop_exit_all_for_return(CodegenContext *ctx) {
	for (int i = ctx->drop_live_count - 1; i >= 0; i--)
		if (!ctx->drop_live[i].consumed)
			drop_emit_one(ctx, i);
	ctx->block_terminated = 1;
}

/* Per-unit symbol/linkage helpers (defined below, near monomorph_mangle). */
static const char *cg_fnsym(CodegenContext *ctx, const char *name, int is_extern, char *buf, size_t n);
/* Archetypes covering a system's params (system ABI param list) — defined near emit_cross_unit_declares. */
static int collect_sys_matching_archs(CodegenContext *ctx, HirSysDecl *sys, const char **out, int max);

/* Is the proc/func named `name` an extern (#foreign, C-ABI)? A `@drop` destructor may be either an
 * arche proc (mangled under per-unit) or an extern (keeps its C name) — the dtor call must match. */
static int decl_name_is_extern(CodegenContext *ctx, const char *name) {
	if (!name)
		return 0;
	for (int i = 0; i < ctx->ast->decl_count; i++) {
		HirDecl *d = ctx->ast->decls[i];
		if (d->kind == HIR_DECL_PROC && d->data.proc->name && strcmp(d->data.proc->name, name) == 0)
			return d->data.proc->is_extern;
		if (d->kind == HIR_DECL_FUNC && d->data.func->name && strcmp(d->data.func->name, name) == 0)
			return d->data.func->is_extern;
	}
	return 0;
}

static void drop_emit_one(CodegenContext *ctx, int idx) {
	char *cell = gen_value_name(ctx);
	buffer_append_fmt(ctx, "  %s = load i64, i64* %s\n", cell, ctx->drop_live[idx].slot);
	char dtor_sym[512];
	const char *dtor = ctx->drop_live[idx].dtor;
	buffer_append_fmt(ctx, "  call void @%s(i64 %s)\n",
	                  cg_fnsym(ctx, dtor, decl_name_is_extern(ctx, dtor), dtor_sym, sizeof(dtor_sym)), cell);
}

static ValueInfo *find_value(CodegenContext *ctx, const char *name) {
	for (int i = ctx->scope_count - 1; i >= 0; i--) {
		ValueScope *scope = &ctx->scopes[i];
		for (int j = scope->value_count - 1; j >= 0; j--) {
			if (strcmp(scope->values[j]->name, name) == 0) {
				return scope->values[j];
			}
		}
	}
	return NULL;
}

static void add_value(CodegenContext *ctx, const char *name, const char *llvm_name, int type) {
	if (ctx->scope_count == 0)
		return;

	ValueScope *scope = &ctx->scopes[ctx->scope_count - 1];
	ValueInfo *val = calloc(1, sizeof(ValueInfo));
	val->name = malloc(strlen(name) + 1);
	strcpy(val->name, name);
	val->llvm_name = malloc(strlen(llvm_name) + 1);
	strcpy(val->llvm_name, llvm_name);
	val->type = type;
	val->arch_name = NULL;
	val->string_len = -1;
	val->field_type = NULL;
	val->bit_width = 32;

	scope->values = realloc(scope->values, (scope->value_count + 1) * sizeof(ValueInfo *));
	scope->values[scope->value_count++] = val;
}

static void add_arch_value(CodegenContext *ctx, const char *name, const char *llvm_name, const char *arch_name) {
	if (ctx->scope_count == 0)
		return;

	ValueScope *scope = &ctx->scopes[ctx->scope_count - 1];
	ValueInfo *val = calloc(1, sizeof(ValueInfo));
	val->name = malloc(strlen(name) + 1);
	strcpy(val->name, name);
	val->llvm_name = malloc(strlen(llvm_name) + 1);
	strcpy(val->llvm_name, llvm_name);
	val->type = 3; /* arch pointer */
	val->arch_name = malloc(strlen(arch_name) + 1);
	strcpy(val->arch_name, arch_name);
	val->string_len = -1;
	val->field_type = NULL;
	val->bit_width = 32;

	scope->values = realloc(scope->values, (scope->value_count + 1) * sizeof(ValueInfo *));
	scope->values[scope->value_count++] = val;
}

static void register_static_arrays_in_scope(CodegenContext *ctx) {
	/* Register each char static array as a type-6 bounded array (element pointer + count N) — a
	 * normal array, like a local char[N], so indexing / .length / slicing all go the type-6 path. */
	if (ctx->scope_count == 0)
		return;

	for (int i = 0; i < ctx->static_array_count; i++) {
		HirStaticDecl *sa = ctx->static_arrays[i];
		if (sa && sa->array.element_type && sa->array.element_type->tag == HIR_TYPE_CHAR) {
			ValueScope *scope = &ctx->scopes[ctx->scope_count - 1];
			/* GEP the `[N x i8]` global down to its first element (an i8*). */
			char *elem0 = gen_value_name(ctx);
			buffer_append_fmt(ctx, "  %s = getelementptr [%d x i8], [%d x i8]* @%s, i64 0, i64 0\n", elem0,
			                  sa->array.size, sa->array.size, sa->array.name);
			ValueInfo *vi = calloc(1, sizeof(ValueInfo));
			vi->name = malloc(strlen(sa->array.name) + 1);
			strcpy(vi->name, sa->array.name);
			vi->llvm_name = malloc(strlen(elem0) + 1);
			strcpy(vi->llvm_name, elem0);
			vi->type = 6; /* element pointer; field_type drives element typing */
			vi->arch_name = NULL;
			vi->string_len = sa->array.size;
			vi->field_type = "char"; /* borrowed literal, like every other field_type assignment */
			vi->bit_width = 8;
			scope->values = realloc(scope->values, (scope->value_count + 1) * sizeof(ValueInfo *));
			scope->values[scope->value_count++] = vi;
		}
	}
}

/* Two archetype decls denote the same shape iff they have the same *set* of component
 * names (order-independent — an archetype is an unordered set of component types). */
static int archetypes_same_shape(HirArchetypeDecl *a, HirArchetypeDecl *b) {
	if (a == b)
		return 1;
	if (!a || !b || a->field_count != b->field_count)
		return 0;
	for (int i = 0; i < a->field_count; i++) {
		int found = 0;
		for (int j = 0; j < b->field_count; j++) {
			if (a->fields[i]->name && b->fields[j]->name && strcmp(a->fields[i]->name, b->fields[j]->name) == 0) {
				found = 1;
				break;
			}
		}
		if (!found)
			return 0;
	}
	return 1;
}

/* One shape = one pool. An archetype name is an *alias* for its shape; every alias resolves
 * to the same storage. The canonical decl is the first-declared archetype of that shape —
 * its name backs the single `%struct.` / `@archetype_` / `@` symbols and its field order is
 * the shape's column layout. */
static HirArchetypeDecl *canonical_archetype_decl(CodegenContext *ctx, HirArchetypeDecl *arch) {
	if (!arch)
		return NULL;
	for (int i = 0; i < ctx->ast->decl_count; i++) {
		HirDecl *decl = ctx->ast->decls[i];
		if (decl->kind == HIR_DECL_ARCHETYPE && archetypes_same_shape(decl->data.archetype, arch))
			return decl->data.archetype;
	}
	return arch;
}

/* Find an archetype declaration by name, resolving aliases to the shape's canonical decl. */
static HirArchetypeDecl *find_archetype_decl(CodegenContext *ctx, const char *name) {
	for (int i = 0; i < ctx->ast->decl_count; i++) {
		HirDecl *decl = ctx->ast->decls[i];
		if (decl->kind == HIR_DECL_ARCHETYPE && strcmp(decl->data.archetype->name, name) == 0) {
			return canonical_archetype_decl(ctx, decl->data.archetype);
		}
	}
	return NULL;
}

/* The canonical shape name for an archetype name (used to form storage symbols). Returns the
 * input unchanged if no archetype by that name exists. */
static const char *canonical_arch_name(CodegenContext *ctx, const char *name) {
	HirArchetypeDecl *c = find_archetype_decl(ctx, name);
	return c ? c->name : name;
}

/* Return compile-time capacity for arch from static declaration, or 0 if not declared static */
static int get_arch_static_capacity(CodegenContext *ctx, const char *arch_name) {
	for (int i = 0; i < ctx->ast->decl_count; i++) {
		if (ctx->ast->decls[i]->kind == HIR_DECL_STATIC) {
			HirStaticDecl *s = ctx->ast->decls[i]->data.static_decl;
			/* A datasheet requirement is a minimum, not an allocation — it emits no storage; the
			 * driver's own pool for the shape carries the real capacity. Skip requirements. */
			if (s->is_requirement)
				continue;
			if (s->kind == HIR_STATIC_ARCHETYPE &&
			    strcmp(canonical_arch_name(ctx, s->archetype.archetype_name), canonical_arch_name(ctx, arch_name)) ==
			        0 &&
			    s->archetype.field_count > 0 && s->archetype.field_values[0]->kind == HIR_EXPR_LITERAL) {
				return atoi(s->archetype.field_values[0]->data.literal.lexeme);
			}
		}
	}
	return 0;
}

/* Compile-time initialized count for a static archetype. Mirrors the
 * count computation in codegen_alloc_expr:
 *   - explicit `static T(cap, N)` second arg → N
 *   - `static T(cap) { field: val, ... }` (init block present) → cap
 *   - `static T(cap)` with no init block → 0
 * Returns -1 if the count is not statically knowable (e.g. non-literal
 * init_length). Callers MUST treat -1 as "cannot elide bounds checks". */
static int get_arch_static_count(CodegenContext *ctx, const char *arch_name) {
	for (int i = 0; i < ctx->ast->decl_count; i++) {
		if (ctx->ast->decls[i]->kind != HIR_DECL_STATIC)
			continue;
		HirStaticDecl *s = ctx->ast->decls[i]->data.static_decl;
		if (s->kind != HIR_STATIC_ARCHETYPE)
			continue;
		if (s->is_requirement) /* datasheet minimum, not an allocation — emits no storage */
			continue;
		if (strcmp(canonical_arch_name(ctx, s->archetype.archetype_name), canonical_arch_name(ctx, arch_name)) != 0)
			continue;
		if (s->archetype.field_count == 0)
			return 0;
		if (s->archetype.init_length) {
			if (s->archetype.init_length->kind != HIR_EXPR_LITERAL)
				return -1;
			return atoi(s->archetype.init_length->data.literal.lexeme);
		}
		if (s->archetype.field_count > 1) {
			/* Init block present — count = capacity. */
			if (s->archetype.field_values[0]->kind != HIR_EXPR_LITERAL)
				return -1;
			return atoi(s->archetype.field_values[0]->data.literal.lexeme);
		}
		return 0;
	}
	return 0;
}

static HirSysDecl *find_sys_decl(CodegenContext *ctx, const char *name) {
	for (int i = 0; i < ctx->ast->decl_count; i++) {
		HirDecl *decl = ctx->ast->decls[i];
		if (decl->kind == HIR_DECL_SYS && strcmp(decl->data.sys->name, name) == 0) {
			return decl->data.sys;
		}
	}
	return NULL;
}

static HirProcDecl *find_proc_decl(CodegenContext *ctx, const char *name) {
	if (!name)
		return NULL;
	for (int i = 0; i < ctx->ast->decl_count; i++) {
		HirDecl *decl = ctx->ast->decls[i];
		if (decl->kind == HIR_DECL_PROC && decl->data.proc->name && strcmp(decl->data.proc->name, name) == 0)
			return decl->data.proc;
	}
	return NULL;
}

static HirFuncDecl *find_func_decl(CodegenContext *ctx, const char *name) {
	for (int i = 0; i < ctx->ast->decl_count; i++) {
		HirDecl *decl = ctx->ast->decls[i];
		if (decl->kind == HIR_DECL_FUNC && strcmp(decl->data.func->name, name) == 0)
			return decl->data.func;
	}
	return NULL;
}

static HirFuncGroupDecl *find_func_group(CodegenContext *ctx, const char *name) {
	for (int i = 0; i < ctx->ast->decl_count; i++) {
		HirDecl *decl = ctx->ast->decls[i];
		if (decl->kind == HIR_DECL_FUNC_GROUP && strcmp(decl->data.func_group->name, name) == 0)
			return decl->data.func_group;
	}
	return NULL;
}

/* True if `proc` has a parameter of bare-category `archetype` type. Such procs
 * are NOT emitted as-is — they must be monomorphized per archetype passed at
 * each call site. */
static int is_archetype_parametric(HirProcDecl *proc) {
	if (!proc)
		return 0;
	for (int i = 0; i < proc->param_count; i++) {
		if (proc->params[i]->type && proc->params[i]->type->tag == HIR_TYPE_ARCHETYPE)
			return 1;
	}
	return 0;
}

/* Index of the first archetype-typed parameter, or -1. */
static int archetype_param_index(HirProcDecl *proc) {
	if (!proc)
		return -1;
	for (int i = 0; i < proc->param_count; i++) {
		if (proc->params[i]->type && proc->params[i]->type->tag == HIR_TYPE_ARCHETYPE)
			return i;
	}
	return -1;
}

/* A proc/func-typed parameter is a compile-time callback: it carries no runtime
 * value, only a known proc/func bound per call site (monomorphized). The type is
 * either an inline proc/func signature (HIR_TYPE_FUNC) or a named alias to one
 * (`done_handler :: proc()()`), resolved through the semantic registry. */
static int is_callback_param(CodegenContext *ctx, HirParam *p) {
	if (!p || !p->type)
		return 0;
	if (p->type->tag == HIR_TYPE_FUNC)
		return 1;
	if (p->type->tag == HIR_TYPE_NAMED && ctx->sem_ctx && p->type->name &&
	    semantic_callable_type_alias(ctx->sem_ctx, p->type->name))
		return 1;
	return 0;
}

/* A proc is callback-parametric if it takes any callback param. Such procs are
 * never emitted directly — only specialized per call site. */
static int has_callback_param(CodegenContext *ctx, HirProcDecl *proc) {
	if (!proc)
		return 0;
	for (int i = 0; i < proc->param_count; i++)
		if (is_callback_param(ctx, proc->params[i]))
			return 1;
	return 0;
}

/* Inside a callback specialization, resolve a callee name that refers to a
 * callback param to its bound proc/func (a direct-call target). Returns `name`
 * unchanged when not an active callback binding. */
static const char *cb_resolve(CodegenContext *ctx, const char *name) {
	if (!name)
		return name;
	for (int i = 0; i < ctx->cb_binding_count; i++)
		if (strcmp(ctx->cb_param_names[i], name) == 0)
			return ctx->cb_bound_names[i];
	return name;
}

/* Emit a comma-separated `type val` argument list, skipping any slot flagged in
 * `skip` (callback args, which have no runtime value). Returns the number of
 * args actually emitted, so callers can get trailing separators right. */
static int emit_call_arglist(CodegenContext *ctx, int count, const int *skip, const char *const *types,
                             char *const *vals, char *const *slice_len) {
	int emitted = 0;
	for (int i = 0; i < count; i++) {
		if (skip && skip[i])
			continue;
		if (emitted > 0)
			buffer_append(ctx, ", ");
		buffer_append_fmt(ctx, "%s %s", types[i], vals[i]);
		emitted++;
		/* A `T[]` slice arg is two positional args: the element pointer (above) then its i64 len. */
		if (slice_len && slice_len[i]) {
			buffer_append_fmt(ctx, ", i64 %s", slice_len[i]);
			emitted++;
		}
	}
	return emitted;
}

/* Count COLUMN-kind primitive fields on an archetype (skips handle columns,
 * meta fields, tuples, etc. — what v1 each_field walks). */
static int count_archetype_iterable_fields(HirArchetypeDecl *arch) {
	if (!arch)
		return 0;
	int n = 0;
	for (int i = 0; i < arch->field_count; i++) {
		HirField *f = arch->fields[i];
		if (!f || f->kind != FIELD_COLUMN)
			continue;
		HirType *t = f->type;
		if (!t)
			continue;
		HirTypeTag tag = t->tag;
		if (tag == HIR_TYPE_INT || tag == HIR_TYPE_FLOAT || tag == HIR_TYPE_CHAR)
			n++;
	}
	return n;
}

static int monomorph_already_emitted(CodegenContext *ctx, const char *proc_name, const char *arch_name) {
	for (int i = 0; i < ctx->mono_emitted_count; i++) {
		if (strcmp(ctx->mono_emitted[i].proc_name, proc_name) == 0 &&
		    strcmp(ctx->mono_emitted[i].arch_name, arch_name) == 0)
			return 1;
	}
	return 0;
}

static void monomorph_mark_emitted(CodegenContext *ctx, const char *proc_name, const char *arch_name) {
	if (ctx->mono_emitted_count >= ctx->mono_emitted_capacity) {
		ctx->mono_emitted_capacity = ctx->mono_emitted_capacity == 0 ? 8 : ctx->mono_emitted_capacity * 2;
		ctx->mono_emitted = realloc(ctx->mono_emitted, ctx->mono_emitted_capacity * sizeof(*ctx->mono_emitted));
	}
	ctx->mono_emitted[ctx->mono_emitted_count].proc_name = malloc(strlen(proc_name) + 1);
	strcpy(ctx->mono_emitted[ctx->mono_emitted_count].proc_name, proc_name);
	ctx->mono_emitted[ctx->mono_emitted_count].arch_name = malloc(strlen(arch_name) + 1);
	strcpy(ctx->mono_emitted[ctx->mono_emitted_count].arch_name, arch_name);
	ctx->mono_emitted_count++;
}

/* Build the mangled symbol name for a monomorphized proc.
 * `out` should be at least 256 bytes. */
static void monomorph_mangle(const char *proc_name, const char *arch_name, char *out, size_t out_sz) {
	snprintf(out, out_sz, "__%s_%s", proc_name, arch_name);
}

/* Per-unit symbol name for an arche-owned function/proc/sys. Under per-unit codegen these get EXTERNAL
 * linkage (cross-object references), so their names must not collide with libc — a dot-bearing prefix
 * is C-incompatible (no C symbol contains a `.`), making collisions impossible and retiring the
 * `internal`-everything workaround. Externs keep their C-ABI name; `main`/`main_user` stay bare (the
 * entry). Inert (returns `name` unchanged) when per_unit is off, so the whole-program path is unaffected. */
static const char *cg_fnsym(CodegenContext *ctx, const char *name, int is_extern, char *buf, size_t n) {
	if (!ctx->per_unit || !name || is_extern || strcmp(name, "main") == 0 || strcmp(name, "main_user") == 0)
		return name;
	snprintf(buf, n, "arche.%s", name);
	return buf;
}
/* Linkage keyword for an arche-owned def: external under per-unit (cross-object), else internal. */
static const char *cg_linkage(CodegenContext *ctx) {
	return ctx->per_unit ? "" : "internal ";
}
/* Linkage keyword for a SHARED definition (global storage, archetype helpers, monomorph instances) that
 * each per-unit module emits identically: `linkonce_odr` lets the linker fold the duplicate definitions
 * to one (ODR holds — same codegen over the same program). Empty (plain external/global) when off. */
static const char *cg_shared(CodegenContext *ctx) {
	return ctx->per_unit ? "linkonce_odr " : "";
}

/* Queue a (proc, arch) pair for emission later, if not already pending or
 * emitted. Returns 1 if newly queued, 0 if already known. */
static int monomorph_enqueue(CodegenContext *ctx, HirProcDecl *proc, HirArchetypeDecl *arch) {
	if (monomorph_already_emitted(ctx, proc->name, arch->name))
		return 0;
	for (int i = 0; i < ctx->mono_pending_count; i++) {
		if (ctx->mono_pending[i].proc == proc && ctx->mono_pending[i].arch == arch)
			return 0;
	}
	if (ctx->mono_pending_count >= ctx->mono_pending_capacity) {
		ctx->mono_pending_capacity = ctx->mono_pending_capacity == 0 ? 8 : ctx->mono_pending_capacity * 2;
		ctx->mono_pending = realloc(ctx->mono_pending, ctx->mono_pending_capacity * sizeof(*ctx->mono_pending));
	}
	ctx->mono_pending[ctx->mono_pending_count].proc = proc;
	ctx->mono_pending[ctx->mono_pending_count].arch = arch;
	ctx->mono_pending_count++;
	return 1;
}

/* Forward declarations needed by emit_monomorphized_proc. */
typedef struct {
	char *saved_output_buffer;
	size_t saved_buffer_size;
	size_t saved_buffer_pos;
} FunctionBodyState;
static void codegen_statement(CodegenContext *ctx, HirStmt *stmt);
static FunctionBodyState begin_function_body(CodegenContext *ctx);
static void end_function_body(CodegenContext *ctx, FunctionBodyState fbs);

/* Bind a `T[]` slice param (any element, incl. char) as a type-6 fat-pointer value: `%argN.ptr`
 * is the element pointer, `%argN.len` the runtime length. Shared by the proc/func/mono emitters so
 * char[] is a normal slice everywhere — no arche_array. */
static void bind_slice_param_value(CodegenContext *ctx, const char *name, HirType *ptype, int argidx) {
	const char *en = field_base_type_name(ptype);
	const char *lt = llvm_type_from_arche(en);
	ValueInfo *vi = calloc(1, sizeof(ValueInfo));
	vi->name = malloc(strlen(name) + 1);
	strcpy(vi->name, name);
	char ptr_name[40], len_name[40];
	snprintf(ptr_name, sizeof(ptr_name), "%%arg%d.ptr", argidx);
	snprintf(len_name, sizeof(len_name), "%%arg%d.len", argidx);
	vi->llvm_name = malloc(strlen(ptr_name) + 1);
	strcpy(vi->llvm_name, ptr_name);
	vi->type = 6;
	vi->arch_name = NULL;
	vi->string_len = -1;
	vi->field_type = en;
	vi->bit_width = strcmp(lt, "double") == 0 ? 64 : (strcmp(lt, "i8") == 0 ? 8 : 32);
	vi->is_slice = 1;
	vi->len_ssa = malloc(strlen(len_name) + 1);
	strcpy(vi->len_ssa, len_name);
	if (ctx->scope_count > 0) {
		ValueScope *scope = &ctx->scopes[ctx->scope_count - 1];
		scope->values = realloc(scope->values, (scope->value_count + 1) * sizeof(ValueInfo *));
		scope->values[scope->value_count++] = vi;
	}
}

/* Emit a monomorphized version of an archetype-parametric proc for one
 * concrete archetype. Adds an entry to mono_emitted. */
static void emit_monomorphized_proc(CodegenContext *ctx, HirProcDecl *proc, HirArchetypeDecl *arch) {
	if (monomorph_already_emitted(ctx, proc->name, arch->name))
		return;
	monomorph_mark_emitted(ctx, proc->name, arch->name);

	int arch_pidx = archetype_param_index(proc);
	if (arch_pidx < 0)
		return; /* shouldn't happen if caller checked */

	char mangled[256];
	monomorph_mangle(proc->name, arch->name, mangled, sizeof(mangled));

	buffer_append_fmt(ctx, "define %svoid @%s(", cg_shared(ctx), mangled);
	for (int i = 0; i < proc->param_count; i++) {
		HirType *param_type = proc->params[i]->type;
		if (i == arch_pidx) {
			buffer_append_fmt(ctx, "%%struct.%s* %%arg%d", arch->name, i);
		} else if (param_type && param_type->tag == HIR_TYPE_ARRAY) {
			/* `T[]` slice (incl. char): two-arg fat pointer `(T* ptr, i64 len)`. */
			const char *bt = llvm_type_from_arche(field_base_type_name(param_type));
			buffer_append_fmt(ctx, "%s* %%arg%d.ptr, i64 %%arg%d.len", bt, i, i);
		} else if (param_type && param_type->tag == HIR_TYPE_NAMED && find_archetype_decl(ctx, param_type->name)) {
			buffer_append_fmt(ctx, "%%struct.%s* %%arg%d", param_type->name, i);
		} else {
			const char *base_type = llvm_type_from_arche(field_base_type_name(param_type));
			buffer_append_fmt(ctx, "%s %%arg%d", base_type, i);
		}
		if (i < proc->param_count - 1)
			buffer_append(ctx, ", ");
	}
	buffer_append(ctx, ") {\n");
	buffer_append(ctx, "entry:\n");

	FunctionBodyState fbs = begin_function_body(ctx);
	push_value_scope(ctx);
	register_static_arrays_in_scope(ctx);

	/* Register parameters: archetype param as `add_arch_value` so existing
	 * codegen recognizes it as an archetype struct pointer. */
	char arch_llvm[32];
	snprintf(arch_llvm, sizeof(arch_llvm), "%%arg%d", arch_pidx);
	for (int i = 0; i < proc->param_count; i++) {
		char param_llvm[32];
		snprintf(param_llvm, sizeof(param_llvm), "%%arg%d", i);
		HirType *param_type = proc->params[i]->type;
		if (i == arch_pidx) {
			add_arch_value(ctx, proc->params[i]->name, param_llvm, arch->name);
		} else if (param_type && param_type->tag == HIR_TYPE_ARRAY) {
			bind_slice_param_value(ctx, proc->params[i]->name, param_type, i);
		} else if (param_type && param_type->tag == HIR_TYPE_NAMED && find_archetype_decl(ctx, param_type->name)) {
			add_arch_value(ctx, proc->params[i]->name, param_llvm, param_type->name);
		} else {
			add_value(ctx, proc->params[i]->name, param_llvm, 0);
		}
	}

	/* Activate monomorphization context for the duration of the body emission. */
	HirArchetypeDecl *saved_arch = ctx->current_archetype_param;
	const char *saved_pname = ctx->current_arch_param_name;
	const char *saved_pllvm = ctx->current_arch_param_llvm;
	ctx->current_archetype_param = arch;
	ctx->current_arch_param_name = proc->params[arch_pidx]->name;
	ctx->current_arch_param_llvm = strdup(arch_llvm);

	for (int i = 0; i < proc->stmt_count; i++) {
		codegen_statement(ctx, proc->stmts[i]);
	}

	free((void *)ctx->current_arch_param_llvm);
	ctx->current_archetype_param = saved_arch;
	ctx->current_arch_param_name = saved_pname;
	ctx->current_arch_param_llvm = saved_pllvm;

	pop_value_scope(ctx);
	buffer_append(ctx, "  ret void\n");
	buffer_append(ctx, "}\n\n");
	end_function_body(ctx, fbs);
}

/* Build the mangled symbol for a callback specialization: the proc name plus
 * each callback arg's bound proc/func name. `bound[i]` is non-NULL only at
 * callback-param indices. */
static void cb_mangle(HirProcDecl *proc, const char *const *bound, char *out, size_t out_sz) {
	int n = snprintf(out, out_sz, "__%s", proc->name);
	for (int i = 0; i < proc->param_count && n < (int)out_sz; i++) {
		if (bound[i])
			n += snprintf(out + n, out_sz - n, "__%s", bound[i]);
	}
}

static int cb_already_emitted(CodegenContext *ctx, const char *mangled) {
	for (int i = 0; i < ctx->cb_emitted_count; i++)
		if (strcmp(ctx->cb_emitted[i], mangled) == 0)
			return 1;
	return 0;
}

static void cb_mark_emitted(CodegenContext *ctx, const char *mangled) {
	if (ctx->cb_emitted_count >= ctx->cb_emitted_capacity) {
		ctx->cb_emitted_capacity = ctx->cb_emitted_capacity == 0 ? 8 : ctx->cb_emitted_capacity * 2;
		ctx->cb_emitted = realloc(ctx->cb_emitted, ctx->cb_emitted_capacity * sizeof(*ctx->cb_emitted));
	}
	ctx->cb_emitted[ctx->cb_emitted_count] = strdup(mangled);
	ctx->cb_emitted_count++;
}

/* Queue a callback specialization (proc + per-param bound names). Dedups by
 * mangled symbol. Takes ownership of nothing; copies bound names. */
static void cb_enqueue(CodegenContext *ctx, HirProcDecl *proc, const char *const *bound, const char *mangled) {
	for (int i = 0; i < ctx->cb_pending_count; i++)
		if (strcmp(ctx->cb_pending[i].mangled, mangled) == 0)
			return;
	if (cb_already_emitted(ctx, mangled))
		return;
	if (ctx->cb_pending_count >= ctx->cb_pending_capacity) {
		ctx->cb_pending_capacity = ctx->cb_pending_capacity == 0 ? 8 : ctx->cb_pending_capacity * 2;
		ctx->cb_pending = realloc(ctx->cb_pending, ctx->cb_pending_capacity * sizeof(*ctx->cb_pending));
	}
	int slot = ctx->cb_pending_count++;
	ctx->cb_pending[slot].proc = proc;
	ctx->cb_pending[slot].mangled = strdup(mangled);
	for (int i = 0; i < proc->param_count && i < 8; i++)
		ctx->cb_pending[slot].bound[i] = bound[i] ? strdup(bound[i]) : NULL;
}

/* Emit a callback-specialized copy of a callback-parametric proc. Callback
 * params take no runtime slot of their own beyond an ignored `i8*` placeholder
 * (so call/define arg counts stay aligned); their calls in the body are
 * rewritten to direct calls to the bound proc via the cb_* active bindings. */
static void emit_callback_monomorphized_proc(CodegenContext *ctx, HirProcDecl *proc, const char *const *bound,
                                             const char *mangled) {
	if (cb_already_emitted(ctx, mangled))
		return;
	cb_mark_emitted(ctx, mangled);

	buffer_append_fmt(ctx, "define %svoid @%s(", cg_shared(ctx), mangled);
	int emitted = 0;
	for (int i = 0; i < proc->param_count; i++) {
		HirType *param_type = proc->params[i]->type;
		if (is_callback_param(ctx, proc->params[i]))
			continue; /* callback params carry no runtime value — dropped from the ABI */
		if (emitted > 0)
			buffer_append(ctx, ", ");
		if (param_type && param_type->tag == HIR_TYPE_ARRAY) {
			/* `T[]` slice (incl. char): two-arg fat pointer `(T* ptr, i64 len)`. */
			const char *bt = llvm_type_from_arche(field_base_type_name(param_type));
			buffer_append_fmt(ctx, "%s* %%arg%d.ptr, i64 %%arg%d.len", bt, i, i);
		} else if (param_type && param_type->tag == HIR_TYPE_NAMED && find_archetype_decl(ctx, param_type->name)) {
			buffer_append_fmt(ctx, "%%struct.%s* %%arg%d", param_type->name, i);
		} else {
			const char *base_type = llvm_type_from_arche(field_base_type_name(param_type));
			buffer_append_fmt(ctx, "%s %%arg%d", base_type, i);
		}
		emitted++;
	}
	buffer_append(ctx, ") {\n");
	buffer_append(ctx, "entry:\n");

	FunctionBodyState fbs = begin_function_body(ctx);
	push_value_scope(ctx);
	register_static_arrays_in_scope(ctx);

	/* Activate the callback bindings (param name → bound proc) for the body. */
	int saved_cb = ctx->cb_binding_count;
	ctx->cb_binding_count = 0;
	for (int i = 0; i < proc->param_count; i++) {
		char param_llvm[32];
		snprintf(param_llvm, sizeof(param_llvm), "%%arg%d", i);
		HirType *param_type = proc->params[i]->type;
		if (is_callback_param(ctx, proc->params[i])) {
			if (ctx->cb_binding_count < 8) {
				ctx->cb_param_names[ctx->cb_binding_count] = proc->params[i]->name;
				ctx->cb_bound_names[ctx->cb_binding_count] = bound[i];
				ctx->cb_binding_count++;
			}
		} else if (param_type && param_type->tag == HIR_TYPE_ARRAY) {
			bind_slice_param_value(ctx, proc->params[i]->name, param_type, i);
		} else if (param_type && param_type->tag == HIR_TYPE_NAMED && find_archetype_decl(ctx, param_type->name)) {
			add_arch_value(ctx, proc->params[i]->name, param_llvm, param_type->name);
		} else {
			add_value(ctx, proc->params[i]->name, param_llvm, 0);
		}
	}

	for (int i = 0; i < proc->stmt_count; i++)
		codegen_statement(ctx, proc->stmts[i]);

	ctx->cb_binding_count = saved_cb;
	pop_value_scope(ctx);
	buffer_append(ctx, "  ret void\n");
	buffer_append(ctx, "}\n\n");
	end_function_body(ctx, fbs);
}

/* Drain the pending monomorph worklist. May add more entries while emitting,
 * so keep looping until empty. */
static void drain_monomorph_worklist(CodegenContext *ctx) {
	while (ctx->mono_pending_count > 0 || ctx->cb_pending_count > 0) {
		while (ctx->mono_pending_count > 0) {
			/* Pop the last entry. Order doesn't matter for correctness. */
			ctx->mono_pending_count--;
			HirProcDecl *p = ctx->mono_pending[ctx->mono_pending_count].proc;
			HirArchetypeDecl *a = ctx->mono_pending[ctx->mono_pending_count].arch;
			emit_monomorphized_proc(ctx, p, a);
		}
		while (ctx->cb_pending_count > 0) {
			/* Take ownership of the popped entry's heap into locals BEFORE emitting:
			 * emit may enqueue a forwarded callback, reusing this now-free slot. */
			int idx = --ctx->cb_pending_count;
			HirProcDecl *p = ctx->cb_pending[idx].proc;
			char *mangled = ctx->cb_pending[idx].mangled;
			char *bound[8] = {0};
			for (int i = 0; i < p->param_count && i < 8; i++)
				bound[i] = (char *)ctx->cb_pending[idx].bound[i];
			emit_callback_monomorphized_proc(ctx, p, (const char *const *)bound, mangled);
			for (int i = 0; i < p->param_count && i < 8; i++)
				free(bound[i]);
			free(mangled);
		}
	}
}

/* For a group call, walk members and return the unique HirFuncDecl whose param
 * tags match args[].resolved.tag on int/float/char. Returns NULL on no-match or
 * ambiguous (which the semantic pass should have already diagnosed). */
static HirFuncDecl *find_group_member_for_call(CodegenContext *ctx, HirFuncGroupDecl *g, HirExpr **args,
                                               int arg_count) {
	HirFuncDecl *match = NULL;
	int match_count = 0;
	for (int m = 0; m < g->member_count; m++) {
		HirFuncDecl *fd = find_func_decl(ctx, g->member_names[m]);
		if (!fd)
			continue;
		if (fd->param_count != arg_count)
			continue;
		int ok = 1;
		for (int j = 0; j < arg_count; j++) {
			HirType *pt = fd->params[j]->type;
			HirTypeTag at = args[j]->resolved.tag;
			if (!pt)
				continue;
			/* Only scalar int/float/char params discriminate group members.
			 * Non-scalar params (arrays, handles, etc.) are identical across
			 * members and don't participate in witness-based dispatch. */
			if (pt->tag != HIR_TYPE_INT && pt->tag != HIR_TYPE_FLOAT && pt->tag != HIR_TYPE_CHAR)
				continue;
			if (pt->tag == HIR_TYPE_INT && at == HIR_TYPE_INT) {
				/* Distinguish int members by width + signedness (e.g. int vs i64). */
				int pw = pt->int_width ? pt->int_width : 32;
				int aw = args[j]->resolved.int_width ? args[j]->resolved.int_width : 32;
				if (pw == aw && pt->int_signed == args[j]->resolved.int_signed)
					continue;
				ok = 0;
				break;
			}
			if (pt->tag == HIR_TYPE_FLOAT && at == HIR_TYPE_FLOAT)
				continue;
			if (pt->tag == HIR_TYPE_CHAR && at == HIR_TYPE_CHAR)
				continue;
			ok = 0;
			break;
		}
		if (!ok)
			continue;
		match = fd;
		match_count++;
	}
	return match_count == 1 ? match : NULL;
}

static const char *get_shaped_field_info(CodegenContext *ctx, HirExpr *field_expr, int *out_rank) {
	if (field_expr->kind != HIR_EXPR_FIELD || field_expr->data.field.base->kind != HIR_EXPR_NAME)
		return NULL;
	const char *vn = field_expr->data.field.base->data.name.name;
	const char *fn = field_expr->data.field.field_name;
	const char *arch_name = NULL;
	ValueInfo *vi = find_value(ctx, vn);
	if (vi && vi->arch_name) {
		arch_name = vi->arch_name;
	} else if (find_archetype_decl(ctx, vn)) {
		arch_name = vn;
	}
	if (!arch_name)
		return NULL;
	HirArchetypeDecl *arch = find_archetype_decl(ctx, arch_name);
	if (!arch)
		return NULL;
	for (int j = 0; j < arch->field_count; j++) {
		if (strcmp(arch->fields[j]->name, fn) == 0 && arch->fields[j]->type->tag == HIR_TYPE_SHAPED_ARRAY) {
			if (out_rank)
				*out_rank = field_total_elements(arch->fields[j]->type);
			return field_base_type_name(arch->fields[j]->type);
		}
	}
	return NULL;
}

/* Check if archetype has a field with given name */
static int archetype_has_field(HirArchetypeDecl *arch, const char *field_name) {
	for (int i = 0; i < arch->field_count; i++) {
		if (strcmp(arch->fields[i]->name, field_name) == 0) {
			return 1;
		}
	}
	return 0;
}

/* Check if archetype has all required fields for a system */
static int archetype_matches_system(HirArchetypeDecl *arch, HirSysDecl *sys) {
	if (!arch || !sys || !sys->params) {
		return 0;
	}
	for (int i = 0; i < sys->param_count; i++) {
		if (!sys->params[i] || !sys->params[i]->name) {
			return 0;
		}
		const char *param_name = sys->params[i]->name;
		if (!archetype_has_field(arch, param_name)) {
			return 0;
		}
	}
	return 1;
}

/* Forward declarations */
static void codegen_expression(CodegenContext *ctx, HirExpr *expr, char *result_buf);
static void codegen_statement(CodegenContext *ctx, HirStmt *stmt);
static int resolve_index_arch(CodegenContext *ctx, HirExpr *base_expr, HirExpr *idx_expr, const char **out_arch_name,
                              const char **out_arch_ptr, int *out_count_idx, int *out_idx_is_i64);
static void emit_bounds_check(CodegenContext *ctx, const char *arch_name, const char *arch_ptr, int count_field_idx,
                              const char *idx_buf, int idx_is_i64);
static int bounds_check_elidable(CodegenContext *ctx, const char *arch_name, HirExpr *idx_expr);
static void emit_const_bounds_check(CodegenContext *ctx, int n, const char *idx_buf, int idx_is_i64);
static int const_index_in_range(CodegenContext *ctx, HirExpr *idx_expr, int n);
static int try_extract_loop_bound(HirStmt *for_stmt, char **out_var, int *out_bound);
static void push_loop_bound(CodegenContext *ctx, const char *var_name, int bound);
static void pop_loop_bound(CodegenContext *ctx);

/* Emit a `buf[lo:hi]` sub-slice. Computes the element pointer `base.ptr + lo` and runtime length
 * `hi - lo`, writing them into ptr_out / len_out (caller char[256]) and the element base name +
 * bit width into *elem_out / *bitw_out. Returns 1 on success (base is a known array/slice), 0 if the
 * base isn't sliceable. Omitted bounds default to lo=0, hi=base length. A read-only borrowed view —
 * no copy, no consume. */
static int codegen_slice(CodegenContext *ctx, HirExpr *e, char *ptr_out, char *len_out, char *cap_out,
                         const char **elem_out, int *bitw_out) {
	HirExpr *base = e->data.slice.base;
	ValueInfo *bv = (base->kind == HIR_EXPR_NAME) ? find_value(ctx, base->data.name.name) : NULL;
	if (!bv)
		return 0;
	const char *elem_base, *elem_llvm;
	char base_ptr[256], base_len[64], base_cap[64];
	if (bv->type == 6 && bv->field_type) {
		elem_base = bv->field_type;
		elem_llvm = llvm_type_from_arche(elem_base);
		strcpy(base_ptr, bv->llvm_name);
		if (bv->is_slice && bv->len_ssa)
			snprintf(base_len, sizeof(base_len), "%s", bv->len_ssa);
		else
			snprintf(base_len, sizeof(base_len), "%d", bv->string_len);
		/* Backing capacity of the base: a slice carries its own cap (else falls back to len); a
		 * bounded T[N] has cap = N. The sub-view's cap is this minus the lo offset (below). */
		if (bv->is_slice)
			snprintf(base_cap, sizeof(base_cap), "%s", bv->cap_ssa ? bv->cap_ssa : base_len);
		else
			snprintf(base_cap, sizeof(base_cap), "%d", bv->string_len);
	} else if (bv->type == 7) {
		elem_base = "char";
		elem_llvm = "i8";
		char *ep = gen_value_name(ctx);
		buffer_append_fmt(ctx, "  %s = getelementptr [%d x i8], [%d x i8]* %s, i64 0, i64 0\n", ep, bv->string_len,
		                  bv->string_len, bv->llvm_name);
		strcpy(base_ptr, ep);
		snprintf(base_len, sizeof(base_len), "%d", bv->string_len);
		snprintf(base_cap, sizeof(base_cap), "%d", bv->string_len);
	} else {
		return 0;
	}
	char lo64[256] = "0";
	if (e->data.slice.lo) {
		char b[256];
		codegen_expression(ctx, e->data.slice.lo, b);
		emit_index_i64(ctx, b, e->data.slice.lo, lo64);
	}
	char hi64[256];
	if (e->data.slice.hi) {
		char b[256];
		codegen_expression(ctx, e->data.slice.hi, b);
		emit_index_i64(ctx, b, e->data.slice.hi, hi64);
	} else {
		snprintf(hi64, sizeof(hi64), "%s", base_len);
	}
	/* Bounds: 0 <= lo <= hi <= len. The unsigned compares also catch a negative lo/hi (sext'd to a
	 * huge i64). On violation, write the OOB message and abort — never a silent over-read. */
	{
		char *c1 = gen_value_name(ctx);
		buffer_append_fmt(ctx, "  %s = icmp ule i64 %s, %s\n", c1, lo64, hi64);
		char *c2 = gen_value_name(ctx);
		buffer_append_fmt(ctx, "  %s = icmp ule i64 %s, %s\n", c2, hi64, base_len);
		char *ok = gen_value_name(ctx);
		buffer_append_fmt(ctx, "  %s = and i1 %s, %s\n", ok, c1, c2);
		int id = ctx->value_counter++;
		char okl[64], faill[64];
		snprintf(okl, sizeof(okl), "slice_ok_%d", id);
		snprintf(faill, sizeof(faill), "slice_fail_%d", id);
		buffer_append_fmt(ctx, "  br i1 %s, label %%%s, label %%%s\n\n", ok, okl, faill);
		buffer_append_fmt(ctx, "%s:\n", faill);
		buffer_append(ctx, "  call i32 @write(i32 2, i8* getelementptr ([28 x i8], [28 x i8]* @.arche_oob, i32 0, "
		                   "i32 0), i32 27)\n");
		buffer_append(ctx, "  call void @abort()\n");
		buffer_append(ctx, "  unreachable\n\n");
		buffer_append_fmt(ctx, "%s:\n", okl);
	}
	char *ptr2 = gen_value_name(ctx);
	buffer_append_fmt(ctx, "  %s = getelementptr %s, %s* %s, i64 %s\n", ptr2, elem_llvm, elem_llvm, base_ptr, lo64);
	char *len2 = gen_value_name(ctx);
	buffer_append_fmt(ctx, "  %s = sub i64 %s, %s\n", len2, hi64, lo64);
	snprintf(ptr_out, 256, "%s", ptr2);
	snprintf(len_out, 256, "%s", len2);
	/* Sub-view capacity = backing capacity from the slice start: base_cap - lo. */
	if (cap_out) {
		char *cap2 = gen_value_name(ctx);
		buffer_append_fmt(ctx, "  %s = sub i64 %s, %s\n", cap2, base_cap, lo64);
		snprintf(cap_out, 256, "%s", cap2);
	}
	*elem_out = elem_base;
	*bitw_out = strcmp(elem_llvm, "double") == 0 ? 64 : (strcmp(elem_llvm, "i8") == 0 ? 8 : 32);
	return 1;
}

/* ========== EXPRESSION CODEGEN ========== */

static void codegen_expression(CodegenContext *ctx, HirExpr *expr, char *result_buf) {
	if (!expr) {
		strcpy(result_buf, "0");
		return;
	}

	switch (expr->kind) {
	case HIR_EXPR_LITERAL: {
		const char *lex = expr->data.literal.lexeme;

		/* Check if it's a char literal (starts with ') */
		if (lex[0] == '\'') {
			snprintf(result_buf, 256, "%d", char_literal_value(lex)); /* result_buf is char[256] by caller contract */
		} else if (lex[0] == '"') {
			/* String literal */
			char *global_name = emit_string_global(ctx, lex);

			/* Compute actual string length accounting for escape sequences */
			size_t str_len = 0;
			for (int i = 1; lex[i] != '"' && lex[i] != '\0'; i++) {
				if (lex[i] == '\\' && lex[i + 1] != '\0') {
					i++; /* skip escape sequence (count as 1 char) */
				}
				str_len++;
			}

			char *res_name = gen_value_name(ctx);
			buffer_append_fmt(ctx, "  %s = getelementptr [%zu x i8], [%zu x i8]* %s, i32 0, i32 0\n", res_name,
			                  str_len + 1, str_len + 1, global_name);
			strcpy(result_buf, res_name);
			free(global_name);
		} else if (strchr(lex, '.') != NULL) {
			/* Float literal */
			strcpy(result_buf, lex);
		} else {
			/* Integer literal */
			strcpy(result_buf, lex);
		}
		break;
	}

	case HIR_EXPR_NAME: {
		const char *name = expr->data.name.name;

		/* Check if this is a compile-time constant */
		const char *const_val = semantic_get_const_value(ctx->sem_ctx, name);
		if (const_val) {
			/* Inline the constant's value. A char-literal lexeme must be emitted as its integer
			 * code (LLVM has no char token); int/float lexemes pass through verbatim. */
			if (const_val[0] == '\'')
				snprintf(result_buf, 256, "%d", char_literal_value(const_val));
			else
				strcpy(result_buf, const_val);
			return;
		}

		ValueInfo *val = find_value(ctx, name);
		if (val) {
			/* If inside implicit loop and this is a type-4 column param, auto-index */
			if (ctx->implicit_loop_index[0] && val->type == 4) {
				const char *arche_type = val->field_type ? val->field_type : "float";
				const char *scalar_type = llvm_type_from_arche(arche_type);
				const char *load_type = elem_llvm_type(ctx, arche_type);
				char *idx_gep = gen_value_name(ctx);
				buffer_append_fmt(ctx, "  %s = getelementptr %s, %s* %s, i64 %s\n", idx_gep, scalar_type, scalar_type,
				                  val->llvm_name, ctx->implicit_loop_index);
				char *elem = gen_value_name(ctx);
				int align = ctx->vector_lanes > 0 ? 8 : 4;

				if (ctx->vector_lanes > 0) {
					/* Vector load: bitcast pointer to vector type, then load */
					char *vec_ptr = gen_value_name(ctx);
					buffer_append_fmt(ctx, "  %s = bitcast %s* %s to %s*\n", vec_ptr, scalar_type, idx_gep, load_type);
					buffer_append_fmt(ctx, "  %s = load %s, %s* %s, align %d\n", elem, load_type, load_type, vec_ptr,
					                  align);
				} else {
					/* Scalar load */
					buffer_append_fmt(ctx, "  %s = load %s, %s* %s, align %d\n", elem, load_type, scalar_type, idx_gep,
					                  align);
				}
				strcpy(result_buf, elem);
			} else if (val->type == 1) {
				/* Type-1: regular allocated value (iN, float, handle, etc) - load from pointer */
				char *loaded = gen_value_name(ctx);
				const char *llvm_type = "i32";
				if (val->field_type) {
					int w, sg;
					if (strcmp(val->field_type, "double") == 0 || strcmp(val->field_type, "float") == 0) {
						llvm_type = "double";
					} else if (strcmp(val->field_type, "handle") == 0 || strcmp(val->field_type, "opaque") == 0) {
						llvm_type = "i64";
					} else if (hir_parse_int_width(val->field_type, &w, &sg)) {
						llvm_type = llvm_int_type(w);
					}
				}
				buffer_append_fmt(ctx, "  %s = load %s, %s* %s\n", loaded, llvm_type, llvm_type, val->llvm_name);
				strcpy(result_buf, loaded);
			} else {
				/* Type-2 (string), 3 (arch), 5 (array), 6 (i8* param): return pointer directly */
				strcpy(result_buf, val->llvm_name);
			}
		} else if (codegen_find_static_array(ctx, name)) {
			/* Static array name — return global reference @name */
			/* Function call handling will convert to pointer when needed */
			char global_ref[256];
			snprintf(global_ref, sizeof(global_ref), "@%s", name);
			strcpy(result_buf, global_ref);
		} else if (codegen_find_scalar(ctx, name)) {
			/* Scalar global read: load the current value from @name. */
			HirStaticDecl *sc = codegen_find_scalar(ctx, name);
			const char *lt = llvm_type_from_arche(field_base_type_name(sc->scalar.type));
			char *loaded = gen_value_name(ctx);
			buffer_append_fmt(ctx, "  %s = load %s, %s* @%s\n", loaded, lt, lt, name);
			strcpy(result_buf, loaded);
		} else if (find_archetype_decl(ctx, name)) {
			/* Archetype name → its shape's storage. Aliases resolve to the canonical name, so
			 * every alias references the one struct/global. Static global is @X directly;
			 * dynamic is loaded from @archetype_X. */
			const char *cn = canonical_arch_name(ctx, name);
			if (get_arch_static_capacity(ctx, cn) > 0) {
				char global_ref[256];
				snprintf(global_ref, sizeof(global_ref), "@%s", cn);
				strcpy(result_buf, global_ref);
			} else {
				char *loaded = gen_value_name(ctx);
				buffer_append_fmt(ctx, "  %s = load %%struct.%s*, %%struct.%s** @archetype_%s\n", loaded, cn, cn, cn);
				strcpy(result_buf, loaded);
			}
		} else {
			/* undefined variable, use 0 */
			strcpy(result_buf, "0");
		}
		break;
	}

	case HIR_EXPR_BINARY: {
		char left_buf[256], right_buf[256];

		/* For comparisons in scalar context, evaluate operands as scalars
		 * (C-style: comparison returns scalar int 0/1). In vector context,
		 * preserve vector lanes so operands load as <N x T> and the
		 * comparison emits as a vector mask. */
		int save_vector_lanes = ctx->vector_lanes;
		if (expr->data.binary.op >= OP_EQ && expr->data.binary.op <= OP_GTE && ctx->vector_lanes == 0) {
			ctx->vector_lanes = 0;
		}

		codegen_expression(ctx, expr->data.binary.left, left_buf);
		codegen_expression(ctx, expr->data.binary.right, right_buf);

		ctx->vector_lanes = save_vector_lanes;

		const char *op;
		int is_float = 0;

		/* Use semantic resolved type if available */
		if (expr->resolved.tag == HIR_TYPE_FLOAT) {
			is_float = 1;
		} else if (expr->data.binary.left->resolved.tag == HIR_TYPE_FLOAT) {
			is_float = 1;
		} else if (expr->data.binary.right->resolved.tag == HIR_TYPE_FLOAT) {
			is_float = 1;
		} else {
			/* Fallback: Try to infer float type from operands */
			if (strchr(left_buf, '.') != NULL || strchr(right_buf, '.') != NULL) {
				is_float = 1;
			}
		}

		/* Integer width/signedness from operands (max width; sign from the first
		 * int operand). Drives iN type + signed/unsigned op selection. */
		int int_width = 32, int_signed = 1;
		if (!is_float) {
			HirType *lt = &expr->data.binary.left->resolved;
			HirType *rt = &expr->data.binary.right->resolved;
			int lw = (lt->tag == HIR_TYPE_INT) ? lt->int_width : 0;
			int rw = (rt->tag == HIR_TYPE_INT) ? rt->int_width : 0;
			int_width = lw > rw ? lw : rw;
			if (int_width == 0)
				int_width = 32; /* unset/non-int operands default to i32 */
			if (expr->data.binary.left->resolved.tag == HIR_TYPE_OPAQUE ||
			    expr->data.binary.right->resolved.tag == HIR_TYPE_OPAQUE)
				int_width = 64; /* opaque cells are pointer-width i64 (e.g. `r == 0`) */
			if (lt->tag == HIR_TYPE_INT)
				int_signed = lt->int_signed;
			else if (rt->tag == HIR_TYPE_INT)
				int_signed = rt->int_signed;
		}

		switch (expr->data.binary.op) {
		case OP_ADD:
			op = is_float ? "fadd" : "add";
			break;
		case OP_SUB:
			op = is_float ? "fsub" : "sub";
			break;
		case OP_MUL:
			op = is_float ? "fmul" : "mul";
			break;
		case OP_DIV:
			op = is_float ? "fdiv" : (int_signed ? "sdiv" : "udiv");
			break;
		case OP_EQ:
			op = is_float ? "oeq" : "eq";
			break;
		case OP_NEQ:
			op = is_float ? "one" : "ne";
			break;
		case OP_LT:
			op = is_float ? "olt" : (int_signed ? "slt" : "ult");
			break;
		case OP_GT:
			op = is_float ? "ogt" : (int_signed ? "sgt" : "ugt");
			break;
		case OP_LTE:
			op = is_float ? "ole" : (int_signed ? "sle" : "ule");
			break;
		case OP_GTE:
			op = is_float ? "oge" : (int_signed ? "sge" : "uge");
			break;
		default:
			op = "add";
			break;
		}

		char *res_name = gen_value_name(ctx);
		const char *type;
		if (is_float && ctx->vector_lanes > 0) {
			type = llvm_vector_type("double", ctx->vector_lanes);
		} else if (is_float) {
			type = "double";
		} else if (ctx->vector_lanes > 0) {
			type = "i32"; /* vector int lanes stay i32 (column SIMD path) */
		} else {
			type = llvm_int_type(int_width);
		}

		/* For float operations, convert integer literals to doubles */
		const char *left_val = left_buf;
		const char *right_val = right_buf;
		char *left_conv = NULL;
		char *right_conv = NULL;

		if (is_float) {
			/* Usual arithmetic conversions: the op's type is double, so each integer operand is
			 * promoted to double (sitofp). `char` is an integer type and promotes too — keying
			 * off the integer *category* (INT or CHAR), not just HIR_TYPE_INT, is what catches a
			 * char-derived digit like `s[i] - '0'`. A bare integer literal (no '.', not an SSA
			 * name) is likewise converted; an already-double value (float-tagged SSA or a '.'
			 * literal) is left alone. */
			int left_needs_conv = 0;
			if (expr->data.binary.left->resolved.tag == HIR_TYPE_INT ||
			    expr->data.binary.left->resolved.tag == HIR_TYPE_CHAR) {
				left_needs_conv = 1;
			} else if (strchr(left_buf, '.') == NULL && strchr(left_buf, 'v') == NULL &&
			           strchr(left_buf, '%') == NULL) {
				/* Integer literal, convert to double */
				left_needs_conv = 1;
			}
			if (left_needs_conv) {
				left_conv = gen_value_name(ctx);
				if (ctx->vector_lanes > 0) {
					char from_type_buf[64];
					char to_type_buf[64];
					snprintf(from_type_buf, sizeof(from_type_buf), "<%d x i32>", ctx->vector_lanes);
					snprintf(to_type_buf, sizeof(to_type_buf), "<%d x double>", ctx->vector_lanes);
					buffer_append_fmt(ctx, "  %s = sitofp %s %s to %s\n", left_conv, from_type_buf, left_buf,
					                  to_type_buf);
				} else {
					buffer_append_fmt(ctx, "  %s = sitofp i32 %s to double\n", left_conv, left_buf);
				}
				left_val = left_conv;
			}

			/* Right operand: same usual-arithmetic-conversion rule as the left. */
			int right_needs_conv = 0;
			if (expr->data.binary.right->resolved.tag == HIR_TYPE_INT ||
			    expr->data.binary.right->resolved.tag == HIR_TYPE_CHAR) {
				right_needs_conv = 1;
			} else if (strchr(right_buf, '.') == NULL && strchr(right_buf, 'v') == NULL &&
			           strchr(right_buf, '%') == NULL) {
				/* Integer literal, convert to double */
				right_needs_conv = 1;
			}
			if (right_needs_conv) {
				right_conv = gen_value_name(ctx);
				if (ctx->vector_lanes > 0) {
					char from_type_buf[64];
					char to_type_buf[64];
					snprintf(from_type_buf, sizeof(from_type_buf), "<%d x i32>", ctx->vector_lanes);
					snprintf(to_type_buf, sizeof(to_type_buf), "<%d x double>", ctx->vector_lanes);
					buffer_append_fmt(ctx, "  %s = sitofp %s %s to %s\n", right_conv, from_type_buf, right_buf,
					                  to_type_buf);
				} else {
					buffer_append_fmt(ctx, "  %s = sitofp i32 %s to double\n", right_conv, right_buf);
				}
				right_val = right_conv;
			}

			/* Splat scalar float literals to vector type when vectorized */
			if (ctx->vector_lanes > 0 && is_float) {
				const char *vec_type = llvm_vector_type("double", ctx->vector_lanes);
				/* no '%' prefix = scalar constant */
				if (strchr(left_val, '%') == NULL) {
					char *ins = gen_value_name(ctx);
					char *splat = gen_value_name(ctx);
					buffer_append_fmt(ctx, "  %s = insertelement %s undef, double %s, i32 0\n", ins, vec_type,
					                  left_val);
					buffer_append_fmt(ctx, "  %s = shufflevector %s %s, %s undef, <4 x i32> zeroinitializer\n", splat,
					                  vec_type, ins, vec_type);
					left_val = splat;
				}
				if (strchr(right_val, '%') == NULL) {
					char *ins = gen_value_name(ctx);
					char *splat = gen_value_name(ctx);
					buffer_append_fmt(ctx, "  %s = insertelement %s undef, double %s, i32 0\n", ins, vec_type,
					                  right_val);
					buffer_append_fmt(ctx, "  %s = shufflevector %s %s, %s undef, <4 x i32> zeroinitializer\n", splat,
					                  vec_type, ins, vec_type);
					right_val = splat;
				}
			}
		}

		/* Scalar integer operands: coerce both to the common width so the op
		 * has matching operand types (e.g. i64 total + i8 byte). */
		char lwconv[256], rwconv[256];
		if (!is_float && ctx->vector_lanes == 0) {
			emit_int_convert(ctx, left_val, &expr->data.binary.left->resolved, int_width, lwconv);
			emit_int_convert(ctx, right_val, &expr->data.binary.right->resolved, int_width, rwconv);
			left_val = lwconv;
			right_val = rwconv;
		}

		if (expr->data.binary.op == OP_AND || expr->data.binary.op == OP_OR) {
			/* Logical `&&` / `||`. Eager: arche expressions have no side effects and no
			 * traps, so eager evaluation is indistinguishable from short-circuit. Normalize
			 * each operand to i1 (`!= 0`), combine with `and`/`or`, zext back to i32 (arche
			 * has no bool; conditions are int 0/1 like comparisons). Scalar int only. */
			const char *lt = llvm_int_type(int_width);
			char *l_i1 = gen_value_name(ctx);
			buffer_append_fmt(ctx, "  %s = icmp ne %s %s, 0\n", l_i1, lt, left_val);
			char *r_i1 = gen_value_name(ctx);
			buffer_append_fmt(ctx, "  %s = icmp ne %s %s, 0\n", r_i1, lt, right_val);
			char *comb = gen_value_name(ctx);
			const char *logop = (expr->data.binary.op == OP_AND) ? "and" : "or";
			buffer_append_fmt(ctx, "  %s = %s i1 %s, %s\n", comb, logop, l_i1, r_i1);
			buffer_append_fmt(ctx, "  %s = zext i1 %s to i32\n", res_name, comb);
			strcpy(result_buf, res_name);
			break;
		}

		if (expr->data.binary.op >= OP_EQ && expr->data.binary.op <= OP_GTE) {
			/* Comparison. In scalar context: emit scalar icmp/fcmp -> i1 -> zext to i32.
			 * In vector context: splat scalar operands to <N x T>, emit vector icmp/fcmp
			 * -> <N x i1>, then zext to <N x i32> so the result can feed downstream
			 * vector arithmetic (e.g. `col * (col > 0)`). */
			if (ctx->vector_lanes > 0) {
				int lanes = ctx->vector_lanes;
				const char *elem_t = is_float ? "double" : "i32";
				char vec_t[32], vec_i1_t[32], vec_i32_t[32];
				snprintf(vec_t, sizeof(vec_t), "<%d x %s>", lanes, elem_t);
				snprintf(vec_i1_t, sizeof(vec_i1_t), "<%d x i1>", lanes);
				snprintf(vec_i32_t, sizeof(vec_i32_t), "<%d x i32>", lanes);

				/* Splat scalar operands (literals) to vectors. A "scalar" here is a
				 * value whose name has no '%' prefix — i.e. a constant or global. */
				const char *lv = left_val;
				const char *rv = right_val;
				if (strchr(lv, '%') == NULL) {
					char *ins = gen_value_name(ctx);
					char *splat = gen_value_name(ctx);
					buffer_append_fmt(ctx, "  %s = insertelement %s undef, %s %s, i32 0\n", ins, vec_t, elem_t, lv);
					buffer_append_fmt(ctx, "  %s = shufflevector %s %s, %s undef, <%d x i32> zeroinitializer\n", splat,
					                  vec_t, ins, vec_t, lanes);
					lv = splat;
				}
				if (strchr(rv, '%') == NULL) {
					char *ins = gen_value_name(ctx);
					char *splat = gen_value_name(ctx);
					buffer_append_fmt(ctx, "  %s = insertelement %s undef, %s %s, i32 0\n", ins, vec_t, elem_t, rv);
					buffer_append_fmt(ctx, "  %s = shufflevector %s %s, %s undef, <%d x i32> zeroinitializer\n", splat,
					                  vec_t, ins, vec_t, lanes);
					rv = splat;
				}

				char *cmp_vi1 = gen_value_name(ctx);
				if (is_float) {
					buffer_append_fmt(ctx, "  %s = fcmp %s %s %s, %s\n", cmp_vi1, op, vec_t, lv, rv);
				} else {
					buffer_append_fmt(ctx, "  %s = icmp %s %s %s, %s\n", cmp_vi1, op, vec_t, lv, rv);
				}
				buffer_append_fmt(ctx, "  %s = zext %s %s to %s\n", res_name, vec_i1_t, cmp_vi1, vec_i32_t);
			} else {
				/* Scalar context */
				const char *cmp_type = is_float ? "double" : llvm_int_type(int_width);
				char *cmp_i1 = gen_value_name(ctx);
				if (is_float) {
					buffer_append_fmt(ctx, "  %s = fcmp %s %s %s, %s\n", cmp_i1, op, cmp_type, left_val, right_val);
				} else {
					buffer_append_fmt(ctx, "  %s = icmp %s %s %s, %s\n", cmp_i1, op, cmp_type, left_val, right_val);
				}
				buffer_append_fmt(ctx, "  %s = zext i1 %s to i32\n", res_name, cmp_i1);
			}
		} else {
			/* arithmetic: vectorized if ctx->vector_lanes > 0 */
			buffer_append_fmt(ctx, "  %s = %s %s %s, %s\n", res_name, op, type, left_val, right_val);
		}

		strcpy(result_buf, res_name);
		break;
	}

	case HIR_EXPR_UNARY: {
		char operand_buf[256];
		codegen_expression(ctx, expr->data.unary.operand, operand_buf);
		if (expr->data.unary.op == UNARY_MOVE) {
			strcpy(result_buf, operand_buf); /* `move x` is transparent — the value passes through */
			break;
		}
		if (expr->data.unary.op == UNARY_COPY) {
			/* `copy x`: duplicate a value buffer into a fresh stack buffer so this binding gets an
			 * owned copy and the source stays alive. Scalars pass through (they are values). */
			if (expr->data.unary.operand->kind == HIR_EXPR_NAME) {
				ValueInfo *ov = find_value(ctx, expr->data.unary.operand->data.name.name);
				if (ov && ov->type == 7) {
					/* char[N] stack buffer: `[N x i8]` clone. */
					int n = ov->string_len;
					char *dup = gen_value_name(ctx);
					emit_alloca(ctx, "  %s = alloca [%d x i8]\n", dup, n);
					char *src_i8 = gen_value_name(ctx);
					buffer_append_fmt(ctx, "  %s = bitcast [%d x i8]* %s to i8*\n", src_i8, n, operand_buf);
					char *dst_i8 = gen_value_name(ctx);
					buffer_append_fmt(ctx, "  %s = bitcast [%d x i8]* %s to i8*\n", dst_i8, n, dup);
					buffer_append_fmt(ctx, "  call void @llvm.memcpy.p0.p0.i64(i8* %s, i8* %s, i64 %d, i1 false)\n",
					                  dst_i8, src_i8, n);
					ctx->uses_memcpy = 1;
					strcpy(result_buf, dup);
					break;
				}
				if (ov && ov->type == 6 && ov->string_len > 0 && !ov->is_slice) {
					/* bounded non-char `T[N]` value: clone into a fresh `[N x T]` and hand back the
					 * element pointer (the bind registers it as an independent type-6 array). */
					int n = ov->string_len;
					const char *e = llvm_type_from_arche(ov->field_type ? ov->field_type : "int");
					int esz = strcmp(e, "double") == 0 ? 8
					          : strcmp(e, "i64") == 0  ? 8
					          : strcmp(e, "i16") == 0  ? 2
					          : strcmp(e, "i8") == 0   ? 1
					                                   : 4;
					char *dup = gen_value_name(ctx);
					emit_alloca(ctx, "  %s = alloca [%d x %s]\n", dup, n, e);
					char *dst_ep = gen_value_name(ctx);
					buffer_append_fmt(ctx, "  %s = getelementptr [%d x %s], [%d x %s]* %s, i64 0, i64 0\n", dst_ep, n,
					                  e, n, e, dup);
					char *src_i8 = gen_value_name(ctx);
					buffer_append_fmt(ctx, "  %s = bitcast %s* %s to i8*\n", src_i8, e, operand_buf);
					char *dst_i8 = gen_value_name(ctx);
					buffer_append_fmt(ctx, "  %s = bitcast %s* %s to i8*\n", dst_i8, e, dst_ep);
					buffer_append_fmt(ctx, "  call void @llvm.memcpy.p0.p0.i64(i8* %s, i8* %s, i64 %d, i1 false)\n",
					                  dst_i8, src_i8, n * esz);
					ctx->uses_memcpy = 1;
					strcpy(result_buf, dst_ep);
					break;
				}
			}
			strcpy(result_buf, operand_buf); /* transparent: scalar / pass-through */
			break;
		}

		char *res_name = gen_value_name(ctx);
		if (expr->data.unary.op == UNARY_NEG) {
			int is_float =
			    expr->resolved.tag == HIR_TYPE_FLOAT || expr->data.unary.operand->resolved.tag == HIR_TYPE_FLOAT;
			if (is_float)
				buffer_append_fmt(ctx, "  %s = fsub double 0.0, %s\n", res_name, operand_buf);
			else
				buffer_append_fmt(ctx, "  %s = sub i32 0, %s\n", res_name, operand_buf);
		} else if (expr->data.unary.op == UNARY_NOT) {
			/* `!x` ≡ `x == 0`: produces an arche int 0/1 (i32), like a comparison. Compare against 0
			 * at the OPERAND's real width (bool=i8, opaque cell=i64, …), not a hardcoded i32. */
			char *eq_i1 = gen_value_name(ctx);
			buffer_append_fmt(ctx, "  %s = icmp eq %s %s, 0\n", eq_i1, cond_int_type(expr->data.unary.operand),
			                  operand_buf);
			buffer_append_fmt(ctx, "  %s = zext i1 %s to i32\n", res_name, eq_i1);
		}
		strcpy(result_buf, res_name);
		break;
	}

	case HIR_EXPR_FIELD: {
		/* Compile-time scalars from a monomorphized archetype-parametric proc.
		 * Short-circuit before the normal field-access path. */
		if (expr->data.field.base->kind == HIR_EXPR_NAME && expr->data.field.field_name) {
			const char *base_name = expr->data.field.base->data.name.name;
			const char *fname = expr->data.field.field_name;
			if (ctx->current_archetype_param && ctx->current_arch_param_name &&
			    strcmp(base_name, ctx->current_arch_param_name) == 0) {
				if (strcmp(fname, "field_count") == 0) {
					int n = count_archetype_iterable_fields(ctx->current_archetype_param);
					snprintf(result_buf, 256, "%d", n);
					break;
				}
				if (strcmp(fname, "capacity") == 0) {
					int cap = get_arch_static_capacity(ctx, ctx->current_archetype_param->name);
					snprintf(result_buf, 256, "%d", cap);
					break;
				}
			}
			if (ctx->current_each_field_binding && ctx->current_each_field_target &&
			    strcmp(base_name, ctx->current_each_field_binding) == 0) {
				if (strcmp(fname, "index") == 0) {
					snprintf(result_buf, 256, "%d", ctx->current_each_field_index);
					break;
				}
				if (strcmp(fname, "name") == 0 && ctx->current_each_field_name_global) {
					/* Emit a bare i8* GEP to the field's name global. Callers
					 * passing this to printf get a regular C string. */
					int len = (int)strlen(ctx->current_each_field_target->name) + 1;
					char *ptr = gen_value_name(ctx);
					buffer_append_fmt(ctx, "  %s = getelementptr [%d x i8], [%d x i8]* %s, i32 0, i32 0\n", ptr, len,
					                  len, ctx->current_each_field_name_global);
					strcpy(result_buf, ptr);
					expr->resolved.tag = HIR_TYPE_CHAR_ARRAY;
					break;
				}
			}
		}

		/* fieldexpr like archetype.field */
		char base_buf[256];
		codegen_expression(ctx, expr->data.field.base, base_buf);

		const char *field_name = expr->data.field.field_name;

		/* Check if base is an arch pointer (type 3) or archetype name */
		ValueInfo *base_val = NULL;
		const char *arch_name_direct = NULL;
		if (expr->data.field.base->kind == HIR_EXPR_NAME) {
			const char *name = expr->data.field.base->data.name.name;
			base_val = find_value(ctx, name);
			if (!base_val && find_archetype_decl(ctx, name)) {
				arch_name_direct = name;
			}
		}

		/* Fixed char[N]/T[N] stack buffer (type 7): .cap/.capacity/.length/.max_length are the
		 * declared size (string_len) — the same bound buf[i] is bounds-checked against. A fixed
		 * array has no dynamic length, so length == capacity == N; content length is strlen(). */
		if (base_val && base_val->type == 7 &&
		    (strcmp(field_name, "cap") == 0 || strcmp(field_name, "capacity") == 0 ||
		     strcmp(field_name, "length") == 0 || strcmp(field_name, "max_length") == 0)) {
			snprintf(result_buf, 256, "%d", base_val->string_len);
			break;
		}

		/* Type-6 `T[]` slice: `.length` is the runtime len; `.cap`/`.capacity`/`.max_length` is the
		 * backing capacity (`cap_ssa`) — the heapless `{ptr,len,cap}` model. Both i64, trunc'd to the
		 * i32 the accessors return. A slice with no carried cap (e.g. a slice param — cap isn't
		 * threaded through the ABI yet) falls back to its length. */
		if (base_val && base_val->type == 6 && base_val->is_slice && base_val->len_ssa &&
		    (strcmp(field_name, "cap") == 0 || strcmp(field_name, "capacity") == 0 ||
		     strcmp(field_name, "length") == 0 || strcmp(field_name, "max_length") == 0)) {
			int want_cap = strcmp(field_name, "length") != 0;
			const char *src = (want_cap && base_val->cap_ssa) ? base_val->cap_ssa : base_val->len_ssa;
			char *t = gen_value_name(ctx);
			buffer_append_fmt(ctx, "  %s = trunc i64 %s to i32\n", t, src);
			snprintf(result_buf, 256, "%s", t);
			break;
		}

		/* Type-6 bounded array (non-char `int[N]`/`float[N]`/…): .cap/.length/etc. are the declared
		 * element count N (string_len) — the same constant bound its indexing is checked against. */
		if (base_val && base_val->type == 6 && base_val->string_len > 0 &&
		    (strcmp(field_name, "cap") == 0 || strcmp(field_name, "capacity") == 0 ||
		     strcmp(field_name, "length") == 0 || strcmp(field_name, "max_length") == 0)) {
			snprintf(result_buf, 256, "%d", base_val->string_len);
			break;
		}

		/* Handle .length property */
		if (strcmp(field_name, "length") == 0) {
			/* For archetype columns: load count field */
			if (base_val && base_val->type == 3 && base_val->arch_name) {
				/* count field is always the last field in archetype struct */
				HirArchetypeDecl *arch = find_archetype_decl(ctx, base_val->arch_name);
				if (arch) {
					int count_idx = arch->field_count; /* count field index */
					char *gep = gen_value_name(ctx);
					buffer_append_fmt(ctx, "  %s = getelementptr %%struct.%s, %%struct.%s* %s, i32 0, i32 %d\n", gep,
					                  base_val->arch_name, base_val->arch_name, base_buf, count_idx);
					char *count = gen_value_name(ctx);
					buffer_append_fmt(ctx, "  %s = load i64, i64* %s\n", count, gep);
					strcpy(result_buf, count);
					break;
				}
			}

			/* For arche_array: load length or max_length field */
			if (base_val && base_val->type == 5) {
				int field_idx = (strcmp(field_name, "max_length") == 0) ? 2 : 1;
				char *gep = gen_value_name(ctx);
				buffer_append_fmt(
				    ctx, "  %s = getelementptr %%struct.arche_array, %%struct.arche_array* %s, i32 0, i32 %d\n", gep,
				    base_buf, field_idx);
				char *loaded = gen_value_name(ctx);
				buffer_append_fmt(ctx, "  %s = load i64, i64* %s\n", loaded, gep);
				char *truncated = gen_value_name(ctx);
				buffer_append_fmt(ctx, "  %s = trunc i64 %s to i32\n", truncated, loaded);
				strcpy(result_buf, truncated);
				break;
			}

			/* For archetype: max_length is the capacity field */
			if (strcmp(field_name, "max_length") == 0 && base_val && base_val->type == 3 && base_val->arch_name) {
				HirArchetypeDecl *arch = find_archetype_decl(ctx, base_val->arch_name);
				if (arch) {
					int cap_idx = arch->field_count + 1;
					char *gep = gen_value_name(ctx);
					buffer_append_fmt(ctx, "  %s = getelementptr %%struct.%s, %%struct.%s* %s, i32 0, i32 %d\n", gep,
					                  base_val->arch_name, base_val->arch_name, base_buf, cap_idx);
					char *cap = gen_value_name(ctx);
					buffer_append_fmt(ctx, "  %s = load i64, i64* %s\n", cap, gep);
					strcpy(result_buf, cap);
					break;
				}
			}
		}

		if (base_val && base_val->type == 3 && base_val->arch_name) {
			/* Find field index in archetype */
			HirArchetypeDecl *arch = find_archetype_decl(ctx, base_val->arch_name);
			if (arch) {
				int field_idx = -1;
				HirField *fdecl = NULL;
				for (int i = 0; i < arch->field_count; i++) {
					if (strcmp(arch->fields[i]->name, field_name) == 0) {
						field_idx = i;
						fdecl = arch->fields[i];
						break;
					}
				}

				if (field_idx >= 0 && fdecl) {
					const char *llvm_type = llvm_type_from_arche(field_base_type_name(fdecl->type));
					int is_static = get_arch_static_capacity(ctx, base_val->arch_name) > 0;

					if (fdecl->kind == FIELD_META) {
						char *gep = gen_value_name(ctx);
						buffer_append_fmt(ctx, "  %s = getelementptr %%struct.%s, %%struct.%s* %s, i32 0, i32 %d\n",
						                  gep, base_val->arch_name, base_val->arch_name, base_buf, field_idx);
						char *loaded = gen_value_name(ctx);
						buffer_append_fmt(ctx, "  %s = load %s, %s* %s\n", loaded, llvm_type, llvm_type, gep);
						strcpy(result_buf, loaded);
					} else {
						/* Col field */
						char *ptr_val = gen_value_name(ctx);
						if (is_static) {
							buffer_append_fmt(
							    ctx, "  %s = getelementptr %%struct.%s, %%struct.%s* %s, i32 0, i32 %d, i64 0\n",
							    ptr_val, base_val->arch_name, base_val->arch_name, base_buf, field_idx);
						} else {
							char *gep = gen_value_name(ctx);
							buffer_append_fmt(ctx, "  %s = getelementptr %%struct.%s, %%struct.%s* %s, i32 0, i32 %d\n",
							                  gep, base_val->arch_name, base_val->arch_name, base_buf, field_idx);
							buffer_append_fmt(ctx, "  %s = load %s*, %s** %s\n", ptr_val, llvm_type, llvm_type, gep);
						}

						if (ctx->implicit_loop_index[0]) {
							const char *load_type = elem_llvm_type(ctx, field_base_type_name(fdecl->type));
							char *idx_gep = gen_value_name(ctx);
							buffer_append_fmt(ctx, "  %s = getelementptr %s, %s* %s, i64 %s\n", idx_gep, llvm_type,
							                  llvm_type, ptr_val, ctx->implicit_loop_index);
							char *elem = gen_value_name(ctx);
							int align = ctx->vector_lanes > 0 ? 8 : 4;

							if (ctx->vector_lanes > 0) {
								char *vec_ptr = gen_value_name(ctx);
								buffer_append_fmt(ctx, "  %s = bitcast %s* %s to %s*\n", vec_ptr, llvm_type, idx_gep,
								                  load_type);
								buffer_append_fmt(ctx, "  %s = load %s, %s* %s, align %d\n", elem, load_type, load_type,
								                  vec_ptr, align);
							} else {
								buffer_append_fmt(ctx, "  %s = load %s, %s* %s, align %d\n", elem, load_type, llvm_type,
								                  idx_gep, align);
							}
							strcpy(result_buf, elem);
						} else {
							strcpy(result_buf, ptr_val);
						}
					}
					break;
				}
			}
		} else if (arch_name_direct) {
			/* Direct archetype name reference */
			HirArchetypeDecl *arch = find_archetype_decl(ctx, arch_name_direct);
			if (arch) {
				int field_idx = -1;
				HirField *fdecl = NULL;
				for (int i = 0; i < arch->field_count; i++) {
					if (strcmp(arch->fields[i]->name, field_name) == 0) {
						field_idx = i;
						fdecl = arch->fields[i];
						break;
					}
				}

				if (field_idx >= 0 && fdecl) {
					const char *llvm_type = llvm_type_from_arche(field_base_type_name(fdecl->type));
					int is_static = get_arch_static_capacity(ctx, arch_name_direct) > 0;

					if (fdecl->kind == FIELD_META) {
						char *gep = gen_value_name(ctx);
						buffer_append_fmt(ctx, "  %s = getelementptr %%struct.%s, %%struct.%s* %s, i32 0, i32 %d\n",
						                  gep, arch_name_direct, arch_name_direct, base_buf, field_idx);
						char *loaded = gen_value_name(ctx);
						buffer_append_fmt(ctx, "  %s = load %s, %s* %s\n", loaded, llvm_type, llvm_type, gep);
						strcpy(result_buf, loaded);
					} else {
						/* Col field */
						char *ptr_val = gen_value_name(ctx);
						if (is_static) {
							buffer_append_fmt(
							    ctx, "  %s = getelementptr %%struct.%s, %%struct.%s* %s, i32 0, i32 %d, i64 0\n",
							    ptr_val, arch_name_direct, arch_name_direct, base_buf, field_idx);
						} else {
							char *gep = gen_value_name(ctx);
							buffer_append_fmt(ctx, "  %s = getelementptr %%struct.%s, %%struct.%s* %s, i32 0, i32 %d\n",
							                  gep, arch_name_direct, arch_name_direct, base_buf, field_idx);
							buffer_append_fmt(ctx, "  %s = load %s*, %s** %s\n", ptr_val, llvm_type, llvm_type, gep);
						}

						if (ctx->implicit_loop_index[0]) {
							const char *load_type = elem_llvm_type(ctx, field_base_type_name(fdecl->type));
							char *idx_gep = gen_value_name(ctx);
							buffer_append_fmt(ctx, "  %s = getelementptr %s, %s* %s, i64 %s\n", idx_gep, llvm_type,
							                  llvm_type, ptr_val, ctx->implicit_loop_index);
							char *elem = gen_value_name(ctx);
							int align = ctx->vector_lanes > 0 ? 8 : 4;

							if (ctx->vector_lanes > 0) {
								char *vec_ptr = gen_value_name(ctx);
								buffer_append_fmt(ctx, "  %s = bitcast %s* %s to %s*\n", vec_ptr, llvm_type, idx_gep,
								                  load_type);
								buffer_append_fmt(ctx, "  %s = load %s, %s* %s, align %d\n", elem, load_type, load_type,
								                  vec_ptr, align);
							} else {
								buffer_append_fmt(ctx, "  %s = load %s, %s* %s, align %d\n", elem, load_type, llvm_type,
								                  idx_gep, align);
							}
							strcpy(result_buf, elem);
						} else {
							strcpy(result_buf, ptr_val);
						}
					}
					break;
				}
			}
		}

		/* Fallback: just return field name (shouldn't reach here) */
		strcpy(result_buf, field_name);
		break;
	}

	case HIR_EXPR_SLICE: {
		/* `buf[lo:hi]` as a bare expression value: emit the sub-view and yield its element pointer.
		 * (Call args and binds use codegen_slice directly to also carry the runtime length.) */
		char sp[256], sl[256], scp[256];
		const char *se;
		int sw;
		if (codegen_slice(ctx, expr, sp, sl, scp, &se, &sw)) {
			(void)sl;
			(void)scp;
			strcpy(result_buf, sp);
		} else {
			strcpy(result_buf, "null");
		}
		break;
	}

	case HIR_EXPR_INDEX: {
		/* Each-field column read: f[i] where f is the current each_field
		 * binding. Resolve to a load from the matching archetype column. */
		if (ctx->current_each_field_binding && ctx->current_each_field_target && ctx->current_archetype_param &&
		    ctx->current_arch_param_llvm && expr->data.index.base->kind == HIR_EXPR_NAME &&
		    strcmp(expr->data.index.base->data.name.name, ctx->current_each_field_binding) == 0 &&
		    expr->data.index.index_count == 1) {
			char idx_local[256];
			codegen_expression(ctx, expr->data.index.indices[0], idx_local);
			int cap = get_arch_static_capacity(ctx, ctx->current_archetype_param->name);
			if (cap <= 0)
				cap = 1;
			const char *base_elem = field_base_type_name(ctx->current_each_field_target->type);
			const char *llvm_elem = llvm_type_from_arche(base_elem);
			char *col_gep = gen_value_name(ctx);
			buffer_append_fmt(ctx, "  %s = getelementptr %%struct.%s, %%struct.%s* %s, i32 0, i32 %d\n", col_gep,
			                  ctx->current_archetype_param->name, ctx->current_archetype_param->name,
			                  ctx->current_arch_param_llvm, ctx->current_each_field_index);
			char idx64[256];
			emit_index_i64(ctx, idx_local, expr->data.index.indices[0], idx64);
			char *elem_gep = gen_value_name(ctx);
			buffer_append_fmt(ctx, "  %s = getelementptr [%d x %s], [%d x %s]* %s, i64 0, i64 %s\n", elem_gep, cap,
			                  llvm_elem, cap, llvm_elem, col_gep, idx64);
			char *val = gen_value_name(ctx);
			buffer_append_fmt(ctx, "  %s = load %s, %s* %s\n", val, llvm_elem, llvm_elem, elem_gep);
			strcpy(result_buf, val);
			/* Tag the expression with its resolved type (tag + int width/sign) so
			 * callers (e.g. group resolution) pick the right overload. */
			{
				HirType *ft = ctx->current_each_field_target->type;
				while (ft && ft->tag == HIR_TYPE_SHAPED_ARRAY)
					ft = ft->elem;
				expr->resolved.tag = ft ? ft->tag : HIR_TYPE_INT;
				if (ft && ft->tag == HIR_TYPE_INT) {
					expr->resolved.int_width = ft->int_width ? ft->int_width : 32;
					expr->resolved.int_signed = ft->int_signed;
				}
			}
			break;
		}

		/* index access: array[index] or field[index] */
		char base_buf[256], idx_buf[256];
		codegen_expression(ctx, expr->data.index.base, base_buf);

		/* Check if base is a type-6 slice pointer variable */
		const char *type6_elem_type = NULL;
		int type6_bound = 0; /* compile-time element count N for a bounded array (0 = unbounded slice) */
		if (expr->data.index.base->kind == HIR_EXPR_NAME) {
			ValueInfo *vi = find_value(ctx, expr->data.index.base->data.name.name);
			if (vi && vi->type == 6 && vi->field_type) {
				type6_elem_type = vi->field_type;
				type6_bound = vi->string_len;
			}
		}

		int shaped_rank = 1;
		const char *shaped_elem = (expr->data.index.index_count >= 1)
		                              ? get_shaped_field_info(ctx, expr->data.index.base, &shaped_rank)
		                              : NULL;

		if (expr->data.index.index_count == 2 && shaped_elem) {
			/* 2D: [entity, elem] → flat = entity * rank + elem */
			char row_raw[256], elem_raw[256], row_buf[256], elem_buf[256];
			codegen_expression(ctx, expr->data.index.indices[0], row_raw);
			codegen_expression(ctx, expr->data.index.indices[1], elem_raw);
			/* Coerce both indices to i64 — the flat-index math is i64 (a narrower
			 * loop var like i32 would otherwise mismatch the `mul i64`). */
			emit_index_i64(ctx, row_raw, expr->data.index.indices[0], row_buf);
			emit_index_i64(ctx, elem_raw, expr->data.index.indices[1], elem_buf);
			char *scaled = gen_value_name(ctx);
			buffer_append_fmt(ctx, "  %s = mul i64 %s, %d\n", scaled, row_buf, shaped_rank);
			char *flat = gen_value_name(ctx);
			buffer_append_fmt(ctx, "  %s = add i64 %s, %s\n", flat, scaled, elem_buf);
			strcpy(idx_buf, flat);
		} else if (expr->data.index.index_count == 1 && shaped_elem) {
			/* Single-index on shaped array: entity * rank → returns slice pointer, no load */
			char row_raw[256], row_buf[256];
			codegen_expression(ctx, expr->data.index.indices[0], row_raw);
			emit_index_i64(ctx, row_raw, expr->data.index.indices[0], row_buf);
			if (shaped_rank > 1) {
				char *scaled = gen_value_name(ctx);
				buffer_append_fmt(ctx, "  %s = mul i64 %s, %d\n", scaled, row_buf, shaped_rank);
				strcpy(idx_buf, scaled);
			} else {
				strcpy(idx_buf, row_buf);
			}
		} else if (expr->data.index.index_count > 0) {
			codegen_expression(ctx, expr->data.index.indices[0], idx_buf);
		}

		/* Determine element type from semantic analysis */
		const char *scalar_type = "i32"; /* default */
		const char *arche_type = NULL;

		if (type6_elem_type) {
			arche_type = type6_elem_type;
			scalar_type = llvm_type_from_arche(type6_elem_type);
		} else if (shaped_elem) {
			arche_type = shaped_elem;
			scalar_type = llvm_type_from_arche(shaped_elem);
		} else if (expr->resolved.tag != HIR_TYPE_UNKNOWN) {
			arche_type = hir_resolved_type_name(expr);
			scalar_type = llvm_type_from_arche(arche_type);
		} else if (expr->data.index.base->resolved.tag != HIR_TYPE_UNKNOWN) {
			/* Use base's resolved type (e.g., for pos[i] where pos is double*) */
			arche_type = hir_resolved_type_name(expr->data.index.base);
			scalar_type = llvm_type_from_arche(arche_type);
		}

		/* In vector mode, load uses vector type; GEP uses scalar pointer */
		const char *load_type = arche_type ? elem_llvm_type(ctx, arche_type) : scalar_type;

		/* Bounds check for archetype column accesses (elided when statically provable). */
		const char *bc_arch_name = NULL, *bc_arch_ptr = NULL;
		int bc_count_idx = -1, bc_idx_is_i64 = 0;
		if (expr->data.index.index_count > 0 && !shaped_elem &&
		    resolve_index_arch(ctx, expr->data.index.base, expr->data.index.indices[0], &bc_arch_name, &bc_arch_ptr,
		                       &bc_count_idx, &bc_idx_is_i64) &&
		    !bounds_check_elidable(ctx, bc_arch_name, expr->data.index.indices[0])) {
			emit_bounds_check(ctx, bc_arch_name, bc_arch_ptr, bc_count_idx, idx_buf, bc_idx_is_i64);
		}

		/* Ensure index is i64 for getelementptr, respecting the index's width. */
		const char *final_idx = idx_buf;
		char final_idx_buf[256];

		if (expr->data.index.index_count > 0 && !shaped_elem) {
			emit_index_i64(ctx, idx_buf, expr->data.index.indices[0], final_idx_buf);
			final_idx = final_idx_buf;
		}

		/* Bounds check a type-6 bounded array read against its compile-time count N (final_idx is
		 * already i64 here), unless the index is provably in range. */
		if (type6_bound > 0 && expr->data.index.index_count > 0 &&
		    !const_index_in_range(ctx, expr->data.index.indices[0], type6_bound))
			emit_const_bounds_check(ctx, type6_bound, final_idx, 1);

		char *res_name = gen_value_name(ctx);
		buffer_append_fmt(ctx, "  %s = getelementptr %s, %s* %s, i64 %s\n", res_name, scalar_type, scalar_type,
		                  base_buf, final_idx);

		if (expr->data.index.index_count == 1 && shaped_elem) {
			/* Single-index on shaped field → return pointer slice, not loaded value */
			strcpy(result_buf, res_name);
		} else {
			char *loaded = gen_value_name(ctx);
			int align = ctx->vector_lanes > 0 ? 8 : 4;

			if (ctx->vector_lanes > 0 && arche_type) {
				/* Vector load: bitcast pointer to vector type, then load */
				char *vec_ptr = gen_value_name(ctx);
				buffer_append_fmt(ctx, "  %s = bitcast %s* %s to %s*\n", vec_ptr, scalar_type, res_name, load_type);
				buffer_append_fmt(ctx, "  %s = load %s, %s* %s, align %d\n", loaded, load_type, load_type, vec_ptr,
				                  align);
			} else {
				/* Scalar load */
				buffer_append_fmt(ctx, "  %s = load %s, %s* %s, align %d\n", loaded, load_type, scalar_type, res_name,
				                  align);
			}

			/* Zero-extend an i8 element load to i32, matching the type-5 (arche_array) and
			 * type-7 (char[N]) paths which always promote: a char/byte element acts as an i32 in
			 * int contexts (printf %d, compares, arithmetic), and a store back to an i8 target
			 * truncs as needed. Without this a local string-literal index (`s := "hi"; s[0]`, a
			 * raw i8* on this general path) stayed i8 and failed the verifier. */
			if (scalar_type && strcmp(scalar_type, "i8") == 0) {
				char *extended = gen_value_name(ctx);
				buffer_append_fmt(ctx, "  %s = zext i8 %s to i32\n", extended, loaded);
				strcpy(result_buf, extended);
			} else {
				strcpy(result_buf, loaded);
			}
		}
		break;
	}

	case HIR_EXPR_CALL: {
		/* function call */
		char *func_name = NULL;
		if (expr->data.call.callee->kind == HIR_EXPR_NAME) {
			func_name = expr->data.call.callee->data.name.name;
			/* Inside a callback specialization, a call to a callback param resolves to
			 * its bound proc/func (direct call). No-op (idempotent) outside one. */
			func_name = (char *)cb_resolve(ctx, func_name);
		}

		/* Width-type cast i64(x)/u8(x)/...: convert the single arg to the target
		 * width (sext/zext/trunc) instead of emitting a call. */
		{
			int cw, csg;
			if (func_name && expr->data.call.arg_count == 1 && hir_parse_int_width(func_name, &cw, &csg)) {
				char arg_buf[256];
				codegen_expression(ctx, expr->data.call.args[0], arg_buf);
				char converted[256];
				emit_int_convert(ctx, arg_buf, &expr->data.call.args[0]->resolved, cw, converted);
				strcpy(result_buf, converted);
				break;
			}
		}

		/* Type conversion to a non-width type: float(x), or a nominal alias/subtype conversion
		 * meters(x)/mps(x). The callee names a type (no proc/func by that name); convert the single
		 * arg to the call's resolved backing — a no-op when the backings already match (the common
		 * same-backing case), else a numeric widen/narrow. */
		{
			int is_prim = func_name && (strcmp(func_name, "int") == 0 || strcmp(func_name, "float") == 0 ||
			                            strcmp(func_name, "char") == 0 || strcmp(func_name, "str") == 0);
			int is_alias = func_name && ctx->sem_ctx && semantic_is_type_alias(ctx->sem_ctx, func_name);
			if (func_name && expr->data.call.arg_count == 1 && (is_prim || is_alias) &&
			    !find_proc_decl(ctx, func_name)) {
				char arg_buf[256];
				codegen_expression(ctx, expr->data.call.args[0], arg_buf);
				HirType *from = &expr->data.call.args[0]->resolved;
				HirType *to = &expr->resolved;
				if (to->tag == HIR_TYPE_FLOAT && from->tag != HIR_TYPE_FLOAT) {
					char *c = gen_value_name(ctx);
					buffer_append_fmt(ctx, "  %s = sitofp i32 %s to double\n", c, arg_buf);
					strcpy(result_buf, c);
				} else if (to->tag == HIR_TYPE_INT && from->tag == HIR_TYPE_FLOAT) {
					char *c = gen_value_name(ctx);
					int tw = to->int_width ? to->int_width : 32;
					buffer_append_fmt(ctx, "  %s = fptosi double %s to %s\n", c, arg_buf, llvm_int_type(tw));
					strcpy(result_buf, c);
				} else if (to->tag == HIR_TYPE_INT && from->tag == HIR_TYPE_INT) {
					char converted[256];
					emit_int_convert(ctx, arg_buf, from, to->int_width ? to->int_width : 32, converted);
					strcpy(result_buf, converted);
				} else {
					strcpy(result_buf, arg_buf); /* same backing (incl. float→float, opaque) — identity */
				}
				break;
			}
		}

		/* Raw Linux/x86-64 syscall intrinsic: syscall(n, a0..a5) -> i64.
		 * Emits the `syscall` instruction directly (no libc, no C shim): number in
		 * rax, args in rdi/rsi/rdx/r10/r8/r9, result in rax; rcx/r11/memory clobbered.
		 * Each argument is coerced to i64 (sext/zext from narrower ints). Recognized by the
		 * resolved callee's `@intrinsic` flag — NOT its (module-mangled) name — so it keeps
		 * working wherever the decl lives (`os.syscall` → `os_syscall`, etc.). */
		HirProcDecl *intrinsic_decl =
		    (func_name && expr->data.call.arg_count == 7) ? find_proc_decl(ctx, func_name) : NULL;
		if (intrinsic_decl && intrinsic_decl->is_intrinsic) {
			char a[7][256];
			for (int i = 0; i < 7; i++) {
				char ab[256];
				codegen_expression(ctx, expr->data.call.args[i], ab);
				HirType *rt = &expr->data.call.args[i]->resolved;
				/* A buffer arg decays to its data pointer (like C): extract the i8* and `ptrtoint`
				 * it to i64 so a buffer can be handed to a raw syscall. The arg's LLVM repr is
				 * known from its ValueInfo type: 5 = %struct.arche_array* (static buffer),
				 * 7 = [N x i8]* (local char buffer), 2 = i8* (string), 6 = i8* (char[] param,
				 * data ptr pre-extracted at entry). */
				ValueInfo *avi = (expr->data.call.args[i]->kind == HIR_EXPR_NAME)
				                     ? find_value(ctx, expr->data.call.args[i]->data.name.name)
				                     : NULL;
				if (avi && (avi->type == 5 || avi->type == 7 || avi->type == 2 || avi->type == 6)) {
					char dptr[256];
					if (avi->type == 5) {
						char *g = gen_value_name(ctx);
						buffer_append_fmt(
						    ctx, "  %s = getelementptr %%struct.arche_array, %%struct.arche_array* %s, i32 0, i32 0\n",
						    g, ab);
						char *l = gen_value_name(ctx);
						buffer_append_fmt(ctx, "  %s = load i8*, i8** %s\n", l, g);
						strcpy(dptr, l);
					} else if (avi->type == 7) {
						char *b = gen_value_name(ctx);
						buffer_append_fmt(ctx, "  %s = bitcast [%d x i8]* %s to i8*\n", b, avi->string_len, ab);
						strcpy(dptr, b);
					} else {
						strcpy(dptr, ab); /* type 2: already i8* */
					}
					char *pi = gen_value_name(ctx);
					buffer_append_fmt(ctx, "  %s = ptrtoint i8* %s to i64\n", pi, dptr);
					strcpy(a[i], pi);
					continue;
				}
				if (rt->tag == HIR_TYPE_INT && rt->int_width == 64) {
					strcpy(a[i], ab);
				} else if (rt->tag == HIR_TYPE_INT) {
					emit_int_convert(ctx, ab, rt, 64, a[i]);
				} else if (rt->tag == HIR_TYPE_OPAQUE || rt->tag == HIR_TYPE_HANDLE) {
					strcpy(a[i], ab); /* opaque cell / handle is already pointer-width i64 */
				} else {
					HirType t32 = {0};
					t32.tag = HIR_TYPE_INT;
					t32.int_width = 32;
					t32.int_signed = 1;
					emit_int_convert(ctx, ab, &t32, 64, a[i]);
				}
			}
			char *res = gen_value_name(ctx);
			buffer_append_fmt(ctx,
			                  "  %s = call i64 asm sideeffect \"syscall\", "
			                  "\"={rax},{rax},{rdi},{rsi},{rdx},{r10},{r8},{r9},~{rcx},~{r11},~{memory}\""
			                  "(i64 %s, i64 %s, i64 %s, i64 %s, i64 %s, i64 %s, i64 %s)\n",
			                  res, a[0], a[1], a[2], a[3], a[4], a[5], a[6]);
			strcpy(result_buf, res);
			break;
		}

		/* Special handling for insert builtin */
		if (func_name && strcmp(func_name, "insert") == 0 && expr->data.call.arg_count > 0) {
			/* RAII: insert moves its value args into the pool — suppress auto-drop for any
			 * opaque local handed in (arg 0 is the archetype name; skip it). */
			for (int ai = 1; ai < expr->data.call.arg_count; ai++) {
				HirExpr *ia = expr->data.call.args[ai];
				if (ia && ia->kind == HIR_EXPR_UNARY && ia->data.unary.op == UNARY_MOVE)
					ia = ia->data.unary.operand;
				if (ia && ia->kind == HIR_EXPR_NAME)
					drop_mark_consumed(ctx, ia->data.name.name);
			}
			/* args[0] is archetype variable, args[1..] are field values */
			char arch_buf[256];
			codegen_expression(ctx, expr->data.call.args[0], arch_buf);

			/* Get arch_name from ValueInfo or archetype name of args[0] */
			const char *arch_name = NULL;
			HirArchetypeDecl *arch = NULL;
			if (expr->data.call.args[0]->kind == HIR_EXPR_NAME) {
				const char *name = expr->data.call.args[0]->data.name.name;
				ValueInfo *arch_var = find_value(ctx, name);
				if (arch_var && arch_var->arch_name) {
					arch_name = arch_var->arch_name;
					arch = find_archetype_decl(ctx, arch_name);
				} else if (find_archetype_decl(ctx, name)) {
					/* Direct archetype name */
					arch_name = name;
					arch = find_archetype_decl(ctx, arch_name);
				}
			}

			if (arch) {
				arch_name = arch->name; /* canonical shape name backs @arche_insert_/%struct. */
			}
			if (arch_name && arch) {
				/* Evaluate all field arguments first */
				char field_bufs[32][256];
				int field_idx = 0;
				int arg_count = 0;
				for (int i = 1; i < expr->data.call.arg_count && arg_count < 32; i++) {
					while (field_idx < arch->field_count && arch->fields[field_idx]->kind != FIELD_COLUMN) {
						field_idx++;
					}
					if (field_idx < arch->field_count) {
						codegen_expression(ctx, expr->data.call.args[i], field_bufs[arg_count]);
						arg_count++;
						field_idx++;
					}
				}

				/* Now emit the call with all arguments */
				char *handle_tmp = gen_value_name(ctx);
				buffer_append_fmt(ctx, "  %s = call i64 @arche_insert_%s(%%struct.%s* %s", handle_tmp, arch_name,
				                  arch_name, arch_buf);

				field_idx = 0;
				for (int i = 0; i < arg_count; i++) {
					while (field_idx < arch->field_count && arch->fields[field_idx]->kind != FIELD_COLUMN) {
						field_idx++;
					}
					if (field_idx < arch->field_count) {
						const char *field_type =
						    llvm_type_from_arche(field_base_type_name(arch->fields[field_idx]->type));
						/* char[N] column: the arg is a string/buffer pointer (memcpy'd whole),
						 * so pass it as a pointer. Numeric array columns keep scalar element-0
						 * init (legacy semantics, e.g. float[10] from a single value). */
						if (field_total_elements(arch->fields[field_idx]->type) > 1 && strcmp(field_type, "i8") == 0) {
							buffer_append_fmt(ctx, ", %s* %s", field_type, field_bufs[i]);
						} else {
							buffer_append_fmt(ctx, ", %s %s", field_type, field_bufs[i]);
						}
						field_idx++;
					}
				}
				buffer_append(ctx, ")\n");
				strcpy(result_buf, handle_tmp);
			}
			break;
		}

		/* Special handling for delete builtin */
		if (func_name && strcmp(func_name, "delete") == 0 && expr->data.call.arg_count >= 1) {
			/* delete(h) infers the pool from the handle's archetype; legacy
			 * delete(table<X>, h) names the target explicitly. */
			char arch_buf[256];
			char idx_buf[256];
			arch_buf[0] = '\0';
			idx_buf[0] = '\0';
			const char *arch_name = NULL;
			if (expr->data.call.arg_count >= 2) {
				codegen_expression(ctx, expr->data.call.args[0], arch_buf);
				codegen_expression(ctx, expr->data.call.args[1], idx_buf);
				if (expr->data.call.args[0]->kind == HIR_EXPR_NAME) {
					const char *name = expr->data.call.args[0]->data.name.name;
					ValueInfo *arch_var = find_value(ctx, name);
					if (arch_var && arch_var->arch_name)
						arch_name = arch_var->arch_name;
					else if (find_archetype_decl(ctx, name))
						arch_name = name;
				}
				if (expr->data.call.args[1]->kind == HIR_EXPR_NAME) {
					ValueInfo *hvi = find_value(ctx, expr->data.call.args[1]->data.name.name);
					if (hvi && hvi->field_type && strcmp(hvi->field_type, "handle") == 0 && hvi->handle_archetype &&
					    arch_name && strcmp(hvi->handle_archetype, arch_name) != 0) {
						fprintf(stderr, "Error: type mismatch in delete: handle for %s cannot delete %s\n",
						        hvi->handle_archetype, arch_name);
						strcpy(result_buf, "0");
						break;
					}
				}
			} else {
				codegen_expression(ctx, expr->data.call.args[0], idx_buf);
				if (expr->data.call.args[0]->kind == HIR_EXPR_NAME) {
					ValueInfo *hv = find_value(ctx, expr->data.call.args[0]->data.name.name);
					if (hv && hv->handle_archetype)
						arch_name = hv->handle_archetype;
				}
				if (arch_name) {
					arch_name = canonical_arch_name(ctx, arch_name);
					snprintf(arch_buf, sizeof(arch_buf), "@%s", arch_name);
				}
			}
			if (arch_name) {
				const char *cn = canonical_arch_name(ctx, arch_name);
				buffer_append_fmt(ctx, "  call void @arche_delete_%s(%%struct.%s* %s, i64 %s)\n", cn, cn, arch_buf,
				                  idx_buf);
				strcpy(result_buf, "0");
			}
			break;
		}

		/* Special handling for dealloc builtin */
		if (func_name && strcmp(func_name, "dealloc") == 0 && expr->data.call.arg_count >= 1) {
			/* args[0] is archetype variable to deallocate */
			char arch_buf[256];
			codegen_expression(ctx, expr->data.call.args[0], arch_buf);

			/* Get arch_name from ValueInfo or archetype name of args[0] */
			const char *arch_name = NULL;
			if (expr->data.call.args[0]->kind == HIR_EXPR_NAME) {
				const char *name = expr->data.call.args[0]->data.name.name;
				ValueInfo *arch_var = find_value(ctx, name);
				if (arch_var && arch_var->arch_name) {
					arch_name = arch_var->arch_name;
				} else if (find_archetype_decl(ctx, name)) {
					/* Direct archetype name */
					arch_name = name;
				}
			}

			if (arch_name) {
				const char *cn = canonical_arch_name(ctx, arch_name);
				buffer_append_fmt(ctx, "  call void @arche_dealloc_%s(%%struct.%s* %s)\n", cn, cn, arch_buf);
				strcpy(result_buf, "0");
			}
			break;
		}

		/* Evaluate arguments and track their ValueInfo types */
		char **arg_bufs = malloc(expr->data.call.arg_count * sizeof(char *));
		int *arg_is_string = malloc(expr->data.call.arg_count * sizeof(int));
		int *arg_is_array_literal = malloc(expr->data.call.arg_count * sizeof(int));
		ValueInfo **arg_values = malloc(expr->data.call.arg_count * sizeof(ValueInfo *));
		/* `move x` at a call site = by-reference (no copy). A plain arg is copied (below),
		 * so a function has no side effects on its caller's data unless `move`d in. The
		 * move marker is transparent in codegen, so unwrap it to classify the operand. */
		int *arg_is_move = malloc(expr->data.call.arg_count * sizeof(int));
		/* A callback-parametric callee takes proc/func-typed params: the matching
		 * args are bare proc/func names with no runtime value (monomorphized away),
		 * so emit no code for them. */
		int *arg_is_callback = malloc(expr->data.call.arg_count * sizeof(int));
		/* For a `buf[lo:hi]` slice argument: a temp type-6 slice ValueInfo (ptr + runtime len) so the
		 * normal slice-arg dispatch handles it like any slice variable. Owned here, freed at cleanup. */
		ValueInfo **arg_slice_vi = malloc(expr->data.call.arg_count * sizeof(ValueInfo *));
		for (int i = 0; i < expr->data.call.arg_count; i++)
			arg_slice_vi[i] = NULL;
		for (int i = 0; i < expr->data.call.arg_count; i++)
			arg_is_callback[i] = 0;
		{
			HirProcDecl *cb_cal = func_name ? find_proc_decl(ctx, func_name) : NULL;
			if (cb_cal && has_callback_param(ctx, cb_cal)) {
				for (int i = 0; i < cb_cal->param_count && i < expr->data.call.arg_count; i++)
					if (is_callback_param(ctx, cb_cal->params[i]))
						arg_is_callback[i] = 1;
			}
		}
		for (int i = 0; i < expr->data.call.arg_count; i++) {
			arg_bufs[i] = malloc(256);
			arg_is_string[i] = 0;
			arg_is_array_literal[i] = 0;
			arg_values[i] = NULL;
			arg_is_move[i] = 0;

			HirExpr *inner = expr->data.call.args[i];
			if (inner->kind == HIR_EXPR_UNARY &&
			    (inner->data.unary.op == UNARY_MOVE || inner->data.unary.op == UNARY_COPY)) {
				/* `move` is by-ref (transparent); `copy` already duplicated in codegen_expression.
				 * Either way, unwrap to the operand to classify its type/size. */
				if (inner->data.unary.op == UNARY_MOVE)
					arg_is_move[i] = 1;
				inner = inner->data.unary.operand;
				/* RAII: `move x` into an own param transfers ownership — suppress x's auto-drop.
				 * This covers explicit early close (`arche_fclose(move f)`): the new owner (the
				 * destructor itself, or any own-param callee) is responsible. */
				if (arg_is_move[i] && inner->kind == HIR_EXPR_NAME)
					drop_mark_consumed(ctx, inner->data.name.name);
			}

			/* Check if this arg is a string literal */
			if (inner->kind == HIR_EXPR_STRING) {
				arg_is_string[i] = 1;
			}
			/* Check if this arg is an old-style string literal (HIR_EXPR_LITERAL with quotes) */
			else if (inner->kind == HIR_EXPR_LITERAL && inner->data.literal.lexeme[0] == '"') {
				arg_is_string[i] = 1;
			}
			/* Check if this arg is an array literal */
			else if (inner->kind == HIR_EXPR_ARRAY_LITERAL) {
				arg_is_array_literal[i] = 1;
			}
			/* `f.name` inside an each_field expansion: produces a raw i8* string. */
			else if (ctx->current_each_field_binding && inner->kind == HIR_EXPR_FIELD &&
			         inner->data.field.base->kind == HIR_EXPR_NAME &&
			         strcmp(inner->data.field.base->data.name.name, ctx->current_each_field_binding) == 0 &&
			         inner->data.field.field_name && strcmp(inner->data.field.field_name, "name") == 0) {
				arg_is_string[i] = 1;
			}

			if (arg_is_callback[i]) {
				strcpy(arg_bufs[i], "null"); /* dead placeholder; selection is the mangled symbol */
				continue;
			}
			/* A `buf[lo:hi]` slice arg: emit the sub-view once and capture (ptr, len) as a temp
			 * type-6 slice ValueInfo, so the slice-arg dispatch below passes it like a slice var. */
			if (inner->kind == HIR_EXPR_SLICE) {
				char sp[256], sl[256], scp[256];
				const char *se;
				int sw;
				if (codegen_slice(ctx, inner, sp, sl, scp, &se, &sw)) {
					ValueInfo *vi = calloc(1, sizeof(ValueInfo));
					vi->name = strdup("");
					vi->llvm_name = strdup(sp);
					vi->type = 6;
					vi->is_slice = 1;
					vi->len_ssa = strdup(sl);
					vi->cap_ssa = strdup(scp);
					vi->field_type = se;
					vi->string_len = -1;
					vi->bit_width = sw;
					arg_slice_vi[i] = vi;
					strcpy(arg_bufs[i], sp);
					continue;
				}
			}
			codegen_expression(ctx, expr->data.call.args[i], arg_bufs[i]);
		}

		/* Second pass: check if arguments are variables holding strings (type 2, 5, 6) or static arrays */
		int *arg_is_static_array = malloc(expr->data.call.arg_count * sizeof(int));
		for (int i = 0; i < expr->data.call.arg_count; i++) {
			arg_is_static_array[i] = 0;
			HirExpr *inner = expr->data.call.args[i];
			if (inner->kind == HIR_EXPR_UNARY &&
			    (inner->data.unary.op == UNARY_MOVE || inner->data.unary.op == UNARY_COPY))
				inner = inner->data.unary.operand;
			if (arg_slice_vi[i]) {
				arg_values[i] = arg_slice_vi[i]; /* a `buf[lo:hi]` slice arg → dispatch as a slice */
			} else if (!arg_is_string[i] && !arg_is_array_literal[i] && inner->kind == HIR_EXPR_NAME) {
				const char *arg_name = inner->data.name.name;
				ValueInfo *var = find_value(ctx, arg_name);
				if (var) {
					arg_values[i] = var;
					if (var->type == 2 || var->type == 5) {
						arg_is_string[i] = 1;
					}
				} else if (codegen_find_static_array(ctx, arg_name)) {
					arg_is_static_array[i] = 1;
				}
			}
		}

		HirProcDecl *callee_proc = find_proc_decl(ctx, func_name);
		HirFuncDecl *callee_func = NULL;
		HirFuncGroupDecl *callee_group = func_name ? find_func_group(ctx, func_name) : NULL;
		if (callee_group) {
			callee_func =
			    find_group_member_for_call(ctx, callee_group, expr->data.call.args, expr->data.call.arg_count);
			/* Safety fallback: if no member matched, route to first member to avoid
			 * an undefined LLVM symbol. Semantic should have already emitted an error. */
			if (!callee_func && callee_group->member_count > 0) {
				callee_func = find_func_decl(ctx, callee_group->member_names[0]);
			}
		} else {
			callee_func = func_name ? find_func_decl(ctx, func_name) : NULL;
		}

		/* Archetype-parametric call site detection. The arg in the archetype
		 * slot must be an HIR_EXPR_NAME referring to a declared archetype. */
		char mono_call_name[256] = {0};
		HirArchetypeDecl *mono_arch = NULL;
		int mono_arch_pidx = -1;
		if (callee_proc && is_archetype_parametric(callee_proc)) {
			mono_arch_pidx = archetype_param_index(callee_proc);
			if (mono_arch_pidx >= 0 && mono_arch_pidx < expr->data.call.arg_count) {
				HirExpr *aexpr = expr->data.call.args[mono_arch_pidx];
				if (aexpr && aexpr->kind == HIR_EXPR_NAME) {
					mono_arch = find_archetype_decl(ctx, aexpr->data.name.name);
				}
			}
			if (mono_arch) {
				monomorph_enqueue(ctx, callee_proc, mono_arch);
				monomorph_mangle(callee_proc->name, mono_arch->name, mono_call_name, sizeof(mono_call_name));
			}
		}

		/* Callback-parametric call site: each callback arg must be a bare proc/func
		 * NAME (compile-time known). Specialize the callee per chosen callback and
		 * route the call to the mangled symbol. */
		int cb_mono = 0;
		if (callee_proc && has_callback_param(ctx, callee_proc)) {
			const char *bound[8] = {0};
			int ok = 1;
			for (int i = 0; i < callee_proc->param_count && i < 8; i++) {
				if (!is_callback_param(ctx, callee_proc->params[i]))
					continue;
				HirExpr *a = (i < expr->data.call.arg_count) ? expr->data.call.args[i] : NULL;
				if (a && a->kind == HIR_EXPR_NAME) {
					/* cb_resolve so a forwarded callback param binds to the concrete proc
					 * of the enclosing specialization, not the param name. */
					bound[i] = cb_resolve(ctx, a->data.name.name);
				} else {
					ok = 0; /* not a known proc/func name — semantic should have flagged it */
				}
			}
			if (ok) {
				cb_mangle(callee_proc, bound, mono_call_name, sizeof(mono_call_name));
				cb_enqueue(ctx, callee_proc, bound, mono_call_name);
				cb_mono = 1;
			}
		}
		char **call_arg_vals = malloc(expr->data.call.arg_count * sizeof(char *));
		const char **call_arg_types = malloc(expr->data.call.arg_count * sizeof(const char *));
		/* For a `T[]` slice arg, the companion runtime length (i64 SSA or literal) emitted as a
		 * second positional arg right after the element pointer; NULL for non-slice args. */
		char **call_arg_len = malloc(expr->data.call.arg_count * sizeof(char *));

		/* Prepare arguments: emit conversions before the call, collect register names */
		for (int i = 0; i < expr->data.call.arg_count; i++) {
			call_arg_vals[i] = malloc(256);
			call_arg_types[i] = "i32"; /* Default type */
			call_arg_len[i] = NULL;

			if (arg_is_callback[i]) {
				strcpy(call_arg_vals[i], "null");
				call_arg_types[i] = "i8*";
				continue;
			}

			/* Determine what callee param expects */
			int callee_wants_arr = 0;        /* unbounded char[] (arche_array struct on non-extern) */
			int callee_wants_shaped_arr = 0; /* sized char[N] (raw i8*) */
			int callee_is_extern = (callee_proc && callee_proc->is_extern) || (callee_func && callee_func->is_extern);
			HirType *callee_pt = NULL;
			if (callee_proc && i < callee_proc->param_count)
				callee_pt = callee_proc->params[i]->type;
			if (callee_func && i < callee_func->param_count)
				callee_pt = callee_func->params[i]->type;
			/* `T[]` param (any element, incl. char) = a two-arg slice (ptr,len) on a non-extern callee. */
			int callee_wants_slice = callee_pt && callee_pt->tag == HIR_TYPE_ARRAY && !callee_is_extern;
			if (callee_wants_slice)
				; /* handled in the type-6 arg branch below; not the arche_array path */
			else if (callee_pt && callee_pt->tag == HIR_TYPE_ARRAY)
				callee_wants_arr = 1;
			else if (callee_pt && callee_pt->tag == HIR_TYPE_SHAPED_ARRAY)
				callee_wants_shaped_arr = 1;

			/* Handle type conversions, emit code before call if needed */
			if (arg_is_static_array[i]) {
				/* Static array: check if needs wrapping in arche_array struct */
				const char *arg_name = expr->data.call.args[i]->data.name.name;
				HirStaticDecl *sa = codegen_find_static_array(ctx, arg_name);
				if (sa) {
					const char *elem_type = "i8";
					const char *elem_ptr_type = "i8*";
					if (sa->array.element_type) {
						const char *elem_name = field_base_type_name(sa->array.element_type);
						if (strcmp(elem_name, "double") == 0) {
							elem_type = "double";
							elem_ptr_type = "double*";
						} else if (strcmp(elem_name, "float") == 0) {
							elem_type = "float";
							elem_ptr_type = "float*";
						} else if (strcmp(elem_name, "int") == 0) {
							elem_type = "i32";
							elem_ptr_type = "i32*";
						}
					}
					char *gep = gen_value_name(ctx);
					buffer_append_fmt(ctx, "  %s = getelementptr [%d x %s], [%d x %s]* @%s, i64 0, i64 0\n", gep,
					                  sa->array.size, elem_type, sa->array.size, elem_type, arg_name);

					/* If callee wants char[] and is not extern, wrap in arche_array struct */
					if (callee_wants_slice) {
						/* `T[]` slice param: pass element pointer + i64 len (the static size N). */
						strcpy(call_arg_vals[i], gep);
						call_arg_types[i] = elem_ptr_type;
						call_arg_len[i] = malloc(32);
						snprintf(call_arg_len[i], 32, "%d", sa->array.size);
					} else if (callee_wants_arr && !callee_is_extern && strcmp(elem_type, "i8") == 0) {
						/* Create struct on stack */
						char *arr_alloca = gen_value_name(ctx);
						emit_alloca(ctx, "  %s = alloca %%struct.arche_array\n", arr_alloca);

						/* Store data pointer in field 0 */
						char *ptr_gep = gen_value_name(ctx);
						buffer_append_fmt(
						    ctx, "  %s = getelementptr %%struct.arche_array, %%struct.arche_array* %s, i32 0, i32 0\n",
						    ptr_gep, arr_alloca);
						buffer_append_fmt(ctx, "  store i8* %s, i8** %s\n", gep, ptr_gep);

						/* Store length in field 1 */
						char *len_gep = gen_value_name(ctx);
						buffer_append_fmt(
						    ctx, "  %s = getelementptr %%struct.arche_array, %%struct.arche_array* %s, i32 0, i32 1\n",
						    len_gep, arr_alloca);
						buffer_append_fmt(ctx, "  store i64 %d, i64* %s\n", sa->array.size, len_gep);

						/* Store capacity in field 2 */
						char *cap_gep = gen_value_name(ctx);
						buffer_append_fmt(
						    ctx, "  %s = getelementptr %%struct.arche_array, %%struct.arche_array* %s, i32 0, i32 2\n",
						    cap_gep, arr_alloca);
						buffer_append_fmt(ctx, "  store i64 %d, i64* %s\n", sa->array.size, cap_gep);

						strcpy(call_arg_vals[i], arr_alloca);
						call_arg_types[i] = "%struct.arche_array*";
					} else {
						strcpy(call_arg_vals[i], gep);
						call_arg_types[i] = elem_ptr_type;
					}
				} else {
					strcpy(call_arg_vals[i], arg_bufs[i]);
					call_arg_types[i] = "i8*";
				}
			} else if (arg_values[i] && arg_values[i]->type == 7) {
				/* Arg is char buffer [N x i8]*. */
				int buf_len = arg_values[i]->string_len;
				char *bitcast = gen_value_name(ctx);
				buffer_append_fmt(ctx, "  %s = bitcast [%d x i8]* %s to i8*\n", bitcast, buf_len, arg_bufs[i]);
				/* Array args pass BY REFERENCE — read-only borrow is the default, so no copy. A
				 * borrowed (non-`move`) array param cannot be mutated (semantic purity check), so
				 * the caller's buffer is never clobbered; `move` args are by-ref too (ownership
				 * handed over). The old by-value memcpy clone is gone.
				 * TODO(FBIP reuse): a producer that does need a private result buffer can reuse a
				 * dead input's storage when aliasing is provably safe (see plan Phase C). */
				if (callee_wants_arr && !callee_is_extern) {
					/* Non-extern callee expects char[] (arche_array*): wrap the
					 * bare buffer pointer in a stack arche_array {ptr,len,cap}. */
					char *arr_alloca = gen_value_name(ctx);
					emit_alloca(ctx, "  %s = alloca %%struct.arche_array\n", arr_alloca);
					char *ptr_gep = gen_value_name(ctx);
					buffer_append_fmt(
					    ctx, "  %s = getelementptr %%struct.arche_array, %%struct.arche_array* %s, i32 0, i32 0\n",
					    ptr_gep, arr_alloca);
					buffer_append_fmt(ctx, "  store i8* %s, i8** %s\n", bitcast, ptr_gep);
					char *len_gep = gen_value_name(ctx);
					buffer_append_fmt(
					    ctx, "  %s = getelementptr %%struct.arche_array, %%struct.arche_array* %s, i32 0, i32 1\n",
					    len_gep, arr_alloca);
					buffer_append_fmt(ctx, "  store i64 %d, i64* %s\n", buf_len, len_gep);
					char *cap_gep = gen_value_name(ctx);
					buffer_append_fmt(
					    ctx, "  %s = getelementptr %%struct.arche_array, %%struct.arche_array* %s, i32 0, i32 2\n",
					    cap_gep, arr_alloca);
					buffer_append_fmt(ctx, "  store i64 %d, i64* %s\n", buf_len, cap_gep);
					strcpy(call_arg_vals[i], arr_alloca);
					call_arg_types[i] = "%struct.arche_array*";
				} else {
					/* Extern (C ABI) or scalar callee: pass bare i8*. */
					strcpy(call_arg_vals[i], bitcast);
					call_arg_types[i] = "i8*";
				}
			} else if (arg_values[i] && arg_values[i]->type == 5) {
				/* Arg is arche_array struct (e.g. a static char buffer) */
				if ((callee_is_extern && callee_wants_arr) || callee_wants_shaped_arr) {
					/* Extract i8* data ptr from struct (field 0): C ABI, or a sized char[N]
					 * param (which takes a bare i8*). */
					char *dp_gep = gen_value_name(ctx);
					buffer_append_fmt(
					    ctx, "  %s = getelementptr %%struct.arche_array, %%struct.arche_array* %s, i32 0, i32 0\n",
					    dp_gep, arg_bufs[i]);
					char *dp = gen_value_name(ctx);
					buffer_append_fmt(ctx, "  %s = load i8*, i8** %s\n", dp, dp_gep);
					strcpy(call_arg_vals[i], dp);
					call_arg_types[i] = "i8*";
				} else if (callee_wants_arr) {
					/* Pass struct ptr directly — non-extern call */
					strcpy(call_arg_vals[i], arg_bufs[i]);
					call_arg_types[i] = "%struct.arche_array*";
				} else {
					/* Default to i32 */
					strcpy(call_arg_vals[i], arg_bufs[i]);
					call_arg_types[i] = "i32";
				}
			} else if (arg_is_string[i]) {
				/* String literals are already i8*, string variables may be arche_array */
				int is_string_literal = (arg_values[i] == NULL);
				int is_array_struct = arg_values[i] && arg_values[i]->type == 5; /* arche_array struct */

				if (callee_wants_slice) {
					/* char[] slice param: pass two positional args `(i8* ptr, i64 len)`. */
					char ptr_out[64], len_out[64];
					if (is_array_struct) {
						/* string var as arche_array: ptr = field 0, len = field 1. */
						char *dp_gep = gen_value_name(ctx);
						buffer_append_fmt(
						    ctx, "  %s = getelementptr %%struct.arche_array, %%struct.arche_array* %s, i32 0, i32 0\n",
						    dp_gep, arg_bufs[i]);
						char *dp = gen_value_name(ctx);
						buffer_append_fmt(ctx, "  %s = load i8*, i8** %s\n", dp, dp_gep);
						char *ln_gep = gen_value_name(ctx);
						buffer_append_fmt(
						    ctx, "  %s = getelementptr %%struct.arche_array, %%struct.arche_array* %s, i32 0, i32 1\n",
						    ln_gep, arg_bufs[i]);
						char *ln = gen_value_name(ctx);
						buffer_append_fmt(ctx, "  %s = load i64, i64* %s\n", ln, ln_gep);
						snprintf(ptr_out, sizeof(ptr_out), "%s", dp);
						snprintf(len_out, sizeof(len_out), "%s", ln);
					} else {
						/* literal or bare i8* var: content length is compile-time known. */
						size_t str_len = 0;
						HirExpr *arg_expr = expr->data.call.args[i];
						if (arg_expr->kind == HIR_EXPR_STRING) {
							str_len = arg_expr->data.string.length;
						} else if (arg_expr->kind == HIR_EXPR_LITERAL && arg_expr->data.literal.lexeme[0] == '"') {
							const char *lex = arg_expr->data.literal.lexeme;
							for (int j = 1; lex[j] != '"' && lex[j] != '\0'; j++) {
								if (lex[j] == '\\' && lex[j + 1] != '\0')
									j++;
								str_len++;
							}
						} else if (arg_values[i] && arg_values[i]->string_len >= 0) {
							str_len = (size_t)arg_values[i]->string_len;
						} else if (ctx->current_each_field_target && arg_expr->kind == HIR_EXPR_FIELD &&
						           arg_expr->data.field.field_name &&
						           strcmp(arg_expr->data.field.field_name, "name") == 0) {
							/* `f.name` reflection: content length is the field identifier's length. */
							str_len = strlen(ctx->current_each_field_target->name);
						}
						snprintf(ptr_out, sizeof(ptr_out), "%s", arg_bufs[i]);
						snprintf(len_out, sizeof(len_out), "%zu", str_len);
					}
					strcpy(call_arg_vals[i], ptr_out);
					call_arg_types[i] = "i8*";
					call_arg_len[i] = malloc(64);
					snprintf(call_arg_len[i], 64, "%s", len_out);
				} else if (is_string_literal && callee_wants_arr && !callee_is_extern) {
					/* String literal passed to non-extern function expecting char[]: wrap in arche_array struct */
					char *str_ptr = arg_bufs[i]; /* i8* from getelementptr */

					/* Need to know string length */
					size_t str_len = 0;
					HirExpr *arg_expr = expr->data.call.args[i];
					if (arg_expr->kind == HIR_EXPR_STRING) {
						/* String from parser (already processed, without quotes) */
						str_len = arg_expr->data.string.length;
					} else if (arg_expr->kind == HIR_EXPR_LITERAL && arg_expr->data.literal.lexeme[0] == '"') {
						/* Old-style string literal with quotes */
						const char *lex = arg_expr->data.literal.lexeme;
						for (int j = 1; lex[j] != '"' && lex[j] != '\0'; j++) {
							if (lex[j] == '\\' && lex[j + 1] != '\0') {
								j++; /* skip escape sequence (count as 1 char) */
							}
							str_len++;
						}
					}

					/* Create struct on stack */
					char *arr_alloca = gen_value_name(ctx);
					emit_alloca(ctx, "  %s = alloca %%struct.arche_array\n", arr_alloca);

					/* Store data pointer in field 0 */
					char *ptr_gep = gen_value_name(ctx);
					buffer_append_fmt(
					    ctx, "  %s = getelementptr %%struct.arche_array, %%struct.arche_array* %s, i32 0, i32 0\n",
					    ptr_gep, arr_alloca);
					buffer_append_fmt(ctx, "  store i8* %s, i8** %s\n", str_ptr, ptr_gep);

					/* Store length in field 1 */
					char *len_gep = gen_value_name(ctx);
					buffer_append_fmt(
					    ctx, "  %s = getelementptr %%struct.arche_array, %%struct.arche_array* %s, i32 0, i32 1\n",
					    len_gep, arr_alloca);
					buffer_append_fmt(ctx, "  store i64 %zu, i64* %s\n", str_len, len_gep);

					/* Store capacity in field 2 */
					char *cap_gep = gen_value_name(ctx);
					buffer_append_fmt(
					    ctx, "  %s = getelementptr %%struct.arche_array, %%struct.arche_array* %s, i32 0, i32 2\n",
					    cap_gep, arr_alloca);
					buffer_append_fmt(ctx, "  store i64 %zu, i64* %s\n", str_len, cap_gep);

					strcpy(call_arg_vals[i], arr_alloca);
					call_arg_types[i] = "%struct.arche_array*";
				} else if (is_string_literal) {
					/* String literal: a NUL-terminated i8* from codegen_expression (the NUL backs the
					 * [N+1] global, past the content length). FFI %s / paths consume it directly. */
					strcpy(call_arg_vals[i], arg_bufs[i]);
					call_arg_types[i] = "i8*";
				} else if (is_array_struct) {
					/* String variable stored as arche_array struct: pass struct ptr (variadic handler will unwrap) */
					strcpy(call_arg_vals[i], arg_bufs[i]);
					call_arg_types[i] = "%struct.arche_array*";
				} else if (callee_is_extern && callee_wants_arr) {
					/* String variable from extern callee expecting char[]: extract i8* from struct */
					char *dp_gep = gen_value_name(ctx);
					buffer_append_fmt(
					    ctx, "  %s = getelementptr %%struct.arche_array, %%struct.arche_array* %s, i32 0, i32 0\n",
					    dp_gep, arg_bufs[i]);
					char *dp = gen_value_name(ctx);
					buffer_append_fmt(ctx, "  %s = load i8*, i8** %s\n", dp, dp_gep);
					strcpy(call_arg_vals[i], dp);
					call_arg_types[i] = "i8*";
				} else if (callee_wants_arr) {
					/* String variable stored as a bare i8* (type 2, e.g. `x := "lit"`): a
					 * non-extern char[] callee expects a %struct.arche_array*, not a raw char
					 * pointer — wrap it in a stack arche_array. (Passing the bare i8* here was a
					 * bug: the callee read the string bytes as {ptr,len,cap} and crashed.) len/cap
					 * use the known literal length, else 0 — arche `strlen` scans to NUL anyway. */
					int slen = (arg_values[i] && arg_values[i]->string_len >= 0) ? arg_values[i]->string_len : 0;
					char *arr_alloca = gen_value_name(ctx);
					emit_alloca(ctx, "  %s = alloca %%struct.arche_array\n", arr_alloca);
					char *ptr_gep = gen_value_name(ctx);
					buffer_append_fmt(
					    ctx, "  %s = getelementptr %%struct.arche_array, %%struct.arche_array* %s, i32 0, i32 0\n",
					    ptr_gep, arr_alloca);
					buffer_append_fmt(ctx, "  store i8* %s, i8** %s\n", arg_bufs[i], ptr_gep);
					char *len_gep = gen_value_name(ctx);
					buffer_append_fmt(
					    ctx, "  %s = getelementptr %%struct.arche_array, %%struct.arche_array* %s, i32 0, i32 1\n",
					    len_gep, arr_alloca);
					buffer_append_fmt(ctx, "  store i64 %d, i64* %s\n", slen, len_gep);
					char *cap_gep = gen_value_name(ctx);
					buffer_append_fmt(
					    ctx, "  %s = getelementptr %%struct.arche_array, %%struct.arche_array* %s, i32 0, i32 2\n",
					    cap_gep, arr_alloca);
					buffer_append_fmt(ctx, "  store i64 %d, i64* %s\n", slen, cap_gep);
					strcpy(call_arg_vals[i], arr_alloca);
					call_arg_types[i] = "%struct.arche_array*";
				} else {
					/* Pass bare pointer */
					strcpy(call_arg_vals[i], arg_bufs[i]);
					call_arg_types[i] = "i8*";
				}
			} else if (arg_is_array_literal[i]) {
				/* Array literal */
				if (callee_wants_arr) {
					/* Pass struct pointer directly */
					strcpy(call_arg_vals[i], arg_bufs[i]);
					call_arg_types[i] = "%struct.arche_array*";
				} else {
					/* Pass bare value */
					strcpy(call_arg_vals[i], arg_bufs[i]);
					call_arg_types[i] = "i32";
				}
			} else {
				/* Check if arg is type 6 (i8* pointer parameter from array param) */
				if (arg_values[i] && arg_values[i]->type == 6) {
					if (callee_wants_slice) {
						/* Pass a `T[]` slice as two positional args: element pointer + i64 len.
						 * An already-slice arg forwards its runtime len; a bounded `T[N]` arg
						 * DECAYS — its compile-time count N becomes the len. */
						strcpy(call_arg_vals[i], arg_bufs[i]);
						const char *e =
						    arg_values[i]->field_type ? llvm_type_from_arche(arg_values[i]->field_type) : "i8";
						if (strcmp(e, "double") == 0)
							call_arg_types[i] = "double*";
						else if (strcmp(e, "i64") == 0)
							call_arg_types[i] = "i64*";
						else if (strcmp(e, "i16") == 0)
							call_arg_types[i] = "i16*";
						else if (strcmp(e, "i8") == 0)
							call_arg_types[i] = "i8*";
						else
							call_arg_types[i] = "i32*";
						call_arg_len[i] = malloc(32);
						if (arg_values[i]->is_slice && arg_values[i]->len_ssa)
							snprintf(call_arg_len[i], 32, "%s", arg_values[i]->len_ssa);
						else
							snprintf(call_arg_len[i], 32, "%d", arg_values[i]->string_len);
					} else if (callee_wants_arr && !callee_is_extern) {
						/* Forwarding a char[] param (already an i8* data ptr) to a
						 * non-extern callee that expects %struct.arche_array*:
						 * re-wrap the pointer in a stack arche_array. The callee
						 * reads only field 0 (data ptr) and uses explicit bounds,
						 * so placeholder len/cap are fine. */
						char *arr_alloca = gen_value_name(ctx);
						emit_alloca(ctx, "  %s = alloca %%struct.arche_array\n", arr_alloca);
						char *ptr_gep = gen_value_name(ctx);
						buffer_append_fmt(
						    ctx, "  %s = getelementptr %%struct.arche_array, %%struct.arche_array* %s, i32 0, i32 0\n",
						    ptr_gep, arr_alloca);
						buffer_append_fmt(ctx, "  store i8* %s, i8** %s\n", arg_bufs[i], ptr_gep);
						char *len_gep = gen_value_name(ctx);
						buffer_append_fmt(
						    ctx, "  %s = getelementptr %%struct.arche_array, %%struct.arche_array* %s, i32 0, i32 1\n",
						    len_gep, arr_alloca);
						buffer_append_fmt(ctx, "  store i64 0, i64* %s\n", len_gep);
						char *cap_gep = gen_value_name(ctx);
						buffer_append_fmt(
						    ctx, "  %s = getelementptr %%struct.arche_array, %%struct.arche_array* %s, i32 0, i32 2\n",
						    cap_gep, arr_alloca);
						buffer_append_fmt(ctx, "  store i64 0, i64* %s\n", cap_gep);
						strcpy(call_arg_vals[i], arr_alloca);
						call_arg_types[i] = "%struct.arche_array*";
					} else {
						/* Bare element pointer for a non-arche_array callee. For a char[]
						 * buffer this is i8*; for a non-char array it is the element pointer
						 * (double, i64, i32, i16 star) matching the callee's element param. */
						strcpy(call_arg_vals[i], arg_bufs[i]);
						const char *e =
						    arg_values[i]->field_type ? llvm_type_from_arche(arg_values[i]->field_type) : "i8";
						if (strcmp(e, "double") == 0)
							call_arg_types[i] = "double*";
						else if (strcmp(e, "i64") == 0)
							call_arg_types[i] = "i64*";
						else if (strcmp(e, "i32") == 0)
							call_arg_types[i] = "i32*";
						else if (strcmp(e, "i16") == 0)
							call_arg_types[i] = "i16*";
						else
							call_arg_types[i] = "i8*";
					}
				} else if (expr->data.call.args[i]->resolved.tag == HIR_TYPE_CHAR_ARRAY) {
					/* Arg is a char[] value with no ValueInfo — e.g. the result of an
					 * extern call like arche_argv() (a raw i8*). For a non-extern char[]
					 * callee, wrap it in a stack arche_array (placeholder len/cap; callees
					 * use strlen on the NUL-terminated data ptr). Extern (C ABI) callees
					 * take the bare i8*. Previously this fell through to a bogus i32. */
					if (callee_wants_arr && !callee_is_extern) {
						char *arr_alloca = gen_value_name(ctx);
						emit_alloca(ctx, "  %s = alloca %%struct.arche_array\n", arr_alloca);
						char *ptr_gep = gen_value_name(ctx);
						buffer_append_fmt(
						    ctx, "  %s = getelementptr %%struct.arche_array, %%struct.arche_array* %s, i32 0, i32 0\n",
						    ptr_gep, arr_alloca);
						buffer_append_fmt(ctx, "  store i8* %s, i8** %s\n", arg_bufs[i], ptr_gep);
						char *len_gep = gen_value_name(ctx);
						buffer_append_fmt(
						    ctx, "  %s = getelementptr %%struct.arche_array, %%struct.arche_array* %s, i32 0, i32 1\n",
						    len_gep, arr_alloca);
						buffer_append_fmt(ctx, "  store i64 0, i64* %s\n", len_gep);
						char *cap_gep = gen_value_name(ctx);
						buffer_append_fmt(
						    ctx, "  %s = getelementptr %%struct.arche_array, %%struct.arche_array* %s, i32 0, i32 2\n",
						    cap_gep, arr_alloca);
						buffer_append_fmt(ctx, "  store i64 0, i64* %s\n", cap_gep);
						strcpy(call_arg_vals[i], arr_alloca);
						call_arg_types[i] = "%struct.arche_array*";
					} else {
						strcpy(call_arg_vals[i], arg_bufs[i]);
						call_arg_types[i] = "i8*";
					}
				} else if (expr->data.call.args[i]->resolved.tag == HIR_TYPE_FLOAT) {
					strcpy(call_arg_vals[i], arg_bufs[i]);
					call_arg_types[i] = "double";
				} else if (callee_pt && callee_pt->tag == HIR_TYPE_INT) {
					/* Coerce an int arg to the callee's DECLARED int width. A bare literal
					 * defaults to i32 in its own resolved type, so handing it to an i64 (or
					 * i16/i8) param would truncate/mismatch — adopt the param's width instead.
					 * emit_int_convert relabels a constant for free and sext/zext/truncs an SSA
					 * value; this also fixes a wider/narrower register arg meeting the param. */
					int want_w = callee_pt->int_width ? callee_pt->int_width : 32;
					emit_int_convert(ctx, arg_bufs[i], &expr->data.call.args[i]->resolved, want_w, call_arg_vals[i]);
					call_arg_types[i] = llvm_int_type(want_w);
				} else if (expr->data.call.args[i]->resolved.tag == HIR_TYPE_INT &&
				           expr->data.call.args[i]->resolved.int_width != 32) {
					/* Wider/narrower int (e.g. i64): pass at its actual LLVM width. */
					strcpy(call_arg_vals[i], arg_bufs[i]);
					call_arg_types[i] = llvm_int_type(expr->data.call.args[i]->resolved.int_width);
				} else if (expr->data.call.args[i]->resolved.tag == HIR_TYPE_OPAQUE ||
				           (callee_pt && (callee_pt->tag == HIR_TYPE_OPAQUE || callee_pt->tag == HIR_TYPE_HANDLE))) {
					/* An opaque/handle cell (a nominal `:: opaque` value or an archetype handle) is
					 * pointer-width i64. The callee's DECLARED param type is authoritative even when the
					 * arg's own resolved type wasn't annotated — e.g. a name used inside a `{ }` block or
					 * a `match` arm, which semantic's type pass does not descend into, so the arg defaults
					 * to UNKNOWN and would otherwise be mis-emitted as i32. */
					strcpy(call_arg_vals[i], arg_bufs[i]);
					call_arg_types[i] = "i64";
				} else {
					/* Default to i32 */
					strcpy(call_arg_vals[i], arg_bufs[i]);
					call_arg_types[i] = "i32";
				}
			}
		}

		/* The extern-type slot marshal was removed; opaque values pass to C by value
		 * (i64 cell), so no handle->pointer conversion is needed. */
		int consume_call_done = 0;
		char *res_name = gen_value_name(ctx);

		/* Special handling for print function with double arguments */
		const char *actual_func_name;
		char actual_sym_buf[512];
		if (mono_arch || cb_mono) {
			actual_func_name = mono_call_name; /* route to monomorphized symbol (already a unique global) */
		} else if (callee_group && callee_func) {
			actual_func_name = callee_func->name; /* route to matched member */
		} else {
			actual_func_name = func_name ? func_name : "unknown";
		}
		/* Per-unit: route arche-owned (non-extern) callees to their mangled external symbol so the call
		 * matches the (mangled) definition. Externs + monomorph instances keep their names. */
		if (!(mono_arch || cb_mono)) {
			int callee_extern = (callee_func && callee_func->is_extern) || (callee_proc && callee_proc->is_extern);
			actual_func_name = cg_fnsym(ctx, actual_func_name, callee_extern, actual_sym_buf, sizeof(actual_sym_buf));
		}

		/* Rewrite the archetype-typed argument so it carries the @<arch> global
		 * pointer (the existing arg-prep loop doesn't know about archetype names
		 * passed by-value). */
		if (mono_arch && mono_arch_pidx >= 0 && mono_arch_pidx < expr->data.call.arg_count) {
			snprintf(call_arg_vals[mono_arch_pidx], 256, "@%s", mono_arch->name);
			char tbuf[64];
			snprintf(tbuf, sizeof(tbuf), "%%struct.%s*", mono_arch->name);
			/* call_arg_types is `const char **`; store a stable string. */
			call_arg_types[mono_arch_pidx] = strdup(tbuf);
		}

		/* Emit the call with prepared arguments (skipped when consume_call_done). */
		/* Check if this is a variadic function like sprintf or printf */
		int is_variadic = func_name && (strcmp(func_name, "sprintf") == 0 || strcmp(func_name, "printf") == 0);
		int is_exit = func_name && strcmp(func_name, "exit") == 0;

		if (consume_call_done)
			goto call_done;

		if (is_exit) {
			/* exit() is a void function that never returns */
			buffer_append_fmt(ctx, "  call void @%s(", actual_func_name);
			emit_call_arglist(ctx, expr->data.call.arg_count, arg_is_callback, call_arg_types, call_arg_vals,
			                  call_arg_len);
			buffer_append(ctx, ")\n");
			buffer_append(ctx, "  unreachable\n");
			strcpy(result_buf, "0");
		} else if (is_variadic) {
			/* For variadic C functions, array struct args must be unwrapped to i8* */
			for (int i = 0; i < expr->data.call.arg_count; i++) {
				if (arg_is_callback[i])
					continue;
				if (call_arg_types[i] && strcmp(call_arg_types[i], "%struct.arche_array*") == 0) {
					char *dp_gep = gen_value_name(ctx);
					buffer_append_fmt(
					    ctx, "  %s = getelementptr %%struct.arche_array, %%struct.arche_array* %s, i32 0, i32 0\n",
					    dp_gep, call_arg_vals[i]);
					char *dp = gen_value_name(ctx);
					buffer_append_fmt(ctx, "  %s = load i8*, i8** %s\n", dp, dp_gep);
					strcpy(call_arg_vals[i], dp);
					call_arg_types[i] = "i8*";
				}
			}
			/* Emit variadic signature based on function */
			if (strcmp(func_name, "sprintf") == 0) {
				buffer_append_fmt(ctx, "  %s = call i32 (i8*, i8*, ...)", res_name);
			} else {
				buffer_append_fmt(ctx, "  %s = call i32 (i8*, ...)", res_name);
			}
			buffer_append_fmt(ctx, " @%s(", actual_func_name);
			emit_call_arglist(ctx, expr->data.call.arg_count, arg_is_callback, call_arg_types, call_arg_vals,
			                  call_arg_len);
			buffer_append(ctx, ")\n");
			strcpy(result_buf, res_name);
		} else {
			/* Normal non-variadic call - determine return type */
			const char *return_type = "i32"; /* default */

			/* A multi-return callee returns an aggregate `{T1, …, Tn}`; the multi-bind that
			 * wraps this call extractvalue's the members. */
			char multiret_buf[512];
			HirType **mr_types = callee_func ? callee_func->return_types : NULL;
			int mr_count = callee_func ? callee_func->return_type_count : 0;
			if (mr_count > 1) {
				llvm_return_list_type(mr_types, mr_count, multiret_buf, sizeof(multiret_buf));
				return_type = multiret_buf;
				buffer_append_fmt(ctx, "  %s = call %s @%s(", res_name, return_type, actual_func_name);
				emit_call_arglist(ctx, expr->data.call.arg_count, arg_is_callback, call_arg_types, call_arg_vals,
				                  call_arg_len);
				buffer_append(ctx, ")\n");
				strcpy(result_buf, res_name);
				goto call_done;
			}

			/* Return type from the func declaration. A scalar-position call uses the single
			 * return type. A proc is not a value (its outputs are out-params), so only a func
			 * yields a result type here. */
			HirType *crt = (callee_func && callee_func->return_type_count > 0) ? callee_func->return_types[0] : NULL;
			if (crt) {
				/* Use the same mapping as the definition's return type so the call site and the
				 * callee agree — char[] / char[N] both become i8* (a byte view). Works for a
				 * returning func, a returning proc, or a returning extern. */
				return_type = return_member_llvm(crt);
			} else if (callee_func && callee_func->is_extern) {
				/* Bare extern with no declared return (`extern name(...)`) ⇒ void. */
				return_type = "void";
			} else if (callee_proc && callee_proc->is_extern) {
				/* An extern proc's C return value is its out-only out-param (void if none). */
				return_type = extern_proc_cret(callee_proc);
			} else if (callee_proc) {
				/* A non-extern proc returns void — its results go through out-pointers. */
				return_type = "void";
			} else if (expr->resolved.tag != HIR_TYPE_UNKNOWN) {
				/* Fallback: use resolved type if available */
				return_type = llvm_type_from_arche(hir_resolved_type_name(expr));
			}

			/* If return type is void, emit void call without assignment */
			if (strcmp(return_type, "void") == 0) {
				buffer_append_fmt(ctx, "  call void @%s(", actual_func_name);
				int n_emitted = emit_call_arglist(ctx, expr->data.call.arg_count, arg_is_callback, call_arg_types,
				                                  call_arg_vals, call_arg_len);
				/* Append the proc's out-pointer args (a non-extern proc writes its results through
				 * them). Set by the enclosing proc-call multi-bind path. */
				for (int i = 0; i < ctx->pending_out_ptr_count; i++) {
					if (n_emitted > 0 || i > 0)
						buffer_append(ctx, ", ");
					buffer_append_fmt(ctx, "%s* %s", ctx->pending_out_ptr_types[i], ctx->pending_out_ptr_vals[i]);
				}
				buffer_append(ctx, ")\n");
				strcpy(result_buf, "0");
			} else {
				buffer_append_fmt(ctx, "  %s = call %s @%s(", res_name, return_type, actual_func_name);
				emit_call_arglist(ctx, expr->data.call.arg_count, arg_is_callback, call_arg_types, call_arg_vals,
				                  call_arg_len);
				buffer_append(ctx, ")\n");
				strcpy(result_buf, res_name);
			}
		}

	call_done:;
		/* Cleanup */
		for (int i = 0; i < expr->data.call.arg_count; i++) {
			free(arg_bufs[i]);
			free(call_arg_vals[i]);
			free(call_arg_len[i]);
			if (arg_slice_vi[i]) {
				free(arg_slice_vi[i]->name);
				free(arg_slice_vi[i]->llvm_name);
				free(arg_slice_vi[i]->len_ssa);
				free(arg_slice_vi[i]);
			}
		}
		free(arg_bufs);
		free(arg_is_string);
		free(arg_is_array_literal);
		free(arg_is_static_array);
		free(arg_values);
		free(arg_is_move);
		free(arg_is_callback);
		free(arg_slice_vi);
		free(call_arg_vals);
		free(call_arg_types);
		free(call_arg_len);
		break;
	}

	case HIR_EXPR_ARRAY_LITERAL: {
		/* Array literal: {elem1, elem2, ...} */
		/* Generate global constant array and wrap in arche_array struct */
		HirExpr **elems = expr->data.array_literal.elements;
		int elem_count = expr->data.array_literal.element_count;

		if (elem_count == 0) {
			strcpy(result_buf, "0");
			break;
		}

		/* Create global constant array */
		char global_name[64];
		snprintf(global_name, sizeof(global_name), "@.arr%d", ctx->string_counter++);

		/* Build array constant declaration and add to globals */
		char global_decl[4096];
		char *decl_pos = global_decl;
		size_t decl_space = sizeof(global_decl);

		decl_pos += snprintf(decl_pos, decl_space, "%s = private constant [%d x i8] [", global_name, elem_count + 1);
		decl_space = sizeof(global_decl) - (decl_pos - global_decl);

		for (int i = 0; i < elem_count; i++) {
			char elem_buf[256];
			codegen_expression(ctx, elems[i], elem_buf);
			decl_pos += snprintf(decl_pos, decl_space, "i8 %s", elem_buf);
			decl_space = sizeof(global_decl) - (decl_pos - global_decl);
			if (i < elem_count - 1) {
				decl_pos += snprintf(decl_pos, decl_space, ", ");
				decl_space = sizeof(global_decl) - (decl_pos - global_decl);
			}
		}
		snprintf(decl_pos, decl_space, ", i8 0]\n");

		/* Append to globals buffer */
		size_t decl_len = strlen(global_decl);
		if (ctx->globals_pos + decl_len >= ctx->globals_size) {
			ctx->globals_size = (ctx->globals_size + decl_len) * 2;
			ctx->globals_buffer = realloc(ctx->globals_buffer, ctx->globals_size);
		}
		strcpy(ctx->globals_buffer + ctx->globals_pos, global_decl);
		ctx->globals_pos += decl_len;

		/* Return the element pointer into the global `[N x i8]` — a normal type-6 char array (the
		 * bind site records the count). No arche_array struct. */
		char *arr_ptr = gen_value_name(ctx);
		buffer_append_fmt(ctx, "  %s = getelementptr [%d x i8], [%d x i8]* %s, i32 0, i32 0\n", arr_ptr, elem_count + 1,
		                  elem_count + 1, global_name);
		strcpy(result_buf, arr_ptr);
		break;
	}

	case HIR_EXPR_ALLOC: {
		/* allocation expression: alloc ArchetypeName(count) */
		const char *arch_name = expr->data.alloc.archetype_name;

		/* Get capacity from first field_value */
		char count_buf[256] = "256";
		if (expr->data.alloc.field_count > 0) {
			codegen_expression(ctx, expr->data.alloc.field_values[0], count_buf);
		}

		/* Find archetype declaration */
		HirArchetypeDecl *arch = find_archetype_decl(ctx, arch_name);
		if (!arch) {
			strcpy(result_buf, "0");
			break;
		}

		/* Struct layout: [pointers...][count][capacity][free_list*][free_count][gen_counters*] */
		/* Calculate struct header size: (field_count pointers + 5 metadata) * 8 = (field_count+5)*8 */
		int struct_sz_bytes = (arch->field_count + 5) * 8;

		/* Calculate byte size per element across all columns */
		int bytes_per_row = 0;
		for (int i = 0; i < arch->field_count; i++) {
			if (arch->fields[i]->kind == FIELD_COLUMN) {
				const char *elem_type = llvm_type_from_arche(field_base_type_name(arch->fields[i]->type));
				int n = field_total_elements(arch->fields[i]->type);
				int elem_sz = llvm_type_sizeof(elem_type);
				bytes_per_row += elem_sz * n;
			}
		}
		/* Add 8 bytes per row for free_list entry (i64), 4 for gen_counters (i32) */
		int total_bytes_per_row = bytes_per_row + 12;

		/* Total size = struct_header + (count * bytes_per_row) + (count * 8 for free_list) */
		/* = struct_sz_bytes + count * total_bytes_per_row */
		char *total_bytes = gen_value_name(ctx);
		char *data_bytes = gen_value_name(ctx);
		buffer_append_fmt(ctx, "  %s = mul i64 %s, %d\n", data_bytes, count_buf, total_bytes_per_row);
		buffer_append_fmt(ctx, "  %s = add i64 %d, %s\n", total_bytes, struct_sz_bytes, data_bytes);

		/* Single calloc (zeros memory for gen_counters) */
		char *raw_ptr = gen_value_name(ctx);
		buffer_append_fmt(ctx, "  %s = call i8* @calloc(i64 1, i64 %s)\n", raw_ptr, total_bytes);

		/* Bitcast to struct pointer */
		char *struct_ptr = gen_value_name(ctx);
		buffer_append_fmt(ctx, "  %s = bitcast i8* %s to %%struct.%s*\n", struct_ptr, raw_ptr, arch_name);

		/* Initialize metadata fields */
		char *count_gep = gen_value_name(ctx);
		buffer_append_fmt(ctx, "  %s = getelementptr %%struct.%s, %%struct.%s* %s, i32 0, i32 %d\n", count_gep,
		                  arch_name, arch_name, struct_ptr, arch->field_count);
		buffer_append_fmt(ctx, "  store i64 0, i64* %s\n", count_gep);

		char *cap_gep = gen_value_name(ctx);
		buffer_append_fmt(ctx, "  %s = getelementptr %%struct.%s, %%struct.%s* %s, i32 0, i32 %d\n", cap_gep, arch_name,
		                  arch_name, struct_ptr, arch->field_count + 1);
		buffer_append_fmt(ctx, "  store i64 %s, i64* %s\n", count_buf, cap_gep);

		char *fc_gep = gen_value_name(ctx);
		buffer_append_fmt(ctx, "  %s = getelementptr %%struct.%s, %%struct.%s* %s, i32 0, i32 %d\n", fc_gep, arch_name,
		                  arch_name, struct_ptr, arch->field_count + 3);
		buffer_append_fmt(ctx, "  store i64 0, i64* %s\n", fc_gep);

		/* Set column pointers and free_list pointer to offsets in the allocated block */
		int col_offset = 0;
		for (int i = 0; i < arch->field_count; i++) {
			if (arch->fields[i]->kind == FIELD_COLUMN) {
				const char *elem_type = llvm_type_from_arche(field_base_type_name(arch->fields[i]->type));

				char *col_gep = gen_value_name(ctx);
				buffer_append_fmt(ctx, "  %s = getelementptr %%struct.%s, %%struct.%s* %s, i32 0, i32 %d\n", col_gep,
				                  arch_name, arch_name, struct_ptr, i);

				char *col_offset_llvm = gen_value_name(ctx);
				buffer_append_fmt(ctx, "  %s = mul i64 %s, %d\n", col_offset_llvm, count_buf, col_offset);

				char *col_data_base = gen_value_name(ctx);
				buffer_append_fmt(ctx, "  %s = getelementptr i8, i8* %s, i64 %d\n", col_data_base, raw_ptr,
				                  struct_sz_bytes);

				char *col_data = gen_value_name(ctx);
				buffer_append_fmt(ctx, "  %s = getelementptr i8, i8* %s, i64 %s\n", col_data, col_data_base,
				                  col_offset_llvm);

				char *col_ptr = gen_value_name(ctx);
				buffer_append_fmt(ctx, "  %s = bitcast i8* %s to %s*\n", col_ptr, col_data, elem_type);

				buffer_append_fmt(ctx, "  store %s* %s, %s** %s\n", elem_type, col_ptr, elem_type, col_gep);

				int n = field_total_elements(arch->fields[i]->type);
				int elem_size = llvm_type_sizeof(elem_type) * n;
				col_offset += elem_size;
			}
		}

		/* Set free_list pointer: it comes after all column rows */
		char *fl_gep = gen_value_name(ctx);
		buffer_append_fmt(ctx, "  %s = getelementptr %%struct.%s, %%struct.%s* %s, i32 0, i32 %d\n", fl_gep, arch_name,
		                  arch_name, struct_ptr, arch->field_count + 2);

		/* free_list offset = struct_header + (all columns data) */
		char *fl_offset = gen_value_name(ctx);
		buffer_append_fmt(ctx, "  %s = mul i64 %s, %d\n", fl_offset, count_buf, bytes_per_row);
		char *fl_data = gen_value_name(ctx);
		buffer_append_fmt(ctx, "  %s = getelementptr i8, i8* %s, i64 %s\n", fl_data, raw_ptr, fl_offset);
		char *fl_add_header = gen_value_name(ctx);
		buffer_append_fmt(ctx, "  %s = getelementptr i8, i8* %s, i64 %d\n", fl_add_header, fl_data, struct_sz_bytes);

		char *fl_ptr = gen_value_name(ctx);
		buffer_append_fmt(ctx, "  %s = bitcast i8* %s to i64*\n", fl_ptr, fl_add_header);

		buffer_append_fmt(ctx, "  store i64* %s, i64** %s\n", fl_ptr, fl_gep);

		/* Set gen_counters pointer: it comes after free_list */
		char *gc_gep = gen_value_name(ctx);
		buffer_append_fmt(ctx, "  %s = getelementptr %%struct.%s, %%struct.%s* %s, i32 0, i32 %d\n", gc_gep, arch_name,
		                  arch_name, struct_ptr, arch->field_count + 4);

		/* gen_counters offset = struct_header + (all columns data) + (free_list data) */
		char *gc_offset = gen_value_name(ctx);
		buffer_append_fmt(ctx, "  %s = mul i64 %s, %d\n", gc_offset, count_buf, bytes_per_row + 8);
		char *gc_data = gen_value_name(ctx);
		buffer_append_fmt(ctx, "  %s = getelementptr i8, i8* %s, i64 %s\n", gc_data, raw_ptr, gc_offset);
		char *gc_add_header = gen_value_name(ctx);
		buffer_append_fmt(ctx, "  %s = getelementptr i8, i8* %s, i64 %d\n", gc_add_header, gc_data, struct_sz_bytes);

		char *gc_ptr = gen_value_name(ctx);
		buffer_append_fmt(ctx, "  %s = bitcast i8* %s to i32*\n", gc_ptr, gc_add_header);

		buffer_append_fmt(ctx, "  store i32* %s, i32** %s\n", gc_ptr, gc_gep);

		/* Handle field initialization: for each named field, fill with the init value */
		for (int init_idx = 1; init_idx < expr->data.alloc.field_count; init_idx++) {
			const char *field_name = expr->data.alloc.field_names[init_idx];
			HirExpr *init_value = expr->data.alloc.field_values[init_idx];

			/* Find field in archetype */
			int field_idx = -1;
			for (int i = 0; i < arch->field_count; i++) {
				if (strcmp(arch->fields[i]->name, field_name) == 0) {
					field_idx = i;
					break;
				}
			}
			if (field_idx == -1) {
				/* Field not found, semantic analysis should catch this */
				continue;
			}

			HirField *field = arch->fields[field_idx];
			if (field->kind != FIELD_COLUMN) {
				/* Only fill column fields */
				continue;
			}

			/* Get field's element type */
			const char *elem_type = llvm_type_from_arche(field_base_type_name(field->type));

			/* Load column pointer from struct */
			char *field_col_ptr_ref = gen_value_name(ctx);
			buffer_append_fmt(ctx, "  %s = getelementptr %%struct.%s, %%struct.%s* %s, i32 0, i32 %d\n",
			                  field_col_ptr_ref, arch_name, arch_name, struct_ptr, field_idx);
			char *field_col_ptr = gen_value_name(ctx);
			buffer_append_fmt(ctx, "  %s = load %s*, %s** %s\n", field_col_ptr, elem_type, elem_type,
			                  field_col_ptr_ref);

			/* Generate loop to fill field[0..count) with init value */
			char loop_cond_label[64], loop_body_label[64], loop_end_label[64];
			char *loop_ctr_alloca = gen_value_name(ctx);
			snprintf(loop_cond_label, sizeof(loop_cond_label), "loop_cond_%d", ctx->value_counter);
			snprintf(loop_body_label, sizeof(loop_body_label), "loop_body_%d", ctx->value_counter);
			snprintf(loop_end_label, sizeof(loop_end_label), "loop_end_%d", ctx->value_counter);
			ctx->value_counter++;

			/* Allocate loop counter variable on stack */
			emit_alloca(ctx, "  %s = alloca i64\n", loop_ctr_alloca);
			buffer_append_fmt(ctx, "  store i64 0, i64* %s\n", loop_ctr_alloca);

			/* Jump to loop condition */
			buffer_append_fmt(ctx, "  br label %%%s\n", loop_cond_label);

			/* Loop condition block */
			buffer_append_fmt(ctx, "%s:\n", loop_cond_label);
			char *loop_counter = gen_value_name(ctx);
			buffer_append_fmt(ctx, "  %s = load i64, i64* %s\n", loop_counter, loop_ctr_alloca);
			char *cond = gen_value_name(ctx);
			buffer_append_fmt(ctx, "  %s = icmp slt i64 %s, %s\n", cond, loop_counter, count_buf);
			buffer_append_fmt(ctx, "  br i1 %s, label %%%s, label %%%s\n", cond, loop_body_label, loop_end_label);

			/* Loop body */
			buffer_append_fmt(ctx, "%s:\n", loop_body_label);

			/* Compute field pointer for this element */
			char *elem_ptr = gen_value_name(ctx);
			buffer_append_fmt(ctx, "  %s = getelementptr %s, %s* %s, i64 %s\n", elem_ptr, elem_type, elem_type,
			                  field_col_ptr, loop_counter);

			/* Compute init value */
			char init_val_buf[256];
			codegen_expression(ctx, init_value, init_val_buf);

			/* Convert type if needed (e.g., i32 to double) */
			const char *init_type = hir_resolved_type_name(init_value);
			const char *init_llvm_type = llvm_type_from_arche(init_type);

			if (strcmp(init_llvm_type, elem_type) != 0) {
				char *converted = gen_value_name(ctx);
				if ((init_llvm_type[0] == 'i' || init_llvm_type[0] == 'd') &&
				    (elem_type[0] == 'i' || elem_type[0] == 'd')) {
					if (init_llvm_type[0] == 'i' && elem_type[0] == 'd') {
						buffer_append_fmt(ctx, "  %s = sitofp %s %s to %s\n", converted, init_llvm_type, init_val_buf,
						                  elem_type);
					} else if (init_llvm_type[0] == 'd' && elem_type[0] == 'i') {
						buffer_append_fmt(ctx, "  %s = fptosi %s %s to %s\n", converted, init_llvm_type, init_val_buf,
						                  elem_type);
					} else {
						converted = init_val_buf;
					}
				} else {
					converted = init_val_buf;
				}
				buffer_append_fmt(ctx, "  store %s %s, %s* %s\n", elem_type, converted, elem_type, elem_ptr);
			} else {
				buffer_append_fmt(ctx, "  store %s %s, %s* %s\n", elem_type, init_val_buf, elem_type, elem_ptr);
			}

			/* Increment loop counter */
			char *next_counter = gen_value_name(ctx);
			buffer_append_fmt(ctx, "  %s = add i64 %s, 1\n", next_counter, loop_counter);
			buffer_append_fmt(ctx, "  store i64 %s, i64* %s\n", next_counter, loop_ctr_alloca);
			buffer_append_fmt(ctx, "  br label %%%s\n", loop_cond_label);

			/* Loop end */
			buffer_append_fmt(ctx, "%s:\n", loop_end_label);
		}

		/* If we did any field initialization, update the count field */
		if (expr->data.alloc.field_count > 1) {
			char *final_count_gep = gen_value_name(ctx);
			buffer_append_fmt(ctx, "  %s = getelementptr %%struct.%s, %%struct.%s* %s, i32 0, i32 %d\n",
			                  final_count_gep, arch_name, arch_name, struct_ptr, arch->field_count);
			buffer_append_fmt(ctx, "  store i64 %s, i64* %s\n", count_buf, final_count_gep);
		}

		strcpy(result_buf, struct_ptr);
		break;
	}

	case HIR_EXPR_STRING: {
		/* Create global constant for string literal */
		char global_name[64];
		snprintf(global_name, sizeof(global_name), "@.str%d", ctx->string_counter++);

		/* Build LLVM global constant declaration (similar to emit_string_global) */
		char llvm_escaped[2048] = "";
		size_t llvm_pos = 0;

		for (int i = 0; i < expr->data.string.length && llvm_pos < sizeof(llvm_escaped) - 10; i++) {
			unsigned char c = (unsigned char)expr->data.string.value[i];
			if (c >= 32 && c < 127 && c != '"' && c != '\\') {
				llvm_escaped[llvm_pos++] = c;
			} else {
				llvm_pos += snprintf(llvm_escaped + llvm_pos, sizeof(llvm_escaped) - llvm_pos, "\\%02X", c);
			}
		}

		/* NUL-terminated `[N+1 x i8]` (the `\00` is the FFI provision past the content length N;
		 * the bind/arg records N as the array length). See emit_string_global. */
		char global_decl[4096];
		snprintf(global_decl, sizeof(global_decl), "%s = private unnamed_addr constant [%d x i8] c\"%s\\00\"\n",
		         global_name, expr->data.string.length + 1, llvm_escaped);

		/* Append to globals buffer */
		size_t decl_len = strlen(global_decl);
		if (ctx->globals_pos + decl_len >= ctx->globals_size) {
			ctx->globals_size = (ctx->globals_size + decl_len) * 2;
			ctx->globals_buffer = realloc(ctx->globals_buffer, ctx->globals_size);
		}
		strcpy(ctx->globals_buffer + ctx->globals_pos, global_decl);
		ctx->globals_pos += decl_len;

		/* Get pointer to first element */
		char *res_name = gen_value_name(ctx);
		buffer_append_fmt(ctx, "  %s = getelementptr [%d x i8], [%d x i8]* %s, i32 0, i32 0\n", res_name,
		                  expr->data.string.length + 1, expr->data.string.length + 1, global_name);
		strcpy(result_buf, res_name);
		break;
	}
	}
}

/* ========== BOUNDS CHECK HELPERS ========== */

static int resolve_index_arch(CodegenContext *ctx, HirExpr *base_expr, HirExpr *idx_expr, const char **out_arch_name,
                              const char **out_arch_ptr, int *out_count_idx, int *out_idx_is_i64) {
	*out_arch_name = NULL;
	*out_arch_ptr = NULL;
	*out_count_idx = -1;
	*out_idx_is_i64 = 0;

	/* Case 1: base is HIR_EXPR_FIELD with archetype backing (e.g., particles.mass[i]) */
	if (base_expr->kind == HIR_EXPR_FIELD && base_expr->data.field.base->kind == HIR_EXPR_NAME) {
		const char *var_name = base_expr->data.field.base->data.name.name;
		ValueInfo *vi = find_value(ctx, var_name);
		if (vi && vi->type == 3 && vi->arch_name) {
			HirArchetypeDecl *arch = find_archetype_decl(ctx, vi->arch_name);
			if (arch) {
				*out_arch_name = vi->arch_name;
				*out_arch_ptr = vi->llvm_name;
				*out_count_idx = arch->field_count;
			}
		} else {
			/* Try direct archetype name (for static allocations) */
			HirArchetypeDecl *arch = find_archetype_decl(ctx, var_name);
			if (arch) {
				static char global_buf[256];
				*out_arch_name = var_name;
				int cap = get_arch_static_capacity(ctx, var_name);
				if (cap > 0)
					snprintf(global_buf, sizeof(global_buf), "@%s", var_name);
				else
					snprintf(global_buf, sizeof(global_buf), "@archetype_%s", var_name);
				*out_arch_ptr = global_buf;
				*out_count_idx = arch->field_count;
			}
		}
	}
	/* Case 2: base is HIR_EXPR_NAME with type 4 (column param in sys) */
	else if (base_expr->kind == HIR_EXPR_NAME) {
		ValueInfo *vi = find_value(ctx, base_expr->data.name.name);
		if (vi && vi->type == 4 && vi->arch_name) {
			HirArchetypeDecl *arch = find_archetype_decl(ctx, vi->arch_name);
			if (arch) {
				static char arch_ptr_buf[256];
				*out_arch_name = vi->arch_name;
				snprintf(arch_ptr_buf, 256, "%%arch_%s", vi->arch_name);
				*out_arch_ptr = arch_ptr_buf;
				*out_count_idx = arch->field_count;
			}
		}
	}

	/* Determine if index is i64 */
	if (idx_expr->kind == HIR_EXPR_NAME) {
		ValueInfo *idx_vi = find_value(ctx, idx_expr->data.name.name);
		if (idx_vi && idx_vi->bit_width == 64) {
			*out_idx_is_i64 = 1;
		}
	}

	return (*out_arch_name != NULL && *out_arch_ptr != NULL && *out_count_idx >= 0);
}

/* If `expr` is an int literal whose value parses to a non-negative int, return that
 * value in *out and return 1. Otherwise return 0. */
static int try_extract_int_literal(HirExpr *expr, int *out) {
	if (!expr || expr->kind != HIR_EXPR_LITERAL || !expr->data.literal.lexeme)
		return 0;
	const char *s = expr->data.literal.lexeme;
	if (!*s)
		return 0;
	long v = strtol(s, NULL, 10);
	if (v < 0 || v > 2147483647L)
		return 0;
	*out = (int)v;
	return 1;
}

/* If the for-stmt has the shape `for (let v = <int_const>; v < BOUND; v += <step>)`
 * (where BOUND is also an int constant), populate out_var/out_bound and return 1.
 * Conservative: only matches the canonical 0..BOUND form actually used in practice. */
static int try_extract_loop_bound(HirStmt *for_stmt, char **out_var, int *out_bound) {
	if (!for_stmt || for_stmt->kind != HIR_STMT_FOR)
		return 0;
	HirStmt *init = for_stmt->data.for_stmt.init;
	HirExpr *cond = for_stmt->data.for_stmt.cond;
	if (!init || !cond)
		return 0;

	/* Init: a `let` declaration introducing the loop variable. */
	const char *var_name = NULL;
	if (init->kind == HIR_STMT_BIND && init->data.bind_stmt.name_count >= 1 && init->data.bind_stmt.names[0]) {
		var_name = init->data.bind_stmt.names[0];
	}
	if (!var_name)
		return 0;

	/* Cond: <var> < <int_literal> (also accept <=). */
	if (cond->kind != HIR_EXPR_BINARY)
		return 0;
	if (cond->data.binary.op != OP_LT && cond->data.binary.op != OP_LTE)
		return 0;
	HirExpr *cmp_left = cond->data.binary.left;
	HirExpr *cmp_right = cond->data.binary.right;
	if (!cmp_left || cmp_left->kind != HIR_EXPR_NAME)
		return 0;
	if (strcmp(cmp_left->data.name.name, var_name) != 0)
		return 0;

	int bound;
	if (!try_extract_int_literal(cmp_right, &bound))
		return 0;
	if (cond->data.binary.op == OP_LTE) {
		if (bound == 2147483647)
			return 0; /* would overflow */
		bound++;      /* `v <= N` means max `v` is N, so exclusive bound is N+1 */
	}

	*out_var = (char *)var_name;
	*out_bound = bound;
	return 1;
}

static void push_loop_bound(CodegenContext *ctx, const char *var_name, int bound) {
	if (ctx->loop_bound_count >= (int)(sizeof(ctx->loop_bounds) / sizeof(ctx->loop_bounds[0])))
		return; /* stack full — silently drop; only affects bounds-check elision, not correctness */
	int i = ctx->loop_bound_count++;
	strncpy(ctx->loop_bounds[i].var_name, var_name, sizeof(ctx->loop_bounds[i].var_name) - 1);
	ctx->loop_bounds[i].var_name[sizeof(ctx->loop_bounds[i].var_name) - 1] = '\0';
	ctx->loop_bounds[i].bound = bound;
}

static void pop_loop_bound(CodegenContext *ctx) {
	if (ctx->loop_bound_count > 0)
		ctx->loop_bound_count--;
}

/* If `idx_expr` references a loop variable currently in scope and we know the loop's
 * upper bound, return that bound. Otherwise return -1. */
static int lookup_loop_var_bound(CodegenContext *ctx, HirExpr *idx_expr) {
	if (!idx_expr || idx_expr->kind != HIR_EXPR_NAME)
		return -1;
	const char *name = idx_expr->data.name.name;
	for (int i = ctx->loop_bound_count - 1; i >= 0; i--) {
		if (strcmp(ctx->loop_bounds[i].var_name, name) == 0)
			return ctx->loop_bounds[i].bound;
	}
	return -1;
}

/* Returns 1 if a bounds check on `arch[idx_expr]` is provably unnecessary. */
static int bounds_check_elidable(CodegenContext *ctx, const char *arch_name, HirExpr *idx_expr) {
	if (!arch_name)
		return 0;
	int cap = get_arch_static_capacity(ctx, arch_name);
	if (cap <= 0)
		return 0; /* dynamic capacity — can't elide statically */
	/* The runtime check is `index < count`, not `index < capacity`. Elide only
	 * when the index is provably less than the static *count*. A static count of
	 * -1 (unknowable at compile time) or 0 (uninitialized archetype) means we
	 * cannot elide. */
	int count = get_arch_static_count(ctx, arch_name);
	if (count <= 0)
		return 0;
	int lit;
	if (try_extract_int_literal(idx_expr, &lit)) {
		return lit >= 0 && lit < count;
	}
	int loop_bound = lookup_loop_var_bound(ctx, idx_expr);
	if (loop_bound > 0 && loop_bound <= count)
		return 1;
	return 0;
}

static void emit_bounds_check(CodegenContext *ctx, const char *arch_name, const char *arch_ptr, int count_field_idx,
                              const char *idx_buf, int idx_is_i64) {
	/* If arch_ptr is a dynamic global (pointer-to-pointer), load the struct ptr first.
	   Static globals (@ArchName) are already %struct.X* — no load needed. */
	const char *struct_ptr = arch_ptr;
	char *loaded_ptr = NULL;
	if (strncmp(arch_ptr, "@archetype_", 11) == 0) {
		loaded_ptr = gen_value_name(ctx);
		buffer_append_fmt(ctx, "  %s = load %%struct.%s*, %%struct.%s** %s\n", loaded_ptr, arch_name, arch_name,
		                  arch_ptr);
		struct_ptr = loaded_ptr;
	}

	/* Load count from archetype struct */
	char *count_gep = gen_value_name(ctx);
	buffer_append_fmt(ctx, "  %s = getelementptr %%struct.%s, %%struct.%s* %s, i32 0, i32 %d\n", count_gep, arch_name,
	                  arch_name, struct_ptr, count_field_idx);
	char *count = gen_value_name(ctx);
	buffer_append_fmt(ctx, "  %s = load i64, i64* %s\n", count, count_gep);

	/* Extend index to i64 if needed */
	const char *idx64 = idx_buf;
	if (!idx_is_i64) {
		char *idx64_val = gen_value_name(ctx);
		buffer_append_fmt(ctx, "  %s = sext i32 %s to i64\n", idx64_val, idx_buf);
		idx64 = idx64_val;
	}

	/* Compare index < count */
	char *cmp = gen_value_name(ctx);
	buffer_append_fmt(ctx, "  %s = icmp ult i64 %s, %s\n", cmp, idx64, count);

	/* Generate branch labels */
	int chk_id = ctx->value_counter++;
	char ok_lbl[64], fail_lbl[64];
	snprintf(ok_lbl, sizeof(ok_lbl), "bounds_ok_%d", chk_id);
	snprintf(fail_lbl, sizeof(fail_lbl), "bounds_fail_%d", chk_id);

	buffer_append_fmt(ctx, "  br i1 %s, label %%%s, label %%%s\n\n", cmp, ok_lbl, fail_lbl);

	/* Emit error block */
	buffer_append_fmt(ctx, "%s:\n", fail_lbl);
	buffer_append(ctx,
	              "  call i32 @write(i32 2, i8* getelementptr ([28 x i8], [28 x i8]* @.arche_oob, i32 0, i32 0), i32 "
	              "27)\n");
	buffer_append(ctx, "  call void @abort()\n");
	buffer_append(ctx, "  unreachable\n\n");

	/* Emit ok block label (execution continues here) */
	buffer_append_fmt(ctx, "%s:\n", ok_lbl);
}

/* Bounds check against a compile-time element count N (for fixed-size arrays whose length is the
 * declared N, not a runtime count). Same abort idiom as emit_bounds_check; an unsigned compare also
 * catches a negative index (it becomes a huge u64). */
static void emit_const_bounds_check(CodegenContext *ctx, int n, const char *idx_buf, int idx_is_i64) {
	const char *idx64 = idx_buf;
	if (!idx_is_i64) {
		char *v = gen_value_name(ctx);
		buffer_append_fmt(ctx, "  %s = sext i32 %s to i64\n", v, idx_buf);
		idx64 = v;
	}
	char *cmp = gen_value_name(ctx);
	buffer_append_fmt(ctx, "  %s = icmp ult i64 %s, %d\n", cmp, idx64, n);
	int chk_id = ctx->value_counter++;
	char ok_lbl[64], fail_lbl[64];
	snprintf(ok_lbl, sizeof(ok_lbl), "bounds_ok_%d", chk_id);
	snprintf(fail_lbl, sizeof(fail_lbl), "bounds_fail_%d", chk_id);
	buffer_append_fmt(ctx, "  br i1 %s, label %%%s, label %%%s\n\n", cmp, ok_lbl, fail_lbl);
	buffer_append_fmt(ctx, "%s:\n", fail_lbl);
	buffer_append(ctx,
	              "  call i32 @write(i32 2, i8* getelementptr ([28 x i8], [28 x i8]* @.arche_oob, i32 0, i32 0), i32 "
	              "27)\n");
	buffer_append(ctx, "  call void @abort()\n");
	buffer_append(ctx, "  unreachable\n\n");
	buffer_append_fmt(ctx, "%s:\n", ok_lbl);
}

/* 1 if `idx_expr` is provably within [0, n): a literal in range, or a loop var bounded <= n. */
static int const_index_in_range(CodegenContext *ctx, HirExpr *idx_expr, int n) {
	int lit;
	if (try_extract_int_literal(idx_expr, &lit))
		return lit >= 0 && lit < n;
	int lb = lookup_loop_var_bound(ctx, idx_expr);
	return lb > 0 && lb <= n;
}

/* ========== WHOLE-COLUMN LOOP HELPER ========== */

/* Walk expression tree and pre-compute column base pointers to hoist them out of loop */
static void hoist_column_geps(CodegenContext *ctx, HirExpr *expr, const char *struct_ptr_val) {
	if (!expr)
		return;

	switch (expr->kind) {
	case HIR_EXPR_FIELD: {
		HirExpr *base = expr->data.field.base;
		if (base && base->kind == HIR_EXPR_NAME) {
			const char *arch_name = base->data.name.name;
			const char *field_name = expr->data.field.field_name;

			/* Find field index */
			HirArchetypeDecl *arch = find_archetype_decl(ctx, arch_name);
			if (arch) {
				int field_idx = -1;
				for (int i = 0; i < arch->field_count; i++) {
					if (strcmp(arch->fields[i]->name, field_name) == 0) {
						field_idx = i;
						break;
					}
				}

				if (field_idx >= 0 && arch->fields[field_idx]->kind == FIELD_COLUMN) {
					/* Generate GEP for column base once (loop-invariant) */
					const char *llvm_type = llvm_type_from_arche(field_base_type_name(arch->fields[field_idx]->type));
					int is_static = get_arch_static_capacity(ctx, arch_name) > 0;

					char *col_ptr = gen_value_name(ctx);
					if (is_static) {
						buffer_append_fmt(ctx,
						                  "  %s = getelementptr %%struct.%s, %%struct.%s* %s, i32 0, i32 %d, i64 0\n",
						                  col_ptr, arch_name, arch_name, struct_ptr_val, field_idx);
					} else {
						char *field_gep = gen_value_name(ctx);
						buffer_append_fmt(ctx, "  %s = getelementptr %%struct.%s, %%struct.%s* %s, i32 0, i32 %d\n",
						                  field_gep, arch_name, arch_name, struct_ptr_val, field_idx);
						buffer_append_fmt(ctx, "  %s = load %s*, %s** %s\n", col_ptr, llvm_type, llvm_type, field_gep);
					}
				}
			}
		}
		if (base)
			hoist_column_geps(ctx, base, struct_ptr_val);
		break;
	}
	case HIR_EXPR_BINARY:
		hoist_column_geps(ctx, expr->data.binary.left, struct_ptr_val);
		hoist_column_geps(ctx, expr->data.binary.right, struct_ptr_val);
		break;
	case HIR_EXPR_UNARY:
		hoist_column_geps(ctx, expr->data.unary.operand, struct_ptr_val);
		break;
	default:
		break;
	}
}

static void emit_whole_column_loop(CodegenContext *ctx, const char *col_ptr, /* SSA reg: scalar* column data */
                                   const char *count,                        /* SSA reg: i64 element count */
                                   const char *scalar_type,                  /* "double" or "i32" */
                                   const char *arche_type,                   /* "float" or "int" */
                                   HirExpr *rhs,                             /* RHS expression */
                                   int op,                     /* OP_NONE = store, others = load+op+store */
                                   const char *struct_ptr_val) /* struct pointer for hoisting */
{
	/* Hoist column base GEPs before loop to avoid recalculating them */
	if (struct_ptr_val) {
		hoist_column_geps(ctx, rhs, struct_ptr_val);
	}

	/* Align count down to 4-element boundary for vector loop */
	char *count_aligned = gen_value_name(ctx);
	buffer_append_fmt(ctx, "  %s = and i64 %s, -4\n", count_aligned, count);

	/* Vector loop setup */
	char *v_ctr_alloca = gen_value_name(ctx);
	emit_alloca(ctx, "  %s = alloca i64\n", v_ctr_alloca);
	buffer_append_fmt(ctx, "  store i64 0, i64* %s\n", v_ctr_alloca);

	/* Generate label names (without % prefix) */
	char vec_loop_lbl[32], vec_body_lbl[32], scalar_setup_lbl[32];
	char scalar_check_lbl[32], scalar_body_lbl[32], done_lbl[32];
	snprintf(vec_loop_lbl, sizeof(vec_loop_lbl), "vec_loop_%d", ctx->value_counter++);
	snprintf(vec_body_lbl, sizeof(vec_body_lbl), "vec_body_%d", ctx->value_counter++);
	snprintf(scalar_setup_lbl, sizeof(scalar_setup_lbl), "scalar_setup_%d", ctx->value_counter++);
	snprintf(scalar_check_lbl, sizeof(scalar_check_lbl), "scalar_check_%d", ctx->value_counter++);
	snprintf(scalar_body_lbl, sizeof(scalar_body_lbl), "scalar_body_%d", ctx->value_counter++);
	snprintf(done_lbl, sizeof(done_lbl), "done_%d", ctx->value_counter++);

	buffer_append_fmt(ctx, "  br label %%%s\n\n", vec_loop_lbl);

	/* Vector loop header */
	buffer_append_fmt(ctx, "%s:\n", vec_loop_lbl);
	char *vi = gen_value_name(ctx);
	buffer_append_fmt(ctx, "  %s = load i64, i64* %s\n", vi, v_ctr_alloca);
	char *vcond = gen_value_name(ctx);
	buffer_append_fmt(ctx, "  %s = icmp slt i64 %s, %s\n", vcond, vi, count_aligned);
	buffer_append_fmt(ctx, "  br i1 %s, label %%%s, label %%%s\n\n", vcond, vec_body_lbl, scalar_setup_lbl);

	/* Vector loop body */
	buffer_append_fmt(ctx, "%s:\n", vec_body_lbl);
	snprintf(ctx->implicit_loop_index, sizeof(ctx->implicit_loop_index), "%s", vi);

	/* Enable vectorization for float columns with vector type conversions for mixed types */
	if (strcmp(arche_type, "float") == 0 || strcmp(arche_type, "double") == 0) {
		ctx->vector_lanes = 4;
	} else {
		ctx->vector_lanes = 0;
	}

	/* Evaluate RHS with appropriate vector context */
	char rhs_buf[256];
	codegen_expression(ctx, rhs, rhs_buf);

	/* Handle compound ops */
	char *compute_result = malloc(256);
	strcpy(compute_result, rhs_buf);
	if (op != OP_NONE) {
		char *loaded = gen_value_name(ctx);
		char *gep = gen_value_name(ctx);
		buffer_append_fmt(ctx, "  %s = getelementptr %s, %s* %s, i64 %s\n", gep, scalar_type, scalar_type, col_ptr, vi);

		/* Load using vector type if vectorized */
		const char *load_type = elem_llvm_type(ctx, arche_type);
		char *load_src = gep;
		if (ctx->vector_lanes > 0) {
			char *vec_ptr = gen_value_name(ctx);
			buffer_append_fmt(ctx, "  %s = bitcast %s* %s to %s*\n", vec_ptr, scalar_type, gep, load_type);
			load_src = vec_ptr;
		}
		buffer_append_fmt(ctx, "  %s = load %s, %s* %s, align 8\n", loaded, load_type, load_type, load_src);

		const char *op_str;
		switch (op) {
		case OP_ADD:
			op_str = "fadd";
			break;
		case OP_SUB:
			op_str = "fsub";
			break;
		case OP_MUL:
			op_str = "fmul";
			break;
		case OP_DIV:
			op_str = "fdiv";
			break;
		default:
			op_str = "fadd";
			break;
		}
		char *op_result = gen_value_name(ctx);
		buffer_append_fmt(ctx, "  %s = %s %s %s, %s\n", op_result, op_str, load_type, loaded, rhs_buf);
		strcpy(compute_result, op_result);
	}

	/* Store result */
	char *target_gep = gen_value_name(ctx);
	buffer_append_fmt(ctx, "  %s = getelementptr %s, %s* %s, i64 %s\n", target_gep, scalar_type, scalar_type, col_ptr,
	                  vi);
	/* For vector stores, bitcast pointer to vector type */
	if (ctx->vector_lanes > 0) {
		const char *vec_type = elem_llvm_type(ctx, arche_type);
		char *vec_ptr = gen_value_name(ctx);
		buffer_append_fmt(ctx, "  %s = bitcast %s* %s to %s*\n", vec_ptr, scalar_type, target_gep, vec_type);
		buffer_append_fmt(ctx, "  store %s %s, %s* %s, align 8\n", vec_type, compute_result, vec_type, vec_ptr);
	} else {
		buffer_append_fmt(ctx, "  store %s %s, %s* %s, align 8\n", scalar_type, compute_result, scalar_type,
		                  target_gep);
	}

	/* Loop increment - use 4 for vectorized ops, 1 for scalar */
	char *vi_new = gen_value_name(ctx);
	int increment = ctx->vector_lanes > 0 ? 4 : 1;
	buffer_append_fmt(ctx, "  %s = add i64 %s, %d\n", vi_new, vi, increment);
	buffer_append_fmt(ctx, "  store i64 %s, i64* %s\n", vi_new, v_ctr_alloca);
	buffer_append_fmt(ctx, "  br label %%%s\n\n", vec_loop_lbl);

	/* Scalar loop setup */
	buffer_append_fmt(ctx, "%s:\n", scalar_setup_lbl);
	char *s_ctr_alloca = gen_value_name(ctx);
	emit_alloca(ctx, "  %s = alloca i64\n", s_ctr_alloca);
	buffer_append_fmt(ctx, "  store i64 %s, i64* %s\n", count_aligned, s_ctr_alloca);
	buffer_append_fmt(ctx, "  br label %%%s\n\n", scalar_check_lbl);

	/* Scalar loop check */
	buffer_append_fmt(ctx, "%s:\n", scalar_check_lbl);
	char *si = gen_value_name(ctx);
	buffer_append_fmt(ctx, "  %s = load i64, i64* %s\n", si, s_ctr_alloca);
	char *scond = gen_value_name(ctx);
	buffer_append_fmt(ctx, "  %s = icmp slt i64 %s, %s\n", scond, si, count);
	buffer_append_fmt(ctx, "  br i1 %s, label %%%s, label %%%s\n\n", scond, scalar_body_lbl, done_lbl);

	/* Scalar loop body */
	buffer_append_fmt(ctx, "%s:\n", scalar_body_lbl);
	ctx->vector_lanes = 0;
	snprintf(ctx->implicit_loop_index, sizeof(ctx->implicit_loop_index), "%s", si);

	/* Evaluate RHS with implicit loop index set */
	codegen_expression(ctx, rhs, rhs_buf);

	/* Handle compound ops */
	strcpy(compute_result, rhs_buf);
	if (op != OP_NONE) {
		char *loaded = gen_value_name(ctx);
		char *gep = gen_value_name(ctx);
		buffer_append_fmt(ctx, "  %s = getelementptr %s, %s* %s, i64 %s\n", gep, scalar_type, scalar_type, col_ptr, si);
		buffer_append_fmt(ctx, "  %s = load %s, %s* %s, align 4\n", loaded, scalar_type, scalar_type, gep);

		const char *op_str;
		switch (op) {
		case OP_ADD:
			op_str = "fadd";
			break;
		case OP_SUB:
			op_str = "fsub";
			break;
		case OP_MUL:
			op_str = "fmul";
			break;
		case OP_DIV:
			op_str = "fdiv";
			break;
		default:
			op_str = "fadd";
			break;
		}
		char *op_result = gen_value_name(ctx);
		buffer_append_fmt(ctx, "  %s = %s %s %s, %s\n", op_result, op_str, scalar_type, loaded, rhs_buf);
		strcpy(compute_result, op_result);
	}

	/* Store result */
	target_gep = gen_value_name(ctx);
	buffer_append_fmt(ctx, "  %s = getelementptr %s, %s* %s, i64 %s\n", target_gep, scalar_type, scalar_type, col_ptr,
	                  si);
	buffer_append_fmt(ctx, "  store %s %s, %s* %s, align 4\n", scalar_type, compute_result, scalar_type, target_gep);

	/* Loop increment */
	char *si_new = gen_value_name(ctx);
	buffer_append_fmt(ctx, "  %s = add i64 %s, 1\n", si_new, si);
	buffer_append_fmt(ctx, "  store i64 %s, i64* %s\n", si_new, s_ctr_alloca);
	buffer_append_fmt(ctx, "  br label %%%s\n\n", scalar_check_lbl);

	/* Done */
	buffer_append_fmt(ctx, "%s:\n", done_lbl);
	ctx->implicit_loop_index[0] = '\0';
	ctx->vector_lanes = 0;

	free(compute_result);
}

/* ========== STATEMENT CODEGEN ========== */

static void codegen_statement(CodegenContext *ctx, HirStmt *stmt) {
	if (!stmt)
		return;

	switch (stmt->kind) {
	case HIR_STMT_BIND: {
		const char *var_name = stmt->data.bind_stmt.names[0];
		char value_buf[256];

		/* Handle type-annotated declaration without initialization */
		if (stmt->data.bind_stmt.type && !stmt->data.bind_stmt.value) {
			HirType *type = stmt->data.bind_stmt.type;

			/* Check for array type */
			if (type->tag == HIR_TYPE_ARRAY) {
				/* `let s: T[];` with no initializer: an empty `(ptr=null, len=0, cap=0)` slice. A
				 * normal type-6 array — no arche_array. (Usually rebound before use.) */
				const char *en = field_base_type_name(type);
				const char *lt = llvm_type_from_arche(en);
				ValueScope *scope = &ctx->scopes[ctx->scope_count - 1];
				ValueInfo *vi = calloc(1, sizeof(ValueInfo));
				vi->name = malloc(strlen(var_name) + 1);
				strcpy(vi->name, var_name);
				vi->llvm_name = strdup("null");
				vi->type = 6;
				vi->arch_name = NULL;
				vi->string_len = -1;
				vi->field_type = en;
				vi->bit_width = strcmp(lt, "double") == 0 ? 64 : (strcmp(lt, "i8") == 0 ? 8 : 32);
				vi->is_slice = 1;
				vi->len_ssa = strdup("0");
				vi->cap_ssa = strdup("0");
				scope->values = realloc(scope->values, (scope->value_count + 1) * sizeof(ValueInfo *));
				scope->values[scope->value_count++] = vi;
			} else if (type->tag == HIR_TYPE_SHAPED_ARRAY) {
				/* Stack-allocated shaped array (`buf: int[8]`, `float[4]`, `char[256]`, …): allocate
				 * `[N x T]` and expose a first-element pointer as a type-6 value. The general index
				 * read/write paths handle element typing from field_type — char is just `i8` here,
				 * a normal array, no type-7/arche_array special case. */
				int rank = type->rank;
				const char *en = field_base_type_name(type); /* "int", "float", "char", … */
				const char *lt = llvm_type_from_arche(en);   /* "i32", "double", "i8", … */
				char *alloca_name = gen_value_name(ctx);
				emit_alloca(ctx, "  %s = alloca [%d x %s]\n", alloca_name, rank, lt);
				char *elem0 = gen_value_name(ctx);
				buffer_append_fmt(ctx, "  %s = getelementptr [%d x %s], [%d x %s]* %s, i64 0, i64 0\n", elem0, rank, lt,
				                  rank, lt, alloca_name);

				ValueInfo *vi = calloc(1, sizeof(ValueInfo));
				vi->name = malloc(strlen(var_name) + 1);
				strcpy(vi->name, var_name);
				vi->llvm_name = malloc(strlen(elem0) + 1);
				strcpy(vi->llvm_name, elem0);
				vi->type = 6; /* element pointer; field_type drives element typing */
				vi->arch_name = NULL;
				vi->string_len = rank;
				vi->field_type = en;
				vi->bit_width = strcmp(lt, "double") == 0 ? 64 : (strcmp(lt, "i8") == 0 ? 8 : 32);

				if (ctx->scope_count > 0) {
					ValueScope *scope = &ctx->scopes[ctx->scope_count - 1];
					scope->values = realloc(scope->values, (scope->value_count + 1) * sizeof(ValueInfo *));
					scope->values[scope->value_count++] = vi;
				}
			} else {
				/* Scalar type: allocate and zero-initialize */
				const char *alloc_type = "i32";
				if (type->tag == HIR_TYPE_FLOAT) {
					alloc_type = "double";
				}

				char *alloca_name = gen_value_name(ctx);
				emit_alloca(ctx, "  %s = alloca %s\n", alloca_name, alloc_type);
				buffer_append_fmt(ctx, "  store %s 0, %s* %s\n", alloc_type, alloc_type, alloca_name);

				ValueInfo *vi = calloc(1, sizeof(ValueInfo));
				vi->name = malloc(strlen(var_name) + 1);
				strcpy(vi->name, var_name);
				vi->llvm_name = malloc(strlen(alloca_name) + 1);
				strcpy(vi->llvm_name, alloca_name);
				vi->type = 1;
				vi->arch_name = NULL;
				vi->string_len = -1;
				vi->field_type = field_base_type_name(type);
				vi->bit_width = strcmp(alloc_type, "double") == 0 ? 64 : 32;

				if (ctx->scope_count > 0) {
					ValueScope *scope = &ctx->scopes[ctx->scope_count - 1];
					scope->values = realloc(scope->values, (scope->value_count + 1) * sizeof(ValueInfo *));
					scope->values[scope->value_count++] = vi;
				}
			}
			break;
		}

		/* Check if the value is a string literal (old style) or array literal (new style from string expansion) */
		int is_string = 0;
		if (stmt->data.bind_stmt.value) {
			if (stmt->data.bind_stmt.value->kind == HIR_EXPR_LITERAL &&
			    stmt->data.bind_stmt.value->data.literal.lexeme[0] == '"') {
				is_string = 1;
			} else if (stmt->data.bind_stmt.value->kind == HIR_EXPR_STRING) {
				/* HIR_EXPR_STRING is also a string */
				is_string = 1;
			} else if (stmt->data.bind_stmt.value->kind == HIR_EXPR_ARRAY_LITERAL) {
				/* Array literal from string expansion */
				is_string = 1;
			}
		}

		/* Check if value is an alloc expression */
		int is_alloc = 0;
		const char *alloc_arch_name = NULL;
		if (stmt->data.bind_stmt.value && stmt->data.bind_stmt.value->kind == HIR_EXPR_ALLOC) {
			is_alloc = 1;
			alloc_arch_name = stmt->data.bind_stmt.value->data.alloc.archetype_name;
		}

		/* Detect: let row = arch.field[entity] where field is shaped array */
		int is_multidim_slice = 0;
		const char *slice_elem_type = NULL;
		if (stmt->data.bind_stmt.value && stmt->data.bind_stmt.value->kind == HIR_EXPR_INDEX &&
		    stmt->data.bind_stmt.value->data.index.index_count == 1) {
			slice_elem_type = get_shaped_field_info(ctx, stmt->data.bind_stmt.value->data.index.base, NULL);
			if (slice_elem_type)
				is_multidim_slice = 1;
		}

		/* `ys := buf[lo:hi]` — bind a read-only slice view: register a type-6 slice (ptr + runtime
		 * length) so `ys[i]` / `ys.length` work; no copy. */
		if (stmt->data.bind_stmt.value && stmt->data.bind_stmt.value->kind == HIR_EXPR_SLICE) {
			char sp[256], sl[256], scp[256];
			const char *se;
			int sw;
			if (codegen_slice(ctx, stmt->data.bind_stmt.value, sp, sl, scp, &se, &sw)) {
				ValueInfo *vi = calloc(1, sizeof(ValueInfo));
				vi->name = malloc(strlen(var_name) + 1);
				strcpy(vi->name, var_name);
				vi->llvm_name = malloc(strlen(sp) + 1);
				strcpy(vi->llvm_name, sp);
				vi->type = 6;
				vi->is_slice = 1;
				vi->len_ssa = malloc(strlen(sl) + 1);
				strcpy(vi->len_ssa, sl);
				vi->cap_ssa = malloc(strlen(scp) + 1);
				strcpy(vi->cap_ssa, scp);
				vi->field_type = se;
				vi->string_len = -1;
				vi->bit_width = sw;
				if (ctx->scope_count > 0) {
					ValueScope *scope = &ctx->scopes[ctx->scope_count - 1];
					scope->values = realloc(scope->values, (scope->value_count + 1) * sizeof(ValueInfo *));
					scope->values[scope->value_count++] = vi;
				}
			}
			break;
		}

		if (stmt->data.bind_stmt.value) {
			codegen_expression(ctx, stmt->data.bind_stmt.value, value_buf);
		} else {
			strcpy(value_buf, "0");
		}

		/* A call that returns a char array — unbounded `char[]` or sized `char[N]` — yields a
		 * raw i8* byte view; bind it as type-6 so `b := f(move b)` is indexable. Check the
		 * resolved tag, and (since a single `char[N]` return resolves to a shaped type) the
		 * callee's declared return type directly. */
		int is_char_array_call = 0;
		const char *arr_elem = "char"; /* element base name of an array-returning call */
		int arr_n = -1;                /* N for a shaped (bounded) return, -1 for unbounded */
		int is_slice_call = 0;         /* non-char `T[]` return = a `{T*, i64}` fat pointer */
		if (stmt->data.bind_stmt.value && stmt->data.bind_stmt.value->kind == HIR_EXPR_CALL) {
			/* The callee's DECLARED return type fixes the ABI: an unbounded `T[]` return is a
			 * `{T*, i64}` fat pointer (extract ptr+len); a sized `T[N]` return is a bare element
			 * pointer. The call's *resolved* tag is CHAR_ARRAY for both, so it can't distinguish —
			 * always prefer the declared type, falling back to the slice form only with no decl. */
			HirFuncDecl *cf = (stmt->data.bind_stmt.value->data.call.callee &&
			                   stmt->data.bind_stmt.value->data.call.callee->kind == HIR_EXPR_NAME)
			                      ? find_func_decl(ctx, stmt->data.bind_stmt.value->data.call.callee->data.name.name)
			                      : NULL;
			if (cf && cf->return_type_count == 1 && cf->return_types[0] && cf->return_types[0]->tag == HIR_TYPE_ARRAY) {
				is_slice_call = 1;
				arr_elem = field_base_type_name(cf->return_types[0]);
				arr_n = -1;
			} else if (cf && cf->return_type_count == 1 && cf->return_types[0] &&
			           cf->return_types[0]->tag == HIR_TYPE_SHAPED_ARRAY) {
				is_char_array_call = 1;
				arr_elem = field_base_type_name(cf->return_types[0]);
				arr_n = cf->return_types[0]->rank; /* bounded → enables .length and bounds checks */
			} else if (!cf && stmt->data.bind_stmt.value->resolved.tag == HIR_TYPE_CHAR_ARRAY) {
				/* No callee decl (extern result, indirect) — treat as an unbounded char[] slice. */
				is_slice_call = 1;
				arr_elem = "char";
				arr_n = -1;
			}
		}

		/* `b := a`, `b := move a`, `b := copy a` where `a` is an array/slice value: the target takes
		 * over (move/copy) the source's storage. value_buf already holds the right pointer — the
		 * source's own pointer for a move (alias; the source binding is consumed in semantic) or a
		 * freshly-cloned buffer for `copy`. Clone the source's array ValueInfo metadata onto the
		 * target so element typing, count/length, and slice-ness carry over. */
		ValueInfo *src_arr_vi = NULL;
		if (stmt->data.bind_stmt.value) {
			HirExpr *rv = stmt->data.bind_stmt.value;
			while (rv && rv->kind == HIR_EXPR_UNARY &&
			       (rv->data.unary.op == UNARY_MOVE || rv->data.unary.op == UNARY_COPY))
				rv = rv->data.unary.operand;
			if (rv && rv->kind == HIR_EXPR_NAME) {
				ValueInfo *sv = find_value(ctx, rv->data.name.name);
				if (sv && (sv->type == 6 || sv->type == 7))
					src_arr_vi = sv;
			}
		}

		if (src_arr_vi) {
			ValueInfo *vi = calloc(1, sizeof(ValueInfo));
			vi->name = malloc(strlen(var_name) + 1);
			strcpy(vi->name, var_name);
			vi->llvm_name = malloc(strlen(value_buf) + 1);
			strcpy(vi->llvm_name, value_buf);
			vi->type = src_arr_vi->type;
			vi->arch_name = NULL;
			vi->string_len = src_arr_vi->string_len;
			vi->field_type = src_arr_vi->field_type;
			vi->bit_width = src_arr_vi->bit_width;
			vi->is_slice = src_arr_vi->is_slice;
			if (src_arr_vi->len_ssa) {
				vi->len_ssa = malloc(strlen(src_arr_vi->len_ssa) + 1);
				strcpy(vi->len_ssa, src_arr_vi->len_ssa);
			}
			if (ctx->scope_count > 0) {
				ValueScope *scope = &ctx->scopes[ctx->scope_count - 1];
				scope->values = realloc(scope->values, (scope->value_count + 1) * sizeof(ValueInfo *));
				scope->values[scope->value_count++] = vi;
			}
		} else if (is_slice_call) {
			/* The call returned a fat pointer `{T*, i64}` in value_buf. Extract the element pointer
			 * and runtime length and bind a type-6 slice so `r[i]` / `r.length` work on the result. */
			const char *lt = llvm_type_from_arche(arr_elem);
			char ptrt[16];
			snprintf(ptrt, sizeof(ptrt), "%s*", lt);
			char *ptr = gen_value_name(ctx);
			buffer_append_fmt(ctx, "  %s = extractvalue { %s, i64 } %s, 0\n", ptr, ptrt, value_buf);
			char *len = gen_value_name(ctx);
			buffer_append_fmt(ctx, "  %s = extractvalue { %s, i64 } %s, 1\n", len, ptrt, value_buf);
			ValueInfo *vi = calloc(1, sizeof(ValueInfo));
			vi->name = malloc(strlen(var_name) + 1);
			strcpy(vi->name, var_name);
			vi->llvm_name = malloc(strlen(ptr) + 1);
			strcpy(vi->llvm_name, ptr);
			vi->type = 6;
			vi->arch_name = NULL;
			vi->string_len = -1;
			vi->field_type = arr_elem;
			vi->bit_width = strcmp(lt, "double") == 0 ? 64 : (strcmp(lt, "i8") == 0 ? 8 : 32);
			vi->is_slice = 1;
			vi->len_ssa = malloc(strlen(len) + 1);
			strcpy(vi->len_ssa, len);
			if (ctx->scope_count > 0) {
				ValueScope *scope = &ctx->scopes[ctx->scope_count - 1];
				scope->values = realloc(scope->values, (scope->value_count + 1) * sizeof(ValueInfo *));
				scope->values[scope->value_count++] = vi;
			}
		} else if (is_char_array_call) {
			/* A func returning an array hands back an element pointer (by reference). Bind the
			 * result as a type-6 array carrying the element type (so `r[i]` indexes at the right
			 * width) and, for a bounded `T[N]` return, the count N (so .length / bounds work). */
			const char *lt = llvm_type_from_arche(arr_elem);
			ValueInfo *vi = calloc(1, sizeof(ValueInfo));
			vi->name = malloc(strlen(var_name) + 1);
			strcpy(vi->name, var_name);
			vi->llvm_name = malloc(strlen(value_buf) + 1);
			strcpy(vi->llvm_name, value_buf);
			vi->type = 6;
			vi->arch_name = NULL;
			vi->string_len = arr_n;
			vi->field_type = arr_elem;
			vi->bit_width = strcmp(lt, "double") == 0 ? 64 : (strcmp(lt, "i8") == 0 ? 8 : 32);
			if (ctx->scope_count > 0) {
				ValueScope *scope = &ctx->scopes[ctx->scope_count - 1];
				scope->values = realloc(scope->values, (scope->value_count + 1) * sizeof(ValueInfo *));
				scope->values[scope->value_count++] = vi;
			}
		} else if (is_multidim_slice) {
			/* value_buf is a raw pointer — store as type-6 ValueInfo */
			ValueInfo *vi = calloc(1, sizeof(ValueInfo));
			vi->name = malloc(strlen(var_name) + 1);
			strcpy(vi->name, var_name);
			vi->llvm_name = malloc(strlen(value_buf) + 1);
			strcpy(vi->llvm_name, value_buf);
			vi->type = 6; /* typed column slice pointer */
			vi->arch_name = NULL;
			vi->string_len = -1;
			vi->field_type = slice_elem_type;
			vi->bit_width = 64;
			if (ctx->scope_count > 0) {
				ValueScope *scope = &ctx->scopes[ctx->scope_count - 1];
				scope->values = realloc(scope->values, (scope->value_count + 1) * sizeof(ValueInfo *));
				scope->values[scope->value_count++] = vi;
			}
		} else if (is_alloc && alloc_arch_name) {
			/* Track as arch pointer (type 3) */
			add_arch_value(ctx, var_name, value_buf, alloc_arch_name);
		} else if (is_string) {
			/* For strings, handle based on type */
			if (stmt->data.bind_stmt.value->kind == HIR_EXPR_ARRAY_LITERAL) {
				/* Array literal: value_buf is the element pointer into the global `[N x i8]`; bind a
				 * type-6 bounded char array carrying the element count. */
				int n = stmt->data.bind_stmt.value->data.array_literal.element_count;
				ValueScope *scope = &ctx->scopes[ctx->scope_count - 1];
				ValueInfo *vi = calloc(1, sizeof(ValueInfo));
				vi->name = malloc(strlen(var_name) + 1);
				strcpy(vi->name, var_name);
				vi->llvm_name = malloc(strlen(value_buf) + 1);
				strcpy(vi->llvm_name, value_buf);
				vi->type = 6;
				vi->arch_name = NULL;
				vi->string_len = n;
				vi->field_type = "char";
				vi->bit_width = 8;
				scope->values = realloc(scope->values, (scope->value_count + 1) * sizeof(ValueInfo *));
				scope->values[scope->value_count++] = vi;
			} else {
				/* `s := "..."`: a string literal binds as a NORMAL type-6 bounded char array — the
				 * element pointer into the bare `[N x i8]` global, carrying the content count N. So
				 * `s.length` is N, `s[i]` is bounds-checked, and there is no NUL terminator. */
				ValueInfo *vi = calloc(1, sizeof(ValueInfo));
				vi->name = malloc(strlen(var_name) + 1);
				strcpy(vi->name, var_name);
				vi->llvm_name = malloc(strlen(value_buf) + 1);
				strcpy(vi->llvm_name, value_buf);
				vi->type = 6; /* bounded char array (element pointer + count) */
				vi->arch_name = NULL;
				vi->string_len = -1;
				{
					HirExpr *sv = stmt->data.bind_stmt.value;
					if (sv->kind == HIR_EXPR_STRING) {
						vi->string_len = (int)sv->data.string.length;
					} else if (sv->kind == HIR_EXPR_LITERAL && sv->data.literal.lexeme &&
					           sv->data.literal.lexeme[0] == '"') {
						const char *lex = sv->data.literal.lexeme;
						int sl = 0;
						for (int j = 1; lex[j] != '"' && lex[j] != '\0'; j++) {
							if (lex[j] == '\\' && lex[j + 1] != '\0') {
								j++;
							}
							sl++;
						}
						vi->string_len = sl;
					}
				}
				vi->field_type = "char";
				vi->bit_width = 8;

				if (ctx->scope_count > 0) {
					ValueScope *scope = &ctx->scopes[ctx->scope_count - 1];
					scope->values = realloc(scope->values, (scope->value_count + 1) * sizeof(ValueInfo *));
					scope->values[scope->value_count++] = vi;
				} else {
					free(vi->name);
					free(vi->llvm_name);
					free(vi);
				}
			}
		} else {
			/* For scalars (int/float/handle), allocate and store based on type */
			const char *alloc_type = "i32";
			const char *store_type = "i32";
			int bit_width = 32;
			const char *resolved_type = NULL;

			/* Check if RHS is an insert call (returns handle/i64) */
			int is_insert_call = 0;
			const char *insert_archetype = NULL;
			if (stmt->data.bind_stmt.value && stmt->data.bind_stmt.value->kind == HIR_EXPR_CALL) {
				const char *func_name = NULL;
				if (stmt->data.bind_stmt.value->data.call.callee &&
				    stmt->data.bind_stmt.value->data.call.callee->kind == HIR_EXPR_NAME) {
					func_name = stmt->data.bind_stmt.value->data.call.callee->data.name.name;
				}
				if (func_name && strcmp(func_name, "insert") == 0) {
					is_insert_call = 1;
					alloc_type = "i64";
					store_type = "i64";
					bit_width = 64;
					resolved_type = "handle";
					/* Extract archetype from insert's first argument */
					if (stmt->data.bind_stmt.value->data.call.arg_count > 0 &&
					    stmt->data.bind_stmt.value->data.call.args[0]->kind == HIR_EXPR_NAME) {
						insert_archetype = stmt->data.bind_stmt.value->data.call.args[0]->data.name.name;
					}
				}
			}

			/* Check the resolved type of the value expression */
			if (!is_insert_call && stmt->data.bind_stmt.value &&
			    stmt->data.bind_stmt.value->resolved.tag != HIR_TYPE_UNKNOWN) {
				resolved_type = hir_resolved_type_name(stmt->data.bind_stmt.value);
				if (strcmp(resolved_type, "double") == 0 || strcmp(resolved_type, "float") == 0) {
					alloc_type = "double";
					store_type = "double";
					bit_width = 64;
				} else if (strcmp(resolved_type, "handle") == 0) {
					alloc_type = "i64";
					store_type = "i64";
					bit_width = 64;
				} else if (strcmp(resolved_type, "opaque") == 0) {
					/* foreign resource value: pointer-width cell held as i64 */
					alloc_type = "i64";
					store_type = "i64";
					bit_width = 64;
				} else if (stmt->data.bind_stmt.value->resolved.tag == HIR_TYPE_INT &&
				           stmt->data.bind_stmt.value->resolved.int_width != 32) {
					/* RHS is a non-i32 integer (e.g. syscall -> i64): infer its width
					   instead of defaulting to i32 and truncating. */
					int w = stmt->data.bind_stmt.value->resolved.int_width;
					alloc_type = llvm_int_type(w);
					store_type = alloc_type;
					bit_width = w;
				}
			}

			/* A fixed-width int annotation (let x: i64 = ...) sets the storage
			 * width; the value is sext/zext/trunc'd to match (literals adopt the
			 * annotated width per the language's literal-typing rule). */
			HirType *ann = stmt->data.bind_stmt.type;
			if (ann && ann->tag == HIR_TYPE_INT && ann->int_width != 32) {
				const char *wt = llvm_int_type(ann->int_width);
				char converted[256];
				emit_int_convert(ctx, value_buf, &stmt->data.bind_stmt.value->resolved, ann->int_width, converted);
				strcpy(value_buf, converted);
				alloc_type = wt;
				store_type = wt;
				bit_width = ann->int_width;
				resolved_type = field_base_type_name(ann);
			}

			char *alloca_name = gen_value_name(ctx);
			emit_alloca(ctx, "  %s = alloca %s\n", alloca_name, alloc_type);
			buffer_append_fmt(ctx, "  store %s %s, %s* %s\n", store_type, value_buf, store_type, alloca_name);

			ValueInfo *vi = calloc(1, sizeof(ValueInfo));
			vi->name = malloc(strlen(var_name) + 1);
			strcpy(vi->name, var_name);
			vi->llvm_name = malloc(strlen(alloca_name) + 1);
			strcpy(vi->llvm_name, alloca_name);
			vi->type = 1;
			vi->arch_name = NULL;
			vi->string_len = -1;
			vi->field_type = resolved_type ? resolved_type : "int";
			vi->handle_archetype = NULL;
			if (insert_archetype) {
				vi->handle_archetype = insert_archetype; /* borrowed HIR name */
			}
			/* Propagate the handle's archetype through a copy (let alias := h) so
			 * delete(alias) can infer the pool. */
			if (!vi->handle_archetype && stmt->data.bind_stmt.value &&
			    stmt->data.bind_stmt.value->kind == HIR_EXPR_NAME) {
				ValueInfo *src = find_value(ctx, stmt->data.bind_stmt.value->data.name.name);
				if (src && src->handle_archetype) {
					vi->handle_archetype = src->handle_archetype; /* borrowed from the source value */
				}
			}
			vi->bit_width = bit_width;

			if (ctx->scope_count > 0) {
				ValueScope *scope = &ctx->scopes[ctx->scope_count - 1];
				scope->values = realloc(scope->values, (scope->value_count + 1) * sizeof(ValueInfo *));
				scope->values[scope->value_count++] = vi;
			} else {
				free(vi->name);
				free(vi->llvm_name);
				free(vi);
			}
		}
		break;
	}

	case HIR_STMT_MULTI_BIND: {
		/* `(t0, …, tk) := f(…)` binds the members of a multi-value return. The call emits an
		 * aggregate `{T0, …, Tk}` (via the EXPR_CALL path); each target gets one extractvalue. */
		HirExpr *rhs = stmt->data.multi_bind.value;
		int target_count = stmt->data.multi_bind.target_count;
		HirBindingTarget *targets = stmt->data.multi_bind.targets;
		const char *fn =
		    (rhs && rhs->kind == HIR_EXPR_CALL && rhs->data.call.callee && rhs->data.call.callee->kind == HIR_EXPR_NAME)
		        ? rhs->data.call.callee->data.name.name
		        : NULL;
		/* Inside a callback specialization, a call to a callback param resolves to
		 * its bound proc (matches the EXPR_CALL path). */
		fn = cb_resolve(ctx, fn);
		HirFuncDecl *callee_func = fn ? find_func_decl(ctx, fn) : NULL;
		HirProcDecl *callee_proc = (!callee_func && fn) ? find_proc_decl(ctx, fn) : NULL;

		/* `_` placeholder in an in-out in-slot: it names no value. The buffer lives in the out-list,
		 * named by the positionally-matching out-target. Rewrite each such `_` arg's NAME to that
		 * out-target buffer in place for the duration of this call's emission, so the existing
		 * array-passing logic just passes that buffer by reference (no copy). Restore afterward so
		 * the HIR is not permanently mutated. */
		char *saved_uscore_names[16];
		HirExpr *saved_uscore_args[16];
		int saved_uscore_count = 0;
		/* Copy-elision: a `copy <name>` arg into an in-out in-slot whose out-target IS `<name>`
		 * needs no actual copy (the caller already consents to `<name>` being written — it is the
		 * out place that gets rebound). Temporarily replace the arg with the copy's operand so the
		 * buffer is passed by reference (no memcpy). Restored after emission. */
		HirExpr **saved_copy_slots[16];
		HirExpr *saved_copy_vals[16];
		int saved_copy_count = 0;
		if (callee_proc && rhs && rhs->kind == HIR_EXPR_CALL) {
			for (int i = 0; i < rhs->data.call.arg_count && i < callee_proc->param_count; i++) {
				HirExpr *a = rhs->data.call.args[i];
				if (!a)
					continue;
				if (!proc_out_param_is_inout_in(callee_proc, i))
					continue;
				/* The out-target (positionally matching out-param) buffer name for this in-out slot. */
				const char *pn = callee_proc->params[i]->name;
				const char *bufname = NULL;
				for (int oi = 0; oi < callee_proc->out_param_count && oi < target_count; oi++) {
					if (callee_proc->out_params[oi]->name && pn && strcmp(callee_proc->out_params[oi]->name, pn) == 0) {
						bufname = targets[oi].name;
						break;
					}
				}
				if (!bufname)
					continue;
				/* `_` placeholder: rewrite its NAME to the out-target buffer. */
				if (a->kind == HIR_EXPR_NAME && a->data.name.name && strcmp(a->data.name.name, "_") == 0) {
					if (saved_uscore_count < 16) {
						saved_uscore_args[saved_uscore_count] = a;
						saved_uscore_names[saved_uscore_count] = a->data.name.name;
						a->data.name.name = (char *)bufname; /* borrowed; restored below */
						saved_uscore_count++;
					}
					continue;
				}
				/* `copy <name>` where <name> == the out-target buffer: elide the copy node. */
				if (a->kind == HIR_EXPR_UNARY && a->data.unary.op == UNARY_COPY && a->data.unary.operand &&
				    a->data.unary.operand->kind == HIR_EXPR_NAME && a->data.unary.operand->data.name.name &&
				    strcmp(a->data.unary.operand->data.name.name, bufname) == 0 && saved_copy_count < 16) {
					saved_copy_slots[saved_copy_count] = &rhs->data.call.args[i];
					saved_copy_vals[saved_copy_count] = a;
					rhs->data.call.args[i] = a->data.unary.operand; /* skip the copy; pass operand directly */
					saved_copy_count++;
				}
			}
		}

		/* Extern proc call with capture `ext(in)(out)`: C writes in-place buffers through the in-args
		 * and returns one value. Emit the C call (its result is the out-only out-param), then store
		 * that result into the out-only target. In-out targets are written in place — skipped here. */
		if (callee_proc && callee_proc->is_extern) {
			char res[256];
			codegen_expression(ctx, rhs, res); /* emits `%res = call <cret> @ext(in-args)` */
			for (int ui = 0; ui < saved_uscore_count; ui++)
				saved_uscore_args[ui]->data.name.name = saved_uscore_names[ui]; /* restore `_` */
			for (int ui = 0; ui < saved_copy_count; ui++)
				*saved_copy_slots[ui] = saved_copy_vals[ui]; /* restore elided `copy` node */
			for (int i = 0; i < target_count && i < callee_proc->out_param_count; i++) {
				if (proc_out_param_is_inout(callee_proc, i))
					continue;
				HirBindingTarget *tgt = &targets[i];
				HirType *ot = callee_proc->out_params[i]->type;
				int is_arr = ot && (ot->tag == HIR_TYPE_ARRAY || ot->tag == HIR_TYPE_SHAPED_ARRAY);
				const char *elem = return_member_llvm(ot);
				if (tgt->is_new) {
					if (is_arr) {
						/* an array C-return is a raw i8* byte view — bind type-6 (value IS the ptr). */
						ValueInfo *vi = calloc(1, sizeof(ValueInfo));
						vi->name = malloc(strlen(tgt->name) + 1);
						strcpy(vi->name, tgt->name);
						vi->llvm_name = malloc(strlen(res) + 1);
						strcpy(vi->llvm_name, res);
						vi->type = 6;
						vi->string_len = -1;
						vi->field_type = "char";
						vi->bit_width = 8;
						if (ctx->scope_count > 0) {
							ValueScope *sc = &ctx->scopes[ctx->scope_count - 1];
							sc->values = realloc(sc->values, (sc->value_count + 1) * sizeof(ValueInfo *));
							sc->values[sc->value_count++] = vi;
						}
					} else {
						char *a = gen_value_name(ctx);
						emit_alloca(ctx, "  %s = alloca %s\n", a, elem);
						buffer_append_fmt(ctx, "  store %s %s, %s* %s\n", elem, res, elem, a);
						ValueInfo *vi = calloc(1, sizeof(ValueInfo));
						vi->name = malloc(strlen(tgt->name) + 1);
						strcpy(vi->name, tgt->name);
						vi->llvm_name = malloc(strlen(a) + 1);
						strcpy(vi->llvm_name, a);
						vi->type = 1;
						vi->string_len = -1;
						vi->field_type = field_base_type_name(ot);
						vi->bit_width = (ot && ot->tag == HIR_TYPE_FLOAT) ? 64
						                : (ot && ot->tag == HIR_TYPE_INT) ? ot->int_width
						                                                  : 64;
						vi->handle_archetype = (ot && ot->tag == HIR_TYPE_HANDLE) ? ot->name : NULL;
						if (ctx->scope_count > 0) {
							ValueScope *sc = &ctx->scopes[ctx->scope_count - 1];
							sc->values = realloc(sc->values, (sc->value_count + 1) * sizeof(ValueInfo *));
							sc->values[sc->value_count++] = vi;
						}
						/* RAII: track a fresh opaque local with a registered destructor (extern-result
						 * out path, e.g. `net_listen(port)(s:)`). */
						if (ot && ot->tag == HIR_TYPE_OPAQUE) {
							const char *dt = drop_dtor_for_type(ctx, ot->name);
							if (dt)
								drop_track(ctx, tgt->name, a, dt);
						}
					}
				} else {
					ValueInfo *e = find_value(ctx, tgt->name);
					if (e && e->type == 1)
						buffer_append_fmt(ctx, "  store %s %s, %s* %s\n", elem, res, elem, e->llvm_name);
				}
			}
			break;
		}

		/* Proc-call statement `foo(in)(out)`: a non-extern proc writes each out-only result through
		 * a trailing out-pointer; in-out results are written in place via the in-arg buffer (so they
		 * are not appended). Allocate/bind each out-only target, stash its pointer, emit the call. */
		if (callee_proc && !callee_proc->is_extern) {
			ctx->pending_out_ptr_count = 0;
			/* Out-only unbounded `T[]` out-params return a `{T*,i64}` slice the callee writes through a
			 * `{T*,i64}*` slot. The slot isn't filled until the call below, so defer binding the target
			 * as a type-6 slice (load ptr+len from the slot) until AFTER the call is emitted. */
			char *defer_slot[16];
			char *defer_name[16];
			const char *defer_elem[16];
			const char *defer_etn[16];
			int defer_count = 0;
			for (int i = 0; i < target_count && i < callee_proc->out_param_count; i++) {
				if (proc_out_param_is_inout(callee_proc, i))
					continue; /* in-out: carried by the in-arg buffer */
				HirBindingTarget *tgt = &targets[i];
				HirType *ot = callee_proc->out_params[i]->type;
				const char *elem = return_member_llvm(ot);
				char *ptr;
				if (tgt->is_new && ot && ot->tag == HIR_TYPE_SHAPED_ARRAY) {
					/* sized array out-param: allocate `[N x T]` here, pass the element pointer, and
					 * bind the target as a type-6 element pointer so `buf[i]` reads it after the call.
					 * No in-out shadow needed — the buffer is a pure output (caller-allocated). */
					const char *etn = field_base_type_name(ot);
					const char *lt = llvm_type_from_arche(etn);
					int n = ot->rank;
					elem = lt; /* the call passes `T* <elem0>` */
					char *arr = gen_value_name(ctx);
					emit_alloca(ctx, "  %s = alloca [%d x %s]\n", arr, n, lt);
					ptr = gen_value_name(ctx);
					buffer_append_fmt(ctx, "  %s = getelementptr [%d x %s], [%d x %s]* %s, i64 0, i64 0\n", ptr, n, lt,
					                  n, lt, arr);
					ValueInfo *vi = calloc(1, sizeof(ValueInfo));
					vi->name = malloc(strlen(tgt->name) + 1);
					strcpy(vi->name, tgt->name);
					vi->llvm_name = malloc(strlen(ptr) + 1);
					strcpy(vi->llvm_name, ptr);
					vi->type = 6;
					vi->string_len = n;
					vi->field_type = etn;
					vi->bit_width = strcmp(lt, "double") == 0 ? 64 : (strcmp(lt, "i8") == 0 ? 8 : 32);
					if (ctx->scope_count > 0) {
						ValueScope *sc = &ctx->scopes[ctx->scope_count - 1];
						sc->values = realloc(sc->values, (sc->value_count + 1) * sizeof(ValueInfo *));
						sc->values[sc->value_count++] = vi;
					}
				} else if (tgt->is_new) {
					ptr = gen_value_name(ctx);
					emit_alloca(ctx, "  %s = alloca %s\n", ptr, elem);
					/* Zero-init the fresh slot so the proc may safely READ an out-param before
					 * writing it — out-params are read-write places, not write-only. */
					if (elem[0] == '{')
						/* aggregate (e.g. a `{i8*, i64}` slice fat pointer): null/0 are ill-typed. */
						buffer_append_fmt(ctx, "  store %s zeroinitializer, %s* %s\n", elem, elem, ptr);
					else if (strcmp(elem, "double") == 0)
						buffer_append_fmt(ctx, "  store double 0.0, double* %s\n", ptr);
					else if (strchr(elem, '*'))
						buffer_append_fmt(ctx, "  store %s null, %s* %s\n", elem, elem, ptr);
					else
						buffer_append_fmt(ctx, "  store %s 0, %s* %s\n", elem, elem, ptr);
					if (ot && ot->tag == HIR_TYPE_ARRAY && elem[0] == '{') {
						/* unbounded `T[]` out-only result: bind as a type-6 slice AFTER the call (the slot
						 * holds the {ptr,len} the callee stored). Stash the slot for the post-call pass. */
						defer_slot[defer_count] = strdup(ptr);
						defer_name[defer_count] = tgt->name;
						defer_elem[defer_count] = llvm_type_from_arche(field_base_type_name(ot));
						defer_etn[defer_count] = field_base_type_name(ot);
						defer_count++;
						int k = ctx->pending_out_ptr_count++;
						ctx->pending_out_ptr_vals[k] = strdup(ptr);
						ctx->pending_out_ptr_types[k] = elem;
						continue;
					}
					ValueInfo *vi = calloc(1, sizeof(ValueInfo));
					vi->name = malloc(strlen(tgt->name) + 1);
					strcpy(vi->name, tgt->name);
					vi->llvm_name = malloc(strlen(ptr) + 1);
					strcpy(vi->llvm_name, ptr);
					vi->type = 1; /* pointer-backed scalar/handle: loads/stores through %ptr */
					vi->string_len = -1;
					vi->field_type = field_base_type_name(ot);
					vi->bit_width = (ot && ot->tag == HIR_TYPE_FLOAT) ? 64
					                : (ot && ot->tag == HIR_TYPE_INT) ? ot->int_width
					                                                  : 64;
					vi->handle_archetype = (ot && ot->tag == HIR_TYPE_HANDLE) ? ot->name : NULL;
					if (ctx->scope_count > 0) {
						ValueScope *sc = &ctx->scopes[ctx->scope_count - 1];
						sc->values = realloc(sc->values, (sc->value_count + 1) * sizeof(ValueInfo *));
						sc->values[sc->value_count++] = vi;
					}
					/* RAII: a fresh opaque local of a type with a registered `@drop` destructor is
					 * tracked for auto-drop at scope exit (unless later consumed). */
					if (ot && ot->tag == HIR_TYPE_OPAQUE) {
						const char *dt = drop_dtor_for_type(ctx, ot->name);
						if (dt)
							drop_track(ctx, tgt->name, ptr, dt);
					}
				} else {
					ValueInfo *e = find_value(ctx, tgt->name);
					ptr = e ? e->llvm_name : (char *)"null";
				}
				int k = ctx->pending_out_ptr_count++;
				ctx->pending_out_ptr_vals[k] = malloc(strlen(ptr) + 1);
				strcpy(ctx->pending_out_ptr_vals[k], ptr);
				ctx->pending_out_ptr_types[k] = elem;
			}
			char tmp[256];
			codegen_expression(ctx, rhs, tmp); /* emits `call void @proc(in…, out-ptrs…)` */
			for (int ui = 0; ui < saved_uscore_count; ui++)
				saved_uscore_args[ui]->data.name.name = saved_uscore_names[ui]; /* restore `_` */
			for (int ui = 0; ui < saved_copy_count; ui++)
				*saved_copy_slots[ui] = saved_copy_vals[ui]; /* restore elided `copy` node */
			/* Post-call: bind each deferred out-only `T[]` target as a type-6 slice, loading the
			 * {ptr,len} the callee stored into its slot. */
			for (int d = 0; d < defer_count; d++) {
				char agg[24];
				snprintf(agg, sizeof(agg), "{ %s*, i64 }", defer_elem[d]);
				char *pgep = gen_value_name(ctx);
				buffer_append_fmt(ctx, "  %s = getelementptr %s, %s* %s, i32 0, i32 0\n", pgep, agg, agg,
				                  defer_slot[d]);
				char *p = gen_value_name(ctx);
				buffer_append_fmt(ctx, "  %s = load %s*, %s** %s\n", p, defer_elem[d], defer_elem[d], pgep);
				char *lgep = gen_value_name(ctx);
				buffer_append_fmt(ctx, "  %s = getelementptr %s, %s* %s, i32 0, i32 1\n", lgep, agg, agg,
				                  defer_slot[d]);
				char *l = gen_value_name(ctx);
				buffer_append_fmt(ctx, "  %s = load i64, i64* %s\n", l, lgep);
				ValueInfo *vi = calloc(1, sizeof(ValueInfo));
				vi->name = strdup(defer_name[d]);
				vi->llvm_name = strdup(p);
				vi->type = 6;
				vi->string_len = -1;
				vi->field_type = defer_etn[d];
				vi->bit_width = strcmp(defer_elem[d], "double") == 0 ? 64 : (strcmp(defer_elem[d], "i8") == 0 ? 8 : 32);
				vi->is_slice = 1;
				vi->len_ssa = strdup(l);
				if (ctx->scope_count > 0) {
					ValueScope *sc = &ctx->scopes[ctx->scope_count - 1];
					sc->values = realloc(sc->values, (sc->value_count + 1) * sizeof(ValueInfo *));
					sc->values[sc->value_count++] = vi;
				}
				free(defer_slot[d]);
			}
			for (int i = 0; i < ctx->pending_out_ptr_count; i++)
				free(ctx->pending_out_ptr_vals[i]);
			ctx->pending_out_ptr_count = 0;
			break;
		}

		/* Return-type list of the callee — a func OR a returning proc; the multi-bind extracts the
		 * members of its aggregate return. */
		HirType **ret_types = callee_func ? callee_func->return_types : NULL;
		int ret_count = callee_func ? callee_func->return_type_count : 0;
		if (ret_types && ret_count > 1) {
			char struct_buf[256];
			codegen_expression(ctx, rhs, struct_buf); /* %res = call {…} @f(…) */
			char ret_type[512];
			llvm_return_list_type(ret_types, ret_count, ret_type, sizeof(ret_type));
			int n = target_count < ret_count ? target_count : ret_count;
			for (int i = 0; i < n; i++) {
				HirType *mt = ret_types[i];
				const char *llvm = return_member_llvm(mt);
				char *val = gen_value_name(ctx);
				buffer_append_fmt(ctx, "  %s = extractvalue %s %s, %d\n", val, ret_type, struct_buf, i);
				HirBindingTarget *atgt = &targets[i];

				/* An array member is a raw i8* byte view. Bind it as type-6 (i8* char pointer,
				 * the value IS the pointer — no alloca) so the caller can index it: `b[i]` after
				 * `b, n := f(move b)`. Mirrors a func returning char[]. */
				if (mt && (mt->tag == HIR_TYPE_ARRAY || mt->tag == HIR_TYPE_SHAPED_ARRAY)) {
					ValueInfo *vi = calloc(1, sizeof(ValueInfo));
					vi->name = malloc(strlen(atgt->name) + 1);
					strcpy(vi->name, atgt->name);
					vi->llvm_name = malloc(strlen(val) + 1);
					strcpy(vi->llvm_name, val);
					vi->type = 6; /* i8* char pointer */
					vi->arch_name = NULL;
					vi->string_len = -1;
					vi->field_type = "char";
					vi->handle_archetype = NULL;
					vi->bit_width = 8;
					if (ctx->scope_count > 0) {
						ValueScope *scope = &ctx->scopes[ctx->scope_count - 1];
						scope->values = realloc(scope->values, (scope->value_count + 1) * sizeof(ValueInfo *));
						scope->values[scope->value_count++] = vi;
					}
					continue;
				}

				const char *ft = "int";
				int bw = 32;
				if (mt && mt->tag == HIR_TYPE_FLOAT) {
					ft = "float";
					bw = 64;
				} else if (mt && mt->tag == HIR_TYPE_INT) {
					ft = "int";
					bw = mt->int_width;
				} else if (mt && mt->tag == HIR_TYPE_HANDLE) {
					ft = "handle";
					bw = 64;
				} else if (mt && mt->tag == HIR_TYPE_OPAQUE) {
					ft = "opaque";
					bw = 64;
				}
				HirBindingTarget *tgt = &targets[i];
				if (tgt->is_new) {
					char *a = gen_value_name(ctx);
					emit_alloca(ctx, "  %s = alloca %s\n", a, llvm);
					buffer_append_fmt(ctx, "  store %s %s, %s* %s\n", llvm, val, llvm, a);
					ValueInfo *vi = calloc(1, sizeof(ValueInfo));
					vi->name = malloc(strlen(tgt->name) + 1);
					strcpy(vi->name, tgt->name);
					vi->llvm_name = malloc(strlen(a) + 1);
					strcpy(vi->llvm_name, a);
					vi->type = 1;
					vi->arch_name = NULL;
					vi->string_len = -1;
					vi->field_type = ft;
					vi->handle_archetype = (mt && mt->tag == HIR_TYPE_HANDLE) ? mt->name : NULL;
					vi->bit_width = bw;
					if (ctx->scope_count > 0) {
						ValueScope *scope = &ctx->scopes[ctx->scope_count - 1];
						scope->values = realloc(scope->values, (scope->value_count + 1) * sizeof(ValueInfo *));
						scope->values[scope->value_count++] = vi;
					}
				} else {
					ValueInfo *e = find_value(ctx, tgt->name);
					if (e)
						buffer_append_fmt(ctx, "  store %s %s, %s* %s\n", llvm, val, llvm, e->llvm_name);
				}
			}
		}
		break;
	}

	case HIR_STMT_ASSIGN: {
		/* Write-back to an out-ONLY unbounded `char[]`/`T[]` out-param (`out = buf[0:r]`): store the
		 * RHS slice's {ptr,len} through the caller's `{T*,i64}*` slot so the caller recovers the view.
		 * Without this the slice is computed and dropped (the proc returns void). */
		if (stmt->data.assign_stmt.target->kind == HIR_EXPR_NAME) {
			ValueInfo *ot = find_value(ctx, stmt->data.assign_stmt.target->data.name.name);
			if (ot && ot->out_aggr_ptr) {
				HirExpr *rv = stmt->data.assign_stmt.value;
				while (rv && rv->kind == HIR_EXPR_UNARY &&
				       (rv->data.unary.op == UNARY_MOVE || rv->data.unary.op == UNARY_COPY))
					rv = rv->data.unary.operand;
				const char *elem = llvm_type_from_arche(ot->field_type ? ot->field_type : "char");
				char agg[24];
				snprintf(agg, sizeof(agg), "{ %s*, i64 }", elem);
				char ptr[256], len[64];
				if (rv && rv->kind == HIR_EXPR_SLICE) {
					char cap[64];
					const char *se;
					int sw;
					codegen_slice(ctx, rv, ptr, len, cap, &se, &sw);
				} else if (rv && rv->kind == HIR_EXPR_NAME) {
					ValueInfo *sv = find_value(ctx, rv->data.name.name);
					strcpy(ptr, sv->llvm_name);
					if (sv->is_slice && sv->len_ssa)
						snprintf(len, sizeof(len), "%s", sv->len_ssa);
					else
						snprintf(len, sizeof(len), "%d", sv->string_len);
				} else if (rv && rv->kind == HIR_EXPR_STRING) {
					codegen_expression(ctx, rv, ptr);
					snprintf(len, sizeof(len), "%d", rv->data.string.length);
				} else {
					codegen_expression(ctx, stmt->data.assign_stmt.value, ptr);
					snprintf(len, sizeof(len), "0");
				}
				char *a1 = gen_value_name(ctx);
				buffer_append_fmt(ctx, "  %s = insertvalue %s undef, %s* %s, 0\n", a1, agg, elem, ptr);
				char *a2 = gen_value_name(ctx);
				buffer_append_fmt(ctx, "  %s = insertvalue %s %s, i64 %s, 1\n", a2, agg, a1, len);
				buffer_append_fmt(ctx, "  store %s %s, %s* %s\n", agg, a2, agg, ot->out_aggr_ptr);
				/* Keep the in-scope view current so later reads in the body see the new ptr/len. */
				free(ot->llvm_name);
				ot->llvm_name = strdup(ptr);
				free(ot->len_ssa);
				ot->len_ssa = strdup(len);
				ot->is_slice = 1;
				break;
			}
		}
		/* `a = b` / `a = move b` / `a = copy b` where both sides are array/slice VALUES: the target
		 * binding takes over the source's storage (move — value_buf is the source's own pointer, the
		 * source is consumed in semantic) or the freshly-cloned buffer (`copy`). Rebind the target's
		 * ValueInfo to the RHS pointer + array metadata: a compile-time ownership transfer, no store. */
		if (stmt->data.assign_stmt.target->kind == HIR_EXPR_NAME) {
			HirExpr *rv = stmt->data.assign_stmt.value;
			while (rv && rv->kind == HIR_EXPR_UNARY &&
			       (rv->data.unary.op == UNARY_MOVE || rv->data.unary.op == UNARY_COPY))
				rv = rv->data.unary.operand;
			ValueInfo *tgt = find_value(ctx, stmt->data.assign_stmt.target->data.name.name);
			ValueInfo *src = (rv && rv->kind == HIR_EXPR_NAME) ? find_value(ctx, rv->data.name.name) : NULL;
			if (tgt && src && (src->type == 6 || src->type == 7) && (tgt->type == 6 || tgt->type == 7)) {
				char vbuf[256];
				codegen_expression(ctx, stmt->data.assign_stmt.value, vbuf);
				free(tgt->llvm_name);
				tgt->llvm_name = malloc(strlen(vbuf) + 1);
				strcpy(tgt->llvm_name, vbuf);
				tgt->type = src->type;
				tgt->string_len = src->string_len;
				tgt->field_type = src->field_type;
				tgt->bit_width = src->bit_width;
				tgt->is_slice = src->is_slice;
				free(tgt->len_ssa);
				tgt->len_ssa = NULL;
				if (src->len_ssa) {
					tgt->len_ssa = malloc(strlen(src->len_ssa) + 1);
					strcpy(tgt->len_ssa, src->len_ssa);
				}
				break;
			}
		}

		/* Each-field column write: f[i] = expr where f is the current
		 * each_field binding. */
		if (ctx->current_each_field_binding && ctx->current_each_field_target && ctx->current_archetype_param &&
		    ctx->current_arch_param_llvm && stmt->data.assign_stmt.target->kind == HIR_EXPR_INDEX &&
		    stmt->data.assign_stmt.target->data.index.base->kind == HIR_EXPR_NAME &&
		    strcmp(stmt->data.assign_stmt.target->data.index.base->data.name.name, ctx->current_each_field_binding) ==
		        0 &&
		    stmt->data.assign_stmt.target->data.index.index_count == 1) {
			char idx_local[256], val_buf[256];
			codegen_expression(ctx, stmt->data.assign_stmt.target->data.index.indices[0], idx_local);
			codegen_expression(ctx, stmt->data.assign_stmt.value, val_buf);
			int cap = get_arch_static_capacity(ctx, ctx->current_archetype_param->name);
			if (cap <= 0)
				cap = 1;
			const char *base_elem = field_base_type_name(ctx->current_each_field_target->type);
			const char *llvm_elem = llvm_type_from_arche(base_elem);
			char *col_gep = gen_value_name(ctx);
			buffer_append_fmt(ctx, "  %s = getelementptr %%struct.%s, %%struct.%s* %s, i32 0, i32 %d\n", col_gep,
			                  ctx->current_archetype_param->name, ctx->current_archetype_param->name,
			                  ctx->current_arch_param_llvm, ctx->current_each_field_index);
			char idx64[256];
			emit_index_i64(ctx, idx_local, stmt->data.assign_stmt.target->data.index.indices[0], idx64);
			char *elem_gep = gen_value_name(ctx);
			buffer_append_fmt(ctx, "  %s = getelementptr [%d x %s], [%d x %s]* %s, i64 0, i64 %s\n", elem_gep, cap,
			                  llvm_elem, cap, llvm_elem, col_gep, idx64);
			buffer_append_fmt(ctx, "  store %s %s, %s* %s\n", llvm_elem, val_buf, llvm_elem, elem_gep);
			break;
		}

		/* Check if this is a whole-column operation (Path A or B) */
		int is_whole_column = 0;

		/* Check Path A: target is HIR_EXPR_FIELD of FIELD_COLUMN (p.pos = p.pos + p.vel) */
		if (stmt->data.assign_stmt.target->kind == HIR_EXPR_FIELD &&
		    stmt->data.assign_stmt.target->data.field.base->kind == HIR_EXPR_NAME) {
			const char *inst_name = stmt->data.assign_stmt.target->data.field.base->data.name.name;
			ValueInfo *inst = find_value(ctx, inst_name);
			const char *arch_name_direct = NULL;
			if (!inst && find_archetype_decl(ctx, inst_name)) {
				arch_name_direct = inst_name;
			}
			if ((inst && inst->type == 3 && inst->arch_name) || arch_name_direct) {
				const char *arch_check_name = inst ? inst->arch_name : arch_name_direct;
				HirArchetypeDecl *arch = find_archetype_decl(ctx, arch_check_name);
				if (arch) {
					const char *fname = stmt->data.assign_stmt.target->data.field.field_name;
					/* Check if fname is a direct field or tuple base */
					int found = 0;
					for (int i = 0; i < arch->field_count; i++) {
						if (strcmp(arch->fields[i]->name, fname) == 0 && arch->fields[i]->kind == FIELD_COLUMN) {
							is_whole_column = 1;
							found = 1;
							break;
						}
					}
					/* Check if fname is a tuple base (e.g., "pos" when fields are "pos_x", "pos_y") */
					if (!found) {
						size_t prefix_len = strlen(fname);
						for (int i = 0; i < arch->field_count; i++) {
							const char *aname = arch->fields[i]->name;
							if (strncmp(aname, fname, prefix_len) == 0 && aname[prefix_len] == '_' &&
							    arch->fields[i]->kind == FIELD_COLUMN) {
								is_whole_column = 1;
								break;
							}
						}
					}
				}
			}
		}

		/* Check Path B: target is HIR_EXPR_NAME type-4 (sys parameter) */
		if (stmt->data.assign_stmt.target->kind == HIR_EXPR_NAME) {
			const char *var_name = stmt->data.assign_stmt.target->data.name.name;
			ValueInfo *val = find_value(ctx, var_name);
			if (val && val->type == 4) {
				is_whole_column = 1;
			}
		}

		/* Only evaluate RHS for scalar operations; whole-column paths evaluate it internally */
		char value_buf[256];
		if (!is_whole_column) {
			codegen_expression(ctx, stmt->data.assign_stmt.value, value_buf);
		}

		/* assignment - find the target variable */
		if (stmt->data.assign_stmt.target->kind == HIR_EXPR_NAME) {
			const char *var_name = stmt->data.assign_stmt.target->data.name.name;
			ValueInfo *val = find_value(ctx, var_name);
			HirStaticDecl *gsc = (!val) ? codegen_find_scalar(ctx, var_name) : NULL;
			if (gsc) {
				/* Scalar global store: the global is mutable; write through @name. */
				const char *llvm_t = llvm_type_from_arche(field_base_type_name(gsc->scalar.type));
				int is_float = strcmp(llvm_t, "float") == 0 || strcmp(llvm_t, "double") == 0;
				if (stmt->data.assign_stmt.op == OP_NONE) {
					buffer_append_fmt(ctx, "  store %s %s, %s* @%s\n", llvm_t, value_buf, llvm_t, var_name);
				} else {
					char *loaded = gen_value_name(ctx);
					buffer_append_fmt(ctx, "  %s = load %s, %s* @%s\n", loaded, llvm_t, llvm_t, var_name);
					const char *op = is_float ? "fadd" : "add";
					switch (stmt->data.assign_stmt.op) {
					case OP_SUB:
						op = is_float ? "fsub" : "sub";
						break;
					case OP_MUL:
						op = is_float ? "fmul" : "mul";
						break;
					case OP_DIV:
						op = is_float ? "fdiv" : "sdiv";
						break;
					default:
						break;
					}
					char *result = gen_value_name(ctx);
					buffer_append_fmt(ctx, "  %s = %s %s %s, %s\n", result, op, llvm_t, loaded, value_buf);
					buffer_append_fmt(ctx, "  store %s %s, %s* @%s\n", llvm_t, result, llvm_t, var_name);
				}
			}
			if (val && val->type == 1) { /* type 1 = scalar pointer */
				int is_float = val->field_type &&
				               (strcmp(val->field_type, "float") == 0 || strcmp(val->field_type, "double") == 0);
				/* opaque/handle cells are pointer-width i64 carried verbatim — never an integer
				 * width conversion (that would truncate a real i64 handle to i32). */
				int is_ptr_cell = val->field_type &&
				                  (strcmp(val->field_type, "opaque") == 0 || strcmp(val->field_type, "handle") == 0);
				const char *llvm_t = is_float ? "double" : llvm_type_from_arche(val->field_type);
				int unsigned_int = val->field_type && val->field_type[0] == 'u';

				if (stmt->data.assign_stmt.op == OP_NONE) {
					/* Convert RHS to the target int width (sext/zext/trunc). */
					if (!is_float && !is_ptr_cell && stmt->data.assign_stmt.value) {
						int tw = 32, sg;
						if (val->field_type)
							hir_parse_int_width(val->field_type, &tw, &sg); /* leaves tw=32 for "int" */
						char converted[256];
						emit_int_convert(ctx, value_buf, &stmt->data.assign_stmt.value->resolved, tw, converted);
						strcpy(value_buf, converted);
					}
					buffer_append_fmt(ctx, "  store %s %s, %s* %s\n", llvm_t, value_buf, llvm_t, val->llvm_name);
				} else {
					/* Compound assignment: load, compute, store */
					char *loaded = gen_value_name(ctx);
					buffer_append_fmt(ctx, "  %s = load %s, %s* %s\n", loaded, llvm_t, llvm_t, val->llvm_name);

					const char *op;
					switch (stmt->data.assign_stmt.op) {
					case OP_ADD:
						op = is_float ? "fadd" : "add";
						break;
					case OP_SUB:
						op = is_float ? "fsub" : "sub";
						break;
					case OP_MUL:
						op = is_float ? "fmul" : "mul";
						break;
					case OP_DIV:
						op = is_float ? "fdiv" : (unsigned_int ? "udiv" : "sdiv");
						break;
					default:
						op = is_float ? "fadd" : "add";
						break;
					}

					char *result = gen_value_name(ctx);
					buffer_append_fmt(ctx, "  %s = %s %s %s, %s\n", result, op, llvm_t, loaded, value_buf);
					buffer_append_fmt(ctx, "  store %s %s, %s* %s\n", llvm_t, result, llvm_t, val->llvm_name);
				}
			}

			/* Path B: target is HIR_EXPR_NAME type-4 (sys parameter) */
			if (val && val->type == 4) {
				/* Column parameter: emit whole-column loop */
				const char *arche_type = val->field_type ? val->field_type : "float";
				/* Skip handle columns — cannot use in sys operations */
				if (strcmp(arche_type, "handle") == 0) {
					break;
				}
				const char *scalar_type = llvm_type_from_arche(arche_type);

				/* Get count from archetype struct */
				char *count_gep = gen_value_name(ctx);
				HirArchetypeDecl *arch = find_archetype_decl(ctx, val->arch_name);
				if (arch) {
					int count_idx = arch->field_count;
					/* Construct the archetype parameter name (handles both old %archetype and new %arch_<name>) */
					char arch_param[256];
					snprintf(arch_param, sizeof(arch_param), "%%arch_%s", val->arch_name);

					buffer_append_fmt(ctx, "  %s = getelementptr %%struct.%s, %%struct.%s* %s, i32 0, i32 %d\n",
					                  count_gep, val->arch_name, val->arch_name, arch_param, count_idx);
					char *count = gen_value_name(ctx);
					buffer_append_fmt(ctx, "  %s = load i64, i64* %s\n", count, count_gep);

					emit_whole_column_loop(ctx, val->llvm_name, count, scalar_type, arche_type,
					                       stmt->data.assign_stmt.value, stmt->data.assign_stmt.op, arch_param);
				}
			}
		} else if (stmt->data.assign_stmt.target->kind == HIR_EXPR_FIELD) {
			/* Path A: target is HIR_EXPR_FIELD of FIELD_COLUMN (p.pos = p.pos + p.vel) */
			if (stmt->data.assign_stmt.target->data.field.base->kind == HIR_EXPR_NAME) {
				const char *inst_name = stmt->data.assign_stmt.target->data.field.base->data.name.name;
				ValueInfo *inst = find_value(ctx, inst_name);
				const char *arch_name_direct = NULL;
				if (!inst && find_archetype_decl(ctx, inst_name)) {
					arch_name_direct = inst_name;
				}

				if ((inst && inst->type == 3 && inst->arch_name) || arch_name_direct) {
					const char *arch_check_name = inst ? inst->arch_name : arch_name_direct;
					char loaded_global[256] = {0};
					HirArchetypeDecl *arch = find_archetype_decl(ctx, arch_check_name);
					const char *fname = stmt->data.assign_stmt.target->data.field.field_name;

					if (arch) {
						/* Find field in archetype */
						int field_idx = -1;
						HirField *fdecl = NULL;
						for (int i = 0; i < arch->field_count; i++) {
							if (strcmp(arch->fields[i]->name, fname) == 0) {
								field_idx = i;
								fdecl = arch->fields[i];
								break;
							}
						}

						if (field_idx >= 0 && fdecl && fdecl->kind == FIELD_COLUMN) {
							/* Load column pointer and count from struct */
							const char *llvm_type = llvm_type_from_arche(field_base_type_name(fdecl->type));
							char *field_gep = gen_value_name(ctx);
							const char *struct_ptr_val;
							if (arch_name_direct) {
								/* Load from global — static use @X directly, dynamic load from @archetype_X */
								if (get_arch_static_capacity(ctx, arch_check_name) > 0) {
									snprintf(loaded_global, sizeof(loaded_global), "@%s", arch_check_name);
								} else {
									char *loaded = gen_value_name(ctx);
									buffer_append_fmt(ctx, "  %s = load %%struct.%s*, %%struct.%s** @archetype_%s\n",
									                  loaded, arch_check_name, arch_check_name, arch_check_name);
									strcpy(loaded_global, loaded);
								}
								struct_ptr_val = loaded_global;
							} else {
								struct_ptr_val = inst->llvm_name;
							}
							int is_static = get_arch_static_capacity(ctx, arch_check_name) > 0;
							char *col_ptr = gen_value_name(ctx);
							if (is_static) {
								buffer_append_fmt(
								    ctx, "  %s = getelementptr %%struct.%s, %%struct.%s* %s, i32 0, i32 %d, i64 0\n",
								    col_ptr, arch_check_name, arch_check_name, struct_ptr_val, field_idx);
							} else {
								buffer_append_fmt(
								    ctx, "  %s = getelementptr %%struct.%s, %%struct.%s* %s, i32 0, i32 %d\n",
								    field_gep, arch_check_name, arch_check_name, struct_ptr_val, field_idx);
								buffer_append_fmt(ctx, "  %s = load %s*, %s** %s\n", col_ptr, llvm_type, llvm_type,
								                  field_gep);
							}

							/* Load count from archetype */
							char *count_gep = gen_value_name(ctx);
							int count_idx = arch->field_count;
							buffer_append_fmt(ctx, "  %s = getelementptr %%struct.%s, %%struct.%s* %s, i32 0, i32 %d\n",
							                  count_gep, arch_check_name, arch_check_name, struct_ptr_val, count_idx);
							char *count = gen_value_name(ctx);
							buffer_append_fmt(ctx, "  %s = load i64, i64* %s\n", count, count_gep);

							/* Emit whole-column loop */
							const char *scalar_type = llvm_type_from_arche(field_base_type_name(fdecl->type));
							emit_whole_column_loop(ctx, col_ptr, count, scalar_type, field_base_type_name(fdecl->type),
							                       stmt->data.assign_stmt.value, stmt->data.assign_stmt.op,
							                       struct_ptr_val);
						} else {
							/* Tuple field: emit loop for each component */
							HirField **tuple_components = NULL;
							int tuple_count = 0;
							size_t prefix_len = strlen(fname);

							for (int i = 0; i < arch->field_count; i++) {
								const char *aname = arch->fields[i]->name;
								if (strncmp(aname, fname, prefix_len) == 0 && aname[prefix_len] == '_') {
									tuple_components =
									    realloc(tuple_components, (tuple_count + 1) * sizeof(HirField *));
									tuple_components[tuple_count++] = arch->fields[i];
								}
							}

							for (int t = 0; t < tuple_count; t++) {
								HirField *comp = tuple_components[t];
								int comp_idx = -1;
								for (int i = 0; i < arch->field_count; i++) {
									if (strcmp(arch->fields[i]->name, comp->name) == 0) {
										comp_idx = i;
										break;
									}
								}

								if (comp_idx >= 0 && comp->kind == FIELD_COLUMN) {
									const char *llvm_type = llvm_type_from_arche(field_base_type_name(comp->type));
									char *field_gep = gen_value_name(ctx);
									const char *struct_ptr_val;
									char loaded_global_comp[256];
									if (arch_name_direct && !loaded_global[0]) {
										/* Load from global — static use @X directly, dynamic load from @archetype_X */
										if (get_arch_static_capacity(ctx, arch_check_name) > 0) {
											snprintf(loaded_global_comp, sizeof(loaded_global_comp), "@%s",
											         arch_check_name);
										} else {
											char *loaded = gen_value_name(ctx);
											buffer_append_fmt(
											    ctx, "  %s = load %%struct.%s*, %%struct.%s** @archetype_%s\n", loaded,
											    arch_check_name, arch_check_name, arch_check_name);
											strcpy(loaded_global_comp, loaded);
										}
										struct_ptr_val = loaded_global_comp;
									} else if (arch_name_direct) {
										struct_ptr_val = loaded_global;
									} else {
										struct_ptr_val = inst->llvm_name;
									}
									int is_static = get_arch_static_capacity(ctx, arch_check_name) > 0;
									char *col_ptr = gen_value_name(ctx);
									if (is_static) {
										buffer_append_fmt(
										    ctx,
										    "  %s = getelementptr %%struct.%s, %%struct.%s* %s, i32 0, i32 %d, i64 0\n",
										    col_ptr, arch_check_name, arch_check_name, struct_ptr_val, comp_idx);
									} else {
										buffer_append_fmt(
										    ctx, "  %s = getelementptr %%struct.%s, %%struct.%s* %s, i32 0, i32 %d\n",
										    field_gep, arch_check_name, arch_check_name, struct_ptr_val, comp_idx);
										buffer_append_fmt(ctx, "  %s = load %s*, %s** %s\n", col_ptr, llvm_type,
										                  llvm_type, field_gep);
									}

									char *count_gep = gen_value_name(ctx);
									int count_idx = arch->field_count;
									buffer_append_fmt(
									    ctx, "  %s = getelementptr %%struct.%s, %%struct.%s* %s, i32 0, i32 %d\n",
									    count_gep, arch_check_name, arch_check_name, struct_ptr_val, count_idx);
									char *count = gen_value_name(ctx);
									buffer_append_fmt(ctx, "  %s = load i64, i64* %s\n", count, count_gep);

									/* Check if RHS is tuple binary op - if so, expand component names */
									HirExpr *rhs_expr = stmt->data.assign_stmt.value;
									const char *scalar_type = llvm_type_from_arche(field_base_type_name(comp->type));

									if (rhs_expr->kind == HIR_EXPR_BINARY &&
									    rhs_expr->data.binary.left->kind == HIR_EXPR_FIELD &&
									    rhs_expr->data.binary.right->kind == HIR_EXPR_FIELD) {
										const char *suffix = strchr(comp->name, '_') + 1;
										const char *left_base = rhs_expr->data.binary.left->data.field.field_name;
										const char *right_base = rhs_expr->data.binary.right->data.field.field_name;

										/* Create modified RHS with component names */
										char left_comp[256], right_comp[256];
										snprintf(left_comp, sizeof(left_comp), "%s_%s", left_base, suffix);
										snprintf(right_comp, sizeof(right_comp), "%s_%s", right_base, suffix);

										/* Create temporary binary expression with component field names */
										HirExpr temp_rhs = *rhs_expr;
										HirExpr temp_left = *rhs_expr->data.binary.left;
										HirExpr temp_right = *rhs_expr->data.binary.right;
										temp_left.data.field.field_name = left_comp;
										temp_right.data.field.field_name = right_comp;
										temp_rhs.data.binary.left = &temp_left;
										temp_rhs.data.binary.right = &temp_right;
										/* Preserve resolved type for operation */
										temp_rhs.resolved = *comp->type;

										emit_whole_column_loop(ctx, col_ptr, count, scalar_type,
										                       field_base_type_name(comp->type), &temp_rhs,
										                       stmt->data.assign_stmt.op, struct_ptr_val);
									} else if (rhs_expr->kind == HIR_EXPR_FIELD) {
										/* Check if RHS is tuple field reference - match components by position */
										const char *rhs_base = rhs_expr->data.field.field_name;

										/* Find which component position this is (t) and find RHS component at same
										 * position */
										HirField **rhs_tuple_components = NULL;
										int rhs_tuple_count = 0;
										size_t rhs_prefix_len = strlen(rhs_base);

										for (int i = 0; i < arch->field_count; i++) {
											const char *aname = arch->fields[i]->name;
											if (strncmp(aname, rhs_base, rhs_prefix_len) == 0 &&
											    aname[rhs_prefix_len] == '_') {
												rhs_tuple_components = realloc(
												    rhs_tuple_components, (rhs_tuple_count + 1) * sizeof(HirField *));
												rhs_tuple_components[rhs_tuple_count++] = arch->fields[i];
											}
										}

										if (t < rhs_tuple_count) {
											/* Use RHS component at same position */
											char *rhs_comp_name = rhs_tuple_components[t]->name;

											/* Create temporary field expression with component field name */
											HirExpr temp_rhs = *rhs_expr;
											temp_rhs.data.field.field_name = rhs_comp_name;
											/* Preserve resolved type */
											temp_rhs.resolved = *comp->type;

											emit_whole_column_loop(ctx, col_ptr, count, scalar_type,
											                       field_base_type_name(comp->type), &temp_rhs,
											                       stmt->data.assign_stmt.op, struct_ptr_val);
										}
										if (rhs_tuple_components)
											free(rhs_tuple_components);
									} else {
										emit_whole_column_loop(
										    ctx, col_ptr, count, scalar_type, field_base_type_name(comp->type),
										    stmt->data.assign_stmt.value, stmt->data.assign_stmt.op, struct_ptr_val);
									}
								}
							}
							if (tuple_components)
								free(tuple_components);
						}
					}
				}
			}
		} else if (stmt->data.assign_stmt.target->kind == HIR_EXPR_INDEX) {
			/* Field indexing assignment: pos[i] = value or pos[i] += value */
			char base_buf[256], idx_buf[256];
			codegen_expression(ctx, stmt->data.assign_stmt.target->data.index.base, base_buf);
			if (stmt->data.assign_stmt.target->data.index.index_count > 0) {
				codegen_expression(ctx, stmt->data.assign_stmt.target->data.index.indices[0], idx_buf);
			}

			/* A type-6 element-pointer target (any array, incl. char) — the only array rep now. */
			ValueInfo *type6_target = NULL;
			if (stmt->data.assign_stmt.target->data.index.base->kind == HIR_EXPR_NAME) {
				ValueInfo *vi = find_value(ctx, stmt->data.assign_stmt.target->data.index.base->data.name.name);
				if (vi && vi->type == 6)
					type6_target = vi;
			}

			if (type6_target && stmt->data.assign_stmt.target->data.index.index_count > 0) {
				/* Type-6 element-pointer assignment. char[] elements are byte stores (value carried
				 * as i32, truncated); non-char elements (int[], float[], …) store the element type
				 * directly — the pointer is already `T*`. */
				char idx_i64[256];
				emit_index_i64(ctx, idx_buf, stmt->data.assign_stmt.target->data.index.indices[0], idx_i64);
				/* Bounds check a bounded array write against its count N (idx_i64 is i64). */
				if (type6_target->string_len > 0 &&
				    !const_index_in_range(ctx, stmt->data.assign_stmt.target->data.index.indices[0],
				                          type6_target->string_len))
					emit_const_bounds_check(ctx, type6_target->string_len, idx_i64, 1);
				const char *et6 = type6_target->field_type ? type6_target->field_type : "char";
				if (strcmp(et6, "char") == 0) {
					char *target_addr = gen_value_name(ctx);
					buffer_append_fmt(ctx, "  %s = getelementptr i8, i8* %s, i64 %s\n", target_addr, base_buf, idx_i64);
					char *trunc_val = gen_value_name(ctx);
					buffer_append_fmt(ctx, "  %s = trunc i32 %s to i8\n", trunc_val, value_buf);
					buffer_append_fmt(ctx, "  store i8 %s, i8* %s, align 1\n", trunc_val, target_addr);
				} else {
					const char *st = llvm_type_from_arche(et6);
					int align = strcmp(st, "double") == 0 ? 8 : 4;
					char *target_addr = gen_value_name(ctx);
					buffer_append_fmt(ctx, "  %s = getelementptr %s, %s* %s, i64 %s\n", target_addr, st, st, base_buf,
					                  idx_i64);
					buffer_append_fmt(ctx, "  store %s %s, %s* %s, align %d\n", st, value_buf, st, target_addr, align);
				}
				break;
			}

			/* Determine element type (scalar for GEP, may be vectorized for load/store) */
			const char *scalar_type = "i32"; /* default */
			const char *arche_type = NULL;

			/* Try to get element type from resolved type info */
			HirExpr *base_expr = stmt->data.assign_stmt.target->data.index.base;
			if (base_expr->resolved.tag != HIR_TYPE_UNKNOWN) {
				arche_type = hir_resolved_type_name(base_expr);
				scalar_type = llvm_type_from_arche(arche_type);
			} else if (base_expr->kind == HIR_EXPR_FIELD) {
				/* Fallback: lookup field type from archetype */
				const char *field_name = base_expr->data.field.field_name;
				ValueInfo *base_val = NULL;

				if (base_expr->data.field.base->kind == HIR_EXPR_NAME) {
					base_val = find_value(ctx, base_expr->data.field.base->data.name.name);
				}

				if (base_val && base_val->type == 3 && base_val->arch_name) {
					HirArchetypeDecl *arch = find_archetype_decl(ctx, base_val->arch_name);
					if (arch) {
						for (int i = 0; i < arch->field_count; i++) {
							if (strcmp(arch->fields[i]->name, field_name) == 0) {
								arche_type = field_base_type_name(arch->fields[i]->type);
								scalar_type = llvm_type_from_arche(arche_type);
								break;
							}
						}
					}
				}
			}

			/* In vector mode, load/store use vector type; GEP uses scalar pointer */
			const char *load_type = arche_type ? elem_llvm_type(ctx, arche_type) : scalar_type;

			/* Bounds check for archetype column accesses (elided when statically provable). */
			const char *bc_arch_name = NULL, *bc_arch_ptr = NULL;
			int bc_count_idx = -1, bc_idx_is_i64 = 0;
			HirExpr *bc_idx_expr = stmt->data.assign_stmt.target->data.index.indices[0];
			if (stmt->data.assign_stmt.target->data.index.index_count > 0 &&
			    resolve_index_arch(ctx, base_expr, bc_idx_expr, &bc_arch_name, &bc_arch_ptr, &bc_count_idx,
			                       &bc_idx_is_i64) &&
			    !bounds_check_elidable(ctx, bc_arch_name, bc_idx_expr)) {
				emit_bounds_check(ctx, bc_arch_name, bc_arch_ptr, bc_count_idx, idx_buf, bc_idx_is_i64);
			}

			/* Ensure index is i64 for getelementptr, respecting the index's width. */
			const char *final_idx = idx_buf;
			char final_idx_buf[256];
			if (stmt->data.assign_stmt.target->data.index.index_count > 0) {
				emit_index_i64(ctx, idx_buf, stmt->data.assign_stmt.target->data.index.indices[0], final_idx_buf);
				final_idx = final_idx_buf;
			}

			/* Compute target address (always uses scalar pointer) */
			char *target_addr = gen_value_name(ctx);
			buffer_append_fmt(ctx, "  %s = getelementptr %s, %s* %s, i64 %s\n", target_addr, scalar_type, scalar_type,
			                  base_buf, final_idx);

			/* Store or compound operation */
			if (stmt->data.assign_stmt.op == OP_NONE) {
				int align = ctx->vector_lanes > 0 ? 8 : 4;
				buffer_append_fmt(ctx, "  store %s %s, %s* %s, align %d\n", load_type, value_buf, scalar_type,
				                  target_addr, align);
			} else {
				/* Compound assignment: load, compute, store */
				char *loaded = gen_value_name(ctx);
				int align = ctx->vector_lanes > 0 ? 8 : 4;
				buffer_append_fmt(ctx, "  %s = load %s, %s* %s, align %d\n", loaded, load_type, scalar_type,
				                  target_addr, align);

				/* Detect if float type for choosing fadd vs add */
				int is_float = (scalar_type[0] == 'd' || (scalar_type[0] == '<' && strstr(scalar_type, "double")));
				const char *op;
				switch (stmt->data.assign_stmt.op) {
				case OP_ADD:
					op = is_float ? "fadd" : "add";
					break;
				case OP_SUB:
					op = is_float ? "fsub" : "sub";
					break;
				case OP_MUL:
					op = is_float ? "fmul" : "mul";
					break;
				case OP_DIV:
					op = is_float ? "fdiv" : "sdiv";
					break;
				default:
					op = "add";
					break;
				}

				char *result = gen_value_name(ctx);
				buffer_append_fmt(ctx, "  %s = %s %s %s, %s\n", result, op, load_type, loaded, value_buf);
				buffer_append_fmt(ctx, "  store %s %s, %s* %s, align %d\n", load_type, result, scalar_type, target_addr,
				                  align);
			}
		}
		break;
	}

	case HIR_STMT_FOR: {
		if (stmt->data.for_stmt.init || stmt->data.for_stmt.incr) {
			/* C-style for loop: for (init; cond; incr) */
			char *loop_label = gen_value_name(ctx);
			char *body_label = gen_value_name(ctx);
			char *exit_label = gen_value_name(ctx);
			char *cont_label = gen_value_name(ctx); /* `continue` latch: runs incr, then re-tests cond */

			push_value_scope(ctx);

			/* If the loop is `for (let v = K; v < N; v += 1)` with N a compile-time int,
			 * track (v, N) so bounds checks on column accesses indexed by v can be elided
			 * when N <= archetype capacity. Pushed before the body, popped after exit. */
			char *bound_var = NULL;
			int bound_val = 0;
			int pushed_bound = 0;
			if (try_extract_loop_bound(stmt, &bound_var, &bound_val)) {
				push_loop_bound(ctx, bound_var, bound_val);
				pushed_bound = 1;
			}

			if (stmt->data.for_stmt.init) {
				codegen_statement(ctx, stmt->data.for_stmt.init);
			}

			if (ctx->loop_exit_count >= ctx->loop_exit_capacity) {
				ctx->loop_exit_capacity = (ctx->loop_exit_capacity == 0) ? 8 : ctx->loop_exit_capacity * 2;
				ctx->loop_exit_labels = realloc(ctx->loop_exit_labels, ctx->loop_exit_capacity * sizeof(char *));
			}
			ctx->loop_exit_labels[ctx->loop_exit_count] = exit_label;
			ctx->loop_exit_count++;
			if (ctx->loop_cont_count >= ctx->loop_cont_capacity) {
				ctx->loop_cont_capacity = (ctx->loop_cont_capacity == 0) ? 8 : ctx->loop_cont_capacity * 2;
				ctx->loop_cont_labels = realloc(ctx->loop_cont_labels, ctx->loop_cont_capacity * sizeof(char *));
			}
			ctx->loop_cont_labels[ctx->loop_cont_count] = cont_label;
			ctx->loop_cont_count++;

			buffer_append_fmt(ctx, "  br label %s\n", loop_label);
			buffer_append_fmt(ctx, "%s:\n", loop_label + 1);

			if (stmt->data.for_stmt.cond) {
				char cond_buf[256];
				codegen_expression(ctx, stmt->data.for_stmt.cond, cond_buf);
				char *cond_i1 = gen_value_name(ctx);
				/* Truthiness: nonzero is true; use the cond value's real width. */
				buffer_append_fmt(ctx, "  %s = icmp ne %s %s, 0\n", cond_i1, cond_int_type(stmt->data.for_stmt.cond),
				                  cond_buf);
				buffer_append_fmt(ctx, "  br i1 %s, label %s, label %s\n", cond_i1, body_label, exit_label);
			} else {
				buffer_append_fmt(ctx, "  br label %s\n", body_label);
			}

			buffer_append_fmt(ctx, "%s:\n", body_label + 1);

			for (int i = 0; i < stmt->data.for_stmt.body_count; i++) {
				codegen_statement(ctx, stmt->data.for_stmt.body[i]);
			}

			/* Latch: `continue` jumps here, so the increment still runs before the next cond test. */
			buffer_append_fmt(ctx, "  br label %s\n", cont_label);
			buffer_append_fmt(ctx, "%s:\n", cont_label + 1);
			ctx->block_terminated = 0;

			if (stmt->data.for_stmt.incr) {
				codegen_statement(ctx, stmt->data.for_stmt.incr);
			}

			buffer_append_fmt(ctx, "  br label %s\n", loop_label);
			buffer_append_fmt(ctx, "%s:\n", exit_label + 1);

			ctx->loop_exit_count--;
			ctx->loop_cont_count--;
			if (pushed_bound)
				pop_loop_bound(ctx);
			pop_value_scope(ctx);
		} else if (!stmt->data.for_stmt.var_name) {
			/* Condition-based or infinite for loop */
			char *loop_label = gen_value_name(ctx);
			char *body_label = gen_value_name(ctx);
			char *exit_label = gen_value_name(ctx);

			if (ctx->loop_exit_count >= ctx->loop_exit_capacity) {
				ctx->loop_exit_capacity = (ctx->loop_exit_capacity == 0) ? 8 : ctx->loop_exit_capacity * 2;
				ctx->loop_exit_labels = realloc(ctx->loop_exit_labels, ctx->loop_exit_capacity * sizeof(char *));
			}
			ctx->loop_exit_labels[ctx->loop_exit_count] = exit_label;
			ctx->loop_exit_count++;
			/* No increment in this loop form, so `continue` just re-tests the condition. */
			if (ctx->loop_cont_count >= ctx->loop_cont_capacity) {
				ctx->loop_cont_capacity = (ctx->loop_cont_capacity == 0) ? 8 : ctx->loop_cont_capacity * 2;
				ctx->loop_cont_labels = realloc(ctx->loop_cont_labels, ctx->loop_cont_capacity * sizeof(char *));
			}
			ctx->loop_cont_labels[ctx->loop_cont_count] = loop_label;
			ctx->loop_cont_count++;

			buffer_append_fmt(ctx, "  br label %s\n", loop_label);
			buffer_append_fmt(ctx, "%s:\n", loop_label + 1);

			if (stmt->data.for_stmt.cond) {
				char cond_buf[256];
				codegen_expression(ctx, stmt->data.for_stmt.cond, cond_buf);
				char *cond_i1 = gen_value_name(ctx);
				/* Truthiness: nonzero is true; use the cond value's real width. */
				buffer_append_fmt(ctx, "  %s = icmp ne %s %s, 0\n", cond_i1, cond_int_type(stmt->data.for_stmt.cond),
				                  cond_buf);
				buffer_append_fmt(ctx, "  br i1 %s, label %s, label %s\n", cond_i1, body_label, exit_label);
			} else {
				buffer_append_fmt(ctx, "  br label %s\n", body_label);
			}

			buffer_append_fmt(ctx, "%s:\n", body_label + 1);

			push_value_scope(ctx);
			for (int i = 0; i < stmt->data.for_stmt.body_count; i++) {
				codegen_statement(ctx, stmt->data.for_stmt.body[i]);
			}
			pop_value_scope(ctx);

			buffer_append_fmt(ctx, "  br label %s\n", loop_label);
			buffer_append_fmt(ctx, "%s:\n", exit_label + 1);

			ctx->loop_exit_count--;
			ctx->loop_cont_count--;
		}
		break;
	}

	case HIR_STMT_BLOCK:
		/* A scoped statement sequence (match desugar / surface `{ }` block). Locals are
		 * function-scoped SSA/allocas, so emit the statements in sequence. */
		for (int i = 0; i < stmt->data.block.count; i++)
			codegen_statement(ctx, stmt->data.block.stmts[i]);
		break;
	case HIR_STMT_IF: {
		char cond_buf[256];
		codegen_expression(ctx, stmt->data.if_stmt.cond, cond_buf);

		/* Bridge the arche-int condition to LLVM's i1 (which `br` requires). Truthiness: nonzero is
		 * true — `icmp ne <val>, 0` (NOT `trunc`, which tests only the low bit, making `if (2)`
		 * false). Use the condition's real width (bool=i8, int=i32, opaque/handle cell=i64, …). */
		const char *branch_cond = cond_buf;
		if (cond_buf[0] == '%') {
			char *truncated = gen_value_name(ctx);
			buffer_append_fmt(ctx, "  %s = icmp ne %s %s, 0\n", truncated, cond_int_type(stmt->data.if_stmt.cond),
			                  cond_buf);
			branch_cond = truncated;
		}

		int has_else = stmt->data.if_stmt.else_count > 0;

		char *then_label = gen_value_name(ctx);
		char *else_label = has_else ? gen_value_name(ctx) : NULL;
		char *exit_label = gen_value_name(ctx);

		/* false jumps to else body when present, otherwise straight to exit */
		buffer_append_fmt(ctx, "  br i1 %s, label %s, label %s\n", branch_cond, then_label,
		                  has_else ? else_label : exit_label);

		buffer_append_fmt(ctx, "%s:\n", then_label + 1);
		ctx->block_terminated = 0; /* fresh live block */
		for (int i = 0; i < stmt->data.if_stmt.then_count; i++) {
			codegen_statement(ctx, stmt->data.if_stmt.then_body[i]);
		}
		buffer_append_fmt(ctx, "  br label %s\n", exit_label);

		if (has_else) {
			buffer_append_fmt(ctx, "%s:\n", else_label + 1);
			ctx->block_terminated = 0; /* fresh live block */
			for (int i = 0; i < stmt->data.if_stmt.else_count; i++) {
				codegen_statement(ctx, stmt->data.if_stmt.else_body[i]);
			}
			buffer_append_fmt(ctx, "  br label %s\n", exit_label);
		}

		buffer_append_fmt(ctx, "%s:\n", exit_label + 1);
		ctx->block_terminated = 0; /* exit block is live (control merges here) */
		break;
	}

	case HIR_STMT_RUN: {
		/* run system - call one function with all matching archetypes as params */
		const char *system_name = stmt->data.run_stmt.system_name;

		/* Find the system definition */
		HirSysDecl *sys = find_sys_decl(ctx, system_name);
		if (!sys) {
			fprintf(stderr, "Error: `run %s` — unknown system '%s'\n", system_name, system_name);
			ctx->had_error = 1;
			buffer_append_fmt(ctx, "  ; ERROR: undefined system '%s'\n", system_name);
			break;
		}

		/* Collect matching archetypes */
		const char *matching_archs[256];
		int matching_count = 0;

		for (int d = 0; d < ctx->ast->decl_count; d++) {
			HirDecl *decl = ctx->ast->decls[d];
			if (decl->kind == HIR_DECL_ARCHETYPE) {
				HirArchetypeDecl *arch = decl->data.archetype;
				if (archetype_matches_system(arch, sys) && matching_count < 256) {
					matching_archs[matching_count++] = arch->name;
				}
			}
		}

		if (matching_count == 0) {
			/* The system reads/writes component columns, but no shape in the program provides all of
			 * them — running it would be a silent no-op. In the device/driver model a driver must
			 * define an archetype with the device's required components. Hard error. */
			fprintf(stderr, "Error: `run %s` — no shape provides the components system '%s' operates on { ",
			        system_name, system_name);
			for (int p = 0; p < sys->param_count; p++)
				fprintf(stderr, "%s%s", p ? ", " : "", sys->params[p] ? sys->params[p]->name : "?");
			fprintf(stderr, " }; a driver must define an archetype with these components\n");
			ctx->had_error = 1;
			buffer_append_fmt(ctx, "  ; ERROR: no matching archetypes for system '%s'\n", system_name);
			break;
		}

		/* Load pointers for dynamic archetypes first */
		char *dynamic_ptrs[256];
		for (int i = 0; i < matching_count; i++) {
			const char *arch_name = matching_archs[i];
			if (get_arch_static_capacity(ctx, arch_name) == 0) {
				char *loaded = gen_value_name(ctx);
				buffer_append_fmt(ctx, "  %s = load %%struct.%s*, %%struct.%s** @archetype_%s\n", loaded, arch_name,
				                  arch_name, arch_name);
				dynamic_ptrs[i] = loaded;
			} else {
				dynamic_ptrs[i] = NULL;
			}
		}

		/* Build: call void @system_name(%struct.A* @A, %struct.B* @B, ...) */
		char sys_call_buf[512];
		buffer_append_fmt(ctx, "  call void @%s(", cg_fnsym(ctx, system_name, 0, sys_call_buf, sizeof(sys_call_buf)));
		for (int i = 0; i < matching_count; i++) {
			if (i > 0)
				buffer_append(ctx, ", ");
			const char *arch_name = matching_archs[i];
			if (get_arch_static_capacity(ctx, arch_name) > 0) {
				buffer_append_fmt(ctx, "%%struct.%s* @%s", arch_name, arch_name);
			} else {
				buffer_append_fmt(ctx, "%%struct.%s* %s", arch_name, dynamic_ptrs[i]);
			}
		}
		buffer_append(ctx, ")\n");

		break;
	}

	case HIR_STMT_EXPR: {
		/* Handle archetype allocation as statement: alloc Particle(5); */
		if (stmt->data.expr_stmt.expr->kind == HIR_EXPR_ALLOC) {
			const char *arch_name = stmt->data.expr_stmt.expr->data.alloc.archetype_name;
			char expr_buf[256];
			codegen_expression(ctx, stmt->data.expr_stmt.expr, expr_buf);

			/* Store allocated pointer in global variable for this archetype */
			char arch_var[256];
			snprintf(arch_var, sizeof(arch_var), "%%archetype_%s", arch_name);
			buffer_append_fmt(ctx, "  store %%struct.%s* %s, %%struct.%s** @archetype_%s\n", arch_name, expr_buf,
			                  arch_name, arch_name);

			/* Add archetype to scope so sys functions can find it */
			add_arch_value(ctx, arch_name, expr_buf, arch_name);
		} else {
			char expr_buf[256];
			codegen_expression(ctx, stmt->data.expr_stmt.expr, expr_buf);
		}
		break;
	}

	case HIR_STMT_BREAK: {
		if (ctx->loop_exit_count > 0) {
			char *exit_label = ctx->loop_exit_labels[ctx->loop_exit_count - 1];
			buffer_append_fmt(ctx, "  br label %s\n", exit_label);
			/* Emit unreachable block label so LLVM doesn't complain about empty BB */
			char *unreachable_label = gen_value_name(ctx);
			buffer_append_fmt(ctx, "%s:\n", unreachable_label + 1);
			buffer_append(ctx, "  unreachable\n");
		} else {
			fprintf(stderr, "Error: break statement outside of loop\n");
		}
		break;
	}

	case HIR_STMT_CONTINUE: {
		if (ctx->loop_cont_count > 0) {
			char *cont_label = ctx->loop_cont_labels[ctx->loop_cont_count - 1];
			buffer_append_fmt(ctx, "  br label %s\n", cont_label);
			/* Emit an unreachable block label so LLVM doesn't see a fallthrough into dead code. */
			char *unreachable_label = gen_value_name(ctx);
			buffer_append_fmt(ctx, "%s:\n", unreachable_label + 1);
			buffer_append(ctx, "  unreachable\n");
		} else {
			fprintf(stderr, "Error: continue statement outside of loop\n");
		}
		break;
	}

	case HIR_STMT_RETURN: {
		/* TODO(sret): a from-scratch aggregate return (`x := make()`, with no `own` input buffer to
		 * thread back) could caller-allocate the bind target and pass it as a hidden out-pointer
		 * here, so the result is built directly in the caller's slot. Deferred — today such a value
		 * is produced by threading an `own` buffer (`g: T[N]; g := fill(move g)`). */
		HirReturnStmt *rs = &stmt->data.return_stmt;
		const char *ret_type = ctx->current_return_type ? ctx->current_return_type : "i32";
		/* RAII: returning an opaque local moves it out — suppress its auto-drop. */
		for (int ri = 0; ri < rs->count; ri++)
			if (rs->values[ri] && rs->values[ri]->kind == HIR_EXPR_NAME)
				drop_mark_consumed(ctx, rs->values[ri]->data.name.name);
		if (rs->count == 0) {
			/* Naked `return;` — early exit from a proc (which is `define void`). Out-params are
			 * written in place as the body runs, so an early exit just terminates the block.
			 * RAII: auto-drop all still-live opaque locals before the early `ret`. */
			drop_exit_all_for_return(ctx);
			buffer_append(ctx, "  ret void\n");
			break;
		}
		/* RAII: drop still-live opaque locals before the value `ret` (after marking returned
		 * ones consumed above so we don't drop the value being handed back). */
		drop_exit_all_for_return(ctx);
		if (rs->count <= 1) {
			/* Returning a non-char `T[]` slice: hand back the fat pointer `{T*, i64}` built from the
			 * returned value's element pointer + runtime length (so the caller recovers .length after
			 * a move-and-return). The returned expr is a slice/array name, possibly wrapped in move. */
			HirType *rt0 = (ctx->current_return_type_count >= 1) ? ctx->current_return_types[0] : NULL;
			int slice_ret = rt0 && rt0->tag == HIR_TYPE_ARRAY;
			HirExpr *rv = rs->values[0];
			while (rv && rv->kind == HIR_EXPR_UNARY &&
			       (rv->data.unary.op == UNARY_MOVE || rv->data.unary.op == UNARY_COPY))
				rv = rv->data.unary.operand;
			ValueInfo *sv = (slice_ret && rv && rv->kind == HIR_EXPR_NAME) ? find_value(ctx, rv->data.name.name) : NULL;
			if (slice_ret && sv && sv->type == 6) {
				const char *e = llvm_type_from_arche(sv->field_type ? sv->field_type : "int");
				char ptrt[16];
				snprintf(ptrt, sizeof(ptrt), "%s*", e);
				char lenbuf[40];
				if (sv->is_slice && sv->len_ssa)
					snprintf(lenbuf, sizeof(lenbuf), "%s", sv->len_ssa);
				else
					snprintf(lenbuf, sizeof(lenbuf), "%d", sv->string_len);
				char *a1 = gen_value_name(ctx);
				buffer_append_fmt(ctx, "  %s = insertvalue %s undef, %s %s, 0\n", a1, ret_type, ptrt, sv->llvm_name);
				char *a2 = gen_value_name(ctx);
				buffer_append_fmt(ctx, "  %s = insertvalue %s %s, i64 %s, 1\n", a2, ret_type, a1, lenbuf);
				buffer_append_fmt(ctx, "  ret %s %s\n", ret_type, a2);
				break;
			}
			/* Returning a STRING LITERAL as `char[]`: codegen_expression yields the i8* data pointer,
			 * but the slice return type is `{i8*, i64}` — wrap it with the literal's byte length so we
			 * hand back a proper (ptr,len) slice (e.g. `return "text/html"`). */
			if (slice_ret && rv && rv->kind == HIR_EXPR_STRING) {
				char sbuf[256];
				codegen_expression(ctx, rs->values[0], sbuf);
				char *a1 = gen_value_name(ctx);
				buffer_append_fmt(ctx, "  %s = insertvalue %s undef, i8* %s, 0\n", a1, ret_type, sbuf);
				char *a2 = gen_value_name(ctx);
				buffer_append_fmt(ctx, "  %s = insertvalue %s %s, i64 %d, 1\n", a2, ret_type, a1,
				                  rv->data.string.length);
				buffer_append_fmt(ctx, "  ret %s %s\n", ret_type, a2);
				break;
			}
			char value_buf[256];
			codegen_expression(ctx, rs->values[0], value_buf);
			buffer_append_fmt(ctx, "  ret %s %s\n", ret_type, value_buf);
		} else {
			/* Multi-value return: build the aggregate with insertvalue, then ret it. */
			char prev[64];
			strcpy(prev, "undef");
			for (int i = 0; i < rs->count; i++) {
				char vbuf[256];
				codegen_expression(ctx, rs->values[i], vbuf);
				const char *mt =
				    (i < ctx->current_return_type_count) ? return_member_llvm(ctx->current_return_types[i]) : "i32";
				char *acc = gen_value_name(ctx);
				buffer_append_fmt(ctx, "  %s = insertvalue %s %s, %s %s, %d\n", acc, ret_type, prev, mt, vbuf, i);
				strncpy(prev, acc, sizeof(prev) - 1);
				prev[sizeof(prev) - 1] = '\0';
			}
			buffer_append_fmt(ctx, "  ret %s %s\n", ret_type, prev);
		}
		break;
	}
	case HIR_STMT_EACH_FIELD: {
		/* Only meaningful inside a monomorphized archetype-parametric proc. */
		if (!ctx->current_archetype_param)
			break;
		HirEachFieldStmt *ef = &stmt->data.each_field;
		HirArchetypeDecl *arch = ctx->current_archetype_param;
		if (strcmp(ef->arch_param_name, ctx->current_arch_param_name) != 0)
			break;

		HirTypeTag filter_tag = HIR_TYPE_UNKNOWN;
		if (ef->filter_type) {
			filter_tag = ef->filter_type->tag;
		}

		for (int i = 0; i < arch->field_count; i++) {
			HirField *fd = arch->fields[i];
			if (!fd || fd->kind != FIELD_COLUMN)
				continue;
			HirType *t = fd->type;
			if (!t)
				continue;
			HirTypeTag tag = t->tag;
			if (tag != HIR_TYPE_INT && tag != HIR_TYPE_FLOAT && tag != HIR_TYPE_CHAR)
				continue;
			if (ef->filter_type && tag != filter_tag) {
				continue;
			}

			/* Lazily emit a name global for this (arch, field). */
			int slen = (int)strlen(fd->name) + 1;
			char gname[64];
			snprintf(gname, sizeof(gname), "@.efield_name_%d", ctx->efield_name_counter++);
			char tmpbuf[256];
			snprintf(tmpbuf, sizeof(tmpbuf), "%s = private unnamed_addr constant [%d x i8] c\"", gname, slen);
			/* append the field name + null byte literal. We escape nothing because
			 * arche field identifiers are [A-Za-z0-9_]. */
			size_t pos = strlen(tmpbuf);
			for (int k = 0; k < slen - 1 && pos < sizeof(tmpbuf) - 8; k++) {
				tmpbuf[pos++] = fd->name[k];
			}
			tmpbuf[pos++] = '\\';
			tmpbuf[pos++] = '0';
			tmpbuf[pos++] = '0';
			tmpbuf[pos++] = '"';
			tmpbuf[pos++] = ',';
			tmpbuf[pos++] = ' ';
			tmpbuf[pos++] = '\0';
			/* Append to globals buffer */
			while (ctx->globals_pos + strlen(tmpbuf) + 16 >= ctx->globals_size) {
				ctx->globals_size *= 2;
				ctx->globals_buffer = realloc(ctx->globals_buffer, ctx->globals_size);
			}
			strcpy(ctx->globals_buffer + ctx->globals_pos, tmpbuf);
			ctx->globals_pos += strlen(tmpbuf);
			/* The constant emitter ends each entry with a newline. Match that. */
			strcpy(ctx->globals_buffer + ctx->globals_pos, "align 1\n");
			ctx->globals_pos += strlen("align 1\n");

			/* Push expansion state. */
			const char *saved_bind = ctx->current_each_field_binding;
			HirField *saved_target = ctx->current_each_field_target;
			int saved_idx = ctx->current_each_field_index;
			const char *saved_gn = ctx->current_each_field_name_global;
			ctx->current_each_field_binding = ef->binding_name;
			ctx->current_each_field_target = fd;
			ctx->current_each_field_index = i;
			ctx->current_each_field_name_global = strdup(gname);

			push_value_scope(ctx);
			for (int j = 0; j < ef->body_count; j++) {
				codegen_statement(ctx, ef->body[j]);
			}
			pop_value_scope(ctx);

			free((void *)ctx->current_each_field_name_global);
			ctx->current_each_field_binding = saved_bind;
			ctx->current_each_field_target = saved_target;
			ctx->current_each_field_index = saved_idx;
			ctx->current_each_field_name_global = saved_gn;
		}
		break;
	}
	}
}

/* ========== DECLARATION CODEGEN ========== */

static void codegen_archetype_decl(CodegenContext *ctx, HirArchetypeDecl *arch) {
	/* One shape = one pool. Aliases (other names for the same component set) share the
	 * canonical decl's struct + storage + helpers, so emit only for the canonical. */
	if (canonical_archetype_decl(ctx, arch) != arch)
		return;

	int static_cap = get_arch_static_capacity(ctx, arch->name);

	/* Generate struct definition for archetype */
	buffer_append_fmt(ctx, "%%struct.%s = type {\n", arch->name);

	for (int i = 0; i < arch->field_count; i++) {
		const char *base_type = llvm_type_from_arche(field_base_type_name(arch->fields[i]->type));

		if (arch->fields[i]->kind == FIELD_COLUMN) {
			/* Array columns (char[N], T[N]) need N slots per row, not one. */
			int per_row = field_total_elements(arch->fields[i]->type);
			if (static_cap > 0)
				buffer_append_fmt(ctx, "  [%d x %s]", static_cap * per_row, base_type);
			else
				buffer_append_fmt(ctx, "  %s*", base_type);
		} else {
			buffer_append_fmt(ctx, "  %s", base_type);
		}

		buffer_append(ctx, ",\n");
	}

	/* count field */
	buffer_append(ctx, "  i64,\n");

	if (static_cap > 0) {
		/* Static: inline free_list array + free_count + gen_counters; no capacity field */
		buffer_append_fmt(ctx, "  [%d x i64],\n", static_cap);
		buffer_append_fmt(ctx, "  i64,\n");
		buffer_append_fmt(ctx, "  [%d x i32]\n", static_cap);
	} else {
		/* Dynamic: capacity + free_list pointer + free_count + gen_counters pointer */
		buffer_append(ctx, "  i64,\n");
		buffer_append(ctx, "  i64*,\n");
		buffer_append(ctx, "  i64,\n");
		buffer_append(ctx, "  i32*\n");
	}

	buffer_append(ctx, "}\n\n");

	/* Emit insert helper function */
	buffer_append_fmt(ctx, "define %si64 @arche_insert_%s(%%struct.%s* %%arch", cg_shared(ctx), arch->name, arch->name);
	for (int i = 0; i < arch->field_count; i++) {
		if (arch->fields[i]->kind == FIELD_COLUMN) {
			const char *base_type = llvm_type_from_arche(field_base_type_name(arch->fields[i]->type));
			/* char[N] column: receive the source by pointer (memcpy'd into the row below).
			 * Numeric array columns keep scalar element-0 init. */
			if (field_total_elements(arch->fields[i]->type) > 1 && strcmp(base_type, "i8") == 0) {
				buffer_append_fmt(ctx, ", %s* %%f%d", base_type, i);
			} else {
				buffer_append_fmt(ctx, ", %s %%f%d", base_type, i);
			}
		}
	}
	buffer_append(ctx, ") {\n");
	buffer_append(ctx, "entry:\n");

	/* Setup allocas for slot and increment flag */
	buffer_append(ctx, "  %slot_var = alloca i64\n");
	buffer_append(ctx, "  %do_incr = alloca i1\n");

	int count_idx = arch->field_count;
	int fl_idx = static_cap > 0 ? arch->field_count + 1 : arch->field_count + 2;
	int fc_idx = static_cap > 0 ? arch->field_count + 2 : arch->field_count + 3;
	int gc_idx = static_cap > 0 ? arch->field_count + 3 : arch->field_count + 4;
	int cap_idx = arch->field_count + 1; /* dynamic only */

	buffer_append_fmt(ctx, "  %%fc_ptr = getelementptr %%struct.%s, %%struct.%s* %%arch, i32 0, i32 %d\n", arch->name,
	                  arch->name, fc_idx);
	buffer_append(ctx, "  %free_count = load i64, i64* %fc_ptr\n");
	buffer_append(ctx, "  %has_free = icmp sgt i64 %free_count, 1\n");
	buffer_append(ctx, "  br i1 %has_free, label %pop_free, label %check_capacity\n\n");

	/* Pop from free_list */
	buffer_append(ctx, "pop_free:\n");
	buffer_append(ctx, "  %new_fc = sub i64 %free_count, 1\n");
	if (static_cap > 0) {
		/* Inline array: 3-index GEP */
		buffer_append_fmt(
		    ctx, "  %%slot_ptr = getelementptr %%struct.%s, %%struct.%s* %%arch, i32 0, i32 %d, i64 %%new_fc\n",
		    arch->name, arch->name, fl_idx);
	} else {
		buffer_append_fmt(ctx, "  %%fl_ptr = getelementptr %%struct.%s, %%struct.%s* %%arch, i32 0, i32 %d\n",
		                  arch->name, arch->name, fl_idx);
		buffer_append(ctx, "  %free_list = load i64*, i64** %fl_ptr\n");
		buffer_append(ctx, "  %slot_ptr = getelementptr i64, i64* %free_list, i64 %new_fc\n");
	}
	buffer_append(ctx, "  %slot = load i64, i64* %slot_ptr\n");
	buffer_append(ctx, "  store i64 %new_fc, i64* %fc_ptr\n");
	buffer_append(ctx, "  store i64 %slot, i64* %slot_var\n");
	buffer_append(ctx, "  store i1 0, i1* %do_incr\n");
	buffer_append(ctx, "  br label %write_fields\n\n");

	/* Check if count is at capacity (fixed budget, no growth) */
	buffer_append(ctx, "check_capacity:\n");
	buffer_append_fmt(ctx, "  %%count_ptr = getelementptr %%struct.%s, %%struct.%s* %%arch, i32 0, i32 %d\n",
	                  arch->name, arch->name, count_idx);
	buffer_append(ctx, "  %count = load i64, i64* %count_ptr\n");
	if (static_cap > 0) {
		buffer_append_fmt(ctx, "  %%at_capacity = icmp sge i64 %%count, %d\n", static_cap);
	} else {
		buffer_append_fmt(ctx, "  %%cap_ptr = getelementptr %%struct.%s, %%struct.%s* %%arch, i32 0, i32 %d\n",
		                  arch->name, arch->name, cap_idx);
		buffer_append(ctx, "  %cap = load i64, i64* %cap_ptr\n");
		buffer_append(ctx, "  %at_capacity = icmp sge i64 %count, %cap\n");
	}
	buffer_append(ctx, "  br i1 %at_capacity, label %overflow, label %use_count\n\n");

	/* Overflow: abort (fixed budget exceeded) */
	buffer_append(ctx, "overflow:\n");
	buffer_append(ctx, "  call void @abort()\n");
	buffer_append(ctx, "  unreachable\n\n");

	/* Use count as slot */
	buffer_append(ctx, "use_count:\n");
	buffer_append(ctx, "  store i64 %count, i64* %slot_var\n");
	buffer_append(ctx, "  store i1 1, i1* %do_incr\n");
	buffer_append(ctx, "  br label %write_fields\n\n");

	/* Write fields block */
	buffer_append(ctx, "write_fields:\n");
	buffer_append(ctx, "  %final_slot = load i64, i64* %slot_var\n");
	int col_idx = 0;
	for (int i = 0; i < arch->field_count; i++) {
		if (arch->fields[i]->kind == FIELD_COLUMN) {
			const char *base_type = llvm_type_from_arche(field_base_type_name(arch->fields[i]->type));
			int col_n = field_total_elements(arch->fields[i]->type);
			const char *flat_slot = "%final_slot";
			char flat_slot_buf[64];
			if (col_n > 1) {
				snprintf(flat_slot_buf, sizeof(flat_slot_buf), "%%flat_slot_%d", col_idx);
				buffer_append_fmt(ctx, "  %s = mul i64 %%final_slot, %d\n", flat_slot_buf, col_n);
				flat_slot = flat_slot_buf;
			}
			if (static_cap > 0) {
				buffer_append_fmt(
				    ctx, "  %%slot%d = getelementptr %%struct.%s, %%struct.%s* %%arch, i32 0, i32 %d, i64 %s\n",
				    col_idx, arch->name, arch->name, i, flat_slot);
			} else {
				buffer_append_fmt(ctx,
				                  "  %%col_pp2_%d = getelementptr %%struct.%s, %%struct.%s* %%arch, i32 0, i32 %d\n",
				                  col_idx, arch->name, arch->name, i);
				buffer_append_fmt(ctx, "  %%col_p2_%d = load %s*, %s** %%col_pp2_%d\n", col_idx, base_type, base_type,
				                  col_idx);
				buffer_append_fmt(ctx, "  %%slot%d = getelementptr %s, %s* %%col_p2_%d, i64 %s\n", col_idx, base_type,
				                  base_type, col_idx, flat_slot);
			}
			if (col_n > 1 && strcmp(base_type, "i8") == 0) {
				/* char[N] column: copy the whole row (col_n bytes) from the source
				 * pointer %f<i> into the row slot. Numeric array columns fall through
				 * to the scalar store below (element-0 init, legacy semantics). */
				int bytes = col_n * llvm_type_sizeof(base_type);
				char dstbuf[64];
				char srcbuf[64];
				if (strcmp(base_type, "i8") == 0) {
					snprintf(dstbuf, sizeof dstbuf, "%%slot%d", col_idx);
					snprintf(srcbuf, sizeof srcbuf, "%%f%d", i);
				} else {
					snprintf(dstbuf, sizeof dstbuf, "%%mcdst%d", col_idx);
					snprintf(srcbuf, sizeof srcbuf, "%%mcsrc%d", col_idx);
					buffer_append_fmt(ctx, "  %s = bitcast %s* %%slot%d to i8*\n", dstbuf, base_type, col_idx);
					buffer_append_fmt(ctx, "  %s = bitcast %s* %%f%d to i8*\n", srcbuf, base_type, i);
				}
				buffer_append_fmt(ctx, "  call void @llvm.memcpy.p0.p0.i64(i8* %s, i8* %s, i64 %d, i1 false)\n", dstbuf,
				                  srcbuf, bytes);
				ctx->uses_memcpy = 1;
			} else {
				buffer_append_fmt(ctx, "  store %s %%f%d, %s* %%slot%d\n", base_type, i, base_type, col_idx);
			}
			col_idx++;
		}
	}

	/* Conditionally increment count */
	buffer_append(ctx, "  %should_incr = load i1, i1* %do_incr\n");
	buffer_append(ctx, "  br i1 %should_incr, label %do_incr_count, label %done\n\n");

	buffer_append(ctx, "do_incr_count:\n");
	buffer_append_fmt(ctx, "  %%count_ptr2 = getelementptr %%struct.%s, %%struct.%s* %%arch, i32 0, i32 %d\n",
	                  arch->name, arch->name, count_idx);
	buffer_append(ctx, "  %curr_count = load i64, i64* %count_ptr2\n");
	buffer_append(ctx, "  %new_count = add i64 %curr_count, 1\n");
	buffer_append(ctx, "  store i64 %new_count, i64* %count_ptr2\n");
	buffer_append(ctx, "  br label %done\n\n");

	buffer_append(ctx, "done:\n");
	/* Load gen_counters[final_slot] */
	if (static_cap > 0) {
		buffer_append_fmt(
		    ctx, "  %%gc_elem = getelementptr %%struct.%s, %%struct.%s* %%arch, i32 0, i32 %d, i64 %%final_slot\n",
		    arch->name, arch->name, gc_idx);
	} else {
		buffer_append_fmt(ctx, "  %%gc_ptr_field = getelementptr %%struct.%s, %%struct.%s* %%arch, i32 0, i32 %d\n",
		                  arch->name, arch->name, gc_idx);
		buffer_append(ctx, "  %gc_arr = load i32*, i32** %gc_ptr_field\n");
		buffer_append(ctx, "  %gc_elem = getelementptr i32, i32* %gc_arr, i64 %final_slot\n");
	}
	buffer_append(ctx, "  %gen_i32 = load i32, i32* %gc_elem\n");
	buffer_append(ctx, "  %gen_i64 = zext i32 %gen_i32 to i64\n");
	buffer_append(ctx, "  %gen_shifted = shl i64 %gen_i64, 32\n");
	buffer_append(ctx, "  %slot_i32 = trunc i64 %final_slot to i32\n");
	buffer_append(ctx, "  %slot_i64 = zext i32 %slot_i32 to i64\n");
	buffer_append(ctx, "  %handle = or i64 %slot_i64, %gen_shifted\n");
	buffer_append(ctx, "  ret i64 %handle\n");
	buffer_append(ctx, "}\n\n");

	/* Emit delete helper function */
	buffer_append_fmt(ctx, "define %svoid @arche_delete_%s(%%struct.%s* %%arch, i64 %%handle) {\n", cg_shared(ctx),
	                  arch->name, arch->name);
	buffer_append(ctx, "entry:\n");

	/* Unpack slot and generation from handle */
	buffer_append(ctx, "  %slot_i32 = trunc i64 %handle to i32\n");
	buffer_append(ctx, "  %slot = zext i32 %slot_i32 to i64\n");
	buffer_append(ctx, "  %hgen_raw = lshr i64 %handle, 32\n");
	buffer_append(ctx, "  %hgen = trunc i64 %hgen_raw to i32\n");

	/* Load gen_counters[slot] */
	if (static_cap > 0) {
		buffer_append_fmt(ctx,
		                  "  %%gc_elem = getelementptr %%struct.%s, %%struct.%s* %%arch, i32 0, i32 %d, i64 %%slot\n",
		                  arch->name, arch->name, gc_idx);
	} else {
		buffer_append_fmt(ctx, "  %%gc_ptr_field = getelementptr %%struct.%s, %%struct.%s* %%arch, i32 0, i32 %d\n",
		                  arch->name, arch->name, gc_idx);
		buffer_append(ctx, "  %gc_arr = load i32*, i32** %gc_ptr_field\n");
		buffer_append(ctx, "  %gc_elem = getelementptr i32, i32* %gc_arr, i64 %slot\n");
	}
	buffer_append(ctx, "  %stored_gen = load i32, i32* %gc_elem\n");

	/* Validate generation */
	buffer_append(ctx, "  %gen_ok = icmp eq i32 %hgen, %stored_gen\n");
	buffer_append(ctx, "  br i1 %gen_ok, label %valid, label %stale\n\n");

	buffer_append(ctx, "stale:\n");
	buffer_append(ctx, "  call void @abort()\n");
	buffer_append(ctx, "  unreachable\n\n");

	buffer_append(ctx, "valid:\n");
	/* Crash loudly on generation exhaustion instead of wrapping (silent ABA): a
	 * slot freed 2^32 times can no longer mint a distinct generation, so a stale
	 * handle could alias a fresh entity. Abort like a stack overflow would. */
	buffer_append(ctx, "  %gen_maxed = icmp eq i32 %stored_gen, -1\n");
	buffer_append(ctx, "  br i1 %gen_maxed, label %gen_exhausted, label %do_free\n\n");
	buffer_append(ctx, "gen_exhausted:\n");
	buffer_append(ctx, "  call void @abort()\n");
	buffer_append(ctx, "  unreachable\n\n");
	buffer_append(ctx, "do_free:\n");
	/* Increment generation */
	buffer_append(ctx, "  %new_gen = add i32 %stored_gen, 1\n");
	buffer_append(ctx, "  store i32 %new_gen, i32* %gc_elem\n");

	/* Load free_count */
	buffer_append_fmt(ctx, "  %%fc_ptr = getelementptr %%struct.%s, %%struct.%s* %%arch, i32 0, i32 %d\n", arch->name,
	                  arch->name, fc_idx);
	buffer_append(ctx, "  %free_count = load i64, i64* %fc_ptr\n");

	/* Push slot to free_list[free_count] */
	if (static_cap > 0) {
		buffer_append_fmt(
		    ctx, "  %%slot_ptr = getelementptr %%struct.%s, %%struct.%s* %%arch, i32 0, i32 %d, i64 %%free_count\n",
		    arch->name, arch->name, fl_idx);
	} else {
		buffer_append_fmt(ctx, "  %%fl_ptr = getelementptr %%struct.%s, %%struct.%s* %%arch, i32 0, i32 %d\n",
		                  arch->name, arch->name, fl_idx);
		buffer_append(ctx, "  %free_list = load i64*, i64** %fl_ptr\n");
		buffer_append(ctx, "  %slot_ptr = getelementptr i64, i64* %free_list, i64 %free_count\n");
	}
	buffer_append(ctx, "  store i64 %slot, i64* %slot_ptr\n");

	/* Increment free_count */
	buffer_append(ctx, "  %new_fc = add i64 %free_count, 1\n");
	buffer_append(ctx, "  store i64 %new_fc, i64* %fc_ptr\n");

	buffer_append(ctx, "  ret void\n");
	buffer_append(ctx, "}\n\n");

	/* Foreign-archetype cell-deref helper: given a handle, validate the
	 * generation (abort on stale) and return the single opaque cell for the
	 * C-marshal layer to hand to extern functions. Emitted only for shapes that
	 * carry an opaque field (the marshal's "unwrap handle -> C pointer"). */
	{
		int opaque_col = -1;
		for (int i = 0; i < arch->field_count; i++) {
			if (arch->fields[i]->type && strcmp(field_base_type_name(arch->fields[i]->type), "opaque") == 0) {
				opaque_col = i;
				break;
			}
		}
		if (opaque_col >= 0) {
			buffer_append_fmt(ctx, "define %si64 @arche_cell_%s(%%struct.%s* %%arch, i64 %%handle) {\n", cg_shared(ctx),
			                  arch->name, arch->name);
			buffer_append(ctx, "entry:\n");
			buffer_append(ctx, "  %slot_i32 = trunc i64 %handle to i32\n");
			buffer_append(ctx, "  %slot = zext i32 %slot_i32 to i64\n");
			buffer_append(ctx, "  %hgen_raw = lshr i64 %handle, 32\n");
			buffer_append(ctx, "  %hgen = trunc i64 %hgen_raw to i32\n");
			if (static_cap > 0) {
				buffer_append_fmt(
				    ctx, "  %%gc_elem = getelementptr %%struct.%s, %%struct.%s* %%arch, i32 0, i32 %d, i64 %%slot\n",
				    arch->name, arch->name, gc_idx);
			} else {
				buffer_append_fmt(ctx,
				                  "  %%gc_ptr_field = getelementptr %%struct.%s, %%struct.%s* %%arch, i32 0, i32 %d\n",
				                  arch->name, arch->name, gc_idx);
				buffer_append(ctx, "  %gc_arr = load i32*, i32** %gc_ptr_field\n");
				buffer_append(ctx, "  %gc_elem = getelementptr i32, i32* %gc_arr, i64 %slot\n");
			}
			buffer_append(ctx, "  %stored_gen = load i32, i32* %gc_elem\n");
			buffer_append(ctx, "  %gen_ok = icmp eq i32 %hgen, %stored_gen\n");
			buffer_append(ctx, "  br i1 %gen_ok, label %valid, label %stale\n\n");
			buffer_append(ctx, "stale:\n");
			buffer_append(ctx, "  call void @abort()\n");
			buffer_append(ctx, "  unreachable\n\n");
			buffer_append(ctx, "valid:\n");
			if (static_cap > 0) {
				buffer_append_fmt(
				    ctx, "  %%cell_ptr = getelementptr %%struct.%s, %%struct.%s* %%arch, i32 0, i32 %d, i64 %%slot\n",
				    arch->name, arch->name, opaque_col);
			} else {
				buffer_append_fmt(ctx, "  %%col_pp = getelementptr %%struct.%s, %%struct.%s* %%arch, i32 0, i32 %d\n",
				                  arch->name, arch->name, opaque_col);
				buffer_append(ctx, "  %col_p = load i64*, i64** %col_pp\n");
				buffer_append(ctx, "  %cell_ptr = getelementptr i64, i64* %col_p, i64 %slot\n");
			}
			buffer_append(ctx, "  %cell = load i64, i64* %cell_ptr\n");
			buffer_append(ctx, "  ret i64 %cell\n");
			buffer_append(ctx, "}\n\n");
		}
	}

	/* Emit dealloc helper function */
	buffer_append_fmt(ctx, "define %svoid @arche_dealloc_%s(%%struct.%s* %%arch) {\n", cg_shared(ctx), arch->name,
	                  arch->name);
	buffer_append(ctx, "entry:\n");
	if (static_cap > 0) {
		/* Static global — nothing to free */
	} else {
		buffer_append(ctx, "  %arch_i8 = bitcast %struct.");
		buffer_append_fmt(ctx, "%s* %%arch to i8*\n", arch->name);
		buffer_append(ctx, "  call void @free(i8* %arch_i8)\n");
	}
	buffer_append(ctx, "  ret void\n");
	buffer_append(ctx, "}\n\n");

	/* Emit global variable for this archetype (after struct type is defined). Shared across per-unit
	 * modules → linkonce_odr so the linker folds the identical duplicates to one pool. */
	if (static_cap > 0) {
		buffer_append_fmt(ctx, "@%s = %sglobal %%struct.%s zeroinitializer\n\n", arch->name, cg_shared(ctx),
		                  arch->name);
	} else {
		buffer_append_fmt(ctx, "@archetype_%s = %sglobal %%struct.%s* null\n\n", arch->name, cg_shared(ctx),
		                  arch->name);
	}
}

static void codegen_static_decl(CodegenContext *ctx, HirStaticDecl *alloc) {
	/* Register the allocation for initialization in main() */
	if (ctx->alloc_count >= ctx->alloc_capacity) {
		ctx->alloc_capacity = (ctx->alloc_capacity == 0) ? 16 : ctx->alloc_capacity * 2;
		ctx->top_level_allocs = realloc(ctx->top_level_allocs, ctx->alloc_capacity * sizeof(ctx->top_level_allocs[0]));
	}
	ctx->top_level_allocs[ctx->alloc_count++] = alloc;
}

/* Generate allocation initialization code (for use in main/init) */
static void codegen_emit_alloc_init(CodegenContext *ctx, HirStaticDecl *alloc) {
	const char *arch_name = alloc->archetype.archetype_name;

	/* Get capacity from first field_value */
	char capacity_buf[256] = "256";
	if (alloc->archetype.field_count > 0) {
		codegen_expression(ctx, alloc->archetype.field_values[0], capacity_buf);
	}

	/* Get init_length from second parameter; default based on whether init block exists */
	char length_buf[256];
	if (alloc->archetype.init_length) {
		codegen_expression(ctx, alloc->archetype.init_length, length_buf);
	} else if (alloc->archetype.field_count > 1) {
		strcpy(length_buf, capacity_buf);
	} else {
		strcpy(length_buf, "0");
	}

	/* Find archetype declaration (canonical shape). Storage symbols use the canonical name,
	 * so a pool declared under any alias initializes the shape's one struct/global. */
	HirArchetypeDecl *arch = find_archetype_decl(ctx, arch_name);
	if (!arch) {
		return;
	}
	arch_name = arch->name;

	int static_cap = get_arch_static_capacity(ctx, arch_name);

	if (static_cap > 0) {
		/* Static global — no malloc. @arch_name is %struct.arch_name* in BSS.
		   Just initialize count and (optionally) field values. */
		int count_idx = arch->field_count;
		int fc_idx = arch->field_count + 2; /* after count + inline free_list array */

		char *count_gep = gen_value_name(ctx);
		buffer_append_fmt(ctx, "  %s = getelementptr %%struct.%s, %%struct.%s* @%s, i32 0, i32 %d\n", count_gep,
		                  arch_name, arch_name, arch_name, count_idx);
		buffer_append_fmt(ctx, "  store i64 %s, i64* %s\n", length_buf, count_gep);

		char *fc_gep = gen_value_name(ctx);
		buffer_append_fmt(ctx, "  %s = getelementptr %%struct.%s, %%struct.%s* @%s, i32 0, i32 %d\n", fc_gep, arch_name,
		                  arch_name, arch_name, fc_idx);
		buffer_append(ctx, "  store i64 1, i64* ");
		buffer_append_fmt(ctx, "%s\n", fc_gep);

		/* Field initialization loops */
		for (int init_idx = 1; init_idx < alloc->archetype.field_count; init_idx++) {
			const char *field_name = alloc->archetype.field_names[init_idx];
			HirExpr *init_value = alloc->archetype.field_values[init_idx];

			int field_idx = -1;
			for (int i = 0; i < arch->field_count; i++) {
				if (strcmp(arch->fields[i]->name, field_name) == 0) {
					field_idx = i;
					break;
				}
			}
			if (field_idx == -1)
				continue;

			HirField *field = arch->fields[field_idx];
			if (field->kind != FIELD_COLUMN)
				continue;

			const char *elem_type = llvm_type_from_arche(field_base_type_name(field->type));

			char loop_cond_label[64], loop_body_label[64], loop_end_label[64];
			char *loop_ctr_alloca = gen_value_name(ctx);
			snprintf(loop_cond_label, sizeof(loop_cond_label), "loop_cond_%d", ctx->value_counter);
			snprintf(loop_body_label, sizeof(loop_body_label), "loop_body_%d", ctx->value_counter);
			snprintf(loop_end_label, sizeof(loop_end_label), "loop_end_%d", ctx->value_counter);
			ctx->value_counter++;

			emit_alloca(ctx, "  %s = alloca i64\n", loop_ctr_alloca);
			buffer_append_fmt(ctx, "  store i64 0, i64* %s\n", loop_ctr_alloca);
			buffer_append_fmt(ctx, "  br label %%%s\n", loop_cond_label);

			buffer_append_fmt(ctx, "%s:\n", loop_cond_label);
			char *loop_counter = gen_value_name(ctx);
			buffer_append_fmt(ctx, "  %s = load i64, i64* %s\n", loop_counter, loop_ctr_alloca);
			char *cond = gen_value_name(ctx);
			buffer_append_fmt(ctx, "  %s = icmp slt i64 %s, %s\n", cond, loop_counter, length_buf);
			buffer_append_fmt(ctx, "  br i1 %s, label %%%s, label %%%s\n", cond, loop_body_label, loop_end_label);

			buffer_append_fmt(ctx, "%s:\n", loop_body_label);

			/* 3-index GEP into inline column array */
			char *elem_ptr = gen_value_name(ctx);
			buffer_append_fmt(ctx, "  %s = getelementptr %%struct.%s, %%struct.%s* @%s, i32 0, i32 %d, i64 %s\n",
			                  elem_ptr, arch_name, arch_name, arch_name, field_idx, loop_counter);

			char init_val_buf[256];
			codegen_expression(ctx, init_value, init_val_buf);

			const char *init_type = hir_resolved_type_name(init_value);
			const char *init_llvm_type = llvm_type_from_arche(init_type);

			if (strcmp(init_llvm_type, elem_type) != 0) {
				char *converted = gen_value_name(ctx);
				if (init_llvm_type[0] == 'i' && elem_type[0] == 'd') {
					buffer_append_fmt(ctx, "  %s = sitofp %s %s to %s\n", converted, init_llvm_type, init_val_buf,
					                  elem_type);
				} else if (init_llvm_type[0] == 'd' && elem_type[0] == 'i') {
					buffer_append_fmt(ctx, "  %s = fptosi %s %s to %s\n", converted, init_llvm_type, init_val_buf,
					                  elem_type);
				} else {
					converted = init_val_buf;
				}
				buffer_append_fmt(ctx, "  store %s %s, %s* %s\n", elem_type, converted, elem_type, elem_ptr);
			} else {
				buffer_append_fmt(ctx, "  store %s %s, %s* %s\n", elem_type, init_val_buf, elem_type, elem_ptr);
			}

			char *next_counter = gen_value_name(ctx);
			buffer_append_fmt(ctx, "  %s = add i64 %s, 1\n", next_counter, loop_counter);
			buffer_append_fmt(ctx, "  store i64 %s, i64* %s\n", next_counter, loop_ctr_alloca);
			buffer_append_fmt(ctx, "  br label %%%s\n", loop_cond_label);

			buffer_append_fmt(ctx, "%s:\n", loop_end_label);
		}

		if (alloc->archetype.field_count > 1) {
			char *final_count_gep = gen_value_name(ctx);
			buffer_append_fmt(ctx, "  %s = getelementptr %%struct.%s, %%struct.%s* @%s, i32 0, i32 %d\n",
			                  final_count_gep, arch_name, arch_name, arch_name, count_idx);
			buffer_append_fmt(ctx, "  store i64 %s, i64* %s\n", length_buf, final_count_gep);
		}
		return;
	}

	/* Dynamic path: single malloc + layout setup */

	/* Struct layout: [pointers...][count][capacity][free_list*][free_count] */
	int struct_sz_bytes = (arch->field_count + 4) * 8;

	int bytes_per_row = 0;
	for (int i = 0; i < arch->field_count; i++) {
		if (arch->fields[i]->kind == FIELD_COLUMN) {
			const char *elem_type = llvm_type_from_arche(field_base_type_name(arch->fields[i]->type));
			int n = field_total_elements(arch->fields[i]->type);
			bytes_per_row += llvm_type_sizeof(elem_type) * n;
		}
	}
	int total_bytes_per_row = bytes_per_row + 8;

	char *total_bytes = gen_value_name(ctx);
	char *data_bytes = gen_value_name(ctx);
	buffer_append_fmt(ctx, "  %s = mul i64 %s, %d\n", data_bytes, capacity_buf, total_bytes_per_row);
	buffer_append_fmt(ctx, "  %s = add i64 %d, %s\n", total_bytes, struct_sz_bytes, data_bytes);

	char *raw_ptr = gen_value_name(ctx);
	buffer_append_fmt(ctx, "  %s = call i8* @malloc(i64 %s)\n", raw_ptr, total_bytes);

	char *struct_ptr = gen_value_name(ctx);
	buffer_append_fmt(ctx, "  %s = bitcast i8* %s to %%struct.%s*\n", struct_ptr, raw_ptr, arch_name);

	buffer_append_fmt(ctx, "  store %%struct.%s* %s, %%struct.%s** @archetype_%s\n", arch_name, struct_ptr, arch_name,
	                  arch_name);

	char *count_gep = gen_value_name(ctx);
	buffer_append_fmt(ctx, "  %s = getelementptr %%struct.%s, %%struct.%s* %s, i32 0, i32 %d\n", count_gep, arch_name,
	                  arch_name, struct_ptr, arch->field_count);
	buffer_append_fmt(ctx, "  store i64 %s, i64* %s\n", length_buf, count_gep);

	char *cap_gep = gen_value_name(ctx);
	buffer_append_fmt(ctx, "  %s = getelementptr %%struct.%s, %%struct.%s* %s, i32 0, i32 %d\n", cap_gep, arch_name,
	                  arch_name, struct_ptr, arch->field_count + 1);
	buffer_append_fmt(ctx, "  store i64 %s, i64* %s\n", capacity_buf, cap_gep);

	char *fc_gep = gen_value_name(ctx);
	buffer_append_fmt(ctx, "  %s = getelementptr %%struct.%s, %%struct.%s* %s, i32 0, i32 %d\n", fc_gep, arch_name,
	                  arch_name, struct_ptr, arch->field_count + 3);
	buffer_append_fmt(ctx, "  store i64 0, i64* %s\n", fc_gep);

	int col_offset = 0;
	for (int i = 0; i < arch->field_count; i++) {
		if (arch->fields[i]->kind == FIELD_COLUMN) {
			const char *elem_type = llvm_type_from_arche(field_base_type_name(arch->fields[i]->type));

			char *col_gep = gen_value_name(ctx);
			buffer_append_fmt(ctx, "  %s = getelementptr %%struct.%s, %%struct.%s* %s, i32 0, i32 %d\n", col_gep,
			                  arch_name, arch_name, struct_ptr, i);

			char *col_offset_llvm = gen_value_name(ctx);
			buffer_append_fmt(ctx, "  %s = mul i64 %s, %d\n", col_offset_llvm, capacity_buf, col_offset);

			char *col_data_base = gen_value_name(ctx);
			buffer_append_fmt(ctx, "  %s = getelementptr i8, i8* %s, i64 %d\n", col_data_base, raw_ptr,
			                  struct_sz_bytes);

			char *col_data = gen_value_name(ctx);
			buffer_append_fmt(ctx, "  %s = getelementptr i8, i8* %s, i64 %s\n", col_data, col_data_base,
			                  col_offset_llvm);

			char *col_ptr = gen_value_name(ctx);
			buffer_append_fmt(ctx, "  %s = bitcast i8* %s to %s*\n", col_ptr, col_data, elem_type);

			buffer_append_fmt(ctx, "  store %s* %s, %s** %s\n", elem_type, col_ptr, elem_type, col_gep);

			int n = field_total_elements(arch->fields[i]->type);
			int elem_size = llvm_type_sizeof(elem_type) * n;
			col_offset += elem_size;
		}
	}

	char *fl_gep = gen_value_name(ctx);
	buffer_append_fmt(ctx, "  %s = getelementptr %%struct.%s, %%struct.%s* %s, i32 0, i32 %d\n", fl_gep, arch_name,
	                  arch_name, struct_ptr, arch->field_count + 2);

	char *fl_offset = gen_value_name(ctx);
	buffer_append_fmt(ctx, "  %s = mul i64 %s, %d\n", fl_offset, capacity_buf, bytes_per_row);
	char *fl_data = gen_value_name(ctx);
	buffer_append_fmt(ctx, "  %s = getelementptr i8, i8* %s, i64 %s\n", fl_data, raw_ptr, fl_offset);
	char *fl_add_header = gen_value_name(ctx);
	buffer_append_fmt(ctx, "  %s = getelementptr i8, i8* %s, i64 %d\n", fl_add_header, fl_data, struct_sz_bytes);

	char *fl_ptr = gen_value_name(ctx);
	buffer_append_fmt(ctx, "  %s = bitcast i8* %s to i64*\n", fl_ptr, fl_add_header);

	buffer_append_fmt(ctx, "  store i64* %s, i64** %s\n", fl_ptr, fl_gep);

	for (int init_idx = 1; init_idx < alloc->archetype.field_count; init_idx++) {
		const char *field_name = alloc->archetype.field_names[init_idx];
		HirExpr *init_value = alloc->archetype.field_values[init_idx];

		int field_idx = -1;
		for (int i = 0; i < arch->field_count; i++) {
			if (strcmp(arch->fields[i]->name, field_name) == 0) {
				field_idx = i;
				break;
			}
		}
		if (field_idx == -1)
			continue;

		HirField *field = arch->fields[field_idx];
		if (field->kind != FIELD_COLUMN)
			continue;

		const char *elem_type = llvm_type_from_arche(field_base_type_name(field->type));

		char *field_col_ptr_ref = gen_value_name(ctx);
		buffer_append_fmt(ctx, "  %s = getelementptr %%struct.%s, %%struct.%s* %s, i32 0, i32 %d\n", field_col_ptr_ref,
		                  arch_name, arch_name, struct_ptr, field_idx);
		char *field_col_ptr = gen_value_name(ctx);
		buffer_append_fmt(ctx, "  %s = load %s*, %s** %s\n", field_col_ptr, elem_type, elem_type, field_col_ptr_ref);

		char loop_cond_label[64], loop_body_label[64], loop_end_label[64];
		char *loop_ctr_alloca = gen_value_name(ctx);
		snprintf(loop_cond_label, sizeof(loop_cond_label), "loop_cond_%d", ctx->value_counter);
		snprintf(loop_body_label, sizeof(loop_body_label), "loop_body_%d", ctx->value_counter);
		snprintf(loop_end_label, sizeof(loop_end_label), "loop_end_%d", ctx->value_counter);
		ctx->value_counter++;

		emit_alloca(ctx, "  %s = alloca i64\n", loop_ctr_alloca);
		buffer_append_fmt(ctx, "  store i64 0, i64* %s\n", loop_ctr_alloca);
		buffer_append_fmt(ctx, "  br label %%%s\n", loop_cond_label);

		buffer_append_fmt(ctx, "%s:\n", loop_cond_label);
		char *loop_counter = gen_value_name(ctx);
		buffer_append_fmt(ctx, "  %s = load i64, i64* %s\n", loop_counter, loop_ctr_alloca);
		char *cond = gen_value_name(ctx);
		buffer_append_fmt(ctx, "  %s = icmp slt i64 %s, %s\n", cond, loop_counter, length_buf);
		buffer_append_fmt(ctx, "  br i1 %s, label %%%s, label %%%s\n", cond, loop_body_label, loop_end_label);

		buffer_append_fmt(ctx, "%s:\n", loop_body_label);

		char *elem_ptr = gen_value_name(ctx);
		buffer_append_fmt(ctx, "  %s = getelementptr %s, %s* %s, i64 %s\n", elem_ptr, elem_type, elem_type,
		                  field_col_ptr, loop_counter);

		char init_val_buf[256];
		codegen_expression(ctx, init_value, init_val_buf);

		const char *init_type = hir_resolved_type_name(init_value);
		const char *init_llvm_type = llvm_type_from_arche(init_type);

		if (strcmp(init_llvm_type, elem_type) != 0) {
			char *converted = gen_value_name(ctx);
			if (init_llvm_type[0] == 'i' && elem_type[0] == 'd') {
				buffer_append_fmt(ctx, "  %s = sitofp %s %s to %s\n", converted, init_llvm_type, init_val_buf,
				                  elem_type);
			} else if (init_llvm_type[0] == 'd' && elem_type[0] == 'i') {
				buffer_append_fmt(ctx, "  %s = fptosi %s %s to %s\n", converted, init_llvm_type, init_val_buf,
				                  elem_type);
			} else {
				converted = init_val_buf;
			}
			buffer_append_fmt(ctx, "  store %s %s, %s* %s\n", elem_type, converted, elem_type, elem_ptr);
		} else {
			buffer_append_fmt(ctx, "  store %s %s, %s* %s\n", elem_type, init_val_buf, elem_type, elem_ptr);
		}

		char *next_counter = gen_value_name(ctx);
		buffer_append_fmt(ctx, "  %s = add i64 %s, 1\n", next_counter, loop_counter);
		buffer_append_fmt(ctx, "  store i64 %s, i64* %s\n", next_counter, loop_ctr_alloca);
		buffer_append_fmt(ctx, "  br label %%%s\n", loop_cond_label);

		buffer_append_fmt(ctx, "%s:\n", loop_end_label);
	}

	if (alloc->archetype.field_count > 1) {
		char *final_count_gep = gen_value_name(ctx);
		buffer_append_fmt(ctx, "  %s = getelementptr %%struct.%s, %%struct.%s* %s, i32 0, i32 %d\n", final_count_gep,
		                  arch_name, arch_name, struct_ptr, arch->field_count);
		buffer_append_fmt(ctx, "  store i64 %s, i64* %s\n", length_buf, final_count_gep);
	}
}

static FunctionBodyState begin_function_body(CodegenContext *ctx) {
	FunctionBodyState state = {
	    .saved_output_buffer = ctx->output_buffer,
	    .saved_buffer_size = ctx->buffer_size,
	    .saved_buffer_pos = ctx->buffer_pos,
	};
	ctx->output_buffer = malloc(4096);
	ctx->buffer_size = 4096;
	ctx->buffer_pos = 0;
	ctx->output_buffer[0] = '\0';

	if (!ctx->alloca_buffer) {
		ctx->alloca_buf_size = 1024;
		ctx->alloca_buffer = malloc(ctx->alloca_buf_size);
	}
	ctx->alloca_buf_pos = 0;
	ctx->alloca_buffer[0] = '\0';
	ctx->hoisting_allocas = 1;
	return state;
}

static void end_function_body(CodegenContext *ctx, FunctionBodyState state) {
	ctx->hoisting_allocas = 0;
	char *body_buf = ctx->output_buffer;

	ctx->output_buffer = state.saved_output_buffer;
	ctx->buffer_size = state.saved_buffer_size;
	ctx->buffer_pos = state.saved_buffer_pos;

	if (ctx->alloca_buf_pos > 0)
		buffer_append(ctx, ctx->alloca_buffer);
	buffer_append(ctx, body_buf);
	free(body_buf);
}

/* Emit a func's LLVM param list (the text inside the parens), shared by the `define` and the per-unit
 * cross-unit `declare`. Slices lower to a `(ptr, len)` pair; sized/extern arrays to an element ptr. */
static void emit_func_params(CodegenContext *ctx, HirFuncDecl *func) {
	for (int i = 0; i < func->param_count; i++) {
		HirType *param_type = func->params[i]->type;
		const char *type_name = field_base_type_name(param_type);
		const char *llvm_type = llvm_type_from_arche(type_name);
		if (param_type && param_type->tag == HIR_TYPE_ARRAY && !func->is_extern)
			buffer_append_fmt(ctx, "%s* %%arg%d.ptr, i64 %%arg%d.len", llvm_type, i, i);
		else if (param_type && param_type->tag == HIR_TYPE_ARRAY)
			buffer_append_fmt(ctx, "%s* %%arg%d", llvm_type, i);
		else if (param_type && param_type->tag == HIR_TYPE_SHAPED_ARRAY)
			buffer_append_fmt(ctx, "%s* %%arg%d", llvm_type, i);
		else
			buffer_append_fmt(ctx, "%s %%arg%d", llvm_type, i);
		if (i < func->param_count - 1)
			buffer_append(ctx, ", ");
	}
}

static void codegen_func_decl(CodegenContext *ctx, HirFuncDecl *func) {
	/* For extern funcs, emit declare stub */
	if (func->is_extern) {
		/* Return type from the declaration: a bare extern with no `->` is void; char[] returns a
		 * raw i8* byte view (e.g. arche_file_map for whole-file mmap). */
		char ret_buf[512];
		if (func->return_type_count == 0)
			snprintf(ret_buf, sizeof(ret_buf), "void");
		else
			func_llvm_return_type(func, ret_buf, sizeof(ret_buf));
		buffer_append_fmt(ctx, "declare %s @%s(", ret_buf, func->name);
		for (int i = 0; i < func->param_count; i++) {
			HirType *param_type = func->params[i]->type;
			const char *type_name = field_base_type_name(param_type);
			const char *base_type = llvm_type_from_arche(type_name);

			/* Check if type is char[] (i8*) or an archetype (struct*). */
			if (param_type && param_type->tag == HIR_TYPE_ARRAY) {
				buffer_append_fmt(ctx, "i8*"); /* C ABI: T[] = raw ptr */
			} else if (find_archetype_decl(ctx, type_name)) {
				buffer_append_fmt(ctx, "%%struct.%s*", type_name);
			} else {
				buffer_append(ctx, base_type);
			}

			if (i < func->param_count - 1) {
				buffer_append(ctx, ", ");
			}
		}
		/* Arche has no variadic syntax; keep the name-based special-case for the C variadics we use. */
		if (strcmp(func->name, "printf") == 0 || strcmp(func->name, "sprintf") == 0)
			buffer_append(ctx, ", ...");
		buffer_append(ctx, ")\n");
		return;
	}

	/* Generate function definition */
	func_llvm_return_type(func, ctx->current_return_type_buf, sizeof(ctx->current_return_type_buf));
	const char *return_type = ctx->current_return_type_buf;
	ctx->current_return_type = return_type;
	ctx->current_func = func;
	ctx->current_return_types = func->return_types;
	ctx->current_return_type_count = func->return_type_count;

	/* `internal` linkage: an Arche program is whole-program-compiled to one LLVM module, so its
	 * funcs/procs need no external symbols. Keeping them out of the global namespace prevents a
	 * user/stdlib name (e.g. the `open`/`read`/`write` syscall wrappers) from overriding the
	 * identically-named libc symbol that the C runtime (io.c's arche_file_map, etc.) links against. */
	char func_sym_buf[512];
	buffer_append_fmt(ctx, "define %s%s @%s(", cg_linkage(ctx), return_type,
	                  cg_fnsym(ctx, func->name, func->is_extern, func_sym_buf, sizeof(func_sym_buf)));
	emit_func_params(ctx, func);
	buffer_append(ctx, ") {\n");
	buffer_append(ctx, "entry:\n");

	FunctionBodyState fbs_func = begin_function_body(ctx);
	push_value_scope(ctx);

	/* Register static arrays in scope */
	register_static_arrays_in_scope(ctx);

	/* Add parameters to scope */
	for (int i = 0; i < func->param_count; i++) {
		char param_name[32];
		snprintf(param_name, sizeof(param_name), "%%arg%d", i);

		HirType *ptype = func->params[i]->type;
		const char *en = field_base_type_name(ptype); /* element base: "char","int","float",… */
		const char *elt = llvm_type_from_arche(en);   /* "i8","i32","double",… */
		if (ptype && ptype->tag == HIR_TYPE_ARRAY && !func->is_extern) {
			/* `T[]` slice (any element, incl. char): %argN.ptr is the element pointer, %argN.len the
			 * runtime length. char is just i8 here — a normal slice, no arche_array. */
			ValueScope *scope = &ctx->scopes[ctx->scope_count - 1];
			ValueInfo *vi = calloc(1, sizeof(ValueInfo));
			vi->name = malloc(strlen(func->params[i]->name) + 1);
			strcpy(vi->name, func->params[i]->name);
			char ptr_name[40], len_name[40];
			snprintf(ptr_name, sizeof(ptr_name), "%%arg%d.ptr", i);
			snprintf(len_name, sizeof(len_name), "%%arg%d.len", i);
			vi->llvm_name = malloc(strlen(ptr_name) + 1);
			strcpy(vi->llvm_name, ptr_name);
			vi->type = 6;
			vi->arch_name = NULL;
			vi->string_len = -1;
			vi->field_type = en;
			vi->bit_width = strcmp(elt, "double") == 0 ? 64 : (strcmp(elt, "i8") == 0 ? 8 : 32);
			vi->is_slice = 1;
			vi->len_ssa = malloc(strlen(len_name) + 1);
			strcpy(vi->len_ssa, len_name);
			scope->values = realloc(scope->values, (scope->value_count + 1) * sizeof(ValueInfo *));
			scope->values[scope->value_count++] = vi;
		} else if (ptype && ptype->tag == HIR_TYPE_ARRAY && !func->is_extern) {
			/* Non-extern unbounded char[]: extract data pointer from the arche_array struct once. */
			ValueScope *scope = &ctx->scopes[ctx->scope_count - 1];
			char *dp_gep = gen_value_name(ctx);
			buffer_append_fmt(ctx,
			                  "  %s = getelementptr %%struct.arche_array, %%struct.arche_array* %s, i32 0, i32 0\n",
			                  dp_gep, param_name);
			char *dp = gen_value_name(ctx);
			buffer_append_fmt(ctx, "  %s = load i8*, i8** %s\n", dp, dp_gep);
			ValueInfo *vi = calloc(1, sizeof(ValueInfo));
			vi->name = malloc(strlen(func->params[i]->name) + 1);
			strcpy(vi->name, func->params[i]->name);
			vi->llvm_name = malloc(strlen(dp) + 1);
			strcpy(vi->llvm_name, dp);
			vi->type = 6;
			vi->arch_name = NULL;
			vi->string_len = -1;
			vi->field_type = "char";
			vi->bit_width = 8;
			scope->values = realloc(scope->values, (scope->value_count + 1) * sizeof(ValueInfo *));
			scope->values[scope->value_count++] = vi;
		} else if (ptype && (ptype->tag == HIR_TYPE_SHAPED_ARRAY || ptype->tag == HIR_TYPE_ARRAY)) {
			/* Sized array (any element) or extern char[]: %argN is already the element pointer —
			 * bind as type-6 so it indexes; field_type drives element typing. */
			ValueScope *scope = &ctx->scopes[ctx->scope_count - 1];
			ValueInfo *vi = calloc(1, sizeof(ValueInfo));
			vi->name = malloc(strlen(func->params[i]->name) + 1);
			strcpy(vi->name, func->params[i]->name);
			vi->llvm_name = malloc(strlen(param_name) + 1);
			strcpy(vi->llvm_name, param_name);
			vi->type = 6;
			vi->arch_name = NULL;
			vi->string_len = -1;
			vi->field_type = en;
			vi->bit_width = strcmp(elt, "double") == 0 ? 64 : (strcmp(elt, "i8") == 0 ? 8 : 32);
			scope->values = realloc(scope->values, (scope->value_count + 1) * sizeof(ValueInfo *));
			scope->values[scope->value_count++] = vi;
		} else {
			add_value(ctx, func->params[i]->name, param_name, 0);
		}
	}

	/* Generate function body */
	for (int i = 0; i < func->stmt_count; i++) {
		codegen_statement(ctx, func->stmts[i]);
	}

	pop_value_scope(ctx);

	/* Fallback return (reached only if the body has no terminating return). */
	const char *ret_value = "0";
	if (func->return_type_count > 1 || strchr(return_type, '{')) {
		ret_value = "zeroinitializer"; /* aggregate return type (multi-value or a `T[]` slice fat pointer) */
	} else if (strcmp(return_type, "double") == 0) {
		ret_value = "0.0";
	} else if (strchr(return_type, '*')) {
		ret_value = "null"; /* pointer return (e.g. char[] -> i8*) */
	}
	buffer_append_fmt(ctx, "  ret %s %s\n", return_type, ret_value);
	end_function_body(ctx, fbs_func);
	buffer_append(ctx, "}\n\n");
}

/* An out-param is in-out when its name is also an in-param: the in-list pointer IS the out slot
 * (in-place mutation), so it is not emitted as a separate out-pointer arg, and an extern's such
 * param is just its C pointer argument. An out-param NOT echoed in the in-list is out-only. */
static int proc_out_param_is_inout(HirProcDecl *proc, int oi) {
	const char *on = proc->out_params[oi]->name;
	if (!on)
		return 0;
	for (int ii = 0; ii < proc->param_count; ii++)
		if (proc->params[ii]->name && strcmp(proc->params[ii]->name, on) == 0)
			return 1;
	return 0;
}

/* True when IN-param `ii` is in-out: its name also appears in the out-list. (Companion to
 * proc_out_param_is_inout, which indexes the OUT-list.) Used to validate/resolve `_` placeholders. */
static int proc_out_param_is_inout_in(HirProcDecl *proc, int ii) {
	if (ii < 0 || ii >= proc->param_count)
		return 0;
	const char *in = proc->params[ii]->name;
	if (!in)
		return 0;
	for (int oi = 0; oi < proc->out_param_count; oi++)
		if (proc->out_params[oi]->name && strcmp(proc->out_params[oi]->name, in) == 0)
			return 1;
	return 0;
}

/* The C return type of an extern proc = its single out-only out-param's LLVM type (an out-param
 * not echoed in the in-list). `void` when there is none. */
static const char *extern_proc_cret(HirProcDecl *proc) {
	for (int oi = 0; oi < proc->out_param_count; oi++)
		if (!proc_out_param_is_inout(proc, oi))
			return extern_member_llvm(proc->out_params[oi]->type);
	return "void";
}

/* Emit a non-extern proc's LLVM param list (in-params + out-only-param pointers), shared by `define`
 * and the per-unit cross-unit `declare`. */
static void emit_proc_params(CodegenContext *ctx, HirProcDecl *proc) {
	for (int i = 0; i < proc->param_count; i++) {
		HirType *param_type = proc->params[i]->type;
		const char *type_name = field_base_type_name(param_type);
		const char *base_type = llvm_type_from_arche(type_name);
		if (param_type && param_type->tag == HIR_TYPE_ARRAY)
			buffer_append_fmt(ctx, "%s* %%arg%d.ptr, i64 %%arg%d.len", base_type, i, i);
		else if (param_type && param_type->tag == HIR_TYPE_SHAPED_ARRAY)
			buffer_append_fmt(ctx, "%s* %%arg%d", base_type, i);
		else if (find_archetype_decl(ctx, type_name))
			buffer_append_fmt(ctx, "%%struct.%s* %%arg%d", type_name, i);
		else
			buffer_append_fmt(ctx, "%s %%arg%d", base_type, i);
		if (i < proc->param_count - 1)
			buffer_append(ctx, ", ");
	}
	int emitted_params = proc->param_count;
	for (int oi = 0; oi < proc->out_param_count; oi++) {
		if (proc_out_param_is_inout(proc, oi))
			continue;
		HirType *ot = proc->out_params[oi]->type;
		const char *otn = field_base_type_name(ot);
		if (emitted_params > 0)
			buffer_append(ctx, ", ");
		if (ot && ot->tag == HIR_TYPE_SHAPED_ARRAY)
			buffer_append_fmt(ctx, "%s* %%out%d", llvm_type_from_arche(otn), oi);
		else if (ot && find_archetype_decl(ctx, otn))
			buffer_append_fmt(ctx, "%%struct.%s* %%out%d", otn, oi);
		else
			buffer_append_fmt(ctx, "%s* %%out%d", return_member_llvm(ot), oi);
		emitted_params++;
	}
}

static void codegen_proc_decl(CodegenContext *ctx, HirProcDecl *proc) {
	/* For extern procs, emit declare stub */
	if (proc->is_extern) {
		/* An extern proc's out-only out-param (a name NOT in the in-list) maps to the C return
		 * value; in-out names are in-place pointer writes already passed in the in-list. At most
		 * one out-only param (C returns one value); none ⇒ void. */
		const char *cret = "void";
		for (int oi = 0; oi < proc->out_param_count; oi++) {
			if (!proc_out_param_is_inout(proc, oi)) {
				cret = extern_member_llvm(proc->out_params[oi]->type);
				break;
			}
		}
		buffer_append_fmt(ctx, "declare %s @%s(", cret, proc->name);
		for (int i = 0; i < proc->param_count; i++) {
			HirType *param_type = proc->params[i]->type;
			const char *type_name = field_base_type_name(param_type);
			const char *base_type = llvm_type_from_arche(type_name);

			/* Check if type is char[] (i8*) or an archetype (struct*). */
			if (param_type && param_type->tag == HIR_TYPE_ARRAY) {
				buffer_append_fmt(ctx, "i8*"); /* C ABI: T[] = raw ptr */
			} else if (find_archetype_decl(ctx, type_name)) {
				buffer_append_fmt(ctx, "%%struct.%s*", type_name);
			} else {
				buffer_append(ctx, base_type);
			}

			if (i < proc->param_count - 1) {
				buffer_append(ctx, ", ");
			}
		}
		/* Add variadic marker for known variadic functions */
		if (strcmp(proc->name, "sprintf") == 0 || strcmp(proc->name, "printf") == 0) {
			buffer_append(ctx, ", ...");
		}
		buffer_append(ctx, ")\n");
		return;
	}

	/* Generate procedure - rename user main to main_user to allow our wrapper */
	int is_user_main = (strcmp(proc->name, "main") == 0);
	const char *proc_name = is_user_main ? "main_user" : proc->name;
	/* A proc is not a value — it returns void. Its results are written through caller-provided
	 * out-param pointers, appended to the signature after the in-params. `main` is void too; the
	 * wrapper drives process exit. */
	snprintf(ctx->current_return_type_buf, sizeof(ctx->current_return_type_buf), "void");
	ctx->current_return_types = NULL;
	ctx->current_return_type_count = 0;
	ctx->current_return_type = ctx->current_return_type_buf;
	ctx->current_func = NULL;
	/* `internal` linkage (see func emission): keeps `main_user` and the syscall-wrapper procs
	 * (`open`/`read`/`write`/…) out of the link-time global namespace, so they don't shadow the
	 * libc symbols the C runtime depends on. The real `@main` entry wrapper stays external. */
	char proc_sym_buf[512];
	buffer_append_fmt(ctx, "define %svoid @%s(", cg_linkage(ctx),
	                  cg_fnsym(ctx, proc_name, 0, proc_sym_buf, sizeof(proc_sym_buf)));

	/* Emit parameter types and names */
	emit_proc_params(ctx, proc);

	buffer_append(ctx, ") {\n");
	buffer_append(ctx, "entry:\n");

	FunctionBodyState fbs_proc = begin_function_body(ctx);
	push_value_scope(ctx);
	ctx->block_terminated = 0;

	/* Register static arrays in scope */
	register_static_arrays_in_scope(ctx);

	/* Register parameters in scope */
	for (int i = 0; i < proc->param_count; i++) {
		char param_llvm[32];
		snprintf(param_llvm, sizeof(param_llvm), "%%arg%d", i);
		HirType *param_type = proc->params[i]->type;
		const char *type_name = field_base_type_name(param_type);

		/* If param type is an archetype, track it as arch pointer (type 3) */
		if (find_archetype_decl(ctx, type_name)) {
			add_arch_value(ctx, proc->params[i]->name, param_llvm, type_name);
		} else if (param_type && param_type->tag == HIR_TYPE_ARRAY) {
			/* `T[]` slice param (any element, incl. char): %argN.ptr is the element pointer,
			 * %argN.len the runtime length — a normal fat pointer, no arche_array. */
			const char *en = type_name; /* element base: "char","int",… */
			const char *lt = llvm_type_from_arche(en);
			ValueInfo *vi = calloc(1, sizeof(ValueInfo));
			vi->name = malloc(strlen(proc->params[i]->name) + 1);
			strcpy(vi->name, proc->params[i]->name);
			char ptr_name[40], len_name[40];
			snprintf(ptr_name, sizeof(ptr_name), "%%arg%d.ptr", i);
			snprintf(len_name, sizeof(len_name), "%%arg%d.len", i);
			vi->llvm_name = malloc(strlen(ptr_name) + 1);
			strcpy(vi->llvm_name, ptr_name);
			vi->type = 6;
			vi->arch_name = NULL;
			vi->string_len = -1;
			vi->field_type = en;
			vi->bit_width = strcmp(lt, "double") == 0 ? 64 : (strcmp(lt, "i8") == 0 ? 8 : 32);
			vi->is_slice = 1;
			vi->len_ssa = malloc(strlen(len_name) + 1);
			strcpy(vi->len_ssa, len_name);
			if (ctx->scope_count > 0) {
				ValueScope *scope = &ctx->scopes[ctx->scope_count - 1];
				scope->values = realloc(scope->values, (scope->value_count + 1) * sizeof(ValueInfo *));
				scope->values[scope->value_count++] = vi;
			}
		} else if (param_type && param_type->tag == HIR_TYPE_SHAPED_ARRAY) {
			/* sized array param: %argN is already the element pointer (char[N] → i8*, int[N] →
			 * i32*) — bind as type-6 so it indexes (field_type drives element typing). */
			const char *en = type_name; /* element base name: "char", "int", "float", … */
			const char *lt = llvm_type_from_arche(en);
			ValueInfo *vi = calloc(1, sizeof(ValueInfo));
			vi->name = malloc(strlen(proc->params[i]->name) + 1);
			strcpy(vi->name, proc->params[i]->name);
			vi->llvm_name = malloc(strlen(param_llvm) + 1);
			strcpy(vi->llvm_name, param_llvm);
			vi->type = 6;
			vi->arch_name = NULL;
			vi->string_len = -1;
			vi->field_type = en;
			vi->handle_archetype = NULL;
			vi->bit_width = strcmp(lt, "double") == 0 ? 64 : (strcmp(lt, "i8") == 0 ? 8 : 32);
			if (ctx->scope_count > 0) {
				ValueScope *scope = &ctx->scopes[ctx->scope_count - 1];
				scope->values = realloc(scope->values, (scope->value_count + 1) * sizeof(ValueInfo *));
				scope->values[scope->value_count++] = vi;
			}
		} else {
			/* Default to i32 for Int, Float, etc. */
			add_value(ctx, proc->params[i]->name, param_llvm, 0);
		}
	}

	/* Register out-only out-params so writes in the body store through the caller's pointer.
	 * In-out out-params are already bound by the in-param loop (same pointer). */
	for (int oi = 0; oi < proc->out_param_count; oi++) {
		if (proc_out_param_is_inout(proc, oi))
			continue;
		char out_llvm[32];
		snprintf(out_llvm, sizeof(out_llvm), "%%out%d", oi);
		HirType *ot = proc->out_params[oi]->type;
		const char *otn = field_base_type_name(ot);
		if (ot && find_archetype_decl(ctx, otn)) {
			add_arch_value(ctx, proc->out_params[oi]->name, out_llvm, otn);
		} else if (ot && ot->tag == HIR_TYPE_ARRAY) {
			/* unbounded `T[]` out-param: the caller allocated a `{T*,i64}` slice slot at %outN. Load it
			 * and bind a type-6 slice so the body can READ it (out-params are read-write — the caller
			 * zero-inits the slot). `out_aggr_ptr` records %outN so assigning a fresh slice (`out = buf[0:r]`)
			 * stores the {ptr,len} back through it; the caller then recovers the returned view. */
			const char *en = field_base_type_name(ot);
			const char *lt = llvm_type_from_arche(en);
			char agg[24];
			snprintf(agg, sizeof(agg), "{ %s*, i64 }", lt);
			char *pgep = gen_value_name(ctx);
			buffer_append_fmt(ctx, "  %s = getelementptr %s, %s* %s, i32 0, i32 0\n", pgep, agg, agg, out_llvm);
			char *p = gen_value_name(ctx);
			buffer_append_fmt(ctx, "  %s = load %s*, %s** %s\n", p, lt, lt, pgep);
			char *lgep = gen_value_name(ctx);
			buffer_append_fmt(ctx, "  %s = getelementptr %s, %s* %s, i32 0, i32 1\n", lgep, agg, agg, out_llvm);
			char *l = gen_value_name(ctx);
			buffer_append_fmt(ctx, "  %s = load i64, i64* %s\n", l, lgep);
			ValueInfo *vi = calloc(1, sizeof(ValueInfo));
			vi->name = malloc(strlen(proc->out_params[oi]->name) + 1);
			strcpy(vi->name, proc->out_params[oi]->name);
			vi->llvm_name = malloc(strlen(p) + 1);
			strcpy(vi->llvm_name, p);
			vi->type = 6;
			vi->string_len = -1;
			vi->field_type = en;
			vi->bit_width = strcmp(lt, "double") == 0 ? 64 : (strcmp(lt, "i8") == 0 ? 8 : 32);
			vi->is_slice = 1;
			vi->len_ssa = malloc(strlen(l) + 1);
			strcpy(vi->len_ssa, l);
			vi->out_aggr_ptr = strdup(out_llvm);
			if (ctx->scope_count > 0) {
				ValueScope *sc = &ctx->scopes[ctx->scope_count - 1];
				sc->values = realloc(sc->values, (sc->value_count + 1) * sizeof(ValueInfo *));
				sc->values[sc->value_count++] = vi;
			}
		} else if (ot && ot->tag == HIR_TYPE_SHAPED_ARRAY) {
			/* sized array out-param: %outN is the element pointer the caller allocated — bind as
			 * type-6 so `buf[i] = …` in the body stores through it (field_type drives elem typing). */
			const char *lt = llvm_type_from_arche(otn);
			ValueInfo *vi = calloc(1, sizeof(ValueInfo));
			vi->name = malloc(strlen(proc->out_params[oi]->name) + 1);
			strcpy(vi->name, proc->out_params[oi]->name);
			vi->llvm_name = malloc(strlen(out_llvm) + 1);
			strcpy(vi->llvm_name, out_llvm);
			vi->type = 6;
			vi->string_len = ot->rank;
			vi->field_type = otn;
			vi->bit_width = strcmp(lt, "double") == 0 ? 64 : (strcmp(lt, "i8") == 0 ? 8 : 32);
			if (ctx->scope_count > 0) {
				ValueScope *scope = &ctx->scopes[ctx->scope_count - 1];
				scope->values = realloc(scope->values, (scope->value_count + 1) * sizeof(ValueInfo *));
				scope->values[scope->value_count++] = vi;
			}
		} else {
			/* scalar out: pointer-backed (type 1) so `=` / `:=` in the body store through it */
			ValueInfo *vi = calloc(1, sizeof(ValueInfo));
			vi->name = malloc(strlen(proc->out_params[oi]->name) + 1);
			strcpy(vi->name, proc->out_params[oi]->name);
			vi->llvm_name = malloc(strlen(out_llvm) + 1);
			strcpy(vi->llvm_name, out_llvm);
			vi->type = 1;
			vi->string_len = -1;
			vi->field_type = otn;
			vi->bit_width = (ot && ot->tag == HIR_TYPE_FLOAT) ? 64 : 32;
			if (ctx->scope_count > 0) {
				ValueScope *scope = &ctx->scopes[ctx->scope_count - 1];
				scope->values = realloc(scope->values, (scope->value_count + 1) * sizeof(ValueInfo *));
				scope->values[scope->value_count++] = vi;
			}
		}
	}

	for (int i = 0; i < proc->stmt_count; i++) {
		codegen_statement(ctx, proc->stmts[i]);
	}

	pop_value_scope(ctx);

	/* A proc returns void — its results were written through the out-param pointers. */
	buffer_append(ctx, "  ret void\n");
	end_function_body(ctx, fbs_proc);
	buffer_append(ctx, "}\n\n");
}

static void codegen_sys_decl(CodegenContext *ctx, HirSysDecl *sys) {
	/* Per-unit: a system is emitted exactly once, in the ENTRY unit (0) — regardless of which unit it
	 * was declared in (a device system lives in the device's unit but is `run` only from the entry).
	 * Emitting it in unit 0 (where the archetype types/pools are also present via the hoist + linkonce_odr
	 * globals) gives a single external definition that the entry's `run` dispatch resolves — no
	 * linkonce_odr duplication, so nothing to keep byte-identical. Skip in any non-entry per-unit module. */
	if (ctx->per_unit && ctx->emit_only_unit >= 1)
		return;

	/* Collect all archetypes that have the required fields */
	const char *matching_archs[256];
	int matching_count = collect_sys_matching_archs(ctx, sys, matching_archs, 256);

	if (matching_count == 0) {
		return;
	}

	/* Generate ONE function taking all matching archetypes as params. Emitted once in the entry unit
	 * (the per-unit filter keeps it to unit 0) with normal linkage — internal whole-program, external
	 * under per-unit — so there is no `linkonce_odr` duplication to keep byte-identical. */
	char sys_sym_buf[512];
	buffer_append_fmt(ctx, "define %svoid @%s(", cg_linkage(ctx),
	                  cg_fnsym(ctx, sys->name, 0, sys_sym_buf, sizeof(sys_sym_buf)));
	for (int i = 0; i < matching_count; i++) {
		if (i > 0)
			buffer_append(ctx, ", ");
		buffer_append_fmt(ctx, "%%struct.%s* %%arch_%s", matching_archs[i], matching_archs[i]);
	}
	buffer_append(ctx, ") #0 {\nentry:\n");

	FunctionBodyState fbs = begin_function_body(ctx);

	/* For each matching archetype: bind columns and emit body inline */
	for (int ai = 0; ai < matching_count; ai++) {
		const char *arch_name = matching_archs[ai];
		char arch_param[256];
		snprintf(arch_param, sizeof(arch_param), "%%arch_%s", arch_name);

		push_value_scope(ctx);

		/* Bind field parameters to this archetype's columns */
		HirArchetypeDecl *arch = find_archetype_decl(ctx, arch_name);
		if (arch) {
			for (int p = 0; p < sys->param_count; p++) {
				const char *param_name = sys->params[p]->name;

				/* Find this field in the archetype */
				for (int f = 0; f < arch->field_count; f++) {
					if (strcmp(arch->fields[f]->name, param_name) == 0) {
						const char *elem_type = llvm_type_from_arche(field_base_type_name(arch->fields[f]->type));

						if (arch->fields[f]->kind == FIELD_COLUMN) {
							/* Column pointer: for static use 3-index GEP to [0], for dynamic load pointer */
							int is_static = get_arch_static_capacity(ctx, arch_name) > 0;
							char *field_ptr = gen_value_name(ctx);
							if (is_static) {
								buffer_append_fmt(
								    ctx, "  %s = getelementptr %%struct.%s, %%struct.%s* %s, i32 0, i32 %d, i64 0\n",
								    field_ptr, arch_name, arch_name, arch_param, f);
							} else {
								char *field_gep = gen_value_name(ctx);
								buffer_append_fmt(ctx,
								                  "  %s = getelementptr %%struct.%s, %%struct.%s* %s, i32 0, i32 %d\n",
								                  field_gep, arch_name, arch_name, arch_param, f);
								buffer_append_fmt(ctx, "  %s = load %s*, %s** %s\n", field_ptr, elem_type, elem_type,
								                  field_gep);
							}

							/* Add to scope as a column pointer (type 4) */
							add_value(ctx, param_name, field_ptr, 4); /* type 4 = column pointer */
							/* Also track the archetype and element type for vectorization detection */
							ValueInfo *col_val = find_value(ctx, param_name);
							if (col_val) {
								col_val->arch_name = malloc(strlen(arch_name) + 1);
								strcpy(col_val->arch_name, arch_name);
								col_val->field_type = field_base_type_name(arch->fields[f]->type);
							}
						} else {
							/* Meta field: load the scalar value */
							char *field_gep = gen_value_name(ctx);
							buffer_append_fmt(ctx, "  %s = getelementptr %%struct.%s, %%struct.%s* %s, i32 0, i32 %d\n",
							                  field_gep, arch_name, arch_name, arch_param, f);

							char *field_val = gen_value_name(ctx);
							buffer_append_fmt(ctx, "  %s = load %s, %s* %s\n", field_val, elem_type, elem_type,
							                  field_gep);

							/* Add to scope */
							add_value(ctx, param_name, field_val, 0); /* type 0 = scalar */
						}
						break;
					}
				}
			}
		}

		/* Emit system body with this archetype's bindings */
		ctx->in_sys = 1;
		for (int s = 0; s < sys->stmt_count; s++) {
			codegen_statement(ctx, sys->stmts[s]);
		}
		ctx->in_sys = 0;

		pop_value_scope(ctx);
	}

	buffer_append(ctx, "  ret void\n");
	end_function_body(ctx, fbs);
	buffer_append(ctx, "}\n\n");

	/* Register function name (no version suffix) for HIR_STMT_RUN lookup */
	codegen_register_sys_version(ctx, sys->name, sys->name);
}

/* ========== PUBLIC API ========== */

int codegen_had_error(const CodegenContext *ctx) {
	return ctx && ctx->had_error;
}

void codegen_set_emit_unit(CodegenContext *ctx, int unit) {
	ctx->per_unit = 1;
	ctx->emit_only_unit = unit;
}
int codegen_per_unit_enabled(void) {
	return getenv("ARCHE_PER_UNIT") != NULL;
}

CodegenContext *codegen_create(HirProgram *ast, SemanticContext *sem_ctx) {
	CodegenContext *ctx = malloc(sizeof(CodegenContext));
	ctx->ast = ast;
	ctx->sem_ctx = sem_ctx;
	ctx->had_error = 0;
	ctx->per_unit = getenv("ARCHE_PER_UNIT") != NULL;
	ctx->emit_only_unit = -1;
	ctx->scopes = NULL;
	ctx->scope_count = 0;
	ctx->value_counter = 0;
	ctx->string_counter = 0;
	ctx->buffer_size = 8192;
	ctx->output_buffer = malloc(ctx->buffer_size);
	ctx->buffer_pos = 0;
	ctx->globals_size = 4096;
	ctx->globals_buffer = malloc(ctx->globals_size);
	ctx->globals_pos = 0;
	ctx->vector_lanes = 0;
	ctx->in_sys = 0;
	ctx->implicit_loop_index[0] = '\0'; /* Initialize to empty (not in loop) */
	ctx->loop_exit_labels = NULL;
	ctx->loop_exit_count = 0;
	ctx->loop_exit_capacity = 0;
	ctx->loop_cont_labels = NULL;
	ctx->loop_cont_count = 0;
	ctx->loop_cont_capacity = 0;
	ctx->sys_versions = NULL;
	ctx->sys_version_count = 0;
	ctx->sys_version_capacity = 0;
	ctx->top_level_allocs = NULL;
	ctx->alloc_count = 0;
	ctx->alloc_capacity = 0;
	ctx->static_arrays = NULL;
	ctx->static_array_count = 0;
	ctx->static_array_capacity = 0;
	ctx->scalars = NULL;
	ctx->scalar_count = 0;
	ctx->scalar_capacity = 0;
	ctx->alloca_buffer = NULL;
	ctx->alloca_buf_size = 0;
	ctx->alloca_buf_pos = 0;
	ctx->hoisting_allocas = 0;
	ctx->loop_bound_count = 0;
	ctx->current_archetype_param = NULL;
	ctx->current_arch_param_name = NULL;
	ctx->current_arch_param_llvm = NULL;
	ctx->current_each_field_binding = NULL;
	ctx->current_each_field_target = NULL;
	ctx->current_each_field_index = 0;
	ctx->current_each_field_name_global = NULL;
	ctx->pending_out_ptr_count = 0;
	ctx->mono_emitted = NULL;
	ctx->mono_emitted_count = 0;
	ctx->mono_emitted_capacity = 0;
	ctx->mono_pending = NULL;
	ctx->mono_pending_count = 0;
	ctx->mono_pending_capacity = 0;
	ctx->cb_binding_count = 0;
	ctx->cb_emitted = NULL;
	ctx->cb_emitted_count = 0;
	ctx->cb_emitted_capacity = 0;
	ctx->cb_pending = NULL;
	ctx->cb_pending_count = 0;
	ctx->cb_pending_capacity = 0;
	ctx->efield_name_counter = 0;
	ctx->uses_memcpy = 0;
	ctx->drop_reg = NULL;
	ctx->drop_reg_count = 0;
	ctx->drop_reg_capacity = 0;
	ctx->drop_live = NULL;
	ctx->drop_live_count = 0;
	ctx->drop_live_capacity = 0;
	ctx->block_terminated = 0;
	return ctx;
}

/* Build the RAII auto-drop registry from `@drop` HIR procs: opaque nominal type -> dtor symbol.
 * The proc name is already module-prefixed (e.g. "io_arche_fclose"); externs keep their bare C
 * name ("net_close"). Keyed by the `own` opaque param's preserved nominal name (e.g. "file"). */
static void codegen_build_drop_registry(CodegenContext *ctx) {
	for (int i = 0; i < ctx->ast->decl_count; i++) {
		HirDecl *d = ctx->ast->decls[i];
		if (!d || d->kind != HIR_DECL_PROC || !d->data.proc || !d->data.proc->is_drop)
			continue;
		HirProcDecl *p = d->data.proc;
		if (p->param_count != 1 || !p->params[0] || !p->params[0]->type)
			continue;
		HirType *pt = p->params[0]->type;
		if (pt->tag != HIR_TYPE_OPAQUE || !pt->name)
			continue;
		if (ctx->drop_reg_count >= ctx->drop_reg_capacity) {
			ctx->drop_reg_capacity = ctx->drop_reg_capacity ? ctx->drop_reg_capacity * 2 : 8;
			ctx->drop_reg = realloc(ctx->drop_reg, ctx->drop_reg_capacity * sizeof(*ctx->drop_reg));
		}
		ctx->drop_reg[ctx->drop_reg_count].type_name = pt->name;
		ctx->drop_reg[ctx->drop_reg_count].dtor = p->name;
		ctx->drop_reg_count++;
	}
}

/* Archetypes whose fields cover all of a system's params (the system's ABI param list). Shared by the
 * system definition (codegen_sys_decl) and its cross-unit declare so they agree exactly. */
static int collect_sys_matching_archs(CodegenContext *ctx, HirSysDecl *sys, const char **out, int max) {
	int n = 0;
	if (!(sys->param_count > 0 && sys->params[0] && sys->params[0]->name))
		return 0;
	for (int d = 0; d < ctx->ast->decl_count; d++) {
		if (ctx->ast->decls[d]->kind != HIR_DECL_ARCHETYPE)
			continue;
		HirArchetypeDecl *arch = ctx->ast->decls[d]->data.archetype;
		int has_all = 1;
		for (int p = 0; p < sys->param_count && has_all; p++) {
			int found = 0;
			for (int f = 0; f < arch->field_count; f++)
				if (strcmp(arch->fields[f]->name, sys->params[p]->name) == 0) {
					found = 1;
					break;
				}
			has_all = found;
		}
		if (has_all && n < max)
			out[n++] = arch->name;
	}
	return n;
}

/* Per-unit: emit `declare`s for every arche func/proc defined in a DIFFERENT unit, so this unit's
 * module is self-contained and the linker resolves the references. Parametric procs (archetype/
 * callback) have no real symbol (only their monomorphized instances do), so they're skipped. Systems
 * are defined only in the entry unit (unit 0), so any non-entry unit that `run`s one needs a declare —
 * over-declare all matching systems there (harmless if unused). Inert unless emitting one unit. */
static void emit_cross_unit_declares(CodegenContext *ctx) {
	if (!ctx->per_unit || ctx->emit_only_unit < 0)
		return;
	for (int i = 0; i < ctx->ast->decl_count; i++) {
		HirDecl *d = ctx->ast->decls[i];
		char sym[512];
		if (d->kind == HIR_DECL_SYS) {
			if (ctx->emit_only_unit < 1)
				continue; /* unit 0 defines systems; no declare needed there */
			const char *archs[256];
			int na = collect_sys_matching_archs(ctx, d->data.sys, archs, 256);
			if (na == 0)
				continue; /* no matching shape → no definition emitted → nothing to declare */
			buffer_append_fmt(ctx, "declare void @%s(", cg_fnsym(ctx, d->data.sys->name, 0, sym, sizeof(sym)));
			for (int a = 0; a < na; a++) {
				if (a > 0)
					buffer_append(ctx, ", ");
				buffer_append_fmt(ctx, "%%struct.%s*", archs[a]);
			}
			buffer_append(ctx, ")\n");
			continue;
		}
		if (d->unit == ctx->emit_only_unit)
			continue;
		if (d->kind == HIR_DECL_FUNC && !d->data.func->is_extern) {
			HirFuncDecl *f = d->data.func;
			char ret[512];
			if (f->return_type_count == 0)
				snprintf(ret, sizeof(ret), "void");
			else
				func_llvm_return_type(f, ret, sizeof(ret));
			buffer_append_fmt(ctx, "declare %s @%s(", ret, cg_fnsym(ctx, f->name, 0, sym, sizeof(sym)));
			emit_func_params(ctx, f);
			buffer_append(ctx, ")\n");
		} else if (d->kind == HIR_DECL_PROC && !d->data.proc->is_extern && !is_archetype_parametric(d->data.proc) &&
		           !has_callback_param(ctx, d->data.proc)) {
			HirProcDecl *p = d->data.proc;
			buffer_append_fmt(ctx, "declare void @%s(", cg_fnsym(ctx, p->name, 0, sym, sizeof(sym)));
			emit_proc_params(ctx, p);
			buffer_append(ctx, ")\n");
		}
	}
}

void codegen_generate(CodegenContext *ctx, FILE *output) {
	/* RAII: build the opaque-type -> destructor registry before emitting any body. */
	codegen_build_drop_registry(ctx);

	/* Preamble: declare external functions */
	buffer_append(ctx, "; Target datalayout and triple would go here\n");
	buffer_append(ctx,
	              "target datalayout = \"e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128\"\n");
	buffer_append(ctx, "target triple = \"x86_64-unknown-linux-gnu\"\n\n");

	/* Declare custom types as opaque structures */
	buffer_append(ctx, "; Type definitions\n");
	buffer_append(ctx, "%struct.Vec3 = type { double, double, double }\n\n");
	/* (The legacy %struct.arche_array {ptr,len,cap} is gone — char is a normal type-6 array now.) */

	/* External C library function declarations */
	buffer_append(ctx, "declare i8* @malloc(i32)\n");
	buffer_append(ctx, "declare i8* @calloc(i64, i64)\n");
	buffer_append(ctx, "declare void @free(i8*)\n");
	buffer_append(ctx, "declare void @abort()\n");
	/* Raw stderr write for bounds-check / panic messages — language runtime (alongside @abort),
	 * not user-facing I/O. Resolves to libc `write` at link (`-lc`). The fd-I/O `write` the user
	 * calls is a separate `os.write` symbol now, so there is no clash. */
	buffer_append(ctx, "declare i32 @write(i32, i8*, i32)\n");
	/* memcpy intrinsic (`copy x` of a local buffer). Declared lazily at module end only if a real
	 * memcpy was emitted, so a program with no copy carries no `llvm.memcpy` (see uses_memcpy). */

	/* Global error message for bounds check failures */
	buffer_append(
	    ctx,
	    "@.arche_oob = private unnamed_addr constant [28 x i8] c\"arche: index out of bounds\\0A\\00\", align 1\n\n");

	/* Per-unit: declare cross-unit funcs/procs up front (inert in whole-program mode). */
	emit_cross_unit_declares(ctx);

	/* Hoist archetype definitions: emit every archetype's struct type (and its insert/delete
	 * helpers) BEFORE any function body. A struct type used as a GEP source element must be sized at
	 * that point in the textual IR, so a proc that touches an archetype declared later in the source
	 * (e.g. behind a `#module` banner) would otherwise emit a def-after-use forward reference that
	 * the optimizer rejects ("base element of getelementptr must be sized"). Types hoist, like every
	 * other top-level name. (Globals may be forward-referenced in LLVM IR, so only the type defs need
	 * hoisting; the `@pool` storage decls stay in source order below.) */
	for (int i = 0; i < ctx->ast->decl_count; i++) {
		if (ctx->ast->decls[i]->kind == HIR_DECL_ARCHETYPE)
			codegen_archetype_decl(ctx, ctx->ast->decls[i]->data.archetype);
	}

	/* Register static arrays and scalar globals up front so a function body may reference one
	 * declared later in the source (globals may be forward-referenced in LLVM IR; the registries
	 * just need to know the name when the using function is emitted). Emission of the `@name`
	 * globals still happens in source order in the main loop below. */
	for (int i = 0; i < ctx->ast->decl_count; i++) {
		HirDecl *dd = ctx->ast->decls[i];
		if (dd->kind != HIR_DECL_STATIC)
			continue;
		if (dd->data.static_decl->kind == HIR_STATIC_ARRAY)
			codegen_register_static_array(ctx, dd->data.static_decl);
		else if (dd->data.static_decl->kind == HIR_STATIC_SCALAR)
			codegen_register_scalar(ctx, dd->data.static_decl);
	}

	/* Generate code for all declarations (this will populate globals_buffer with string constants) */
	int has_init_proc = 0;
	for (int i = 0; i < ctx->ast->decl_count; i++) {
		HirDecl *decl = ctx->ast->decls[i];

		/* Per-unit codegen: when restricted to one unit, emit only THIS unit's non-extern func/proc
		 * BODIES (cross-unit callees are `declare`d up front; externs are `declare`d in every module so
		 * they stay visible). Systems are NOT filtered by `decl->unit` — a device system lives in the
		 * device's unit but is `run` from the entry, so it is emitted once in unit 0 (the guard in
		 * codegen_sys_decl), not in its source unit. Inert by default (emit_only_unit == -1). */
		if (ctx->per_unit && ctx->emit_only_unit >= 0 && decl->unit != ctx->emit_only_unit) {
			int is_ext = (decl->kind == HIR_DECL_FUNC && decl->data.func->is_extern) ||
			             (decl->kind == HIR_DECL_PROC && decl->data.proc->is_extern);
			if (!is_ext &&
			    (decl->kind == HIR_DECL_FUNC || decl->kind == HIR_DECL_PROC || decl->kind == HIR_DECL_FUNC_GROUP))
				continue;
		}

		switch (decl->kind) {
		case HIR_DECL_ARCHETYPE:
			/* Already emitted in the hoist pre-pass above. */
			break;
		case HIR_DECL_STATIC: {
			HirStaticDecl *s = decl->data.static_decl;
			if (s->kind == HIR_STATIC_ARCHETYPE && s->is_requirement) {
				/* A datasheet storage requirement (minimum rows) — emits no storage; the driver's
				 * own pool for the shape is the real allocation. */
				break;
			}
			if (s->kind == HIR_STATIC_ARCHETYPE) {
				codegen_static_decl(ctx, s);
			} else if (s->kind == HIR_STATIC_SCALAR) {
				/* Scalar global: `@name = global <ty> <const>`. The variable is mutable (loaded/
				 * stored at use sites); only its initial value is a compile-time constant. */
				const char *lt = llvm_type_from_arche(field_base_type_name(s->scalar.type));
				char cst[64];
				codegen_scalar_init_const(ctx, s->scalar.init, cst, sizeof(cst));
				buffer_append_fmt(ctx, "@%s = %sglobal %s %s\n", s->scalar.name, cg_shared(ctx), lt, cst);
				/* registered in the pre-pass above */
			} else {
				const char *llvm_type = "i8";
				const char *elem_name = field_base_type_name(s->array.element_type);
				int is_char = strcmp(elem_name, "char") == 0;
				if (strcmp(elem_name, "double") == 0) {
					llvm_type = "double";
				} else if (strcmp(elem_name, "float") == 0) {
					llvm_type = "float";
				} else if (strcmp(elem_name, "int") == 0) {
					llvm_type = "i32";
				}
				/* char and non-char static arrays alike: a plain `[N x T]` global. char is just i8 —
				 * a normal array, no arche_array struct wrapper. (`is_char` only selects the element.) */
				(void)is_char;
				buffer_append_fmt(ctx, "@%s = %sglobal [%d x %s] zeroinitializer\n", s->array.name, cg_shared(ctx),
				                  s->array.size, llvm_type);
				/* registered in the pre-pass above */
			}
			break;
		}
		case HIR_DECL_FUNC:
			codegen_func_decl(ctx, decl->data.func);
			break;
		case HIR_DECL_FUNC_GROUP:
			/* No codegen for func groups yet */
			break;
		case HIR_DECL_PROC:
			if (strcmp(decl->data.proc->name, "init") == 0) {
				has_init_proc = 1;
			}
			/* Archetype-parametric and callback-parametric procs only emit when
			 * called with a concrete archetype / known callback. Defer to the
			 * EXPR_CALL handler to materialize the specializations. */
			if (is_archetype_parametric(decl->data.proc) || has_callback_param(ctx, decl->data.proc)) {
				break;
			}
			codegen_proc_decl(ctx, decl->data.proc);
			break;
		case HIR_DECL_SYS:
			codegen_sys_decl(ctx, decl->data.sys);
			break;
		case HIR_DECL_CONST:
			/* Value consts are inlined at their use sites (semantic_get_const_value); type
			 * aliases are erased before lowering. No per-declaration code to emit. */
			break;
		case HIR_DECL_WORLD:
			/* No codegen for worlds. */
			break;
		}
	}

	/* Drain monomorphization worklist: emit specialized bodies for every
	 * (archetype-parametric proc, archetype) pair encountered at a call site. */
	drain_monomorph_worklist(ctx);

	/* Emit global constants (strings, etc.) */
	if (ctx->globals_pos > 0) {
		buffer_append(ctx, "\n; Global constants\n");
		char temp[ctx->globals_pos + 1];
		strcpy(temp, ctx->globals_buffer);
		buffer_append(ctx, temp);
		buffer_append(ctx, "\n");
	}

	/* Generate main entry point (always needed for initialization) */
	int has_main_proc = 0;
	for (int i = 0; i < ctx->ast->decl_count; i++) {
		if (ctx->ast->decls[i]->kind == HIR_DECL_PROC && strcmp(ctx->ast->decls[i]->data.proc->name, "main") == 0) {
			has_main_proc = 1;
			break;
		}
	}

	/* The process entry wrapper is program-global: emit it only in the entry unit (0) — never in a
	 * per-unit module for an imported unit. (emit_only_unit -1 = whole-program, also emits it.) */
	if (!(ctx->per_unit && ctx->emit_only_unit >= 1)) {
		char init_sym[512], mainu_sym[512];
		buffer_append(ctx, "\ndeclare void @arche_set_args(i32, i8**)\n");
		buffer_append(ctx, "\ndefine i32 @main(i32 %argc, i8** %argv) {\n");
		buffer_append(ctx, "entry:\n");
		buffer_append(ctx, "  call void @arche_set_args(i32 %argc, i8** %argv)\n");

		/* Emit allocation initialization code (always, regardless of user main) */
		for (int i = 0; i < ctx->alloc_count; i++) {
			codegen_emit_alloc_init(ctx, ctx->top_level_allocs[i]);
		}

		if (has_init_proc)
			buffer_append_fmt(ctx, "  call void @%s()\n", cg_fnsym(ctx, "init", 0, init_sym, sizeof(init_sym)));

		if (has_main_proc)
			buffer_append_fmt(ctx, "  call void @%s()\n", cg_fnsym(ctx, "main_user", 0, mainu_sym, sizeof(mainu_sym)));

		buffer_append(ctx, "  ret i32 0\n");
		buffer_append(ctx, "}\n");
	}

	/* (No built-in print helpers: printing is NOT a language primitive. Numeric/text printing is
	 * the `fmt` library's job — `fmt.printf`/`fmt.print_float` bind libc directly. The compiler
	 * therefore emits no `@printf` reference of its own.) */

	/* Emit AVX2 function attributes */
	buffer_append(ctx, "\nattributes #0 = { \"target-features\"=\"+avx2,+avx\" }\n");

	/* Lazily declare the memcpy intrinsic only if a real copy used it (module-scope `declare`
	 * order is irrelevant in LLVM IR). */
	if (ctx->uses_memcpy)
		buffer_append(ctx, "\ndeclare void @llvm.memcpy.p0.p0.i64(i8*, i8*, i64, i1)\n");

	/* Output the generated IR */
	fprintf(output, "%s", ctx->output_buffer);
}

void codegen_free(CodegenContext *ctx) {
	if (!ctx)
		return;

	while (ctx->scope_count > 0) {
		pop_value_scope(ctx);
	}
	free(ctx->scopes);
	free(ctx->output_buffer);
	free(ctx->globals_buffer);
	free(ctx->alloca_buffer);
	free(ctx->sys_versions);
	free(ctx->loop_exit_labels);
	free(ctx->loop_cont_labels);
	free(ctx->top_level_allocs);
	free(ctx->static_arrays);
	for (int i = 0; i < ctx->drop_live_count; i++) {
		free(ctx->drop_live[i].var_name);
		free(ctx->drop_live[i].slot);
	}
	free(ctx->drop_live);
	free(ctx->drop_reg);
	free(ctx);
}
