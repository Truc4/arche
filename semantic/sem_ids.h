#ifndef SEM_IDS_H
#define SEM_IDS_H

/* DefId — the single, stable identity of a declaration.
 *
 * Every analysis/reachability/codegen decision keys on a DefId, never on a string name (names exist
 * only for display + diagnostics). A DefId is `(unit, index)`: which compilation unit owns the decl,
 * and its index within that unit's decl table. Today there is one unit (the whole inlined program),
 * so `unit` is always 0 and `index` is the position in `ctx->decls`; the field exists now so the
 * per-unit split (Phase 1) is a data change, not an API change. See the compilation-model plan.
 *
 * `DEFID_NONE` marks "resolved to nothing in the program" — a call/ref whose target is an extern,
 * a libc symbol, a builtin, or simply unresolved. Code that consumes a DefId must treat NONE as
 * "no in-program edge", never index with it. */

#include <stdint.h>

typedef struct {
	int32_t unit;
	int32_t index;
} DefId;

static inline DefId defid_make(int32_t unit, int32_t index) {
	return (DefId){unit, index};
}

static inline DefId defid_none(void) {
	return (DefId){-1, -1};
}

static inline int defid_is_none(DefId d) {
	return d.index < 0;
}

static inline int defid_eq(DefId a, DefId b) {
	return a.unit == b.unit && a.index == b.index;
}

/* Provenance of a decl's owning unit — stamped from the search root the module loader matched (NOT
 * recovered from a path string), so it is correct by construction across install layouts. ENTRY is the
 * zero value (the root program being compiled). Dependencies (STDLIB/CORE) are never dead-code-linted;
 * a USER_MODULE's private decls are. */
typedef enum {
	DECL_ORIGIN_ENTRY,       /* the root/entry program */
	DECL_ORIGIN_USER_MODULE, /* a module from the user's own source tree */
	DECL_ORIGIN_STDLIB,      /* a bundled stdlib module */
	DECL_ORIGIN_CORE,        /* a core module (the prelude itself is prepended text, not a module) */
} DeclOrigin;

#endif /* SEM_IDS_H */
