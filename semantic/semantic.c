#include "semantic.h"
#include "../parser/parser.h"
#include "../syntax/syntax_view.h"
#include "sem_decls.h"
#include "sem_diag_internal.h"
#include "sem_diagnostics.h"
#include "sem_hints.h"
#include "sem_model.h"
#include "sem_types.h"
#include "tycheck.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* C99 doesn't expose strdup; declare it explicitly (as sem_model.c does). */
char *strdup(const char *s);

/* ========== DATA STRUCTURES ========== */

typedef struct {
	char *name;
	TypeId type_id;
	FieldKind kind;
} FieldInfo;

typedef struct {
	char *signature; /* deterministic key: "field:type:kind;" per field in order */
	FieldInfo **fields;
	int field_count;
	int is_allocated;     /* 1 once any alias for this shape has been alloc'd */
	int alloc_capacity;   /* the driver pool's capacity (rows), 0 if not allocated by the driver */
	int alloc_init_count; /* the pool's guaranteed-live initial count (M in `Arch[N](M)`), 0 if none — the
	                       * sound basis for eliding a column bounds check (count can only grow via insert) */
	int min_rows;         /* max storage REQUIREMENT from device datasheets; the driver pool must meet it */
	int req_count;        /* how many distinct datasheets posted a requirement on this shape (shared-shape) */
	char *req_first;      /* name in the first requirement's decl (for the shared-shape build note) */
	int has_public_def;   /* 1 if any EXPORTED definition names this shape — a storage requirement needs one
	                       * (a driver can only size a shape it can see; a `#module` shape is private) */
} ArchetypeInfo;

typedef struct {
	char *name;
	ArchetypeInfo *archetype;
} AliasEntry;

typedef struct {
	char *name;
	TypeId type_id;
	char *archetype_name;      /* for variables that refer to archetype entries */
	const char *inferred_type; /* for variables without explicit type, stores inferred type name (backing) */
	const char *nominal_type;  /* unresolved nominal alias name (e.g. "file"), or NULL — for distinctness */
	int is_consumed;           /* 1 if a consume-param call / move / return has consumed this binding */
	int is_param;              /* 1 if this is a function parameter (borrowed — exempt from must-consume) */
	int is_own;                /* 1 if an `own` parameter (owned: caller passed it via move/copy, may be mutated) */
	int is_out_place;  /* 1 if a writable out-param slot (out-only, or the out side of an in-out that shadows the in
	                      borrow) */
	int is_const;      /* 1 if an immutable local constant (`k :: e` / `k : T : e`) */
	int is_referenced; /* 1 once any read-site (EXPR_NAME, field-base, etc.) touched this binding */
	int borrows_local; /* 1 if bound from a slice/row of a FRESH LOCAL array — returning it would dangle */
	SourceLoc loc;     /* declaration site, for the must-consume / unused-local diagnostics */
} VariableInfo;

typedef struct {
	VariableInfo **vars;
	int var_count;
} Scope;

typedef struct {
	char *name;     /* group name (owned) */
	char **members; /* member func names (borrowed; pointers into syntax tree FuncGroup) */
	int member_count;
	SourceLoc loc;
} GroupInfo;

/* ParamSummary / FieldSummary / DeclSummary — the resolved per-decl signature side-table that
 * supersedes walking AstProgram. Defined in sem_decls.h (shared with tycheck.c). */

struct SemanticContext {
	ArchetypeInfo **archetypes; /* one per unique shape */
	int archetype_count;

	AliasEntry **aliases; /* one per arche declaration */
	int alias_count;

	char **known_funcs;
	int known_func_count;

	/* Callable aliases: `handler :: some_proc` binds a name to an existing proc/func value
	 * (Stage A of proc/func types). Resolved to the target at call sites; the binding itself
	 * is compile-time (dropped before codegen). Distinct from type aliases and value consts. */
	char **callable_alias_names;
	char **callable_alias_targets;
	int callable_alias_count;

	/* Named callable-type aliases: `Handler :: proc()(w:int)` binds a name to a structural
	 * callable type. tyid_from_name resolves the name to the signature's TypeId. */
	char **ctype_alias_names;
	TypeId *ctype_alias_ids;
	int ctype_alias_count;

	/* Enums: a distinct int-backed type with named variants. enum_type_names lists the enum type
	 * names; the flat (enum, variant, value) triples back variant resolution (`method.get` and bare
	 * variants in `match`). Compile-time only — erased before codegen. */
	char **enum_type_names;
	int enum_type_count;
	char **enum_var_enum;
	char **enum_var_name;
	long *enum_var_value;
	int enum_var_count;

	/* Names of `static` global arrays, registered in a pre-pass (order-independent) so func-purity
	 * can reject a func reading or writing one — a func's only inputs are params + `::` constants. */
	char **static_names;
	int static_name_count;

	GroupInfo *groups;
	int group_count;

	char **const_names;             /* compile-time constant names */
	const char **const_values;      /* literal lexeme strings */
	const char **const_value_types; /* each const's resolved type name ("int"/"float"/"char"/…) */
	SourceLoc *const_locs;          /* loc of each const's declaration */
	int const_count;

	/* Nominal type aliases: `name :: <type>` (a `::` decl whose RHS names a type, not a
	 * literal). Identity is the name; `backing` is the RHS name (possibly another alias —
	 * resolved through the chain by resolve_type_alias). Erased to the backing before
	 * lowering, so codegen never sees an alias. */
	char **type_alias_names;
	char **type_alias_backings;
	int *type_alias_transparent; /* per-alias tier: 1 = transparent (tier-1, == backing), 0 = subtype */
	SourceLoc *type_alias_locs;  /* loc of each alias's declaration, for late diagnostics */
	int type_alias_count;

	/* Opaque destructor registry (RAII): a `@drop` proc binds its `own` opaque param type to
	 * itself; any still-live local of that type gets an auto-drop call at scope exit. One per
	 * type (re-registration is an error). Codegen reads it via semantic_drop_dtor. */
	char **drop_type_names; /* opaque type name (e.g. "file") */
	char **drop_proc_names; /* destructor proc name (e.g. "arche_fclose") */
	SourceLoc *drop_locs;   /* loc of each registration, for the re-registration error */
	int drop_count;

	Scope *scopes;
	int scope_count;

	int error_count;

	/* Track which archetype we're analyzing a sys for (NULL if not in sys) */
	const char *current_sys_archetype;

	/* Track the proc currently being analyzed (NULL if not in a proc body).
	 * Used by each_field to verify its RHS is an `archetype` parameter of this proc. */
	DeclSummary *current_proc;

	/* Track the func currently being analyzed (NULL if not in a func body). Used by STMT_RETURN
	 * to require a value in a func and forbid one in a proc/sys (which use out-params). */
	DeclSummary *current_func;

	/* Track if inside proc/sys body (for alloc enforcement) */
	int in_body;

	/* 1 while analyzing a `sys` body. A `sys` supports no `return` at all (naked or valued). */
	int in_sys;

	/* 1 only while analyzing a call that sits in a statement / bind-RHS position (where an
	 * *action* is allowed). A proc or extern call is an action, not a value, so it may appear
	 * only there — never nested inside another expression. Set by STMT_EXPR / STMT_BIND /
	 * STMT_ASSIGN, captured-and-cleared at the top of EXPR_CALL so the call's own args are
	 * value positions. */
	int stmt_call_ok;
	/* Like stmt_call_ok but set ONLY for the value of an out-list statement (`f(in)(out)` /
	 * multi-bind), NOT a plain `x := f(…)` bind. The mandatory-ok builtins insert/delete are valid
	 * only here, so this distinguishes `insert(P,…)(h:,ok:)` from an illegal `h := insert(…)`. */
	int proc_call_stmt_ok;

	/* AstProgram for looking up declarations */
	/* The resolved decl-signature table — the single source of truth for analysis (with the syntax
	 * tree + SemModel side table). Built once by sem_collect_decls (post rename/qualify) and read by
	 * the decl analyzers, the cross-decl scans, and tycheck. Owned here; freed at context teardown. */
	DeclSummary **decls;
	int decl_count;

	/* Per-unit exported interfaces (one per inlined module), persisted + owned here. Cross-unit
	 * resolution and the qualify pass read these; the registry is dynamic (no module-count cap). */
	UnitInterface **interfaces;
	int interface_count;
	int interface_cap;

	/* Analysis-allocated strings handed BY POINTER to the type-alias registry (alias names + backings
	 * built on the fly during analysis). The registry borrows them for the context's lifetime, so they
	 * are owned here and freed at teardown. (Replaces the transitive ownership the old type pool gave
	 * these strings before Phase 3.) */
	char **owned_strs;
	int owned_str_count, owned_str_cap;

	/* The interned TypeId arena — the type representation (Phase 3), the sole type identity in the
	 * middle of the compiler. Lives for the whole context so analysis can intern types; tycheck shares
	 * it. Freed LAST at teardown (HirType borrows its interned name strings, so it must outlive
	 * lowering+codegen). */
	TypeArena *ty_arena;

	/* Resolved types keyed by syntax tree node id, kept out of the tree.
	 * Populated during analysis; lowering reads it from here. */
	SemModel *model;

	/* Editor-facing inferred facts keyed by syntax tree node id (call-site param resolution).
	 * Populated during analysis; read by the analyzer's explicit view, not by lowering. */
	SemHints *hints;

	/* Structured diagnostics: appended by error_at()/lint_emit()/sem_emit_<slug> so
	 * editors can iterate them after analysis (alongside the existing stderr prints,
	 * which stay for CLI). Each SemDiag is individually heap-allocated so pointers
	 * returned by sem_emit_<slug> wrappers stay valid until semantic_context_free,
	 * even when later emissions grow the array. */
	SemDiag **diags;
	int diag_count, diag_cap;

	/* Active `@allow(<slug>)` suppression set for the currently-analyzed decl.
	 * analyze_*_decl pushes the decl's allow_slugs here on entry and restores the
	 * previous values on exit; sem_emit_<slug> for lints consults this list and
	 * silently drops matching diagnostics. Errors are never suppressible. */
	char **active_allow_slugs;
	int active_allow_slug_count;

	/* True when analyze_expression is invoked directly as a call argument.
	 * EXPR_UNARY MOVE/COPY consults this to detect the E0112 misuse pattern
	 * (`move x` outside any call argument). The flag is captured into a local
	 * at expression-entry and immediately cleared, so recursion into the
	 * operand of a `move x` doesn't see itself as still-in-arg-position. */
	int analyzing_call_arg;
};

/* ========== UTILITY FUNCTIONS ========== */

static int sig_part_cmp(const void *a, const void *b) {
	return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* Archetype identity is the *set* of component type names — unordered. We build one
 * "name:type:kind;" part per field and **sort** them, so `{a,b}` and `{b,a}` produce the
 * same signature and share a shape. (Codegen is keyed by archetype name + per-decl column
 * order, so the sort only affects shape dedup, not memory layout.) */
static const char *sem_tyid_name(SemanticContext *ctx, TypeId t); /* fwd */
static char *compute_shape_signature(SemanticContext *ctx, FieldSummary *fields, int field_count) {
	char **parts = field_count > 0 ? malloc(field_count * sizeof(char *)) : NULL;
	size_t total = 1;
	for (int i = 0; i < field_count; i++) {
		FieldSummary *f = &fields[i];
		const char *type_name = "unknown";
		TyKind k = tyid_kind(ctx->ty_arena, f->type_id);
		if (k == TYK_SLICE)
			type_name = "array";
		else if (k == TYK_ARRAY)
			type_name = "shaped_array";
		else {
			const char *n = sem_tyid_name(ctx, f->type_id);
			if (n)
				type_name = n;
		}
		const char *kind_str = (f->kind == FIELD_META) ? "meta" : "col";
		char part[256];
		snprintf(part, sizeof(part), "%s:%s:%s;", f->name, type_name, kind_str);
		parts[i] = malloc(strlen(part) + 1);
		strcpy(parts[i], part);
		total += strlen(part);
	}
	if (parts)
		qsort(parts, field_count, sizeof(char *), sig_part_cmp);
	char *sig = malloc(total);
	sig[0] = '\0';
	for (int i = 0; i < field_count; i++) {
		strcat(sig, parts[i]);
		free(parts[i]);
	}
	free(parts);
	return sig;
}

static ArchetypeInfo *find_archetype_by_signature(SemanticContext *ctx, const char *sig) {
	for (int i = 0; i < ctx->archetype_count; i++) {
		if (strcmp(ctx->archetypes[i]->signature, sig) == 0)
			return ctx->archetypes[i];
	}
	return NULL;
}

static ArchetypeInfo *find_archetype(SemanticContext *ctx, const char *name) {
	for (int i = 0; i < ctx->alias_count; i++) {
		if (strcmp(ctx->aliases[i]->name, name) == 0)
			return ctx->aliases[i]->archetype;
	}
	return NULL;
}

static const char *archetype_any_alias(SemanticContext *ctx, ArchetypeInfo *arch) {
	for (int i = 0; i < ctx->alias_count; i++) {
		if (ctx->aliases[i]->archetype == arch)
			return ctx->aliases[i]->name;
	}
	return "<unnamed>";
}

static FieldInfo *find_field(ArchetypeInfo *arch, const char *name) {
	if (!arch)
		return NULL;
	for (int i = 0; i < arch->field_count; i++) {
		if (strcmp(arch->fields[i]->name, name) == 0) {
			return arch->fields[i];
		}
	}
	return NULL;
}

static int find_known_func(SemanticContext *ctx, const char *name) {
	for (int i = 0; i < ctx->known_func_count; i++) {
		if (strcmp(ctx->known_funcs[i], name) == 0) {
			return 1;
		}
	}
	return 0;
}

/* A new local binding must not shadow a global proc/func: a variable and a callable share the
 * value namespace, so the bare name becomes ambiguous (reads resolve to the local, but an
 * assignment target resolves to the callable — a silent footgun). Shadowing a variable/param and
 * the `:=` move-rebind idiom stay legal, since those names are not callables. */
static void check_shadows_callable(SemanticContext *ctx, const char *name, SourceLoc loc) {
	if (name && strcmp(name, "_") != 0 && find_known_func(ctx, name))
		sem_emit_local_shadows_callable(ctx, loc, name);
}

static void register_func(SemanticContext *ctx, const char *name) {
	if (find_known_func(ctx, name)) {
		return; /* already registered */
	}
	ctx->known_funcs = realloc(ctx->known_funcs, (ctx->known_func_count + 1) * sizeof(char *));
	ctx->known_funcs[ctx->known_func_count] = malloc(strlen(name) + 1);
	strcpy(ctx->known_funcs[ctx->known_func_count], name);
	ctx->known_func_count++;
}

/* 1 if `name` is the name of a proc / func / overload-group declared anywhere in the program
 * (order-independent — the whole AST exists). Backs callable-alias classification. */
static int prog_has_callable(SemanticContext *ctx, const char *name) {
	if (!name)
		return 0;
	for (int i = 0; i < ctx->decl_count; i++) {
		DeclSummary *d = ctx->decls[i];
		if ((d->kind == DECL_PROC || d->kind == DECL_FUNC || d->kind == DECL_FUNC_GROUP) && d->name &&
		    strcmp(d->name, name) == 0)
			return 1;
	}
	return 0;
}

static void register_callable_alias(SemanticContext *ctx, const char *name, const char *target) {
	ctx->callable_alias_names = realloc(ctx->callable_alias_names, (ctx->callable_alias_count + 1) * sizeof(char *));
	ctx->callable_alias_targets =
	    realloc(ctx->callable_alias_targets, (ctx->callable_alias_count + 1) * sizeof(char *));
	char *n = malloc(strlen(name) + 1), *t = malloc(strlen(target) + 1);
	strcpy(n, name);
	strcpy(t, target);
	ctx->callable_alias_names[ctx->callable_alias_count] = n;
	ctx->callable_alias_targets[ctx->callable_alias_count] = t;
	ctx->callable_alias_count++;
	register_func(ctx, name); /* so calls through the alias aren't flagged undefined */
}

/* Resolve a callable-alias name to its ultimate proc/func target (follows chains, guarded
 * against cycles). Returns NULL if `name` is not a callable alias. */
static const char *resolve_callable_alias(SemanticContext *ctx, const char *name) {
	const char *cur = NULL;
	for (int hop = 0; name && hop < 64; hop++) {
		const char *next = NULL;
		for (int i = 0; i < ctx->callable_alias_count; i++)
			if (strcmp(ctx->callable_alias_names[i], name) == 0) {
				next = ctx->callable_alias_targets[i];
				break;
			}
		if (!next)
			break;
		cur = next;
		name = next;
	}
	return cur;
}

static void register_callable_type_alias(SemanticContext *ctx, const char *name, TypeId id) {
	ctx->ctype_alias_names = realloc(ctx->ctype_alias_names, (ctx->ctype_alias_count + 1) * sizeof(char *));
	ctx->ctype_alias_ids = realloc(ctx->ctype_alias_ids, (ctx->ctype_alias_count + 1) * sizeof(TypeId));
	char *n = malloc(strlen(name) + 1);
	strcpy(n, name);
	ctx->ctype_alias_names[ctx->ctype_alias_count] = n;
	ctx->ctype_alias_ids[ctx->ctype_alias_count] = id;
	ctx->ctype_alias_count++;
}

static TypeId callable_type_alias_id(SemanticContext *ctx, const char *name) {
	if (!ctx || !name)
		return TYID_UNKNOWN;
	for (int i = 0; i < ctx->ctype_alias_count; i++)
		if (strcmp(ctx->ctype_alias_names[i], name) == 0)
			return ctx->ctype_alias_ids[i];
	return TYID_UNKNOWN;
}

static char *sem_dupz(const char *s);

/* Record an enum's type name + its (variant → value) entries. The type-alias-to-int registration
 * is done by the caller (register_type_alias is defined later). */
static void register_enum_entries(SemanticContext *ctx, DeclSummary *e) {
	/* Define-once + datasheet coalescing for the enum's variant table. (The type-alias side — `name`
	 * backed by `int` — is registered by the caller via register_type_alias_tiered, which dedups a
	 * datasheet alias on backing agreement but cannot see variants: two enums both back `int`, so a
	 * differing variant set would slip past it.) If this enum name is already registered, the variants
	 * must AGREE — a datasheet re-declaring the same enum is shared vocabulary, so dedup silently; a
	 * different variant set or value is a redefinition the alias path cannot detect. */
	for (int t = 0; t < ctx->enum_type_count; t++) {
		if (strcmp(ctx->enum_type_names[t], e->name) != 0)
			continue;
		int existing = 0;
		for (int i = 0; i < ctx->enum_var_count; i++)
			if (strcmp(ctx->enum_var_enum[i], e->name) == 0)
				existing++;
		int agree = (existing == e->enum_variant_count);
		for (int i = 0; agree && i < e->enum_variant_count; i++) {
			int found = 0;
			for (int j = 0; j < ctx->enum_var_count; j++)
				if (strcmp(ctx->enum_var_enum[j], e->name) == 0 &&
				    strcmp(ctx->enum_var_name[j], e->enum_variant_names[i]) == 0) {
					found = (ctx->enum_var_value[j] == e->enum_variant_values[i]);
					break;
				}
			if (!found)
				agree = 0;
		}
		if (!agree)
			sem_emit_type_alias_redefined(ctx, e->loc, e->name);
		return; /* already registered — never duplicate the type name or its variants */
	}
	ctx->enum_type_names = realloc(ctx->enum_type_names, (ctx->enum_type_count + 1) * sizeof(char *));
	ctx->enum_type_names[ctx->enum_type_count++] = sem_dupz(e->name);
	for (int i = 0; i < e->enum_variant_count; i++) {
		int n = ctx->enum_var_count;
		ctx->enum_var_enum = realloc(ctx->enum_var_enum, (n + 1) * sizeof(char *));
		ctx->enum_var_name = realloc(ctx->enum_var_name, (n + 1) * sizeof(char *));
		ctx->enum_var_value = realloc(ctx->enum_var_value, (n + 1) * sizeof(long));
		ctx->enum_var_enum[n] = sem_dupz(e->name);
		ctx->enum_var_name[n] = sem_dupz(e->enum_variant_names[i]);
		ctx->enum_var_value[n] = e->enum_variant_values[i];
		ctx->enum_var_count++;
	}
}

static int enum_is_type(SemanticContext *ctx, const char *name) {
	if (!ctx || !name)
		return 0;
	for (int i = 0; i < ctx->enum_type_count; i++)
		if (strcmp(ctx->enum_type_names[i], name) == 0)
			return 1;
	return 0;
}

/* Look up a variant's value. If `en` is non-NULL, scope to that enum; else search all enums
 * (for bare variant patterns in `match`). Returns 1 + sets *out on success. */
static int enum_variant_lookup(SemanticContext *ctx, const char *en, const char *variant, long *out) {
	if (!ctx || !variant)
		return 0;
	for (int i = 0; i < ctx->enum_var_count; i++)
		if ((!en || strcmp(ctx->enum_var_enum[i], en) == 0) && strcmp(ctx->enum_var_name[i], variant) == 0) {
			*out = ctx->enum_var_value[i];
			return 1;
		}
	return 0;
}

static int is_static_name(SemanticContext *ctx, const char *name) {
	if (!name)
		return 0;
	for (int i = 0; i < ctx->static_name_count; i++)
		if (strcmp(ctx->static_names[i], name) == 0)
			return 1;
	return 0;
}

/* 1 if `name` is a top-level static ARRAY (a global buffer `name : T[N]`). A global buffer is
 * registered as a scope variable with its ELEMENT type (a scalar TypeId), so its type alone
 * can't tell a buffer from a scalar — the DeclTable's static_kind is the distinguishing signal.
 * Used by the not-indexable check so `buf[i]` on a global buffer is never mis-flagged. */
static int name_is_static_array(SemanticContext *ctx, const char *name) {
	if (!name)
		return 0;
	for (int i = 0; i < ctx->decl_count; i++) {
		DeclSummary *d = ctx->decls[i];
		if (d->kind == DECL_STATIC && d->static_kind == STATIC_KIND_ARRAY && d->name && strcmp(d->name, name) == 0)
			return 1;
	}
	return 0;
}

/* If `name` is an N-D matrix const (row stride > 1), return its element type name (e.g. "char") and
 * write the row width to `*stride`; else NULL. Lets `S[i]` (one index) type as a ROW slice. */
static const char *name_const_matrix(SemanticContext *ctx, const char *name, int *stride) {
	if (!name)
		return NULL;
	for (int i = 0; i < ctx->decl_count; i++) {
		DeclSummary *d = ctx->decls[i];
		if (d->kind == DECL_STATIC && d->static_kind == STATIC_KIND_ARRAY && d->static_row_stride > 1 && d->name &&
		    strcmp(d->name, name) == 0) {
			if (stride)
				*stride = d->static_row_stride;
			return sem_tyid_name(ctx, d->static_type_id);
		}
	}
	return NULL;
}

/* 1 if `name` is an aggregate value const (a `::`-bound array const, e.g. `XS :: {1,2,3}`), which is
 * immutable — assigning to it or to one of its elements is an error. */
static int name_is_const_static_array(SemanticContext *ctx, const char *name) {
	if (!name)
		return 0;
	for (int i = 0; i < ctx->decl_count; i++) {
		DeclSummary *d = ctx->decls[i];
		if (d->kind == DECL_STATIC && d->static_kind == STATIC_KIND_ARRAY && d->static_is_const && d->name &&
		    strcmp(d->name, name) == 0)
			return 1;
	}
	return 0;
}

static void register_static_name(SemanticContext *ctx, const char *name) {
	if (!name || is_static_name(ctx, name))
		return;
	ctx->static_names = realloc(ctx->static_names, (ctx->static_name_count + 1) * sizeof(char *));
	ctx->static_names[ctx->static_name_count] = malloc(strlen(name) + 1);
	strcpy(ctx->static_names[ctx->static_name_count], name);
	ctx->static_name_count++;
}

/* Forward-declare normalize_type_name so helpers below can use it. */
static const char *normalize_type_name(const char *type_name);

static GroupInfo *find_group(SemanticContext *ctx, const char *name) {
	for (int i = 0; i < ctx->group_count; i++) {
		if (strcmp(ctx->groups[i].name, name) == 0)
			return &ctx->groups[i];
	}
	return NULL;
}

/* AST-kill: resolved decl-signature lookups (supersede the AstProgram `decls` scans).
 * The summary of a proc or func named `name` (call-signature resolution), or NULL. */
static DeclSummary *find_callable_sig(SemanticContext *ctx, const char *name) {
	if (!name)
		return NULL;
	for (int i = 0; i < ctx->decl_count; i++) {
		DeclSummary *d = ctx->decls[i];
		if ((d->kind == DECL_PROC || d->kind == DECL_FUNC) && !d->is_policy && d->name && strcmp(d->name, name) == 0)
			return d; /* a policy is NOT callable — it's invoked via `!name` (see find_policy_sig) */
	}
	return NULL;
}

/* A failure-policy decl by name (the `!name` namespace), or NULL. Separate from find_callable_sig so a
 * policy and a func/proc may share a name (`zero` the divide policy and `zero` a user func coexist). */
static DeclSummary *find_policy_sig(SemanticContext *ctx, const char *name) {
	if (!name)
		return NULL;
	for (int i = 0; i < ctx->decl_count; i++) {
		DeclSummary *d = ctx->decls[i];
		if (d->is_policy && d->name && strcmp(d->name, name) == 0)
			return d;
	}
	return NULL;
}

/* As find_policy_sig, but matching a specific op category — `abort`/`undefined` exist as both a bounds
 * and a divide policy, so the category disambiguates which decl `!name` resolves to. */
static DeclSummary *find_policy_sig_cat(SemanticContext *ctx, const char *name, PolicyCategory cat) {
	if (!name)
		return NULL;
	for (int i = 0; i < ctx->decl_count; i++) {
		DeclSummary *d = ctx->decls[i];
		if (d->is_policy && d->policy_category == (int)cat && d->name && strcmp(d->name, name) == 0)
			return d;
	}
	return NULL;
}

static const char *policy_cat_name(PolicyCategory c); /* defined below, near the bounds-policy checks */
static char *sem_txt_dup(SynText t);                  /* defined below */
static char *sv_name_expr_dup(SyntaxView v);          /* defined below */

/* Scan a handler body for an archetype-column reference (`Arch.field`) to an archetype other than
 * `target` — a likely copy-paste mismatch. Returns the foreign name (static buffer) or NULL. */
static const char *handler_body_foreign_arch(SemanticContext *ctx, SyntaxView v, const char *target) {
	static char foreign[128];
	if (!sv_present(v))
		return NULL;
	const SyntaxNode *n = v.node;
	for (int i = 0; i < n->child_count; i++) {
		const SyntaxElem *ch = &n->children[i];
		if (ch->tag == SE_TOKEN && ch->as.token.kind == TOK_IDENT) {
			int len = (int)ch->as.token.length;
			if (len > 0 && len < (int)sizeof foreign) {
				char buf[128];
				memcpy(buf, v.src + ch->as.token.offset, (size_t)len);
				buf[len] = '\0';
				if (find_archetype(ctx, buf) && (!target || strcmp(buf, target) != 0)) {
					snprintf(foreign, sizeof foreign, "%s", buf);
					return foreign;
				}
			}
		} else if (ch->tag == SE_NODE) {
			SyntaxView cv = {ch->as.node, v.src};
			const char *r = handler_body_foreign_arch(ctx, cv, target);
			if (r)
				return r;
		}
	}
	return NULL;
}

/* Validate the `?handler` on a pool `insert(...)` call (operates on the call syntax `v`). The sigil
 * must be `?` (handler), the name must resolve to a @policy(pool), and a handler whose body reads a
 * different archetype's columns than the insert target is a lint. Emits at most one diagnostic. */
static void sem_check_insert_handler(SemanticContext *ctx, SyntaxView v, SourceLoc loc) {
	SyntaxView pol = sv_child(v, SN_POLICY_REF);
	if (!sv_present(pol))
		return; /* unannotated → the pool's declared handler or the `reject` default (resolved in codegen) */
	SynText nt = sv_token(pol, TOK_IDENT);
	if (!nt.ptr)
		return;
	char *nm = sem_txt_dup(nt);
	int is_handler = sv_token(pol, TOK_QUESTION).ptr != NULL;
	if (!is_handler) {
		sem_emit_policy_wrong_sigil(ctx, loc, nm, 1); /* `!` on an insert → needs `?` */
		free(nm);
		return;
	}
	DeclSummary *p = find_policy_sig_cat(ctx, nm, POLICY_CAT_POOL);
	if (!p) {
		DeclSummary *any = find_policy_sig(ctx, nm);
		if (!any)
			sem_emit_policy_unknown(ctx, loc, nm);
		else
			sem_emit_policy_wrong_category(ctx, loc, nm, "pool", policy_cat_name(any->policy_category));
		free(nm);
		return;
	}
	/* W0019: the handler reads a foreign pool's columns. */
	char *target = sv_name_expr_dup(sem_node_at_expr(v, 0));
	if (target) {
		const char *foreign = handler_body_foreign_arch(ctx, p->body_node, target);
		if (foreign)
			sem_emit_lint_handler_foreign_arch(ctx, loc, nm, foreign, target);
		free(target);
	}
	free(nm);
}

/* The compilation unit a reference originates in: the enclosing proc/func's owning unit (0 = entry). */
static int sem_ref_unit(SemanticContext *ctx) {
	if (ctx->current_proc)
		return ctx->current_proc->unit;
	if (ctx->current_func)
		return ctx->current_func->unit;
	return 0;
}

/* Visibility gate for a bare callable reference. Returns 1 if some proc/func named `name` is visible
 * from `ref_unit` (same unit — any band — OR exported), 0 if the only matches are NON-exported decls
 * owned by other units, -1 if no callable named `name` exists at all.
 *
 * This makes the region band a real visibility rule rather than a side effect of name mangling: a
 * pure-Arche private decl is renamed to `mod.name`, so a bare `name` never matches it here; a
 * `#foreign` extern keeps its raw C link name (it can't be mangled), so WITHOUT this gate its symbol
 * would stay callable across a device boundary — the device's implementation leaking past its public
 * API. A `#module`/`#file` extern is in scope only inside its own unit's files; from any importer it
 * is simply not defined. (The C symbol stays linkable — that's an unavoidable ABI fact — but arche
 * name resolution no longer reaches it.) */
static int sem_callable_visible(SemanticContext *ctx, const char *name, int ref_unit) {
	if (!name)
		return -1;
	int saw_hidden = 0;
	for (int i = 0; i < ctx->decl_count; i++) {
		DeclSummary *d = ctx->decls[i];
		if ((d->kind != DECL_PROC && d->kind != DECL_FUNC) || !d->name || strcmp(d->name, name) != 0)
			continue;
		if (d->unit == ref_unit || d->visibility == VIS_EXPORTED)
			return 1;
		saw_hidden = 1;
	}
	return saw_hidden ? 0 : -1;
}

/* The summary of a func named `name`, or NULL. */
static DeclSummary *find_func_sig(SemanticContext *ctx, const char *name) {
	if (!name)
		return NULL;
	for (int i = 0; i < ctx->decl_count; i++)
		if (ctx->decls[i]->kind == DECL_FUNC && ctx->decls[i]->name && strcmp(ctx->decls[i]->name, name) == 0)
			return ctx->decls[i];
	return NULL;
}

/* The summary of a proc named `name`, or NULL. */
static DeclSummary *find_proc_sig(SemanticContext *ctx, const char *name) {
	if (!name)
		return NULL;
	for (int i = 0; i < ctx->decl_count; i++)
		if (ctx->decls[i]->kind == DECL_PROC && ctx->decls[i]->name && strcmp(ctx->decls[i]->name, name) == 0)
			return ctx->decls[i];
	return NULL;
}

static VariableInfo *find_variable(SemanticContext *ctx, const char *name) {
	/* Search innermost to outermost scope; within a scope prefer the NEWEST binding so a
	 * shadow wins. This is what makes `:=` rebinding work: `buf := foo(move buf)` consumes the
	 * old `buf` and binds a fresh one, and name resolution then sees the fresh one. */
	for (int i = ctx->scope_count - 1; i >= 0; i--) {
		Scope *scope = &ctx->scopes[i];
		for (int j = scope->var_count - 1; j >= 0; j--) {
			if (strcmp(scope->vars[j]->name, name) == 0) {
				return scope->vars[j];
			}
		}
	}
	return NULL;
}

/* Append a structured diagnostic. The CLI's stderr print is left to the caller so
 * --dump and the warm server share the same in-memory list without doubling output.
 * Each SemDiag is its own heap allocation; the array stores pointers so growing it
 * does not invalidate previously-returned pointers (notes can be attached safely
 * after later emissions). Bumps `error_count` when severity is 1 so callers don't
 * track it separately. Returns the new SemDiag so wrappers can attach notes. */
SemDiag *diag_push(SemanticContext *ctx, int severity, int has_loc, SourceLoc loc, const char *code, const char *name,
                   const char *msg) {
	if (!ctx)
		return NULL;
	if (ctx->diag_count >= ctx->diag_cap) {
		ctx->diag_cap = ctx->diag_cap ? ctx->diag_cap * 2 : 16;
		ctx->diags = realloc(ctx->diags, (size_t)ctx->diag_cap * sizeof(SemDiag *));
	}
	SemDiag *d = malloc(sizeof(SemDiag));
	d->severity = severity ? 1 : 0;
	d->has_loc = has_loc ? 1 : 0;
	d->loc = loc;
	d->code = code; /* NULL for the legacy error_at/lint_emit paths until migration */
	d->name = name ? name : "semantic";
	d->message = strdup(msg ? msg : "");
	d->notes = NULL;
	d->note_count = 0;
	ctx->diags[ctx->diag_count++] = d;
	if (d->severity)
		ctx->error_count++;
	return d;
}

/* Diagnostics: ALL semantic.c error sites now route through the typed wrappers in
 * sem_diagnostics.h (`sem_emit_<slug>`). The wrappers preserve the exact stderr
 * format the legacy `error_at` / `lint_emit` used to print, and append to
 * ctx->diags via diag_push so editor consumers see them. The legacy `error_at`
 * and `lint_emit` functions are gone — see git history if you need to revive them. */

/* Lint configuration moved to semantic/sem_diagnostics.c (table-driven). The three
 * `semantic_set_lint_*` functions are now backward-compat shims defined there that
 * route through `semantic_set_diag(SEM_LINT_<slug>, ...)`. The two lint emit sites
 * in this file (proc-could-be-func / proc-no-effect, and func-impure below) call
 * the typed `sem_emit_lint_*` wrappers, which consult the runtime config + format
 * the same stderr line `lint_emit` used to. */

static const char *resolve_type_alias(SemanticContext *ctx, const char *name);
static const char *sem_tyid_name(SemanticContext *ctx, TypeId t);

/* True if a binding is opaque-backed (the linear, move-only kind). Used both by the
 * must-consume check and by return/insert auto-marking (only opaque is consumed by
 * being returned or inserted — data and handles copy). */
static int var_is_opaque(SemanticContext *ctx, VariableInfo *v) {
	if (!v)
		return 0;
	if (v->inferred_type && strcmp(v->inferred_type, "opaque") == 0)
		return 1;
	const char *tn = sem_tyid_name(ctx, v->type_id);
	if (tn && strcmp(tn, "opaque") == 0)
		return 1;
	return 0;
}

/* The nominal opaque type name for a variable (e.g. "file"/"socket"), used to look up its
 * registered `@drop` destructor. NULL if not an opaque local with a nominal name. */
static const char *var_opaque_type_name(SemanticContext *ctx, VariableInfo *v) {
	if (!var_is_opaque(ctx, v))
		return NULL;
	if (v->nominal_type)
		return v->nominal_type;
	return tyid_nominal_name(ctx->ty_arena, v->type_id);
}

/* The destructor proc name registered (via `@drop`) for opaque type `type_name`, or NULL if
 * none. Codegen consults this (via semantic_drop_dtor) to know whether a live opaque local
 * gets an auto-drop call at scope exit. */
static const char *drop_dtor_for(SemanticContext *ctx, const char *type_name) {
	if (!type_name)
		return NULL;
	for (int i = 0; i < ctx->drop_count; i++)
		if (strcmp(ctx->drop_type_names[i], type_name) == 0)
			return ctx->drop_proc_names[i];
	return NULL;
}

/* Register `type_name`'s destructor as `proc_name`. One per type — a second `@drop` for the
 * same opaque type is E0119. */
static void register_drop(SemanticContext *ctx, const char *type_name, const char *proc_name, SourceLoc loc) {
	for (int i = 0; i < ctx->drop_count; i++) {
		if (strcmp(ctx->drop_type_names[i], type_name) == 0) {
			sem_emit_drop_redefined(ctx, loc, type_name);
			return;
		}
	}
	ctx->drop_type_names = realloc(ctx->drop_type_names, (ctx->drop_count + 1) * sizeof(char *));
	ctx->drop_proc_names = realloc(ctx->drop_proc_names, (ctx->drop_count + 1) * sizeof(char *));
	ctx->drop_locs = realloc(ctx->drop_locs, (ctx->drop_count + 1) * sizeof(SourceLoc));
	ctx->drop_type_names[ctx->drop_count] = sem_dupz(type_name);
	ctx->drop_proc_names[ctx->drop_count] = sem_dupz(proc_name);
	ctx->drop_locs[ctx->drop_count] = loc;
	ctx->drop_count++;
}

/* Validate a `@drop` proc's signature (`proc (own T)()`, T opaque, no out-params) and register
 * its destructor. Errors (E0118) on a bad signature without registering. */
static void analyze_drop_decl(SemanticContext *ctx, DeclSummary *proc, const char *declared_type) {
	if (!proc)
		return;
	if (proc->out_param_count != 0) {
		sem_emit_drop_invalid(ctx, proc->loc,
		                      "a `@drop` destructor must be `proc (own T)()` — it may not have out-parameters");
		return;
	}
	if (proc->param_count != 1) {
		sem_emit_drop_invalid(ctx, proc->loc,
		                      "a `@drop` destructor must take exactly one `own` parameter of an opaque type");
		return;
	}
	ParamSummary *p = &proc->params[0];
	if (!p->is_own) {
		sem_emit_drop_invalid(ctx, proc->loc, "a `@drop` destructor's parameter must be `own`");
		return;
	}
	TypeId p_tid = p->type_id;
	const char *p_resolved = sem_tyid_name(ctx, p_tid);              /* "opaque" */
	const char *p_nominal = tyid_nominal_name(ctx->ty_arena, p_tid); /* the written opaque name */
	if (!p_resolved || strcmp(p_resolved, "opaque") != 0) {
		sem_emit_drop_invalid(ctx, proc->loc, "a `@drop` destructor's parameter must be of an opaque type");
		return;
	}
	/* The `@drop(<type>)` name must match the parameter's type — the decorator states what is
	 * dropped; a mismatch is a typo, not silently ignored. */
	if (declared_type && (!p_nominal || strcmp(declared_type, p_nominal) != 0)) {
		sem_emit_drop_invalid(ctx, proc->loc,
		                      "`@drop(...)` names a different type than the destructor's parameter — they must match");
		return;
	}
	register_drop(ctx, p_nominal, proc->name, proc->loc);
}

static void push_scope(SemanticContext *ctx) {
	ctx->scopes = realloc(ctx->scopes, (ctx->scope_count + 1) * sizeof(Scope));
	ctx->scopes[ctx->scope_count].vars = NULL;
	ctx->scopes[ctx->scope_count].var_count = 0;
	ctx->scope_count++;
}

static void pop_scope(SemanticContext *ctx) {
	if (ctx->scope_count > 0) {
		Scope *scope = &ctx->scopes[ctx->scope_count - 1];
		for (int i = 0; i < scope->var_count; i++) {
			VariableInfo *v = scope->vars[i];
			/* Linear must-consume: an opaque LOCAL must be consumed (move / close / return /
			 * insert) before its scope ends. Borrowed params are exempt. With RAII, an opaque
			 * type that HAS a registered `@drop` destructor is instead auto-dropped at scope
			 * exit (codegen emits the call) — not an error. A destructor-less opaque keeps the
			 * old must-consume rule. */
			if (var_is_opaque(ctx, v) && !v->is_param && !v->is_consumed &&
			    !drop_dtor_for(ctx, var_opaque_type_name(ctx, v))) {
				sem_emit_opaque_not_consumed(ctx, v->loc, v->name);
			}
			/* W0004 unused_local: warn for non-param locals never read. Exemptions: names starting
			 * with '_' (rust convention); a `move`d/consumed binding (handing it off IS a use); a
			 * binding whose name is an OUT-PARAM of the enclosing proc (the kill-and-rebind form
			 * `buf := f(move buf)` produces such a local that is the proc's OUTPUT, not dead); and a
			 * binding SHADOWED by a later same-named one in this scope — the original of a kill-and-
			 * rebind `f(move x)(x:)` is replaced, not unused. Opaque locals are handled by the
			 * must-consume rule above (suppress to avoid double-firing). */
			int is_outparam_name = 0;
			if (ctx->current_proc && v->name) {
				for (int op = 0; op < ctx->current_proc->out_param_count; op++) {
					const char *opn = ctx->current_proc->out_params[op].name;
					if (opn && strcmp(opn, v->name) == 0) {
						is_outparam_name = 1;
						break;
					}
				}
			}
			int is_shadowed = 0;
			if (v->name) {
				for (int j = i + 1; j < scope->var_count; j++)
					if (scope->vars[j]->name && strcmp(scope->vars[j]->name, v->name) == 0) {
						is_shadowed = 1;
						break;
					}
			}
			if (!v->is_param && !v->is_referenced && !v->is_consumed && !is_outparam_name && !is_shadowed && v->name &&
			    v->name[0] != '_' && !var_is_opaque(ctx, v)) {
				sem_emit_lint_unused_local(ctx, v->loc, v->name);
			}
		}
		for (int i = 0; i < scope->var_count; i++) {
			free(scope->vars[i]->name);
			free(scope->vars[i]);
		}
		free(scope->vars);
		ctx->scope_count--;
	}
}

static void add_variable_with_archetype(SemanticContext *ctx, const char *name, TypeId type_id,
                                        const char *archetype_name);

static void add_variable(SemanticContext *ctx, const char *name, TypeId type_id) {
	add_variable_with_archetype(ctx, name, type_id, NULL);
}

/* Flag the most-recently-added variable as a parameter — exempt from the opaque must-consume
 * check at scope exit. `is_own` records whether it was declared `own` (owned: the caller passed
 * it via `move`/`copy`, so the body may mutate it); a non-`own` param is a read-only borrow. */
static void mark_last_param(SemanticContext *ctx, int is_own) {
	if (ctx->scope_count > 0) {
		Scope *s = &ctx->scopes[ctx->scope_count - 1];
		if (s->var_count > 0) {
			s->vars[s->var_count - 1]->is_param = 1;
			s->vars[s->var_count - 1]->is_own = is_own;
		}
	}
}

static void add_variable_with_archetype(SemanticContext *ctx, const char *name, TypeId type_id,
                                        const char *archetype_name) {
	if (ctx->scope_count == 0)
		return;

	Scope *scope = &ctx->scopes[ctx->scope_count - 1];
	VariableInfo *var = malloc(sizeof(VariableInfo));
	var->name = malloc(strlen(name) + 1);
	strcpy(var->name, name);
	var->type_id = type_id;
	var->archetype_name = archetype_name ? malloc(strlen(archetype_name) + 1) : NULL;
	if (var->archetype_name)
		strcpy(var->archetype_name, archetype_name);
	var->inferred_type = NULL;
	var->nominal_type = NULL;
	var->is_consumed = 0;
	var->is_param = 0;
	var->is_own = 0;
	var->is_out_place = 0;
	var->is_const = 0;
	var->is_referenced = 0;
	var->borrows_local = 0;
	var->loc.line = 0;
	var->loc.column = 0;

	scope->vars = realloc(scope->vars, (scope->var_count + 1) * sizeof(VariableInfo *));
	scope->vars[scope->var_count++] = var;
}

/* Flag the most-recently-added variable as a writable out-param slot. An out-param is a place
 * the body fills; for an in-out it shadows the in-list borrow, so writing it needs no `own`. */
static void mark_last_out_place(SemanticContext *ctx) {
	if (ctx->scope_count > 0) {
		Scope *s = &ctx->scopes[ctx->scope_count - 1];
		if (s->var_count > 0)
			s->vars[s->var_count - 1]->is_out_place = 1;
	}
}

/* Flag the most-recently-added variable as an immutable constant (`k :: e` / `k : T : e`). */
static void mark_last_const(SemanticContext *ctx) {
	if (ctx->scope_count > 0) {
		Scope *s = &ctx->scopes[ctx->scope_count - 1];
		if (s->var_count > 0)
			s->vars[s->var_count - 1]->is_const = 1;
	}
}

/* ========== FORWARD DECLARATIONS ========== */

static void analyze_expression(SemanticContext *ctx, SyntaxView v);
static void analyze_statement(SemanticContext *ctx, SyntaxView v);
static int proc_param_is_inout(DeclSummary *proc, int param_idx);
static const char *resolve_expression_type(SemanticContext *ctx, SyntaxView v);

/* By-reference aggregate param types: arrays are passed by reference (borrowed read-only by
 * default), so mutating one through a non-`move` param is a purity violation. Scalars are by
 * value (a freely-mutable local copy); opaque can't be indexed/assigned at all. */
static int type_is_byref_aggregate(const TypeArena *a, TypeId t) {
	TyKind k = tyid_kind(a, t);
	return k == TYK_SLICE || k == TYK_ARRAY;
}

/* Implicit `move` of a bare move-only name in an ownership-taking position (defined below). */
static void implicit_move_consume(SemanticContext *ctx, SyntaxView v);

/* AST-kill (transitional): recover the SyntaxView an AstProgram node was reconstructed from.
 * Lets analysis read the immutable syntax tree directly while the AstProgram readers are
 * converted to views leaf-up. NULL-safe (an absent node yields an absent view), since AST
 * pointers handed to converted readers are frequently optional (e.g. a bind with no value).
 * Removed with AstProgram once nothing reads these structs. */
#define AST_SV(n) ((n) ? ((SyntaxView){(n)->sn, (n)->sn_src}) : ((SyntaxView){NULL, NULL}))

/* Forward declarations for view/decl helpers defined further down but used by earlier analysis. */
static char *sem_txt_dup(SynText t);
static char *sem_cv_dup(SyntaxView v);
static char *sv_name_expr_dup(SyntaxView e);
static char *sv_resolved_name(SemanticContext *ctx, SyntaxView v);
static Operator sem_tok_to_op(TokenKind k);
static DeclSummary *decl_summary_from_node(SemanticContext *ctx, SyntaxView dv);
static void sem_expand_tuple_groups_table(SemanticContext *ctx);
static void sem_fill_decl_type_ids(SemanticContext *ctx, DeclSummary *ds);
static char *sem_own_str(SemanticContext *ctx, char *s);

/* Canonicalize the capitalized primitive spellings (`Int`/`Float`/…) to their lowercase keywords. */
static const char *normalize_type_name(const char *type_name) {
	if (!type_name)
		return type_name;
	if (strcmp(type_name, "Int") == 0)
		return "int";
	if (strcmp(type_name, "Float") == 0)
		return "float";
	if (strcmp(type_name, "Str") == 0)
		return "str";
	if (strcmp(type_name, "Char") == 0)
		return "char";
	if (strcmp(type_name, "Void") == 0)
		return "void";
	return type_name;
}

/* Returns 1 if `type_name` is a fixed-width integer type name
 * (byte, i8/u8 .. i64/u64, i128/u128). Always available in the language. */
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

/* Returns 1 if `type_name` (already normalized) is a built-in primitive. */
static int is_primitive_type_name(const char *type_name) {
	if (!type_name)
		return 0;
	const char *n = normalize_type_name(type_name);
	return strcmp(n, "int") == 0 || strcmp(n, "float") == 0 || strcmp(n, "char") == 0 || strcmp(n, "str") == 0 ||
	       strcmp(n, "void") == 0 || is_width_int_name(n);
}

/* If `name` is a nominal type alias, follow the chain to its ultimate backing type
 * name (a primitive or `opaque`); otherwise return `name` unchanged. The guard bounds
 * the walk so an accidental cycle can't loop forever. */
/* Match a registry alias name against a (possibly module-qualified) reference: an exact match, or
 * the reference's `mod.tail` matching the registered bare `tail` (so a qualified `io.file` resolves
 * to the `file :: opaque` registered inside the `io` module). */
static int alias_name_matches(const char *registered, const char *ref) {
	if (strcmp(registered, ref) == 0)
		return 1;
	const char *dot = strrchr(ref, '.');
	return dot && strcmp(registered, dot + 1) == 0;
}

static const char *resolve_type_alias(SemanticContext *ctx, const char *name) {
	if (!ctx || !name)
		return name;
	for (int guard = 0; guard <= ctx->type_alias_count; guard++) {
		int found = 0;
		for (int i = 0; i < ctx->type_alias_count; i++) {
			if (alias_name_matches(ctx->type_alias_names[i], name)) {
				name = ctx->type_alias_backings[i];
				found = 1;
				break;
			}
		}
		if (!found)
			break;
	}
	return name;
}

/* 1 if `name` is a registered nominal type alias (qualified or bare). */
static int is_type_alias(SemanticContext *ctx, const char *name) {
	if (!ctx || !name)
		return 0;
	for (int i = 0; i < ctx->type_alias_count; i++)
		if (alias_name_matches(ctx->type_alias_names[i], name))
			return 1;
	return 0;
}

/* 1 if `name(x)` is a TYPE conversion rather than a proc/func call: a primitive/width cast
 * (`i64(x)`, `float(x)`) or a nominal alias/subtype conversion (`meters(x)`, `mps(x)`). The callee
 * names a type, so only the argument is analyzed and the expression takes that type. */
static int is_type_conversion_callee(SemanticContext *ctx, const char *name) {
	return is_primitive_type_name(name) || is_type_alias(ctx, name);
}

/* ---- constant / type-alias registration (pass 0 helpers) ----
 * A `name : [meta] : value` declaration is a *type alias* when its RHS denotes a type, or a
 * *value const* when its RHS denotes a value. The classification is by denotation (what the RHS
 * names), not by syntactic node kind — so `tau :: pi` (pi a value) is a value const, not an alias.
 * `name`/`backing`/`lexeme` are stored by pointer and must outlive ctx (syntax tree or static strings). */

/* `@implements(<device>.<req>, …)` map (driver-side): each device requirement TYPE name (e.g.
 * `file1`, `handle`) maps to the driver's own type name (e.g. `file`, `dh`). Built once at the start
 * of pass 0 from the entry decls' decorators (sem_build_impl_map), consulted while registering
 * datasheet types and interning device param/return types so the device's opaque is UNIFIED with /
 * RENAMED to the driver's — the semantic mirror of lowering's g_impl pass. Cleared at pass end. */
#define SEM_IMPL_CAP 64
static char *g_sem_impl_req[SEM_IMPL_CAP];
static char *g_sem_impl_driver[SEM_IMPL_CAP];
static int g_sem_impl_n = 0;

static const char *impl_driver_for(const char *name) {
	if (!name)
		return NULL;
	for (int i = 0; i < g_sem_impl_n; i++)
		if (strcmp(g_sem_impl_req[i], name) == 0)
			return g_sem_impl_driver[i];
	return NULL;
}

/* Register a nominal alias `name → backing`; redefinition must AGREE (same backing).
 * `loc` is the alias declaration's source position — used for the redefinition error
 * and stored so the late "unknown backing" pass can blame the original site. */
static void register_type_alias_tiered(SemanticContext *ctx, const char *name, const char *backing, int transparent,
                                       SourceLoc loc, int from_datasheet) {
	/* A datasheet type that a driver `@implements` is renamed out of existence: the driver's own type
	 * (registered separately) is the identity. Skipping it both UNIFIES (test: file1/file2 → file) and
	 * avoids the define-once collision when the driver reuses a colliding name (test: dconf.handle → dh
	 * frees `handle` for the importer's own). `name` is borrowed from the decl (freed at decl teardown),
	 * so skipping does not leak. */
	if (from_datasheet && impl_driver_for(name))
		return;
	for (int j = 0; j < ctx->type_alias_count; j++) {
		if (strcmp(ctx->type_alias_names[j], name) == 0) {
			/* Define-once: a type/component name is declared exactly once program-wide. A second
			 * definition with a different backing is E0010; with the same backing it's still a
			 * redefinition (E0045) — reference the name instead of redeclaring it.
			 * EXCEPTION: a datasheet decl that AGREES with an existing definition is shared global
			 * vocabulary (two devices using the same shape) — dedup silently, do not trip E0045. */
			if (strcmp(ctx->type_alias_backings[j], backing) != 0)
				sem_emit_type_alias_redefined(ctx, loc, name);
			else if (!from_datasheet)
				sem_emit_component_redefined(ctx, loc, name);
			return;
		}
	}
	ctx->type_alias_names = realloc(ctx->type_alias_names, (ctx->type_alias_count + 1) * sizeof(char *));
	ctx->type_alias_backings = realloc(ctx->type_alias_backings, (ctx->type_alias_count + 1) * sizeof(char *));
	ctx->type_alias_transparent = realloc(ctx->type_alias_transparent, (ctx->type_alias_count + 1) * sizeof(int));
	ctx->type_alias_locs = realloc(ctx->type_alias_locs, (ctx->type_alias_count + 1) * sizeof(SourceLoc));
	ctx->type_alias_names[ctx->type_alias_count] = (char *)name;
	ctx->type_alias_backings[ctx->type_alias_count] = (char *)backing;
	ctx->type_alias_transparent[ctx->type_alias_count] = transparent;
	ctx->type_alias_locs[ctx->type_alias_count] = loc;
	ctx->type_alias_count++;
}

/* Back-compat shim: the common case registers a tier-2 subtype (the distinct-by-default). */
static void register_type_alias(SemanticContext *ctx, const char *name, const char *backing, SourceLoc loc,
                                int from_datasheet) {
	register_type_alias_tiered(ctx, name, backing, 0, loc, from_datasheet);
}

/* 1 if `name` is a registered TRANSPARENT (tier-1) alias — same identity as its backing. A tier-2
 * subtype (the default) returns 0, as does any non-alias name. */
static int alias_is_transparent(SemanticContext *ctx, const char *name) {
	if (!ctx || !name)
		return 0;
	for (int i = 0; i < ctx->type_alias_count; i++)
		if (alias_name_matches(ctx->type_alias_names[i], name))
			return ctx->type_alias_transparent[i];
	return 0;
}

/* The stored value lexeme of a value const, or NULL. */
static const char *value_const_lexeme(SemanticContext *ctx, const char *name) {
	for (int j = 0; j < ctx->const_count; j++)
		if (strcmp(ctx->const_names[j], name) == 0)
			return ctx->const_values[j];
	return NULL;
}

/* The resolved type name of a value const ("int"/"float"/"char"/…), or NULL. */
static const char *value_const_type(SemanticContext *ctx, const char *name) {
	for (int j = 0; j < ctx->const_count; j++)
		if (strcmp(ctx->const_names[j], name) == 0)
			return ctx->const_value_types[j];
	return NULL;
}

/* Register a value const `name = lexeme` of type `type`; redefinition is an error. The type lets a
 * const reference resolve to its real type (so a float const is a float, not a default int).
 * `loc` is the constant's declaration position — used for the redefinition error. */
static void register_value_const(SemanticContext *ctx, const char *name, const char *lexeme, const char *type,
                                 SourceLoc loc) {
	for (int j = 0; j < ctx->const_count; j++) {
		if (strcmp(ctx->const_names[j], name) == 0) {
			sem_emit_constant_redefined(ctx, loc, name);
			return;
		}
	}
	ctx->const_names = realloc(ctx->const_names, (ctx->const_count + 1) * sizeof(char *));
	ctx->const_values = realloc(ctx->const_values, (ctx->const_count + 1) * sizeof(const char *));
	ctx->const_value_types = realloc(ctx->const_value_types, (ctx->const_count + 1) * sizeof(const char *));
	ctx->const_locs = realloc(ctx->const_locs, (ctx->const_count + 1) * sizeof(SourceLoc));
	ctx->const_names[ctx->const_count] = (char *)name;
	ctx->const_values[ctx->const_count] = lexeme;
	ctx->const_value_types[ctx->const_count] = type;
	ctx->const_locs[ctx->const_count] = loc;
	ctx->const_count++;
}

/* 1 if the bare name `r` currently denotes a type (a primitive, opaque, or a registered alias). */
static int name_denotes_type(SemanticContext *ctx, const char *r) {
	return is_primitive_type_name(r) || strcmp(r, "opaque") == 0 || is_type_alias(ctx, r);
}

/* For a typed value const `name : T : <literal>`, check the literal is compatible with the declared
 * type T. Only concrete primitive types are checked; a clear category mismatch (a float literal for
 * an int, a string for a number) is an error. So an explicit annotation is no longer inert. */
static void check_const_literal_type(SemanticContext *ctx, DeclSummary *c) {
	if (!c)
		return;
	TyKind dk = tyid_kind(ctx->ty_arena, c->const_decl_type_id);
	if (dk != TYK_NOMINAL && dk != TYK_PRIM)
		return; /* only concrete named declared types */
	if (c->const_value_kind != EXPR_LITERAL && c->const_value_kind != EXPR_STRING)
		return; /* only literal RHS (a name RHS is a value-const chain, checked elsewhere) */
	const char *want = sem_tyid_name(ctx, c->const_decl_type_id);
	if (!want)
		return;
	if (!is_primitive_type_name(want))
		return; /* non-primitive declared type: out of scope for the literal check */
	const char *got = resolve_expression_type(ctx, c->const_value); /* "int" / "float" / "char" / "char_array" */
	if (!got)
		return;
	/* A value const is substituted as its raw literal lexeme, so the literal's category must match
	 * the declared type — there is no coercion at the substitution site. A string never fits a
	 * scalar; a `float` const needs a float literal (an int lexeme in a double slot miscompiles);
	 * a float literal never fits int/char. int and char literals interchange (C treats them alike). */
	int ok = 1;
	if (strcmp(got, "char_array") == 0)
		ok = 0;
	else if (strcmp(want, "float") == 0)
		ok = (strcmp(got, "float") == 0);
	else if (strcmp(got, "float") == 0)
		ok = 0;
	if (!ok) {
		sem_emit_const_type_mismatch(ctx, c->const_value_loc, c->name, want,
		                             strcmp(got, "char_array") == 0 ? "string" : got);
	}
}

/* The backing-type name for a type-form RHS (`name : type : T` or a tuple field), or NULL if the
 * form can't back a nominal alias (e.g. array/handle/shaped — unsupported as alias backings). */
/* The meta-type `type` is legal ONLY as a declaration's declared type (`name : type : T`). It is
 * not a storable value type, so it must not reach a parameter / return / field / variable slot —
 * a parameter of type `type` would be a generic type parameter, and generics (monomorphization)
 * are not implemented yet. Reject it there with a clear "not supported yet" message (the grammar
 * admits the form; the feature behind it just doesn't exist). Returns 1 if it errored. */
static int reject_meta_type(SemanticContext *ctx, TypeId t, SourceLoc loc, const char *where) {
	const char *n = tyid_nominal_name(ctx->ty_arena, t);
	if (n && strcmp(n, "type") == 0) {
		sem_emit_meta_type_invalid_position(ctx, loc, where);
		return 1;
	}
	return 0;
}

/* Type of a bare name (the EXPR_NAME resolution, factored out). Returns a STABLE pointer
 * (a variable/const/archetype name owned elsewhere), never the caller's transient `name`
 * buffer — `name` is used only for lookups here. */
/* Type of a bare name (the EXPR_NAME resolution, factored out). Returns a STABLE pointer
 * (a variable/const/archetype name owned elsewhere), never the caller's transient `name`
 * buffer — `name` is used only for lookups here. */
/* Type of a bare name (the EXPR_NAME resolution, factored out). Returns a STABLE pointer
 * (a variable/const/archetype name owned elsewhere), never the caller's transient `name`
 * buffer — `name` is used only for lookups here. */
static const char *resolve_name_type(SemanticContext *ctx, const char *name) {
	VariableInfo *var = find_variable(ctx, name);
	if (var) {
		/* The resolved base type of the var's declared type (handle → archetype name; array → element
		 * base; nominal → backing chain → prim/name). */
		const char *r = sem_tyid_name(ctx, var->type_id);
		if (r)
			return r;
		if (var->inferred_type)
			return resolve_type_alias(ctx, normalize_type_name(var->inferred_type));
	}
	/* A reference to a top-level value const resolves to that const's type. */
	const char *ct = value_const_type(ctx, name);
	if (ct)
		return ct;
	/* An archetype reference resolves to its (stable) alias name — the stored name with the same
	 * value as `name` (the alias the user wrote), not the transient lookup buffer. */
	if (find_archetype(ctx, name)) {
		for (int i = 0; i < ctx->alias_count; i++)
			if (strcmp(ctx->aliases[i]->name, name) == 0)
				return ctx->aliases[i]->name;
	}
	return NULL;
}

/* Type of `base_name.field_name` (the EXPR_FIELD resolution, factored out). Stable returns. */
static const char *resolve_field_type(SemanticContext *ctx, const char *base_name, const char *field_name) {
	if (strcmp(field_name, "length") == 0 || strcmp(field_name, "max_length") == 0 || strcmp(field_name, "cap") == 0 ||
	    strcmp(field_name, "capacity") == 0)
		return "int";
	ArchetypeInfo *arch = find_archetype(ctx, base_name);
	if (!arch) {
		VariableInfo *var = find_variable(ctx, base_name);
		if (var && var->archetype_name)
			arch = find_archetype(ctx, var->archetype_name);
	}
	if (arch) {
		FieldInfo *field = find_field(arch, field_name);
		if (field && field->type_id != TYID_UNKNOWN)
			return sem_tyid_name(ctx, field->type_id);
	}
	return NULL;
}

/* Stable canonical literal for a primitive/width type name (the result of a `T(x)` cast), so
 * the resolved type outlives the transient callee buffer. NULL if not a known primitive. */
static const char *canonical_cast_type(const char *s) {
	const char *n = normalize_type_name(s);
	static const char *const prims[] = {"int", "float", "char", "str", "void", "byte", "i8",  "i16",
	                                    "i32", "i64",   "i128", "u8",  "u16",  "u32",  "u64", "u128"};
	for (size_t i = 0; i < sizeof(prims) / sizeof(prims[0]); i++)
		if (strcmp(n, prims[i]) == 0)
			return prims[i];
	return NULL;
}

/* Type of a base = leading IDENT + optional SN_FIELD_NAME chain (the base shape inside
 * SN_FIELD_EXPR / SN_INDEX_EXPR / SN_SLICE_EXPR). nf==0 → bare name; nf==1 → single field
 * access; nf>=2 → the old AST's nested-FIELD base isn't a NAME, so only the metadata props
 * on the LAST field resolve (matching the old EXPR_FIELD path), else NULL.
 * A NESTED base (general postfix) resolves the base SUB-EXPRESSION's type first, then applies the
 * trailing field(s): a `.length`/`.cap`-style property is `int`; element access falls through. */
static const char *resolve_base_chain_type(SemanticContext *ctx, SyntaxView v) {
	if (has_nested_base(v)) {
		SyntaxView base = base_subexpr(v);
		int nf = sv_count(v, SN_FIELD_NAME);
		if (nf == 0)
			return resolve_expression_type(ctx, base); /* e.g. `a[i][j]` — element of the base */
		char *fld = sem_cv_dup(sv_child_at(v, SN_FIELD_NAME, nf - 1));
		int is_prop = fld && (strcmp(fld, "length") == 0 || strcmp(fld, "max_length") == 0 || strcmp(fld, "cap") == 0 ||
		                      strcmp(fld, "capacity") == 0);
		free(fld);
		return is_prop ? "int" : resolve_expression_type(ctx, base);
	}
	int nf = sv_count(v, SN_FIELD_NAME);
	char *idnt = sv_resolved_name(ctx, v);
	const char *r;
	if (nf == 0) {
		r = resolve_name_type(ctx, idnt);
	} else {
		char *fld = sem_cv_dup(sv_child_at(v, SN_FIELD_NAME, nf - 1));
		if (nf == 1)
			r = resolve_field_type(ctx, idnt, fld);
		else
			r = (strcmp(fld, "length") == 0 || strcmp(fld, "max_length") == 0 || strcmp(fld, "cap") == 0 ||
			     strcmp(fld, "capacity") == 0)
			        ? "int"
			        : NULL;
		free(fld);
	}
	free(idnt);
	return r;
}

/* Return type of a plain func named `func_name` (the non-group func path of a call). */
static const char *resolve_func_return(SemanticContext *ctx, const char *func_name) {
	DeclSummary *fs = find_func_sig(ctx, func_name);
	if (!fs || fs->return_type_count == 0)
		return NULL;
	TypeId rid = fs->return_type_ids[0];
	TyKind rk = tyid_kind(ctx->ty_arena, rid);
	/* An array return resolves to its ELEMENT base; a char array stays "char_array". */
	if (rk == TYK_SLICE || rk == TYK_ARRAY) {
		const char *en = sem_tyid_name(ctx, rid); /* resolves through to the element base */
		return (en && strcmp(en, "char") == 0) ? "char_array" : en;
	}
	return sem_tyid_name(ctx, rid); /* handle → archetype name; nominal/prim → resolved name */
}

/* Resolved (backing) type name of an expression, read from the immutable syntax tree.
 * Returns a STABLE pointer (a string literal, or a name owned by a variable/field/alias
 * table) so callers may store it. `alloc` is not a keyword in arche, so there is no case
 * for it (the EXPR_ALLOC node kind is dead). */
static const char *resolve_expression_type(SemanticContext *ctx, SyntaxView v) {
	if (!sv_present(v))
		return NULL;
	switch (sv_kind(v)) {
	case SN_PAREN_EXPR:
		return resolve_expression_type(ctx, sem_first_expr(v));

	case SN_LITERAL_EXPR: {
		char *lex = sem_cv_dup_first_token(v);
		const char *r;
		if (lex[0] == '"')
			r = "char_array";
		else if (lex[0] == '\'')
			r = "char";
		else if (strchr(lex, '.') || strchr(lex, 'e') || strchr(lex, 'E'))
			r = "float";
		else
			r = "i32"; /* `i32` is the canonical integer; `int` is its transparent alias */
		free(lex);
		return r;
	}

	case SN_STRING_EXPR:
		return "char_array";

	case SN_NAME_EXPR: {
		char *nm = sv_resolved_name(ctx, v);
		const char *r = resolve_name_type(ctx, nm);
		free(nm);
		return r;
	}

	case SN_FIELD_EXPR: {
		/* `Enum.variant` is a value of the ENUM type (distinct, i32-backed). It folds to its int value
		 * at codegen, but its TYPE is the enum — so `f : fd = fd.stdin` type-checks and a raw int can't
		 * masquerade as a case. */
		if (sv_count(v, SN_FIELD_NAME) == 1) {
			char *idnt = sv_resolved_name(ctx, v);
			if (enum_is_type(ctx, idnt)) {
				char *fld = sem_cv_dup(sv_child_at(v, SN_FIELD_NAME, 0));
				long ev = 0;
				int is_variant = enum_variant_lookup(ctx, idnt, fld, &ev);
				free(fld);
				if (is_variant)
					return sem_own_str(ctx, idnt); /* the enum type name (pool-owned, stable) */
			}
			free(idnt);
		}
		return resolve_base_chain_type(ctx, v);
	}

	case SN_INDEX_EXPR: {
		/* A single index on an N-D matrix const yields a ROW (a slice: `S[i]` is a `char[]`); a full
		 * index set (`M[i, j]`) yields the scalar element. */
		char *bn = sv_name_expr_dup(v);
		int mstride = 0;
		const char *met = bn ? name_const_matrix(ctx, bn, &mstride) : NULL;
		free(bn);
		if (met && mstride > 1) {
			int ni = 0;
			while (sv_present(sem_node_at_expr(v, ni)))
				ni++;
			if (ni <= 1)
				return strcmp(met, "char") == 0 ? "char_array" : met; /* a row slice */
			return met;                                               /* full index → scalar element */
		}
		/* An index yields the base's ELEMENT type; a char buffer ("char_array") indexes to "char". */
		const char *base_type = resolve_base_chain_type(ctx, v);
		if (base_type && strcmp(base_type, "char_array") == 0)
			return "char";
		return base_type;
	}

	case SN_SLICE_EXPR:
		/* a slice has the same element type as its base array */
		return resolve_base_chain_type(ctx, v);

	case SN_BINARY_EXPR: {
		Operator op = sem_binary_op(v);
		if (op >= OP_EQ && op <= OP_GTE)
			return "int"; /* comparisons are boolean */
		const char *left_type = resolve_expression_type(ctx, sem_node_at_expr(v, 0));
		const char *right_type = resolve_expression_type(ctx, sem_node_at_expr(v, 1));
		if (left_type && strcmp(left_type, "double") == 0)
			return "double";
		if (right_type && strcmp(right_type, "double") == 0)
			return "double";
		if (left_type && strcmp(left_type, "float") == 0)
			return "float";
		if (right_type && strcmp(right_type, "float") == 0)
			return "float";
		return left_type ? left_type : right_type;
	}

	case SN_UNARY_EXPR:
		/* `!x` is an int 0/1; `-x`/`move`/`copy` keep the operand's type */
		if (sv_has_token(v, TOK_BANG))
			return "int";
		return resolve_expression_type(ctx, sem_first_expr(v));

	case SN_CALL_EXPR: {
		/* Callee name: the qualify pass recorded the resolved (mangled for `mod.f`) name in the
		 * side model. Fall back to the bare SN_CALLEE_NAME for an unqualified call not recorded;
		 * a qualified call with no recorded name is a non-module field call → unresolved. */
		const char *resolved = ctx->model ? sem_model_callee_name(ctx->model, sv_id(v)) : NULL;
		char *fallback = NULL;
		if (!resolved) {
			if (sv_count(v, SN_FIELD_NAME) != 0)
				return NULL;
			fallback = sem_cv_dup(sv_child(v, SN_CALLEE_NAME));
		}
		const char *func_name = resolved ? resolved : fallback;
		const char *result = NULL;
		if (func_name) {
			if (is_width_int_name(func_name) || is_primitive_type_name(func_name) || is_type_alias(ctx, func_name)) {
				/* `T(x)` cast: result is the (stable canonical) target type, or an alias backing. */
				const char *r = resolve_type_alias(ctx, func_name);
				result = (r == func_name) ? canonical_cast_type(func_name) : r;
			} else if (strcmp(func_name, "insert") == 0) {
				result = "handle"; /* `insert(table<X>, …)` yields a handle (i64) */
			} else {
				GroupInfo *gi = find_group(ctx, func_name);
				if (gi) {
					/* a group: pick the matching member by static arg types */
					int argc = 0;
					while (sv_present(sem_node_at_expr(v, argc)))
						argc++;
					for (int m = 0; m < gi->member_count && !result; m++) {
						DeclSummary *fd = find_func_sig(ctx, gi->members[m]);
						if (!fd || fd->param_count != argc)
							continue;
						int ok = 1;
						for (int j = 0; j < argc; j++) {
							const char *rt = resolve_expression_type(ctx, sem_node_at_expr(v, j));
							TypeId pid = fd->params[j].type_id;
							TyKind pk = tyid_kind(ctx->ty_arena, pid);
							if (!rt || (pk != TYK_NOMINAL && pk != TYK_PRIM)) {
								ok = 0;
								break;
							}
							if (!tyid_equal(sem_tyid_of_name(ctx, rt), pid)) {
								ok = 0;
								break;
							}
						}
						if (ok && fd->return_type_count > 0) {
							TyKind rk = tyid_kind(ctx->ty_arena, fd->return_type_ids[0]);
							if (rk == TYK_NOMINAL || rk == TYK_PRIM)
								result = sem_tyid_name(ctx, fd->return_type_ids[0]);
						}
					}
				} else {
					result = resolve_func_return(ctx, func_name);
				}
			}
		}
		free(fallback);
		return result;
	}

	default:
		return NULL;
	}
}

/* The *nominal* type of an expression for distinctness checks — the unresolved alias name
 * (e.g. "file"), NOT the backing. NULL when the expression has no known nominal alias
 * (a literal, a raw primitive, an unknown). Distinct from resolve_expression_type, which
 * resolves through to the backing for ops/codegen. */
static const char *nominal_type_of_expr(SemanticContext *ctx, SyntaxView v) {
	if (!sv_present(v))
		return NULL;
	SyntaxNodeKind k = sv_kind(v);
	/* `move x` / `copy x` are transparent markers — their nominal type is the operand's. */
	if (k == SN_UNARY_EXPR) {
		if (sv_has_token(v, TOK_MOVE) || sv_has_token(v, TOK_COPY))
			return nominal_type_of_expr(ctx, sem_first_expr(v));
		return NULL;
	}
	if (k == SN_NAME_EXPR) {
		char *nm = sv_resolved_name(ctx, v);
		VariableInfo *var = find_variable(ctx, nm);
		free(nm);
		if (!var)
			return NULL;
		if (var->nominal_type)
			return var->nominal_type;
		/* Fall back to the declared type name when it is a non-transparent alias — covers params and
		 * any var whose nominal_type wasn't recorded at bind time (so an opaque `file` parameter keeps
		 * its distinct identity through a call, not just a fresh `x: file` local). */
		/* A tier-2 distinct subtype value carries its own identity (so `x: file` keeps `file` through a
		 * call). Tier-2 == a nominal with a backing; its name is the distinct alias. */
		if (tyid_kind(ctx->ty_arena, var->type_id) == TYK_NOMINAL &&
		    tyid_backing(ctx->ty_arena, var->type_id) != TYID_UNKNOWN)
			return tyid_nominal_name(ctx->ty_arena, var->type_id);
		/* Untyped `k := Enum.case` records the distinct alias in inferred_type but no type_id — keep the
		 * nominal so `k` stays the enum (not its int backing) through a call. */
		if (var->inferred_type && is_type_alias(ctx, var->inferred_type) &&
		    !alias_is_transparent(ctx, var->inferred_type))
			return var->inferred_type;
		return NULL;
	}
	/* simple (unqualified) callee only — a qualified `mod.f` has SN_FIELD_NAME children */
	if (k == SN_CALL_EXPR && sv_count(v, SN_FIELD_NAME) == 0) {
		char *cn = sem_cv_dup(sv_child(v, SN_CALLEE_NAME));
		DeclSummary *fd = find_func_sig(ctx, cn);
		free(cn);
		/* On the fly: the DeclSummary's return_type_ids are filled post-analysis,
		 * but this runs DURING analysis. */
		TypeId frt = (fd && fd->return_type_count > 0) ? fd->return_type_ids[0] : TYID_UNKNOWN;
		if (tyid_kind(ctx->ty_arena, frt) == TYK_NOMINAL && tyid_backing(ctx->ty_arena, frt) != TYID_UNKNOWN)
			return tyid_nominal_name(ctx->ty_arena, frt);
	}
	return NULL;
}

/* ========== EXPRESSION ANALYSIS ========== */

/* Analyze a FIELD/INDEX/SLICE base = leading IDENT + optional SN_FIELD_NAME chain. Replicates the
 * old recursion's diagnostics: base-name resolution (undefined symbol), and archetype/tuple field
 * checks. The old AST flattened `a.b.c` into nested FIELDs; here we read the flat node directly.
 * `field_loc` is the location used for field diagnostics (the whole expr). */
static void analyze_base_chain(SemanticContext *ctx, SyntaxView v, SourceLoc field_loc) {
	/* General postfix on a NESTED base (`M[i].length`, `f().g`, `a[i][j]`): analyze the base
	 * sub-expression (which catches errors inside it, e.g. `M[bad]`), then accept the trailing
	 * field/index — it is a property or element access on a COMPUTED value, not a flat name, so the
	 * leading-name checks below don't apply. */
	if (has_nested_base(v)) {
		analyze_expression(ctx, base_subexpr(v));
		return;
	}
	char *idnt = sv_resolved_name(ctx, v);
	int nf = sv_count(v, SN_FIELD_NAME);

	/* `Enum.variant` is a compile-time constant — the old code folded it before any symbol/field
	 * check. No diagnostics; its i32 type is recorded by resolve_expression_type. */
	if (nf == 1 && enum_is_type(ctx, idnt)) {
		char *f0 = sem_cv_dup(sv_child_at(v, SN_FIELD_NAME, 0));
		long ev = 0;
		int variant = enum_variant_lookup(ctx, idnt, f0, &ev);
		free(f0);
		if (variant) {
			free(idnt);
			return;
		}
	}

	/* Base name resolution (old recursion into the base NAME). The leftmost IDENT names a variable,
	 * archetype, const, or known func; mark a referenced variable so W0004 doesn't fire. */
	VariableInfo *base_var = find_variable(ctx, idnt);
	if (base_var)
		base_var->is_referenced = 1;

	if (nf == 0) {
		/* a bare name used as an index/slice base — same checks as EXPR_NAME */
		int is_known_func = find_known_func(ctx, idnt);
		int is_arch = find_archetype(ctx, idnt) != NULL;
		int is_const = semantic_get_const_value(ctx, idnt) != NULL;
		if (!is_known_func && !base_var && !is_arch && !is_const)
			sem_emit_undefined_symbol(ctx, field_loc, idnt);
		else if (base_var && base_var->is_consumed)
			sem_emit_use_after_consume(ctx, field_loc, idnt);
		free(idnt);
		return;
	}

	/* Field access. The field of interest is the LAST name in the chain; for nf>=2 the old code's
	 * base is itself a FIELD (not a NAME), so only the nested-tuple expansion `arch.tuple.comp`
	 * fires — replicate that for nf==2 — and otherwise the simple base-NAME field checks apply. */
	char *field_name = sem_cv_dup(sv_child_at(v, SN_FIELD_NAME, nf - 1));

	/* nf>=2: nested `arch.tuple.comp` → arch must have a `tuple_comp` field. Old code only handled a
	 * single level of nesting (base FIELD whose base is a NAME), i.e. nf==2. */
	if (nf == 2) {
		char *tuple_base = sem_cv_dup(sv_child_at(v, SN_FIELD_NAME, 0));
		ArchetypeInfo *arch = find_archetype(ctx, idnt);
		if (!arch && base_var && base_var->archetype_name)
			arch = find_archetype(ctx, base_var->archetype_name);
		if (arch) {
			char expanded[256];
			snprintf(expanded, sizeof(expanded), "%s_%s", tuple_base, field_name);
			if (find_field(arch, expanded)) {
				free(tuple_base);
				free(field_name);
				free(idnt);
				return; /* resolved as a tuple component */
			}
		}
		free(tuple_base);
		/* fall through to simple checks below (base treated as the IDENT) */
	}

	/* Simple `name.field` checks (the old `base->type == EXPR_NAME` path). */
	if (nf == 1) {
		ArchetypeInfo *arch = find_archetype(ctx, idnt);
		if (!arch) {
			if (!base_var) {
				/* A value const can be a field base — e.g. `.length`/`.cap` on a `char[]` string const
				 * (`name :: "linux"`). The length-family fields resolve to int (resolve_field_type). */
				if (semantic_get_const_value(ctx, idnt) != NULL)
					goto done;
				sem_emit_undefined_field_base(ctx, field_loc, idnt);
				goto done;
			}
			TypeArena *A = ctx->ty_arena;
			TypeId bt = base_var->type_id;
			TyKind btk = tyid_kind(A, bt);
			/* reading a component through a HANDLE value is unsupported */
			int base_is_handle =
			    btk == TYK_HANDLE || (base_var->inferred_type && strcmp(base_var->inferred_type, "handle") == 0);
			if (base_is_handle) {
				const char *an = btk == TYK_HANDLE ? tyid_handle_name(A, bt) : NULL;
				sem_emit_cannot_read_through_handle(ctx, field_loc, field_name, idnt, an ? an : "the archetype");
				goto done;
			}
			if (base_var->archetype_name)
				arch = find_archetype(ctx, base_var->archetype_name);
			/* E0111: a typed variable with no field-access shape (skip tuples, archetype params,
			 * sys-column access, and array metadata props). */
			int sys_column_access = 0;
			if (base_var->is_param && ctx->current_sys_archetype) {
				ArchetypeInfo *sa = find_archetype(ctx, ctx->current_sys_archetype);
				if (sa && find_field(sa, idnt))
					sys_column_access = 1;
			}
			if (!arch && bt != TYID_UNKNOWN && !sys_column_access) {
				if (btk == TYK_NOMINAL)
					arch = find_archetype(ctx, tyid_nominal_name(A, bt));
				if (!arch && btk != TYK_TUPLE && btk != TYK_ARCHETYPE_CATEGORY) {
					if ((btk == TYK_SLICE || btk == TYK_ARRAY) &&
					    (strcmp(field_name, "cap") == 0 || strcmp(field_name, "capacity") == 0 ||
					     strcmp(field_name, "length") == 0 || strcmp(field_name, "max_length") == 0))
						goto done;
					/* A static array's VariableInfo carries its ELEMENT type, so `.length`/`.cap` on it
					 * aren't caught above — allow them here (they resolve to int). */
					if (name_is_static_array(ctx, idnt) &&
					    (strcmp(field_name, "cap") == 0 || strcmp(field_name, "capacity") == 0 ||
					     strcmp(field_name, "length") == 0 || strcmp(field_name, "max_length") == 0))
						goto done;
					const char *kind_name;
					switch (btk) {
					case TYK_NOMINAL:
						kind_name = tyid_nominal_name(A, bt); /* archetype/opaque/scalar name */
						break;
					case TYK_SLICE:
						kind_name = "array";
						break;
					case TYK_ARRAY:
						kind_name = "shaped array";
						break;
					default:
						kind_name = "value";
						break;
					}
					sem_emit_field_on_non_archetype(ctx, field_loc, kind_name, field_name);
					goto done;
				}
			}
		}
		/* check the field exists on this archetype */
		if (arch) {
			FieldInfo *found_field = find_field(arch, field_name);
			if (!found_field) {
				int is_tuple_base = 0;
				size_t fl = strlen(field_name);
				for (int i = 0; i < arch->field_count; i++)
					if (strncmp(arch->fields[i]->name, field_name, fl) == 0 && arch->fields[i]->name[fl] == '_') {
						is_tuple_base = 1;
						break;
					}
				if (is_tuple_base) {
					/* tuple field access — codegen expands it */
				} else {
					char expanded2[256];
					snprintf(expanded2, sizeof(expanded2), "%s_%s", idnt, field_name);
					if (!find_field(arch, expanded2))
						sem_emit_no_field(ctx, field_loc, archetype_any_alias(ctx, arch), field_name);
				}
			}
		}
	}
done:
	free(field_name);
	free(idnt);
}

/* The interned type of a whole array VALUE CONST: a fixed `[N]elem` (1-D), or the nested
 * `[rows][stride]elem` for a 2-D matrix (`static_row_stride` is the inner dim). A SUB-slice or partial
 * row is a separate `[]T`/`[stride]T` value; this is the whole aggregate. */
static TypeId array_const_type_id(SemanticContext *ctx, const DeclSummary *d) {
	TypeId elem = d->static_type_id;
	/* Row count = number of top-level elements in the initializer literal. (Robust to `static_size`,
	 * which is the FLAT total for a numeric matrix but the ROW count for a string matrix — counting the
	 * literal sidesteps that bookkeeping difference.) */
	int rows = 0;
	if (sv_present(d->static_init))
		while (sv_present(sem_node_at_expr(d->static_init, rows)))
			rows++;
	if (rows <= 0)
		rows = d->static_size > 0 ? d->static_size : 1;
	if (d->static_row_stride > 1)
		return tyid_of_array(ctx->ty_arena, tyid_of_array(ctx->ty_arena, elem, d->static_row_stride), rows);
	return tyid_of_array(ctx->ty_arena, elem, rows);
}

/* TypeId-native type of an expression for the side model — preserves the slice/array structure that
 * the stringly resolver collapses to its element. A `b[lo:hi]` sub-slice → `[]elem`; a PARTIAL matrix
 * index `M[i]` (a row) → the sized inner `[stride]elem`; a whole array const → its `[N]`/`[N][W]` type;
 * everything else falls back to the string round-trip. This is what makes slice/row/array binds carry
 * the real `[]T`/`[N]T`/`[N][W]T` into the model (so hints, tycheck, and the dangling check see it). */
static TypeId sem_expr_type_id(SemanticContext *ctx, SyntaxView v) {
	SyntaxNodeKind k = sv_kind(v);
	/* A NAME referring to a slice/array variable or param keeps its real `[]T`/`[N]T` type — the
	 * string path collapses it to the element (`sem_tyid_name` of an array yields the element base), so
	 * read `var->type_id` directly. Keeps both sides of a slice assignment (`out = raw[0:n]`) in sync. */
	if (k == SN_NAME_EXPR) {
		char *nm = sv_name_expr_dup(v);
		if (nm) {
			VariableInfo *var = find_variable(ctx, nm);
			if (var && var->type_id != TYID_UNKNOWN) {
				TyKind tk = tyid_kind(ctx->ty_arena, var->type_id);
				if (tk == TYK_SLICE || tk == TYK_ARRAY) {
					free(nm);
					return var->type_id;
				}
			}
			/* a NAME referring to an array/slice VALUE CONST keeps its real `[]T`. Checked even when a
			 * VariableInfo exists, because a static-array const's VariableInfo carries the ELEMENT type
			 * (not the array), so the var check above misses it. Mirrors sem_decl_type_id's array case. */
			for (int i = 0; i < ctx->decl_count; i++) {
				DeclSummary *d = ctx->decls[i];
				if (d->kind == DECL_STATIC && d->static_kind == STATIC_KIND_ARRAY && d->name &&
				    strcmp(d->name, nm) == 0) {
					free(nm);
					return array_const_type_id(ctx, d);
				}
			}
			free(nm);
		}
	}
	if (k == SN_SLICE_EXPR || k == SN_INDEX_EXPR) {
		int row_stride = 0; /* >0 ⇒ a sized matrix ROW `[stride]elem`; a SLICE stays a `[]elem` view */
		int is_slice_val = (k == SN_SLICE_EXPR);
		if (k == SN_INDEX_EXPR) {
			/* a partial index of a matrix const (fewer indices than its rank) is a row */
			char *bn = sv_name_expr_dup(v);
			int stride = 0;
			const char *met = bn ? name_const_matrix(ctx, bn, &stride) : NULL;
			free(bn);
			if (met && stride > 1) {
				int ni = 0;
				while (sv_present(sem_node_at_expr(v, ni)))
					ni++;
				if (ni <= 1) {
					is_slice_val = 1;
					row_stride = stride;
				}
			}
		}
		if (is_slice_val) {
			const char *es = resolve_expression_type(ctx, v); /* element (or "char_array" for a char row) */
			const char *elem = (es && strcmp(es, "char_array") == 0) ? "char" : es;
			TypeId etid = sem_tyid_of_name(ctx, elem ? elem : "int");
			return row_stride > 0 ? tyid_of_array(ctx->ty_arena, etid, row_stride) : tyid_of_slice(ctx->ty_arena, etid);
		}
	}
	const char *resolved = resolve_expression_type(ctx, v);
	const char *nom = nominal_type_of_expr(ctx, v);
	int nomset = nom && is_type_alias(ctx, nom) && !alias_is_transparent(ctx, nom);
	return sem_tyid_of_name(ctx, nomset ? nom : resolved);
}

static void analyze_expression(SemanticContext *ctx, SyntaxView v) {
	if (!sv_present(v))
		return;

	/* Capture-and-clear the call-arg flag (see the old comment): only EXPR_CALL re-sets it per arg. */
	int was_arg = ctx->analyzing_call_arg;
	ctx->analyzing_call_arg = 0;
	SourceLoc loc = sem_node_loc(v.node);

	switch (sv_kind(v)) {
	case SN_LITERAL_EXPR:
	case SN_STRING_EXPR:
	case SN_ARRAY_LIT_EXPR:
		break;

	case SN_PAREN_EXPR:
		analyze_expression(ctx, sem_first_expr(v));
		break;

	case SN_NAME_EXPR: {
		char *name = sv_resolved_name(ctx, v);
		int is_known_func = find_known_func(ctx, name);
		VariableInfo *name_var = find_variable(ctx, name);
		int is_var = name_var != NULL;
		int is_arch = find_archetype(ctx, name) != NULL;
		int is_const = semantic_get_const_value(ctx, name) != NULL;
		if (is_var)
			name_var->is_referenced = 1;
		if (!is_known_func && !is_var && !is_arch && !is_const)
			sem_emit_undefined_symbol(ctx, loc, name);
		else if (is_var && name_var->is_consumed)
			sem_emit_use_after_consume(ctx, loc, name);
		free(name);
		break;
	}

	case SN_FIELD_EXPR:
		analyze_base_chain(ctx, v, loc);
		break;

	case SN_INDEX_EXPR:
		analyze_base_chain(ctx, v, loc);
		/* not-indexable (E0201): a bare scalar variable cannot be subscripted. Fire ONLY when the
		 * base is a DIRECT variable (no field chain) with an explicit scalar declared type. This is
		 * the only false-positive-free signal: `resolve_base_chain_type` returns an array's ELEMENT
		 * type (so `b: int[N]; b[i]` would look like a scalar `int` base), and inferred/unknown bases
		 * are ambiguous — leave arrays, strings, handles, opaques, and inferred bases alone. */
		if (sv_count(v, SN_FIELD_NAME) == 0) {
			char *bn = sv_resolved_name(ctx, v);
			VariableInfo *bv = bn ? find_variable(ctx, bn) : NULL;
			TyKind bvk = bv ? tyid_kind(ctx->ty_arena, bv->type_id) : TYK_UNKNOWN;
			if (bv && (bvk == TYK_PRIM || bvk == TYK_NOMINAL) && !name_is_static_array(ctx, bn)) {
				const char *rn = sem_tyid_name(ctx, bv->type_id);
				if (rn && is_primitive_type_name(rn) && strcmp(rn, "str") != 0 && strcmp(rn, "void") != 0)
					sem_emit_not_indexable(ctx, loc, rn);
			}
			free(bn);
		}
		for (int i = 0; sv_present(sem_node_at_expr(v, i)); i++)
			analyze_expression(ctx, sem_node_at_expr(v, i));
		break;

	case SN_SLICE_EXPR:
		/* a read-only borrowed sub-view: base + optional bounds, consumes nothing */
		analyze_base_chain(ctx, v, loc);
		for (int i = 0; sv_present(sem_node_at_expr(v, i)); i++)
			analyze_expression(ctx, sem_node_at_expr(v, i));
		break;

	case SN_BINARY_EXPR:
		analyze_expression(ctx, sem_node_at_expr(v, 0));
		analyze_expression(ctx, sem_node_at_expr(v, 1));
		break;

	case SN_UNARY_EXPR: {
		SyntaxView operand = sem_first_expr(v);
		analyze_expression(ctx, operand);
		(void)was_arg;
		/* `move x` transfers ownership: mark x consumed; can't move out of a borrowed param. */
		if (sv_has_token(v, TOK_MOVE) && sv_present(operand) && sv_kind(operand) == SN_NAME_EXPR) {
			char *nm = sv_name_expr_dup(operand);
			VariableInfo *mv = find_variable(ctx, nm);
			if (mv) {
				if (mv->is_param && !mv->is_own && type_is_byref_aggregate(ctx->ty_arena, mv->type_id))
					sem_emit_cannot_move_borrowed(ctx, sem_node_loc(operand.node), nm);
				mv->is_consumed = 1;
			}
			free(nm);
		}
		/* `copy x` duplicates x (x kept); opaque is non-copyable; clone only for local `T[N]`. */
		if (sv_has_token(v, TOK_COPY) && sv_present(operand) && sv_kind(operand) == SN_NAME_EXPR) {
			char *nm = sv_name_expr_dup(operand);
			VariableInfo *cv = find_variable(ctx, nm);
			if (cv) {
				if (var_is_opaque(ctx, cv))
					sem_emit_cannot_copy_opaque(ctx, sem_node_loc(operand.node), nm);
				else if (type_is_byref_aggregate(ctx->ty_arena, cv->type_id) &&
				         !(tyid_kind(ctx->ty_arena, cv->type_id) == TYK_ARRAY && !cv->is_param))
					sem_emit_copy_unsupported(ctx, sem_node_loc(operand.node), nm);
			}
			free(nm);
		}
		break;
	}

	case SN_CALL_EXPR: {
		int call_stmt_ok = ctx->stmt_call_ok;
		int outlist_call_ok = ctx->proc_call_stmt_ok;
		ctx->stmt_call_ok = 0;
		ctx->proc_call_stmt_ok = 0;

		/* resolved callee name (qualify-mangled for `mod.f`) from the side model; NULL for a
		 * non-module qualified field call. */
		const char *resolved_callee = ctx->model ? sem_model_callee_name(ctx->model, sv_id(v)) : NULL;
		char *func_name = NULL;
		if (resolved_callee)
			func_name = sem_dupz(resolved_callee);
		else if (sv_count(v, SN_FIELD_NAME) == 0)
			func_name = sem_cv_dup(sv_child(v, SN_CALLEE_NAME));

		int argc = sem_expr_count(v);

		/* Type conversion `T(x)`: callee is a type name; analyze the args and stop. But constructing an
		 * opaque this way is illegal — opaque handles are born only at the FFI boundary, never minted in
		 * arche, so `res(5)` for an opaque `res` is rejected. */
		if (func_name && is_type_conversion_callee(ctx, func_name)) {
			const char *cb = resolve_type_alias(ctx, func_name);
			if (cb && strcmp(cb, "opaque") == 0)
				sem_emit_opaque_construct(ctx, sem_node_loc(v.node), func_name);
			for (int i = 0; i < argc; i++) {
				ctx->analyzing_call_arg = 1;
				analyze_expression(ctx, sem_node_at_expr(v, i));
			}
			free(func_name);
			break;
		}

		/* Analyze the callee name (the old recursion into the callee EXPR_NAME): a bare reference
		 * that is not a known func / variable / archetype / const is an undefined symbol — e.g. an
		 * unqualified module export `fread` or a typo `definitely_not_defined`. */
		if (func_name) {
			/* Visibility gate: a callee resolving ONLY to a `#module`/`#file` private extern of another
			 * unit is out of scope — binding to it would leak that device's raw implementation past its
			 * public API. Suppress BOTH callable paths (known-func table + decl table) when the name is
			 * hidden-cross-unit, so it falls through to the undefined-symbol error. sem_callable_visible
			 * returns 1 visible, 0 hidden-only-cross-unit, -1 none. (Pure private decls are renamed to
			 * `mod.name`, so a bare name never reaches them; only un-mangled `#foreign` externs need this.) */
			int hidden = sem_callable_visible(ctx, func_name, sem_ref_unit(ctx)) == 0;
			int is_known_func = !hidden && find_known_func(ctx, func_name);
			int is_group = find_group(ctx, func_name) != NULL;
			VariableInfo *cv = find_variable(ctx, func_name);
			int is_arch = find_archetype(ctx, func_name) != NULL;
			int is_const = semantic_get_const_value(ctx, func_name) != NULL;
			int is_decl = !hidden && find_callable_sig(ctx, func_name) != NULL;
			if (cv)
				cv->is_referenced = 1;
			if (!is_known_func && !is_group && !is_decl && !cv && !is_arch && !is_const)
				sem_emit_undefined_symbol(ctx, loc, func_name);
			else if (cv && cv->is_consumed)
				sem_emit_use_after_consume(ctx, loc, func_name);
		}

		/* Resolve a callee proc up front to validate `_` placeholder args (legal only in an
		 * in-out in-slot). */
		DeclSummary *call_callee_proc = func_name ? find_proc_sig(ctx, func_name) : NULL;
		for (int i = 0; i < argc; i++) {
			SyntaxView arg = sem_node_at_expr(v, i);
			if (sv_kind(arg) == SN_NAME_EXPR && sv_count(arg, SN_FIELD_NAME) == 0 && sv_text_eq(arg, "_")) {
				if (!call_callee_proc || !proc_param_is_inout(call_callee_proc, i))
					sem_emit_underscore_not_inout(ctx, sem_node_loc(arg.node));
				continue;
			}
			ctx->analyzing_call_arg = 1;
			analyze_expression(ctx, arg);
		}
		if (!func_name)
			break;

		/* insert(Foo, v…) consumes any opaque-backed value args (arg 0 is the archetype). */
		if (strcmp(func_name, "insert") == 0) {
			for (int i = 1; i < argc; i++) {
				SyntaxView a = sem_node_at_expr(v, i);
				if (sv_kind(a) == SN_NAME_EXPR && sv_count(a, SN_FIELD_NAME) == 0) {
					char *nm = sv_name_expr_dup(a);
					VariableInfo *iv = find_variable(ctx, nm);
					if (iv && var_is_opaque(ctx, iv))
						iv->is_consumed = 1;
					free(nm);
				}
			}
		}

		/* Mandatory-ok: insert/delete report success as a value, so they are statement-only — they must
		 * be the value of a proc-call statement carrying an out-list. A bare `insert(…)`, an `x := insert(…)`
		 * bind, or a nested `i32(insert(…))` (any context with !call_stmt_ok) is rejected here. The valid
		 * `insert(P,…)(h:, ok:)` / `delete(h)(ok:)` form analyzes its call with call_stmt_ok set. */
		if (strcmp(func_name, "insert") == 0 && !outlist_call_ok)
			sem_emit_insert_delete_outlist(ctx, loc, "insert", "insert(P, …)(handle:, ok:)");
		if (strcmp(func_name, "delete") == 0 && !outlist_call_ok)
			sem_emit_insert_delete_outlist(ctx, loc, "delete", "delete(h)(ok:)");
		if (strcmp(func_name, "insert") == 0)
			sem_check_insert_handler(ctx, v, loc); /* validate the `?handler` (sigil + @policy(pool)) */

		GroupInfo *gi = find_group(ctx, func_name);
		if (!gi) {
			DeclSummary *cs = find_callable_sig(ctx, func_name);
			if (cs) {
				int param_count = cs->param_count;
				ParamSummary *params = cs->params;
				int is_extern = cs->is_extern;
				int is_proc = cs->kind == DECL_PROC;
				/* value/action boundary: a proc/extern is an action — not nestable in an expr. */
				if ((is_proc || is_extern) && !call_stmt_ok)
					sem_emit_action_in_expression(ctx, loc, is_extern ? "extern" : "proc", func_name);
				int n = param_count < argc ? param_count : argc;
				/* explicit-view: record the resolved param (name + own) per argument node id */
				for (int j = 0; j < n; j++) {
					ParamSummary *p = &params[j];
					SyntaxView a = sem_node_at_expr(v, j);
					if (p->name && sv_present(a))
						sem_hints_set_param(ctx->hints, sv_id(a), p->name, p->is_own);
				}
				/* own param: a bare move-only name is an implicit move */
				for (int j = 0; j < n; j++) {
					if (!params[j].is_own)
						continue;
					implicit_move_consume(ctx, sem_node_at_expr(v, j));
				}
				/* a `T[]` slice cannot satisfy a sized `T[N]` parameter */
				for (int j = 0; j < n; j++) {
					ParamSummary *p = &params[j];
					if (tyid_kind(ctx->ty_arena, p->type_id) != TYK_ARRAY)
						continue;
					SyntaxView a = sem_node_at_expr(v, j);
					while (sv_present(a) && sv_kind(a) == SN_UNARY_EXPR &&
					       (sv_has_token(a, TOK_MOVE) || sv_has_token(a, TOK_COPY)))
						a = sem_first_expr(a);
					if (sv_present(a) && sv_kind(a) == SN_NAME_EXPR && sv_count(a, SN_FIELD_NAME) == 0) {
						char *nm = sv_name_expr_dup(a);
						VariableInfo *av = find_variable(ctx, nm);
						if (av && tyid_kind(ctx->ty_arena, av->type_id) == TYK_SLICE) {
							fprintf(stderr,
							        "Error: cannot pass a slice `T[]` to a sized `T[N]` parameter '%s' — a "
							        "slice's length is only known at runtime; sizing flows one way "
							        "(`T[N]` decays to `T[]`), never back\n",
							        p->name ? p->name : "?");
							ctx->error_count++;
						}
						free(nm);
					}
				}
				/* (Extern-arg nominal distinctness is handled by the general per-argument nominal check,
				 * which reports it in the unified `expected 'X', got 'Y'` vocabulary — no extern-only
				 * special case. The former bolted-on check here was redundant and emitted a duplicate
				 * diagnostic.) */
			}
			free(func_name);
			break;
		}

		/* group: overload resolution by static arg types (only when every arg is a concrete prim) */
		int can_diagnose = 1;
		for (int j = 0; j < argc; j++) {
			const char *rt = resolve_expression_type(ctx, sem_node_at_expr(v, j));
			if (!rt) {
				can_diagnose = 0;
				break;
			}
			const char *nrt = normalize_type_name(rt);
			if (strcmp(nrt, "int") != 0 && strcmp(nrt, "float") != 0 && strcmp(nrt, "char") != 0) {
				can_diagnose = 0;
				break;
			}
		}
		if (can_diagnose) {
			int match_count = 0;
			for (int m = 0; m < gi->member_count; m++) {
				DeclSummary *fd = find_func_sig(ctx, gi->members[m]);
				if (!fd || fd->param_count != argc)
					continue;
				int ok = 1;
				for (int j = 0; j < argc; j++) {
					const char *rt = resolve_expression_type(ctx, sem_node_at_expr(v, j));
					TypeId pid = fd->params[j].type_id;
					TyKind pk = tyid_kind(ctx->ty_arena, pid);
					if (!rt || (pk != TYK_NOMINAL && pk != TYK_PRIM)) {
						ok = 0;
						break;
					}
					if (!tyid_equal(sem_tyid_of_name(ctx, rt), pid)) {
						ok = 0;
						break;
					}
				}
				if (ok)
					match_count++;
			}
			if (match_count == 0)
				sem_emit_no_group_match(ctx, loc, func_name);
			else if (match_count > 1)
				sem_emit_ambiguous_group_call(ctx, loc, func_name);
		}
		free(func_name);
		break;
	}

	default:
		break;
	}

	/* Resolve + record this expression's type in the side model, keyed by node id. The interned id
	 * prefers the distinct tier-2 nominal (so a `meters` value keeps its identity); else the resolved
	 * backing. This is the single home for an expression's type — read by tycheck + lowering. */
	if (ctx->model) {
		uint32_t nid = sv_id(v);
		sem_model_set_expr_type_id(ctx->model, nid, sem_expr_type_id(ctx, v));
	}
}

/* A bare name in an ownership-TAKING position (bind/assign RHS, `own`-param arg) is an implicit
 * `move` for move-only types (arrays/slices, opaque): consume the binding and record the elided
 * `move` for the editor. A read-only borrowed param can't be moved out (error). Scalars and other
 * `Copy` types are never consumed by a bare name — they copy, as before. Explicit `move`/`copy`
 * are handled in EXPR_UNARY and never reach here as a bare EXPR_NAME. */
static void implicit_move_consume(SemanticContext *ctx, SyntaxView v) {
	if (!sv_present(v) || sv_kind(v) != SN_NAME_EXPR)
		return;
	char *nm = sv_resolved_name(ctx, v);
	VariableInfo *var = find_variable(ctx, nm);
	if (!var || !(type_is_byref_aggregate(ctx->ty_arena, var->type_id) || var_is_opaque(ctx, var))) {
		free(nm); /* no var, or a Copy type: a bare name copies, never moves */
		return;
	}
	if (var->is_param && !var->is_own && type_is_byref_aggregate(ctx->ty_arena, var->type_id)) {
		sem_emit_cannot_move_borrowed(ctx, sem_node_loc(v.node), nm);
		free(nm);
		return;
	}
	free(nm);
	var->is_consumed = 1;
	uint32_t nid = sv_id(v);
	sem_hints_set_elided_move(ctx->hints, nid);
	/* Record the elided move in the lowering model too: lowering materializes an explicit
	 * `move` HIR node here so codegen sees the transfer in the program (HIR), not by
	 * re-deriving the rule from syntax. This is what suppresses the source's RAII auto-drop. */
	if (ctx->model)
		sem_model_set_implicit_move(ctx->model, nid);
}

/* ========== STATEMENT ANALYSIS ========== */

int sem_stmt_count(SyntaxView v) {
	int c = 0;
	for (int i = 0; i < v.node->child_count; i++)
		if (v.node->children[i].tag == SE_NODE) {
			SyntaxNodeKind k = v.node->children[i].as.node->kind;
			if (k >= SN_BIND_STMT && k <= SN_MATCH_STMT)
				c++;
		}
	return c;
}
SyntaxView sem_stmt_at(SyntaxView v, int idx) {
	int c = 0;
	for (int i = 0; i < v.node->child_count; i++)
		if (v.node->children[i].tag == SE_NODE) {
			SyntaxNodeKind k = v.node->children[i].as.node->kind;
			if (k >= SN_BIND_STMT && k <= SN_MATCH_STMT) {
				if (c == idx)
					return (SyntaxView){v.node->children[i].as.node, v.src};
				c++;
			}
		}
	return (SyntaxView){NULL, v.src};
}
static char *sem_type_ref_name(SyntaxView t);
static int sem_type_ref_alias_marked(SyntaxView t);

/* One out-target of a multi-bind (`a, b := f()`) or proc-call (`f(in)(out)`). */
typedef struct {
	char *name;     /* owned by the array; caller frees */
	int is_new;     /* a fresh binding (`:`) vs an assignment to an existing place */
	TypeId type_id; /* interned explicit target type (TYID_UNKNOWN if none) */
} MbTarget;

/* Read the out-targets directly from the view, replacing the transient `cst_build_stmt` whose only
 * use was reaching `.data.multi_bind.targets`. Mirrors the two AST builders' target walks exactly;
 * types are pooled on ctx (no detach dance needed), names are owned by the returned array. Returns
 * the count and sets `*out` to a calloc'd array the caller frees (each name + the array). */
static int sem_read_mb_targets(SemanticContext *ctx, SyntaxView v, MbTarget **out) {
	int cap = v.node->child_count > 0 ? v.node->child_count : 1;
	MbTarget *ts = calloc(cap, sizeof(MbTarget));
	int n = 0;
	if (sv_kind(v) == SN_PROC_CALL_STMT) {
		for (int i = 0; i < v.node->child_count; i++) {
			if (v.node->children[i].tag != SE_NODE || v.node->children[i].as.node->kind != SN_OUT_ARG)
				continue;
			SyntaxView cnv = {v.node->children[i].as.node, v.src};
			ts[n].name = sem_txt_dup(sv_token(cnv, TOK_IDENT));
			ts[n].is_new = sv_has_token(cnv, TOK_COLON);
			ts[n].type_id = sem_intern_view(ctx, sem_type_at(cnv, 0));
			n++;
		}
		*out = ts;
		return n;
	}
	/* SN_MULTI_BIND_STMT: walk the pre-`=` children. `paren` form keeps per-target `:` newness; the
	 * shorthand form makes every target new. */
	int eq_idx = -1, lparen_idx = -1;
	for (int i = 0; i < v.node->child_count; i++)
		if (v.node->children[i].tag == SE_TOKEN) {
			TokenKind tk = v.node->children[i].as.token.kind;
			if (tk == TOK_LPAREN && lparen_idx < 0)
				lparen_idx = i;
			if (tk == TOK_EQ) {
				eq_idx = i;
				break;
			}
		}
	int paren = (lparen_idx >= 0 && lparen_idx < eq_idx);
	const char *pend = NULL;
	int pend_len = 0, pend_active = 0, pend_new = 0;
	TypeId pend_type_id = TYID_UNKNOWN;
#define SEM_MB_FLUSH()                                                                                                 \
	do {                                                                                                               \
		if (pend_active) {                                                                                             \
			ts[n].name = sem_txt_dup((SynText){pend, (size_t)pend_len});                                               \
			ts[n].is_new = paren ? pend_new : 1;                                                                       \
			ts[n].type_id = pend_type_id;                                                                              \
			n++;                                                                                                       \
			pend_active = 0;                                                                                           \
			pend_new = 0;                                                                                              \
			pend_type_id = TYID_UNKNOWN;                                                                               \
		}                                                                                                              \
	} while (0)
	for (int i = 0; i < eq_idx; i++) {
		SyntaxElem *ch = &v.node->children[i];
		if (ch->tag == SE_NODE) {
			SyntaxNodeKind k = ch->as.node->kind;
			if (k == SN_NAME_EXPR) {
				SEM_MB_FLUSH();
				SynText t = sv_token((SyntaxView){ch->as.node, v.src}, TOK_IDENT);
				pend = t.ptr;
				pend_len = (int)t.len;
				pend_active = 1;
			} else if (k >= SN_TYPE_REF && k <= SN_TYPE_FUNC && pend_active) {
				pend_type_id = sem_intern_view(ctx, (SyntaxView){ch->as.node, v.src});
			}
			continue;
		}
		TokenKind tk = ch->as.token.kind;
		if (tk == TOK_IDENT) {
			SEM_MB_FLUSH();
			pend = v.src + ch->as.token.offset;
			pend_len = (int)ch->as.token.length;
			pend_active = 1;
		} else if (tk == TOK_COLON && pend_active) {
			pend_new = 1;
		} else if (tk == TOK_COMMA) {
			SEM_MB_FLUSH();
		}
	}
	SEM_MB_FLUSH();
#undef SEM_MB_FLUSH
	*out = ts;
	return n;
}

static void analyze_statement(SemanticContext *ctx, SyntaxView v) {
	if (!sv_present(v))
		return;
	SourceLoc loc = sem_node_loc(v.node);

	switch (sv_kind(v)) {
	case SN_BIND_STMT: {
		/* `:` introduces the value ⇒ constant; `=` ⇒ variable (mirror cst_build_stmt). */
		int n_colon = 0, has_eq = 0;
		for (int i = 0; i < v.node->child_count; i++)
			if (v.node->children[i].tag == SE_TOKEN) {
				TokenKind tk = v.node->children[i].as.token.kind;
				if (tk == TOK_COLON)
					n_colon++;
				else if (tk == TOK_EQ)
					has_eq = 1;
			}
		int is_const = (!has_eq && n_colon >= 2);
		SyntaxView t0 = sem_type_at(v, 0), t1 = sem_type_at(v, 1);
		SynText t0name = sv_present(t0) ? sv_token(t0, TOK_IDENT) : (SynText){NULL, 0};
		int t0_is_meta = t0name.ptr && t0name.len == 4 && memcmp(t0name.ptr, "type", 4) == 0;

		char *bind_name = sem_cv_dup(sem_node_at_expr(v, 0));
		int has_btype = 0; /* an explicit non-type-form type was written (`x : T = …`) */
		TypeId btype_id = TYID_UNKNOWN;
		int has_type_form = 0;         /* `x : type : <T>` — a local type alias */
		const char *tf_backing = NULL; /* type-form backing name (leaked to the alias registry) */
		int tf_transparent = 0;
		SyntaxView value_view = {NULL, v.src};
		if (is_const && t0_is_meta && sv_present(t1)) {
			has_type_form = 1;
			if (sv_kind(t1) == SN_TYPE_REF) {
				char *raw = sem_type_ref_name(t1); /* alias-marker stripped, qualified folded */
				if (strcmp(raw, "archetype") == 0 || strcmp(raw, "type") == 0)
					free(raw);
				else {
					tf_backing = sem_own_str(ctx, raw); /* registry borrows; pool frees at teardown */
					tf_transparent = sem_type_ref_alias_marked(t1);
				}
			}
		} else {
			if (sv_present(t0)) {
				has_btype = 1;
				btype_id = sem_intern_view(ctx, t0);
			}
			value_view = sem_node_at_expr(v, 1);
		}

		/* A local constant whose RHS denotes a TYPE is a local nominal type alias. */
		if (is_const) {
			const char *backing = NULL;
			if (has_type_form)
				backing = tf_backing;
			else if (sv_present(value_view) && sv_kind(value_view) == SN_NAME_EXPR) {
				char *vn = sv_resolved_name(ctx, value_view);
				if (name_denotes_type(ctx, vn))
					backing = sem_own_str(ctx, vn); /* registry borrows; pool frees at teardown */
				else
					free(vn);
			}
			if (backing || has_type_form) {
				if (!backing) {
					sem_emit_local_alias_invalid_backing(ctx, loc);
					free(bind_name);
				} else {
					/* The alias registry BORROWS the name (and backing) by pointer; both are owned by the
					 * context string pool (freed at teardown), so bind_name is handed to the pool here. */
					register_type_alias_tiered(ctx, sem_own_str(ctx, bind_name), backing, tf_transparent, loc, 0);
					const char *resolved = resolve_type_alias(ctx, bind_name);
					if (!is_primitive_type_name(resolved) && strcmp(resolved, "opaque") != 0)
						sem_emit_type_alias_unknown_backing(ctx, loc, bind_name, resolved);
				}
				if (ctx->model)
					sem_model_set_bind_alias(ctx->model, sv_id(v));
				break; /* compile-time only */
			}
		}
		/* `archetype` is only valid as a parameter type. */
		if (has_btype && tyid_kind(ctx->ty_arena, btype_id) == TYK_ARCHETYPE_CATEGORY) {
			sem_emit_archetype_only_as_param(ctx, loc);
			free(bind_name);
			break;
		}
		if (reject_meta_type(ctx, btype_id, loc, "variable type")) {
			free(bind_name);
			break;
		}
		/* Single-value bind: analyze value, implicit move, declare. */
		ctx->stmt_call_ok = (sv_present(value_view) && sv_kind(value_view) == SN_CALL_EXPR);
		analyze_expression(ctx, value_view);
		ctx->stmt_call_ok = 0;
		/* Record the BINDING's type onto the bind node id — the uniform key every binding's type reads
		 * (locals + file-scope decls), so the reader needs no per-kind value-node logic. UNCONDITIONAL so
		 * the node→type map is complete: annotated → the DECLARED type (btype_id), else → type-of(RHS).
		 * Keyed on `v` (the bind node), disjoint from the value node's own slot. */
		if (ctx->model)
			sem_model_set_expr_type_id(ctx->model, sv_id(v),
			                           has_btype                ? btype_id
			                           : sv_present(value_view) ? sem_model_expr_type_id(ctx->model, sv_id(value_view))
			                                                    : TYID_UNKNOWN);
		implicit_move_consume(ctx, value_view);

		check_shadows_callable(ctx, bind_name, loc);
		add_variable(ctx, bind_name, btype_id);
		/* Borrow-source taint: a bind whose RHS is a freshly-formed slice/row of a FRESH LOCAL array
		 * (`r := loc[0:3]` / `r := M[i]` of a local matrix) makes `r` borrow stack storage that is
		 * reclaimed on return — so `return r` would dangle. Mark it (propagating from a tainted base) so
		 * the return check rejects it, WITHOUT the over-broad "is it a byref local" proxy that also trips
		 * safe own-buffer / param-borrow returns. A slice of a param/own/global is NOT tainted. */
		if (!has_btype && sv_present(value_view) &&
		    (sv_kind(value_view) == SN_SLICE_EXPR || sv_kind(value_view) == SN_INDEX_EXPR)) {
			char *vbn = sv_name_expr_dup(value_view);
			VariableInfo *vbv = vbn ? find_variable(ctx, vbn) : NULL;
			free(vbn);
			int base_is_fresh_local = vbv && !vbv->is_param && type_is_byref_aggregate(ctx->ty_arena, vbv->type_id);
			if ((base_is_fresh_local || (vbv && vbv->borrows_local))) {
				VariableInfo *rv2 = find_variable(ctx, bind_name);
				if (rv2)
					rv2->borrows_local = 1;
			}
		}

		/* Type annotation → inferred/nominal type. */
		if (ctx->scope_count > 0) {
			Scope *scope = &ctx->scopes[ctx->scope_count - 1];
			if (scope->var_count > 0) {
				VariableInfo *var = scope->vars[scope->var_count - 1];
				if (has_btype) {
					TyKind bk = tyid_kind(ctx->ty_arena, btype_id);
					if (bk == TYK_HANDLE) {
						var->inferred_type = tyid_handle_name(ctx->ty_arena, btype_id);
					} else if (bk == TYK_NOMINAL || bk == TYK_PRIM) {
						var->inferred_type = sem_tyid_name(ctx, btype_id);
						const char *nn = tyid_nominal_name(ctx->ty_arena, btype_id);
						if (nn && is_type_alias(ctx, nn) && !alias_is_transparent(ctx, nn))
							var->nominal_type = nn;
					} else if (bk == TYK_ARRAY || bk == TYK_SLICE) {
						TypeId et = btype_id;
						TyKind ek;
						while ((ek = tyid_kind(ctx->ty_arena, et)) == TYK_ARRAY || ek == TYK_SLICE)
							et = tyid_elem(ctx->ty_arena, et);
						ek = tyid_kind(ctx->ty_arena, et);
						var->inferred_type = (ek == TYK_PRIM || ek == TYK_NOMINAL) ? sem_tyid_name(ctx, et) : NULL;
					}
				} else if (sv_present(value_view)) {
					/* Untyped bind: infer the variable's type from its value (stable resolved name). */
					var->inferred_type = resolve_expression_type(ctx, value_view);
				}
			}
		}
		/* A value const (`k :: 5`) reaching here — not a type alias — is immutable. */
		if (is_const)
			mark_last_const(ctx);
		free(bind_name);
		break;
	}

	case SN_ASSIGN_STMT: {
		SyntaxView target = sem_node_at_expr(v, 0);
		SyntaxView value = sem_node_at_expr(v, 1);
		analyze_expression(ctx, target);
		ctx->stmt_call_ok = (sv_present(value) && sv_kind(value) == SN_CALL_EXPR);
		analyze_expression(ctx, value);
		ctx->stmt_call_ok = 0;
		implicit_move_consume(ctx, value);
		if (sv_kind(target) == SN_NAME_EXPR) {
			char *tn = sv_resolved_name(ctx, target);
			VariableInfo *t = find_variable(ctx, tn);
			if (t && t->is_const)
				sem_emit_assign_to_const(ctx, sem_node_loc(target.node), tn);
			else if (t && t->is_consumed)
				sem_emit_assign_after_move(ctx, sem_node_loc(target.node), tn);
			free(tn);
		}
		/* An aggregate value const (`XS :: {…}`) is immutable — reject both `XS = …` and an element
		 * write `XS[i] = …` (its backing global is read-only). The leftmost IDENT of the target is the
		 * base name in both forms. */
		{
			char *tln = sv_name_expr_dup(target);
			if (tln && name_is_const_static_array(ctx, tln))
				sem_emit_assign_to_const(ctx, sem_node_loc(target.node), tln);
			free(tln);
		}
		/* Purity: a borrowed (non-`move`) array parameter is read-only. Uses the leftmost name. */
		{
			const char *rn = ctx->model ? sem_model_ref_name(ctx->model, sv_id(target)) : NULL;
			char *ln = rn ? sem_dupz(rn) : sv_name_expr_dup(target);
			VariableInfo *pv = ln ? find_variable(ctx, ln) : NULL;
			if (pv && pv->is_param && !pv->is_own && !pv->is_out_place &&
			    type_is_byref_aggregate(ctx->ty_arena, pv->type_id))
				sem_emit_cannot_mutate_borrowed(ctx, loc, ln);
			free(ln);
		}
		break;
	}

	case SN_FOR_STMT: {
		/* C-style `for ( init ; cond ; incr ) { body }` vs infinite/conditional `for [cond] { body }`. */
		if (sv_has_token(v, TOK_LPAREN)) {
			push_scope(ctx);
			int seen_brace = 0, seg = 0;
			for (int i = 0; i < v.node->child_count; i++) {
				SyntaxElem *ch = &v.node->children[i];
				if (ch->tag == SE_TOKEN) {
					if (ch->as.token.kind == TOK_LBRACE)
						seen_brace = 1;
					else if (ch->as.token.kind == TOK_SEMI && !seen_brace && seg < 2)
						seg++;
					continue;
				}
				if (seen_brace)
					continue;
				SyntaxNodeKind k = ch->as.node->kind;
				SyntaxView cv = {ch->as.node, v.src};
				if (seg == 0 && k >= SN_BIND_STMT && k <= SN_MATCH_STMT)
					analyze_statement(ctx, cv);
				else if (seg == 1 && k >= SN_LITERAL_EXPR && k <= SN_PAREN_EXPR)
					analyze_expression(ctx, cv);
				else if (seg == 2 && k >= SN_BIND_STMT && k <= SN_MATCH_STMT)
					analyze_statement(ctx, cv);
			}
			/* body (statements after `{`) */
			seen_brace = 0;
			for (int i = 0; i < v.node->child_count; i++) {
				SyntaxElem *ch = &v.node->children[i];
				if (ch->tag == SE_TOKEN) {
					if (ch->as.token.kind == TOK_LBRACE)
						seen_brace = 1;
					continue;
				}
				if (!seen_brace)
					continue;
				SyntaxNodeKind k = ch->as.node->kind;
				if (k >= SN_BIND_STMT && k <= SN_MATCH_STMT)
					analyze_statement(ctx, (SyntaxView){ch->as.node, v.src});
			}
			pop_scope(ctx);
			break;
		}
		/* infinite/conditional: an expr child (if present) is the condition, statement children the body */
		SyntaxView cond = sem_node_at_expr(v, 0);
		if (sv_present(cond))
			analyze_expression(ctx, cond);
		push_scope(ctx);
		for (int i = 0, n = sem_stmt_count(v); i < n; i++)
			analyze_statement(ctx, sem_stmt_at(v, i));
		pop_scope(ctx);
		break;
	}

	case SN_IF_STMT: {
		analyze_expression(ctx, sem_node_at_expr(v, 0));

		/* RAII all-paths-or-none consumption analysis (verbatim, operates on ctx scopes). */
		int snap_count = 0;
		for (int si = 0; si < ctx->scope_count; si++)
			for (int vi = 0; vi < ctx->scopes[si].var_count; vi++)
				if (var_is_opaque(ctx, ctx->scopes[si].vars[vi]))
					snap_count++;
		VariableInfo **snap_vars = snap_count ? malloc(snap_count * sizeof(VariableInfo *)) : NULL;
		int *snap_before = snap_count ? malloc(snap_count * sizeof(int)) : NULL;
		int *snap_then = snap_count ? malloc(snap_count * sizeof(int)) : NULL;
		{
			int k = 0;
			for (int si = 0; si < ctx->scope_count; si++)
				for (int vi = 0; vi < ctx->scopes[si].var_count; vi++) {
					VariableInfo *vv = ctx->scopes[si].vars[vi];
					if (var_is_opaque(ctx, vv)) {
						snap_vars[k] = vv;
						snap_before[k] = vv->is_consumed;
						k++;
					}
				}
		}

		/* then-body = statement children before TOK_ELSE; else-body = after. */
		int else_seen = 0;
		push_scope(ctx);
		for (int i = 0; i < v.node->child_count; i++) {
			SyntaxElem *ch = &v.node->children[i];
			if (ch->tag == SE_TOKEN && ch->as.token.kind == TOK_ELSE) {
				else_seen = 1;
				continue;
			}
			if (else_seen || ch->tag != SE_NODE)
				continue;
			SyntaxNodeKind k = ch->as.node->kind;
			if (k >= SN_BIND_STMT && k <= SN_MATCH_STMT)
				analyze_statement(ctx, (SyntaxView){ch->as.node, v.src});
		}
		pop_scope(ctx);

		for (int k = 0; k < snap_count; k++) {
			snap_then[k] = snap_vars[k]->is_consumed;
			snap_vars[k]->is_consumed = snap_before[k];
		}

		push_scope(ctx);
		else_seen = 0;
		for (int i = 0; i < v.node->child_count; i++) {
			SyntaxElem *ch = &v.node->children[i];
			if (ch->tag == SE_TOKEN && ch->as.token.kind == TOK_ELSE) {
				else_seen = 1;
				continue;
			}
			if (!else_seen || ch->tag != SE_NODE)
				continue;
			SyntaxNodeKind k = ch->as.node->kind;
			if (k >= SN_BIND_STMT && k <= SN_MATCH_STMT)
				analyze_statement(ctx, (SyntaxView){ch->as.node, v.src});
		}
		pop_scope(ctx);

		for (int k = 0; k < snap_count; k++) {
			if (snap_before[k])
				continue;
			int then_c = snap_then[k];
			int else_c = snap_vars[k]->is_consumed;
			if (then_c != else_c) {
				sem_emit_drop_conditional(ctx, snap_vars[k]->loc, snap_vars[k]->name);
				snap_vars[k]->is_consumed = 1;
			} else {
				snap_vars[k]->is_consumed = then_c;
			}
		}
		free(snap_vars);
		free(snap_before);
		free(snap_then);
		break;
	}

	case SN_RUN_STMT:
		break;

	case SN_EXPR_STMT:
		ctx->stmt_call_ok = 1;
		analyze_expression(ctx, sem_node_at_expr(v, 0));
		ctx->stmt_call_ok = 0;
		break;

	case SN_RETURN_STMT: {
		int rcount = sem_expr_count(v);
		if (ctx->in_sys)
			sem_emit_sys_no_return(ctx, loc);
		else if (rcount > 0 && !ctx->current_func)
			sem_emit_proc_return_has_value(ctx, loc);
		for (int i = 0; i < rcount; i++) {
			SyntaxView rv = sem_node_at_expr(v, i);
			analyze_expression(ctx, rv);
			if (sv_kind(rv) == SN_NAME_EXPR) {
				char *nm = sv_resolved_name(ctx, rv);
				VariableInfo *rvar = find_variable(ctx, nm);
				if (rvar && var_is_opaque(ctx, rvar))
					rvar->is_consumed = 1;
				if (ctx->current_func && rvar && type_is_byref_aggregate(ctx->ty_arena, rvar->type_id) &&
				    !rvar->is_param) {
					fprintf(stderr, "Error: cannot return a local array by value (array copy-out is not implemented); "
					                "return an `own` parameter or thread a caller-provided buffer instead\n");
					ctx->error_count++;
				} else if (ctx->current_func && rvar && rvar->borrows_local) {
					/* `r := loc[0:3]; return r` — `r` borrows a fresh local's storage (reclaimed on return),
					 * so handing it back dangles. The direct form `return loc[0:3]` is caught below. */
					fprintf(stderr, "Error: cannot return a slice of a local array — its storage does not outlive the "
					                "function; slice an `own`/caller-provided buffer or a global instead\n");
					ctx->error_count++;
				}
				free(nm);
			}
			/* A freshly-formed slice/row of a LOCAL stack array escaping by return is a dangling pointer
			 * — the local's storage is reclaimed on return. Only applies when the func actually returns a
			 * slice (so a scalar element return `return a[i]` from an `int` func is untouched). A slice of
			 * a PARAM (the caller's buffer) or a const/static global is fine (that storage outlives the
			 * call), so only a non-param local byref-aggregate base is rejected. Mirrors the whole-array
			 * check above, for `base[lo:hi]` and the matrix row `M[i]`. */
			else if (ctx->current_func && (sv_kind(rv) == SN_SLICE_EXPR || sv_kind(rv) == SN_INDEX_EXPR) &&
			         ctx->current_func->return_type_count > 0) {
				TypeId rid = ctx->current_func->return_type_ids[0];
				if (rid == TYID_UNKNOWN && ctx->current_func->return_type_nodes &&
				    sv_present(ctx->current_func->return_type_nodes[0]))
					rid = sem_intern_view(ctx, ctx->current_func->return_type_nodes[0]);
				if (type_is_byref_aggregate(ctx->ty_arena, rid)) {
					char *bn = sv_name_expr_dup(rv);
					VariableInfo *bvar = bn ? find_variable(ctx, bn) : NULL;
					if (bvar && type_is_byref_aggregate(ctx->ty_arena, bvar->type_id) && !bvar->is_param) {
						fprintf(stderr,
						        "Error: cannot return a slice of a local array — its storage does not outlive the "
						        "function; slice an `own`/caller-provided buffer or a global instead\n");
						ctx->error_count++;
					}
					free(bn);
				}
			}
		}
		break;
	}

	case SN_BREAK_STMT:
	case SN_CONTINUE_STMT:
		break;

	case SN_EACH_FIELD_STMT: {
		/* 1st IDENT = binding name, 2nd IDENT = archetype param name; first type node = filter. */
		char *binding_name = NULL, *arch_param = NULL;
		int ni = 0;
		for (int i = 0; i < v.node->child_count; i++)
			if (v.node->children[i].tag == SE_TOKEN && v.node->children[i].as.token.kind == TOK_IDENT) {
				SynText t = {v.src + v.node->children[i].as.token.offset, v.node->children[i].as.token.length};
				if (ni == 0)
					binding_name = sem_txt_dup(t);
				else if (ni == 1)
					arch_param = sem_txt_dup(t);
				ni++;
			}
		if (!binding_name)
			binding_name = sem_dupz("");
		if (!arch_param)
			arch_param = sem_dupz("");
		SyntaxView filter_tv = sem_type_at(v, 0);
		if (sv_present(filter_tv)) {
			TypeId fid = sem_intern_view(ctx, filter_tv);
			TyKind fk = tyid_kind(ctx->ty_arena, fid);
			if (fk != TYK_PRIM && fk != TYK_NOMINAL) {
				sem_emit_each_field_filter_type_not_name(ctx, loc);
			} else {
				const char *fn = sem_tyid_name(ctx, fid); /* `int` resolves to `i32` */
				if (!fn || (!is_width_int_name(fn) && strcmp(fn, "float") != 0 && strcmp(fn, "char") != 0))
					sem_emit_each_field_filter_type_not_primitive(ctx, loc);
			}
		}
		int arch_param_ok = 0;
		if (ctx->current_proc) {
			for (int i = 0; i < ctx->current_proc->param_count; i++) {
				ParamSummary *p = &ctx->current_proc->params[i];
				if (p->name && strcmp(p->name, arch_param) == 0 &&
				    tyid_kind(ctx->ty_arena, p->type_id) == TYK_ARCHETYPE_CATEGORY) {
					arch_param_ok = 1;
					break;
				}
			}
		}
		if (!arch_param_ok)
			sem_emit_each_field_invalid_rhs(ctx, loc, arch_param);
		push_scope(ctx);
		add_variable(ctx, binding_name, TYID_UNKNOWN);
		for (int i = 0, n = sem_stmt_count(v); i < n; i++)
			analyze_statement(ctx, sem_stmt_at(v, i));
		pop_scope(ctx);
		free(binding_name);
		free(arch_param);
		break;
	}

	case SN_MULTI_BIND_STMT:
	case SN_PROC_CALL_STMT: {
		/* Build a transient Statement only to read its multi-bind targets (the SEM_MB_FLUSH token
		 * walk is intricate); analyze the value expression and the bodies via views. The transient
		 * is freed at the end — targets are read into local state first, none escape. */
		MbTarget *mbt = NULL;
		int mbt_count = sem_read_mb_targets(ctx, v, &mbt);
		DeclSummary *mb_callee_proc = NULL;
		SyntaxView mb_value = {NULL, v.src};
		/* the value is the sole call/expr child */
		for (int i = 0; i < v.node->child_count; i++)
			if (v.node->children[i].tag == SE_NODE) {
				SyntaxNodeKind k = v.node->children[i].as.node->kind;
				if (k >= SN_LITERAL_EXPR && k <= SN_PAREN_EXPR) {
					mb_value = (SyntaxView){v.node->children[i].as.node, v.src};
					break;
				}
			}
		/* analyze the value (whole multi-return RHS) first, then bind targets */
		ctx->stmt_call_ok = (sv_present(mb_value) && sv_kind(mb_value) == SN_CALL_EXPR);
		ctx->proc_call_stmt_ok = ctx->stmt_call_ok; /* out-list statement: insert/delete are valid here */
		if (sv_present(mb_value))
			analyze_expression(ctx, mb_value);
		ctx->stmt_call_ok = 0;
		ctx->proc_call_stmt_ok = 0;

		/* resolve callee proc for the inout-redundant lint + out-param typing; also detect the
		 * mandatory-ok builtins insert/delete, whose out-slots are typed explicitly below. */
		const char *mb_builtin = NULL; /* "insert" | "delete" | NULL */
		if (sv_present(mb_value) && sv_kind(mb_value) == SN_CALL_EXPR) {
			const char *cn = ctx->model ? sem_model_callee_name(ctx->model, sv_id(mb_value)) : NULL;
			char *cnf = cn ? sem_dupz(cn) : sem_cv_dup(sv_child(mb_value, SN_CALLEE_NAME));
			mb_callee_proc = find_proc_sig(ctx, cnf);
			if (cnf && strcmp(cnf, "insert") == 0)
				mb_builtin = "insert";
			else if (cnf && strcmp(cnf, "delete") == 0)
				mb_builtin = "delete";
			free(cnf);
		}
		/* Validate the mandatory out-list arity: insert → (handle:, ok:), delete → (ok:). */
		if (mb_builtin && strcmp(mb_builtin, "insert") == 0 && mbt_count != 2)
			sem_emit_insert_delete_outlist(ctx, loc, "insert", "insert(P, …)(handle:, ok:)");
		if (mb_builtin && strcmp(mb_builtin, "delete") == 0 && mbt_count != 1)
			sem_emit_insert_delete_outlist(ctx, loc, "delete", "delete(h)(ok:)");
		/* W0016 discarded_ok: the capacity/handle `ok` (insert's 2nd out, delete's only out) was
		 * discarded with `_` — a silently-ignored failure. */
		if (mb_builtin) {
			int ok_idx = (strcmp(mb_builtin, "insert") == 0) ? 1 : 0;
			if (ok_idx < mbt_count && mbt[ok_idx].name && strcmp(mbt[ok_idx].name, "_") == 0)
				sem_emit_lint_discarded_ok(ctx, loc, mb_builtin);
		}

		/* W0011 inout_redundant: an in-arg NAME equal to an out-target NAME at an in-out position. */
		if (mb_callee_proc && sv_present(mb_value) && sv_kind(mb_value) == SN_CALL_EXPR) {
			for (int i = 0, ac = sem_expr_count(mb_value); i < ac; i++) {
				SyntaxView a = sem_node_at_expr(mb_value, i);
				if (sv_kind(a) != SN_NAME_EXPR || sv_text_eq(a, "_"))
					continue;
				if (!proc_param_is_inout(mb_callee_proc, i))
					continue;
				char *an = sv_resolved_name(ctx, a);
				for (int t = 0; t < mbt_count; t++)
					if (mbt[t].name && strcmp(mbt[t].name, an) == 0) {
						sem_emit_lint_inout_redundant_arg(ctx, sem_node_loc(a.node), an);
						break;
					}
				free(an);
			}
		}

		/* bind the targets (new shadows; existing must be live) */
		for (int i = 0; i < mbt_count; i++) {
			MbTarget *t = &mbt[i];
			TypeId bind_type = t->type_id;
			if (bind_type == TYID_UNKNOWN && mb_callee_proc && i < mb_callee_proc->out_param_count)
				bind_type = mb_callee_proc->out_params[i].type_id;
			/* mandatory-ok builtins: insert → (handle, int), delete → (int). */
			int is_handle_slot = 0;
			if (bind_type == TYID_UNKNOWN && mb_builtin) {
				if (strcmp(mb_builtin, "insert") == 0 && i == 0)
					is_handle_slot = 1;
				else
					bind_type = sem_tyid_of_name(ctx, "int");
			}
			if (t->is_new) {
				/* already added above for "_"-filtered new targets; re-add to capture type/nominal */
				if (t->name && strcmp(t->name, "_") != 0) {
					add_variable(ctx, t->name, bind_type);
					TyKind bk = tyid_kind(ctx->ty_arena, bind_type);
					if (is_handle_slot && ctx->scope_count > 0) {
						/* insert's handle out-slot: an opaque generation-checked handle (i64). It carries
						 * no TypeId, but `delete(h)` and handle equality rely on inferred_type "handle". */
						Scope *sc = &ctx->scopes[ctx->scope_count - 1];
						if (sc->var_count > 0)
							sc->vars[sc->var_count - 1]->inferred_type = "handle";
					} else if ((bk == TYK_NOMINAL || bk == TYK_PRIM) && ctx->scope_count > 0) {
						Scope *sc = &ctx->scopes[ctx->scope_count - 1];
						if (sc->var_count > 0) {
							VariableInfo *vv = sc->vars[sc->var_count - 1];
							vv->inferred_type = sem_tyid_name(ctx, bind_type);
							const char *nn = tyid_nominal_name(ctx->ty_arena, bind_type);
							if (nn && is_type_alias(ctx, nn) && !alias_is_transparent(ctx, nn))
								vv->nominal_type = nn;
						}
					}
				}
			} else {
				VariableInfo *existing = find_variable(ctx, t->name);
				if (!existing)
					sem_emit_assign_to_undeclared(ctx, loc, t->name);
				else if (existing->is_consumed)
					sem_emit_assign_after_move(ctx, loc, t->name);
			}
		}
		/* target types are pooled on ctx (sem_read_mb_targets), so a borrowing var->type stays valid;
		 * only the owned names + the array itself need freeing here. */
		for (int i = 0; i < mbt_count; i++)
			free(mbt[i].name);
		free(mbt);
		break;
	}

	case SN_BLOCK:
		/* a standalone `{ … }` nested scope — visit its statements so their exprs get typed */
		push_scope(ctx);
		for (int i = 0, n = sem_stmt_count(v); i < n; i++)
			analyze_statement(ctx, sem_stmt_at(v, i));
		pop_scope(ctx);
		break;

	case SN_MATCH_STMT: {
		/* analyze-only: visit the scrutinee and each arm's body (real dispatch is lowered elsewhere) */
		SyntaxView scrut = sem_node_at_expr(v, 0);
		if (sv_present(scrut))
			analyze_expression(ctx, scrut);
		int narm = sv_count(v, SN_MATCH_ARM);
		for (int i = 0; i < narm; i++) {
			SyntaxView arm = sv_child_at(v, SN_MATCH_ARM, i);
			push_scope(ctx);
			for (int j = 0, n = sem_stmt_count(arm); j < n; j++)
				analyze_statement(ctx, sem_stmt_at(arm, j));
			pop_scope(ctx);
		}
		break;
	}

	default:
		break;
	}
}

/* ========== DECLARATION ANALYSIS ========== */

static void analyze_archetype_decl(SemanticContext *ctx, DeclSummary *arch) {
	if (!arch)
		return;

	/* A `type`-typed component (`arche A { f :: type }`) is a generic component — not supported. */
	for (int i = 0; i < arch->field_count; i++)
		reject_meta_type(ctx, arch->fields[i].type_id, arch->fields[i].loc, "archetype component type");

	/* proc/func types can't be archetype components — archetypes are data; per-row behavior
	 * dispatch is the anti-pattern (Stage D dropped). Use `match` or a system instead. */
	for (int i = 0; i < arch->field_count; i++) {
		TypeId t = arch->fields[i].type_id;
		const char *tn = tyid_nominal_name(ctx->ty_arena, t);
		/* inline `on_hit :: proc()()`/`func`, or a named callable-type alias `on_hit :: handler`. */
		int callable = tyid_is_callable(ctx->ty_arena, t) || (tn && callable_type_alias_id(ctx, tn) != TYID_UNKNOWN);
		if (callable)
			sem_emit_callable_in_archetype(ctx, arch->fields[i].loc, arch->fields[i].name);
	}

	/* Set semantics: a component type may appear at most once in an archetype. The
	 * component's type name IS its access path, so a repeat would be unreachable. */
	for (int i = 0; i < arch->field_count; i++) {
		for (int j = i + 1; j < arch->field_count; j++) {
			if (arch->fields[i].name && arch->fields[j].name &&
			    strcmp(arch->fields[i].name, arch->fields[j].name) == 0) {
				sem_emit_duplicate_component(ctx, arch->fields[i].loc, arch->fields[i].name);
			}
		}
	}

	char *sig = compute_shape_signature(ctx, arch->fields, arch->field_count);
	ArchetypeInfo *shape = find_archetype_by_signature(ctx, sig);

	if (!shape) {
		/* New unique shape — create it */
		shape = malloc(sizeof(ArchetypeInfo));
		shape->signature = sig;
		shape->is_allocated = 0;
		shape->alloc_capacity = 0;
		shape->alloc_init_count = 0;
		shape->min_rows = 0;
		shape->req_count = 0;
		shape->req_first = NULL;
		shape->has_public_def = 0;

		/* Count total fields after expanding tuples */
		int expanded_field_count = 0;
		for (int i = 0; i < arch->field_count; i++) {
			if (tyid_kind(ctx->ty_arena, arch->fields[i].type_id) == TYK_TUPLE) {
				expanded_field_count += tyid_tuple_count(ctx->ty_arena, arch->fields[i].type_id);
			} else {
				expanded_field_count++;
			}
		}

		shape->fields = malloc(expanded_field_count * sizeof(FieldInfo *));
		shape->field_count = expanded_field_count;

		/* Populate fields, expanding tuples into virtual fields */
		int field_idx = 0;
		for (int i = 0; i < arch->field_count; i++) {
			FieldSummary *field = &arch->fields[i];
			TypeArena *A = ctx->ty_arena;
			if (tyid_kind(A, field->type_id) == TYK_TUPLE) {
				/* Expand tuple into component fields */
				for (int j = 0; j < tyid_tuple_count(A, field->type_id); j++) {
					FieldInfo *fi = malloc(sizeof(FieldInfo));
					char expanded_name[512];
					snprintf(expanded_name, sizeof(expanded_name), "%s_%s", field->name,
					         tyid_tuple_field_name(A, field->type_id, j));
					fi->name = malloc(strlen(expanded_name) + 1);
					strcpy(fi->name, expanded_name);
					fi->type_id = tyid_tuple_field_type(A, field->type_id, j);
					fi->kind = field->kind;
					shape->fields[field_idx++] = fi;
				}
			} else {
				FieldInfo *fi = malloc(sizeof(FieldInfo));
				fi->name = malloc(strlen(field->name) + 1);
				strcpy(fi->name, field->name);
				fi->type_id = field->type_id;
				fi->kind = field->kind;
				shape->fields[field_idx++] = fi;
			}
		}

		ctx->archetypes = realloc(ctx->archetypes, (ctx->archetype_count + 1) * sizeof(ArchetypeInfo *));
		ctx->archetypes[ctx->archetype_count++] = shape;
	} else {
		free(sig); /* duplicate signature; existing shape already owns one */
	}

	/* Tuple fields are flattened to scalar columns in lowering (syntax tree->AST), not
	   here: the AST is tuple-free, the syntax tree keeps tuples. The flat `shape` above
	   is still built from the tuple fields for type checking. */

	/* Validate handle types: must reference a known archetype. */
	for (int i = 0; i < arch->field_count; i++) {
		TypeId ft = arch->fields[i].type_id;
		if (tyid_kind(ctx->ty_arena, ft) != TYK_HANDLE)
			continue;
		const char *target = tyid_handle_name(ctx->ty_arena, ft);
		if (!find_archetype(ctx, target)) {
			fprintf(stderr, "Error: unknown archetype '%s' in handle type for field '%s'\n", target,
			        arch->fields[i].name);
			ctx->error_count++;
		}
	}

	/* A shape is global vocabulary: if ANY definition of it is exported (a driver decl, or a device's
	 * public-band shape), the shape is public and a datasheet storage requirement can name it. A shape
	 * seen only via a `#module` (unit-private) definition is NOT public. */
	if (arch->visibility == VIS_EXPORTED)
		shape->has_public_def = 1;

	/* Register alias */
	AliasEntry *entry = malloc(sizeof(AliasEntry));
	entry->name = malloc(strlen(arch->name) + 1);
	strcpy(entry->name, arch->name);
	entry->archetype = shape;
	ctx->aliases = realloc(ctx->aliases, (ctx->alias_count + 1) * sizeof(AliasEntry *));
	ctx->aliases[ctx->alias_count++] = entry;
}

/* Dimensionality of an aggregate-const initializer literal: a scalar is 0-D, a string literal is a
 * 1-D char row, and an `{ … }` array literal is one more than its first element's. So `{1,2}`→1,
 * `{{1,2},{3,4}}`→2, `{"a","bb"}`→2 (a char matrix), `{{{…}}}`→3. Used to reject >2-D consts with a
 * clear message instead of letting an under-counted flat global reach (and fail at) LLVM. */
static int const_array_lit_dims(SyntaxView v) {
	if (!sv_present(v))
		return 0;
	if (sv_kind(v) == SN_STRING_EXPR)
		return 1;
	if (sv_kind(v) != SN_ARRAY_LIT_EXPR)
		return 0;
	for (int i = 0; i < v.node->child_count; i++)
		if (v.node->children[i].tag == SE_NODE)
			return 1 + const_array_lit_dims((SyntaxView){v.node->children[i].as.node, v.src});
	return 1; /* empty `{}` — a 1-D (zero-length) array */
}

static void analyze_static_array_decl(SemanticContext *ctx, DeclSummary *s) {
	if (!s)
		return;

	/* Aggregate consts are 1-D arrays or 2-D matrices / string tables only — the flat storage model
	 * carries a single row stride, which can't describe a 3rd dimension. Reject higher rank cleanly
	 * (the size would otherwise be under-counted and miscompile at the LLVM layer). */
	if (s->static_has_init && sv_present(s->static_init) && sv_kind(s->static_init) == SN_ARRAY_LIT_EXPR) {
		int dims = const_array_lit_dims(s->static_init);
		if (dims > 2) {
			fprintf(stderr,
			        "Error: array '%s' has %d dimensions — array literals support at most 2 (a 1-D array, or a "
			        "2-D matrix / string table); higher-rank arrays are not yet supported\n",
			        s->name, dims);
			ctx->error_count++;
			return;
		}
	}

	/* A 2-D NUMERIC matrix must be rectangular: every row exactly `row_stride` wide. The flat global
	 * reserves rows*stride slots with NO padding for int/float, so a ragged row would emit fewer leaves
	 * than reserved and fail opaquely at the LLVM layer. (char/string rows ARE NUL-padded to the widest,
	 * so a short string row is legal — those are skipped here.) */
	if (s->static_has_init && s->static_row_stride > 1 && sv_present(s->static_init) &&
	    sv_kind(s->static_init) == SN_ARRAY_LIT_EXPR) {
		int is_str_matrix = 0;
		for (int i = 0; i < s->static_init.node->child_count; i++)
			if (s->static_init.node->children[i].tag == SE_NODE) {
				is_str_matrix = (s->static_init.node->children[i].as.node->kind == SN_STRING_EXPR);
				break;
			}
		if (!is_str_matrix) {
			int row = 0;
			for (int i = 0; i < s->static_init.node->child_count; i++) {
				if (s->static_init.node->children[i].tag != SE_NODE)
					continue;
				SyntaxNode *rn = s->static_init.node->children[i].as.node;
				if (rn->kind != SN_ARRAY_LIT_EXPR)
					continue;
				int w = 0;
				for (int j = 0; j < rn->child_count; j++)
					if (rn->children[j].tag == SE_NODE)
						w++;
				if (w != s->static_row_stride) {
					fprintf(stderr,
					        "Error: matrix '%s' is not rectangular — row %d has %d element%s but the matrix width "
					        "is %d\n",
					        s->name, row, w, w == 1 ? "" : "s", s->static_row_stride);
					ctx->error_count++;
					return;
				}
				row++;
			}
		}
	}

	/* Validate element type is a scalar */
	TyKind sk = tyid_kind(ctx->ty_arena, s->static_type_id);
	if (s->static_type_id == TYID_UNKNOWN) {
		fprintf(stderr, "Error: static array '%s' missing element type\n", s->name);
		ctx->error_count++;
		return;
	}

	if (sk != TYK_NOMINAL && sk != TYK_PRIM) {
		fprintf(stderr, "Error: static array '%s' element type must be scalar (int, float, char, etc.)\n", s->name);
		ctx->error_count++;
		return;
	}

	const char *type_name = sem_tyid_name(ctx, s->static_type_id);
	if (!type_name)
		type_name = "?";
	if (strcmp(type_name, "int") != 0 && strcmp(type_name, "i32") != 0 && strcmp(type_name, "float") != 0 &&
	    strcmp(type_name, "char") != 0 && strcmp(type_name, "double") != 0) {
		fprintf(stderr, "Error: static array '%s' has unsupported element type '%s'\n", s->name, type_name);
		ctx->error_count++;
		return;
	}

	/* Validate size is positive */
	if (s->static_size <= 0) {
		fprintf(stderr, "Error: static array '%s' has invalid size %d\n", s->name, s->static_size);
		ctx->error_count++;
		return;
	}

	/* `buf : T[N] = {…}` — a typed initializer is now emitted by codegen (a `[N x T]` global with the
	 * literal's values); an absent initializer stays zero-initialized. For a 1-D array with both a
	 * declared `[N]` and a `{…}` literal, the element count must match the declared size. */
	if (s->static_has_init && s->static_row_stride <= 1 && sv_present(s->static_init) &&
	    sv_kind(s->static_init) == SN_ARRAY_LIT_EXPR) {
		int n = 0;
		for (int i = 0; i < s->static_init.node->child_count; i++)
			if (s->static_init.node->children[i].tag == SE_NODE)
				n++;
		if (n != s->static_size) {
			fprintf(stderr,
			        "Error: array '%s' is declared with size %d but its initializer has %d element%s — "
			        "the element count must match the declared size\n",
			        s->name, s->static_size, n, n == 1 ? "" : "s");
			ctx->error_count++;
			return;
		}
	}

	/* Registration is hoisted: the name was added to the global scope in the pre-pass so forward
	 * references to it resolve regardless of declaration order. */
}

/* A top-level mutable global's initial value must be compile-time-known (static storage is
 * initialized at compile/link time — there is no startup code). An absent view = the implicit
 * `= 0`. View form of the old sem_is_const_init (reads the tree, not the AST). */
static int sem_is_const_init(SemanticContext *ctx, SyntaxView e) {
	if (!sv_present(e))
		return 1; /* implicit `= 0` */
	SyntaxNodeKind k = sv_kind(e);
	if (k == SN_LITERAL_EXPR || k == SN_STRING_EXPR)
		return 1;
	if (k == SN_NAME_EXPR) {
		char *nm = sv_name_expr_dup(e);
		int r = semantic_get_const_value(ctx, nm) != NULL;
		free(nm);
		return r;
	}
	/* `Enum.variant` folds to a compile-time int constant — the leftmost name is an enum type. */
	if (k == SN_FIELD_EXPR) {
		char *nm = sv_resolved_name(ctx, e);
		int r = nm && enum_is_type(ctx, nm);
		free(nm);
		return r;
	}
	/* CTFE: constant arithmetic or a pure `func` of constants folds at compile time (a `func` is a
	 * value), so it is a valid compile-time initializer. */
	if (k == SN_BINARY_EXPR || k == SN_UNARY_EXPR || k == SN_CALL_EXPR || k == SN_PAREN_EXPR) {
		int v;
		if (semantic_try_const_int(ctx, e, &v))
			return 1;
	}
	return 0;
}

static void analyze_static_scalar_decl(SemanticContext *ctx, DeclSummary *s) {
	if (!s)
		return;
	TyKind sk = tyid_kind(ctx->ty_arena, s->static_type_id);
	if (sk != TYK_NOMINAL && sk != TYK_PRIM) {
		fprintf(stderr, "Error: global '%s' has an invalid scalar type\n", s->name);
		ctx->error_count++;
		return;
	}
	if (!sem_is_const_init(ctx, s->static_init)) {
		fprintf(stderr,
		        "Error: global '%s' initializer must be a compile-time constant (a literal, a const, or a pure "
		        "func of them)\n",
		        s->name);
		ctx->error_count++;
		return;
	}
	/* Registration is hoisted (see the pre-pass); nothing to add here. */
}

/* ---------------------------------------------------------------------------
 * CTFE — compile-time evaluation of a constant integer expression.
 *
 * A `func` is a value (README): with compile-time-constant arguments its result is itself a
 * compile-time constant. This folds an integer expression — literals, `const`s, arithmetic, and
 * calls to pure `func`s (straight-line `:=` binds + a terminal `return`) — for use in a constant
 * position (pool capacity, array size, const decl, field default, global scalar init). Integer-only;
 * anything it cannot fold returns 0 so the caller keeps its existing "must be a constant" error.
 * ------------------------------------------------------------------------- */
#define CTFE_MAX_DEPTH 64

typedef struct {
	const char *name; /* borrowed: a param name (owned by the DeclSummary) or a bind name (freed by the caller) */
	long val;
} CtfeBinding;

typedef struct {
	CtfeBinding *vars; /* the param/local scope of the func body under evaluation; NULL at top level */
	int count;
	int cap;
	int depth;
} CtfeScope;

static int ctfe_eval(SemanticContext *ctx, SyntaxView e, CtfeScope *scope, long *out);

static int ctfe_lookup_local(const CtfeScope *scope, const char *name, long *out) {
	if (!scope || !name)
		return 0;
	for (int i = scope->count - 1; i >= 0; i--)
		if (scope->vars[i].name && strcmp(scope->vars[i].name, name) == 0) {
			*out = scope->vars[i].val;
			return 1;
		}
	return 0;
}

/* Evaluate a pure-func call with constant integer arguments: bind its params, walk its straight-line
 * body (`:=` binds, terminal `return`). */
static int ctfe_eval_call(SemanticContext *ctx, SyntaxView call, CtfeScope *caller, long *out) {
	int depth = (caller ? caller->depth : 0) + 1;
	if (depth > CTFE_MAX_DEPTH)
		return 0;
	char *callee = semantic_call_callee_name(ctx, call);
	if (!callee)
		return 0;
	const DeclSummary *fn = semantic_find_callable_sig(ctx, callee);
	free(callee);
	if (!fn || fn->kind != DECL_FUNC || fn->is_extern || fn->return_type_count != 1)
		return 0;
	if (!sv_present(fn->body_node))
		return 0;
	int argc = sem_expr_count(call);
	if (argc != fn->param_count)
		return 0;

	CtfeScope inner = {0};
	inner.depth = depth;
	inner.cap = argc + 16;
	inner.vars = calloc(inner.cap, sizeof(CtfeBinding));
	char *owned[64];
	int owned_count = 0;
	int ok = 0;

	/* Bind arguments (evaluated in the caller's scope) to the func's params. */
	for (int i = 0; i < argc; i++) {
		long av;
		if (!ctfe_eval(ctx, sem_node_at_expr(call, i), caller, &av))
			goto done;
		inner.vars[inner.count].name = fn->params[i].name;
		inner.vars[inner.count].val = av;
		inner.count++;
	}

	/* Walk the body: binds extend the scope; the first `return` yields the result. */
	int nstmt = sem_stmt_count(fn->body_node);
	for (int i = 0; i < nstmt; i++) {
		SyntaxView st = sem_stmt_at(fn->body_node, i);
		SyntaxNodeKind sk = sv_kind(st);
		if (sk == SN_BIND_STMT) {
			if (inner.count >= inner.cap || owned_count >= 64)
				goto done;
			char *nm = sv_name_expr_dup(sem_node_at_expr(st, 0));
			long bv;
			if (!nm || !ctfe_eval(ctx, sem_node_at_expr(st, 1), &inner, &bv)) {
				free(nm);
				goto done;
			}
			owned[owned_count++] = nm;
			inner.vars[inner.count].name = nm;
			inner.vars[inner.count].val = bv;
			inner.count++;
		} else if (sk == SN_RETURN_STMT) {
			long rv;
			if (ctfe_eval(ctx, sem_node_at_expr(st, 0), &inner, &rv)) {
				*out = rv;
				ok = 1;
			}
			goto done;
		} else {
			goto done; /* unsupported statement form — cannot fold */
		}
	}
done:
	for (int i = 0; i < owned_count; i++)
		free(owned[i]);
	free(inner.vars);
	return ok;
}

static int ctfe_eval(SemanticContext *ctx, SyntaxView e, CtfeScope *scope, long *out) {
	if (!sv_present(e))
		return 0;
	switch (sv_kind(e)) {
	case SN_PAREN_EXPR:
		return ctfe_eval(ctx, sem_first_expr(e), scope, out);
	case SN_LITERAL_EXPR: {
		char *lx = sem_cv_dup_first_token(e);
		if (!lx)
			return 0;
		int ok = 0, is_float = 0;
		for (char *p = lx; *p; p++)
			if (*p == '.') {
				is_float = 1;
				break;
			}
		if (!is_float) {
			char *end = NULL;
			long v = strtol(lx, &end, 0);
			if (end && end != lx && *end == '\0') {
				*out = v;
				ok = 1;
			}
		}
		free(lx);
		return ok;
	}
	case SN_NAME_EXPR: {
		char *nm = sv_name_expr_dup(e);
		if (!nm)
			return 0;
		long v;
		int ok = 0;
		if (ctfe_lookup_local(scope, nm, &v)) {
			*out = v;
			ok = 1;
		} else {
			const char *cv = semantic_get_const_value(ctx, nm);
			if (cv) {
				char *end = NULL;
				long c = strtol(cv, &end, 0);
				if (end && end != cv && *end == '\0') {
					*out = c;
					ok = 1;
				}
			}
		}
		free(nm);
		return ok;
	}
	case SN_UNARY_EXPR: {
		long v;
		if (!ctfe_eval(ctx, sem_node_at_expr(e, 0), scope, &v))
			return 0;
		if (sv_has_token(e, TOK_MINUS)) {
			*out = -v;
			return 1;
		}
		return 0;
	}
	case SN_BINARY_EXPR: {
		long l, r;
		if (!ctfe_eval(ctx, sem_node_at_expr(e, 0), scope, &l))
			return 0;
		if (!ctfe_eval(ctx, sem_node_at_expr(e, 1), scope, &r))
			return 0;
		switch (sem_binary_op(e)) {
		case OP_ADD:
			*out = l + r;
			return 1;
		case OP_SUB:
			*out = l - r;
			return 1;
		case OP_MUL:
			*out = l * r;
			return 1;
		case OP_DIV:
			if (r == 0)
				return 0;
			*out = l / r;
			return 1;
		case OP_MOD:
			if (r == 0)
				return 0;
			*out = l % r;
			return 1;
		case OP_EQ:
			*out = (l == r);
			return 1;
		case OP_NEQ:
			*out = (l != r);
			return 1;
		case OP_LT:
			*out = (l < r);
			return 1;
		case OP_GT:
			*out = (l > r);
			return 1;
		case OP_LTE:
			*out = (l <= r);
			return 1;
		case OP_GTE:
			*out = (l >= r);
			return 1;
		default:
			return 0; /* &&/|| or none — not foldable here */
		}
	}
	case SN_CALL_EXPR:
		return ctfe_eval_call(ctx, e, scope, out);
	default:
		return 0;
	}
}

/* Public: fold `e` to a compile-time integer constant. Returns 1 and writes *out on success, 0 if it
 * cannot be folded (the caller then keeps its own "must be a constant" diagnostic). */
int semantic_try_const_int(SemanticContext *ctx, SyntaxView e, int *out) {
	long v;
	if (ctx && ctfe_eval(ctx, e, NULL, &v)) {
		*out = (int)v;
		return 1;
	}
	return 0;
}

static void analyze_static_decl(SemanticContext *ctx, DeclSummary *alloc) {
	if (!alloc)
		return;

	/* Validate archetype exists */
	ArchetypeInfo *arch = find_archetype(ctx, alloc->name);
	if (!arch) {
		fprintf(stderr, "Error: unknown archetype '%s' in alloc\n", alloc->name);
		ctx->error_count++;
		return;
	}

	/* A pool decl from a device datasheet is a storage REQUIREMENT, not an allocation: it records a
	 * minimum the driver's own pool must meet (composed by `max` across datasheets), never allocates.
	 * The driver-pool-meets-minimum and missing-pool checks happen in the final sweep (sem_check_storage
	 * _requirements), so the order of requirement vs allocation decls does not matter. */
	if (alloc->is_requirement) {
		if (alloc->static_pool_count >= 0 && alloc->static_pool_count > arch->min_rows)
			arch->min_rows = alloc->static_pool_count;
		arch->req_count++;
		if (!arch->req_first)
			arch->req_first = sem_dupz(alloc->name);
		return;
	}

	/* Check if this shape has already been allocated. Each shape (field structure)
	   can have multiple archetype handles/names pointing to it, but only one can
	   allocate/initialize it. Once allocated, the shape is live in the world. */
	if (arch->is_allocated) {
		fprintf(stderr, "Error: Shape already allocated (archetype '%s' shares shape with an earlier allocation)\n",
		        alloc->name);
		ctx->error_count++;
		return;
	}
	arch->is_allocated = 1;

	/* Validate count is provided and is a literal */
	if (alloc->static_field_count == 0 || !sv_present(alloc->static_fields[0])) {
		fprintf(stderr, "Error: alloc missing count expression\n");
		ctx->error_count++;
		return;
	}

	if (alloc->static_pool_count < 0) {
		/* Not a bare literal — try to fold it (a `const`, or a pure `func` of constants). Runtime-dynamic
		 * counts are intentionally not a language feature; a dynamic-archetype library is the path there. */
		int folded;
		if (semantic_try_const_int(ctx, alloc->static_fields[0], &folded) && folded >= 0) {
			alloc->static_pool_count = folded;
		} else {
			fprintf(
			    stderr,
			    "Error: alloc count must be a compile-time constant (a literal, a const, or a pure func of them)\n");
			ctx->error_count++;
			return;
		}
	}
	/* Record the driver pool's capacity so the final sweep can check it against datasheet minimums. */
	arch->alloc_capacity = alloc->static_pool_count;
	/* Guaranteed-live initial count (M): the bounds prover elides a column index proven `< M`. Mirrors
	 * codegen's old static-count elision; only set when an explicit init_size literal was given. */
	arch->alloc_init_count = alloc->static_init_length_present ? alloc->static_init_count : 0;

	/* Validate: init block requires explicit init_size parameter */
	if (alloc->static_field_count > 1 && !alloc->static_init_length_present) {
		fprintf(stderr,
		        "Error: init block requires explicit init_size parameter: static %s(capacity, init_size) { ... }\n",
		        alloc->name);
		ctx->error_count++;
		return;
	}

	/* Analyze field initialization expressions */
	for (int i = 1; i < alloc->static_field_count; i++) {
		analyze_expression(ctx, alloc->static_fields[i]);
	}
}

/* Format a shape's component names as "{a, b, c}" into `out` (for diagnostics). */
static void sem_format_shape_fields(ArchetypeInfo *arch, char *out, size_t cap) {
	size_t n = 0;
	n += (size_t)snprintf(out + n, n < cap ? cap - n : 0, "{");
	for (int i = 0; i < arch->field_count && n < cap; i++)
		n += (size_t)snprintf(out + n, n < cap ? cap - n : 0, "%s%s", i ? ", " : "",
		                      arch->fields[i]->name ? arch->fields[i]->name : "?");
	if (n < cap)
		snprintf(out + n, cap - n, "}");
}

/* Final sweep (after all decls analyzed): a device datasheet posts a storage REQUIREMENT (min rows)
 * on a shape; the driver owns the actual pool. Enforce: a required shape with no driver pool is a
 * hard error; a driver pool smaller than the composed minimum is an error. Also emit a non-fatal note
 * when two+ datasheets require the same shape (shared pool). Sizing is keyed off the shape, so order
 * of requirement vs allocation decls does not matter. */
static void sem_check_storage_requirements(SemanticContext *ctx) {
	for (int a = 0; a < ctx->archetype_count; a++) {
		ArchetypeInfo *arch = ctx->archetypes[a];
		if (!arch || arch->min_rows <= 0)
			continue;
		char fields[512];
		sem_format_shape_fields(arch, fields, sizeof(fields));
		const char *nm = arch->req_first ? arch->req_first : "shape";
		if (!arch->has_public_def) {
			/* The datasheet states a storage requirement for a shape no PUBLIC definition names. A driver
			 * can only size a shape it can see, so the requirement is unsatisfiable — the shape must be
			 * defined publicly (a shape in `#module` is private to its device). */
			fprintf(stderr,
			        "Error: storage requirement %s[%d] names no public shape %s — define %s publicly "
			        "(a `#module` shape is private to its device)\n",
			        nm, arch->min_rows, fields, nm);
			ctx->error_count++;
			continue;
		}
		if (!arch->is_allocated) {
			fprintf(stderr,
			        "Error: no storage for %s required by a device datasheet — run `arche fill` or add %s[%d]\n",
			        fields, nm, arch->min_rows);
			ctx->error_count++;
			continue;
		}
		if (arch->alloc_capacity < arch->min_rows) {
			fprintf(stderr, "Error: a device requires >=%d rows of %s; the driver sized %d\n", arch->min_rows, fields,
			        arch->alloc_capacity);
			ctx->error_count++;
		}
		if (arch->req_count >= 2)
			fprintf(stderr, "note: %d device datasheets require %s -> one shared pool, size = %d\n", arch->req_count,
			        fields, arch->alloc_capacity);
	}
}

static void sem_check_device_impl_decls(SemanticContext *ctx) {
	for (int i = 0; i < ctx->decl_count; i++) {
		DeclSummary *d = ctx->decls[i];
		if (!d->from_device_impl)
			continue;
		const char *nm0 = d->name;
		const char *what = NULL;
		/* An archetype (shape) MAY be defined in a device's impl: a shape is a set of components, and a
		 * device that uses a shape must define it where it uses it (so it resolves locally) — every
		 * definition of the same shape coalesces by its canonical component types. Only TYPES (the
		 * shared vocabulary, which belong in the datasheet) and STORAGE (the driver's) are forbidden. */
		if (d->kind == DECL_ENUM)
			what = "define a type";
		else if (d->kind == DECL_CONST && nm0 && is_type_alias(ctx, nm0)) /* type alias / opaque (not a value const) */
			what = "define a type";
		else if (d->kind == DECL_STATIC && d->static_kind == STATIC_KIND_ARCHETYPE)
			what = "allocate a pool";
		if (!what)
			continue;
		fprintf(stderr,
		        "Error: a device's impl cannot %s ('%s') — types belong in its .ds.arche "
		        "datasheet, and allocation is the driver's\n",
		        what, nm0 ? nm0 : "?");
		ctx->error_count++;
	}
}

/* A datasheet (`.ds.arche`) DESCRIBES REQUIREMENTS for the driver — it defines nothing real. A shape
 * (archetype) is a definition, not a requirement: it must live in the device's impl or the driver,
 * where it coalesces to one canonical shape. The datasheet may state the components a shape needs and a
 * pool-size requirement (`Node[N]`, kept as a requirement elsewhere), but never the shape itself. */
static void sem_check_datasheet_decls(SemanticContext *ctx) {
	for (int i = 0; i < ctx->decl_count; i++) {
		DeclSummary *d = ctx->decls[i];
		if (!d->is_datasheet || d->kind != DECL_ARCHETYPE)
			continue;
		fprintf(stderr,
		        "Error: a datasheet describes requirements, not shapes ('%s') — define the shape in the "
		        "device or driver\n",
		        d->name ? d->name : "?");
		ctx->error_count++;
	}
}

/* ========== PROC LINT HELPERS ========== */

/* Builtins that mutate archetype state (registered in semantic_analyze). */
static int name_is_archetype_mutating_builtin(const char *name) {
	if (!name)
		return 0;
	return strcmp(name, "insert") == 0 || strcmp(name, "delete") == 0 || strcmp(name, "dealloc") == 0;
}

/* A call to a proc-typed parameter (a compile-time callback) is an action, so it
 * counts as an effect — same as calling a named proc. A func-typed callback call
 * is pure. The param type may be an inline `proc(...)` or a named alias to one. */
static int name_is_proc_typed_param(SemanticContext *ctx, DeclSummary *proc, const char *name) {
	if (!proc || !name)
		return 0;
	for (int i = 0; i < proc->param_count; i++) {
		ParamSummary *p = &proc->params[i];
		if (!p->name || strcmp(p->name, name) != 0)
			continue;
		TypeId t = p->type_id;
		if (tyid_is_callable(ctx->ty_arena, t))
			return tyid_is_proc(ctx->ty_arena, t) ? 1 : 0;
		const char *tn = tyid_nominal_name(ctx->ty_arena, t);
		if (tn && tyid_is_proc(ctx->ty_arena, callable_type_alias_id(ctx, tn)))
			return 1;
		return 0;
	}
	return 0;
}

/* Returns the leftmost identifier in an lvalue-ish expression chain, or NULL.
 * For `Transaction.price[i]` returns "Transaction"; for `x` returns "x". */
/* Run the proc-could-be-func and proc-no-effect lints on a non-extern proc. */
/* A proc "could be a func" iff its body would pass func-purity — the SAME predicate
 * `enforce_func_purity` uses — so the lint and the hard error agree on what "pure" means. */
static const char *func_purity_body_view(SemanticContext *ctx, SyntaxView declnode);

static void lint_proc_decl(SemanticContext *ctx, DeclSummary *proc) {
	if (!proc || proc->is_extern)
		return; /* #foreign procs are exempt — in-out IS the C-ABI idiom there. */

	/* W0012 inout_param_shadow: a non-foreign proc with an in-out param (an out-param shadowing the
	 * in-param of the same name). The in-out idiom is legitimate ONLY for `#foreign` procs (C-ABI
	 * alignment); a pure-arche proc should hand back a fresh out-only result instead. */
	for (int i = 0; i < proc->param_count; i++)
		if (proc_param_is_inout(proc, i))
			sem_emit_lint_inout_param_shadow(ctx, proc->loc, proc->params[i].name ? proc->params[i].name : "<param>");

	if (proc->allow_pure_proc)
		return;

	/* A proc taking a proc-typed callback param is inherently action-shaped: it
	 * exists to invoke that callback (an effect the purity walk can't see, since
	 * the callee is a param, not a named proc). Don't nudge it toward `func`. */
	for (int i = 0; i < proc->param_count; i++)
		if (name_is_proc_typed_param(ctx, proc, proc->params[i].name))
			return;

	/* A proc whose body has effects is legitimately a proc — nothing to lint. */
	if (func_purity_body_view(ctx, proc->body_node) != NULL)
		return;

	/* Pure body. `main` is the entry point: it can't be removed and can't be a func, and an empty/
	 * effect-free main is a normal mid-edit state — never lint it (neither could-be-func nor no-effect).
	 * A `func` returns EXACTLY ONE value, so only a SINGLE-out pure proc could be a func; a multi-out
	 * pure proc is legitimately a (multi-return) proc — no lint. A zero-out pure proc does nothing
	 * observable — flag for removal. The purity test is the SAME predicate enforce_func_purity uses, so
	 * "could be a func" means exactly "would compile as a func". */
	int is_main = proc->name && strcmp(proc->name, "main") == 0;
	if (is_main) {
		return;
	} else if (proc->out_param_count == 1) {
		sem_emit_lint_proc_could_be_func(ctx, proc->loc, proc->name ? proc->name : "<unknown>");
	} else if (proc->out_param_count == 0) {
		sem_emit_lint_proc_no_effect(ctx, proc->loc, proc->name ? proc->name : "<unknown>");
	}
}

/* ===== func-purity (`func-impure` lint) =====
 * A `func` must be functional. Unlike `name_is_effectful_callee` (the proc-could-be-func lint),
 * we do NOT treat a multi-return func as effectful — calling another (enforced-pure) func is fine.
 * A call is an effect iff the callee is a proc, an extern, or an archetype-mutating builtin. */
static const char *func_call_effect_reason(SemanticContext *ctx, const char *name) {
	static char buf[160];
	if (!ctx || !name)
		return NULL;
	if (name_is_archetype_mutating_builtin(name)) {
		snprintf(buf, sizeof(buf), "calls archetype-mutating builtin '%s'", name);
		return buf;
	}
	for (int i = 0; i < ctx->decl_count; i++) {
		DeclSummary *d = ctx->decls[i];
		if (d->kind == DECL_PROC && d->name && strcmp(d->name, name) == 0) {
			snprintf(buf, sizeof(buf), d->is_extern ? "calls extern '%s'" : "calls proc '%s'", name);
			return buf;
		}
		if (d->kind == DECL_FUNC && d->name && strcmp(d->name, name) == 0) {
			if (d->is_extern) {
				snprintf(buf, sizeof(buf), "calls extern '%s'", name);
				return buf;
			}
			return NULL; /* a non-extern func is (being) enforced pure — fine to call */
		}
	}
	return NULL;
}

/* AST-kill: func-purity walk over the syntax tree. Returns the first impurity reason found in the
 * subtree rooted at `v` (a statement or expression view), or NULL if pure. Mirrors the old AST
 * walker: a `run`, an effectful call, or a read/write of archetype/global state is an effect. A
 * func's only inputs are its params + `::` constants, so any static/global touch makes it impure. */
static const char *purity_walk(SemanticContext *ctx, SyntaxView v) {
	if (!sv_present(v))
		return NULL;
	const char *r;
	SyntaxNodeKind k = sv_kind(v);
	if (k == SN_RUN_STMT)
		return "runs a system (`run`)";
	if (k == SN_CALL_EXPR) {
		const char *cn = ctx->model ? sem_model_callee_name(ctx->model, sv_id(v)) : NULL;
		char *fb = (!cn && sv_count(v, SN_FIELD_NAME) == 0) ? sem_cv_dup(sv_child(v, SN_CALLEE_NAME)) : NULL;
		const char *name = cn ? cn : fb;
		if (name && (r = func_call_effect_reason(ctx, name))) {
			free(fb);
			return r;
		}
		free(fb);
	} else if (k == SN_ASSIGN_STMT) {
		char *tn = sv_resolved_name(ctx, sem_node_at_expr(v, 0));
		const char *rr = NULL;
		if (tn && find_archetype(ctx, tn))
			rr = "writes static memory (an archetype column)";
		else if (tn && is_static_name(ctx, tn))
			rr = "writes a mutable global";
		free(tn);
		if (rr)
			return rr;
	} else if (k == SN_NAME_EXPR || k == SN_FIELD_EXPR || k == SN_INDEX_EXPR || k == SN_SLICE_EXPR) {
		char *nm = sv_resolved_name(ctx, v);
		const char *rr = NULL;
		if (nm) {
			if (find_archetype(ctx, nm))
				rr = "reads static memory (an archetype column)";
			else if (is_static_name(ctx, nm) && !name_is_const_static_array(ctx, nm))
				rr = "reads a mutable global"; /* a `::` const array is immutable — reading it is pure */
		}
		free(nm);
		if (rr)
			return rr;
	}
	for (int i = 0; i < v.node->child_count; i++)
		if (v.node->children[i].tag == SE_NODE)
			if ((r = purity_walk(ctx, (SyntaxView){v.node->children[i].as.node, v.src})))
				return r;
	return NULL;
}

/* Pure iff every statement in the decl's body (its `node` view) is pure. */
static const char *func_purity_body_view(SemanticContext *ctx, SyntaxView declnode) {
	const char *r;
	for (int i = 0, n = sem_stmt_count(declnode); i < n; i++)
		if ((r = purity_walk(ctx, sem_stmt_at(declnode, i))))
			return r;
	return NULL;
}

/* Enforce func purity: a non-extern `func` body must be pure (no effects). This is a RULE, not a
 * lint — a violation is a hard compile error. Effects belong in a proc. */
static void enforce_func_purity(SemanticContext *ctx, DeclSummary *func) {
	if (!func || func->is_extern || func->is_policy)
		return; /* a policy is a macro, not a pure func — it may mutate operands and call `exit()` */
	const char *reason = func_purity_body_view(ctx, func->body_node);
	if (reason) {
		sem_emit_func_not_pure(ctx, func->loc, func->name ? func->name : "<unknown>", reason);
	}
}

/* W0021 func_could_be_const: the return expression carries no computation — a numeric/char
 * literal (`42`, `'x'`, `3.14`, `"linux"`) or a bare reference to a value const. Deliberately
 * tight: a `func` that computes its result (arithmetic, a call, an `Enum.variant`, a field/index)
 * is left alone, even when CTFE could fold it — those exist for readability, not by mistake.
 *
 * A string literal IS flagged now that arche has `char[]` value consts: a `func() -> []char`
 * returning a literal could be `name :: "literal"`. */
static int func_return_is_constish(SemanticContext *ctx, SyntaxView e) {
	if (!sv_present(e))
		return 0;
	SyntaxNodeKind k = sv_kind(e);
	if (k == SN_LITERAL_EXPR || k == SN_STRING_EXPR) /* int/float/char OR a string literal */
		return 1;
	if (k == SN_NAME_EXPR && sv_count(e, SN_FIELD_NAME) == 0) {
		char *nm = sv_name_expr_dup(e);
		int r = nm && semantic_get_const_value(ctx, nm) != NULL;
		free(nm);
		return r;
	}
	return 0;
}

/* A zero-parameter `func` whose entire body is a single `return <literal/const>;` is a
 * constant in a func costume — suggest a `::` value const (referenced without `()`). A func
 * WITH parameters genuinely maps inputs to a value, so it is never flagged; that's the
 * "no input" half of the rule. */
static void lint_func_could_be_const(SemanticContext *ctx, DeclSummary *func) {
	if (!func || func->is_extern || func->is_policy)
		return;
	if (func->param_count != 0 || func->return_type_count != 1)
		return; /* takes inputs, or returns a tuple — a real func, not a const */
	if (sem_stmt_count(func->body_node) != 1)
		return; /* more than a lone return — there's logic, leave it */
	SyntaxView st = sem_stmt_at(func->body_node, 0);
	if (sv_kind(st) != SN_RETURN_STMT)
		return;
	if (!func_return_is_constish(ctx, sem_node_at_expr(st, 0)))
		return;
	sem_emit_lint_func_could_be_const(ctx, func->loc, func->name ? func->name : "<unknown>");
}

/* ===== value-array / slice bounds analysis (Phase D: OOB totality) =====
 * A plain proc/func must prove every VALUE-array / slice index in-bounds; an unprovable one is an
 * abort site (func → error, proc → must be `proc!`). Scope: indexing of array/slice PARAMETERS
 * (`s[i]`, `buf[i]`). Pool COLUMNS (`Arch.field[i]`) and locals are the trusted data-oriented model
 * and are exempt — so this targets exactly the string/buffer code where OOB bugs bite. Provable:
 * a literal index into a sized `T[N]` param (0<=k<N); an index var proven `0 <= v < base.length/.cap`
 * (or `< N` for a sized base) by an enclosing `for v:=K>=0; v<bound; v+=+>0` loop or by guard-exits
 * (`if (v<0){ret/break/continue} … if (v>=bound){…}`). `&&` conjunctions and guards compose.
 * Intentionally conservative: anything else is reported unprovable. */

#define BND_MAX_FACTS 64
typedef struct {
	char *var;  /* index variable name (owned); NULL for a pure min-length fact */
	char *base; /* upper-bound base name (owned), or NULL when only nonneg is known */
	int nonneg; /* 1 once v>=0 is established */
	int minlen; /* if var==NULL && base!=NULL: `base.length >= minlen` (proves literal idx < minlen) */
	int litub;  /* if var!=NULL: `var < litub` (a literal upper bound; 0 = none) — proves idx into a
	             * sized T[N] when litub <= N */
} BndFact;

/* A local array/slice declaration seen in the body, so `name[i]` on a local is checked like a
 * param. kind: 1 = sized `T[N]` (size = N), 2 = slice `T[]`. Persists for the whole decl walk
 * (not snapshot-scoped); last declaration of a name wins (conservative). */
typedef struct {
	char *name;
	int kind;
	int size;
} BndLocal;

typedef struct {
	BndFact facts[BND_MAX_FACTS];
	int count;
	BndLocal locals[BND_MAX_FACTS];
	int local_count;
	int lint_columns;   /* 1 = also emit W0017 for unprovable pool-column (`Arch.field[i]`) indexing */
	int check_policies; /* 1 = the failure-policy validation pass (E0097-99/E0124/W0018); see sem_check_policies */
} BndEnv;

/* int value of a literal node, or -1 if not a nonneg int literal. */
static int bnd_lit_int(SyntaxView v) {
	if (!sv_present(v) || sv_kind(v) != SN_LITERAL_EXPR)
		return -1;
	char *t = sem_cv_dup(v);
	if (!t)
		return -1;
	int neg = (t[0] == '-');
	const char *p = neg ? t + 1 : t;
	for (const char *q = p; *q; q++)
		if (*q < '0' || *q > '9') {
			free(t);
			return -1;
		}
	int val = atoi(p);
	free(t);
	return neg ? -1 : val; /* a negative literal is never a valid index */
}

/* If `v` is a plain NAME expr (no field chain), its name (owned), else NULL. */
static char *bnd_plain_name(SemanticContext *ctx, SyntaxView v) {
	if (!sv_present(v) || sv_kind(v) != SN_NAME_EXPR || sv_count(v, SN_FIELD_NAME) != 0)
		return NULL;
	return sv_resolved_name(ctx, v);
}

/* If `v` is a `X.length` / `X.cap` extent expr, the base name X (owned), else NULL. */
static char *bnd_extent_base(SemanticContext *ctx, SyntaxView v) {
	if (!sv_present(v) || sv_kind(v) != SN_FIELD_EXPR)
		return NULL;
	int nf = sv_count(v, SN_FIELD_NAME);
	if (nf != 1)
		return NULL;
	char *fld = sem_cv_dup(sv_child_at(v, SN_FIELD_NAME, 0));
	int is_extent = fld && (strcmp(fld, "length") == 0 || strcmp(fld, "cap") == 0 || strcmp(fld, "capacity") == 0 ||
	                        strcmp(fld, "max_length") == 0 || strcmp(fld, "count") == 0);
	free(fld);
	if (!is_extent)
		return NULL;
	return sv_resolved_name(ctx, v); /* the root X */
}

/* param kind of `name`: 1 = sized `T[N]` (out_n=N), 2 = slice `T[]`, 0 = not a checked array base. */
static int bnd_param_kind(SemanticContext *ctx, DeclSummary *d, const char *name, int *out_n) {
	for (int pass = 0; pass < 2; pass++) {
		ParamSummary *ps = pass ? d->out_params : d->params;
		int n = pass ? d->out_param_count : d->param_count;
		for (int i = 0; i < n; i++) {
			if (!ps[i].name || strcmp(ps[i].name, name) != 0)
				continue;
			TyKind k = tyid_kind(ctx->ty_arena, ps[i].type_id);
			if (k == TYK_ARRAY) {
				if (out_n)
					*out_n = tyid_array_len(ctx->ty_arena, ps[i].type_id);
				return 1;
			}
			if (k == TYK_SLICE)
				return 2;
			return 0;
		}
	}
	return 0;
}

static void bnd_local_add(BndEnv *e, const char *name, int kind, int size) {
	if (!name || e->local_count >= BND_MAX_FACTS)
		return;
	e->locals[e->local_count].name = sem_dupz(name);
	e->locals[e->local_count].kind = kind;
	e->locals[e->local_count].size = size;
	e->local_count++;
}

/* Kind of an indexed plain-name base: a param/out-param array, a tracked local array, or — for an
 * unknown name that is neither a module static nor an archetype — a conservatively-checked
 * slice-like local. Module statics and archetype columns stay exempt (kind 0). */
static int bnd_base_kind(SemanticContext *ctx, DeclSummary *d, BndEnv *e, const char *name, int *out_n) {
	int k = bnd_param_kind(ctx, d, name, out_n);
	if (k != 0)
		return k;
	for (int i = e->local_count - 1; i >= 0; i--) /* last decl wins */
		if (e->locals[i].name && strcmp(e->locals[i].name, name) == 0) {
			if (out_n)
				*out_n = e->locals[i].size;
			return e->locals[i].kind;
		}
	/* Only PARAMs and explicitly-typed local arrays are gated. An unknown plain name (module static,
	 * archetype column, inferred-type local, tuple, …) stays exempt to avoid false positives. */
	return 0;
}

/* Facts are append-only within a scope; a var may carry several (one per upper base, plus a nonneg
 * marker). proven(v, base) needs BOTH a nonneg marker and an upper-bound fact naming `base`. */
static void bnd_env_add(BndEnv *e, const char *var, const char *base, int nonneg) {
	if (!var || e->count >= BND_MAX_FACTS)
		return;
	e->facts[e->count].var = sem_dupz(var);
	e->facts[e->count].base = base ? sem_dupz(base) : NULL;
	e->facts[e->count].nonneg = nonneg;
	e->facts[e->count].minlen = 0;
	e->facts[e->count].litub = 0;
	e->count++;
}

/* Record `base.length >= n` (so a literal index k < n into `base` is in-bounds). */
static void bnd_env_add_min(BndEnv *e, const char *base, int n) {
	if (!base || n <= 0 || e->count >= BND_MAX_FACTS)
		return;
	e->facts[e->count].var = NULL;
	e->facts[e->count].base = sem_dupz(base);
	e->facts[e->count].nonneg = 0;
	e->facts[e->count].minlen = n;
	e->facts[e->count].litub = 0;
	e->count++;
}

/* Record `var < k` (a literal upper bound; proves a var index into a sized T[N] when k <= N). */
static void bnd_env_add_litub(BndEnv *e, const char *var, int k) {
	if (!var || k <= 0 || e->count >= BND_MAX_FACTS)
		return;
	e->facts[e->count].var = sem_dupz(var);
	e->facts[e->count].base = NULL;
	e->facts[e->count].nonneg = 0;
	e->facts[e->count].minlen = 0;
	e->facts[e->count].litub = k;
	e->count++;
}

/* Smallest literal upper bound known for `var` (INT_MAX if none). */
static int bnd_litub(BndEnv *e, const char *var) {
	int best = 2147483647;
	for (int i = 0; i < e->count; i++)
		if (e->facts[i].litub > 0 && e->facts[i].var && strcmp(e->facts[i].var, var) == 0 && e->facts[i].litub < best)
			best = e->facts[i].litub;
	return best;
}

/* True if `base.length >= k+1` is known (proves literal index k). */
static int bnd_minlen_ok(BndEnv *e, const char *base, int k) {
	for (int i = 0; i < e->count; i++)
		if (!e->facts[i].var && e->facts[i].base && base && strcmp(e->facts[i].base, base) == 0 &&
		    e->facts[i].minlen > k)
			return 1;
	return 0;
}

static void bnd_env_truncate(BndEnv *e, int to) {
	if (to >= e->count)
		return; /* never GROW the count — a mid-block kill may have shrunk it below `to` */
	for (int i = to; i < e->count; i++) {
		free(e->facts[i].var);
		free(e->facts[i].base);
	}
	e->count = to;
}

/* Snapshot/restore the whole fact set so a nested block scopes BOTH its additions and its kills
 * (a conditional `i = 1` inside an `if` must not destroy the outer `i >= 0` fact). The snapshot
 * deep-copies; restore frees the current facts and transfers the snapshot's ownership back. */
static BndFact *bnd_snapshot(BndEnv *e, int *out_n) {
	*out_n = e->count;
	if (e->count == 0)
		return NULL;
	BndFact *s = malloc((size_t)e->count * sizeof(BndFact));
	for (int i = 0; i < e->count; i++) {
		s[i].var = e->facts[i].var ? sem_dupz(e->facts[i].var) : NULL;
		s[i].base = e->facts[i].base ? sem_dupz(e->facts[i].base) : NULL;
		s[i].nonneg = e->facts[i].nonneg;
		s[i].minlen = e->facts[i].minlen;
		s[i].litub = e->facts[i].litub;
	}
	return s;
}
static void bnd_restore(BndEnv *e, BndFact *snap, int n) {
	for (int i = 0; i < e->count; i++) {
		free(e->facts[i].var);
		free(e->facts[i].base);
	}
	for (int i = 0; i < n; i++)
		e->facts[i] = snap[i]; /* transfer ownership */
	e->count = n;
	free(snap);
}

/* Drop every fact about `var` (used when `var` is reassigned to an unknown value). */
static void bnd_env_kill(BndEnv *e, const char *var) {
	int w = 0;
	for (int i = 0; i < e->count; i++) {
		if (e->facts[i].var && strcmp(e->facts[i].var, var) == 0) {
			free(e->facts[i].var);
			free(e->facts[i].base);
		} else
			e->facts[w++] = e->facts[i];
	}
	e->count = w;
}

static int bnd_is_nonneg(BndEnv *e, const char *var) {
	for (int i = 0; i < e->count; i++)
		if (e->facts[i].nonneg && e->facts[i].var && strcmp(e->facts[i].var, var) == 0)
			return 1;
	return 0;
}

/* Is the value expression provably >= 0? Nonneg literal, a known-nonneg name, or a sum of two
 * nonneg sub-expressions (so `k := i + j` with i,j >= 0 is nonneg without a dead guard). */
static int bnd_expr_nonneg(SemanticContext *ctx, BndEnv *e, SyntaxView v) {
	if (!sv_present(v))
		return 0;
	if (sv_kind(v) == SN_LITERAL_EXPR)
		return bnd_lit_int(v) >= 0;
	if (sv_kind(v) == SN_NAME_EXPR && sv_count(v, SN_FIELD_NAME) == 0) {
		char *n = sv_resolved_name(ctx, v);
		int r = n ? bnd_is_nonneg(e, n) : 0;
		free(n);
		return r;
	}
	if (sv_kind(v) == SN_BINARY_EXPR && sem_binary_op(v) == OP_ADD)
		return bnd_expr_nonneg(ctx, e, sem_node_at_expr(v, 0)) && bnd_expr_nonneg(ctx, e, sem_node_at_expr(v, 1));
	return 0;
}

/* True if `var` is proven in [0, base): a nonneg marker AND an upper-bound fact naming `base`. */
static int bnd_proven(BndEnv *e, const char *var, const char *base) {
	if (!bnd_is_nonneg(e, var))
		return 0;
	for (int i = 0; i < e->count; i++)
		if (e->facts[i].var && e->facts[i].base && base && strcmp(e->facts[i].var, var) == 0 &&
		    strcmp(e->facts[i].base, base) == 0)
			return 1;
	return 0;
}

/* Extract bound facts from a (possibly `&&`-conjoined) condition into env:
 *   `v < X.length`        ⇒ upper(v, X)
 *   `X.length > k`        ⇒ minlen(X, k+1)   (and `k < X.length`)
 *   `X.length >= k`       ⇒ minlen(X, k)
 * so guards like `if (s.length > 0)` validate the literal index `s[0]`. */
static void bnd_collect_facts(SemanticContext *ctx, SyntaxView cond, BndEnv *e) {
	if (!sv_present(cond) || sv_kind(cond) != SN_BINARY_EXPR)
		return;
	Operator op = sem_binary_op(cond);
	if (op == OP_AND) {
		bnd_collect_facts(ctx, sem_node_at_expr(cond, 0), e);
		bnd_collect_facts(ctx, sem_node_at_expr(cond, 1), e);
		return;
	}
	SyntaxView l = sem_node_at_expr(cond, 0), r = sem_node_at_expr(cond, 1);
	if (op == OP_LT) {
		/* v < X.length  → upper(v, X) */
		char *v = bnd_plain_name(ctx, l);
		char *b = bnd_extent_base(ctx, r);
		if (v && b)
			bnd_env_add(e, v, b, 0);
		/* v < K (literal) → litub(v, K) — proves v into a sized T[N] with K <= N */
		int rlit = bnd_lit_int(r);
		if (v && rlit >= 0)
			bnd_env_add_litub(e, v, rlit);
		free(v);
		free(b);
		/* k < X.length → minlen(X, k+1) */
		int k = bnd_lit_int(l);
		char *xb = bnd_extent_base(ctx, r);
		if (k >= 0 && xb)
			bnd_env_add_min(e, xb, k + 1);
		free(xb);
	} else if (op == OP_GT) {
		/* X.length > k → minlen(X, k+1) */
		char *xb = bnd_extent_base(ctx, l);
		int k = bnd_lit_int(r);
		if (xb && k >= 0)
			bnd_env_add_min(e, xb, k + 1);
		free(xb);
		/* v > k (k >= 0 literal) → v >= 0 */
		char *v = bnd_plain_name(ctx, l);
		if (v && bnd_lit_int(r) >= 0)
			bnd_env_add(e, v, NULL, 1);
		free(v);
	} else if (op == OP_GTE) {
		/* X.length >= k → minlen(X, k) */
		char *xb = bnd_extent_base(ctx, l);
		int k = bnd_lit_int(r);
		if (xb && k > 0)
			bnd_env_add_min(e, xb, k);
		free(xb);
		/* v >= k (k >= 0 literal) → v >= 0 */
		char *v = bnd_plain_name(ctx, l);
		if (v && bnd_lit_int(r) >= 0)
			bnd_env_add(e, v, NULL, 1);
		free(v);
	}
}

/* Does the if-then body unconditionally exit the enclosing flow (return/break/continue)? */
static int bnd_body_exits(SyntaxView ifv, int else_part) {
	/* then-statements are the stmt children before any TOK_ELSE; else-statements after. We only
	 * need a coarse check: any return/break/continue among the relevant statements. */
	(void)else_part;
	for (int i = 0, n = sem_stmt_count(ifv); i < n; i++) {
		SyntaxNodeKind k = sv_kind(sem_stmt_at(ifv, i));
		if (k == SN_RETURN_STMT || k == SN_BREAK_STMT || k == SN_CONTINUE_STMT)
			return 1;
	}
	return 0;
}

/* Add the facts implied by the NEGATION of one comparison `cond` (used for guard-exits, where the
 * if-body exits so the rest of the block runs only when `cond` was false). Recognizes:
 *   v < 0          ⇒ v >= 0           (nonneg)
 *   v >= X.length  ⇒ v <  X.length    (upper)
 *   v > X.length / v >= X.length      (upper, conservative)
 *   X.length < K   ⇒ X.length >= K    (minlen)
 *   X.length <= K  ⇒ X.length >  K    (minlen K+1)
 * Returns 1 if it recognized the form. */
static int bnd_guard_neg(SemanticContext *ctx, SyntaxView cond, BndEnv *e) {
	if (!sv_present(cond) || sv_kind(cond) != SN_BINARY_EXPR)
		return 0;
	Operator op = sem_binary_op(cond);
	if (op == OP_OR) { /* !(A || B) = !A && !B */
		int a = bnd_guard_neg(ctx, sem_node_at_expr(cond, 0), e);
		int b = bnd_guard_neg(ctx, sem_node_at_expr(cond, 1), e);
		return a || b;
	}
	SyntaxView l = sem_node_at_expr(cond, 0), r = sem_node_at_expr(cond, 1);
	/* X.length < K  /  X.length <= K  ⇒ minlen */
	if (op == OP_LT || op == OP_LTE) {
		char *xb = bnd_extent_base(ctx, l);
		int k = bnd_lit_int(r);
		if (xb && k >= 0) {
			bnd_env_add_min(e, xb, op == OP_LTE ? k + 1 : k);
			free(xb);
			return 1;
		}
		free(xb);
	}
	char *v = bnd_plain_name(ctx, l);
	int handled = 0;
	if (v && op == OP_LT && bnd_lit_int(r) == 0) {
		bnd_env_add(e, v, NULL, 1); /* v < 0 exit ⇒ v >= 0 */
		handled = 1;
	} else if (v && (op == OP_GTE || op == OP_GT)) {
		char *b = bnd_extent_base(ctx, r);
		int klit = bnd_lit_int(r);
		if (b) {
			bnd_env_add(e, v, b, 0); /* v >= X.length exit ⇒ v < X.length */
			handled = 1;
		} else if (klit >= 0) {
			bnd_env_add_litub(e, v, op == OP_GT ? klit + 1 : klit); /* v >= K exit ⇒ v < K */
			handled = 1;
		}
		free(b);
	}
	free(v);
	return handled;
}

/* Decompose a comparison `<name> <op> <rhs>` (name a plain var, op a relational operator) into its
 * parts. Returns 1 on a match, filling *name (owned), *op, *rhs (a view into the same tree). */
static int bnd_cmp_parts(SemanticContext *ctx, SyntaxView cmp, char **name, Operator *op, SyntaxView *rhs) {
	if (!sv_present(cmp) || sv_kind(cmp) != SN_BINARY_EXPR)
		return 0;
	Operator o = sem_binary_op(cmp);
	if (o != OP_LT && o != OP_LTE && o != OP_GT && o != OP_GTE)
		return 0;
	char *n = bnd_plain_name(ctx, sem_node_at_expr(cmp, 0));
	if (!n)
		return 0;
	*name = n;
	*op = o;
	*rhs = sem_node_at_expr(cmp, 1);
	return 1;
}

/* True if `guard_op` is the exact logical complement of `loop_op` (so a guard testing `guard_op` can
 * never hold when the loop's `loop_op` holds): `<`/`>=`, `<=`/`>`, `>`/`<=`, `>=`/`<`. */
static int bnd_is_complement(Operator loop_op, Operator guard_op) {
	return (loop_op == OP_LT && guard_op == OP_GTE) || (loop_op == OP_LTE && guard_op == OP_GT) ||
	       (loop_op == OP_GT && guard_op == OP_LTE) || (loop_op == OP_GTE && guard_op == OP_LT);
}

/* True if two RHS bound expressions are the SAME bound: equal plain names, equal int literals, or the
 * same extent accessor (same base AND same `.length`/`.cap`/… field). Conservative — anything else is
 * treated as different (no false "redundant"). */
static int bnd_view_same(SemanticContext *ctx, SyntaxView a, SyntaxView b) {
	char *na = bnd_plain_name(ctx, a), *nb = bnd_plain_name(ctx, b);
	if (na && nb) {
		int eq = strcmp(na, nb) == 0;
		free(na);
		free(nb);
		return eq;
	}
	free(na);
	free(nb);
	int la = bnd_lit_int(a), lb = bnd_lit_int(b);
	if (la >= 0 && lb >= 0)
		return la == lb;
	if (sv_present(a) && sv_present(b) && sv_kind(a) == SN_FIELD_EXPR && sv_kind(b) == SN_FIELD_EXPR) {
		char *ba = bnd_extent_base(ctx, a), *bb = bnd_extent_base(ctx, b);
		int eq = 0;
		if (ba && bb && strcmp(ba, bb) == 0 && sv_count(a, SN_FIELD_NAME) == 1 && sv_count(b, SN_FIELD_NAME) == 1) {
			char *fa = sem_cv_dup(sv_child_at(a, SN_FIELD_NAME, 0));
			char *fb = sem_cv_dup(sv_child_at(b, SN_FIELD_NAME, 0));
			eq = fa && fb && strcmp(fa, fb) == 0;
			free(fa);
			free(fb);
		}
		free(ba);
		free(bb);
		return eq;
	}
	return 0;
}

/* W0020: flag a LEADING guard-exit in a loop body that re-tests the loop's own condition (the exact
 * complement of `cond` on the same operands). Sound: a leading guard-exit only exits or falls through
 * UNCHANGED, so at each such guard the loop variable and bound still hold their body-top values, where
 * the loop condition is guaranteed (the loop re-checks it every iteration). The run stops at the first
 * statement that isn't a guard-exit (it may reassign the operands). */
static void bnd_lint_loop_cond_guards(SemanticContext *ctx, SyntaxView forstmt, SyntaxView cond) {
	char *lname = NULL;
	Operator lop;
	SyntaxView lrhs;
	if (!bnd_cmp_parts(ctx, cond, &lname, &lop, &lrhs))
		return;
	int seen_brace = 0;
	for (int i = 0; i < forstmt.node->child_count; i++) {
		SyntaxElem *ch = &forstmt.node->children[i];
		if (ch->tag == SE_TOKEN) {
			if (ch->as.token.kind == TOK_LBRACE)
				seen_brace = 1;
			continue;
		}
		if (!seen_brace)
			continue;
		SyntaxView st = {ch->as.node, forstmt.src};
		if (sv_kind(st) != SN_IF_STMT || !bnd_body_exits(st, 0))
			break; /* end of the leading guard-exit run — a later statement may reassign the operands */
		char *gname = NULL;
		Operator gop;
		SyntaxView grhs;
		if (bnd_cmp_parts(ctx, sem_node_at_expr(st, 0), &gname, &gop, &grhs)) {
			if (gname && strcmp(lname, gname) == 0 && bnd_is_complement(lop, gop) && bnd_view_same(ctx, lrhs, grhs))
				sem_emit_lint_redundant_guard(ctx, sem_node_loc(st.node), gname);
			free(gname);
		}
	}
	free(lname);
}

/* A guard-exit `if (COND) { return/break/continue }` establishes COND's negation for the rest of
 * the enclosing block. Returns 1 if recognized. */
static int bnd_guard_exit(SemanticContext *ctx, SyntaxView ifv, BndEnv *e) {
	if (!bnd_body_exits(ifv, 0))
		return 0;
	return bnd_guard_neg(ctx, sem_node_at_expr(ifv, 0), e);
}

static const char *bnd_check_stmt(SemanticContext *ctx, DeclSummary *d, SyntaxView v, BndEnv *e);

/* If `v` is a compile-time integer constant (a literal, or unary-minus on one), set *out and return
 * 1; else 0. (bnd_lit_int collapses a negative literal to -1, losing the value — the failure-policy
 * pass needs the signed value to tell a provably-OOB negative index apart from a non-literal. A
 * negative index `a[-7]` parses as SN_UNARY_EXPR(`-`, 7), so the negate case must be folded here;
 * `a[0 - 7]` is a binary expr, deliberately left as a runtime value — no constant folding.) */
static int bnd_lit_int_signed(SyntaxView v, int *out) {
	if (!sv_present(v))
		return 0;
	if (sv_kind(v) == SN_UNARY_EXPR && sv_has_token(v, TOK_MINUS)) {
		int inner;
		if (bnd_lit_int_signed(sem_node_at_expr(v, 0), &inner)) {
			*out = -inner;
			return 1;
		}
		return 0;
	}
	if (sv_kind(v) != SN_LITERAL_EXPR)
		return 0;
	char *t = sem_cv_dup(v);
	if (!t)
		return 0;
	int neg = (t[0] == '-');
	const char *p = neg ? t + 1 : t;
	int ok = (*p != '\0');
	for (const char *q = p; *q; q++)
		if (*q < '0' || *q > '9')
			ok = 0;
	int val = atoi(p);
	free(t);
	if (!ok)
		return 0;
	*out = neg ? -val : val;
	return 1;
}

static const char *policy_cat_name(PolicyCategory c) {
	switch (c) {
	case POLICY_CAT_BOUNDS:
		return "bounds";
	case POLICY_CAT_POOL:
		return "pool";
	case POLICY_CAT_DIVIDE:
		return "divide";
	default:
		return "?";
	}
}

/* Crash-free enforcement flags — set by the CLI (cmd_build), consulted by the failure-policy pass.
 * `--no-abort` rejects any op resolving to `!abort` (implicit OR explicit); `--no-implicit-abort`
 * rejects only the default/implicit `!abort` (a deliberate, visible `!abort` is still allowed);
 * `!undefined` (the raw, runtime-unsafe opt-out) is rejected BY DEFAULT — `--allow-undefined` opts in. */
static int g_no_abort = 0;
static int g_no_implicit_abort = 0;
static int g_allow_undefined = 0; /* default: `!undefined` is forbidden in user code; flag opts in */
static int g_forbid_allow = 0;
void semantic_set_no_abort(int on) {
	g_no_abort = on;
}
void semantic_set_no_implicit_abort(int on) {
	g_no_implicit_abort = on;
}
void semantic_set_allow_undefined(int on) {
	g_allow_undefined = on;
}
void semantic_set_forbid_allow(int on) {
	g_forbid_allow = on;
}

/* The crash-free flags assert the USER's code is total — they don't fire on the bundled core/stdlib
 * (which aren't policy-annotated) nor on the prepended prelude text (origin ENTRY, but above the
 * user's first line). Mirrors the origin/line-offset gate the dead-code passes use. */
static int decl_is_user_code(DeclSummary *d) {
	if (d->origin == DECL_ORIGIN_STDLIB || d->origin == DECL_ORIGIN_CORE)
		return 0;
	int core_off = semantic_print_line_offset();
	if (d->origin == DECL_ORIGIN_ENTRY && core_off > 0 && d->loc.line <= core_off)
		return 0; /* prepended prelude */
	return 1;
}

/* Validate an explicit `!name` against the op's category `want` (and the func-total / `--no-abort` /
 * `--no-undefined` rules). `d` is the enclosing decl; `loc` the op site. Emits at most one diagnostic.
 * Category, not signature, disambiguates: `abort`/`undefined` exist for both bounds and divide. */
static void validate_explicit_policy(SemanticContext *ctx, DeclSummary *d, SourceLoc loc, const char *name,
                                     PolicyCategory want) {
	if (strcmp(name, "abort") == 0) {
		if (d->kind == DECL_FUNC) {
			sem_emit_policy_func_aborts(ctx, loc, d->name ? d->name : "<anon>");
			return;
		}
		if (g_no_abort && decl_is_user_code(d)) {
			sem_emit_policy_abort_forbidden(ctx, loc, "this `!abort`", "--no-abort");
			return;
		}
	} else if (strcmp(name, "undefined") == 0) {
		/* A policy is the safety mechanism — it may never opt out of safety. `!undefined` (a raw,
		 * unchecked op) inside ANY policy body is an error regardless of flags. Unprovable accesses are
		 * still allowed in a policy as long as they stay TOTAL (e.g. an eviction handler's clamped column
		 * scan) — only the raw opt-out is banned. Covers bounds AND divide `!undefined` (shared validator). */
		if (d->is_policy) {
			sem_emit_policy_uses_undefined(ctx, loc, d->name ? d->name : "<anon>");
			return;
		}
		/* `!undefined` is forbidden in user code BY DEFAULT (it's the raw, runtime-unsafe opt-out);
		 * `--allow-undefined` (or `--unchecked`) opts back in. Bundled core/stdlib are always exempt. */
		if (!g_allow_undefined && decl_is_user_code(d)) {
			sem_emit_policy_undefined_forbidden(ctx, loc);
			return;
		}
	}
	if (find_policy_sig_cat(ctx, name, want))
		return; /* a policy of this name AND category exists — valid */
	DeclSummary *any = find_policy_sig(ctx, name);
	if (!any)
		sem_emit_policy_unknown(ctx, loc, name);
	else
		sem_emit_policy_wrong_category(ctx, loc, name, policy_cat_name(want), policy_cat_name(any->policy_category));
}

/* Validate the failure policy on a single index/slice op. `kind`/`n` are as bnd_base_kind reports
 * for `base` (kind 0 = base isn't a tracked value-array ⇒ provability unknown). The prover's verdict
 * is authoritative — a policy attaches ONLY to ops it can't decide:
 *   provably OOB const  → E0097 (even with a policy: a statically-wrong access, not a runtime case);
 *   provably safe       → W0018 if an explicit policy is present (dead — the op can never fail);
 *   unprovable          → an explicit `!name` is validated: E0098 `!abort` in a func/policy (must be
 *                         total), E0099 unknown policy, E0124 wrong @policy(category).
 * Slices skip provability (the prover doesn't bound them) — only their explicit policy is validated. */
static void bnd_policy_check(SemanticContext *ctx, DeclSummary *d, BndEnv *e, SyntaxView v, int is_slice,
                             const char *base, int kind, int n) {
	char *explicit_pol = NULL;
	SyntaxView pol = sv_child(v, SN_POLICY_REF);
	if (sv_present(pol)) {
		SynText t = sv_token(pol, TOK_IDENT);
		if (t.ptr)
			explicit_pol = sem_txt_dup(t);
	}
	SourceLoc loc = sem_node_loc(v.node);

	/* An index/slice is a panic op (no failure channel) — it takes `!`, not the `?` handler sigil. */
	if (explicit_pol && sv_token(pol, TOK_QUESTION).ptr != NULL) {
		sem_emit_policy_wrong_sigil(ctx, loc, explicit_pol, 0);
		free(explicit_pol);
		return;
	}

	int provably_oob = 0, provably_safe = 0, oob_lit = 0;
	if (!is_slice && kind != 0) {
		SyntaxView idx = sem_node_at_expr(v, 0);
		int lit;
		if (bnd_lit_int_signed(idx, &lit)) {
			if (lit < 0) {
				provably_oob = 1;
				oob_lit = lit;
			} else if (kind == 1 && n >= 0) {
				if (lit >= n) {
					provably_oob = 1;
					oob_lit = lit;
				} else {
					provably_safe = 1;
				}
			} else if (kind == 3 && n > 0 && lit < n) {
				/* column (kind 3): `n` is the pool's guaranteed-live init count. A literal below it is
				 * in range; `lit >= n` is NOT OOB (the count can grow via insert) — keep the check. */
				provably_safe = 1;
			} else if (bnd_minlen_ok(e, base, lit)) {
				provably_safe = 1;
			}
		} else {
			char *iv = bnd_plain_name(ctx, idx);
			if (iv) {
				if (bnd_proven(e, iv, base))
					provably_safe = 1;
				else if (kind == 1 && n >= 0 && bnd_is_nonneg(e, iv) && bnd_litub(e, iv) > 0 && bnd_litub(e, iv) <= n)
					provably_safe = 1;
				/* column loop var bounded by a literal `K <= init count` (mirrors the old codegen
				 * `bounds_check_elidable`: `loop_bound <= static_count`). */
				else if (kind == 3 && n > 0 && bnd_is_nonneg(e, iv) && bnd_litub(e, iv) <= n)
					provably_safe = 1;
			}
			free(iv);
		}
	}

	if (provably_oob) {
		sem_emit_policy_provable_oob(ctx, loc, base ? base : "?", oob_lit, n);
	} else if (provably_safe) {
		/* Proven in-bounds ⇒ no failure policy applies. THE single verdict: read by the analyzer (no
		 * ghost inlay) AND by lowering→codegen (elide the policy macro). Neither re-derives. */
		if (ctx->model)
			sem_model_set_policy_elided(ctx->model, sv_id(v));
		if (explicit_pol)
			sem_emit_lint_policy_on_safe_op(ctx, loc, explicit_pol, base ? base : "?");
	} else if (explicit_pol) { /* unprovable: an explicit policy governs the op — validate it */
		validate_explicit_policy(ctx, d, loc, explicit_pol, POLICY_CAT_BOUNDS);
	} else if (d->kind != DECL_FUNC && (g_no_abort || g_no_implicit_abort) && decl_is_user_code(d)) {
		/* unprovable, unannotated, in a proc/sys → the implicit default is `!abort`. */
		sem_emit_policy_abort_forbidden(ctx, loc, "this op's implicit `!abort`",
		                                g_no_abort ? "--no-abort" : "--no-implicit-abort");
	}
	free(explicit_pol);
}

/* Recurse into every expression/child of a node, checking each value-array index. Statement-shaped
 * children that introduce their own scope (for/if/block) are handled by bnd_check_stmt; here we just
 * descend generic expression trees. */
static const char *bnd_check_expr(SemanticContext *ctx, DeclSummary *d, SyntaxView v, BndEnv *e) {
	if (!sv_present(v))
		return NULL;
	/* `A && B`: B is evaluated only when A holds, so check B under the facts A implies (this validates
	 * `s.length > 0 && s[0] == '-'`). */
	if (sv_kind(v) == SN_BINARY_EXPR && sem_binary_op(v) == OP_AND) {
		const char *r = bnd_check_expr(ctx, d, sem_node_at_expr(v, 0), e);
		if (r)
			return r;
		int saved = e->count;
		bnd_collect_facts(ctx, sem_node_at_expr(v, 0), e);
		r = bnd_check_expr(ctx, d, sem_node_at_expr(v, 1), e);
		bnd_env_truncate(e, saved);
		return r;
	}
	/* Divide-by-zero policy validation: `a / b !name` / `a % b !name` — the policy must be @policy(divide).
	 * (A divide is never statically OOB the way an index is, so there's only the category check.) */
	if (e->check_policies && sv_kind(v) == SN_BINARY_EXPR) {
		int bop = sem_binary_op(v);
		if (bop == OP_DIV || bop == OP_MOD) {
			SyntaxView pol = sv_child(v, SN_POLICY_REF);
			if (sv_present(pol)) {
				SynText t = sv_token(pol, TOK_IDENT);
				if (t.ptr) {
					char *nm = sem_txt_dup(t);
					if (sv_token(pol, TOK_QUESTION).ptr != NULL) /* divide is a panic op: `!`, not `?` */
						sem_emit_policy_wrong_sigil(ctx, sem_node_loc(v.node), nm, 0);
					else
						validate_explicit_policy(ctx, d, sem_node_loc(v.node), nm, POLICY_CAT_DIVIDE);
					free(nm);
				}
			}
		}
	}
	/* W0017 (lint mode only): a pool-column index `Arch.field[i]` whose `i` isn't proven in-bounds
	 * (constant, or `i < Arch.length/.count`). Advisory — prefer handles. Not gated (columns are the
	 * trusted bulk model), so this only warns, never forces `proc!`. */
	if (e->lint_columns && sv_kind(v) == SN_INDEX_EXPR && sv_count(v, SN_FIELD_NAME) > 0 &&
	    !sv_present(sv_child(v, SN_POLICY_REF))) {
		/* An explicit `!policy` (e.g. `Arch.f[i] !undefined`) IS the acknowledgment W0017 asks for —
		 * the failure behavior is declared at the site, so don't also warn about the raw slot. */
		char *root = sv_resolved_name(ctx, v);
		if (root && find_archetype(ctx, root)) {
			SyntaxView idx = sem_node_at_expr(v, 0);
			int ok = (bnd_lit_int(idx) >= 0); /* constant slot: assume within capacity */
			if (!ok) {
				char *iv = bnd_plain_name(ctx, idx);
				if (iv)
					ok = bnd_proven(e, iv, root); /* i < Arch.length/.count, i >= 0 */
				free(iv);
			}
			if (!ok)
				sem_emit_lint_raw_pool_index(ctx, sem_node_loc(v.node), root);
		}
		free(root);
	}
	/* Failure-policy validation on a pool-column index (`Arch.f[i] !policy`). kind 3 = column: there's
	 * no static count, so the only proof path is symbolic — `i` proven `0 <= i < Arch.count/length` by a
	 * guard/loop (`bnd_proven` against the LIVE count, sounder than codegen's static-count elision). A
	 * proven column index needs no policy (sets `policy_elided`); an unproven one validates its explicit
	 * `!name` / keeps the implicit check. A bare literal `Arch.f[3]` stays unproven (no length guard). */
	if (e->check_policies && sv_kind(v) == SN_INDEX_EXPR && sv_count(v, SN_FIELD_NAME) > 0) {
		char *root = sv_resolved_name(ctx, v);
		ArchetypeInfo *ai = root ? find_archetype(ctx, root) : NULL;
		if (ai)
			bnd_policy_check(ctx, d, e, v, 0, root, 3, ai->alloc_init_count); /* n = guaranteed-live count M */
		free(root);
	}
	if (sv_kind(v) == SN_INDEX_EXPR && sv_count(v, SN_FIELD_NAME) == 0) {
		char *base = sv_resolved_name(ctx, v);
		int n = -1;
		int kind = base ? bnd_base_kind(ctx, d, e, base, &n) : 0;
		if (e->check_policies) { /* failure-policy validation pass (emits; never short-circuits) */
			bnd_policy_check(ctx, d, e, v, 0, base, kind, n);
		} else if (kind != 0) { /* a checked array/slice parameter */
			SyntaxView idx = sem_node_at_expr(v, 0);
			int lit = bnd_lit_int(idx);
			int ok = 0;
			if (lit >= 0) {
				if (kind == 1 && n >= 0)
					ok = (lit < n); /* literal into sized T[N] */
				else
					ok = bnd_minlen_ok(e, base, lit); /* slice (or unknown N): need a length guard */
			} else {
				char *iv = bnd_plain_name(ctx, idx);
				if (iv) {
					ok = bnd_proven(e, iv, base);
					/* sized T[N]: a literal upper bound `iv < K` with K <= N also proves it */
					if (!ok && kind == 1 && n >= 0 && bnd_is_nonneg(e, iv) && bnd_litub(e, iv) <= n)
						ok = 1;
				}
				free(iv);
			}
			if (!ok) {
				static char buf[160];
				snprintf(buf, sizeof buf, "indexes '%s' with an unproven bound", base);
				free(base);
				return buf;
			}
		}
		free(base);
	}
	if (e->check_policies && sv_kind(v) == SN_SLICE_EXPR) {
		char *base = sv_resolved_name(ctx, v);
		int n = -1;
		int kind = base ? bnd_base_kind(ctx, d, e, base, &n) : 0;
		bnd_policy_check(ctx, d, e, v, 1, base, kind, n);
		free(base);
	}
	for (int i = 0; i < v.node->child_count; i++)
		if (v.node->children[i].tag == SE_NODE) {
			const char *r = bnd_check_expr(ctx, d, (SyntaxView){v.node->children[i].as.node, v.src}, e);
			if (r)
				return r;
		}
	return NULL;
}

/* Walk statements in order with a fact env; returns the first unprovable index reason or NULL.
 * Facts mutated inside the block (adds AND kills) are scoped to it via snapshot/restore. */
static const char *bnd_check_block(SemanticContext *ctx, DeclSummary *d, SyntaxView block, BndEnv *e) {
	int snn;
	BndFact *snap = bnd_snapshot(e, &snn);
	const char *r = NULL;
	for (int i = 0, n = sem_stmt_count(block); i < n && !r; i++)
		r = bnd_check_stmt(ctx, d, sem_stmt_at(block, i), e);
	bnd_restore(e, snap, snn);
	return r;
}

static const char *bnd_check_stmt(SemanticContext *ctx, DeclSummary *d, SyntaxView v, BndEnv *e) {
	if (!sv_present(v))
		return NULL;
	SyntaxNodeKind k = sv_kind(v);
	if (k == SN_FOR_STMT) {
		/* extract the loop var's init (>=0?) and the cond's `v < bound` facts for the body */
		int snn;
		BndFact *snap = bnd_snapshot(e, &snn);
		/* find init/cond among header segments: init is a bind/assign, cond is a bare expr before `{` */
		SyntaxView cond = {NULL, v.src};
		int init_nonneg_var_seen = 0;
		char *init_var = NULL;
		int seen_brace = 0, seg = 0;
		for (int i = 0; i < v.node->child_count; i++) {
			SyntaxElem *ch = &v.node->children[i];
			if (ch->tag == SE_TOKEN) {
				if (ch->as.token.kind == TOK_LBRACE)
					seen_brace = 1;
				else if (ch->as.token.kind == TOK_SEMI && !seen_brace && seg < 2)
					seg++;
				continue;
			}
			if (seen_brace)
				continue;
			SyntaxView cv = {ch->as.node, v.src};
			SyntaxNodeKind ck = sv_kind(cv);
			if (seg == 0 && (ck == SN_BIND_STMT || ck == SN_ASSIGN_STMT)) {
				/* init `var := K` with K provably nonneg ⇒ var is nonneg at loop entry */
				char *iv = sv_resolved_name(ctx, sem_node_at_expr(cv, 0));
				int nn = bnd_expr_nonneg(ctx, e, sem_node_at_expr(cv, 1));
				if (iv && nn) {
					init_var = iv;
					init_nonneg_var_seen = 1;
				} else {
					free(iv);
				}
			} else if (seg == 1 && ck >= SN_LITERAL_EXPR && ck <= SN_PAREN_EXPR) {
				cond = cv;
			}
		}
		/* push cond upper-bound facts; mark the init var nonneg */
		bnd_collect_facts(ctx, cond, e);
		if (init_var)
			bnd_env_add(e, init_var, NULL, 1);
		(void)init_nonneg_var_seen;
		/* W0020: leading guard-exits that re-test this loop's own condition can never fire. Lint pass
		 * only (where @allow is armed); emit-once since each for-stmt is visited once per pass. */
		if (e->lint_columns)
			bnd_lint_loop_cond_guards(ctx, v, cond);
		/* Check ONLY the post-`{` body statements (NOT the header init/incr — the incr `i=i+1` would
		 * otherwise kill the loop var's facts before the body is checked). */
		const char *r = NULL;
		seen_brace = 0;
		for (int i = 0; i < v.node->child_count && !r; i++) {
			SyntaxElem *ch = &v.node->children[i];
			if (ch->tag == SE_TOKEN) {
				if (ch->as.token.kind == TOK_LBRACE)
					seen_brace = 1;
				continue;
			}
			if (!seen_brace)
				continue;
			r = bnd_check_stmt(ctx, d, (SyntaxView){ch->as.node, v.src}, e);
		}
		bnd_restore(e, snap, snn);
		free(init_var);
		return r;
	}
	if (k == SN_IF_STMT) {
		/* guard-exit: establish negated facts for statements AFTER this if (same block); these must
		 * PERSIST in the enclosing block (the enclosing bnd_check_block scopes them). */
		if (bnd_guard_exit(ctx, v, e))
			return bnd_check_expr(ctx, d, sem_node_at_expr(v, 0), e);
		/* plain if: within the then-body, the cond's `v < X.length` / minlen facts hold */
		SyntaxView cond = sem_node_at_expr(v, 0);
		const char *r = bnd_check_expr(ctx, d, cond, e);
		if (r)
			return r;
		int snn;
		BndFact *snap = bnd_snapshot(e, &snn);
		bnd_collect_facts(ctx, cond, e);
		r = bnd_check_block(ctx, d, v, e);
		bnd_restore(e, snap, snn);
		return r;
	}
	if (k == SN_BLOCK)
		return bnd_check_block(ctx, d, v, e);
	if (k == SN_MATCH_STMT) {
		int narm = sv_count(v, SN_MATCH_ARM);
		for (int i = 0; i < narm; i++) {
			const char *r = bnd_check_block(ctx, d, sv_child_at(v, SN_MATCH_ARM, i), e);
			if (r)
				return r;
		}
		return bnd_check_expr(ctx, d, sem_node_at_expr(v, 0), e);
	}
	if (k == SN_BIND_STMT || k == SN_ASSIGN_STMT) {
		/* track counter nonneg: `v := K>=0` / `v = K>=0` marks v nonneg; any other reassignment of a
		 * plain name invalidates what we knew about it. The value expr is still scanned for indices. */
		const char *r = bnd_check_expr(ctx, d, sem_node_at_expr(v, 1), e);
		if (r)
			return r;
		char *tgt = bnd_plain_name(ctx, sem_node_at_expr(v, 0));
		if (tgt) {
			int nn = bnd_expr_nonneg(ctx, e, sem_node_at_expr(v, 1));
			if (k == SN_ASSIGN_STMT)
				bnd_env_kill(e, tgt);
			if (nn)
				bnd_env_add(e, tgt, NULL, 1);
		}
		/* Track a typed local array/slice decl so `tgt[i]` is checked like a param. */
		if (k == SN_BIND_STMT && tgt) {
			SyntaxView t0 = sem_type_at(v, 0);
			if (sv_present(t0)) {
				TypeId tid = sem_intern_view(ctx, t0);
				TyKind tk = tyid_kind(ctx->ty_arena, tid);
				if (tk == TYK_ARRAY)
					bnd_local_add(e, tgt, 1, tyid_array_len(ctx->ty_arena, tid));
				else if (tk == TYK_SLICE)
					bnd_local_add(e, tgt, 2, 0);
			}
		}
		free(tgt);
		return bnd_check_expr(ctx, d, sem_node_at_expr(v, 0), e);
	}
	/* ordinary statement: scan its expressions for indices */
	return bnd_check_expr(ctx, d, v, e);
}

/* One-time pass: emit W0017 (raw_pool_index) for unprovable pool-column indexing in each proc/func/
 * sys body. Each site warns once. */
static void sem_check_raw_pool_lint(SemanticContext *ctx) {
	for (int di = 0; di < ctx->decl_count; di++) {
		DeclSummary *d = ctx->decls[di];
		if ((d->kind != DECL_PROC && d->kind != DECL_FUNC && d->kind != DECL_SYS) || d->is_extern)
			continue;
		if (!sv_present(d->body_node))
			continue;
		BndEnv e;
		e.count = 0;
		e.local_count = 0;
		e.lint_columns = 1;
		e.check_policies = 0;
		/* This pass runs after analyze cleared active_allow_slugs, so re-arm the decl's @allow set
		 * around the walk — otherwise a `@allow(raw_pool_index)` on the decl is silently ignored (the
		 * lint's documented escape hatch). Mirrors the dead-code pass above. */
		ctx->active_allow_slugs = d->allow_slugs;
		ctx->active_allow_slug_count = d->allow_slug_count;
		for (int i = 0, n = sem_stmt_count(d->body_node); i < n; i++)
			bnd_check_stmt(ctx, d, sem_stmt_at(d->body_node, i), &e); /* return ignored; emits lints */
		ctx->active_allow_slugs = NULL;
		ctx->active_allow_slug_count = 0;
		bnd_env_truncate(&e, 0);
		for (int i = 0; i < e.local_count; i++)
			free(e.locals[i].name);
	}
}

/* `--forbid-allow`: a strict build that tolerates no lint escape hatches in the user's own code —
 * every `@allow(<slug>)` is a hard error (E0127). Scoped to user code (not the bundled core/stdlib),
 * like the crash-free flags. Fix the underlying lint, don't suppress it. */
static void sem_check_forbid_allow(SemanticContext *ctx) {
	if (!g_forbid_allow)
		return;
	for (int di = 0; di < ctx->decl_count; di++) {
		DeclSummary *d = ctx->decls[di];
		if (!decl_is_user_code(d))
			continue;
		for (int i = 0; i < d->allow_slug_count; i++)
			sem_emit_allow_forbidden(ctx, d->loc, d->allow_slugs[i]);
	}
}

/* The interned `TypeId` a top-level declaration denotes — the type its name binds to, sourced from
 * whichever DeclSummary field(s) hold it — or TYID_UNKNOWN when the declaration is not a value/type
 * binding. This is a GENERAL compiler fact (it completes the node→type map for decl nodes, the same
 * fact hover/go-to-type want), not inlay-specific. The per-kind sourcing lives ONLY here; everything
 * downstream is kind-agnostic. The switch is EXHAUSTIVE with NO `default`: a new DeclKind is a
 * compile-time forcing function (-Wswitch), never a silent drop. */
static TypeId sem_decl_type_id(SemanticContext *ctx, DeclSummary *d) {
	switch (d->kind) {
	case DECL_CONST: {
		/* A VALUE const denotes its value's type; a TYPE alias (`int :: alias i32`) denotes the `type`
		 * meta-type — `int : type : alias i32` is the longhand, so its ⟨type⟩ slot is `type`. */
		const char *vt = value_const_type(ctx, d->name);
		if (vt)
			return sem_tyid_of_name(ctx, vt);
		if (is_type_alias(ctx, d->name))
			return sem_tyid_of_name(ctx, "type");
		return TYID_UNKNOWN;
	}
	case DECL_STATIC:
		/* SCALAR → its scalar type; ARRAY → the array type built over its ELEMENT type (static_type_id
		 * holds the element, not the array); ARCHETYPE-allocation (`Particle[4]`) is not a name::value
		 * binding → UNKNOWN. */
		switch (d->static_kind) {
		case STATIC_KIND_SCALAR:
			return d->static_type_id;
		case STATIC_KIND_ARRAY:
			return array_const_type_id(ctx, d);
		default:
			return TYID_UNKNOWN; /* STATIC_KIND_ARCHETYPE */
		}
	case DECL_FUNC: /* also policies — a policy is a DECL_FUNC with is_policy */
	case DECL_PROC:
	case DECL_SYS: {
		/* Each form gets its OWN callable kind — func/proc/sys/policy never unify. Params are common;
		 * a func carries its return list, a proc its out-params, sys/policy none. */
		/* foreign decls have a fully computed signature (sem_fill_decl_type_ids types their params/returns
		 * unconditionally), so they get their type like any other callable — no special-case hide. */
		int np = d->param_count;
		TypeId pbuf[32];
		TypeId *params = np > 32 ? malloc((size_t)np * sizeof(TypeId)) : pbuf;
		for (int i = 0; i < np; i++)
			/* func/proc/policy params are typed (`a: int`); a sys's are bare COMPONENT names whose type
			 * is the component itself — resolve those by name so `sys(pos, vel)` isn't `sys(<unknown>)`. */
			params[i] = (d->kind == DECL_SYS && d->params[i].name) ? sem_tyid_of_name(ctx, d->params[i].name)
			                                                       : d->params[i].type_id;
		TypeId out;
		if (d->kind == DECL_SYS) {
			out = tyid_of_sys(ctx->ty_arena, params, np);
		} else if (d->is_policy) {
			out = tyid_of_policy(ctx->ty_arena, params, np);
		} else if (d->kind == DECL_PROC) {
			int nr = d->out_param_count;
			TypeId rbuf[8];
			TypeId *rets = nr > 8 ? malloc((size_t)nr * sizeof(TypeId)) : rbuf;
			for (int i = 0; i < nr; i++)
				rets[i] = d->out_params[i].type_id;
			out = tyid_of_proc(ctx->ty_arena, params, np, rets, nr);
			if (rets != rbuf)
				free(rets);
		} else {
			int nr = d->return_type_count;
			TypeId rbuf[8];
			TypeId *rets = nr > 8 ? malloc((size_t)nr * sizeof(TypeId)) : rbuf;
			for (int i = 0; i < nr; i++)
				rets[i] = d->return_type_ids[i];
			out = tyid_of_func(ctx->ty_arena, params, np, rets, nr);
			if (rets != rbuf)
				free(rets);
		}
		if (params != pbuf)
			free(params);
		return out;
	}
	case DECL_ARCHETYPE:
		return tyid_of_archetype_category(ctx->ty_arena);
	case DECL_ENUM:
		/* An enum decl (`Method :: enum{…}`) DENOTES a type, so its `⟨type⟩` slot is the `type` meta —
		 * the longhand `Method : type : enum{…}`, mirroring the type-alias arm in DECL_CONST. */
		return sem_tyid_of_name(ctx, "type");
	case DECL_FUNC_GROUP:
		return TYID_UNKNOWN; /* an overload SET has no single type — a principled "none", not a dropped kind */
	case DECL_WORLD:
	case DECL_USE:
		return TYID_UNKNOWN; /* not value/type bindings — no `⟨type⟩` slot to fill */
	}
	return TYID_UNKNOWN; /* unreachable: switch is exhaustive over DeclKind (-Wswitch enforces) */
}

/* Complete the node→type map for every top-level declaration node, keyed by the decl node — the SAME
 * key locals use (see the SN_BIND_STMT handler) — so a uniform, kind-agnostic reader covers every
 * binding form at file scope. Records unconditionally (an UNKNOWN is the map's default and is inert),
 * so "this node has a type" is a complete fact, not a curated subset. Runs after const-fixpoint +
 * type-fill, so all sources are final. */
static void sem_record_binding_types(SemanticContext *ctx) {
	if (!ctx->model)
		return;
	for (int di = 0; di < ctx->decl_count; di++) {
		DeclSummary *d = ctx->decls[di];
		if (!sv_present(d->node))
			continue;
		sem_model_set_expr_type_id(ctx->model, sv_id(d->node), sem_decl_type_id(ctx, d));
	}
}

/* The policy a fallible op explicitly applies (`expr !P`), resolved by the op's category — or NULL.
 * Bounds for index/slice, divide for `/`,`%`; `?`-handlers (pool) are a separate mechanism. */
static DeclSummary *policy_op_applies(SemanticContext *ctx, SyntaxView v) {
	PolicyCategory cat;
	SyntaxNodeKind k = sv_kind(v);
	if (k == SN_INDEX_EXPR || k == SN_SLICE_EXPR) {
		cat = POLICY_CAT_BOUNDS;
	} else if (k == SN_BINARY_EXPR) {
		int op = sem_binary_op(v);
		if (op != OP_DIV && op != OP_MOD)
			return NULL;
		cat = POLICY_CAT_DIVIDE;
	} else {
		return NULL;
	}
	SyntaxView pol = sv_child(v, SN_POLICY_REF);
	if (!sv_present(pol) || sv_token(pol, TOK_QUESTION).ptr) /* `!` panic policies only */
		return NULL;
	SynText t = sv_token(pol, TOK_IDENT);
	if (!t.ptr)
		return NULL;
	char *nm = sem_txt_dup(t);
	DeclSummary *p = nm ? find_policy_sig_cat(ctx, nm, cat) : NULL;
	free(nm);
	return p;
}

/* Collect (deduped, capped) every policy the subtree `v` explicitly applies. */
static void policy_edges_walk(SemanticContext *ctx, SyntaxView v, DeclSummary **out, int *n, int cap) {
	if (!sv_present(v))
		return;
	DeclSummary *p = policy_op_applies(ctx, v);
	if (p) {
		int dup = 0;
		for (int i = 0; i < *n; i++)
			if (out[i] == p) {
				dup = 1;
				break;
			}
		if (!dup && *n < cap)
			out[(*n)++] = p;
	}
	for (int i = 0; i < v.node->child_count; i++)
		if (v.node->children[i].tag == SE_NODE)
			policy_edges_walk(ctx, (SyntaxView){v.node->children[i].as.node, v.src}, out, n, cap);
}

/* True if policy `cur` can reach `target` through explicit `!P` applications. `seen` prevents infinite
 * looping in THIS traversal (we're detecting cycles), bounded by `cap`. */
static int policy_reaches(SemanticContext *ctx, DeclSummary *cur, DeclSummary *target, DeclSummary **seen, int *seen_n,
                          int cap) {
	if (!cur || !sv_present(cur->body_node))
		return 0;
	DeclSummary *edges[32];
	int en = 0;
	for (int i = 0, m = sem_stmt_count(cur->body_node); i < m; i++)
		policy_edges_walk(ctx, sem_stmt_at(cur->body_node, i), edges, &en, 32);
	for (int i = 0; i < en; i++) {
		if (edges[i] == target)
			return 1;
		int visited = 0;
		for (int j = 0; j < *seen_n; j++)
			if (seen[j] == edges[i]) {
				visited = 1;
				break;
			}
		if (!visited && *seen_n < cap) {
			seen[(*seen_n)++] = edges[i];
			if (policy_reaches(ctx, edges[i], target, seen, seen_n, cap))
				return 1;
		}
	}
	return 0;
}

/* E0214: a policy is inlined as a MACRO, so a policy that applies itself (directly or transitively)
 * would expand forever — a codegen stack overflow. Detect such cycles over explicit `!P` applications
 * in policy bodies and reject before codegen. (The implicit default of an unannotated op resolves to a
 * core total policy that doesn't index, so it can't close a cycle — only explicit `!P` can.) */
static void sem_check_policy_cycles(SemanticContext *ctx) {
	for (int di = 0; di < ctx->decl_count; di++) {
		DeclSummary *d = ctx->decls[di];
		if (!d->is_policy || d->is_extern || !sv_present(d->body_node))
			continue;
		DeclSummary *seen[64];
		int seen_n = 0;
		if (policy_reaches(ctx, d, d, seen, &seen_n, 64))
			sem_emit_cyclic_policy(ctx, d->loc, d->name ? d->name : "<anon>");
	}
}

/* Failure-policy validation: walk every proc/func/policy body and check each index/slice op's policy
 * against the prover's verdict (proven-safe ⇒ no policy; provably-OOB ⇒ error; unprovable ⇒ validate
 * the explicit `!name`). See bnd_policy_check for the emitted diagnostics (E0097-99/E0124/W0018). */
static void sem_check_policies(SemanticContext *ctx) {
	for (int di = 0; di < ctx->decl_count; di++) {
		DeclSummary *d = ctx->decls[di];
		if ((d->kind != DECL_PROC && d->kind != DECL_FUNC && d->kind != DECL_SYS) || d->is_extern)
			continue;
		if (!sv_present(d->body_node))
			continue;
		BndEnv e;
		e.count = 0;
		e.local_count = 0;
		e.lint_columns = 0;
		e.check_policies = 1;
		for (int i = 0, n = sem_stmt_count(d->body_node); i < n; i++)
			bnd_check_stmt(ctx, d, sem_stmt_at(d->body_node, i), &e);
		bnd_env_truncate(&e, 0);
		for (int i = 0; i < e.local_count; i++)
			free(e.locals[i].name);
	}
}

/* True if in-param `param_idx` is an in-out param: its name also appears in the out-list.
 * Mirrors codegen's proc_out_param_is_inout (codegen.c). An in-out's out-param shadows it. */
static int proc_param_is_inout(DeclSummary *proc, int param_idx) {
	if (!proc || param_idx < 0 || param_idx >= proc->param_count)
		return 0;
	const char *pn = proc->params[param_idx].name;
	if (!pn)
		return 0;
	for (int i = 0; i < proc->out_param_count; i++)
		if (proc->out_params[i].name && strcmp(proc->out_params[i].name, pn) == 0)
			return 1;
	return 0;
}

static void analyze_proc_decl(SemanticContext *ctx, DeclSummary *proc) {
	if (!proc)
		return;

	/* Register proc name as a known function */
	register_func(ctx, proc->name);

	/* Validate parameters for extern vs non-extern rules. */
	for (int i = 0; i < proc->param_count; i++) {
		ParamSummary *p = &proc->params[i];
		reject_meta_type(ctx, p->type_id, p->loc, "parameter type");
		if (proc->is_extern) {
			const char *tname = tyid_nominal_name(ctx->ty_arena, p->type_id);
			if (tname && !is_primitive_type_name(tname) && !find_archetype(ctx, tname) && !is_type_alias(ctx, tname)) {
				sem_emit_extern_proc_bad_type(ctx, p->loc, tname, proc->name);
			}
			/* An extern is assumed to mutate every in-param (no body to verify). A mutated borrow
			 * breaks the read-only contract, so a by-ref array in-param must be `own` — UNLESS an
			 * out-param shadows it (in-out), in which case the write targets the out place. */
			if (type_is_byref_aggregate(ctx->ty_arena, p->type_id) && !p->is_own && !proc_param_is_inout(proc, i)) {
				sem_emit_extern_array_param_needs_own(ctx, p->loc, p->name ? p->name : "?", proc->name);
			}
			/* `consume` is valid on any param type (consume consumes — not opaque-special). */
		}
	}

	/* For extern procs, validate the out-param types too (parity with extern func return). An
	 * out-only out-param maps to the C return value; an in-out one to an in-place pointer write. */
	if (proc->is_extern) {
		for (int i = 0; i < proc->out_param_count; i++) {
			const char *tname = tyid_nominal_name(ctx->ty_arena, proc->out_params[i].type_id);
			if (tname && !is_primitive_type_name(tname) && !find_archetype(ctx, tname) && !is_type_alias(ctx, tname)) {
				sem_emit_extern_proc_bad_return(ctx, proc->out_params[i].loc, tname, proc->name);
			}
		}
		return;
	}

	/* Validate `archetype` parameter constraints: at most one per proc. */
	int archetype_param_count = 0;
	for (int i = 0; i < proc->param_count; i++) {
		if (tyid_kind(ctx->ty_arena, proc->params[i].type_id) == TYK_ARCHETYPE_CATEGORY) {
			archetype_param_count++;
		}
	}
	if (archetype_param_count > 1) {
		sem_emit_multiple_archetype_params(ctx, proc->loc, proc->name);
	}

	push_scope(ctx);

	/* Add parameters as variables in proc scope */
	for (int i = 0; i < proc->param_count; i++) {
		const char *param_name = proc->params[i].name;
		TypeId pid = proc->params[i].type_id;

		/* Check if param type is an archetype (a bare nominal naming a known shape) */
		const char *type_name = tyid_nominal_name(ctx->ty_arena, pid);
		const char *arch_name = (type_name && find_archetype(ctx, type_name)) ? type_name : NULL;

		if (arch_name)
			add_variable_with_archetype(ctx, param_name, pid, arch_name);
		else
			add_variable(ctx, param_name, pid);
		mark_last_param(ctx, proc->params[i].is_own);
	}

	/* Out-params are writable places the body fills. An OUT-ONLY param is a fresh owned slot.
	 * An IN-OUT param (its name is also in the in-list) is registered AGAIN here so the out
	 * place SHADOWS the in-list borrow — that is what lets the body write it without `own`
	 * (ownership is enforced on the in-list only). The shadow inherits the matching in-param's
	 * `is_own`, so a borrowed in-out stays non-movable (only mutable in place) while an `own`
	 * in-out stays movable. `name = …` in the body resolves to this place. */
	for (int i = 0; i < proc->out_param_count; i++) {
		const char *on = proc->out_params[i].name;
		int in_idx = -1;
		for (int j = 0; j < proc->param_count; j++)
			if (proc->params[j].name && on && strcmp(proc->params[j].name, on) == 0) {
				in_idx = j;
				break;
			}
		add_variable(ctx, on, proc->out_params[i].type_id);
		mark_last_param(ctx, in_idx >= 0 ? proc->params[in_idx].is_own : 1);
		mark_last_out_place(ctx);
	}

	DeclSummary *prev_proc = ctx->current_proc;
	ctx->current_proc = proc;
	ctx->in_body = 1;
	for (int i = 0, n = sem_stmt_count(proc->body_node); i < n; i++)
		analyze_statement(ctx, sem_stmt_at(proc->body_node, i));
	ctx->in_body = 0;

	pop_scope(ctx); /* with current_proc still = proc, so the unused-local lint can see its out-params */
	ctx->current_proc = prev_proc;

	/* No flow check: an unwritten out-param is fine — out slots are zero-initialized before the
	 * call (a fresh `name:` is zero-stored; an existing place is already initialized), so a proc
	 * can never hand back garbage. Zero is just its default result. */

	/* Run the proc-vs-func lints after typechecking the body. */
	lint_proc_decl(ctx, proc);
}

static void analyze_sys_decl(SemanticContext *ctx, DeclSummary *sys) {
	if (!sys)
		return;

	push_scope(ctx);

	/* infer which archetype this sys operates on by matching parameter names to fields */
	const char *sys_archetype = NULL;
	ArchetypeInfo *arch_info = NULL;
	for (int a = 0; a < ctx->archetype_count; a++) {
		int matches = 0;
		for (int p = 0; p < sys->param_count; p++) {
			if (find_field(ctx->archetypes[a], sys->params[p].name)) {
				matches++;
			}
		}
		/* if all parameters match fields in this archetype, this is our archetype */
		if (matches == sys->param_count && sys->param_count > 0) {
			sys_archetype = archetype_any_alias(ctx, ctx->archetypes[a]);
			arch_info = ctx->archetypes[a];
			break;
		}
	}

	/* Check that no parameter is a handle column */
	if (arch_info) {
		for (int p = 0; p < sys->param_count; p++) {
			FieldInfo *field = find_field(arch_info, sys->params[p].name);
			if (field && tyid_kind(ctx->ty_arena, field->type_id) == TYK_HANDLE) {
				sem_emit_handle_in_sys_param(ctx, sys->params[p].loc, sys->params[p].name);
			}
		}
	}

	/* add parameters as variables, using field types from archetype if available */
	for (int i = 0; i < sys->param_count; i++) {
		reject_meta_type(ctx, sys->params[i].type_id, sys->params[i].loc, "sys parameter type");
		TypeId param_type = sys->params[i].type_id;
		/* If no explicit type and we found the archetype, use the field's type */
		if (param_type == TYID_UNKNOWN && arch_info) {
			FieldInfo *field = find_field(arch_info, sys->params[i].name);
			if (field)
				param_type = field->type_id;
		}
		add_variable(ctx, sys->params[i].name, param_type);
		mark_last_param(ctx, sys->params[i].is_own);
	}

	const char *old_sys_archetype = ctx->current_sys_archetype;
	ctx->current_sys_archetype = sys_archetype;

	int prev_in_sys = ctx->in_sys;
	ctx->in_sys = 1;
	ctx->in_body = 1;
	for (int i = 0, n = sem_stmt_count(sys->body_node); i < n; i++)
		analyze_statement(ctx, sem_stmt_at(sys->body_node, i));
	ctx->in_body = 0;
	ctx->in_sys = prev_in_sys;

	ctx->current_sys_archetype = old_sys_archetype;
	pop_scope(ctx);
}

static void analyze_func_group(SemanticContext *ctx, DeclSummary *group) {
	if (!group)
		return;

	/* Name collision: group name must not match a prior func, proc, extern, or group. */
	if (find_known_func(ctx, group->name) || find_group(ctx, group->name)) {
		sem_emit_group_name_collision(ctx, group->loc, group->name);
		return;
	}

	if (group->member_count == 0) {
		sem_emit_empty_group(ctx, group->loc);
		return;
	}

	DeclSummary **resolved = calloc(group->member_count, sizeof(DeclSummary *));
	for (int i = 0; i < group->member_count; i++) {
		DeclSummary *fd = find_func_sig(ctx, group->member_names[i]);
		if (!fd) {
			sem_emit_unknown_group_member(ctx, group->loc, group->member_names[i], group->name);
			continue;
		}
		if (fd->is_extern) {
			sem_emit_group_member_extern(ctx, group->loc, group->member_names[i], group->name);
		}
		/* Forward-reference check: members declared earlier in source are already in known_funcs. */
		if (!find_known_func(ctx, group->member_names[i])) {
			sem_emit_group_member_not_declared(ctx, group->loc, group->member_names[i], group->name);
		}
		resolved[i] = fd;
	}

	/* Pairwise signature distinctness. */
	for (int i = 0; i < group->member_count; i++) {
		if (!resolved[i])
			continue;
		for (int j = i + 1; j < group->member_count; j++) {
			if (!resolved[j])
				continue;
			if (resolved[i]->param_count != resolved[j]->param_count)
				continue;
			int all_eq = 1;
			for (int k = 0; k < resolved[i]->param_count; k++) {
				if (!tyid_equal(resolved[i]->params[k].type_id, resolved[j]->params[k].type_id)) {
					all_eq = 0;
					break;
				}
			}
			if (all_eq) {
				sem_emit_duplicate_group_signatures(ctx, group->loc, group->member_names[i], group->member_names[j],
				                                    group->name);
			}
		}
	}
	free(resolved);

	/* Register the group and treat its name as a known callable. */
	ctx->groups = realloc(ctx->groups, (ctx->group_count + 1) * sizeof(GroupInfo));
	GroupInfo *gi = &ctx->groups[ctx->group_count++];
	gi->name = malloc(strlen(group->name) + 1);
	strcpy(gi->name, group->name);
	gi->members = group->member_names; /* borrowed */
	gi->member_count = group->member_count;
	gi->loc = group->loc;
	register_func(ctx, group->name);
}

static void analyze_func_decl(SemanticContext *ctx, DeclSummary *func) {
	if (!func)
		return;

	/* Register func name as a known function */
	register_func(ctx, func->name);

	/* `archetype` is a proc-only parameter type in v1; funcs cannot have one. */
	for (int i = 0; i < func->param_count; i++) {
		reject_meta_type(ctx, func->params[i].type_id, func->params[i].loc, "parameter type");
		if (tyid_kind(ctx->ty_arena, func->params[i].type_id) == TYK_ARCHETYPE_CATEGORY) {
			sem_emit_archetype_funcs_only(ctx, func->params[i].loc, func->name);
			break;
		}
	}
	for (int i = 0; i < func->return_type_count; i++) {
		reject_meta_type(ctx, func->return_type_ids[i], func->loc, "return type");
		if (tyid_kind(ctx->ty_arena, func->return_type_ids[i]) == TYK_ARCHETYPE_CATEGORY) {
			sem_emit_archetype_not_return_type(ctx, func->loc, func->name);
			break;
		}
	}
	/* Validate parameters and return type for extern vs non-extern rules. */
	for (int i = 0; i < func->param_count; i++) {
		ParamSummary *p = &func->params[i];
		if (func->is_extern) {
			const char *tname = tyid_nominal_name(ctx->ty_arena, p->type_id);
			if (tname && !is_primitive_type_name(tname) && !is_type_alias(ctx, tname)) {
				sem_emit_extern_func_bad_type(ctx, p->loc, tname, func->name);
			}
			/* `consume` is valid on any param type (consume consumes — not opaque-special). */
		}
	}

	/* Validate return types: extern funcs must use known types. */
	if (func->is_extern) {
		for (int i = 0; i < func->return_type_count; i++) {
			const char *tname = tyid_nominal_name(ctx->ty_arena, func->return_type_ids[i]);
			if (tname && !is_primitive_type_name(tname) && !is_type_alias(ctx, tname)) {
				sem_emit_extern_func_bad_return(ctx, func->loc, tname, func->name);
			}
		}
	}

	/* For extern funcs, no body to analyze */
	if (func->is_extern) {
		return;
	}

	push_scope(ctx);

	/* add parameters as variables */
	for (int i = 0; i < func->param_count; i++) {
		add_variable(ctx, func->params[i].name, func->params[i].type_id);
		mark_last_param(ctx, func->params[i].is_own);
	}

	DeclSummary *prev_func = ctx->current_func;
	ctx->current_func = func;
	for (int i = 0, n = sem_stmt_count(func->body_node); i < n; i++)
		analyze_statement(ctx, sem_stmt_at(func->body_node, i));
	ctx->current_func = prev_func;

	enforce_func_purity(ctx, func); /* a `func` must be pure — hard error if not */
	lint_func_could_be_const(ctx, func);

	pop_scope(ctx);
}

static void analyze_decl(SemanticContext *ctx, DeclSummary *ds) {
	if (!ds)
		return;

	/* Push the decl's `@allow(<slug>)` suppression set so lints fired during this
	 * frame can be silenced. Restored on exit — decls aren't nested in arche, but
	 * save/restore keeps the API correct if that ever changes. Errors never
	 * consult this list (errors are not suppressible). */
	char **prev_slugs = ctx->active_allow_slugs;
	int prev_count = ctx->active_allow_slug_count;
	ctx->active_allow_slugs = ds->allow_slugs;
	ctx->active_allow_slug_count = ds->allow_slug_count;

	switch (ds->kind) {
	case DECL_ARCHETYPE:
		analyze_archetype_decl(ctx, ds);
		break;
	case DECL_STATIC:
		if (ds->static_kind == STATIC_KIND_ARCHETYPE)
			analyze_static_decl(ctx, ds);
		else if (ds->static_kind == STATIC_KIND_SCALAR)
			analyze_static_scalar_decl(ctx, ds);
		else
			analyze_static_array_decl(ctx, ds);
		break;
	case DECL_USE:
		/* Module use — resolved before semantic analysis */
		break;
	case DECL_PROC:
		analyze_proc_decl(ctx, ds);
		break;
	case DECL_SYS:
		analyze_sys_decl(ctx, ds);
		break;
	case DECL_FUNC:
		analyze_func_decl(ctx, ds);
		break;
	case DECL_FUNC_GROUP:
		analyze_func_group(ctx, ds);
		break;
	case DECL_CONST:
		/* Value consts and nominal type aliases are collected in pass 0 (and aliases erased before
		 * lowering); there is nothing to analyze per-declaration here. */
		break;
	case DECL_WORLD:
		/* Worlds carry no analyzable body in v1. */
		break;
	case DECL_ENUM:
		/* Registered in pass 0 (enum type + variants); erased before lowering. */
		break;
	}

	ctx->active_allow_slugs = prev_slugs;
	ctx->active_allow_slug_count = prev_count;
}

/* ========== syntax tree -> AstProgram reconstruction (semantic_analyze_cst) ==========
 *
 * Rather than rewrite the ~3000-line analysis traversal onto views, the syntax tree path
 * reconstructs an analyzable `AstProgram` directly from the immutable lossless syntax tree
 * (mirroring lower/lower.c's lower_*_cst walk), then runs the SAME analysis core.
 * This guarantees the side-model + error contract is byte-identical to the
 * AstProgram path: each Expression/Statement carries cst_id = (syntax tree node id + 1), so
 * the side model — keyed by `cst_id - 1` — is keyed by the exact node id the syntax tree
 * lowerer reads back. Module syntax trees are inlined + name-prefixed exactly as main.c's
 * resolve_uses does, and top-level tuple groups are expanded into archetype fields
 * exactly as main.c's expand_archetype_tuple_groups does. */

/* ---- small text helpers ---- */
static char *sem_txt_dup(SynText t) {
	char *s = malloc(t.len + 1);
	if (t.ptr)
		memcpy(s, t.ptr, t.len);
	s[t.len] = '\0';
	return s;
}
static char *sem_cv_dup(SyntaxView v) {
	return sem_txt_dup(sv_text(v));
}
/* Like sem_cv_dup but only the node's first TOKEN leaf — token-precise, so it excludes trailing
 * trivia the node span may include. A const value that is the last token before a comment would
 * otherwise swallow the comment into its lexeme (read as float, leaked into codegen). Mirrors
 * lower.c's sv_dup_first_token. */
char *sem_cv_dup_first_token(SyntaxView v) {
	if (v.node) {
		for (int i = 0; i < v.node->child_count; i++) {
			if (v.node->children[i].tag == SE_TOKEN) {
				SynText t = {v.src + v.node->children[i].as.token.offset, v.node->children[i].as.token.length};
				return sem_txt_dup(t);
			}
		}
	}
	return sem_cv_dup(v);
}
static char *sem_dupz(const char *s) {
	char *r = malloc(strlen(s) + 1);
	strcpy(r, s);
	return r;
}

/* The 1-based line/column of the first DIRECT child token of `kind`, or {0,0}. */
static SourceLoc sem_direct_token_loc(const SyntaxNode *n, TokenKind kind) {
	SourceLoc loc = {0, 0};
	if (!n)
		return loc;
	for (int i = 0; i < n->child_count; i++)
		if (n->children[i].tag == SE_TOKEN && n->children[i].as.token.kind == kind) {
			loc.line = n->children[i].as.token.line;
			loc.column = n->children[i].as.token.column;
			return loc;
		}
	return loc;
}

/* The 1-based line/column of the first token leaf in a node's subtree (the node's start),
 * so reconstructed decls/statements carry source locations for diagnostics. */
SourceLoc sem_node_loc(const SyntaxNode *n) {
	SourceLoc loc = {0, 0};
	if (!n)
		return loc;
	for (int i = 0; i < n->child_count; i++) {
		if (n->children[i].tag == SE_TOKEN) {
			loc.line = n->children[i].as.token.line;
			loc.column = n->children[i].as.token.column;
			return loc;
		}
		SourceLoc sub = sem_node_loc(n->children[i].as.node);
		if (sub.line) {
			return sub;
		}
	}
	return loc;
}

/* ---- type interning (syntax tree type node -> TypeId) ---- */
SyntaxView sem_type_at(SyntaxView v, int idx);
static int sv_type_count_sem(SyntaxView v);

/* 1 if a `name :: <rhs>` const carries the `alias` transparent-marker: a loose IDENT token `alias`
 * sitting after the binding name (the backing-name value is an expr node, not a loose token). */
static int syntax_const_alias_marked(SyntaxView d) {
	int seen_name = 0;
	for (int i = 0; i < d.node->child_count; i++) {
		const SyntaxElem *e = &d.node->children[i];
		if (e->tag != SE_TOKEN || e->as.token.kind != TOK_IDENT)
			continue;
		if (!seen_name) {
			seen_name = 1; /* the binding name */
			continue;
		}
		return e->as.token.length == 5 && memcmp(d.src + e->as.token.offset, "alias", 5) == 0;
	}
	return 0;
}

/* The archetype name inside `handle<X>` / `handle(X)` (the IDENT that isn't "handle"). */
static char *syntax_handle_name(SyntaxView t) {
	for (int i = 0; i < t.node->child_count; i++)
		if (t.node->children[i].tag == SE_TOKEN && t.node->children[i].as.token.kind == TOK_IDENT) {
			SynText nm = {t.src + t.node->children[i].as.token.offset, t.node->children[i].as.token.length};
			if (!(nm.len == 6 && memcmp(nm.ptr, "handle", 6) == 0))
				return sem_txt_dup(nm);
		}
	return sem_dupz("");
}

/* Type name from an SN_TYPE_REF: a qualified `mod.Name` (two IDENTs) folds to `mod_Name` (the
 * module's mangled type symbol), matching lower.c's type_ref_name; a bare type returns its IDENT. */
/* 1 if this SN_TYPE_REF has a `.` token — a qualified `mod.name`. Distinguishes a real two-IDENT
 * qualified type from the `alias T` transparent marker (two adjacent IDENTs, no dot). */
static int sem_type_ref_has_dot(SyntaxView t) {
	for (int i = 0; i < t.node->child_count; i++)
		if (t.node->children[i].tag == SE_TOKEN && t.node->children[i].as.token.kind == TOK_DOT)
			return 1;
	return 0;
}

/* 1 if this SN_TYPE_REF carries the leading `alias` transparent-marker (with a real backing name
 * following): two adjacent IDENTs where the first is `alias`, and no `.` (so it is not `mod.name`). */
static int sem_type_ref_alias_marked(SyntaxView t) {
	SynText ids[2];
	int n = 0;
	for (int i = 0; i < t.node->child_count && n < 2; i++) {
		const SyntaxElem *e = &t.node->children[i];
		if (e->tag == SE_TOKEN && e->as.token.kind == TOK_IDENT) {
			ids[n].ptr = t.src + e->as.token.offset;
			ids[n].len = e->as.token.length;
			n++;
		}
	}
	return n >= 2 && ids[0].len == 5 && memcmp(ids[0].ptr, "alias", 5) == 0 && !sem_type_ref_has_dot(t);
}

static char *sem_type_ref_name(SyntaxView t) {
	SynText ids[2];
	int n = 0;
	for (int i = 0; i < t.node->child_count && n < 2; i++) {
		const SyntaxElem *e = &t.node->children[i];
		if (e->tag == SE_TOKEN && e->as.token.kind == TOK_IDENT) {
			ids[n].ptr = t.src + e->as.token.offset;
			ids[n].len = e->as.token.length;
			n++;
		}
	}
	/* `alias T`: transparent marker — the real type name is the second IDENT, not a `mod.name`. */
	if (n >= 2 && ids[0].len == 5 && memcmp(ids[0].ptr, "alias", 5) == 0 && !sem_type_ref_has_dot(t))
		return sem_txt_dup(ids[1]);
	if (n >= 2) {
		size_t L = ids[0].len + 1 + ids[1].len + 1;
		char *r = malloc(L);
		snprintf(r, L, "%.*s.%.*s", (int)ids[0].len, ids[0].ptr, (int)ids[1].len, ids[1].ptr);
		return r;
	}
	return sem_txt_dup(sv_token(t, TOK_IDENT));
}

/* ---- TypeId interning (Phase 3): the SHARED resolvers used by both the DeclSummary builder and
 * tycheck, so a given type interns to the SAME TypeId everywhere. Ported from tycheck's
 * tyid_from_name/tyid_from_typeref; all arena ops go through ctx->ty_arena. ---- */

TypeId sem_tyid_of_name(SemanticContext *ctx, const char *n) {
	if (!ctx || !n || !n[0])
		return TYID_UNKNOWN;
	TypeArena *arena = ctx->ty_arena;
	TypeId ct = callable_type_alias_id(ctx, n);
	if (ct != TYID_UNKNOWN)
		return ct;
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
	/* `int` is the default integer width = `i32`. Canonicalize it HERE, deterministically — NOT via
	 * the alias registry — so it interns identically whether or not core.arche's `int :: alias i32`
	 * has been registered yet (the registry's population order otherwise makes `int` flip between
	 * `i32` and a bare prim depending on the pass, breaking type identity). Nothing downstream needs
	 * `int`: it is `i32` everywhere in the compiler. */
	if (strcmp(n, "int") == 0)
		n = "i32";
	/* A tier-2 distinct subtype interns by its OWN name (so `meters` != `float`) while carrying its
	 * backing TypeId — its own identity for tyid_equal, one-way usable-as / lowerable-through backing. */
	if (is_type_alias(ctx, n) && !alias_is_transparent(ctx, n)) {
		const char *b = resolve_type_alias(ctx, n);
		TypeId backing = (b && b[0] && strcmp(b, n) != 0) ? sem_tyid_of_name(ctx, b) : TYID_UNKNOWN;
		return tyid_of_nominal_sub(arena, n, backing);
	}
	const char *r = resolve_type_alias(ctx, n);
	if (!r || !r[0])
		return TYID_UNKNOWN;
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
	/* `char_array` stays a distinct nominal (NOT collapsed to PRIM_STR) so it round-trips back to
	 * "char_array" for the resolvers + lowering's CHAR_ARRAY, distinct from the `str` keyword. */
	return tyid_of_nominal(arena, r);
}

/* Intern a type straight from a syntax type-node view — the sole type-node→TypeId builder (mirrors
 * the SN_TYPE_* shapes the parser produces, routing names through sem_tyid_of_name's alias tiering).
 * No intermediate TypeRef. */
TypeId sem_intern_view(SemanticContext *ctx, SyntaxView t) {
	if (!ctx || !sv_present(t))
		return TYID_UNKNOWN;
	TypeArena *arena = ctx->ty_arena;
	switch (sv_kind(t)) {
	case SN_TYPE_REF: {
		char *raw = sem_type_ref_name(t);
		TypeId id;
		if (strcmp(raw, "archetype") == 0)
			id = tyid_of_archetype_category(arena);
		else if (strcmp(raw, "opaque") == 0)
			id = tyid_of_nominal(arena, "opaque");
		else if (strcmp(raw, "type") == 0)
			id = tyid_of_nominal(arena, "type"); /* meta-type, so reject_meta_type can see it */
		else
			id = sem_tyid_of_name(ctx, raw); /* alias tiering (transparent collapse / tier-2 sub) inside */
		free(raw);
		return id;
	}
	case SN_TYPE_ARRAY: {
		char *en = sem_txt_dup(sv_token(t, TOK_IDENT));
		TypeId elem = (strcmp(en, "opaque") == 0) ? tyid_of_nominal(arena, "opaque") : sem_tyid_of_name(ctx, en);
		free(en);
		return tyid_of_slice(arena, elem);
	}
	case SN_TYPE_SHAPED_ARRAY: {
		/* `T[a][b]…` — innermost element is the named type; each `[n]` adds a rank. */
		char *en = sem_txt_dup(sv_token(t, TOK_IDENT));
		TypeId elem = (strcmp(en, "opaque") == 0) ? tyid_of_nominal(arena, "opaque") : sem_tyid_of_name(ctx, en);
		free(en);
		int ranks[16], nr = 0;
		for (int i = 0; i < t.node->child_count && nr < 16; i++)
			if (t.node->children[i].tag == SE_TOKEN && t.node->children[i].as.token.kind == TOK_NUMBER) {
				char buf[32];
				int l = (int)t.node->children[i].as.token.length;
				if (l > 31)
					l = 31;
				memcpy(buf, t.src + t.node->children[i].as.token.offset, l);
				buf[l] = '\0';
				ranks[nr++] = atoi(buf);
			}
		TypeId cur = elem;
		for (int i = nr - 1; i >= 0; i--)
			cur = tyid_of_array(arena, cur, ranks[i]);
		return cur;
	}
	case SN_TYPE_HANDLE: {
		char *hn = syntax_handle_name(t);
		TypeId id = tyid_of_handle(arena, hn);
		free(hn);
		return id;
	}
	case SN_TYPE_TUPLE: {
		/* `(x: T, y: U)` — field names are IDENTs preceding each `:`, types the SN_TYPE_* children. */
		int n = sv_type_count_sem(t);
		char *nbuf[32];
		TypeId tbuf[32];
		char **names = n > 32 ? malloc((size_t)n * sizeof(char *)) : nbuf;
		TypeId *types = n > 32 ? malloc((size_t)n * sizeof(TypeId)) : tbuf;
		int fi = 0;
		const char *pend = NULL;
		int pend_len = 0;
		for (int i = 0; i < t.node->child_count && fi < n; i++) {
			SyntaxElem *ch = &t.node->children[i];
			if (ch->tag == SE_TOKEN && ch->as.token.kind == TOK_IDENT) {
				pend = t.src + ch->as.token.offset;
				pend_len = (int)ch->as.token.length;
			} else if (ch->tag == SE_NODE) {
				SyntaxNodeKind k = ch->as.node->kind;
				if (k >= SN_TYPE_REF && k <= SN_TYPE_FUNC) {
					names[fi] = sem_txt_dup((SynText){pend, pend ? (size_t)pend_len : 0});
					types[fi] = sem_intern_view(ctx, (SyntaxView){ch->as.node, t.src});
					fi++;
					pend = NULL;
				}
			}
		}
		TypeId out = tyid_of_tuple(arena, (const char *const *)names, types, fi);
		for (int i = 0; i < fi; i++)
			free(names[i]);
		if (names != nbuf)
			free(names);
		if (types != tbuf)
			free(types);
		return out;
	}
	case SN_TYPE_PROC:
	case SN_TYPE_FUNC: {
		int is_proc = (sv_kind(t) == SN_TYPE_PROC);
		int np = sv_count(t, SN_PARAM);
		TypeId pbuf[32];
		TypeId *params = np > 32 ? malloc((size_t)np * sizeof(TypeId)) : pbuf;
		for (int i = 0; i < np; i++)
			params[i] = sem_intern_view(ctx, sem_type_at(sv_child_at(t, SN_PARAM, i), 0));
		TypeId rbuf[8];
		int nr;
		TypeId *rets;
		if (is_proc) {
			nr = sv_count(t, SN_OUT_PARAM);
			rets = nr > 8 ? malloc((size_t)nr * sizeof(TypeId)) : rbuf;
			for (int i = 0; i < nr; i++)
				rets[i] = sem_intern_view(ctx, sem_type_at(sv_child_at(t, SN_OUT_PARAM, i), 0));
		} else {
			/* a func's single return is the SN_TYPE_FUNC's direct type-node child */
			nr = 1;
			rets = rbuf;
			rets[0] = sem_intern_view(ctx, sem_type_at(t, 0));
		}
		TypeId out = is_proc ? tyid_of_proc(arena, params, np, rets, nr) : tyid_of_func(arena, params, np, rets, nr);
		if (params != pbuf)
			free(params);
		if (rets != rbuf)
			free(rets);
		return out;
	}
	default:
		return TYID_UNKNOWN;
	}
}

/* A stable, resolved base type-NAME for a TypeId (arrays → element; nominal → backing chain →
 * prim/name; handle → its archetype name). Returns a static literal or an arena-interned string
 * (both outlive the context), so callers may store the result. NULL if unknown. Mirrors what the old
 * resolve_name_type/resolve_field_type returned from a TypeRef. */
static const char *sem_tyid_name(SemanticContext *ctx, TypeId t) {
	if (!ctx)
		return NULL;
	TypeArena *a = ctx->ty_arena;
	while (tyid_kind(a, t) == TYK_SLICE || tyid_kind(a, t) == TYK_ARRAY)
		t = tyid_elem(a, t);
	if (tyid_kind(a, t) == TYK_HANDLE)
		return tyid_handle_name(a, t);
	while (tyid_kind(a, t) == TYK_NOMINAL && tyid_backing(a, t) != TYID_UNKNOWN)
		t = tyid_backing(a, t);
	switch (tyid_kind(a, t)) {
	case TYK_PRIM:
		switch (tyid_prim(a, t)) {
		case PRIM_INT:
			return "i32";
		case PRIM_FLOAT:
			return "float";
		case PRIM_CHAR:
			return "char";
		case PRIM_STR:
			return "str";
		case PRIM_VOID:
			return "void";
		case PRIM_BOOL:
			return "bool";
		default:
			return NULL;
		}
	case TYK_NOMINAL:
		return tyid_nominal_name(a, t);
	case TYK_ARCHETYPE_CATEGORY:
		return "archetype";
	default:
		return NULL;
	}
}

/* ---- syntax tree navigation (same shapes as lower/lower.c) ---- */
SyntaxView sem_first_expr(SyntaxView v) {
	for (int i = 0; i < v.node->child_count; i++)
		if (v.node->children[i].tag == SE_NODE) {
			SyntaxNodeKind k = v.node->children[i].as.node->kind;
			if (k >= SN_LITERAL_EXPR && k <= SN_PAREN_EXPR)
				return (SyntaxView){v.node->children[i].as.node, v.src};
		}
	return (SyntaxView){NULL, v.src};
}
SyntaxView sem_type_at(SyntaxView v, int idx) {
	int c = 0;
	for (int i = 0; i < v.node->child_count; i++)
		if (v.node->children[i].tag == SE_NODE) {
			SyntaxNodeKind k = v.node->children[i].as.node->kind;
			if (k >= SN_TYPE_REF && k <= SN_TYPE_FUNC) {
				if (c == idx)
					return (SyntaxView){v.node->children[i].as.node, v.src};
				c++;
			}
		}
	return (SyntaxView){NULL, v.src};
}
static int sv_type_count_sem(SyntaxView v) {
	int c = 0;
	for (int i = 0; i < v.node->child_count; i++)
		if (v.node->children[i].tag == SE_NODE) {
			SyntaxNodeKind k = v.node->children[i].as.node->kind;
			if (k >= SN_TYPE_REF && k <= SN_TYPE_FUNC)
				c++;
		}
	return c;
}
SyntaxView sem_node_at_expr(SyntaxView v, int idx) {
	int c = 0;
	for (int i = 0; i < v.node->child_count; i++)
		if (v.node->children[i].tag == SE_NODE) {
			SyntaxNodeKind k = v.node->children[i].as.node->kind;
			if (k >= SN_LITERAL_EXPR && k <= SN_PAREN_EXPR) {
				if (c == idx)
					return (SyntaxView){v.node->children[i].as.node, v.src};
				c++;
			}
		}
	return (SyntaxView){NULL, v.src};
}
Operator sem_binary_op(SyntaxView v) {
	for (int i = 0; i < v.node->child_count; i++)
		if (v.node->children[i].tag == SE_TOKEN) {
			Operator op = sem_tok_to_op(v.node->children[i].as.token.kind);
			if (op != OP_NONE)
				return op;
		}
	return OP_NONE;
}
int sem_expr_count(SyntaxView v) {
	int c = 0;
	for (int i = 0; i < v.node->child_count; i++)
		if (v.node->children[i].tag == SE_NODE) {
			SyntaxNodeKind k = v.node->children[i].as.node->kind;
			if (k >= SN_LITERAL_EXPR && k <= SN_PAREN_EXPR)
				c++;
		}
	return c;
}

static Operator sem_tok_to_op(TokenKind k) {
	switch (k) {
	case TOK_PLUS:
		return OP_ADD;
	case TOK_MINUS:
		return OP_SUB;
	case TOK_STAR:
		return OP_MUL;
	case TOK_SLASH:
		return OP_DIV;
	case TOK_EQ_EQ:
		return OP_EQ;
	case TOK_BANG_EQ:
		return OP_NEQ;
	case TOK_LT:
		return OP_LT;
	case TOK_GT:
		return OP_GT;
	case TOK_LT_EQ:
		return OP_LTE;
	case TOK_GT_EQ:
		return OP_GTE;
	case TOK_AMP_AMP:
		return OP_AND;
	case TOK_PIPE_PIPE:
		return OP_OR;
	default:
		return OP_NONE;
	}
}

/* Name string of an SN_NAME_EXPR view. `table<Name>` in value position resolves to the bare
 * archetype name (the 2nd IDENT); otherwise the sole IDENT. Caller frees. Shared by
 * cst_build_expr and the view-driven analysis so the two never disagree. */
static char *sv_name_expr_dup(SyntaxView e) {
	if (has_nested_base(e))
		return sv_name_expr_dup(base_subexpr(e)); /* leftmost IDENT of a nested postfix base */
	if (sv_has_token(e, TOK_LT)) {
		char *nm = NULL;
		int seen = 0;
		for (int i = 0; i < e.node->child_count; i++)
			if (e.node->children[i].tag == SE_TOKEN && e.node->children[i].as.token.kind == TOK_IDENT) {
				SynText t = {e.src + e.node->children[i].as.token.offset, e.node->children[i].as.token.length};
				if (seen++) {
					nm = sem_txt_dup(t);
					break;
				}
			}
		return nm ? nm : sem_cv_dup(e);
	}
	return sem_txt_dup(sv_token(e, TOK_IDENT));
}

static char *sv_resolved_name(SemanticContext *ctx, SyntaxView v) {
	const char *r = (ctx && ctx->model && sv_present(v)) ? sem_model_ref_name(ctx->model, sv_id(v)) : NULL;
	if (r)
		return sem_dupz(r);
	return sv_name_expr_dup(v);
}

/* Scan a syntax tree decl node's direct-child tokens for `@allow(<slug>)` decorators and
 * return the captured slugs as a freshly allocated array (caller takes ownership;
 * decl_free releases). Multiple decorators are accepted; the search advances past
 * each `@ allow ( IDENT )` 5-token sequence to find the next one. */
static void syntax_extract_allow_slugs(SyntaxView d, char ***out_slugs, int *out_count) {
	int count = 0;
	char **slugs = NULL;
	int n = d.node->child_count;
	for (int i = 0; i + 4 < n; i++) {
		const SyntaxElem *e1 = &d.node->children[i];
		if (e1->tag != SE_TOKEN || e1->as.token.kind != TOK_AT)
			continue;
		const SyntaxElem *e2 = &d.node->children[i + 1];
		if (e2->tag != SE_TOKEN || e2->as.token.kind != TOK_IDENT)
			continue;
		if (e2->as.token.length != 5 || memcmp(d.src + e2->as.token.offset, "allow", 5) != 0)
			continue;
		const SyntaxElem *e3 = &d.node->children[i + 2];
		if (e3->tag != SE_TOKEN || e3->as.token.kind != TOK_LPAREN)
			continue;
		const SyntaxElem *e4 = &d.node->children[i + 3];
		if (e4->tag != SE_TOKEN || e4->as.token.kind != TOK_IDENT)
			continue;
		const SyntaxElem *e5 = &d.node->children[i + 4];
		if (e5->tag != SE_TOKEN || e5->as.token.kind != TOK_RPAREN)
			continue;
		size_t slugL = e4->as.token.length;
		char *slug = malloc(slugL + 1);
		memcpy(slug, d.src + e4->as.token.offset, slugL);
		slug[slugL] = '\0';
		slugs = realloc(slugs, (size_t)(count + 1) * sizeof(char *));
		slugs[count++] = slug;
		i += 4; /* advance past the 5-token decorator (the loop bump adds 1 more) */
	}
	*out_slugs = slugs;
	*out_count = count;
}

/* Scan a syntax tree decl node's direct-child tokens for a `@drop` decorator (the `@ drop`
 * two-token sequence). Returns 1 if present. */
static int syntax_has_drop_decorator(SyntaxView d) {
	int n = d.node->child_count;
	for (int i = 0; i + 1 < n; i++) {
		const SyntaxElem *e1 = &d.node->children[i];
		if (e1->tag != SE_TOKEN || e1->as.token.kind != TOK_AT)
			continue;
		const SyntaxElem *e2 = &d.node->children[i + 1];
		if (e2->tag != SE_TOKEN || e2->as.token.kind != TOK_IDENT)
			continue;
		if (e2->as.token.length == 4 && memcmp(d.src + e2->as.token.offset, "drop", 4) == 0)
			return 1;
	}
	return 0;
}

/* The type named in `@drop(<type>)`, e.g. "socket" — the IDENT two tokens after the `drop` IDENT
 * (skipping `(`). NULL if absent/malformed. Caller owns the returned string. */
static char *syntax_drop_type(SyntaxView d) {
	int n = d.node->child_count;
	for (int i = 0; i + 3 < n; i++) {
		const SyntaxElem *at = &d.node->children[i];
		if (at->tag != SE_TOKEN || at->as.token.kind != TOK_AT)
			continue;
		const SyntaxElem *kw = &d.node->children[i + 1];
		if (kw->tag != SE_TOKEN || kw->as.token.kind != TOK_IDENT)
			continue;
		if (kw->as.token.length != 4 || memcmp(d.src + kw->as.token.offset, "drop", 4) != 0)
			continue;
		const SyntaxElem *lp = &d.node->children[i + 2];
		const SyntaxElem *ty = &d.node->children[i + 3];
		if (lp->tag == SE_TOKEN && lp->as.token.kind == TOK_LPAREN && ty->tag == SE_TOKEN &&
		    ty->as.token.kind == TOK_IDENT) {
			size_t len = ty->as.token.length;
			char *s = malloc(len + 1);
			memcpy(s, d.src + ty->as.token.offset, len);
			s[len] = '\0';
			return s;
		}
	}
	return NULL;
}

/* The op category a `policy` decl serves, from its `@policy(<category>)` decorator: the IDENT inside
 * the parens. `policy` lexes as TOK_POLICY (a keyword), so the sequence is `@ policy ( <cat> )`.
 * POLICY_CAT_NONE if absent/malformed/unrecognized. */
static PolicyCategory syntax_policy_category(SyntaxView d) {
	int n = d.node->child_count;
	for (int i = 0; i + 4 < n; i++) {
		const SyntaxElem *at = &d.node->children[i];
		if (at->tag != SE_TOKEN || at->as.token.kind != TOK_AT)
			continue;
		const SyntaxElem *kw = &d.node->children[i + 1];
		if (kw->tag != SE_TOKEN || kw->as.token.kind != TOK_POLICY)
			continue;
		const SyntaxElem *lp = &d.node->children[i + 2];
		const SyntaxElem *cat = &d.node->children[i + 3];
		const SyntaxElem *rp = &d.node->children[i + 4];
		if (lp->tag != SE_TOKEN || lp->as.token.kind != TOK_LPAREN || cat->tag != SE_TOKEN ||
		    cat->as.token.kind != TOK_IDENT || rp->tag != SE_TOKEN || rp->as.token.kind != TOK_RPAREN)
			continue;
		const char *p = d.src + cat->as.token.offset;
		size_t len = cat->as.token.length;
		if (len == 6 && memcmp(p, "bounds", 6) == 0)
			return POLICY_CAT_BOUNDS;
		if (len == 4 && memcmp(p, "pool", 4) == 0)
			return POLICY_CAT_POOL;
		if (len == 6 && memcmp(p, "divide", 6) == 0)
			return POLICY_CAT_DIVIDE;
		return POLICY_CAT_NONE;
	}
	return POLICY_CAT_NONE;
}

/* Build the `@implements` map from every decl's `@implements(<dev>.<req>, …)` decorator: each
 * requirement tail maps to the decorated decl's own name. Mirrors lower.c collect_impl_binds but
 * keyed for semantic use (datasheet-type skip + device param rename). */
static void sem_build_impl_map(SemanticContext *ctx) {
	g_sem_impl_n = 0;
	for (int i = 0; i < ctx->decl_count; i++) {
		DeclSummary *c = ctx->decls[i];
		if (!c || !c->name || !c->node.node)
			continue;
		const SyntaxNode *decl = c->node.node;
		const char *src = c->node.src;
		int n = decl->child_count;
		for (int a = 0; a + 1 < n; a++) {
			const SyntaxElem *ea = &decl->children[a];
			const SyntaxElem *eb = &decl->children[a + 1];
			if (ea->tag != SE_TOKEN || ea->as.token.kind != TOK_AT)
				continue;
			if (eb->tag != SE_TOKEN || eb->as.token.kind != TOK_IDENT)
				continue;
			if (eb->as.token.length != 10 || memcmp(src + eb->as.token.offset, "implements", 10) != 0)
				continue;
			int j = a + 2;
			if (j >= n || decl->children[j].tag != SE_TOKEN || decl->children[j].as.token.kind != TOK_LPAREN)
				continue;
			const char *tail = NULL;
			int tail_len = 0;
			for (j++; j < n; j++) {
				const SyntaxElem *t = &decl->children[j];
				if (t->tag != SE_TOKEN)
					continue;
				int k = t->as.token.kind;
				if (k == TOK_IDENT) {
					tail = src + t->as.token.offset; /* keep the LAST segment of a qualified `dev.req` */
					tail_len = (int)t->as.token.length;
				} else if (k == TOK_COMMA || k == TOK_RPAREN) {
					if (tail && g_sem_impl_n < SEM_IMPL_CAP) {
						char *req = malloc((size_t)tail_len + 1);
						memcpy(req, tail, (size_t)tail_len);
						req[tail_len] = '\0';
						g_sem_impl_req[g_sem_impl_n] = req;
						g_sem_impl_driver[g_sem_impl_n] = sem_dupz(c->name);
						g_sem_impl_n++;
					}
					tail = NULL;
					if (k == TOK_RPAREN)
						break;
				}
			}
		}
	}
}

static void sem_clear_impl_map(void) {
	for (int i = 0; i < g_sem_impl_n; i++) {
		free(g_sem_impl_req[i]);
		free(g_sem_impl_driver[i]);
	}
	g_sem_impl_n = 0;
}

/* Re-intern a param/return TypeId through the @implements map: if its nominal name is a device
 * requirement, give it the driver's type identity instead. */
static TypeId impl_rename_tid(SemanticContext *ctx, TypeId t) {
	const char *drv = impl_driver_for(tyid_nominal_name(ctx->ty_arena, t));
	return drv ? sem_tyid_of_name(ctx, drv) : t;
}

/* Apply the @implements renames to every device-impl decl's param/return TypeIds, after the fill.
 * Entry/user decls are untouched (so the importer's own same-named type is preserved). */
static void sem_apply_impl_renames(SemanticContext *ctx) {
	if (g_sem_impl_n == 0)
		return;
	for (int i = 0; i < ctx->decl_count; i++) {
		DeclSummary *d = ctx->decls[i];
		if (!d || !d->from_device_impl)
			continue;
		for (int p = 0; p < d->param_count; p++)
			d->params[p].type_id = impl_rename_tid(ctx, d->params[p].type_id);
		for (int p = 0; p < d->out_param_count; p++)
			d->out_params[p].type_id = impl_rename_tid(ctx, d->out_params[p].type_id);
		for (int r = 0; r < d->return_type_count; r++)
			d->return_type_ids[r] = impl_rename_tid(ctx, d->return_type_ids[r]);
	}
}

/* ===== Unified-grammar RHS value forms (P2 classification) =====
 * In the unified grammar a declaration is a binding `name :: <form>`. These helpers build the
 * abstract decl from the RHS value-form node `f` (an SN_PROC_EXPR / SN_FUNC_EXPR / …) with the
 * name taken from the binding LHS. The extraction mirrors the legacy keyword-led decl cases
 * below (which become dead code once old syntax is removed); the only differences are the name
 * source and that children are read from `f` rather than the decl node. `name` is owned. */

/* The binding LHS name: the IDENT immediately before the first top-level `:` of the decl.
 * Skips any leading `@decorator` / `@allow(slug)` idents (which precede the name). */
static SynText sem_binding_name(SyntaxView d) {
	SynText last = {NULL, 0};
	for (int i = 0; i < d.node->child_count; i++) {
		SyntaxElem *e = &d.node->children[i];
		if (e->tag != SE_TOKEN)
			continue;
		if (e->as.token.kind == TOK_COLON)
			break;
		if (e->as.token.kind == TOK_IDENT)
			last = (SynText){d.src + e->as.token.offset, e->as.token.length};
	}
	return last;
}

/* True if the decl is decorated (its first token is `@`). A decorator with args — `@allow(x)`,
 * `@implements(dev.foo)` — has a `(` that must NOT be mistaken for a tuple-group paren, and a first
 * IDENT that is the decorator, not the binding name (mirror of lower.c's decl_is_decorated). */
static int sem_decl_is_decorated(const SyntaxNode *n) {
	for (int i = 0; i < n->child_count; i++)
		if (n->children[i].tag == SE_TOKEN)
			return n->children[i].as.token.kind == TOK_AT;
	return 0;
}

/* Find the unified-grammar RHS value/type form among a binding's children, if any. */
static SyntaxView sem_rhs_form(SyntaxView d) {
	for (int i = 0; i < d.node->child_count; i++) {
		if (d.node->children[i].tag != SE_NODE)
			continue;
		SyntaxNodeKind k = d.node->children[i].as.node->kind;
		if (k == SN_PROC_EXPR || k == SN_FUNC_EXPR || k == SN_POLICY_EXPR || k == SN_SYS_EXPR || k == SN_ARCH_EXPR ||
		    k == SN_GROUP_EXPR || k == SN_ENUM_EXPR || k == SN_TYPE_PROC || k == SN_TYPE_FUNC) {
			SyntaxView v = {d.node->children[i].as.node, d.src};
			return v;
		}
	}
	SyntaxView none = {NULL, d.src};
	return none;
}

/* ---- module syntax tree registry (parallel to lower_add_module) ---- */
typedef struct {
	char *name;
	const SyntaxNode *root;
	const char *src;
	char *filename;    /* source path; a `*.ds.arche` file is a device datasheet (decls stay global) */
	DeclOrigin origin; /* search root the loader matched: user tree / stdlib / core */
} SemModule;
static SemModule g_sem_modules[64];
static int g_sem_module_count = 0;

/* Editor-only: a module to inline into the ROOT namespace even though the root has no `#import` for
 * it. Set when the open document is a member file of a device folder — its sibling datasheet defines
 * the device's types as global vocabulary (decls stay flat/unprefixed), so the open impl file's bare
 * references resolve. Compilation never sets this (a device is only ever loaded whole, via import), so
 * the compiler's behavior is unchanged. Reset per analysis in semantic_reset_modules. */
static char *g_sem_extra_inline;

/* A device datasheet file: its decls are shared global vocabulary, registered UNPREFIXED (mirror
 * of lower.c's is_datasheet_file). */
static int sem_is_datasheet_file(const char *fn) {
	if (!fn)
		return 0;
	size_t L = strlen(fn);
	return L >= 9 && strcmp(fn + L - 9, ".ds.arche") == 0;
}

void semantic_add_module(const char *name, const SyntaxNode *root, const char *src, const char *filename,
                         DeclOrigin origin) {
	if (g_sem_module_count >= 64 || !name || !root)
		return;
	g_sem_modules[g_sem_module_count].name = sem_dupz(name);
	g_sem_modules[g_sem_module_count].root = root;
	g_sem_modules[g_sem_module_count].src = src;
	g_sem_modules[g_sem_module_count].filename = filename ? sem_dupz(filename) : NULL;
	g_sem_modules[g_sem_module_count].origin = origin;
	g_sem_module_count++;
}

void semantic_reset_modules(void) {
	for (int i = 0; i < g_sem_module_count; i++) {
		free(g_sem_modules[i].name);
		free(g_sem_modules[i].filename);
	}
	g_sem_module_count = 0;
	free(g_sem_extra_inline);
	g_sem_extra_inline = NULL;
}

void semantic_set_extra_inline_module(const char *name) {
	free(g_sem_extra_inline);
	g_sem_extra_inline = name ? sem_dupz(name) : NULL;
}

int semantic_has_module(const char *name) {
	for (int i = 0; i < g_sem_module_count; i++)
		if (strcmp(g_sem_modules[i].name, name) == 0)
			return 1;
	return 0;
}

/* ---- module name-prefixing (mirrors main.c resolve_uses / prefix_module) ---- */
static int sem_name_in_set(char **set, int count, const char *name) {
	for (int i = 0; i < count; i++)
		if (strcmp(set[i], name) == 0)
			return 1;
	return 0;
}
/* Qualify a module-local name to its identity `<mod>.<name>` (dotted). The `.` is a legal LLVM
 * global-identifier character (cf. `@llvm.memcpy`), so this identity needs no emission quoting, and
 * it reads as a clean qualified name in diagnostics. */
static char *sem_prefix_name(const char *prefix, const char *name) {
	int p = (int)strlen(prefix), n = (int)strlen(name);
	char *r = malloc(p + 1 + n + 1);
	memcpy(r, prefix, p);
	r[p] = '.';
	memcpy(r + p + 1, name, n);
	r[p + 1 + n] = 0;
	return r;
}
static void sem_maybe_rename(char **slot, const char *prefix, char **set, int count) {
	if (!*slot || !sem_name_in_set(set, count, *slot))
		return;
	char *old = *slot;
	*slot = sem_prefix_name(prefix, old);
	free(old);
}
/* Context for the qualify pass, so it can emit a `module has no member` diagnostic. Set just before
 * the pass runs (file-static like g_lower_sem; the pass is single-threaded over one program). */
static SemanticContext *g_sem_qualify_ctx = NULL;

/* True if `base` names an imported module (a qualify prefix). */
static int sem_base_is_module(char **prefix, int n, const char *base) {
	for (int m = 0; m < n; m++)
		if (strcmp(base, prefix[m]) == 0)
			return 1;
	return 0;
}

/* Qualified module access (semantic AST mirror of lower.c's hir_q_*): bind `mod.member` to its
 * member's identity. Member is looked up by literal name in the module's export set. */
static int sem_qual_lookup(char **prefix, char ***set, int *count, int n, const char *base, const char *field,
                           char *out, size_t out_sz) {
	for (int m = 0; m < n; m++) {
		if (strcmp(base, prefix[m]) != 0)
			continue;
		for (int s = 0; s < count[m]; s++) {
			/* Each entry is "<visible>=<target-symbol>": match the visible (qualified) name,
			 * resolve to its target. A pure-Arche export targets `<mod>_<name>`; a foreign
			 * export targets its real link name (`fmt.printf` → libc `printf`, `net.listen` →
			 * `net_listen`). */
			const char *e = set[m][s];
			const char *eq = strchr(e, '=');
			if (!eq)
				continue;
			size_t vlen = (size_t)(eq - e);
			if (strlen(field) == vlen && strncmp(field, e, vlen) == 0) {
				snprintf(out, out_sz, "%s", eq + 1);
				return 1;
			}
		}
	}
	return 0;
}

/* AST-kill step C: the rename port — applies the loader's module/file rename to a tree-built
 * DeclSummary IN PLACE, reusing the proven sem_rename_typeref/sem_maybe_rename. Mirrors
 * sem_rename_decl exactly (names + types); body/value exprs are renamed via the channel pass, not
 * here. Lets the tree-direct DeclTable carry resolved names/types without the AstProgram. */
static void sem_rename_decl_summary(DeclSummary *ds, const char *prefix, char **set, int count) {
	if (ds->is_drop)
		sem_maybe_rename(&ds->drop_type, prefix, set, count);
	/* Only NAMES are prefixed. Type references are NOT renamed: a type's identity is the interned
	 * TypeId of its bare name, and both a module decl's signature and any use of that type intern the
	 * same bare name, so they match without a prefix. (DECL_ENUM names are not renamed.) */
	switch (ds->kind) {
	case DECL_ARCHETYPE:
	case DECL_PROC:
	case DECL_SYS:
	case DECL_FUNC:
	case DECL_STATIC:
	case DECL_CONST:
	case DECL_WORLD:
		sem_maybe_rename(&ds->name, prefix, set, count);
		break;
	case DECL_FUNC_GROUP:
		sem_maybe_rename(&ds->name, prefix, set, count);
		for (int i = 0; i < ds->member_count; i++)
			sem_maybe_rename(&ds->member_names[i], prefix, set, count);
		break;
	default:
		break;
	}
}

/* Gated (ARCHE_VALIDATE_TREE_SIG) snapshots of the rename ops the loader applied per decl node, so
 * the tree-direct builder + rename port can be diffed against the AST path. Throwaway scaffolding,
 * removed when build_decl_table flips to tree-direct. */
typedef struct {
	const SyntaxNode *node;
	char *prefix;
	char **set;
	int count;
} SemRnOp;
static SemRnOp *g_rnops = NULL;
static int g_rnop_count = 0, g_rnop_cap = 0, g_rnop_on = 0;
static void sem_record_rnop(const SyntaxNode *node, const char *prefix, char **set, int count) {
	if (!g_rnop_on || !node)
		return;
	if (g_rnop_count >= g_rnop_cap) {
		g_rnop_cap = g_rnop_cap ? g_rnop_cap * 2 : 256;
		g_rnops = realloc(g_rnops, (size_t)g_rnop_cap * sizeof(SemRnOp));
	}
	SemRnOp *r = &g_rnops[g_rnop_count++];
	r->node = node;
	r->prefix = sem_dupz(prefix);
	r->count = count;
	r->set = calloc(count ? count : 1, sizeof(char *));
	for (int i = 0; i < count; i++)
		r->set[i] = sem_dupz(set[i]);
}
static void sem_free_rnops(void) {
	for (int i = 0; i < g_rnop_count; i++) {
		free(g_rnops[i].prefix);
		for (int j = 0; j < g_rnops[i].count; j++)
			free(g_rnops[i].set[j]);
		free(g_rnops[i].set);
	}
	free(g_rnops);
	g_rnops = NULL;
	g_rnop_count = 0;
	g_rnop_cap = 0;
}

/* Transient snapshot of the persisted per-unit interfaces (ctx->interfaces), in the flat array form the
 * tree-qualify channel pass consults to resolve `mod.f` references. Rebuilt each analysis from the
 * registry; freed by sem_free_exports after qualification (the interfaces themselves live on the ctx). */
static char **g_exp_prefix = NULL;
static char ***g_exp_set = NULL;
static int *g_exp_count = NULL;
static int g_exp_n = 0;
static void sem_snapshot_exports(SemanticContext *ctx) {
	int n = ctx->interface_count;
	g_exp_n = n;
	g_exp_prefix = calloc(n ? n : 1, sizeof(char *));
	g_exp_set = calloc(n ? n : 1, sizeof(char **));
	g_exp_count = calloc(n ? n : 1, sizeof(int));
	for (int m = 0; m < n; m++) {
		UnitInterface *u = ctx->interfaces[m];
		g_exp_prefix[m] = sem_dupz(u->unit_name);
		g_exp_count[m] = u->export_count;
		g_exp_set[m] = calloc(u->export_count ? u->export_count : 1, sizeof(char *));
		for (int s = 0; s < u->export_count; s++)
			g_exp_set[m][s] = sem_dupz(u->exports[s]);
	}
}
static void sem_free_exports(void) {
	for (int m = 0; m < g_exp_n; m++) {
		free(g_exp_prefix[m]);
		for (int s = 0; s < g_exp_count[m]; s++)
			free(g_exp_set[m][s]);
		free(g_exp_set[m]);
	}
	free(g_exp_prefix);
	free(g_exp_set);
	free(g_exp_count);
	g_exp_prefix = NULL;
	g_exp_set = NULL;
	g_exp_count = NULL;
	g_exp_n = 0;
}

/* Apply the loader's recorded rename ops for `declnode` to a bare name IN PLACE (mutates *slot,
 * freeing the old) — the same prefixing the AST rename applied to a module-internal reference. */
static void sem_apply_rnops(char **slot, const SyntaxNode *declnode) {
	for (int r = 0; r < g_rnop_count; r++)
		if (g_rnops[r].node == declnode)
			sem_maybe_rename(slot, g_rnops[r].prefix, g_rnops[r].set, g_rnops[r].count);
}

/* Build one module decl from `node`, append it to prog, and record its name in the module's
 * `full` set (intra-module resolution) and — when `exported` — its `expset` (externally visible via
 * qualified `mod.name`). Non-externs are prefixed to `<mod>_<name>` and recorded under their source
 * name. Externs keep their unprefixed decl (the C ABI symbol), are NOT added to `full`, and go into
 * `expset` under the prefix-stripped visible name so `mod.<visible>` reconstructs the C symbol
 * (e.g. `net.listen` → `net_listen`). Shared by the top-level module loop and the recursion into
 * `#foreign { ... }` / `#module { ... }` block regions. */
static void sem_add_module_decl(SemanticContext *ctx, const SyntaxNode *node, const char *msrc, const char *mod_name,
                                char ***full, int *fulln, int *fullcap, char ***expset, int *expn, int *expcap,
                                int exported, int is_datasheet, int module_is_device, int file_local, char ***fileset,
                                int *filesetn, int *filesetcap, int unit, DeclOrigin origin) {
	/* AST-kill: build the resolved DeclSummary straight from the tree node and store it in the table
	 * (bare names; build_decl_table applies this module's recorded rename ops). Provenance flags are
	 * loader context (not tree-derivable), so they are set here. */
	DeclSummary *md = decl_summary_from_node(ctx, (SyntaxView){node, msrc});
	if (!md)
		return;
	/* Datasheet decls are shared global vocabulary: mark them so identical component/type redefs
	 * across two datasheets dedup (devices sharing a shape) instead of tripping define-once. */
	md->is_datasheet = is_datasheet;
	/* Region-band visibility, from the loader's bands: `#file` ⇒ file-local, else `#module` (not
	 * exported) ⇒ unit-private, else exported. The id-keyed reachability + future cross-unit resolution
	 * read this instead of sniffing `.` in the renamed name. */
	md->visibility = file_local ? VIS_FILE : (exported ? VIS_EXPORTED : VIS_UNIT);
	md->unit = unit;
	md->origin = origin;
	/* A decl from a device's IMPL (`.arche` of a unit that also has a `.ds.arche`) — the rule-3 sweep
	 * rejects type/archetype/allocation definitions there (a device's impl is behavior-only). */
	md->from_device_impl = module_is_device && !is_datasheet;
	/* A pool decl in a datasheet (`.ds.arche`) is a storage REQUIREMENT (min rows), not an allocation. */
	if (is_datasheet && md->kind == DECL_STATIC && md->static_kind == STATIC_KIND_ARCHETYPE)
		md->is_requirement = 1;
	ctx->decls[ctx->decl_count++] = md;
	int is_ext = md->is_extern;
	const char *nm = md->name;
	if (!nm)
		return;
	/* A member is accessed by its LITERAL declared name. A decl is registered FLAT (unprefixed, bare
	 * export) when it's foreign (C ABI symbol), a datasheet decl (shared global vocabulary), an EXPORTED
	 * ARCHETYPE (a public shape is global vocabulary — its name is bare, never `<device>.Name`; a shape
	 * in `#module` stays unit-private, so a datasheet storage requirement that names it can't resolve a
	 * public shape and errors), OR from a PLAIN module (no `.ds.arche`) — a plain/path module merges flat
	 * into the importer (Jai `#load`), so `helper()` not `mod.helper()`. Only a DEVICE's pure-Arche impl
	 * behavior decls (procs/systems/funcs) are prefixed to `<device>.<name>` (the namespaced contract). */
	int flat = is_ext || is_datasheet || !module_is_device || (md->kind == DECL_ARCHETYPE && exported);
	/* A `#file` decl is file-local: it must NOT join the cross-file `full` set (so sibling files can't
	 * bind to it) and is never exported. It goes into the per-file `fileset` instead; sem_inline_module
	 * then renames it (+ its intra-file references) to a file-unique identity. */
	if (file_local) {
		if (*filesetn == *filesetcap) {
			*filesetcap = *filesetcap ? *filesetcap * 2 : 8;
			*fileset = realloc(*fileset, (size_t)*filesetcap * sizeof(char *));
		}
		(*fileset)[(*filesetn)++] = sem_dupz(nm);
		return;
	}
	if (!flat) {
		if (*fulln == *fullcap) {
			*fullcap = *fullcap ? *fullcap * 2 : 8;
			*full = realloc(*full, (size_t)*fullcap * sizeof(char *));
		}
		(*full)[(*fulln)++] = sem_dupz(nm);
	}
	if (exported) {
		if (*expn == *expcap) {
			*expcap = *expcap ? *expcap * 2 : 8;
			*expset = realloc(*expset, (size_t)*expcap * sizeof(char *));
		}
		/* "<member>=<identity>": flat → the bare name; a device's pure-Arche decl → `<device>.<name>`. */
		char entry[512];
		if (flat)
			snprintf(entry, sizeof(entry), "%s=%s", nm, nm);
		else
			snprintf(entry, sizeof(entry), "%s=%s.%s", nm, mod_name, nm);
		(*expset)[(*expn)++] = sem_dupz(entry);
	}
}

/* True for a declaration node kind eligible for collection (excludes SN_USE_DECL, handled
 * separately, and SN_REGION, which is a container/marker rather than a decl). */
static int sem_is_collectible_decl(SyntaxNodeKind k) {
	return k >= SN_WORLD_DECL && k <= SN_USE_DECL && k != SN_USE_DECL && k != SN_DEFAULT_DECL;
}

/* The module name an `#import` element resolves to: IDENT = the name verbatim (device by name);
 * STRING = a path (module by path) whose name is the basename minus a trailing `.arche`. Returns a
 * malloc'd name (caller frees), or NULL for any other token. Mirror of lower.c's helper. */
static char *sem_import_token_module_name(const char *src, const SyntaxElem *tok) {
	if (tok->tag != SE_TOKEN)
		return NULL;
	TokenKind k = tok->as.token.kind;
	if (k != TOK_IDENT && k != TOK_STRING)
		return NULL;
	size_t off = tok->as.token.offset, len = tok->as.token.length;
	if (k == TOK_STRING && len >= 2) { /* strip the quotes */
		off += 1;
		len -= 2;
	}
	char *s = sem_txt_dup((SynText){src + off, len});
	if (k == TOK_STRING) {
		char *slash = strrchr(s, '/');
		char *base = slash ? slash + 1 : s;
		size_t bl = strlen(base);
		if (bl > 6 && strcmp(base + bl - 6, ".arche") == 0)
			base[bl - 6] = '\0';
		if (base != s)
			memmove(s, base, strlen(base) + 1);
	}
	return s;
}

/* A file may carry at most one region of each kind — one `#module`, one `#file`, one `#foreign`, one
 * `#import`. The forms are unions (`#import io` and `#import { … }` are both the `#import` kind), so a
 * second occurrence of any kind is rejected; collect into the single region instead. Scans a file's
 * top-level syntax tree children and emits E0121 on each redundant region. Diagnostics route through the
 * pass-wide ctx (NULL on the AST-only unit-test path, which then skips — mirrors the qualify pass). */
static void sem_check_one_region_per_file(const SyntaxNode *root, const char *src) {
	if (!g_sem_qualify_ctx || !root)
		return;
	int seen_module = 0, seen_file = 0, seen_foreign = 0, seen_import = 0;
	int core_off = semantic_print_line_offset(); /* prepended-core lines; its own regions don't count */
	for (int i = 0; i < root->child_count; i++) {
		if (root->children[i].tag != SE_NODE)
			continue;
		const SyntaxNode *cn = root->children[i].as.node;
		if (core_off > 0 && sem_node_loc(cn).line <= core_off)
			continue; /* core (the prelude) is prepended text — its `#foreign`/region is not the user's */
		const char *name = NULL;
		int *seen = NULL;
		if (cn->kind == SN_REGION) {
			SyntaxView rv = {cn, src};
			if (sv_has_token(rv, TOK_HASH_FOREIGN)) {
				name = "#foreign";
				seen = &seen_foreign;
			} else if (sv_has_token(rv, TOK_HASH_FILE)) {
				name = "#file";
				seen = &seen_file;
			} else {
				name = "#module";
				seen = &seen_module;
			}
		} else if (cn->kind == SN_USE_DECL) {
			name = "#import";
			seen = &seen_import;
		} else {
			continue;
		}
		if (*seen)
			sem_emit_duplicate_region(g_sem_qualify_ctx, sem_node_loc(cn), name);
		*seen = 1;
	}
}

/* Inline module `mod_name`'s decls into `prog` (prefixed so intra-module refs resolve), record its
 * exported names in acc for the `mod.name → mod_name` qualify pass, then RECURSIVELY inline the
 * module's own `#import`s — so a module may use qualified access to a transitively-imported module
 * (`csv` calling `parse.atof`). Dedup via the acc set makes import cycles safe. */
static UnitInterface *sem_iface_find(SemanticContext *ctx, const char *unit_name) {
	for (int i = 0; i < ctx->interface_count; i++)
		if (strcmp(ctx->interfaces[i]->unit_name, unit_name) == 0)
			return ctx->interfaces[i];
	return NULL;
}
static UnitInterface *sem_iface_add(SemanticContext *ctx, const char *unit_name) {
	if (ctx->interface_count == ctx->interface_cap) {
		ctx->interface_cap = ctx->interface_cap ? ctx->interface_cap * 2 : 8;
		ctx->interfaces = realloc(ctx->interfaces, (size_t)ctx->interface_cap * sizeof(UnitInterface *));
	}
	UnitInterface *u = calloc(1, sizeof(UnitInterface));
	u->unit_id = ctx->interface_count + 1; /* >0; 0 is reserved for the entry/root unit */
	u->unit_name = sem_dupz(unit_name);
	ctx->interfaces[ctx->interface_count++] = u;
	return u;
}

static void sem_inline_module(SemanticContext *ctx, const char *mod_name) {
	if (sem_iface_find(ctx, mod_name))
		return; /* already inlined (direct or via another transitive path) — registry makes it cycle-safe */
	/* Register the unit up front (cycle-safe: a transitive re-import finds it) and stamp every decl
	 * with this unit id. Rolled back below if the module turns out to have no files. */
	UnitInterface *iface = sem_iface_add(ctx, mod_name);
	int uid = iface->unit_id;
	int first = ctx->decl_count;
	int found = 0;
	char **full = NULL;
	int fulln = 0, fullcap = 0;
	char **expset = NULL;
	int expn = 0, expcap = 0;
	/* A module is a DEVICE if ANY of its files is a `.ds.arche` datasheet. A device's impl (`.arche`)
	 * is behavior-only — decls collected from it are tagged so the rule-3 sweep can reject type/
	 * archetype/allocation definitions there. */
	int module_is_device = 0;
	for (int m = 0; m < g_sem_module_count; m++)
		if (strcmp(g_sem_modules[m].name, mod_name) == 0 && sem_is_datasheet_file(g_sem_modules[m].filename))
			module_is_device = 1;
	for (int m = 0; m < g_sem_module_count; m++) {
		if (strcmp(g_sem_modules[m].name, mod_name) != 0)
			continue;
		found = 1;
		const SyntaxNode *mr = g_sem_modules[m].root;
		const char *msrc = g_sem_modules[m].src;
		sem_check_one_region_per_file(mr, msrc);
		int ds = sem_is_datasheet_file(g_sem_modules[m].filename); /* `.ds.arche` → decls stay global */
		int exported = 1;                                          /* band resets per file */
		int file_local = 0;                                        /* sticky once a `#file` banner is seen */
		int file_first = ctx->decl_count;                          /* this file's decl range, for the #file rename */
		char **fileset = NULL;                                     /* this file's `#file` decl names */
		int filesetn = 0, filesetcap = 0;
		for (int j = 0; j < mr->child_count; j++) {
			if (mr->children[j].tag != SE_NODE)
				continue;
			const SyntaxNode *cn = mr->children[j].as.node;
			SyntaxNodeKind mk = cn->kind;
			if (mk == SN_REGION) {
				SyntaxView rv = {cn, msrc};
				int is_block = sv_has_token(rv, TOK_LBRACE);
				int is_foreign = sv_has_token(rv, TOK_HASH_FOREIGN);
				int is_file = sv_has_token(rv, TOK_HASH_FILE);
				if (is_block) {
					int child_exp = is_foreign ? exported : 0;
					int child_fl = is_file ? 1 : file_local;
					for (int c = 0; c < cn->child_count; c++) {
						if (cn->children[c].tag != SE_NODE)
							continue;
						if (!sem_is_collectible_decl(cn->children[c].as.node->kind))
							continue;
						sem_add_module_decl(ctx, cn->children[c].as.node, msrc, mod_name, &full, &fulln, &fullcap,
						                    &expset, &expn, &expcap, child_exp, ds, module_is_device, child_fl,
						                    &fileset, &filesetn, &filesetcap, uid, g_sem_modules[m].origin);
					}
				} else if (!is_foreign) {
					exported = 0;
					if (is_file)
						file_local = 1; /* `#file` banner → rest of this file is file-local */
				}
				continue;
			}
			if (!sem_is_collectible_decl(mk))
				continue;
			sem_add_module_decl(ctx, cn, msrc, mod_name, &full, &fulln, &fullcap, &expset, &expn, &expcap, exported, ds,
			                    module_is_device, file_local, &fileset, &filesetn, &filesetcap, uid,
			                    g_sem_modules[m].origin);
		}
		/* Rename this file's `#file` decls (+ their intra-file references) to a file-unique identity
		 * `<mod>.__f<m>.<name>` so a sibling file's bare reference to the same name does NOT bind to
		 * them — `#file` = visible only within its own file. (`m` is unique per file.) */
		if (filesetn > 0) {
			char fprefix[300];
			snprintf(fprefix, sizeof(fprefix), "%s.__f%d", mod_name, m);
			for (int dd = file_first; dd < ctx->decl_count; dd++)
				sem_record_rnop(ctx->decls[dd]->node.node, fprefix, fileset, filesetn);
			for (int x = 0; x < filesetn; x++)
				free(fileset[x]);
		}
		free(fileset);
	}
	if (!found) {
		free(full);
		free(expset);
		/* roll back the speculatively-registered interface (module had no files) */
		free(iface->unit_name);
		free(iface);
		ctx->interface_count--;
		return;
	}
	/* Scope resolution: rename this module's pure-Arche decls + their intra-module references to the
	 * qualified identity `<mod>.<name>` (foreign decls keep their C-symbol name). `full` is the set
	 * of this module's pure-Arche names, so a bare reference inside the module binds to its own
	 * member. */
	for (int dd = first; dd < ctx->decl_count; dd++)
		sem_record_rnop(ctx->decls[dd]->node.node, mod_name, full, fulln);
	for (int x = 0; x < fulln; x++)
		free(full[x]);
	free(full);
	iface->exports = expset; /* transfer ownership; iface was registered up front */
	iface->export_count = expn;
	/* Transitive: inline this module's own `#import`s (registry entry exists → cycle-safe). */
	for (int m = 0; m < g_sem_module_count; m++) {
		if (strcmp(g_sem_modules[m].name, mod_name) != 0)
			continue;
		const SyntaxNode *mr = g_sem_modules[m].root;
		const char *msrc = g_sem_modules[m].src;
		for (int j = 0; j < mr->child_count; j++) {
			if (mr->children[j].tag != SE_NODE || mr->children[j].as.node->kind != SN_USE_DECL)
				continue;
			const SyntaxNode *un = mr->children[j].as.node;
			for (int t = 0; t < un->child_count; t++) {
				char *sub = sem_import_token_module_name(msrc, &un->children[t]);
				if (!sub)
					continue;
				sem_inline_module(ctx, sub);
				free(sub);
			}
		}
	}
}

/* Validate the program's `@default(<kind>, <category>, <policy>)` directives (run after the decl table
 * is collected, so policies — incl. core's — are visible). Each names a core/user policy by name and
 * targets one (effect-kind, op-category) cell; rules: the category must be recognized; `pool` is
 * proc-only and a `func` cell cannot name `abort` (a func stays total); the policy must exist in that
 * category; and at most one directive per cell (E0128). */
static void sem_check_default_directives(SemanticContext *ctx, const SyntaxNode *root, const char *src) {
	int seen[2][4] = {{0, 0, 0, 0}, {0, 0, 0, 0}}; /* [effect_kind 0=proc/1=func][category 1/2/3] */
	for (int i = 0; i < root->child_count; i++) {
		if (root->children[i].tag != SE_NODE)
			continue;
		const SyntaxNode *n = root->children[i].as.node;
		if (n->kind != SN_DEFAULT_DECL)
			continue;
		SourceLoc loc = sem_node_loc(n);
		int effect = 0, cat = 0, seenarg = 0;
		char *policy = NULL;
		for (int c = 0; c < n->child_count && seenarg < 3; c++) {
			if (n->children[c].tag != SE_TOKEN)
				continue;
			TokenKind tk = n->children[c].as.token.kind;
			uint32_t off = n->children[c].as.token.offset, len = n->children[c].as.token.length;
			if (tk == TOK_AT || tk == TOK_LPAREN || tk == TOK_RPAREN || tk == TOK_COMMA)
				continue;
			if (seenarg == 0 && tk == TOK_IDENT && len == 7 && memcmp(src + off, "default", 7) == 0)
				continue;
			if (seenarg == 0)
				effect = (tk == TOK_FUNC) ? 1 : 0;
			else if (seenarg == 1)
				cat = (len == 6 && memcmp(src + off, "bounds", 6) == 0)   ? 1
				      : (len == 4 && memcmp(src + off, "pool", 4) == 0)   ? 2
				      : (len == 6 && memcmp(src + off, "divide", 6) == 0) ? 3
				                                                          : 0;
			else
				policy = sem_txt_dup((SynText){src + off, len});
			seenarg++;
		}
		const char *kindname = effect == 1 ? "func" : "proc";
		const char *catname = cat == 1 ? "bounds" : cat == 2 ? "pool" : cat == 3 ? "divide" : "?";
		if (cat == 0) {
			sem_emit_default_invalid(ctx, loc, "unknown category — use bounds, divide, or pool");
		} else if (effect == 1 && cat == 2) {
			sem_emit_default_invalid(ctx, loc, "pool is proc-only — an insert is an action, not a func value");
		} else if (effect == 1 && policy && strcmp(policy, "abort") == 0) {
			sem_emit_default_invalid(ctx, loc, "a func must stay total — it cannot default to `abort`");
		} else if (policy && !find_policy_sig_cat(ctx, policy, (PolicyCategory)cat)) {
			DeclSummary *any = find_policy_sig(ctx, policy);
			if (!any)
				sem_emit_policy_unknown(ctx, loc, policy);
			else
				sem_emit_policy_wrong_category(ctx, loc, policy, policy_cat_name((PolicyCategory)cat),
				                               policy_cat_name(any->policy_category));
		} else if (seen[effect][cat]) {
			sem_emit_duplicate_default(ctx, loc, kindname, catname);
		} else {
			seen[effect][cat] = 1;
		}
		free(policy);
	}
}

/* Collect the resolved DeclSummary table from the main-file syntax tree plus all registered module
 * syntax trees, inlining + name-prefixing modules exactly as main.c's resolve_uses does. Summaries
 * are built directly from the tree (bare names) into ctx->decls; the loader records each module's
 * rename ops (build_decl_table applies them) and snapshots the export sets for the tree-qualify pass.
 * No abstract AST is built — the syntax tree + SemModel + this table are the single source of truth. */
static void sem_collect_decls(SemanticContext *ctx, const SyntaxNode *root, const char *src) {
	SyntaxView r = sv_root(root, src);
	/* Deep count so decls nested in `#foreign { }` / `#module { }` block regions (collected by the
	 * region recursion below) can't overflow the array; over-estimates are harmless. */
	int cap = sv_node_count_deep(r) + 8;
	for (int m = 0; m < g_sem_module_count; m++)
		cap += sv_node_count_deep(sv_root(g_sem_modules[m].root, g_sem_modules[m].src)) + 1;
	ctx->decls = calloc(cap ? (size_t)cap : 1, sizeof(DeclSummary *));
	ctx->decl_count = 0;

	sem_check_one_region_per_file(root, src);

	for (int i = 0; i < root->child_count; i++) {
		if (root->children[i].tag != SE_NODE)
			continue;
		SyntaxNodeKind k = root->children[i].as.node->kind;
		/* A region marker. The banner form contributes no decls here (its following siblings are
		 * collected normally); a `{ ... }` block's child decls are collected inline. In the main
		 * file there is no export band to narrow (that's module-only), so the marker kind is moot. */
		if (k == SN_REGION) {
			const SyntaxNode *rn = root->children[i].as.node;
			if (sv_has_token((SyntaxView){rn, src}, TOK_LBRACE)) {
				for (int c = 0; c < rn->child_count; c++) {
					if (rn->children[c].tag != SE_NODE)
						continue;
					if (!sem_is_collectible_decl(rn->children[c].as.node->kind))
						continue;
					DeclSummary *ad = decl_summary_from_node(ctx, (SyntaxView){rn->children[c].as.node, src});
					if (ad)
						ctx->decls[ctx->decl_count++] = ad;
				}
			}
			continue;
		}
		if (k < SN_WORLD_DECL || k > SN_USE_DECL)
			continue;
		SyntaxView dv = {root->children[i].as.node, src};

		if (k == SN_DEFAULT_DECL)
			continue; /* a program-default directive: validated in sem_check_default_directives */

		if (k == SN_USE_DECL) {
			/* One element per import: IDENT = device by name, STRING = module by path. Inline each —
			 * the helper recurses into the module's own transitive imports + dedups. */
			const SyntaxNode *un = dv.node;
			for (int t = 0; t < un->child_count; t++) {
				char *mod_name = sem_import_token_module_name(src, &un->children[t]);
				if (!mod_name)
					continue;
				sem_inline_module(ctx, mod_name);
				free(mod_name);
			}
			continue;
		}

		DeclSummary *ad = decl_summary_from_node(ctx, dv);
		if (ad)
			ctx->decls[ctx->decl_count++] = ad;
	}

	/* Editor-only: inline the open document's own device module (its sibling datasheet) even though the
	 * root has no `#import` for it. A device's datasheet decls stay flat/global, so the open impl file's
	 * bare type references (`fd`, …) resolve — matching how the file behaves when the device is imported
	 * whole. sem_inline_module dedups, so a real import of the same name above is harmless. */
	if (g_sem_extra_inline)
		sem_inline_module(ctx, g_sem_extra_inline);

	/* Scope resolution snapshot: the tree-qualify pass in build_decl_table binds every `mod.member`
	 * reference/call to its member's qualified identity (looked up by literal name in the module's
	 * export set, no prefix stripping), records it in the callee_name/ref_name channels, and emits a
	 * precise `module has no member` diagnostic for an unknown member — straight from the SN tree. */
	sem_snapshot_exports(ctx);
	/* Module exports are reachable ONLY via qualified access (handled by the tree-qualify pass); a bare
	 * `name` does NOT resolve to a module export. The persisted interfaces are owned by ctx (freed at
	 * teardown), so nothing transient to release here. */

	/* Now that all policies (incl. core's) are in the decl table, validate the `@default` directives. */
	sem_check_default_directives(ctx, root, src);
}

/* W0013 unused_function (Rust `dead_code`): warn on a top-level func/proc that is never reachable
 * from a root. Roots = `main`, externs (C-ABI surface), module/`#file` decls (qualified name carries
 * a `.` — not the user's main file, an imported API surface), `_`-prefixed names, and `@allow`'d
 * decls. arche has no `pub` keyword for main-file decls, so "exported" collapses into "is a module
 * decl". The lint runs on the resolved DeclTable + SemModel call/ref channels — no new tree walk of
 * the kind analysis already did, just a reachability sweep. */

/* === Reachability (id-keyed) ===
 * Edges are read from the SemModel's DefId channels (callee_def / ref_def), populated by
 * sem_bind_defs after name resolution. Identity is a DefId, never a per-edge string scan. */

/* A callable decl: the kinds reachability tracks as call/seed targets and roots. */
static int decl_is_callable(DeclKind k) {
	return k == DECL_FUNC || k == DECL_PROC || k == DECL_FUNC_GROUP || k == DECL_SYS;
}

/* A reference TARGET: any top-level decl a name/call can resolve to. Broader than callable so the
 * id-keyed channels bind references to data decls (static/const/enum) too — the basis for the
 * unused-static/const (W0014) and unused-enum (W0015) lints. */
static int decl_is_ref_target(DeclKind k) {
	return decl_is_callable(k) || k == DECL_STATIC || k == DECL_CONST || k == DECL_ENUM;
}

/* name -> decl index over reference-target decls, built once (sorted + bsearch). E0031 keeps func/proc
 * names unique, and names don't collide across the other kinds in a valid program. */
typedef struct {
	const char *name;
	int idx;
} NameIdx;
static int nameidx_cmp(const void *a, const void *b) {
	return strcmp(((const NameIdx *)a)->name, ((const NameIdx *)b)->name);
}
static int build_name_index(SemanticContext *ctx, NameIdx **out) {
	NameIdx *arr = malloc((size_t)(ctx->decl_count > 0 ? ctx->decl_count : 1) * sizeof(NameIdx));
	int n = 0;
	for (int i = 0; i < ctx->decl_count; i++) {
		DeclSummary *d = ctx->decls[i];
		if (decl_is_ref_target(d->kind) && d->name)
			arr[n++] = (NameIdx){d->name, i};
	}
	qsort(arr, (size_t)n, sizeof(NameIdx), nameidx_cmp);
	*out = arr;
	return n;
}
static int name_index_lookup(const NameIdx *arr, int n, const char *name) {
	if (!name)
		return -1;
	NameIdx key = {name, -1};
	const NameIdx *hit = bsearch(&key, arr, (size_t)n, sizeof(NameIdx), nameidx_cmp);
	return hit ? hit->idx : -1;
}

/* Populate the DefId channels from the resolved name channels (their id-keyed twins). Runs once after
 * build_decl_table, when every decl carries its final renamed name. A name with no callable match
 * (extern/libc/builtin) stays DEFID_NONE. */
static void sem_bind_defs(SemanticContext *ctx) {
	if (!ctx->model)
		return;
	NameIdx *arr = NULL;
	int n = build_name_index(ctx, &arr);
	int cap = sem_model_cap(ctx->model);
	for (uint32_t id = 0; id < (uint32_t)cap; id++) {
		const char *cn = sem_model_callee_name(ctx->model, id);
		if (cn) {
			int idx = name_index_lookup(arr, n, cn);
			if (idx >= 0)
				sem_model_set_callee_def(ctx->model, id, defid_make(ctx->decls[idx]->unit, idx));
		}
		const char *rn = sem_model_ref_name(ctx->model, id);
		if (rn) {
			int idx = name_index_lookup(arr, n, rn);
			if (idx >= 0)
				sem_model_set_ref_def(ctx->model, id, defid_make(ctx->decls[idx]->unit, idx));
		}
	}
	free(arr);
}

/* Resolve a name to a callable decl index, or -1. Cold path only — overload-group members are names,
 * not nodes; the hot call/ref edges use the DefId channels directly. */
static int dead_decl_index_for(SemanticContext *ctx, const char *name) {
	if (!name)
		return -1;
	for (int i = 0; i < ctx->decl_count; i++) {
		DeclSummary *d = ctx->decls[i];
		if (!decl_is_callable(d->kind))
			continue;
		if (d->name && strcmp(d->name, name) == 0)
			return i;
	}
	return -1;
}

/* Mark decl `idx` reachable; a func GROUP marks every member too (overload resolution is not re-run
 * here, so marking the whole group is the conservative no-false-positive choice). New marks enqueue. */
static void dead_mark(SemanticContext *ctx, int idx, char *reachable, int *work, int *work_n) {
	if (idx < 0 || reachable[idx])
		return;
	reachable[idx] = 1;
	work[(*work_n)++] = idx;
	DeclSummary *d = ctx->decls[idx];
	if (d->kind == DECL_FUNC_GROUP)
		for (int m = 0; m < d->member_count; m++)
			dead_mark(ctx, dead_decl_index_for(ctx, d->member_names[m]), reachable, work, work_n);
}

static void dead_mark_def(SemanticContext *ctx, DefId d, char *reachable, int *work, int *work_n) {
	if (!defid_is_none(d))
		dead_mark(ctx, d.index, reachable, work, work_n); /* one unit today; d.unit == 0 */
}

/* Walk a body subtree, marking the DefId target of every call (callee_def) and every bare name
 * reference (ref_def — a func/proc/sys handed by name, e.g. a compile-time callback arg). */
static void dead_walk(SemanticContext *ctx, const SyntaxNode *n, const char *src, char *reachable, int *work,
                      int *work_n) {
	if (!n)
		return;
	SyntaxView v = (SyntaxView){n, src};
	SyntaxNodeKind k = sv_kind(v);
	if (k == SN_CALL_EXPR)
		dead_mark_def(ctx, sem_model_callee_def(ctx->model, sv_id(v)), reachable, work, work_n);
	else if (k == SN_NAME_EXPR)
		dead_mark_def(ctx, sem_model_ref_def(ctx->model, sv_id(v)), reachable, work, work_n);
	for (int i = 0; i < n->child_count; i++)
		if (n->children[i].tag == SE_NODE)
			dead_walk(ctx, n->children[i].as.node, src, reachable, work, work_n);
}

/* A func/proc/sys that is always kept regardless of reachability — visibility/origin-aware (Go
 * capitals / Rust `pub`), replacing the old `.`-in-name heuristic. Entry-unit decls are NOT roots here;
 * the crate-kind gate in sem_check_dead_code decides whether to flag them (binary = closed world). */
static int dead_is_root(const DeclSummary *d) {
	if (!d->name)
		return 1;
	if (strcmp(d->name, "main") == 0)
		return 1;
	if (d->is_extern && d->visibility == VIS_EXPORTED)
		return 1;   /* EXPORTED C-ABI surface — may be linked/called from outside the unit. A private
		             * (`#module`/`#file`) extern has no arche body either, but it is a device-internal
		             * import: dead exactly when no in-unit caller (a wrapper) reaches it. Visibility
		             * decides root-ness uniformly — the exported surface IS the root set, foreign or not.
		             * (Stdlib/core externs still fall through to the dependency rule below and stay kept.) */
	if (d->is_drop) /* opaque destructor — invoked by the compiler's RAII path, never syntactically */
		return 1;
	if (d->is_policy) /* a failure policy — invoked by the compiler at fallible op sites via `!name` */
		return 1;
	if (d->name[0] == '_') /* Rust `_`-prefix silence */
		return 1;
	if (d->origin == DECL_ORIGIN_STDLIB || d->origin == DECL_ORIGIN_CORE)
		return 1; /* a dependency — Go/Rust don't dead-code-lint deps */
	if (d->origin == DECL_ORIGIN_USER_MODULE && d->visibility == VIS_EXPORTED)
		return 1; /* public library API — an importer may call it */
	return 0;
}

/* The source path of a decl's owning module (NULL for entry-unit decls or if unknown). Used to give
 * cross-file dead-code diagnostics a file the user can open — a W-code on an imported module's private
 * decl otherwise prints a bare module-local line number with no file. Maps decl.unit → UnitInterface
 * (unit_name) → the registered module's filename. */
static const char *sem_decl_module_path(SemanticContext *ctx, const DeclSummary *d) {
	if (d->origin == DECL_ORIGIN_ENTRY)
		return NULL;
	for (int i = 0; i < ctx->interface_count; i++) {
		if (ctx->interfaces[i]->unit_id != d->unit)
			continue;
		const char *modname = ctx->interfaces[i]->unit_name;
		for (int m = 0; m < g_sem_module_count; m++)
			if (g_sem_modules[m].name && modname && strcmp(g_sem_modules[m].name, modname) == 0)
				return g_sem_modules[m].filename;
		return NULL;
	}
	return NULL;
}

static void sem_check_dead_code(SemanticContext *ctx) {
	if (!ctx->model || ctx->decl_count <= 0)
		return;
	char *reachable = calloc((size_t)ctx->decl_count, 1);
	int *work = malloc((size_t)ctx->decl_count * sizeof(int));
	if (!reachable || !work) {
		free(reachable);
		free(work);
		return;
	}
	/* The core prelude is prepended (lines 1..core_off of the combined stream); its decls — e.g. the
	 * compiler-emitted `streq` — are not user-owned and must never be flagged. core_off = 0 when core
	 * is NOT prepended (compiling core.arche itself), so the guard is inert there. */
	int core_off = semantic_print_line_offset();
	/* Crate-kind by signal, not file position (Go: a `main` proc ⇒ binary). The entry unit is a BINARY
	 * iff it defines `main` — then it is a closed world and any unreachable entry decl is dead. With no
	 * `main` (e.g. the LSP opened a library module on its own), flag NOTHING in the entry unit, so a
	 * library's public API is never reported dead while you type. */
	int entry_is_binary = 0;
	for (int i = 0; i < ctx->decl_count; i++) {
		DeclSummary *d = ctx->decls[i];
		if (d->origin == DECL_ORIGIN_ENTRY && d->name && strcmp(d->name, "main") == 0) {
			entry_is_binary = 1;
			break;
		}
	}
	int work_n = 0;
	/* seed roots. Systems are entry points — invoked by `run`, which records no call edge — so seed
	 * every `sys` and walk its body; a func/proc reachable only from a system is thus kept alive
	 * (systems themselves are never flagged). Other callables seed only when they are roots. */
	for (int i = 0; i < ctx->decl_count; i++) {
		DeclSummary *d = ctx->decls[i];
		if (d->kind == DECL_SYS)
			dead_mark(ctx, i, reachable, work, &work_n);
		else if ((d->kind == DECL_FUNC || d->kind == DECL_PROC || d->kind == DECL_FUNC_GROUP) && dead_is_root(d))
			dead_mark(ctx, i, reachable, work, &work_n);
	}
	/* transitive marking — reachable[] doubles as the visited set, so self/mutual recursion among
	 * live funcs terminates and a cycle unreachable from any root is never seeded (both warn). */
	while (work_n > 0) {
		DeclSummary *d = ctx->decls[work[--work_n]];
		if (d->body_node.node)
			dead_walk(ctx, d->body_node.node, d->body_node.src, reachable, work, &work_n);
	}
	/* emit for every unreachable, non-root func/proc */
	for (int i = 0; i < ctx->decl_count; i++) {
		DeclSummary *d = ctx->decls[i];
		if (d->kind != DECL_FUNC && d->kind != DECL_PROC)
			continue;
		if (reachable[i] || dead_is_root(d))
			continue;
		/* The prepended-core guard is an ENTRY-unit concern: core.arche occupies lines 1..core_off of the
		 * combined entry source. Module decls carry their OWN file's (small) line numbers, so this guard
		 * must NOT apply to them — gate it on entry origin. */
		if (d->origin == DECL_ORIGIN_ENTRY && core_off > 0 && d->loc.line <= core_off)
			continue; /* prepended core prelude — not user code */
		if (d->origin == DECL_ORIGIN_ENTRY && !entry_is_binary)
			continue; /* library checked standalone — its surface is API, not dead code */
		/* the pass runs after analyze cleared active_allow_slugs, so re-arm @allow suppression for
		 * this decl around the emit (sem_emit_v matches the `unused_function` slug). `dead_code` is
		 * honored as a Rust-compatible alias. */
		ctx->active_allow_slugs = d->allow_slugs;
		ctx->active_allow_slug_count = d->allow_slug_count;
		if (!sem_diag_slug_suppressed(ctx, "dead_code"))
			sem_emit_lint_unused_function(ctx, d->loc, d->name, sem_decl_module_path(ctx, d));
		ctx->active_allow_slugs = NULL;
		ctx->active_allow_slug_count = 0;
	}

	/* W0014 unused_static_const / W0015 unused_enum — data decls. "Used" = referenced ANYWHERE (any
	 * incoming id-keyed ref), which the qualify pass records for uses in code, initializers, and sizes.
	 * Same visibility/origin/crate-kind roots as functions; a static referenced only by dead code is
	 * conservatively treated as used (no false positive). */
	{
		char *referenced = calloc((size_t)ctx->decl_count, 1);
		int cap = sem_model_cap(ctx->model);
		for (int id = 0; id < cap; id++) {
			DefId cd = sem_model_callee_def(ctx->model, (uint32_t)id);
			if (!defid_is_none(cd) && cd.index < ctx->decl_count)
				referenced[cd.index] = 1;
			DefId rd = sem_model_ref_def(ctx->model, (uint32_t)id);
			if (!defid_is_none(rd) && rd.index < ctx->decl_count)
				referenced[rd.index] = 1;
		}
		for (int i = 0; i < ctx->decl_count; i++) {
			DeclSummary *d = ctx->decls[i];
			int is_static_data = d->kind == DECL_STATIC &&
			                     (d->static_kind == STATIC_KIND_SCALAR || d->static_kind == STATIC_KIND_ARRAY) &&
			                     !d->is_requirement;
			int is_enum = d->kind == DECL_ENUM;
			if ((!is_static_data && !is_enum) || referenced[i])
				continue;
			if (!d->name || d->name[0] == '_')
				continue;
			if (d->origin == DECL_ORIGIN_STDLIB || d->origin == DECL_ORIGIN_CORE)
				continue; /* dependency — not linted */
			if (d->origin == DECL_ORIGIN_USER_MODULE && d->visibility == VIS_EXPORTED)
				continue; /* public module API */
			if (d->origin == DECL_ORIGIN_ENTRY && core_off > 0 && d->loc.line <= core_off)
				continue; /* prepended core prelude */
			if (d->origin == DECL_ORIGIN_ENTRY && !entry_is_binary)
				continue; /* library checked standalone */
			ctx->active_allow_slugs = d->allow_slugs;
			ctx->active_allow_slug_count = d->allow_slug_count;
			if (!sem_diag_slug_suppressed(ctx, "dead_code")) {
				const char *mp = sem_decl_module_path(ctx, d);
				if (is_enum)
					sem_emit_lint_unused_enum(ctx, d->loc, d->name, mp);
				else
					sem_emit_lint_unused_static_const(
					    ctx, d->loc, d->static_kind == STATIC_KIND_SCALAR ? "static" : "static array", d->name, mp);
			}
			ctx->active_allow_slugs = NULL;
			ctx->active_allow_slug_count = 0;
		}
		free(referenced);
	}
	free(reachable);
	free(work);
}

/* ========== PUBLIC API ========== */

/* The shared analysis core: all passes that read the (already-prepared) AstProgram tree.
 * Run by semantic_analyze_cst on an AstProgram reconstructed from the syntax tree. */
/* Runs purely on the resolved DeclTable (ctx->decls) + SyntaxView + SemModel — the reconstructed
 * AstProgram has already been consumed by build_decl_table and freed before this is called. */
static void analyze_program_core(SemanticContext *ctx) {
	/* pass 0: collect constants and nominal type aliases. A `name : [meta] : value` decl is a
	 * value const when its RHS denotes a value, or a type alias when its RHS denotes a type.
	 * Unambiguous forms (type-form RHS, literal RHS) are classified directly; a bare-name RHS is
	 * deferred and resolved by a small fixpoint below — so `tau :: pi` is a value const (not an
	 * alias), and chains / forward refs resolve regardless of declaration order. */
	/* `@implements` opaque unification/rename: build the driver→requirement map up front so datasheet
	 * registration can skip renamed device types and the post-fill rename can rewrite device params. */
	sem_build_impl_map(ctx);
	int deferred_cap = ctx->decl_count > 0 ? ctx->decl_count : 1;
	const char **deferred_name = malloc(sizeof(char *) * deferred_cap);
	const char **deferred_rhs = malloc(sizeof(char *) * deferred_cap);
	int *deferred_value_ctx = malloc(sizeof(int) * deferred_cap);
	int *deferred_transparent = malloc(sizeof(int) * deferred_cap);
	int *deferred_done = calloc(deferred_cap, sizeof(int));
	int *deferred_datasheet = calloc(deferred_cap, sizeof(int)); /* shared datasheet vocab dedups on agreement */
	SourceLoc *deferred_loc = malloc(sizeof(SourceLoc) * deferred_cap);
	int deferred_count = 0;

	for (int i = 0; i < ctx->decl_count; i++) {
		DeclSummary *c = ctx->decls[i];
		if (c->kind != DECL_CONST)
			continue;
		int dsheet = c->is_datasheet; /* shared datasheet vocabulary dedups on agreement */
		SourceLoc cloc = c->const_value_loc;

		/* Type-form RHS (`name : type : T`, or the tuple `name :: (x,y:..)`): a nominal alias. */
		if (c->const_type_value_id != TYID_UNKNOWN) {
			TypeId tv = c->const_type_value_id;
			TyKind tvk = tyid_kind(ctx->ty_arena, tv);
			if (tyid_is_callable(ctx->ty_arena, tv)) {
				/* `name :: proc()(…)` / `func` — a named structural callable type. */
				register_callable_type_alias(ctx, c->name, tv);
				continue;
			}
			if (tvk == TYK_TUPLE) {
				for (int f = 0; f < tyid_tuple_count(ctx->ty_arena, tv); f++) {
					TypeId ft = tyid_tuple_field_type(ctx->ty_arena, tv, f);
					const char *fbacking = sem_tyid_name(ctx, ft);
					if (!fbacking) {
						sem_emit_tuple_field_not_simple(ctx, cloc);
						continue;
					}
					const char *fn = tyid_tuple_field_name(ctx->ty_arena, tv, f);
					size_t L = strlen(c->name) + 1 + strlen(fn) + 1;
					char *aname = malloc(L);
					snprintf(aname, L, "%s_%s", c->name, fn);
					register_type_alias(ctx, aname, fbacking, cloc, dsheet); /* aname leaks like the old path */
				}
			} else {
				const char *backing = sem_tyid_name(ctx, tv);
				if (!backing)
					sem_emit_alias_backing_invalid(ctx, cloc);
				else
					register_type_alias_tiered(ctx, c->name, backing, c->is_transparent, cloc, dsheet);
			}
			continue;
		}

		/* Literal RHS: a value const. Its type is the explicit declared type if present, else the
		 * literal's own type (`3.14`→float, `42`→int, `"s"`→char_array) — so the const resolves to its
		 * real type. A string const (`name :: "linux"`) is a `[N]char` whose reference lowers back to
		 * the string literal (see lower.c), so all the existing `char[]`/`.length`/decay machinery applies. */
		if (c->const_value_kind == EXPR_LITERAL || c->const_value_kind == EXPR_STRING) {
			const char *vt = NULL;
			TyKind ddk = tyid_kind(ctx->ty_arena, c->const_decl_type_id);
			if (ddk == TYK_NOMINAL || ddk == TYK_PRIM)
				vt = sem_tyid_name(ctx, c->const_decl_type_id);
			if (!vt)
				vt = resolve_expression_type(ctx, c->const_value);
			register_value_const(ctx, c->name, c->const_value_lexeme, vt, cloc);
			continue;
		}

		/* Bare-name RHS: ambiguous (type or value) — defer for the fixpoint. An explicit concrete
		 * declared type (`PI : float : x`) forces value context. */
		if (c->const_value_kind == EXPR_NAME) {
			deferred_name[deferred_count] = c->name;
			deferred_rhs[deferred_count] = c->const_value_name;
			deferred_value_ctx[deferred_count] = (c->const_decl_type_id != TYID_UNKNOWN);
			deferred_transparent[deferred_count] = c->is_transparent;
			deferred_datasheet[deferred_count] = dsheet;
			deferred_loc[deferred_count] = cloc;
			deferred_count++;
			continue;
		}

		/* CTFE: a const whose RHS is constant arithmetic or a pure `func` of constants folds to an
		 * integer value (a `func` is a value). Register the folded result so it resolves like a
		 * literal const everywhere downstream. */
		{
			int folded;
			if (semantic_try_const_int(ctx, c->const_value, &folded)) {
				char tmp[32];
				snprintf(tmp, sizeof(tmp), "%d", folded);
				register_value_const(ctx, c->name, sem_dupz(tmp), "int", cloc);
				continue;
			}
		}

		sem_emit_const_rhs_invalid(ctx, cloc);
	}

	/* Fixpoint: classify each deferred bare-name const by what its RHS denotes. */
	int progress = 1;
	while (progress) {
		progress = 0;
		for (int d = 0; d < deferred_count; d++) {
			if (deferred_done[d])
				continue;
			const char *r = deferred_rhs[d];
			if (name_denotes_type(ctx, r)) {
				if (deferred_value_ctx[d]) {
					sem_emit_const_value_is_type(ctx, deferred_loc[d], deferred_name[d], r);
				} else {
					register_type_alias_tiered(ctx, deferred_name[d], r, deferred_transparent[d], deferred_loc[d],
					                           deferred_datasheet[d]);
				}
				deferred_done[d] = 1;
				progress = 1;
				continue;
			}
			const char *lex = value_const_lexeme(ctx, r);
			if (lex) {
				/* `tau :: pi` — tau takes pi's value AND pi's type. */
				register_value_const(ctx, deferred_name[d], lex, value_const_type(ctx, r), deferred_loc[d]);
				deferred_done[d] = 1;
				progress = 1;
				continue;
			}
			/* `handler :: some_proc` — a callable alias (Stage A of proc/func types). */
			if (prog_has_callable(ctx, r)) {
				register_callable_alias(ctx, deferred_name[d], r);
				deferred_done[d] = 1;
				progress = 1;
			}
		}
	}
	/* Unresolved leftovers: a value context names something undefined; otherwise presume an
	 * intended type alias with an unknown backing (reported by the validation loop below). */
	for (int d = 0; d < deferred_count; d++) {
		if (deferred_done[d])
			continue;
		if (deferred_value_ctx[d]) {
			sem_emit_unknown_const_value(ctx, deferred_loc[d], deferred_rhs[d], deferred_name[d]);
		} else {
			register_type_alias_tiered(ctx, deferred_name[d], deferred_rhs[d], deferred_transparent[d], deferred_loc[d],
			                           deferred_datasheet[d]);
		}
	}
	free(deferred_name);
	free(deferred_rhs);
	free(deferred_value_ctx);
	free(deferred_transparent);
	free(deferred_done);
	free(deferred_datasheet);
	free(deferred_loc);

	/* Enums: register the type (as an int-backed alias so `m: method` resolves) and its variants
	 * (so `method.get` and bare variants in `match` resolve to their int values). Compile-time only. */
	for (int i = 0; i < ctx->decl_count; i++) {
		DeclSummary *e = ctx->decls[i];
		if (e->kind != DECL_ENUM)
			continue;
		register_enum_entries(ctx, e);
		/* An enum is a DISTINCT (tier-2) int-backed type: an enum value is usable AS its int backing
		 * (so `match`/comparison and `printf("%d", …)` work), but a raw int is NOT usable as the enum —
		 * you must name a case (`color.red`) or convert explicitly (`color(0)`). */
		register_type_alias_tiered(ctx, sem_dupz(e->name), "int", 0, e->loc, e->is_datasheet);
	}

	/* Inline component definitions: `arche Foo { hp :: int, … }` mints the nominal type `hp`
	 * (≡ a top-level `hp :: int`) and includes it as a component — so the component is a real
	 * type, not a bare field label. A bare reference (`arche Foo { hp }`) has field type == its
	 * own name and is skipped here. Array/tuple components are the column's own type, not a
	 * registerable alias. Redefinition must agree, exactly as for top-level aliases. */
	for (int i = 0; i < ctx->decl_count; i++) {
		DeclSummary *a = ctx->decls[i];
		if (a->kind != DECL_ARCHETYPE)
			continue;
		int ds = a->is_datasheet; /* datasheet shapes share vocabulary — dedup on agreement */
		for (int f = 0; f < a->field_count; f++) {
			FieldSummary *fd = &a->fields[f];
			if (!sv_present(fd->type_node) || !fd->name)
				continue;
			/* This runs in pass 0 (before the TypeId fill), so read the raw written type name straight
			 * from the node (an array/tuple/handle component is column-only — no backing). */
			const char *backing = NULL;
			if (sv_kind(fd->type_node) == SN_TYPE_REF) {
				char *raw = sem_type_ref_name(fd->type_node);
				if (strcmp(raw, "opaque") == 0)
					backing = "opaque";
				else if (strcmp(raw, "archetype") != 0 && strcmp(raw, "type") != 0 && strcmp(raw, fd->name) != 0)
					backing = sem_own_str(ctx, sem_dupz(raw)); /* registry borrows; pool frees at teardown */
				free(raw);
			}
			if (!backing)
				continue; /* bare reference, or an array/tuple component (column-only) */
			int done = 0;
			for (int j = 0; j < ctx->type_alias_count; j++) {
				if (strcmp(ctx->type_alias_names[j], fd->name) == 0) {
					/* Define-once (E0045 on agreement, E0010 on a different backing): an inline
					 * component that re-defines a name already minted elsewhere — by another
					 * archetype or a top-level alias — is a redefinition, not a shared reference.
					 * EXCEPTION: a datasheet shape that AGREES is shared vocabulary (two devices using
					 * the same shape) — dedup silently. */
					if (strcmp(ctx->type_alias_backings[j], backing) != 0)
						sem_emit_type_alias_redefined(ctx, fd->loc, fd->name);
					else if (!ds)
						sem_emit_component_redefined(ctx, fd->loc, fd->name);
					done = 1;
					break;
				}
			}
			if (done)
				continue;
			/* A component is a tier-2 subtype (distinct nominal, usable as backing) — same as a
			 * top-level `name :: T`. Route through the registry helper so the tier array stays in sync. */
			register_type_alias_tiered(ctx, fd->name, backing, 0, fd->loc, ds);
		}
	}

	/* Validate each alias resolves (through any chain) to a real backing type. */
	for (int i = 0; i < ctx->type_alias_count; i++) {
		const char *b = resolve_type_alias(ctx, ctx->type_alias_names[i]);
		if (!is_primitive_type_name(b) && strcmp(b, "opaque") != 0) {
			sem_emit_type_alias_unknown_backing(ctx, ctx->type_alias_locs[i], ctx->type_alias_names[i], b);
		}
	}

	/* Typed value consts: the explicit declared type now constrains the literal (no longer inert). */
	for (int i = 0; i < ctx->decl_count; i++) {
		if (ctx->decls[i]->kind == DECL_CONST)
			check_const_literal_type(ctx, ctx->decls[i]);
	}

	/* global scope: holds module-level variables (static arrays, etc.) */
	push_scope(ctx);

	/* Intern each decl's type nodes into parallel TypeIds NOW — pass 0 has registered every top-level
	 * type alias (decl signatures only reference those; local body aliases come later and don't appear
	 * in signatures), so the ids are final. Filling here (not post-everything) lets the archetype +
	 * body passes read the STORED ids directly. */
	for (int i = 0; i < ctx->decl_count; i++)
		if (ctx->decls[i])
			sem_fill_decl_type_ids(ctx, ctx->decls[i]);

	/* `@implements`: rewrite device-impl param/return TypeIds from each requirement to the driver's
	 * type, so tycheck sees the unified/renamed identity (file1/file2 → file; dconf.handle → dh). The
	 * map is no longer needed afterward. */
	sem_apply_impl_renames(ctx);
	sem_clear_impl_map();

	/* Tuple-group field expansion: now that field type_ids are interned, rewrite any field whose type
	 * names a tuple-group const to that tuple's id (the column flattens to `field_<member>`). */
	sem_expand_tuple_groups_table(ctx);

	/* pass 1: collect all archetypes */
	for (int i = 0; i < ctx->decl_count; i++) {
		if (ctx->decls[i]->kind == DECL_ARCHETYPE) {
			analyze_decl(ctx, ctx->decls[i]);
		}
	}

	/* pre-pass: hoist every top-level mutable binding (buffer or scalar) — register its name into
	 * the global scope so forward references resolve regardless of declaration order (top-level
	 * names hoist, like procs/funcs/types), and register it for func-purity so a func touching a
	 * mutable global is rejected wherever the global sits. */
	for (int i = 0; i < ctx->decl_count; i++) {
		DeclSummary *d = ctx->decls[i];
		if (d->kind != DECL_STATIC)
			continue;
		if (d->static_kind == STATIC_KIND_ARRAY || d->static_kind == STATIC_KIND_SCALAR) {
			register_static_name(ctx, d->name);
			add_variable(ctx, d->name, d->static_type_id);
		}
	}

	/* pre-pass: register every proc/func name so a body may reference any function regardless of
	 * declaration order — module scope is order-independent (a module proc may call one defined
	 * later in the same file/folder). register_func dedups, so the later per-decl registration in
	 * analyze_{proc,func}_decl is a no-op. Func GROUPS are excluded: analyze_func_group_decl checks
	 * find_known_func to detect a clashing name, so pre-registering the group would self-trip E0031. */
	for (int i = 0; i < ctx->decl_count; i++) {
		DeclSummary *d = ctx->decls[i];
		if (d->kind == DECL_PROC || d->kind == DECL_FUNC)
			register_func(ctx, d->name);
	}

	/* pre-pass: register every `@drop` proc's destructor (opaque type -> proc) BEFORE any body
	 * is analyzed, so pop_scope's auto-drop decision sees the registry regardless of decl order. */
	for (int i = 0; i < ctx->decl_count; i++) {
		DeclSummary *d = ctx->decls[i];
		if (d->is_drop && d->kind == DECL_PROC)
			analyze_drop_decl(ctx, d, d->drop_type);
	}

	/* pass 2: analyze other declarations */
	for (int i = 0; i < ctx->decl_count; i++) {
		if (ctx->decls[i]->kind != DECL_ARCHETYPE && ctx->decls[i]->kind != DECL_CONST) {
			analyze_decl(ctx, ctx->decls[i]);
		}
	}

	/* pass 2.5: device datasheet storage requirements vs the driver's pools (min met, none missing). */
	sem_check_storage_requirements(ctx);
	sem_check_device_impl_decls(ctx);
	sem_check_datasheet_decls(ctx);
	sem_check_raw_pool_lint(ctx);  /* W0017: advise handles for unprovable pool-column indexing */
	sem_check_policies(ctx);       /* E0097-99/E0124/W0018: failure-policy validation at index/slice ops */
	sem_check_policy_cycles(ctx);  /* E0214: a policy that applies itself would inline forever */
	sem_check_forbid_allow(ctx);   /* E0127: --forbid-allow rejects @allow(...) in user code */
	sem_record_binding_types(ctx); /* editor inlay: type-of(RHS) per top-level decl, keyed by decl node */

	/* Unique-name rule (Rust/Go): a func and a proc — or two of either — may not share a name. The
	 * two kinds are still distinct (keyword, `-> T` vs out-list, call form), but the name namespace
	 * is single, so a clash is E0031. Scans real decls only (not builtins). */
	for (int i = 0; i < ctx->decl_count; i++) {
		DeclSummary *di = ctx->decls[i];
		if ((di->kind != DECL_FUNC && di->kind != DECL_PROC) || di->is_policy)
			continue; /* a policy is a separate namespace (invoked via `!name`, never called) */
		const char *ni = di->name;
		if (!ni)
			continue;
		const char *ki = (di->kind == DECL_FUNC) ? "func" : "proc";
		for (int j = 0; j < i; j++) {
			DeclSummary *dj = ctx->decls[j];
			if ((dj->kind != DECL_FUNC && dj->kind != DECL_PROC) || dj->is_policy)
				continue;
			/* A stdlib symbol is module-qualified (`os.write`) — it never duplicates a global/core/user
			 * name (core `write`, a user `write`) in real resolution; the two only appear flat together in
			 * the all-stdlib codegen-test harness. Skip any pair involving a stdlib decl. */
			if (di->origin == DECL_ORIGIN_STDLIB || dj->origin == DECL_ORIGIN_STDLIB)
				continue;
			if (dj->name && strcmp(dj->name, ni) == 0) {
				sem_emit_duplicate_decl(ctx, di->loc, ki, ni);
				break;
			}
		}
	}

	/* pass 4: tycheck — bidirectional type checker. Phase A encodes one rule
	 * (return-value types). Diagnostics flow through the same registry as
	 * everything else. Fail-open: unencoded shapes synth to TYID_UNKNOWN. */
	tycheck_run(ctx);

	/* pass 5: dead-code lint (W0013) — reachability sweep over the resolved DeclTable. */
	sem_check_dead_code(ctx);
}

/* Allocate + zero-initialize a SemanticContext and register builtins. Shared by both
 * entry points. */
static SemanticContext *make_context(void) {
	SemanticContext *ctx = malloc(sizeof(SemanticContext));
	ctx->archetypes = NULL;
	ctx->archetype_count = 0;
	ctx->decls = NULL;
	ctx->decl_count = 0;
	ctx->interfaces = NULL;
	ctx->interface_count = 0;
	ctx->interface_cap = 0;
	ctx->ty_arena = ty_arena_new(); /* must exist before any decl/expr typing */
	ctx->aliases = NULL;
	ctx->alias_count = 0;
	ctx->known_funcs = NULL;
	ctx->known_func_count = 0;
	ctx->callable_alias_names = NULL;
	ctx->callable_alias_targets = NULL;
	ctx->callable_alias_count = 0;
	ctx->ctype_alias_names = NULL;
	ctx->ctype_alias_ids = NULL;
	ctx->ctype_alias_count = 0;
	ctx->enum_type_names = NULL;
	ctx->enum_type_count = 0;
	ctx->enum_var_enum = NULL;
	ctx->enum_var_name = NULL;
	ctx->enum_var_value = NULL;
	ctx->enum_var_count = 0;
	ctx->static_names = NULL;
	ctx->static_name_count = 0;
	ctx->groups = NULL;
	ctx->group_count = 0;
	ctx->const_names = NULL;
	ctx->const_values = NULL;
	ctx->const_value_types = NULL;
	ctx->const_locs = NULL;
	ctx->const_count = 0;
	ctx->type_alias_names = NULL;
	ctx->type_alias_backings = NULL;
	ctx->type_alias_transparent = NULL;
	ctx->type_alias_locs = NULL;
	ctx->type_alias_count = 0;
	ctx->owned_strs = NULL;
	ctx->owned_str_count = 0;
	ctx->owned_str_cap = 0;
	ctx->drop_type_names = NULL;
	ctx->drop_proc_names = NULL;
	ctx->drop_locs = NULL;
	ctx->drop_count = 0;
	ctx->scopes = NULL;
	ctx->scope_count = 0;
	ctx->error_count = 0;
	ctx->current_sys_archetype = NULL;
	ctx->current_proc = NULL;
	ctx->current_func = NULL;
	ctx->in_sys = 0;
	ctx->in_body = 0;
	ctx->stmt_call_ok = 0;
	ctx->proc_call_stmt_ok = 0; /* only the out-list statement sets this; cleared in every EXPR_CALL */
	ctx->model = sem_model_new();
	ctx->hints = sem_hints_new();
	ctx->diags = NULL;
	ctx->diag_count = 0;
	ctx->diag_cap = 0;
	ctx->active_allow_slugs = NULL;
	ctx->active_allow_slug_count = 0;
	ctx->analyzing_call_arg = 0;

	register_func(ctx, "write");
	register_func(ctx, "insert");
	register_func(ctx, "delete");
	register_func(ctx, "dealloc");
	return ctx;
}

/* Take ownership of an analysis-allocated string handed by pointer to the type-alias registry, so it
 * lives for the context and is freed at teardown. Returns `s` for inline use. NULL-safe. */
static char *sem_own_str(SemanticContext *ctx, char *s) {
	if (!s)
		return NULL;
	if (ctx->owned_str_count >= ctx->owned_str_cap) {
		ctx->owned_str_cap = ctx->owned_str_cap ? ctx->owned_str_cap * 2 : 16;
		ctx->owned_strs = realloc(ctx->owned_strs, (size_t)ctx->owned_str_cap * sizeof(char *));
	}
	ctx->owned_strs[ctx->owned_str_count++] = s;
	return s;
}

/* The pattern's VALUE token of a match arm — the literal, the `_`, or the case of a qualified
 * `Enum.case` (the LAST identifier before the arm `:`). NULL if none. */
static const SyntaxElem *match_arm_pattern_tok(const SyntaxNode *arm) {
	const SyntaxElem *last = NULL;
	for (int c = 0; c < arm->child_count; c++) {
		if (arm->children[c].tag != SE_TOKEN)
			continue;
		TokenKind k = arm->children[c].as.token.kind;
		if (k == TOK_COLON)
			break; /* the pattern ends at the arm `:` */
		if (k == TOK_IDENT || k == TOK_NUMBER || k == TOK_STRING || k == TOK_CHAR_LIT)
			last = &arm->children[c]; /* keep last → the case in a qualified `Enum.case` */
	}
	return last;
}

/* The enum a bare variant name belongs to, or NULL. */
static const char *enum_of_variant(SemanticContext *ctx, const char *var, int len) {
	for (int i = 0; i < ctx->enum_var_count; i++)
		if ((int)strlen(ctx->enum_var_name[i]) == len && memcmp(ctx->enum_var_name[i], var, len) == 0)
			return ctx->enum_var_enum[i];
	return NULL;
}

/* Exhaustiveness check for one `match`: an enum match must cover every variant (or have `_`);
 * an open-key (int/string/char) match must have a `_`. */
static void check_one_match(SemanticContext *ctx, const SyntaxNode *m, const char *src) {
	int has_wild = 0;
	const char *en = NULL;
	for (int i = 0; i < m->child_count; i++) {
		if (m->children[i].tag != SE_NODE || m->children[i].as.node->kind != SN_MATCH_ARM)
			continue;
		const SyntaxElem *pt = match_arm_pattern_tok(m->children[i].as.node);
		if (!pt)
			continue;
		const char *txt = src + pt->as.token.offset;
		int len = (int)pt->as.token.length;
		if (pt->as.token.kind == TOK_IDENT) {
			if (len == 1 && txt[0] == '_')
				has_wild = 1;
			else {
				const char *e = enum_of_variant(ctx, txt, len);
				if (e && !en)
					en = e;
			}
		}
	}
	SourceLoc loc = sem_direct_token_loc((SyntaxNode *)m, TOK_MATCH);
	if (en) {
		/* An enum match is closed: `_` is rejected (it would silently absorb variants added later —
		 * the caller should name an explicit case, e.g. `not_found`). Every variant must be covered. */
		if (has_wild)
			sem_emit_wildcard_in_enum_match(ctx, loc);
		for (int v = 0; v < ctx->enum_var_count; v++) {
			if (strcmp(ctx->enum_var_enum[v], en) != 0)
				continue;
			int covered = 0;
			for (int i = 0; i < m->child_count && !covered; i++) {
				if (m->children[i].tag != SE_NODE || m->children[i].as.node->kind != SN_MATCH_ARM)
					continue;
				const SyntaxElem *pt = match_arm_pattern_tok(m->children[i].as.node);
				if (pt && pt->as.token.kind == TOK_IDENT &&
				    (int)strlen(ctx->enum_var_name[v]) == (int)pt->as.token.length &&
				    memcmp(ctx->enum_var_name[v], src + pt->as.token.offset, pt->as.token.length) == 0)
					covered = 1;
			}
			if (!covered)
				sem_emit_non_exhaustive_match(ctx, loc, ctx->enum_var_name[v]);
		}
	} else if (!has_wild) {
		sem_emit_non_exhaustive_match(ctx, loc, "_");
	}
}

static void walk_matches(SemanticContext *ctx, const SyntaxNode *n, const char *src) {
	if (!n)
		return;
	if (n->kind == SN_MATCH_STMT)
		check_one_match(ctx, n, src);
	for (int i = 0; i < n->child_count; i++)
		if (n->children[i].tag == SE_NODE)
			walk_matches(ctx, n->children[i].as.node, src);
}

/* The node whose direct children are a proc/func/sys body's statements. In the unified grammar a
 * `name :: proc(){…}` decl node carries the body under its SN_PROC_EXPR/SN_FUNC_EXPR/SN_SYS_EXPR
 * value-form child; the legacy SN_*_DECL form holds the statements directly. */
static SyntaxView sem_decl_body_node(SyntaxView dn) {
	if (!sv_present(dn))
		return dn;
	for (int i = 0; i < dn.node->child_count; i++)
		if (dn.node->children[i].tag == SE_NODE) {
			SyntaxNodeKind k = dn.node->children[i].as.node->kind;
			if (k >= SN_BIND_STMT && k <= SN_MATCH_STMT)
				return dn; /* statements are direct children */
		}
	for (int i = 0; i < dn.node->child_count; i++)
		if (dn.node->children[i].tag == SE_NODE) {
			SyntaxNodeKind k = dn.node->children[i].as.node->kind;
			if (k == SN_PROC_EXPR || k == SN_FUNC_EXPR || k == SN_POLICY_EXPR || k == SN_SYS_EXPR)
				return (SyntaxView){dn.node->children[i].as.node, dn.src};
		}
	return dn;
}

/* AST-kill step A: build the resolved decl-signature table from the fully resolved owned_prog
 * (post rename/qualify/tuple-expand). Additive — readers migrate onto it incrementally. */
/* Tree-direct twin of sem_param_summary (which copies a cst_build_param'd AST Parameter): a
 * ParamSummary straight from an SN_PARAM node. Type pooled on ctx; name owned. */
static ParamSummary sem_param_summary_node(SyntaxView p) {
	ParamSummary s = {0};
	s.name = sem_cv_dup(sv_child(p, SN_PARAM_NAME));
	s.type_node = sem_type_at(p, 0);
	s.is_own = sv_has_token(p, TOK_OWN);
	s.loc = sem_node_loc(p.node); /* param-name site, for LSP goto + diagnostics */
	return s;
}

/* AST-kill step C: build a DeclSummary DIRECTLY from a syntax-tree decl node — no AstProgram Decl.
 * Names/types are BARE (the loader's rename is applied to the summary afterwards, reusing
 * sem_rename_typeref/sem_maybe_rename). Returns NULL for a decl kind not yet ported here
 * (archetype/enum/static/const/world/use), so the caller keeps the AST-built summary during the
 * transition. Mirrors build_proc_from/build_func_from/build_func_group_from + cst_build_param. */
/* Map a value-expression node kind to the legacy ExpressionType (so const_value_kind matches what
 * decl_summary_from read from the AST `c->value->type`). */
static int sem_expr_kind_of(SyntaxNodeKind k) {
	switch (k) {
	case SN_LITERAL_EXPR:
		return EXPR_LITERAL;
	case SN_NAME_EXPR:
		return EXPR_NAME;
	case SN_FIELD_EXPR:
		return EXPR_FIELD;
	case SN_INDEX_EXPR:
		return EXPR_INDEX;
	case SN_SLICE_EXPR:
		return EXPR_SLICE;
	case SN_BINARY_EXPR:
		return EXPR_BINARY;
	case SN_UNARY_EXPR:
		return EXPR_UNARY;
	case SN_CALL_EXPR:
		return EXPR_CALL;
	case SN_ARRAY_LIT_EXPR:
		return EXPR_ARRAY_LITERAL;
	case SN_STRING_EXPR:
		return EXPR_STRING;
	default:
		return -1;
	}
}

/* AST-kill step C: build a DECL_CONST DeclSummary directly from the tree (mirrors the const
 * classification in cst_build_decl_inner + decl_summary_from's DECL_CONST case). Covers the
 * callable-type alias (`name :: proc()()`), the tuple group (`name (a,b) :: T`), the typed value /
 * type alias (`name : T : v` / `name : type : T`), and the plain value (`name :: v`). */
static DeclSummary *decl_summary_const_node(SemanticContext *ctx, SyntaxView dv) {
	DeclSummary *ds = calloc(1, sizeof(DeclSummary));
	ds->kind = DECL_CONST;
	ds->static_kind = -1;
	ds->const_value_kind = -1;
	ds->loc = sem_node_loc(dv.node);
	ds->node = dv;
	ds->body_node = sem_decl_body_node(dv);
	syntax_extract_allow_slugs(dv, &ds->allow_slugs, &ds->allow_slug_count);
	ds->is_drop = syntax_has_drop_decorator(dv);
	ds->drop_type = syntax_drop_type(dv);
	ds->is_transparent = syntax_const_alias_marked(dv);
	int decorated = sem_decl_is_decorated(dv.node);
	SynText bn = decorated ? sem_binding_name(dv) : sv_token(dv, TOK_IDENT);
	ds->name = bn.ptr ? sem_txt_dup(bn) : NULL;
	ds->const_value_loc = ds->loc;

	/* Aggregate value const → an immutable static-array global (a flat `[total]elem`, arche's
	 * row-stride model). Three forms: 1-D scalar `{1,2,3}`, N×M matrix `{ {…},{…} }`, and N strings
	 * `{ "a","bb" }` (an N×W char matrix). The element type is the innermost scalar. */
	{
		SyntaxView valv = sem_node_at_expr(dv, 0);
		if (sv_present(valv) && sv_kind(valv) == SN_ARRAY_LIT_EXPR) {
			SyntaxView first = {0};
			int ec = 0;
			for (int i = 0; i < valv.node->child_count; i++)
				if (valv.node->children[i].tag == SE_NODE) {
					if (ec == 0)
						first = (SyntaxView){valv.node->children[i].as.node, valv.src};
					ec++;
				}
			int total = ec, stride = 1;
			const char *et = "int";
			SyntaxView elem_for_infer = first;
			if (sv_present(first) && sv_kind(first) == SN_ARRAY_LIT_EXPR) {
				int inner = 0;
				SyntaxView ifirst = {0};
				for (int i = 0; i < first.node->child_count; i++)
					if (first.node->children[i].tag == SE_NODE) {
						if (inner == 0)
							ifirst = (SyntaxView){first.node->children[i].as.node, first.src};
						inner++;
					}
				total = ec * inner;
				stride = inner;
				elem_for_infer = ifirst;
			} else if (sv_present(first) && sv_kind(first) == SN_STRING_EXPR) {
				et = "char";
				/* total stays the row count: row-indexing `S[i]` bounds-checks against it; the flat
				 * `[rows*width]char` global size is computed in lowering/codegen. The row width is the
				 * widest string (escapes count as one char). */
				int maxw = 0;
				for (int i = 0; i < valv.node->child_count; i++)
					if (valv.node->children[i].tag == SE_NODE &&
					    valv.node->children[i].as.node->kind == SN_STRING_EXPR) {
						SyntaxView sv = {valv.node->children[i].as.node, valv.src};
						char *raw = sem_cv_dup_first_token(sv); /* the quoted literal */
						int w = 0;
						if (raw)
							for (int k = 1; raw[k] && raw[k] != '"'; k++) {
								if (raw[k] == '\\' && raw[k + 1])
									k++;
								w++;
							}
						free(raw);
						if (w > maxw)
							maxw = w;
					}
				stride = maxw > 0 ? maxw : 1;
			}
			ds->kind = DECL_STATIC;
			ds->static_kind = STATIC_KIND_ARRAY;
			ds->static_is_const = 1; /* an aggregate value const — immutable */
			ds->static_init = valv;
			ds->static_has_init = 1;
			ds->static_size = total;
			ds->static_row_stride = stride;
			SyntaxView declty = sem_type_at(dv, 0);
			TypeId declid = sv_present(declty) ? sem_intern_view(ctx, declty) : TYID_UNKNOWN;
			TyKind dk = tyid_kind(ctx->ty_arena, declid);
			if (dk == TYK_ARRAY) {
				/* Walk the declared array dims down to the innermost SCALAR element. A nested matrix
				 * type `[N][M]T` flattens to `N*M` slots (product of dims) with the innermost dim as
				 * the row stride; a plain `[N]T` keeps size N and stride 1. (A count mismatch vs the
				 * initializer is caught in analysis.) */
				TypeId cur = declid;
				long prod = 1;
				int dims = 0, innermost = 1;
				while (tyid_kind(ctx->ty_arena, cur) == TYK_ARRAY) {
					int dn = tyid_array_len(ctx->ty_arena, cur);
					if (dn > 0) {
						prod *= dn;
						innermost = dn;
					}
					cur = tyid_elem(ctx->ty_arena, cur);
					dims++;
				}
				ds->static_type_id = cur; /* innermost scalar */
				if (prod > 0)
					ds->static_size = (int)prod;
				if (dims >= 2)
					ds->static_row_stride = innermost;
			} else if (dk == TYK_SLICE) {
				ds->static_type_id = tyid_elem(ctx->ty_arena, declid); /* `[]T` slot keeps inferred size */
			} else {
				if (strcmp(et, "char") != 0) {
					const char *r = sv_present(elem_for_infer) ? resolve_expression_type(ctx, elem_for_infer) : "int";
					et = (!r || strcmp(r, "i32") == 0) ? "int" : r;
				}
				ds->static_type_id = sem_tyid_of_name(ctx, et);
			}
			return ds;
		}
	}

	SyntaxView form = sem_rhs_form(dv);
	if (sv_present(form) && (sv_kind(form) == SN_TYPE_PROC || sv_kind(form) == SN_TYPE_FUNC)) {
		ds->const_type_value_id = sem_intern_view(ctx, form); /* callable-type alias */
		ds->const_value_loc = sem_node_loc(form.node);
		return ds;
	}
	if (!decorated && sv_has_token(dv, TOK_LPAREN)) {
		/* tuple group: type_value = a tuple of the parenthesized suffix names, each typed by the shared
		 * type after `::`. */
		SyntaxView memberty = sem_type_at(dv, 0);
		TypeId shared_id = sv_present(memberty) ? sem_intern_view(ctx, memberty) : TYID_UNKNOWN;
		int in_paren = 0, n = 0;
		for (int i = 0; i < dv.node->child_count; i++)
			if (dv.node->children[i].tag == SE_TOKEN) {
				TokenKind tk = dv.node->children[i].as.token.kind;
				if (tk == TOK_LPAREN)
					in_paren = 1;
				else if (tk == TOK_RPAREN)
					in_paren = 0;
				else if (tk == TOK_IDENT && in_paren)
					n++;
			}
		char **names = calloc(n > 0 ? n : 1, sizeof(char *));
		TypeId *types = calloc(n > 0 ? n : 1, sizeof(TypeId));
		int fi = 0;
		in_paren = 0;
		for (int i = 0; i < dv.node->child_count && fi < n; i++)
			if (dv.node->children[i].tag == SE_TOKEN) {
				TokenKind tk = dv.node->children[i].as.token.kind;
				if (tk == TOK_LPAREN)
					in_paren = 1;
				else if (tk == TOK_RPAREN)
					in_paren = 0;
				else if (tk == TOK_IDENT && in_paren) {
					SynText t = {dv.src + dv.node->children[i].as.token.offset, dv.node->children[i].as.token.length};
					names[fi] = sem_txt_dup(t);
					types[fi] = shared_id;
					fi++;
				}
			}
		ds->const_type_value_id = tyid_of_tuple(ctx->ty_arena, (const char *const *)names, types, fi);
		for (int i = 0; i < fi; i++)
			free(names[i]);
		free(names);
		free(types);
		return ds;
	}
	SyntaxView t0 = sem_type_at(dv, 0), t1 = sem_type_at(dv, 1);
	SynText t0name = sv_present(t0) ? sv_token(t0, TOK_IDENT) : (SynText){NULL, 0};
	int t0_is_meta = t0name.ptr && t0name.len == 4 && memcmp(t0name.ptr, "type", 4) == 0;
	if (t0_is_meta && sv_present(t1)) {
		ds->const_decl_type_id = sem_intern_view(ctx, t0); /* TYPE_TYPE */
		ds->const_type_value_id = sem_intern_view(ctx, t1);
		ds->const_value_loc = sem_node_loc(t1.node);
	} else {
		if (sv_present(t0)) {
			ds->const_decl_type_id = sem_intern_view(ctx, t0);
		}
		SyntaxView val = sem_node_at_expr(dv, 0);
		if (sv_present(val)) {
			ds->const_value = val;
			ds->const_value_kind = sem_expr_kind_of(sv_kind(val));
			ds->const_value_loc = sem_node_loc(val.node);
			if (sv_kind(val) == SN_LITERAL_EXPR || sv_kind(val) == SN_STRING_EXPR)
				ds->const_value_lexeme = sem_cv_dup_first_token(val);
			else if (sv_kind(val) == SN_NAME_EXPR)
				ds->const_value_name = sv_name_expr_dup(val);
		}
	}
	return ds;
}

static DeclSummary *decl_summary_from_node(SemanticContext *ctx, SyntaxView dv) {
	if (sv_kind(dv) == SN_WORLD_DECL) {
		DeclSummary *ds = calloc(1, sizeof(DeclSummary));
		ds->kind = DECL_WORLD;
		ds->static_kind = -1;
		ds->loc = sem_node_loc(dv.node);
		ds->node = dv;
		ds->body_node = sem_decl_body_node(dv);
		ds->name = sem_txt_dup(sv_token(dv, TOK_IDENT));
		syntax_extract_allow_slugs(dv, &ds->allow_slugs, &ds->allow_slug_count);
		ds->is_drop = syntax_has_drop_decorator(dv);
		ds->drop_type = syntax_drop_type(dv);
		return ds;
	}
	if (sv_kind(dv) == SN_STATIC_DECL) {
		DeclSummary *ds = calloc(1, sizeof(DeclSummary));
		ds->kind = DECL_STATIC;
		ds->static_pool_count = -1; /* DECL_STATIC default (decl_summary_from); const_value_kind stays 0 */
		ds->loc = sem_node_loc(dv.node);
		ds->node = dv;
		ds->body_node = sem_decl_body_node(dv);
		syntax_extract_allow_slugs(dv, &ds->allow_slugs, &ds->allow_slug_count);
		ds->is_drop = syntax_has_drop_decorator(dv);
		ds->drop_type = syntax_drop_type(dv);
		if (sv_has_token(dv, TOK_LBRACKET)) {
			/* pool `Name[C](N){V}` — archetype name = dotted IDENT head before `[`; field values are
			 * the expr nodes by phase ([cap] (len) {fields}). */
			ds->static_kind = STATIC_KIND_ARCHETYPE;
			/* prefix pool `[C]Name`: the archetype name is the top-level IDENT run AFTER the capacity
			 * `[…]`, collected in the phase walk below (phase 0). */
			char an[256];
			int al = 0;
			int cap = dv.node->child_count + 1;
			ds->static_fields = calloc(cap, sizeof(SyntaxView));
			int phase = 0; /* 1=cap 2=len 3=fields */
			for (int i = 0; i < dv.node->child_count; i++) {
				SyntaxElem *ch = &dv.node->children[i];
				if (ch->tag == SE_TOKEN) {
					TokenKind tk = ch->as.token.kind;
					if (tk == TOK_LBRACKET)
						phase = 1;
					else if (tk == TOK_LPAREN)
						phase = 2;
					else if (tk == TOK_LBRACE)
						phase = 3;
					else if (tk == TOK_RBRACKET || tk == TOK_RPAREN || tk == TOK_RBRACE)
						phase = 0;
					else if (tk == TOK_IDENT && phase == 0) {
						/* archetype name segment (top level, after the capacity `[]`) */
						if (al > 0 && al < (int)sizeof(an) - 1)
							an[al++] = '.';
						for (int k = 0; k < (int)ch->as.token.length && al < (int)sizeof(an) - 1; k++)
							an[al++] = dv.src[ch->as.token.offset + k];
					}
					continue;
				}
				SyntaxNodeKind k = ch->as.node->kind;
				if (k < SN_LITERAL_EXPR || k > SN_PAREN_EXPR)
					continue;
				SyntaxView ev = {ch->as.node, dv.src};
				if (phase == 1) {
					ds->static_fields[0] = ev;
					ds->static_field_count = 1;
					if (sv_kind(ev) == SN_LITERAL_EXPR) {
						char *lx = sem_cv_dup_first_token(ev);
						if (lx)
							ds->static_pool_count = atoi(lx);
						free(lx);
					}
				} else if (phase == 2) {
					ds->static_init_length_present = 1;
					if (sv_kind(ev) == SN_LITERAL_EXPR) {
						char *lx = sem_cv_dup_first_token(ev);
						if (lx)
							ds->static_init_count = atoi(lx);
						free(lx);
					}
				} else if (phase == 3) {
					ds->static_fields[ds->static_field_count++] = ev;
				}
			}
			an[al] = '\0';
			ds->name = sem_dupz(an);
		} else {
			char *aname = sem_txt_dup(sv_token(dv, TOK_IDENT));
			SyntaxView arr_ty = sem_type_at(dv, 0);
			SyntaxView initv = sem_node_at_expr(dv, 0);
			TypeId full_id = sem_intern_view(ctx, arr_ty);
			TyKind fullk = tyid_kind(ctx->ty_arena, full_id);
			int is_array = (fullk == TYK_ARRAY || fullk == TYK_SLICE);
			ds->name = aname;
			if (is_array) {
				ds->static_kind = STATIC_KIND_ARRAY;
				ds->static_type_id = tyid_elem(ctx->ty_arena, full_id);
				for (int i = 0; i < arr_ty.node->child_count; i++)
					if (arr_ty.node->children[i].tag == SE_TOKEN &&
					    arr_ty.node->children[i].as.token.kind == TOK_NUMBER) {
						char buf[32];
						int l = (int)arr_ty.node->children[i].as.token.length;
						if (l > 31)
							l = 31;
						memcpy(buf, arr_ty.src + arr_ty.node->children[i].as.token.offset, l);
						buf[l] = '\0';
						ds->static_size = atoi(buf);
						break;
					}
				ds->static_has_init = sv_present(initv);
			} else {
				ds->static_kind = STATIC_KIND_SCALAR;
				ds->static_type_id = full_id; /* UNKNOWN → inferred below */
				if (ds->static_type_id == TYID_UNKNOWN) {
					int isf = 0;
					if (sv_present(initv) && sv_kind(initv) == SN_LITERAL_EXPR) {
						char *lx = sem_cv_dup_first_token(initv);
						isf = lx && strpbrk(lx, ".eE") != NULL;
						free(lx);
					}
					ds->static_type_id = sem_tyid_of_name(ctx, isf ? "float" : "i32");
				}
				ds->static_init = initv;
			}
		}
		return ds;
	}
	SyntaxView form = sem_rhs_form(dv);
	if (!sv_present(form))
		return decl_summary_const_node(ctx, dv); /* a const binding has no value-form RHS */
	SyntaxNodeKind fk = sv_kind(form);
	DeclKind kind;
	if (fk == SN_PROC_EXPR)
		kind = DECL_PROC;
	else if (fk == SN_FUNC_EXPR || fk == SN_POLICY_EXPR)
		kind = DECL_FUNC; /* a policy is a func for typing/codegen; category handled at op site */
	else if (fk == SN_SYS_EXPR)
		kind = DECL_SYS;
	else if (fk == SN_GROUP_EXPR)
		kind = DECL_FUNC_GROUP;
	else if (fk == SN_ENUM_EXPR)
		kind = DECL_ENUM;
	else if (fk == SN_ARCH_EXPR)
		kind = DECL_ARCHETYPE;
	else
		return decl_summary_const_node(ctx, dv); /* SN_TYPE_PROC/FUNC RHS — a named callable-type alias */
	DeclSummary *ds = calloc(1, sizeof(DeclSummary));
	ds->kind = kind;
	ds->is_policy = (fk == SN_POLICY_EXPR); /* a `policy` form — invoked via `!name`, never called */
	ds->policy_category = ds->is_policy ? syntax_policy_category(dv) : POLICY_CAT_NONE;
	ds->static_kind = -1; /* mirror decl_summary_from: static_kind=-1 for all; pool_count/value_kind stay 0 */
	ds->loc = sem_node_loc(dv.node);
	ds->node = dv;
	ds->body_node = sem_decl_body_node(dv);
	SynText bn = sem_binding_name(dv);
	ds->name = bn.ptr ? sem_txt_dup(bn) : NULL;
	syntax_extract_allow_slugs(dv, &ds->allow_slugs, &ds->allow_slug_count);
	ds->is_drop = syntax_has_drop_decorator(dv);
	ds->drop_type = syntax_drop_type(dv);
	if (kind == DECL_FUNC_GROUP) {
		int nmem = 0;
		for (int i = 0; i < form.node->child_count; i++)
			if (form.node->children[i].tag == SE_TOKEN && form.node->children[i].as.token.kind == TOK_IDENT)
				nmem++;
		ds->member_names = calloc(nmem ? nmem : 1, sizeof(char *));
		for (int i = 0; i < form.node->child_count; i++)
			if (form.node->children[i].tag == SE_TOKEN && form.node->children[i].as.token.kind == TOK_IDENT) {
				SynText t = {form.src + form.node->children[i].as.token.offset, form.node->children[i].as.token.length};
				ds->member_names[ds->member_count++] = sem_txt_dup(t);
			}
		return ds;
	}
	if (kind == DECL_ENUM) {
		int nv = sv_count(form, SN_ENUM_VARIANT);
		ds->enum_variant_names = calloc(nv ? nv : 1, sizeof(char *));
		ds->enum_variant_values = calloc(nv ? nv : 1, sizeof(long));
		long next = 0;
		for (int i = 0; i < nv; i++) {
			SyntaxView ev = sv_child_at(form, SN_ENUM_VARIANT, i);
			long val = next;
			for (int c = 0; c < ev.node->child_count; c++)
				if (ev.node->children[c].tag == SE_TOKEN && ev.node->children[c].as.token.kind == TOK_NUMBER) {
					char buf[32];
					int l = (int)ev.node->children[c].as.token.length;
					if (l > 31)
						l = 31;
					memcpy(buf, ev.src + ev.node->children[c].as.token.offset, l);
					buf[l] = '\0';
					val = atol(buf);
					break;
				}
			ds->enum_variant_names[ds->enum_variant_count] = sem_txt_dup(sv_token(ev, TOK_IDENT));
			ds->enum_variant_values[ds->enum_variant_count++] = val;
			next = val + 1;
		}
		return ds;
	}
	if (kind == DECL_ARCHETYPE) {
		int nf = sv_count(form, SN_FIELD_NAME);
		ds->fields = calloc(nf > 0 ? nf : 1, sizeof(FieldSummary));
		for (int i = 0; i < form.node->child_count; i++) {
			if (form.node->children[i].tag != SE_NODE || form.node->children[i].as.node->kind != SN_FIELD_NAME)
				continue;
			SyntaxView fn = {form.node->children[i].as.node, form.src};
			SyntaxView ty = {NULL, form.src};
			int meta_explicit = 0;
			for (int k = i + 1; k < form.node->child_count; k++) {
				if (form.node->children[k].tag == SE_TOKEN) {
					if (form.node->children[k].as.token.kind == TOK_IDENT &&
					    form.node->children[k].as.token.length == 4 &&
					    strncmp(form.src + form.node->children[k].as.token.offset, "type", 4) == 0)
						meta_explicit = 1;
					continue;
				}
				SyntaxNodeKind kk = form.node->children[k].as.node->kind;
				if (kk == SN_FIELD_NAME)
					break;
				if (kk >= SN_TYPE_REF && kk <= SN_TYPE_FUNC) {
					ty.node = form.node->children[k].as.node;
					break;
				}
			}
			char *fname = sem_cv_dup(fn);
			ds->fields[ds->field_count].name = fname;
			ds->fields[ds->field_count].type_node = ty; /* absent → the field NAME is its type */
			ds->fields[ds->field_count].kind = FIELD_COLUMN;
			ds->fields[ds->field_count].meta_explicit = meta_explicit;
			ds->fields[ds->field_count].loc = sem_node_loc(fn.node);
			ds->field_count++;
		}
		return ds;
	}
	int np = sv_count(form, SN_PARAM);
	ds->param_count = np;
	ds->params = calloc(np ? np : 1, sizeof(ParamSummary));
	for (int i = 0; i < np; i++)
		ds->params[i] = sem_param_summary_node(sv_child_at(form, SN_PARAM, i));
	if (kind == DECL_PROC) {
		ds->is_extern = !sv_has_token(form, TOK_LBRACE);
		ds->is_variadic = sv_has_token(form, TOK_DOTDOTDOT);
		ds->allow_pure_proc = sv_has_token(form, TOK_AT);
		int no = sv_count(form, SN_OUT_PARAM);
		ds->out_param_count = no;
		ds->out_params = calloc(no ? no : 1, sizeof(ParamSummary));
		for (int i = 0; i < no; i++)
			ds->out_params[i] = sem_param_summary_node(sv_child_at(form, SN_OUT_PARAM, i));
	} else if (kind == DECL_FUNC) {
		int nt = sv_type_count_sem(form);
		ds->return_type_count = nt;
		ds->return_type_nodes = calloc(nt ? (size_t)nt : 1, sizeof(SyntaxView));
		for (int i = 0; i < nt; i++)
			ds->return_type_nodes[i] = sem_type_at(form, i);
	}
	return ds;
}

static void free_decl_summary(DeclSummary *ds); /* fwd */

/* AST-kill step C: the DeclTable twin of sem_expand_tuple_groups — if an archetype field's type is a
 * bare name matching a top-level tuple-group const (a `name :: (x,y:T)` whose const_type_value is a
 * TYPE_TUPLE), replace the field type with a pooled copy of that tuple (so the column flattens to
 * `field_<member>`). Runs after the table is built + renamed. */
static void sem_maybe_expand_tuple(SemanticContext *ctx, FieldSummary *fd) {
	/* The field's interned type is a bare nominal naming a tuple-group const → become that tuple.
	 * Runs AFTER sem_fill_decl_type_ids, so fd->type_id and the const's tuple id are both populated. */
	const char *ref = tyid_nominal_name(ctx->ty_arena, fd->type_id);
	if (!ref)
		return;
	for (int i = 0; i < ctx->decl_count; i++) {
		DeclSummary *c = ctx->decls[i];
		if (c->kind != DECL_CONST || !c->name || tyid_kind(ctx->ty_arena, c->const_type_value_id) != TYK_TUPLE)
			continue;
		if (strcmp(c->name, ref) != 0)
			continue;
		fd->type_id = c->const_type_value_id;
		return;
	}
}

static void sem_expand_tuple_groups_table(SemanticContext *ctx) {
	for (int a = 0; a < ctx->decl_count; a++)
		if (ctx->decls[a] && ctx->decls[a]->kind == DECL_ARCHETYPE)
			for (int f = 0; f < ctx->decls[a]->field_count; f++)
				sem_maybe_expand_tuple(ctx, &ctx->decls[a]->fields[f]);
}

/* Channel recording for ONE tree node: resolve its leftmost name (a
 * single `mod.f` via the export sets, else the bare leftmost prefixed by the decl's rename ops) and
 * record callee_name (calls) / ref_name (name/field/index/slice), keyed by node id — the identity
 * the view-driven analysis looks names up under. `declnode` keys the decl's recorded rename ops. */
static void sem_tree_qualify_node(SemanticContext *ctx, SyntaxView v, const SyntaxNode *declnode) {
	if (!ctx->model)
		return;
	SyntaxNodeKind k = sv_kind(v);
	int is_call = (k == SN_CALL_EXPR);
	int is_ref = (k == SN_NAME_EXPR || k == SN_FIELD_EXPR || k == SN_INDEX_EXPR || k == SN_SLICE_EXPR);
	if (!is_call && !is_ref)
		return;
	int has_fields = sv_count(v, SN_FIELD_NAME) >= 1;
	char *base =
	    (is_call && !has_fields) ? sem_cv_dup(sv_child(v, SN_CALLEE_NAME)) : sem_txt_dup(sv_token(v, TOK_IDENT));
	if (!base || !base[0]) {
		free(base);
		return;
	}
	char *resolved = NULL;
	if (has_fields) {
		char *field = sem_cv_dup(sv_child_at(v, SN_FIELD_NAME, 0));
		char mangled[256];
		if (field &&
		    sem_qual_lookup(g_exp_prefix, g_exp_set, g_exp_count, g_exp_n, base, field, mangled, sizeof(mangled)))
			resolved = sem_dupz(mangled);
		else if (field && sem_base_is_module(g_exp_prefix, g_exp_n, base))
			/* `mod.member` where `mod` is a module but `member` is not exported: the precise
			 * "module 'm' has no member 'x'" diagnostic (mirrors the AST sem_qualify_expr). */
			sem_emit_module_no_member(ctx, sem_node_loc(v.node), base, field);
		free(field);
	}
	if (!resolved) {
		sem_apply_rnops(&base, declnode); /* a bare module-internal ref → its prefixed identity */
		resolved = base;
		base = NULL;
	}
	if (is_call)
		sem_model_set_callee_name(ctx->model, sv_id(v), resolved);
	else
		sem_model_set_ref_name(ctx->model, sv_id(v), resolved);
	free(base);
	free(resolved);
}

/* Walk a tree subtree, qualifying every reference/call node under decl `declnode`. */
static void sem_tree_qualify_walk(SemanticContext *ctx, const SyntaxNode *n, const char *src,
                                  const SyntaxNode *declnode) {
	if (!n)
		return;
	sem_tree_qualify_node(ctx, (SyntaxView){n, src}, declnode);
	for (int i = 0; i < n->child_count; i++)
		if (n->children[i].tag == SE_NODE)
			sem_tree_qualify_walk(ctx, n->children[i].as.node, src, declnode);
}

/* Finalize the DeclSummary table that sem_collect_decls populated (bare names): apply each decl's
 * recorded module/file rename ops, record the callee_name/ref_name channels by walking its body +
 * value exprs on the tree (the tree-qualify pass), then expand top-level tuple groups. */
/* Phase 3: intern every type node this DeclSummary carries into a parallel TypeId. Called after rename
 * + tuple-expansion, so the names/shapes are final — the ids match what tycheck computes on the same
 * (renamed) type nodes. */
static void sem_fill_decl_type_ids(SemanticContext *ctx, DeclSummary *ds) {
	for (int p = 0; p < ds->param_count; p++)
		ds->params[p].type_id = sem_intern_view(ctx, ds->params[p].type_node);
	for (int p = 0; p < ds->out_param_count; p++)
		ds->out_params[p].type_id = sem_intern_view(ctx, ds->out_params[p].type_node);
	if (ds->return_type_count > 0) {
		ds->return_type_ids = calloc((size_t)ds->return_type_count, sizeof(TypeId));
		for (int r = 0; r < ds->return_type_count; r++)
			ds->return_type_ids[r] = sem_intern_view(ctx, ds->return_type_nodes[r]);
	}
	for (int f = 0; f < ds->field_count; f++)
		ds->fields[f].type_id = sv_present(ds->fields[f].type_node) ? sem_intern_view(ctx, ds->fields[f].type_node)
		                                                            : sem_tyid_of_name(ctx, ds->fields[f].name);
}

static void build_decl_table(SemanticContext *ctx) {
	for (int i = 0; i < ctx->decl_count; i++) {
		DeclSummary *ds = ctx->decls[i];
		if (!ds)
			continue;
		const SyntaxNode *dn = ds->node.node;
		for (int r = 0; r < g_rnop_count; r++)
			if (g_rnops[r].node == dn)
				sem_rename_decl_summary(ds, g_rnops[r].prefix, g_rnops[r].set, g_rnops[r].count);
		if (ds->body_node.node)
			sem_tree_qualify_walk(ctx, ds->body_node.node, ds->body_node.src, dn);
		if (ds->const_value.node)
			sem_tree_qualify_walk(ctx, ds->const_value.node, ds->const_value.src, dn);
		if (ds->static_init.node)
			sem_tree_qualify_walk(ctx, ds->static_init.node, ds->static_init.src, dn);
		for (int s = 0; s < ds->static_field_count; s++)
			if (ds->static_fields[s].node)
				sem_tree_qualify_walk(ctx, ds->static_fields[s].node, ds->static_fields[s].src, dn);
	}
	sem_free_rnops();
	sem_free_exports();
}

/* Free one DeclSummary (its owned names + arrays). Types are interned TypeIds in the arena, not owned
 * here. Unused fields are NULL/0, so this is correct for every decl kind. */
static void free_decl_summary(DeclSummary *ds) {
	if (!ds)
		return;
	free(ds->name);
	free(ds->drop_type);
	for (int p = 0; p < ds->param_count; p++)
		free(ds->params[p].name);
	free(ds->params);
	for (int p = 0; p < ds->out_param_count; p++)
		free(ds->out_params[p].name);
	free(ds->out_params);
	free(ds->return_type_nodes);
	free(ds->return_type_ids);
	for (int m = 0; m < ds->member_count; m++)
		free(ds->member_names[m]);
	free(ds->member_names);
	for (int f = 0; f < ds->field_count; f++)
		free(ds->fields[f].name);
	free(ds->fields);
	for (int e = 0; e < ds->enum_variant_count; e++)
		free(ds->enum_variant_names[e]);
	free(ds->enum_variant_names);
	free(ds->enum_variant_values);
	free(ds->const_value_lexeme);
	free(ds->const_value_name);
	free(ds->static_fields);
	for (int a = 0; a < ds->allow_slug_count; a++)
		free(ds->allow_slugs[a]);
	free(ds->allow_slugs);
	free(ds);
}

static void free_decl_table(SemanticContext *ctx) {
	for (int i = 0; i < ctx->decl_count; i++)
		free_decl_summary(ctx->decls[i]);
	free(ctx->decls);
	ctx->decls = NULL;
	ctx->decl_count = 0;
	for (int i = 0; i < ctx->interface_count; i++) {
		for (int e = 0; e < ctx->interfaces[i]->export_count; e++)
			free(ctx->interfaces[i]->exports[e]);
		free(ctx->interfaces[i]->exports);
		free(ctx->interfaces[i]->unit_name);
		free(ctx->interfaces[i]);
	}
	free(ctx->interfaces);
	ctx->interfaces = NULL;
	ctx->interface_count = 0;
	ctx->interface_cap = 0;
}

/* DeclTable accessors for out-of-file readers (tycheck). */
int semantic_decl_count(const SemanticContext *ctx) {
	return ctx ? ctx->decl_count : 0;
}
const DeclSummary *semantic_decl_at(const SemanticContext *ctx, int i) {
	return (ctx && i >= 0 && i < ctx->decl_count) ? ctx->decls[i] : NULL;
}
const DeclSummary *semantic_find_callable_sig(const SemanticContext *ctx, const char *name) {
	return find_callable_sig((SemanticContext *)ctx, name);
}
char *semantic_call_callee_name(SemanticContext *ctx, SyntaxView call) {
	const char *resolved = ctx->model ? sem_model_callee_name(ctx->model, sv_id(call)) : NULL;
	if (resolved)
		return sem_dupz(resolved);
	if (sv_count(call, SN_FIELD_NAME) != 0)
		return NULL; /* qualified, non-module field call — unresolved */
	return sem_cv_dup(sv_child(call, SN_CALLEE_NAME));
}

/* The source FILE a decl was parsed from, or NULL when it lives in the entry buffer (the root program,
 * which the analyzer prepends core into — the caller splits core vs user by line). Matched by the decl
 * node's source-buffer pointer against the module registry, so multi-file folder modules and `.ds.arche`
 * datasheets resolve to the exact member file (not just the module name). For LSP goto navigation. */
const char *semantic_decl_src_file(const SemanticContext *ctx, int index) {
	const DeclSummary *d = semantic_decl_at(ctx, index);
	if (!d || !d->node.src)
		return NULL;
	for (int m = 0; m < g_sem_module_count; m++)
		if (g_sem_modules[m].src == d->node.src)
			return g_sem_modules[m].filename;
	return NULL;
}

/* The source file a registered module `name` was loaded from (NULL if not a module). For LSP goto on a
 * module qualifier (`foo` in `foo.bar`) — jump to the module's own file. Folder modules report their
 * first member file. */
const char *semantic_module_file(const char *name) {
	if (!name)
		return NULL;
	for (int m = 0; m < g_sem_module_count; m++)
		if (g_sem_modules[m].name && strcmp(g_sem_modules[m].name, name) == 0)
			return g_sem_modules[m].filename;
	return NULL;
}

/* Index of the first TYPE-defining decl named `name` (archetype / enum / a const that binds a type),
 * else -1. Drives LSP goto-type-definition. Small N → linear scan. */
int semantic_find_type_decl_index(const SemanticContext *ctx, const char *name) {
	if (!ctx || !name)
		return -1;
	for (int i = 0; i < ctx->decl_count; i++) {
		const DeclSummary *d = ctx->decls[i];
		if (!d->name || strcmp(d->name, name) != 0)
			continue;
		if (d->kind == DECL_ARCHETYPE || d->kind == DECL_ENUM || d->kind == DECL_CONST)
			return i;
	}
	return -1;
}

/* Index of the `@drop` destructor proc registered for opaque type `type_name` (its implementation),
 * else -1. Drives LSP goto-implementation for opaque types. */
int semantic_drop_proc_decl_index(const SemanticContext *ctx, const char *type_name) {
	const char *proc = drop_dtor_for((SemanticContext *)ctx, type_name);
	if (!proc)
		return -1;
	for (int i = 0; i < ctx->decl_count; i++) {
		const DeclSummary *d = ctx->decls[i];
		if (decl_is_callable(d->kind) && d->name && strcmp(d->name, proc) == 0)
			return i;
	}
	return -1;
}

SemanticContext *semantic_analyze_cst(const SyntaxNode *root, const char *src) {
	SemanticContext *ctx = make_context();
	if (!root)
		return ctx;
	/* Collect the resolved DeclSummary table straight from the immutable syntax tree (+ module syntax
	 * trees) and run the analysis core. No abstract AST is built — analysis + tycheck read only the
	 * table, the tree, and the SemModel side table. */
	/* sem_collect_decls + the tree-qualify emit `module has no member` / region diagnostics via this ctx. */
	g_sem_qualify_ctx = ctx;
	g_rnop_on = 1; /* record the loader's rename ops so build_decl_table can rename the tree summaries */
	sem_collect_decls(ctx, root, src);
	g_sem_qualify_ctx = NULL;
	build_decl_table(ctx);
	sem_bind_defs(ctx); /* id-keyed channels: resolve callee_name/ref_name → DefId once names are final */
	analyze_program_core(ctx);
	walk_matches(ctx, root, src); /* exhaustiveness: enums register during analyze, so check after */
	return ctx;
}

void semantic_context_free(SemanticContext *ctx) {
	if (!ctx)
		return;

	sem_model_free(ctx->model);
	sem_hints_free(ctx->hints);

	for (int i = 0; i < ctx->diag_count; i++) {
		SemDiag *d = ctx->diags[i];
		for (int j = 0; j < d->note_count; j++)
			free(d->notes[j].message);
		free(d->notes);
		free(d->message);
		free(d);
	}
	free(ctx->diags);

	/* free constants (names only, values are owned by AST) */
	free(ctx->const_names);
	free(ctx->const_values);
	free(ctx->const_value_types);
	free(ctx->const_locs);

	/* free type-alias tables (name/backing strings are owned by the syntax tree) */
	for (int i = 0; i < ctx->owned_str_count; i++)
		free(ctx->owned_strs[i]);
	free(ctx->owned_strs);
	free(ctx->type_alias_names);
	free(ctx->type_alias_backings);
	free(ctx->type_alias_transparent);
	free(ctx->type_alias_locs);

	/* free opaque-destructor registry (these strings are owned here — sem_dupz'd) */
	for (int i = 0; i < ctx->drop_count; i++) {
		free(ctx->drop_type_names[i]);
		free(ctx->drop_proc_names[i]);
	}
	free(ctx->drop_type_names);
	free(ctx->drop_proc_names);
	free(ctx->drop_locs);

	/* free shapes (one per unique column structure) */
	for (int i = 0; i < ctx->archetype_count; i++) {
		ArchetypeInfo *arch = ctx->archetypes[i];
		free(arch->signature);
		free(arch->req_first);
		for (int j = 0; j < arch->field_count; j++) {
			free(arch->fields[j]->name);
			/* don't free arch->fields[j]->type - owned by AST */
			free(arch->fields[j]);
		}
		free(arch->fields);
		free(arch);
	}
	free(ctx->archetypes);

	/* free alias entries */
	for (int i = 0; i < ctx->alias_count; i++) {
		free(ctx->aliases[i]->name);
		free(ctx->aliases[i]);
	}
	free(ctx->aliases);

	/* free known functions */
	for (int i = 0; i < ctx->known_func_count; i++) {
		free(ctx->known_funcs[i]);
	}
	free(ctx->known_funcs);

	/* free static-global names */
	for (int i = 0; i < ctx->static_name_count; i++)
		free(ctx->static_names[i]);
	free(ctx->static_names);

	/* free group table (members are borrowed from syntax tree) */
	for (int i = 0; i < ctx->group_count; i++) {
		free(ctx->groups[i].name);
	}
	free(ctx->groups);

	/* free scopes and variables */
	for (int i = 0; i < ctx->scope_count; i++) {
		Scope *scope = &ctx->scopes[i];
		for (int j = 0; j < scope->var_count; j++) {
			free(scope->vars[j]->name);
			free(scope->vars[j]->archetype_name);
			/* don't free scope->vars[j]->type - might be owned by AST or NULL */
			free(scope->vars[j]);
		}
		free(scope->vars);
	}
	free(ctx->scopes);

	/* AST-kill: free the resolved decl-signature table (TypeId-only now; no per-decl TypeRefs). */
	free_decl_table(ctx);

	/* Freed LAST: HirType borrows the arena's interned name strings through lowering+codegen, and
	 * this is the compiler's final teardown. */
	ty_arena_free(ctx->ty_arena);

	free(ctx);
}

int semantic_has_errors(SemanticContext *ctx) {
	return ctx->error_count > 0;
}

SemModel *sem_context_model(SemanticContext *ctx) {
	return ctx ? ctx->model : NULL;
}

TypeArena *sem_context_arena(SemanticContext *ctx) {
	return ctx ? ctx->ty_arena : NULL;
}

SemHints *sem_context_hints(SemanticContext *ctx) {
	return ctx ? ctx->hints : NULL;
}

int sem_diag_slug_suppressed(SemanticContext *ctx, const char *slug) {
	if (!ctx || !slug)
		return 0;
	for (int i = 0; i < ctx->active_allow_slug_count; i++) {
		if (ctx->active_allow_slugs[i] && strcmp(ctx->active_allow_slugs[i], slug) == 0)
			return 1;
	}
	return 0;
}

int sem_diag_count(const SemanticContext *ctx) {
	return ctx ? ctx->diag_count : 0;
}

const SemDiag *sem_diag_at(const SemanticContext *ctx, int i) {
	if (!ctx || i < 0 || i >= ctx->diag_count)
		return NULL;
	return ctx->diags[i];
}

/* Append a related-location note to a previously-emitted diagnostic. NULL-tolerant
 * (sem_emit_<slug> wrappers return NULL for suppressed lints; callers don't branch). */
void sem_diag_note(SemDiag *parent, SourceLoc loc, const char *fmt, ...) {
	if (!parent)
		return;
	va_list ap;
	va_start(ap, fmt);
	va_list aq;
	va_copy(aq, ap);
	int needed = vsnprintf(NULL, 0, fmt, aq);
	va_end(aq);
	if (needed < 0) {
		va_end(ap);
		return;
	}
	char *msg = malloc((size_t)needed + 1);
	vsnprintf(msg, (size_t)needed + 1, fmt, ap);
	va_end(ap);
	parent->notes = realloc(parent->notes, (size_t)(parent->note_count + 1) * sizeof(SemDiagNote));
	parent->notes[parent->note_count].loc = loc;
	parent->notes[parent->note_count].message = msg;
	parent->note_count++;
}

const char *semantic_resolve_type_alias(SemanticContext *ctx, const char *name) {
	if (!ctx || !name)
		return name;
	return resolve_type_alias(ctx, name);
}

/* 1 if `name` is a registered type alias at all (tier-1 or tier-2). Lets the typechecker tell a
 * distinct-by-default subtype name apart from a bare primitive/width-int/archetype name. */
int semantic_is_type_alias(SemanticContext *ctx, const char *name) {
	return is_type_alias(ctx, name);
}

/* 1 if `name` is a TRANSPARENT (tier-1) alias — interchangeable with its backing. A tier-2 subtype
 * (the default) or a non-alias name returns 0. */
int semantic_alias_is_transparent(SemanticContext *ctx, const char *name) {
	return alias_is_transparent(ctx, name);
}

const char *semantic_resolve_callable_alias(SemanticContext *ctx, const char *name) {
	if (!ctx || !name)
		return NULL;
	return resolve_callable_alias(ctx, name);
}

TypeId semantic_callable_type_alias(SemanticContext *ctx, const char *name) {
	return callable_type_alias_id(ctx, name);
}

int semantic_is_enum_type(SemanticContext *ctx, const char *name) {
	return enum_is_type(ctx, name);
}

int semantic_enum_variant_value(SemanticContext *ctx, const char *enum_name, const char *variant, long *out) {
	return enum_variant_lookup(ctx, enum_name, variant, out);
}

int semantic_find_enum_variant(SemanticContext *ctx, const char *variant, long *out) {
	return enum_variant_lookup(ctx, NULL, variant, out);
}

int semantic_error_count(const SemanticContext *ctx) {
	if (!ctx)
		return 0;
	return ctx->error_count;
}

int semantic_archetype_exists(SemanticContext *ctx, const char *name) {
	return find_archetype(ctx, name) != NULL;
}

int semantic_field_exists(SemanticContext *ctx, const char *archetype_name, const char *field_name) {
	ArchetypeInfo *arch = find_archetype(ctx, archetype_name);
	return find_field(arch, field_name) != NULL;
}

FieldKind semantic_field_kind(SemanticContext *ctx, const char *archetype_name, const char *field_name) {
	ArchetypeInfo *arch = find_archetype(ctx, archetype_name);
	FieldInfo *field = find_field(arch, field_name);
	return field ? field->kind : FIELD_COLUMN;
}

const char *semantic_field_type_name(SemanticContext *ctx, const char *archetype_name, const char *field_name) {
	ArchetypeInfo *arch = find_archetype(ctx, archetype_name);
	FieldInfo *field = find_field(arch, field_name);
	if (!field)
		return NULL;
	TyKind k = tyid_kind(ctx->ty_arena, field->type_id);
	if (k != TYK_PRIM && k != TYK_NOMINAL)
		return NULL;
	return sem_tyid_name(ctx, field->type_id);
}

const char *semantic_get_const_value(SemanticContext *ctx, const char *const_name) {
	if (!ctx || !const_name)
		return NULL;
	for (int i = 0; i < ctx->const_count; i++) {
		if (strcmp(ctx->const_names[i], const_name) == 0) {
			return ctx->const_values[i];
		}
	}
	return NULL;
}
