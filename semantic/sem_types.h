#ifndef SEM_TYPES_H
#define SEM_TYPES_H

/* Type intern table: TypeId (u32) names a structural type interned in a
 * per-compilation arena. Equality is `a == b` on TypeId. The arena owns all
 * strings the data refers to (e.g. archetype names, opaque nominal names).
 *
 * This is the source-of-truth representation for types in the typechecker.
 * The string-based `sem_model_expr_type` table coexists during Phase B
 * migration and will be retired once codegen reads tyid directly.
 *
 * Design notes:
 * - TypeId 0 is reserved as the "unknown" sentinel. Fail-open during build-out:
 *   `check(e, expected)` matches anything against `tyid_unknown` so missing
 *   rules don't generate noise.
 * - Hash-consing is structural: identical (kind, payload) tuples intern once.
 *   Two `tyid_of_slice(arena, Int)` calls return the same TypeId.
 * - Function types (`tyid_of_func`) carry param + return TypeId arrays; the
 *   arrays are owned by the arena. */

#include <stdint.h>

typedef uint32_t TypeId;

#define TYID_UNKNOWN ((TypeId)0)

/* Primitive kinds the arena knows by construction. Anything else is named
 * (TYK_NOMINAL) — archetype names, type aliases, opaque tags. */
typedef enum { PRIM_VOID, PRIM_BOOL, PRIM_INT, PRIM_FLOAT, PRIM_CHAR, PRIM_STR, PRIM_COUNT } PrimKind;

typedef enum {
	TYK_UNKNOWN, /* sentinel — id 0 */
	TYK_PRIM,
	TYK_NOMINAL, /* archetype names, type aliases, opaque tags */
	TYK_SLICE,
	TYK_ARRAY,
	TYK_TUPLE,
	TYK_HANDLE,
	TYK_ARCHETYPE_CATEGORY, /* the bare `archetype` keyword (map param only) */
	/* The callable FORMS are DISTINCT kinds — Arche's whole identity is that func (pure value), proc
	 * (action), map (transform), and policy (failure macro) are not the same thing. They never unify. */
	TYK_FUNC,  /* func: (params) -> return — a pure value */
	TYK_PROC,  /* proc: (in)(out) — an action */
	TYK_SYS,   /* map: (components) — a data transform */
	TYK_POLICY /* policy: (operands) — a failure-handling macro */
} TyKind;

typedef struct TypeArena TypeArena;

TypeArena *ty_arena_new(void);
void ty_arena_free(TypeArena *a);

/* Constructors. All deterministic / hash-consed. */
TypeId tyid_of_prim(TypeArena *a, PrimKind p);
TypeId tyid_of_nominal(TypeArena *a, const char *name); /* standalone nominal (backing = unknown) */
/* A tier-2 DISTINCT subtype: its own identity, one-way usable AS `backing`. A tier-1 transparent
 * alias must NOT use this — intern it directly as its backing's TypeId so tyid_equal collapses. */
TypeId tyid_of_nominal_sub(TypeArena *a, const char *name, TypeId backing);
TypeId tyid_of_slice(TypeArena *a, TypeId elem);
TypeId tyid_of_array(TypeArena *a, TypeId elem, int rank);
TypeId tyid_of_tuple(TypeArena *a, const char *const *field_names, const TypeId *field_types, int field_count);
TypeId tyid_of_handle(TypeArena *a, const char *archetype_name);
TypeId tyid_of_archetype_category(TypeArena *a);
/* The three callable forms, each its own distinct kind. `returns` is a func's single return, a proc's
 * out-params, or a map's (none). Structural inequality (`proc()(int) != func()->int`) is automatic —
 * a different kind interns to a different id. */
TypeId tyid_of_func(TypeArena *a, const TypeId *params, int param_count, const TypeId *returns, int return_count);
TypeId tyid_of_proc(TypeArena *a, const TypeId *params, int param_count, const TypeId *returns, int return_count);
TypeId tyid_of_map(TypeArena *a, const TypeId *params, int param_count);
TypeId tyid_of_policy(TypeArena *a, const TypeId *params, int param_count);

/* Inspection. */
TyKind tyid_kind(const TypeArena *a, TypeId t);
int tyid_is_proc(const TypeArena *a, TypeId t);              /* 1 if t is a TYK_PROC */
int tyid_is_callable(const TypeArena *a, TypeId t);          /* 1 if t is a func OR proc (a first-class callable) */
PrimKind tyid_prim(const TypeArena *a, TypeId t);            /* the PrimKind of a TYK_PRIM, else PRIM_COUNT */
const char *tyid_nominal_name(const TypeArena *a, TypeId t); /* a TYK_NOMINAL's interned name, else NULL */
const char *tyid_handle_name(const TypeArena *a, TypeId t);  /* a TYK_HANDLE's archetype name, else NULL */
TypeId tyid_elem(const TypeArena *a, TypeId t);              /* an array/shaped element type, else UNKNOWN */
int tyid_array_len(const TypeArena *a, TypeId t);            /* a shaped array's static length, else -1 */
int tyid_tuple_count(const TypeArena *a, TypeId t);
const char *tyid_tuple_field_name(const TypeArena *a, TypeId t, int i);
TypeId tyid_tuple_field_type(const TypeArena *a, TypeId t, int i);
int tyid_equal(TypeId a, TypeId b); /* same interned id */
int tyid_is_unknown(TypeId t);
/* The backing of a tier-2 distinct subtype (else TYID_UNKNOWN). */
TypeId tyid_backing(const TypeArena *a, TypeId t);
/* Is `from` usable where `to` is expected? `from == to`, or `from`'s backing chain reaches `to`. */
int tyid_usable_as(const TypeArena *a, TypeId from, TypeId to);

/* Display: writes a human-friendly type string into buf (truncates safely).
 * Returns buf for chaining. Used by E0200 type_mismatch and friends. */
const char *tyid_display(const TypeArena *a, TypeId t, char *buf, int buflen);

#endif /* SEM_TYPES_H */
