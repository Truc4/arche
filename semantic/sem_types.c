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
		} slice;
		struct {
			TypeId elem;
			int len;
		} array;
		struct {
			const char **names; /* interned in str pool */
			TypeId *types;
			int count;
		} tuple;
		struct {
			const char *name;            /* interned */
			const char **variant_names;  /* interned; [variant_count] */
			TypeId **variant_payloads;   /* [variant_count][payload_counts[v]] */
			int *variant_payload_counts; /* [variant_count] */
			int variant_count;
			int complete; /* 0 until tyid_sum_complete fills the variants */
		} sum;
		struct {
			const char *archetype_name;
		} handle;
		struct {
			const char *extern_name; /* interned; the under-applied extern's name, or NULL for a structural
			                          * annotation `Eff(T…)` matched only on out-slots */
			TypeId *out_slots;       /* [out_slot_count] — the types yielded when run */
			const char **out_slot_names; /* [out_slot_count] — interned NAME per out-slot (a named parameter,
			                              * not an anonymous return); NULL entry = unnamed. The names are
			                              * intrinsic to the Eff (declarable as `Eff(buf: T, …)` or inferred
			                              * from the constructing extern's out-params). */
			int out_slot_count;
		} eff;
		struct {
			TypeId *params;
			int param_count;
			TypeId *returns; /* func: single return; proc: out-params; map/policy: none */
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
		} else if (n->kind == TYK_SUM) {
			for (int v = 0; v < n->data.sum.variant_count; v++)
				free(n->data.sum.variant_payloads[v]);
			free(n->data.sum.variant_payloads);
			free(n->data.sum.variant_payload_counts);
			free(n->data.sum.variant_names);
		} else if (n->kind == TYK_FUNC || n->kind == TYK_PROC || n->kind == TYK_MAP || n->kind == TYK_POLICY) {
			free(n->data.func.params);
			free(n->data.func.returns);
		} else if (n->kind == TYK_EFF) {
			free(n->data.eff.out_slots);
			free(n->data.eff.out_slot_names); /* the name strings are interned (arena-owned); free only the array */
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
	/* A concrete `Eff#extern(out…)` is usable as the STRUCTURAL annotation `Eff(out…)` with the same
	 * out-slots: the build site mints the concrete one, while a func's declared `-> Eff(int,int)` is
	 * structural (extern_name NULL). A structural target accepts any extern; a concrete target requires
	 * the same extern. (Out-slots compared by id; arche has no Eff subtyping beyond this.) */
	if (a && tyid_kind(a, from) == TYK_EFF && tyid_kind(a, to) == TYK_EFF) {
		int nf = tyid_eff_out_count(a, from);
		if (nf == tyid_eff_out_count(a, to)) {
			int eq = 1;
			for (int i = 0; i < nf && eq; i++)
				if (tyid_eff_out_at(a, from, i) != tyid_eff_out_at(a, to, i))
					eq = 0;
			const char *te = tyid_eff_extern_name(a, to);
			if (eq && (te == NULL || te == tyid_eff_extern_name(a, from)))
				return 1;
		}
	}
	for (TypeId t = from; t != TYID_UNKNOWN; t = tyid_backing(a, t))
		if (t == to)
			return 1;
	return 0;
}

TypeId tyid_of_slice(TypeArena *a, TypeId elem) {
	for (int i = 1; i < a->node_count; i++) {
		TypeNode *n = &a->nodes[i];
		if (n->kind == TYK_SLICE && n->data.slice.elem == elem)
			return (TypeId)i;
	}
	TypeNode node = {0};
	node.kind = TYK_SLICE;
	node.data.slice.elem = elem;
	return push_node(a, node);
}

TypeId tyid_of_array(TypeArena *a, TypeId elem, int rank) {
	for (int i = 1; i < a->node_count; i++) {
		TypeNode *n = &a->nodes[i];
		if (n->kind == TYK_ARRAY && n->data.array.elem == elem && n->data.array.len == rank)
			return (TypeId)i;
	}
	TypeNode node = {0};
	node.kind = TYK_ARRAY;
	node.data.array.elem = elem;
	node.data.array.len = rank;
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

TypeId tyid_sum_forward(TypeArena *a, const char *name) {
	const char *interned = intern_str(a, name);
	for (int i = 1; i < a->node_count; i++) {
		TypeNode *n = &a->nodes[i];
		if (n->kind == TYK_SUM && n->data.sum.name == interned)
			return (TypeId)i; /* already forwarded/completed under this name */
	}
	TypeNode node = {0};
	node.kind = TYK_SUM;
	node.data.sum.name = interned;
	node.data.sum.complete = 0;
	return push_node(a, node);
}

void tyid_sum_complete(TypeArena *a, TypeId sum, const char *const *variant_names,
                       const TypeId *const *variant_payloads, const int *variant_payload_counts, int variant_count) {
	if (!a || sum == 0 || (int)sum >= a->node_count)
		return;
	TypeNode *n = &a->nodes[sum];
	if (n->kind != TYK_SUM || n->data.sum.complete)
		return;
	n->data.sum.variant_count = variant_count;
	n->data.sum.variant_names = malloc((variant_count ? variant_count : 1) * sizeof(char *));
	n->data.sum.variant_payloads = malloc((variant_count ? variant_count : 1) * sizeof(TypeId *));
	n->data.sum.variant_payload_counts = malloc((variant_count ? variant_count : 1) * sizeof(int));
	for (int v = 0; v < variant_count; v++) {
		n->data.sum.variant_names[v] = intern_str(a, variant_names[v]);
		int pc = variant_payload_counts[v];
		n->data.sum.variant_payload_counts[v] = pc;
		n->data.sum.variant_payloads[v] = malloc((pc ? pc : 1) * sizeof(TypeId));
		for (int i = 0; i < pc; i++)
			n->data.sum.variant_payloads[v][i] = variant_payloads[v][i];
	}
	n->data.sum.complete = 1;
}

const char *tyid_sum_name(const TypeArena *a, TypeId t) {
	if (!a || t == 0 || (int)t >= a->node_count || a->nodes[t].kind != TYK_SUM)
		return NULL;
	return a->nodes[t].data.sum.name;
}

int tyid_sum_variant_count(const TypeArena *a, TypeId t) {
	if (!a || t == 0 || (int)t >= a->node_count || a->nodes[t].kind != TYK_SUM)
		return -1;
	return a->nodes[t].data.sum.variant_count;
}

int tyid_sum_variant_index(const TypeArena *a, TypeId t, const char *nm) {
	if (!a || t == 0 || (int)t >= a->node_count || a->nodes[t].kind != TYK_SUM || !nm)
		return -1;
	const TypeNode *n = &a->nodes[t];
	for (int v = 0; v < n->data.sum.variant_count; v++)
		if (strcmp(n->data.sum.variant_names[v], nm) == 0)
			return v;
	return -1;
}

const char *tyid_sum_variant_name(const TypeArena *a, TypeId t, int v) {
	if (!a || t == 0 || (int)t >= a->node_count || a->nodes[t].kind != TYK_SUM)
		return NULL;
	const TypeNode *n = &a->nodes[t];
	if (v < 0 || v >= n->data.sum.variant_count)
		return NULL;
	return n->data.sum.variant_names[v];
}

int tyid_sum_variant_payload_count(const TypeArena *a, TypeId t, int v) {
	if (!a || t == 0 || (int)t >= a->node_count || a->nodes[t].kind != TYK_SUM)
		return -1;
	const TypeNode *n = &a->nodes[t];
	if (v < 0 || v >= n->data.sum.variant_count)
		return -1;
	return n->data.sum.variant_payload_counts[v];
}

TypeId tyid_sum_variant_payload_at(const TypeArena *a, TypeId t, int v, int i) {
	if (!a || t == 0 || (int)t >= a->node_count || a->nodes[t].kind != TYK_SUM)
		return TYID_UNKNOWN;
	const TypeNode *n = &a->nodes[t];
	if (v < 0 || v >= n->data.sum.variant_count || i < 0 || i >= n->data.sum.variant_payload_counts[v])
		return TYID_UNKNOWN;
	return n->data.sum.variant_payloads[v][i];
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

/* Hash-cons an Eff node. Keyed on (extern_name, out_slots) so a structural `Eff(int,int)` (extern_name
 * NULL) and a concrete `Eff#fwrite(int,int)` are DISTINCT ids — the run site uses the concrete extern,
 * while a func's declared `-> Eff(int,int)` annotation is the structural one (checked on out-slots only). */
static TypeId intern_eff(TypeArena *a, const char *extern_name, const TypeId *out_slots, const char *const *names,
                         int out_slot_count) {
	const char *interned = extern_name ? intern_str(a, extern_name) : NULL;
	/* Intern each name up front so the hash-cons key and storage use pointer identity. */
	const char *iname[64];
	int has_name = 0;
	for (int j = 0; j < out_slot_count; j++) {
		const char *nm = (names && j < out_slot_count) ? names[j] : NULL;
		iname[j < 64 ? j : 63] = nm ? intern_str(a, nm) : NULL;
		if (nm)
			has_name = 1;
	}
	for (int i = 1; i < a->node_count; i++) {
		TypeNode *n = &a->nodes[i];
		if (n->kind != TYK_EFF || n->data.eff.extern_name != interned || n->data.eff.out_slot_count != out_slot_count)
			continue;
		int eq = 1;
		for (int j = 0; j < out_slot_count && eq; j++)
			if (n->data.eff.out_slots[j] != out_slots[j])
				eq = 0;
		/* Names are part of the carried identity: two Effs with the same slot TYPES but different slot
		 * NAMES are distinct nodes (each carries its own names). Assignability ignores names (see
		 * tyid_assignable), so this does not affect structural compatibility. */
		for (int j = 0; j < out_slot_count && eq && j < 64; j++) {
			const char *en = n->data.eff.out_slot_names ? n->data.eff.out_slot_names[j] : NULL;
			if (en != iname[j])
				eq = 0;
		}
		if (eq)
			return (TypeId)i;
	}
	TypeNode node = {0};
	node.kind = TYK_EFF;
	node.data.eff.extern_name = interned;
	node.data.eff.out_slot_count = out_slot_count;
	node.data.eff.out_slots = malloc((out_slot_count ? out_slot_count : 1) * sizeof(TypeId));
	if (out_slot_count)
		memcpy(node.data.eff.out_slots, out_slots, out_slot_count * sizeof(TypeId));
	node.data.eff.out_slot_names = NULL;
	if (has_name) {
		node.data.eff.out_slot_names = malloc((out_slot_count ? out_slot_count : 1) * sizeof(const char *));
		for (int j = 0; j < out_slot_count; j++)
			node.data.eff.out_slot_names[j] = (j < 64) ? iname[j] : NULL;
	}
	return push_node(a, node);
}

TypeId tyid_of_eff_structural(TypeArena *a, const TypeId *out_slots, int out_slot_count) {
	return intern_eff(a, NULL, out_slots, NULL, out_slot_count);
}
TypeId tyid_of_eff_concrete(TypeArena *a, const char *extern_name, const TypeId *out_slots, int out_slot_count) {
	return intern_eff(a, extern_name, out_slots, NULL, out_slot_count);
}
/* Like the above but carries an explicit NAME per out-slot (a named parameter). `names[j]` may be NULL for
 * an unnamed slot. `extern_name` NULL = structural (a declared `Eff(buf: T, …)` annotation). */
TypeId tyid_of_eff_named(TypeArena *a, const char *extern_name, const TypeId *out_slots, const char *const *names,
                         int out_slot_count) {
	return intern_eff(a, extern_name, out_slots, names, out_slot_count);
}
const char *tyid_eff_out_name_at(const TypeArena *a, TypeId t, int i) {
	if (!a || t == 0 || (int)t >= a->node_count || a->nodes[t].kind != TYK_EFF)
		return NULL;
	if (i < 0 || i >= a->nodes[t].data.eff.out_slot_count || !a->nodes[t].data.eff.out_slot_names)
		return NULL;
	return a->nodes[t].data.eff.out_slot_names[i];
}
const char *tyid_eff_extern_name(const TypeArena *a, TypeId t) {
	if (!a || t == 0 || (int)t >= a->node_count || a->nodes[t].kind != TYK_EFF)
		return NULL;
	return a->nodes[t].data.eff.extern_name;
}
int tyid_eff_out_count(const TypeArena *a, TypeId t) {
	if (!a || t == 0 || (int)t >= a->node_count || a->nodes[t].kind != TYK_EFF)
		return -1;
	return a->nodes[t].data.eff.out_slot_count;
}
TypeId tyid_eff_out_at(const TypeArena *a, TypeId t, int i) {
	if (!a || t == 0 || (int)t >= a->node_count || a->nodes[t].kind != TYK_EFF)
		return TYID_UNKNOWN;
	if (i < 0 || i >= a->nodes[t].data.eff.out_slot_count)
		return TYID_UNKNOWN;
	return a->nodes[t].data.eff.out_slots[i];
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
	if (param_count) /* a 0-count memcpy from a NULL src is UB (no-arg proc/func) — guard it */
		memcpy(node.data.func.params, params, param_count * sizeof(TypeId));
	node.data.func.returns = malloc(return_count * sizeof(TypeId));
	if (return_count)
		memcpy(node.data.func.returns, returns, return_count * sizeof(TypeId));
	return push_node(a, node);
}

TypeId tyid_of_func(TypeArena *a, const TypeId *params, int param_count, const TypeId *returns, int return_count) {
	return intern_callable(a, TYK_FUNC, params, param_count, returns, return_count);
}
TypeId tyid_of_proc(TypeArena *a, const TypeId *params, int param_count, const TypeId *returns, int return_count) {
	return intern_callable(a, TYK_PROC, params, param_count, returns, return_count);
}
TypeId tyid_of_map(TypeArena *a, const TypeId *params, int param_count) {
	return intern_callable(a, TYK_MAP, params, param_count, NULL, 0);
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
	if (n->kind == TYK_SLICE)
		return n->data.slice.elem;
	if (n->kind == TYK_ARRAY)
		return n->data.array.elem;
	return TYID_UNKNOWN;
}

int tyid_array_len(const TypeArena *a, TypeId t) {
	if (!a || t == 0 || (int)t >= a->node_count)
		return -1;
	const TypeNode *n = &a->nodes[t];
	return n->kind == TYK_ARRAY ? n->data.array.len : -1;
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
	case TYK_SLICE: {
		char inner[128];
		tyid_display(a, n->data.slice.elem, inner, sizeof(inner));
		snprintf(buf, buflen, "[]%s", inner);
		break;
	}
	case TYK_ARRAY: {
		char inner[128];
		tyid_display(a, n->data.array.elem, inner, sizeof(inner));
		snprintf(buf, buflen, "[%d]%s", n->data.array.len, inner);
		break;
	}
	case TYK_TUPLE:
		snprintf(buf, buflen, "tuple(%d)", n->data.tuple.count);
		break;
	case TYK_SUM:
		snprintf(buf, buflen, "%s", n->data.sum.name ? n->data.sum.name : "sum");
		break;
	case TYK_HANDLE:
		snprintf(buf, buflen, "handle(%s)", n->data.handle.archetype_name ? n->data.handle.archetype_name : "?");
		break;
	case TYK_EFF: {
		char os[300] = "";
		for (int i = 0; i < n->data.eff.out_slot_count; i++) {
			char one[128];
			tyid_display(a, n->data.eff.out_slots[i], one, sizeof(one));
			size_t l = strlen(os);
			snprintf(os + l, sizeof(os) - l, "%s%s", i ? ", " : "", one);
		}
		snprintf(buf, buflen, "Eff(%s)", os);
		break;
	}
	case TYK_ARCHETYPE_CATEGORY:
		snprintf(buf, buflen, "archetype");
		break;
	case TYK_FUNC:
	case TYK_PROC:
	case TYK_MAP:
	case TYK_POLICY: {
		/* Render each callable form in Arche's own spelling — `func(T, U) -> R`, `proc(T)(A)`,
		 * `map(T)`, `policy(T)` — with the real param/return types (cf. Odin/Jai showing the types). */
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
			snprintf(buf, buflen, "%s(%s)", n->kind == TYK_MAP ? "map" : "policy", ps);
		}
		break;
	}
	}
	return buf;
}
