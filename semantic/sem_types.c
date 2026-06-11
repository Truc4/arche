#include "sem_types.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Internal layout: an arena owns a growable array of TypeNode + a string pool.
 * TypeId is index+1 (0 reserved for UNKNOWN). Lookup for interning is linear
 * over the existing nodes — fine for the volume a single compilation produces
 * (typically <500 types). If profiling ever shows it, swap for a hash map; the
 * public API doesn't change. */

typedef struct {
	TyKind kind;
	union {
		PrimKind prim;
		/* A nominal type: an interned name + an optional backing TypeId. `backing == 0` is a
		 * standalone nominal (opaque tag, archetype name, enum, unresolved name). `backing != 0`
		 * is a tier-2 DISTINCT subtype — its own identity (so `meters != float` under tyid_equal)
		 * that is one-way usable AS its backing (tyid_usable_as). A tier-1 TRANSPARENT alias never
		 * reaches here: the caller interns it directly as its backing's TypeId (so `int == i32`). */
		struct {
			const char *name; /* interned in str pool */
			TypeId backing;
		} nominal;
		struct {
			TypeId elem;
		} array;
		struct {
			TypeId elem;
			int rank;
		} shaped;
		struct {
			const char **names; /* interned in str pool */
			TypeId *types;
			int count;
		} tuple;
		struct {
			const char *archetype_name;
		} handle;
		struct {
			TypeId *params;
			int param_count;
			TypeId *returns; /* func: single return; proc: out-params; sys/policy: none */
			int return_count;
		} func; /* shared payload for the four callable KINDS (TYK_FUNC/PROC/SYS/POLICY) — the kind, not
		         * a flag, tells them apart, so a proc-type and func-type never unify. */
	} data;
} TypeNode;

struct TypeArena {
	TypeNode *nodes;
	int node_count;
	int node_cap;

	char **strings;
	int string_count;
	int string_cap;
};

TypeArena *ty_arena_new(void) {
	TypeArena *a = calloc(1, sizeof(TypeArena));
	/* Reserve TypeId 0 by placing a sentinel UNKNOWN node at index 0. Internally
	 * we'll still index nodes 1..N as id 1..N. */
	a->node_cap = 32;
	a->nodes = malloc(a->node_cap * sizeof(TypeNode));
	a->nodes[0].kind = TYK_UNKNOWN;
	a->node_count = 1;
	return a;
}

void ty_arena_free(TypeArena *a) {
	if (!a)
		return;
	for (int i = 0; i < a->node_count; i++) {
		TypeNode *n = &a->nodes[i];
		if (n->kind == TYK_TUPLE) {
			free(n->data.tuple.names);
			free(n->data.tuple.types);
		} else if (n->kind == TYK_FUNC || n->kind == TYK_PROC || n->kind == TYK_SYS || n->kind == TYK_POLICY) {
			free(n->data.func.params);
			free(n->data.func.returns);
		}
	}
	free(a->nodes);
	for (int i = 0; i < a->string_count; i++)
		free(a->strings[i]);
	free(a->strings);
	free(a);
}

/* Intern a string: pointer-equal across calls with the same content. */
static const char *intern_str(TypeArena *a, const char *s) {
	if (!s)
		return NULL;
	for (int i = 0; i < a->string_count; i++) {
		if (strcmp(a->strings[i], s) == 0)
			return a->strings[i];
	}
	if (a->string_count == a->string_cap) {
		a->string_cap = a->string_cap ? a->string_cap * 2 : 16;
		a->strings = realloc(a->strings, a->string_cap * sizeof(char *));
	}
	char *copy = malloc(strlen(s) + 1);
	strcpy(copy, s);
	a->strings[a->string_count++] = copy;
	return copy;
}

static TypeId push_node(TypeArena *a, TypeNode node) {
	if (a->node_count == a->node_cap) {
		a->node_cap *= 2;
		a->nodes = realloc(a->nodes, a->node_cap * sizeof(TypeNode));
	}
	int id = a->node_count++;
	a->nodes[id] = node;
	return (TypeId)id;
}

TypeId tyid_of_prim(TypeArena *a, PrimKind p) {
	for (int i = 1; i < a->node_count; i++) {
		TypeNode *n = &a->nodes[i];
		if (n->kind == TYK_PRIM && n->data.prim == p)
			return (TypeId)i;
	}
	TypeNode node = {0};
	node.kind = TYK_PRIM;
	node.data.prim = p;
	return push_node(a, node);
}

TypeId tyid_of_nominal_sub(TypeArena *a, const char *name, TypeId backing) {
	const char *interned = intern_str(a, name);
	for (int i = 1; i < a->node_count; i++) {
		TypeNode *n = &a->nodes[i];
		if (n->kind == TYK_NOMINAL && n->data.nominal.name == interned && n->data.nominal.backing == backing)
			return (TypeId)i;
	}
	TypeNode node = {0};
	node.kind = TYK_NOMINAL;
	node.data.nominal.name = interned;
	node.data.nominal.backing = backing;
	return push_node(a, node);
}

TypeId tyid_of_nominal(TypeArena *a, const char *name) {
	return tyid_of_nominal_sub(a, name, TYID_UNKNOWN);
}

/* The backing TypeId of a tier-2 distinct subtype, or TYID_UNKNOWN for a prim / standalone nominal /
 * non-nominal. */
TypeId tyid_backing(const TypeArena *a, TypeId t) {
	if (!a || t == 0 || (int)t >= a->node_count)
		return TYID_UNKNOWN;
	const TypeNode *n = &a->nodes[t];
	return n->kind == TYK_NOMINAL ? n->data.nominal.backing : TYID_UNKNOWN;
}

/* Is a value of type `from` usable where `to` is expected? `from == to`, or `from` is a distinct
 * subtype whose backing chain reaches `to` (one-way: `meters` usable as `float`, not vice versa). */
int tyid_usable_as(const TypeArena *a, TypeId from, TypeId to) {
	for (TypeId t = from; t != TYID_UNKNOWN; t = tyid_backing(a, t))
		if (t == to)
			return 1;
	return 0;
}

TypeId tyid_of_array(TypeArena *a, TypeId elem) {
	for (int i = 1; i < a->node_count; i++) {
		TypeNode *n = &a->nodes[i];
		if (n->kind == TYK_ARRAY && n->data.array.elem == elem)
			return (TypeId)i;
	}
	TypeNode node = {0};
	node.kind = TYK_ARRAY;
	node.data.array.elem = elem;
	return push_node(a, node);
}

TypeId tyid_of_shaped(TypeArena *a, TypeId elem, int rank) {
	for (int i = 1; i < a->node_count; i++) {
		TypeNode *n = &a->nodes[i];
		if (n->kind == TYK_SHAPED_ARRAY && n->data.shaped.elem == elem && n->data.shaped.rank == rank)
			return (TypeId)i;
	}
	TypeNode node = {0};
	node.kind = TYK_SHAPED_ARRAY;
	node.data.shaped.elem = elem;
	node.data.shaped.rank = rank;
	return push_node(a, node);
}

TypeId tyid_of_tuple(TypeArena *a, const char *const *field_names, const TypeId *field_types, int field_count) {
	const char **interned_names = malloc(field_count * sizeof(char *));
	for (int i = 0; i < field_count; i++)
		interned_names[i] = field_names ? intern_str(a, field_names[i]) : NULL;

	for (int i = 1; i < a->node_count; i++) {
		TypeNode *n = &a->nodes[i];
		if (n->kind != TYK_TUPLE || n->data.tuple.count != field_count)
			continue;
		int eq = 1;
		for (int j = 0; j < field_count; j++) {
			if (n->data.tuple.types[j] != field_types[j] || n->data.tuple.names[j] != interned_names[j]) {
				eq = 0;
				break;
			}
		}
		if (eq) {
			free(interned_names);
			return (TypeId)i;
		}
	}

	TypeNode node = {0};
	node.kind = TYK_TUPLE;
	node.data.tuple.count = field_count;
	node.data.tuple.names = interned_names;
	node.data.tuple.types = malloc(field_count * sizeof(TypeId));
	memcpy(node.data.tuple.types, field_types, field_count * sizeof(TypeId));
	return push_node(a, node);
}

TypeId tyid_of_handle(TypeArena *a, const char *archetype_name) {
	const char *interned = intern_str(a, archetype_name);
	for (int i = 1; i < a->node_count; i++) {
		TypeNode *n = &a->nodes[i];
		if (n->kind == TYK_HANDLE && n->data.handle.archetype_name == interned)
			return (TypeId)i;
	}
	TypeNode node = {0};
	node.kind = TYK_HANDLE;
	node.data.handle.archetype_name = interned;
	return push_node(a, node);
}

TypeId tyid_of_archetype_category(TypeArena *a) {
	for (int i = 1; i < a->node_count; i++) {
		if (a->nodes[i].kind == TYK_ARCHETYPE_CATEGORY)
			return (TypeId)i;
	}
	TypeNode node = {0};
	node.kind = TYK_ARCHETYPE_CATEGORY;
	return push_node(a, node);
}

/* Hash-cons a callable node of `kind` (TYK_FUNC/PROC/SYS/POLICY). Dedup keys on the KIND, so a proc and
 * func with identical (params, returns) intern to DIFFERENT ids — structural inequality, no flag. */
static TypeId intern_callable(TypeArena *a, TyKind kind, const TypeId *params, int param_count, const TypeId *returns,
                              int return_count) {
	for (int i = 1; i < a->node_count; i++) {
		TypeNode *n = &a->nodes[i];
		if (n->kind != kind || n->data.func.param_count != param_count || n->data.func.return_count != return_count)
			continue;
		int eq = 1;
		for (int j = 0; j < param_count && eq; j++)
			if (n->data.func.params[j] != params[j])
				eq = 0;
		for (int j = 0; j < return_count && eq; j++)
			if (n->data.func.returns[j] != returns[j])
				eq = 0;
		if (eq)
			return (TypeId)i;
	}
	TypeNode node = {0};
	node.kind = kind;
	node.data.func.param_count = param_count;
	node.data.func.return_count = return_count;
	node.data.func.params = malloc(param_count * sizeof(TypeId));
	memcpy(node.data.func.params, params, param_count * sizeof(TypeId));
	node.data.func.returns = malloc(return_count * sizeof(TypeId));
	memcpy(node.data.func.returns, returns, return_count * sizeof(TypeId));
	return push_node(a, node);
}

TypeId tyid_of_func(TypeArena *a, const TypeId *params, int param_count, const TypeId *returns, int return_count) {
	return intern_callable(a, TYK_FUNC, params, param_count, returns, return_count);
}
TypeId tyid_of_proc(TypeArena *a, const TypeId *params, int param_count, const TypeId *returns, int return_count) {
	return intern_callable(a, TYK_PROC, params, param_count, returns, return_count);
}
TypeId tyid_of_sys(TypeArena *a, const TypeId *params, int param_count) {
	return intern_callable(a, TYK_SYS, params, param_count, NULL, 0);
}
TypeId tyid_of_policy(TypeArena *a, const TypeId *params, int param_count) {
	return intern_callable(a, TYK_POLICY, params, param_count, NULL, 0);
}

TyKind tyid_kind(const TypeArena *a, TypeId t) {
	if (!a || t == 0 || (int)t >= a->node_count)
		return TYK_UNKNOWN;
	return a->nodes[t].kind;
}

int tyid_is_proc(const TypeArena *a, TypeId t) {
	return a && t != 0 && (int)t < a->node_count && a->nodes[t].kind == TYK_PROC;
}

int tyid_is_callable(const TypeArena *a, TypeId t) {
	if (!a || t == 0 || (int)t >= a->node_count)
		return 0;
	TyKind k = a->nodes[t].kind;
	return k == TYK_FUNC || k == TYK_PROC;
}

PrimKind tyid_prim(const TypeArena *a, TypeId t) {
	if (!a || t == 0 || (int)t >= a->node_count || a->nodes[t].kind != TYK_PRIM)
		return PRIM_COUNT;
	return a->nodes[t].data.prim;
}

const char *tyid_nominal_name(const TypeArena *a, TypeId t) {
	if (!a || t == 0 || (int)t >= a->node_count || a->nodes[t].kind != TYK_NOMINAL)
		return NULL;
	return a->nodes[t].data.nominal.name;
}

const char *tyid_handle_name(const TypeArena *a, TypeId t) {
	if (!a || t == 0 || (int)t >= a->node_count || a->nodes[t].kind != TYK_HANDLE)
		return NULL;
	return a->nodes[t].data.handle.archetype_name;
}

TypeId tyid_elem(const TypeArena *a, TypeId t) {
	if (!a || t == 0 || (int)t >= a->node_count)
		return TYID_UNKNOWN;
	const TypeNode *n = &a->nodes[t];
	if (n->kind == TYK_ARRAY)
		return n->data.array.elem;
	if (n->kind == TYK_SHAPED_ARRAY)
		return n->data.shaped.elem;
	return TYID_UNKNOWN;
}

int tyid_shaped_rank(const TypeArena *a, TypeId t) {
	if (!a || t == 0 || (int)t >= a->node_count)
		return -1;
	const TypeNode *n = &a->nodes[t];
	return n->kind == TYK_SHAPED_ARRAY ? n->data.shaped.rank : -1;
}

int tyid_tuple_count(const TypeArena *a, TypeId t) {
	if (!a || t == 0 || (int)t >= a->node_count || a->nodes[t].kind != TYK_TUPLE)
		return 0;
	return a->nodes[t].data.tuple.count;
}

const char *tyid_tuple_field_name(const TypeArena *a, TypeId t, int i) {
	if (!a || t == 0 || (int)t >= a->node_count || a->nodes[t].kind != TYK_TUPLE || i < 0 ||
	    i >= a->nodes[t].data.tuple.count)
		return NULL;
	return a->nodes[t].data.tuple.names[i];
}

TypeId tyid_tuple_field_type(const TypeArena *a, TypeId t, int i) {
	if (!a || t == 0 || (int)t >= a->node_count || a->nodes[t].kind != TYK_TUPLE || i < 0 ||
	    i >= a->nodes[t].data.tuple.count)
		return TYID_UNKNOWN;
	return a->nodes[t].data.tuple.types[i];
}

int tyid_equal(TypeId a, TypeId b) {
	return a == b;
}

int tyid_is_unknown(TypeId t) {
	return t == TYID_UNKNOWN;
}

static const char *prim_name(PrimKind p) {
	switch (p) {
	case PRIM_VOID:
		return "void";
	case PRIM_BOOL:
		return "bool";
	case PRIM_INT:
		return "i32"; /* `i32` is the canonical 32-bit integer; `int` is a transparent alias of it */
	case PRIM_FLOAT:
		return "float";
	case PRIM_CHAR:
		return "char";
	case PRIM_STR:
		return "str";
	default:
		return "?";
	}
}

const char *tyid_display(const TypeArena *a, TypeId t, char *buf, int buflen) {
	if (!a || t == 0 || (int)t >= a->node_count) {
		snprintf(buf, buflen, "<unknown>");
		return buf;
	}
	const TypeNode *n = &a->nodes[t];
	switch (n->kind) {
	case TYK_UNKNOWN:
		snprintf(buf, buflen, "<unknown>");
		break;
	case TYK_PRIM:
		snprintf(buf, buflen, "%s", prim_name(n->data.prim));
		break;
	case TYK_NOMINAL:
		snprintf(buf, buflen, "%s", n->data.nominal.name ? n->data.nominal.name : "?");
		break;
	case TYK_ARRAY: {
		char inner[128];
		tyid_display(a, n->data.array.elem, inner, sizeof(inner));
		snprintf(buf, buflen, "[]%s", inner);
		break;
	}
	case TYK_SHAPED_ARRAY: {
		char inner[128];
		tyid_display(a, n->data.shaped.elem, inner, sizeof(inner));
		snprintf(buf, buflen, "[%d]%s", n->data.shaped.rank, inner);
		break;
	}
	case TYK_TUPLE:
		snprintf(buf, buflen, "tuple(%d)", n->data.tuple.count);
		break;
	case TYK_HANDLE:
		snprintf(buf, buflen, "handle(%s)", n->data.handle.archetype_name ? n->data.handle.archetype_name : "?");
		break;
	case TYK_ARCHETYPE_CATEGORY:
		snprintf(buf, buflen, "archetype");
		break;
	case TYK_FUNC:
	case TYK_PROC:
	case TYK_SYS:
	case TYK_POLICY: {
		/* Render each callable form in Arche's own spelling — `func(T, U) -> R`, `proc(T)(A)`,
		 * `sys(T)`, `policy(T)` — with the real param/return types (cf. Odin/Jai showing the types). */
		char ps[400] = "";
		for (int i = 0; i < n->data.func.param_count; i++) {
			char one[128];
			tyid_display(a, n->data.func.params[i], one, sizeof(one));
			size_t l = strlen(ps);
			snprintf(ps + l, sizeof(ps) - l, "%s%s", i ? ", " : "", one);
		}
		if (n->kind == TYK_PROC) {
			char rs[400] = "";
			for (int i = 0; i < n->data.func.return_count; i++) {
				char one[128];
				tyid_display(a, n->data.func.returns[i], one, sizeof(one));
				size_t l = strlen(rs);
				snprintf(rs + l, sizeof(rs) - l, "%s%s", i ? ", " : "", one);
			}
			snprintf(buf, buflen, "proc(%s)(%s)", ps, rs);
		} else if (n->kind == TYK_FUNC) {
			char rs[128] = "void";
			if (n->data.func.return_count >= 1)
				tyid_display(a, n->data.func.returns[0], rs, sizeof(rs));
			snprintf(buf, buflen, "func(%s) -> %s", ps, rs);
		} else {
			snprintf(buf, buflen, "%s(%s)", n->kind == TYK_SYS ? "sys" : "policy", ps);
		}
		break;
	}
	}
	return buf;
}
