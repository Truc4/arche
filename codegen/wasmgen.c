// wasmgen.c — direct HIR→wasm backend (see wasmgen.h). A peer of codegen.c: same HirProgram input, emits a
// self-contained wasm module instead of LLVM IR text (no LLVM/clang/wasm-ld). Built incrementally:
//   M0 ✓ encoder · pipeline ✓ wired into compile.c · M3 ✓ the compiler itself cross-compiled.
//   Compute coverage: run-once `system` → `_start`. TYPE-AWARE (i32 + f64): integer/float locals, literals
//   (int 0x/0b/0o/_, float, bool, char), BIND/ASSIGN (incl. compound), C-style FOR, IF, arithmetic +
//   comparison + &&/||, int↔float promotion, and fmt.printf → host arche_print / arche_printf (8-byte arg
//   slots; %d/%u/%x/%c/%f formatted host-side). NOT yet: arrays/indexing, pools/entity-lit, i64, the reactor.
#include "wasmgen.h"
#include "../hir/hir.h"
#include "../syntax/type_ref.h"
#include "wasm_encode.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WG_MAX_LOCALS 256
#define WG_MAX_ARRAYS 128

typedef struct {
	const char *name;
	uint8_t wt;
} WgLocal;
// A fixed-size local array, statically placed in linear memory at `base` (compute _start runs once, so no
// reentrancy → static allocation is safe). Indexing is `base + i*elem_size`; load/store keyed by elem type.
typedef struct {
	const char *name;
	int base;
	int elem_size;
	uint8_t elem_wt;
	int elem_signed;
	int total;
} WgArray;

typedef struct {
	WasmModule m;
	WasmBuf body;                  // the accumulating _start instruction stream
	WasmBuf strings;               // packed string bytes → one data segment at strings_base
	int strings_base;              // linear-memory offset of the string pool
	int args_base;                 // scratch region where printf args are marshalled (8-byte slots) for the host
	int arrays_cursor;             // bump pointer for statically-placed local arrays
	int f_print;                   // import arche_print(ptr,len)
	int f_printf;                  // import arche_printf(fmtPtr,fmtLen,argsPtr,argc)
	WgLocal locals[WG_MAX_LOCALS]; // name→index + wasm type (HIR name pointers, stable for the emit)
	int local_count;
	WgArray arrays[WG_MAX_ARRAYS];
	int array_count;
	struct {
		const char *name;
		const HirExpr *value;
	} consts[WG_MAX_LOCALS]; // top-level `NAME :: value` consts, inlined at use
	int const_count;
} Wg;

// The wasm value type for an HIR type: float → f64, everything else (int/char/bool/handle) → i32.
static uint8_t wg_wtype(const HirType *t) {
	return (t && t->tag == HIR_TYPE_FLOAT) ? WT_F64 : WT_I32;
}
static int wg_is_array(const HirType *t) {
	return t && (t->tag == HIR_TYPE_SHAPED_ARRAY || t->tag == HIR_TYPE_ARRAY || t->tag == HIR_TYPE_CHAR_ARRAY);
}
static const HirType *wg_base_elem(const HirType *t) {
	while (t && (t->tag == HIR_TYPE_SHAPED_ARRAY || t->tag == HIR_TYPE_ARRAY))
		t = t->elem;
	return t;
}
static int wg_total_elems(const HirType *t) {
	if (!t)
		return 1;
	if (t->tag == HIR_TYPE_SHAPED_ARRAY)
		return (t->rank > 0 ? t->rank : 1) * wg_total_elems(t->elem);
	if (t->tag == HIR_TYPE_ARRAY)
		return wg_total_elems(t->elem);
	return 1;
}
static int wg_scalar_size(const HirType *t) {
	if (!t)
		return 4;
	if (t->tag == HIR_TYPE_FLOAT)
		return 8;
	if (t->tag == HIR_TYPE_CHAR || t->tag == HIR_TYPE_CHAR_ARRAY)
		return 1;
	if (t->tag == HIR_TYPE_INT)
		return t->int_width >= 8 ? t->int_width / 8 : 4;
	return 4;
}

static int wg_local(Wg *g, const char *name, uint8_t wt) {
	for (int i = 0; i < g->local_count; i++)
		if (strcmp(g->locals[i].name, name) == 0)
			return i;
	if (g->local_count < WG_MAX_LOCALS) {
		g->locals[g->local_count].name = name;
		g->locals[g->local_count].wt = wt;
	}
	return g->local_count++;
}

// Parse an Arche integer literal: 0x/0b/0o prefixes + `_` digit separators (strtol base-0 handles none).
static int32_t wg_parse_int(const char *s) {
	char buf[80];
	int n = 0;
	for (const char *p = s; *p && n < 79; p++)
		if (*p != '_')
			buf[n++] = *p;
	buf[n] = 0;
	const char *p = buf;
	int neg = 0;
	if (*p == '-') {
		neg = 1;
		p++;
	} else if (*p == '+')
		p++;
	long v;
	if (p[0] == '0' && (p[1] == 'b' || p[1] == 'B'))
		v = strtol(p + 2, NULL, 2);
	else if (p[0] == '0' && (p[1] == 'o' || p[1] == 'O'))
		v = strtol(p + 2, NULL, 8);
	else if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
		v = strtol(p + 2, NULL, 16);
	else
		v = strtol(p, NULL, 10);
	return (int32_t)(neg ? -v : v);
}

static int32_t wg_parse_char(const char *lx) {
	const char *p = lx;
	if (*p == '\'')
		p++;
	if (*p == '\\') {
		char e = p[1];
		return e == 'n'    ? '\n'
		       : e == 't'  ? '\t'
		       : e == 'r'  ? '\r'
		       : e == '0'  ? 0
		       : e == '\\' ? '\\'
		       : e == '\'' ? '\''
		                   : e;
	}
	return (unsigned char)*p;
}

static void emit_const(WasmBuf *b, int32_t v) {
	wb_byte(b, 0x41);
	wb_i(b, v);
}
static void emit_fconst(WasmBuf *b, double d) {
	wb_byte(b, 0x44);
	uint8_t by[8];
	memcpy(by, &d, 8);
	wb_bytes(b, by, 8);
}
static void emit_local_get(WasmBuf *b, int i) {
	wb_byte(b, 0x20);
	wb_u(b, i);
}
static void emit_local_set(WasmBuf *b, int i) {
	wb_byte(b, 0x21);
	wb_u(b, i);
}

static int binop_i(Operator op) {
	switch (op) {
	case OP_ADD:
		return 0x6a;
	case OP_SUB:
		return 0x6b;
	case OP_MUL:
		return 0x6c;
	case OP_DIV:
		return 0x6d;
	case OP_MOD:
		return 0x6f;
	case OP_EQ:
		return 0x46;
	case OP_NEQ:
		return 0x47;
	case OP_LT:
		return 0x48;
	case OP_GT:
		return 0x4a;
	case OP_LTE:
		return 0x4c;
	case OP_GTE:
		return 0x4e;
	case OP_AND:
		return 0x71;
	case OP_OR:
		return 0x72; // bool operands are 0/1 → bitwise == logical
	default:
		return 0;
	}
}
static int binop_f(Operator op) {
	switch (op) {
	case OP_ADD:
		return 0xa0;
	case OP_SUB:
		return 0xa1;
	case OP_MUL:
		return 0xa2;
	case OP_DIV:
		return 0xa3;
	case OP_EQ:
		return 0x61;
	case OP_NEQ:
		return 0x62;
	case OP_LT:
		return 0x63;
	case OP_GT:
		return 0x64;
	case OP_LTE:
		return 0x65;
	case OP_GTE:
		return 0x66;
	default:
		return 0;
	}
}

static void wg_intern_string(Wg *g, const char *raw, int rawlen, int *out_off, int *out_len) {
	int off = g->strings_base + (int)g->strings.len, n = 0;
	for (int i = 0; i < rawlen; i++) {
		char c = raw[i];
		if (c == '\\' && i + 1 < rawlen) {
			char e = raw[++i];
			c = e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e == '0' ? '\0' : e;
		}
		wb_byte(&g->strings, (uint8_t)c);
		n++;
	}
	*out_off = off;
	*out_len = n;
}

// Unwrap an effect-application `f(...)(_:)` down to the inner call `f(...)` (the callee-is-a-call chain).
static const HirExpr *wg_unwrap_eff(const HirExpr *e) {
	while (e && e->kind == HIR_EXPR_CALL && e->data.call.callee && e->data.call.callee->kind == HIR_EXPR_CALL)
		e = e->data.call.callee;
	return e;
}
// A call to fmt.print / fmt.printf (the callee names it). Assumes eff-application already unwrapped.
static int is_print_call(const HirExpr *e) {
	if (!e || e->kind != HIR_EXPR_CALL)
		return 0;
	const HirExpr *c = e->data.call.callee;
	const char *nm = !c                          ? NULL
	                 : c->kind == HIR_EXPR_NAME  ? c->data.name.name
	                 : c->kind == HIR_EXPR_FIELD ? c->data.field.field_name
	                                             : NULL;
	return nm && (strstr(nm, "printf") || strcmp(nm, "print") == 0);
}

static WgArray *wg_array_find(Wg *g, const char *name) {
	for (int i = 0; i < g->array_count; i++)
		if (strcmp(g->arrays[i].name, name) == 0)
			return &g->arrays[i];
	return NULL;
}
// Register a fixed-size local array `name` of type `t`; statically place it and return its record.
static WgArray *wg_array_decl(Wg *g, const char *name, const HirType *t) {
	if (g->array_count >= WG_MAX_ARRAYS)
		return NULL;
	const HirType *base = wg_base_elem(t);
	WgArray *a = &g->arrays[g->array_count++];
	a->name = name;
	a->elem_size = wg_scalar_size(base);
	a->elem_wt = (base && base->tag == HIR_TYPE_FLOAT) ? WT_F64 : WT_I32;
	a->elem_signed = base && base->tag == HIR_TYPE_INT ? base->int_signed : 0;
	a->total = wg_total_elems(t);
	a->base = g->arrays_cursor;
	g->arrays_cursor += a->total * a->elem_size;
	return a;
}
// Emit a load/store for one element (address already on the stack for load; addr+value for store).
static void wg_emit_load(Wg *g, const WgArray *a) {
	if (a->elem_wt == WT_F64) {
		wb_byte(&g->body, 0x2b);
		wb_byte(&g->body, 0x03);
		wb_byte(&g->body, 0x00);
		return;
	} // f64.load
	if (a->elem_size == 1) {
		wb_byte(&g->body, a->elem_signed ? 0x2c : 0x2d);
		wb_byte(&g->body, 0x00);
		wb_byte(&g->body, 0x00);
		return;
	} // i32.load8_s/u
	if (a->elem_size == 2) {
		wb_byte(&g->body, a->elem_signed ? 0x2e : 0x2f);
		wb_byte(&g->body, 0x01);
		wb_byte(&g->body, 0x00);
		return;
	} // i32.load16_s/u
	wb_byte(&g->body, 0x28);
	wb_byte(&g->body, 0x02);
	wb_byte(&g->body, 0x00); // i32.load (4; 8-byte int is lossy — see NEEDS-HUMAN i64)
}
static void wg_emit_store(Wg *g, const WgArray *a) {
	if (a->elem_wt == WT_F64) {
		wb_byte(&g->body, 0x39);
		wb_byte(&g->body, 0x03);
		wb_byte(&g->body, 0x00);
		return;
	} // f64.store
	if (a->elem_size == 1) {
		wb_byte(&g->body, 0x3a);
		wb_byte(&g->body, 0x00);
		wb_byte(&g->body, 0x00);
		return;
	} // i32.store8
	if (a->elem_size == 2) {
		wb_byte(&g->body, 0x3b);
		wb_byte(&g->body, 0x01);
		wb_byte(&g->body, 0x00);
		return;
	} // i32.store16
	wb_byte(&g->body, 0x36);
	wb_byte(&g->body, 0x02);
	wb_byte(&g->body, 0x00); // i32.store
}

static void wg_emit_expr(Wg *g, const HirExpr *e);

// Emit the address of `arr[index]` (arr->base + index*elem_size) onto the stack.
static void wg_emit_elem_addr(Wg *g, WgArray *a, const HirExpr *index) {
	emit_const(&g->body, a->base);
	wg_emit_expr(g, index);
	if (a->elem_size != 1) {
		emit_const(&g->body, a->elem_size);
		wb_byte(&g->body, 0x6c);
	}                        // i32.mul
	wb_byte(&g->body, 0x6a); // i32.add
}

// Emit `e`, coercing its value to f64 if it isn't already (for a mixed int/float operand).
static void wg_emit_as_float(Wg *g, const HirExpr *e) {
	wg_emit_expr(g, e);
	if (wg_wtype(&e->resolved) != WT_F64)
		wb_byte(&g->body, 0xb7); // f64.convert_i32_s
}

static void wg_emit_expr(Wg *g, const HirExpr *e) {
	if (!e) {
		emit_const(&g->body, 0);
		return;
	}
	switch (e->kind) {
	case HIR_EXPR_LITERAL: {
		const char *lx = e->data.literal.lexeme;
		if (wg_wtype(&e->resolved) == WT_F64)
			emit_fconst(&g->body, strtod(lx, NULL));
		else if (e->resolved.tag == HIR_TYPE_CHAR)
			emit_const(&g->body, wg_parse_char(lx));
		else if (strcmp(lx, "true") == 0)
			emit_const(&g->body, 1);
		else if (strcmp(lx, "false") == 0)
			emit_const(&g->body, 0);
		else
			emit_const(&g->body, wg_parse_int(lx));
		break;
	}
	case HIR_EXPR_NAME: {
		const char *nm = e->data.name.name;
		for (int i = 0; i < g->const_count; i++) // a reference to a top-level const → inline its value
			if (strcmp(g->consts[i].name, nm) == 0) {
				wg_emit_expr(g, g->consts[i].value);
				return;
			}
		emit_local_get(&g->body, wg_local(g, nm, wg_wtype(&e->resolved)));
		break;
	}
	case HIR_EXPR_BINARY: {
		const HirExpr *l = e->data.binary.left, *r = e->data.binary.right;
		int isf = wg_wtype(&l->resolved) == WT_F64 || wg_wtype(&r->resolved) == WT_F64;
		int op = isf ? binop_f(e->data.binary.op) : binop_i(e->data.binary.op);
		if (op) {
			if (isf) {
				wg_emit_as_float(g, l);
				wg_emit_as_float(g, r);
			} else {
				wg_emit_expr(g, l);
				wg_emit_expr(g, r);
			}
			wb_byte(&g->body, (uint8_t)op);
		} else
			emit_const(&g->body, 0); // unsupported binop → stack-safe placeholder (arche exprs are pure)
		break;
	}
	case HIR_EXPR_INDEX: {
		const HirExpr *base = e->data.index.base;
		WgArray *a = base->kind == HIR_EXPR_NAME ? wg_array_find(g, base->data.name.name) : NULL;
		if (a && e->data.index.index_count == 1) {
			wg_emit_elem_addr(g, a, e->data.index.indices[0]);
			wg_emit_load(g, a);
		} else
			emit_const(&g->body, 0);
		break;
	}
	case HIR_EXPR_FIELD: {
		// `arr.length` on a local array → the (compile-time) element count.
		const HirExpr *base = e->data.field.base;
		const char *fn = e->data.field.field_name;
		WgArray *a = (base->kind == HIR_EXPR_NAME && fn && strcmp(fn, "length") == 0)
		                 ? wg_array_find(g, base->data.name.name)
		                 : NULL;
		if (a)
			emit_const(&g->body, a->total);
		else
			emit_const(&g->body, 0);
		break;
	}
	default:
		emit_const(&g->body, 0);
		break; // SLICE/UNARY/CALL/ALLOC/ENTITY_LIT/etc. not yet supported
	}
}

static void wg_emit_printf(Wg *g, const HirExpr *call) {
	int argc = call->data.call.arg_count;
	const HirExpr *fmt = argc >= 1 ? call->data.call.args[0] : NULL;
	if (!fmt || fmt->kind != HIR_EXPR_STRING)
		return;
	int off, len;
	wg_intern_string(g, fmt->data.string.value, fmt->data.string.length, &off, &len);
	int nv = argc - 1;
	if (nv <= 0) { // bare string → arche_print(ptr,len)
		emit_const(&g->body, off);
		emit_const(&g->body, len);
		wb_byte(&g->body, 0x10);
		wb_u(&g->body, g->f_print);
		return;
	}
	for (int j = 0; j < nv; j++) { // marshal each arg into an 8-byte slot
		const HirExpr *a = call->data.call.args[1 + j];
		int slot = g->args_base + j * 8;
		// %s: a string arg → store {ptr, len} in the slot (host reads both). Covers string literals + char[] locals.
		if (a->kind == HIR_EXPR_STRING || a->resolved.tag == HIR_TYPE_CHAR_ARRAY) {
			int p = 0, l = 0;
			if (a->kind == HIR_EXPR_STRING)
				wg_intern_string(g, a->data.string.value, a->data.string.length, &p, &l);
			else {
				WgArray *arr = a->kind == HIR_EXPR_NAME ? wg_array_find(g, a->data.name.name) : NULL;
				if (arr) {
					p = arr->base;
					l = arr->total;
				}
			}
			emit_const(&g->body, slot);
			emit_const(&g->body, p);
			wb_byte(&g->body, 0x36);
			wb_byte(&g->body, 0x02);
			wb_byte(&g->body, 0x00);
			emit_const(&g->body, slot + 4);
			emit_const(&g->body, l);
			wb_byte(&g->body, 0x36);
			wb_byte(&g->body, 0x02);
			wb_byte(&g->body, 0x00);
			continue;
		}
		emit_const(&g->body, slot);
		wg_emit_expr(g, a);
		if (wg_wtype(&a->resolved) == WT_F64) {
			wb_byte(&g->body, 0x39);
			wb_byte(&g->body, 0x03);
			wb_byte(&g->body, 0x00);
		} // f64.store
		else {
			wb_byte(&g->body, 0x36);
			wb_byte(&g->body, 0x02);
			wb_byte(&g->body, 0x00);
		} // i32.store
	}
	emit_const(&g->body, off);
	emit_const(&g->body, len);
	emit_const(&g->body, g->args_base);
	emit_const(&g->body, nv);
	wb_byte(&g->body, 0x10);
	wb_u(&g->body, g->f_printf);
}

static void wg_emit_stmt(Wg *g, const HirStmt *s) {
	if (!s)
		return;
	switch (s->kind) {
	case HIR_STMT_BIND: {
		const HirBindStmt *b = &s->data.bind_stmt;
		const HirType *at = b->type && wg_is_array(b->type)
		                        ? b->type
		                        : (b->value && wg_is_array(&b->value->resolved) ? &b->value->resolved : NULL);
		if (at) { // an array local: statically place it; init from an array literal if present (else zero)
			WgArray *a = wg_array_decl(g, b->names[0], at);
			if (a && b->value && b->value->kind == HIR_EXPR_ARRAY_LITERAL) {
				const HirExpr *lit = b->value;
				for (int k = 0; k < lit->data.array_literal.element_count && k < a->total; k++) {
					emit_const(&g->body, a->base + k * a->elem_size);
					if (a->elem_wt == WT_F64)
						wg_emit_as_float(g, lit->data.array_literal.elements[k]);
					else
						wg_emit_expr(g, lit->data.array_literal.elements[k]);
					wg_emit_store(g, a);
				}
			}
			break;
		}
		uint8_t wt = b->value ? wg_wtype(&b->value->resolved) : WT_I32;
		if (b->value)
			wg_emit_expr(g, b->value);
		else
			emit_const(&g->body, 0);
		emit_local_set(&g->body, wg_local(g, b->names[0], wt));
		break;
	}
	case HIR_STMT_ASSIGN: {
		const HirAssignStmt *a = &s->data.assign_stmt;
		if (a->target->kind == HIR_EXPR_INDEX) { // arr[i] = v  (or compound arr[i] op= v)
			const HirExpr *base = a->target->data.index.base;
			WgArray *arr = base->kind == HIR_EXPR_NAME ? wg_array_find(g, base->data.name.name) : NULL;
			if (arr && a->target->data.index.index_count == 1) {
				const HirExpr *idx = a->target->data.index.indices[0];
				wg_emit_elem_addr(g, arr, idx);
				if (a->op != OP_NONE) {
					wg_emit_elem_addr(g, arr, idx);
					wg_emit_load(g, arr);
				} // load current for compound
				if (arr->elem_wt == WT_F64)
					wg_emit_as_float(g, a->value);
				else
					wg_emit_expr(g, a->value);
				if (a->op != OP_NONE) {
					int op = arr->elem_wt == WT_F64 ? binop_f(a->op) : binop_i(a->op);
					if (op)
						wb_byte(&g->body, (uint8_t)op);
				}
				wg_emit_store(g, arr);
			}
			break;
		}
		if (a->target->kind != HIR_EXPR_NAME)
			break; // other targets: later
		uint8_t wt = wg_wtype(&a->target->resolved);
		int idx = wg_local(g, a->target->data.name.name, wt);
		if (a->op == OP_NONE) {
			wg_emit_expr(g, a->value);
		} else {
			emit_local_get(&g->body, idx);
			int op = wt == WT_F64 ? binop_f(a->op) : binop_i(a->op);
			if (wt == WT_F64)
				wg_emit_as_float(g, a->value);
			else
				wg_emit_expr(g, a->value);
			if (op)
				wb_byte(&g->body, (uint8_t)op);
		}
		emit_local_set(&g->body, idx);
		break;
	}
	case HIR_STMT_FOR: {
		const HirForStmt *f = &s->data.for_stmt;
		if (f->init)
			wg_emit_stmt(g, f->init);
		wb_byte(&g->body, 0x02);
		wb_byte(&g->body, 0x40); // block (void)  ← exit target (depth 1)
		wb_byte(&g->body, 0x03);
		wb_byte(&g->body, 0x40); // loop (void)   ← continue target (depth 0)
		if (f->cond) {
			wg_emit_expr(g, f->cond);
			wb_byte(&g->body, 0x45 /*i32.eqz*/);
			wb_byte(&g->body, 0x0d);
			wb_u(&g->body, 1);
		} // br_if exit when !cond
		for (int j = 0; j < f->body_count; j++)
			wg_emit_stmt(g, f->body[j]);
		if (f->incr)
			wg_emit_stmt(g, f->incr);
		wb_byte(&g->body, 0x0c);
		wb_u(&g->body, 0);       // br loop (continue)
		wb_byte(&g->body, 0x0b); // end loop
		wb_byte(&g->body, 0x0b); // end block
		break;
	}
	case HIR_STMT_IF: {
		const HirIfStmt *f = &s->data.if_stmt;
		wg_emit_expr(g, f->cond);
		wb_byte(&g->body, 0x04);
		wb_byte(&g->body, 0x40); // if (void)
		for (int j = 0; j < f->then_count; j++)
			wg_emit_stmt(g, f->then_body[j]);
		if (f->else_count) {
			wb_byte(&g->body, 0x05 /*else*/);
			for (int j = 0; j < f->else_count; j++)
				wg_emit_stmt(g, f->else_body[j]);
		}
		wb_byte(&g->body, 0x0b); // end if
		break;
	}
	case HIR_STMT_EXPR: {
		const HirExpr *e = wg_unwrap_eff(s->data.expr_stmt.expr); // f(...)(_:) → f(...)
		if (is_print_call(e))
			wg_emit_printf(g, e);
		break;
	}
	default:
		break; // MULTI_BIND / EACH / RUN etc. → later
	}
}

int wasmgen_generate(HirProgram *ast, SemanticContext *sem, const char *out_path) {
	(void)sem;
	Wg g;
	memset(&g, 0, sizeof(g));
	wm_init(&g.m);
	wm_memory(&g.m, 4); // 256 KiB: args (512) · strings (1024+) · arrays (16384+)
	wm_export_mem(&g.m, "memory");
	wb_init(&g.body);
	wb_init(&g.strings);
	g.args_base = 512;
	g.strings_base = 1024;
	g.arrays_cursor = 16384;

	uint8_t p2[] = {WT_I32, WT_I32};
	uint8_t p4[] = {WT_I32, WT_I32, WT_I32, WT_I32};
	g.f_print = wm_import_func(&g.m, "arche_print", wm_type(&g.m, p2, 2, NULL, 0));
	g.f_printf = wm_import_func(&g.m, "arche_printf", wm_type(&g.m, p4, 4, NULL, 0));

	int has_run = 0;
	for (int i = 0; i < ast->decl_count; i++) {
		if (ast->decls[i]->kind == HIR_DECL_RUN)
			has_run = 1;
		if (ast->decls[i]->kind == HIR_DECL_CONST && g.const_count < WG_MAX_LOCALS) {
			HirConstDecl *c = ast->decls[i]->data.constant;
			if (c && c->name && c->value) {
				g.consts[g.const_count].name = c->name;
				g.consts[g.const_count].value = c->value;
				g.const_count++;
			}
		}
	}
	if (has_run) {
		for (int i = 0; i < ast->decl_count; i++) {
			HirDecl *d = ast->decls[i];
			if (d->kind != HIR_DECL_KERNEL)
				continue;
			HirKernelDecl *k = d->data.kernel;
			if (k->kind != HIR_KERNEL_SYSTEM || k->param_count != 0)
				continue; // run-once systems
			for (int j = 0; j < k->stmt_count; j++)
				wg_emit_stmt(&g, k->stmts[j]);
		}
	}
	wb_byte(&g.body, 0x0b); // end

	int t_void = wm_type(&g.m, NULL, 0, NULL, 0);
	int f_start = wm_func(&g.m, t_void);
	if (g.local_count > 0) {
		uint8_t lt[WG_MAX_LOCALS];
		uint32_t lc[WG_MAX_LOCALS];
		for (int i = 0; i < g.local_count; i++) {
			lt[i] = g.locals[i].wt;
			lc[i] = 1;
		} // one group per local → first-seen indexing
		wm_code(&g.m, lt, lc, g.local_count, &g.body);
	} else
		wm_code(&g.m, NULL, NULL, 0, &g.body);
	wm_export_func(&g.m, "_start", f_start);

	if (g.strings.len)
		wm_data(&g.m, g.strings_base, g.strings.data, g.strings.len);

	WasmBuf out;
	wm_finish(&g.m, &out);
	FILE *f = fopen(out_path, "wb");
	int ok = 0;
	if (f) {
		ok = fwrite(out.data, 1, out.len, f) == out.len;
		fclose(f);
	}
	wm_free(&g.m);
	wb_free(&g.body);
	wb_free(&g.strings);
	wb_free(&out);
	return ok;
}
