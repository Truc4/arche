#include "semantic.h"
#include "../parser/parser.h"
#include "../syntax/syntax_view.h"
#include "sem_decls.h"
#include "sem_diag_internal.h"
#include "sem_diagnostics.h"
#include "sem_hints.h"
#include "sem_model.h"
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
	TypeRef *type;
	FieldKind kind;
} FieldInfo;

typedef struct {
	char *signature; /* deterministic key: "field:type:kind;" per field in order */
	FieldInfo **fields;
	int field_count;
	int is_allocated;   /* 1 once any alias for this shape has been alloc'd */
	int alloc_capacity; /* the driver pool's capacity (rows), 0 if not allocated by the driver */
	int min_rows;       /* max storage REQUIREMENT from device datasheets; the driver pool must meet it */
	int req_count;      /* how many distinct datasheets posted a requirement on this shape (shared-shape) */
	char *req_first;    /* name in the first requirement's decl (for the shared-shape build note) */
} ArchetypeInfo;

typedef struct {
	char *name;
	ArchetypeInfo *archetype;
} AliasEntry;

typedef struct {
	char *name;
	TypeRef *type;
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
	struct TypeRef **ctype_alias_refs; /* owned by the AST */
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

	/* AstProgram for looking up declarations */
	AstProgram *prog;

	/* syntax tree path only: the AstProgram reconstructed from the syntax tree (owned here; freed with the
	 * context). The side model borrows type-name strings that live in it, so it must
	 * outlive lowering+codegen — exactly like main.c keeps the parser-built `prog` alive. */
	AstProgram *owned_prog;

	/* AST-kill: the resolved decl-signature table that supersedes `owned_prog` for analysis.
	 * Built once (post rename/qualify) and read by the decl analyzers, the cross-decl scans, and
	 * tycheck. Owned here; freed at context teardown. */
	DeclSummary **decls;
	int decl_count;

	/* AST-kill (transitional): TypeRefs built on demand by view-driven analysis (via cst_build_type)
	 * are owned here and freed at context teardown — so a VariableInfo can borrow a type (and its
	 * name strings) for its whole lifetime without a use-after-free, exactly as it borrowed the
	 * AstProgram's TypeRefs before. */
	TypeRef **owned_types;
	int owned_type_count, owned_type_cap;

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
static char *compute_shape_signature(FieldSummary *fields, int field_count) {
	char **parts = field_count > 0 ? malloc(field_count * sizeof(char *)) : NULL;
	size_t total = 1;
	for (int i = 0; i < field_count; i++) {
		FieldSummary *f = &fields[i];
		const char *type_name = "unknown";
		if (f->type) {
			if (f->type->kind == TYPE_NAME)
				type_name = f->type->data.name;
			else if (f->type->kind == TYPE_ARRAY)
				type_name = "array";
			else if (f->type->kind == TYPE_SHAPED_ARRAY)
				type_name = "shaped_array";
			else if (f->type->kind == TYPE_OPAQUE)
				type_name = "opaque";
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

static void register_callable_type_alias(SemanticContext *ctx, const char *name, TypeRef *ref) {
	ctx->ctype_alias_names = realloc(ctx->ctype_alias_names, (ctx->ctype_alias_count + 1) * sizeof(char *));
	ctx->ctype_alias_refs = realloc(ctx->ctype_alias_refs, (ctx->ctype_alias_count + 1) * sizeof(TypeRef *));
	char *n = malloc(strlen(name) + 1);
	strcpy(n, name);
	ctx->ctype_alias_names[ctx->ctype_alias_count] = n;
	ctx->ctype_alias_refs[ctx->ctype_alias_count] = ref;
	ctx->ctype_alias_count++;
}

static TypeRef *callable_type_alias_ref(SemanticContext *ctx, const char *name) {
	if (!ctx || !name)
		return NULL;
	for (int i = 0; i < ctx->ctype_alias_count; i++)
		if (strcmp(ctx->ctype_alias_names[i], name) == 0)
			return ctx->ctype_alias_refs[i];
	return NULL;
}

static char *sem_dupz(const char *s);

/* Record an enum's type name + its (variant → value) entries. The type-alias-to-int registration
 * is done by the caller (register_type_alias is defined later). */
static void register_enum_entries(SemanticContext *ctx, DeclSummary *e) {
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
		if ((d->kind == DECL_PROC || d->kind == DECL_FUNC) && d->name && strcmp(d->name, name) == 0)
			return d;
	}
	return NULL;
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

/* TypeRef structural equality. Used for member-signature distinctness. */
static int type_ref_equal(const TypeRef *a, const TypeRef *b) {
	if (a == NULL && b == NULL)
		return 1;
	if (a == NULL || b == NULL)
		return 0;
	if (a->kind != b->kind)
		return 0;
	switch (a->kind) {
	case TYPE_NAME: {
		const char *an = normalize_type_name(a->data.name);
		const char *bn = normalize_type_name(b->data.name);
		return an && bn && strcmp(an, bn) == 0;
	}
	case TYPE_ARRAY:
		return type_ref_equal(a->data.array.element_type, b->data.array.element_type);
	case TYPE_SHAPED_ARRAY:
		return a->data.shaped_array.rank == b->data.shaped_array.rank &&
		       type_ref_equal(a->data.shaped_array.element_type, b->data.shaped_array.element_type);
	case TYPE_HANDLE: {
		const char *an = a->data.handle.archetype_name;
		const char *bn = b->data.handle.archetype_name;
		return an && bn && strcmp(an, bn) == 0;
	}
	case TYPE_TUPLE: {
		if (a->data.tuple.field_count != b->data.tuple.field_count)
			return 0;
		for (int i = 0; i < a->data.tuple.field_count; i++) {
			if (!type_ref_equal(a->data.tuple.field_types[i], b->data.tuple.field_types[i]))
				return 0;
		}
		return 1;
	}
	case TYPE_ARCHETYPE:
		return 1; /* `archetype` parameter type: any two are equal */
	case TYPE_OPAQUE:
		return 1; /* bare opaque == bare opaque; nominal distinctness is by alias name */
	case TYPE_TYPE:
		return 1; /* the meta-type: any two `type` are equal */
	case TYPE_PROC:
	case TYPE_FUNC:
		/* Callable types match structurally (param/result shape), names ignored. */
		if (a->data.callable.is_proc != b->data.callable.is_proc ||
		    a->data.callable.param_count != b->data.callable.param_count ||
		    a->data.callable.result_count != b->data.callable.result_count)
			return 0;
		for (int i = 0; i < a->data.callable.param_count; i++)
			if (!type_ref_equal(a->data.callable.param_types[i], b->data.callable.param_types[i]))
				return 0;
		for (int i = 0; i < a->data.callable.result_count; i++)
			if (!type_ref_equal(a->data.callable.result_types[i], b->data.callable.result_types[i]))
				return 0;
		return 1;
	}
	return 0;
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

/* True if a binding is opaque-backed (the linear, move-only kind). Used both by the
 * must-consume check and by return/insert auto-marking (only opaque is consumed by
 * being returned or inserted — data and handles copy). */
static int var_is_opaque(SemanticContext *ctx, VariableInfo *v) {
	if (!v)
		return 0;
	if (v->inferred_type && strcmp(v->inferred_type, "opaque") == 0)
		return 1;
	if (v->type) {
		if (v->type->kind == TYPE_OPAQUE)
			return 1;
		if (v->type->kind == TYPE_NAME && strcmp(resolve_type_alias(ctx, v->type->data.name), "opaque") == 0)
			return 1;
	}
	return 0;
}

/* The nominal opaque type name for a variable (e.g. "file"/"socket"), used to look up its
 * registered `@drop` destructor. NULL if not an opaque local with a nominal name. */
static const char *var_opaque_type_name(SemanticContext *ctx, VariableInfo *v) {
	if (!var_is_opaque(ctx, v))
		return NULL;
	if (v->nominal_type)
		return v->nominal_type;
	if (v->type && v->type->kind == TYPE_NAME)
		return v->type->data.name;
	return NULL;
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
	if (!p->type || p->type->kind != TYPE_NAME || strcmp(resolve_type_alias(ctx, p->type->data.name), "opaque") != 0) {
		sem_emit_drop_invalid(ctx, proc->loc, "a `@drop` destructor's parameter must be of an opaque type");
		return;
	}
	/* The `@drop(<type>)` name must match the parameter's type — the decorator states what is
	 * dropped; a mismatch is a typo, not silently ignored. */
	if (declared_type && strcmp(declared_type, p->type->data.name) != 0) {
		sem_emit_drop_invalid(ctx, proc->loc,
		                      "`@drop(...)` names a different type than the destructor's parameter — they must match");
		return;
	}
	register_drop(ctx, p->type->data.name, proc->name, proc->loc);
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

static void add_variable_with_archetype(SemanticContext *ctx, const char *name, TypeRef *type,
                                        const char *archetype_name);

static void add_variable(SemanticContext *ctx, const char *name, TypeRef *type) {
	add_variable_with_archetype(ctx, name, type, NULL);
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

static void add_variable_with_archetype(SemanticContext *ctx, const char *name, TypeRef *type,
                                        const char *archetype_name) {
	if (ctx->scope_count == 0)
		return;

	Scope *scope = &ctx->scopes[ctx->scope_count - 1];
	VariableInfo *var = malloc(sizeof(VariableInfo));
	var->name = malloc(strlen(name) + 1);
	strcpy(var->name, name);
	var->type = type;
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
static const char *lvalue_leftmost_name(Expression *expr);

/* By-reference aggregate param types: arrays are passed by reference (borrowed read-only by
 * default), so mutating one through a non-`move` param is a purity violation. Scalars are by
 * value (a freely-mutable local copy); opaque can't be indexed/assigned at all. */
static int type_is_byref_aggregate(const TypeRef *t) {
	return t && (t->kind == TYPE_ARRAY || t->kind == TYPE_SHAPED_ARRAY);
}

/* Implicit `move` of a bare move-only name in an ownership-taking position (defined below). */
static void implicit_move_consume(SemanticContext *ctx, SyntaxView v);

/* AST-kill (transitional): recover the SyntaxView an AstProgram node was reconstructed from.
 * Lets analysis read the immutable syntax tree directly while the AstProgram readers are
 * converted to views leaf-up. NULL-safe (an absent node yields an absent view), since AST
 * pointers handed to converted readers are frequently optional (e.g. a bind with no value).
 * Removed with AstProgram once nothing reads these structs. */
#define AST_SV(n) ((n) ? ((SyntaxView){(n)->sn, (n)->sn_src}) : ((SyntaxView){NULL, NULL}))

/* Own a transient analysis-built TypeRef on the context (freed at teardown); returns it. */
static TypeRef *analysis_own_type(SemanticContext *ctx, TypeRef *t);

/* Syntax-tree helpers used by the converted analysis, defined alongside cst_to_program below. */
SyntaxView sem_first_expr(SyntaxView v);
SyntaxView sem_node_at_expr(SyntaxView v, int idx);
SyntaxView sem_type_at(SyntaxView v, int idx);
static TypeRef *cst_build_type(SyntaxView t);
static Statement *cst_build_stmt(SyntaxView s);
static char *sem_cv_dup(SyntaxView v);
static char *sem_txt_dup(SynText t);
char *sem_cv_dup_first_token(SyntaxView v);
SourceLoc sem_node_loc(const SyntaxNode *n);
static Operator sem_tok_to_op(TokenKind k);
Operator sem_binary_op(SyntaxView v);
int sem_expr_count(SyntaxView v);
/* Name string of an SN_NAME_EXPR view (mirrors cst_build_expr's SN_NAME_EXPR case, incl.
 * table<Name> → bare archetype). Caller frees. */
static char *sv_name_expr_dup(SyntaxView e);
/* Resolved leftmost name of a NAME/FIELD/INDEX/SLICE view: the AST-resolved name (module-inlined
 * names are prefixed) from the ref_name channel, else the bare token. Caller frees. */
static char *sv_resolved_name(SemanticContext *ctx, SyntaxView v);

/* ========== TYPE RESOLUTION ========== */

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

/* Register a nominal alias `name → backing`; redefinition must AGREE (same backing).
 * `loc` is the alias declaration's source position — used for the redefinition error
 * and stored so the late "unknown backing" pass can blame the original site. */
static void register_type_alias_tiered(SemanticContext *ctx, const char *name, const char *backing, int transparent,
                                       SourceLoc loc, int from_datasheet) {
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
	if (!c || !c->const_decl_type || c->const_decl_type->kind != TYPE_NAME)
		return; /* only concrete named declared types */
	if (c->const_value_kind != EXPR_LITERAL)
		return; /* only literal RHS (a name RHS is a value-const chain, checked elsewhere) */
	const char *want = resolve_type_alias(ctx, normalize_type_name(c->const_decl_type->data.name));
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
static const char *type_backing_name(TypeRef *t) {
	if (!t)
		return NULL;
	if (t->kind == TYPE_NAME)
		return t->data.name;
	if (t->kind == TYPE_OPAQUE)
		return "opaque";
	return NULL;
}

/* The meta-type `type` is legal ONLY as a declaration's declared type (`name : type : T`). It is
 * not a storable value type, so it must not reach a parameter / return / field / variable slot —
 * a parameter of type `type` would be a generic type parameter, and generics (monomorphization)
 * are not implemented yet. Reject it there with a clear "not supported yet" message (the grammar
 * admits the form; the feature behind it just doesn't exist). Returns 1 if it errored. */
static int reject_meta_type(SemanticContext *ctx, TypeRef *t, const char *where) {
	if (t && t->kind == TYPE_TYPE) {
		sem_emit_meta_type_invalid_position(ctx, t->loc, where);
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
		if (var->type) {
			if (var->type->kind == TYPE_HANDLE)
				return var->type->data.handle.archetype_name;
			if (var->type->kind == TYPE_NAME)
				return resolve_type_alias(ctx, normalize_type_name(var->type->data.name));
			/* An array variable resolves to its ELEMENT base type, so `b[i]` and width-sensitive
			 * consumers see int/float/i64, not a defaulted int. */
			if (var->type->kind == TYPE_SHAPED_ARRAY || var->type->kind == TYPE_ARRAY) {
				TypeRef *et = var->type;
				while (et && et->kind == TYPE_SHAPED_ARRAY)
					et = et->data.shaped_array.element_type;
				while (et && et->kind == TYPE_ARRAY)
					et = et->data.array.element_type;
				if (et && et->kind == TYPE_NAME)
					return resolve_type_alias(ctx, normalize_type_name(et->data.name));
			}
		}
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
		if (field && field->type) {
			TypeRef *ft = field->type;
			while (ft->kind == TYPE_SHAPED_ARRAY)
				ft = ft->data.shaped_array.element_type;
			if (ft->kind == TYPE_NAME)
				return resolve_type_alias(ctx, normalize_type_name(ft->data.name));
		}
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
 * on the LAST field resolve (matching the old EXPR_FIELD path), else NULL. */
static const char *resolve_base_chain_type(SemanticContext *ctx, SyntaxView v) {
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
	if (fs) {
		TypeRef *rt = fs->return_type_count > 0 ? fs->return_types[0] : NULL;
		if (!rt)
			return NULL;
		if (rt->kind == TYPE_HANDLE)
			return rt->data.handle.archetype_name;
		if (rt->kind == TYPE_NAME)
			return resolve_type_alias(ctx, normalize_type_name(rt->data.name));
		if (rt->kind == TYPE_SHAPED_ARRAY || rt->kind == TYPE_ARRAY) {
			/* An array return resolves to its ELEMENT type; a char array stays "char_array". */
			TypeRef *et = rt;
			while (et && et->kind == TYPE_SHAPED_ARRAY)
				et = et->data.shaped_array.element_type;
			while (et && et->kind == TYPE_ARRAY)
				et = et->data.array.element_type;
			if (et && et->kind == TYPE_NAME && et->data.name) {
				const char *en = normalize_type_name(et->data.name);
				return (strcmp(en, "char") == 0) ? "char_array" : resolve_type_alias(ctx, en);
			}
			return "char_array";
		}
		if (rt->kind == TYPE_OPAQUE)
			return "opaque"; /* foreign resource value (pointer-width i64) */
		return NULL;
	}
	return NULL;
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
		/* `Enum.variant` is a compile-time int constant (enums are i32-backed), the same value the
		 * old analysis folded the field access into. Handle it before the archetype/field path. */
		if (sv_count(v, SN_FIELD_NAME) == 1) {
			char *idnt = sv_resolved_name(ctx, v);
			if (enum_is_type(ctx, idnt)) {
				char *fld = sem_cv_dup(sv_child_at(v, SN_FIELD_NAME, 0));
				long ev = 0;
				int is_variant = enum_variant_lookup(ctx, idnt, fld, &ev);
				free(idnt);
				free(fld);
				if (is_variant)
					return "i32";
			} else {
				free(idnt);
			}
		}
		return resolve_base_chain_type(ctx, v);
	}

	case SN_INDEX_EXPR: {
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
							TypeRef *pt = fd->params[j].type;
							if (!rt || !pt || pt->kind != TYPE_NAME) {
								ok = 0;
								break;
							}
							if (strcmp(normalize_type_name(pt->data.name), normalize_type_name(rt)) != 0) {
								ok = 0;
								break;
							}
						}
						TypeRef *frt = fd->return_type_count > 0 ? fd->return_types[0] : NULL;
						if (ok && frt && frt->kind == TYPE_NAME)
							result = normalize_type_name(frt->data.name);
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
		if (var->type && var->type->kind == TYPE_NAME && is_type_alias(ctx, var->type->data.name) &&
		    !alias_is_transparent(ctx, var->type->data.name))
			return var->type->data.name;
		return NULL;
	}
	/* simple (unqualified) callee only — a qualified `mod.f` has SN_FIELD_NAME children */
	if (k == SN_CALL_EXPR && sv_count(v, SN_FIELD_NAME) == 0) {
		char *cn = sem_cv_dup(sv_child(v, SN_CALLEE_NAME));
		DeclSummary *fd = find_func_sig(ctx, cn);
		free(cn);
		TypeRef *frt = (fd && fd->return_type_count > 0) ? fd->return_types[0] : NULL;
		if (frt && frt->kind == TYPE_NAME && is_type_alias(ctx, frt->data.name))
			return frt->data.name;
	}
	return NULL;
}

/* ========== EXPRESSION ANALYSIS ========== */

/* Analyze a FIELD/INDEX/SLICE base = leading IDENT + optional SN_FIELD_NAME chain. Replicates the
 * old recursion's diagnostics: base-name resolution (undefined symbol), and archetype/tuple field
 * checks. The old AST flattened `a.b.c` into nested FIELDs; here we read the flat node directly.
 * `field_loc` is the location used for field diagnostics (the whole expr). */
static void analyze_base_chain(SemanticContext *ctx, SyntaxView v, SourceLoc field_loc) {
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
				sem_emit_undefined_field_base(ctx, field_loc, idnt);
				goto done;
			}
			/* reading a component through a HANDLE value is unsupported */
			int base_is_handle = (base_var->type && base_var->type->kind == TYPE_HANDLE) ||
			                     (base_var->inferred_type && strcmp(base_var->inferred_type, "handle") == 0);
			if (base_is_handle) {
				const char *an = (base_var->type && base_var->type->kind == TYPE_HANDLE)
				                     ? base_var->type->data.handle.archetype_name
				                     : NULL;
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
			if (!arch && base_var->type && !sys_column_access) {
				if (base_var->type->kind == TYPE_NAME)
					arch = find_archetype(ctx, base_var->type->data.name);
				if (!arch && base_var->type->kind != TYPE_TUPLE && base_var->type->kind != TYPE_ARCHETYPE) {
					if ((base_var->type->kind == TYPE_ARRAY || base_var->type->kind == TYPE_SHAPED_ARRAY) &&
					    (strcmp(field_name, "cap") == 0 || strcmp(field_name, "capacity") == 0 ||
					     strcmp(field_name, "length") == 0 || strcmp(field_name, "max_length") == 0))
						goto done;
					const char *kind_name;
					switch (base_var->type->kind) {
					case TYPE_NAME:
						kind_name = base_var->type->data.name;
						break;
					case TYPE_ARRAY:
						kind_name = "array";
						break;
					case TYPE_SHAPED_ARRAY:
						kind_name = "shaped array";
						break;
					case TYPE_OPAQUE:
						kind_name = "opaque";
						break;
					case TYPE_TYPE:
						kind_name = "type";
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
				if (mv->is_param && !mv->is_own && type_is_byref_aggregate(mv->type))
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
				else if (type_is_byref_aggregate(cv->type) &&
				         !(cv->type && cv->type->kind == TYPE_SHAPED_ARRAY && !cv->is_param))
					sem_emit_copy_unsupported(ctx, sem_node_loc(operand.node), nm);
			}
			free(nm);
		}
		break;
	}

	case SN_CALL_EXPR: {
		int call_stmt_ok = ctx->stmt_call_ok;
		ctx->stmt_call_ok = 0;

		/* resolved callee name (qualify-mangled for `mod.f`) from the side model; NULL for a
		 * non-module qualified field call. */
		const char *resolved_callee = ctx->model ? sem_model_callee_name(ctx->model, sv_id(v)) : NULL;
		char *func_name = NULL;
		if (resolved_callee)
			func_name = sem_dupz(resolved_callee);
		else if (sv_count(v, SN_FIELD_NAME) == 0)
			func_name = sem_cv_dup(sv_child(v, SN_CALLEE_NAME));

		int argc = sem_expr_count(v);

		/* Type conversion `T(x)`: callee is a type name; analyze the args and stop. */
		if (func_name && is_type_conversion_callee(ctx, func_name)) {
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
			int is_known_func = find_known_func(ctx, func_name);
			int is_group = find_group(ctx, func_name) != NULL;
			VariableInfo *cv = find_variable(ctx, func_name);
			int is_arch = find_archetype(ctx, func_name) != NULL;
			int is_const = semantic_get_const_value(ctx, func_name) != NULL;
			/* a user func/proc decl is a valid callee even if not in the known-func table */
			int is_decl = find_callable_sig(ctx, func_name) != NULL;
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
					if (!p->type || p->type->kind != TYPE_SHAPED_ARRAY)
						continue;
					SyntaxView a = sem_node_at_expr(v, j);
					while (sv_present(a) && sv_kind(a) == SN_UNARY_EXPR &&
					       (sv_has_token(a, TOK_MOVE) || sv_has_token(a, TOK_COPY)))
						a = sem_first_expr(a);
					if (sv_present(a) && sv_kind(a) == SN_NAME_EXPR && sv_count(a, SN_FIELD_NAME) == 0) {
						char *nm = sv_name_expr_dup(a);
						VariableInfo *av = find_variable(ctx, nm);
						if (av && av->type && av->type->kind == TYPE_ARRAY) {
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
				/* extern-type nominal distinctness per argument */
				if (is_extern) {
					int check_count = param_count < argc ? param_count : argc;
					for (int j = 0; j < check_count; j++) {
						ParamSummary *p = &params[j];
						if (p->type && p->type->kind == TYPE_NAME && is_type_alias(ctx, p->type->data.name)) {
							const char *arg_nominal = nominal_type_of_expr(ctx, sem_node_at_expr(v, j));
							if (arg_nominal && strcmp(arg_nominal, p->type->data.name) != 0)
								sem_emit_extern_type_mismatch(ctx, sem_node_loc(sem_node_at_expr(v, j).node), func_name,
								                              p->name ? p->name : "?", p->type->data.name, arg_nominal);
						}
					}
				}
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
					TypeRef *pt = fd->params[j].type;
					if (!pt || pt->kind != TYPE_NAME) {
						ok = 0;
						break;
					}
					if (strcmp(normalize_type_name(pt->data.name), normalize_type_name(rt)) != 0) {
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

	/* Resolve + record this expression's type (and nominal) in the side model, keyed by node id. */
	const char *resolved = resolve_expression_type(ctx, v);
	if (ctx->model) {
		uint32_t nid = sv_id(v);
		sem_model_set_expr_type(ctx->model, nid, resolved);
		const char *nom = nominal_type_of_expr(ctx, v);
		if (nom && is_type_alias(ctx, nom) && !alias_is_transparent(ctx, nom))
			sem_model_set_expr_nominal(ctx->model, nid, nom);
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
	if (!var || !(type_is_byref_aggregate(var->type) || var_is_opaque(ctx, var))) {
		free(nm); /* no var, or a Copy type: a bare name copies, never moves */
		return;
	}
	if (var->is_param && !var->is_own && type_is_byref_aggregate(var->type)) {
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
/* Build a TypeRef from a type-node view, owned by the context (stable for the lifetime of any
 * VariableInfo that borrows it). Absent view → NULL. */
static TypeRef *sem_view_type(SemanticContext *ctx, SyntaxView tv) {
	return sv_present(tv) ? analysis_own_type(ctx, cst_build_type(tv)) : NULL;
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
		TypeRef *btype = NULL, *btype_value = NULL;
		SyntaxView value_view = {NULL, v.src};
		if (is_const && t0_is_meta && sv_present(t1)) {
			btype_value = sem_view_type(ctx, t1); /* `x : type : <T>` — local type alias */
		} else {
			btype = sem_view_type(ctx, t0);
			value_view = sem_node_at_expr(v, 1);
		}

		/* A local constant whose RHS denotes a TYPE is a local nominal type alias. */
		if (is_const) {
			const char *backing = NULL;
			if (btype_value)
				backing = type_backing_name(btype_value);
			else if (sv_present(value_view) && sv_kind(value_view) == SN_NAME_EXPR) {
				char *vn = sv_resolved_name(ctx, value_view);
				if (name_denotes_type(ctx, vn))
					backing = vn; /* leaked-with-alias-table lifetime; matches old AST-owned name */
				else
					free(vn);
			}
			if (backing || btype_value) {
				if (!backing) {
					sem_emit_local_alias_invalid_backing(ctx, loc);
					free(bind_name);
				} else {
					/* The alias registry BORROWS the name (and backing) by pointer — they must outlive
					 * the context, so bind_name is NOT freed here (it is intentionally handed off). */
					register_type_alias_tiered(ctx, bind_name, backing, btype_value ? btype_value->is_transparent : 0,
					                           loc, 0);
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
		if (btype && btype->kind == TYPE_ARCHETYPE) {
			sem_emit_archetype_only_as_param(ctx, loc);
			free(bind_name);
			break;
		}
		if (reject_meta_type(ctx, btype, "variable type")) {
			free(bind_name);
			break;
		}
		/* Single-value bind: analyze value, implicit move, declare. */
		ctx->stmt_call_ok = (sv_present(value_view) && sv_kind(value_view) == SN_CALL_EXPR);
		analyze_expression(ctx, value_view);
		ctx->stmt_call_ok = 0;
		implicit_move_consume(ctx, value_view);

		check_shadows_callable(ctx, bind_name, loc);
		add_variable(ctx, bind_name, btype);

		/* Type annotation → inferred/nominal type. */
		if (ctx->scope_count > 0) {
			Scope *scope = &ctx->scopes[ctx->scope_count - 1];
			if (scope->var_count > 0) {
				VariableInfo *var = scope->vars[scope->var_count - 1];
				if (btype) {
					TypeRef *t = btype;
					if (t->kind == TYPE_HANDLE)
						var->inferred_type = t->data.handle.archetype_name;
					else if (t->kind == TYPE_NAME) {
						var->inferred_type = resolve_type_alias(ctx, t->data.name);
						if (is_type_alias(ctx, t->data.name))
							var->nominal_type = t->data.name;
					} else if (t->kind == TYPE_SHAPED_ARRAY || t->kind == TYPE_ARRAY) {
						TypeRef *et = t;
						while (et && et->kind == TYPE_SHAPED_ARRAY)
							et = et->data.shaped_array.element_type;
						while (et && et->kind == TYPE_ARRAY)
							et = et->data.array.element_type;
						var->inferred_type =
						    (et && et->kind == TYPE_NAME) ? resolve_type_alias(ctx, et->data.name) : NULL;
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
		/* Purity: a borrowed (non-`move`) array parameter is read-only. Uses the leftmost name. */
		{
			const char *rn = ctx->model ? sem_model_ref_name(ctx->model, sv_id(target)) : NULL;
			char *ln = rn ? sem_dupz(rn) : sv_name_expr_dup(target);
			VariableInfo *pv = ln ? find_variable(ctx, ln) : NULL;
			if (pv && pv->is_param && !pv->is_own && !pv->is_out_place && type_is_byref_aggregate(pv->type))
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
				if (ctx->current_func && rvar && rvar->type && type_is_byref_aggregate(rvar->type) && !rvar->is_param) {
					fprintf(stderr, "Error: cannot return a local array by value (array copy-out is not implemented); "
					                "return an `own` parameter or thread a caller-provided buffer instead\n");
					ctx->error_count++;
				}
				free(nm);
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
		TypeRef *filter = sem_view_type(ctx, sem_type_at(v, 0));
		if (filter) {
			if (filter->kind != TYPE_NAME) {
				sem_emit_each_field_filter_type_not_name(ctx, loc);
			} else {
				const char *fn = normalize_type_name(filter->data.name);
				if (!fn || (strcmp(fn, "int") != 0 && strcmp(fn, "float") != 0 && strcmp(fn, "char") != 0))
					sem_emit_each_field_filter_type_not_primitive(ctx, loc);
			}
		}
		int arch_param_ok = 0;
		if (ctx->current_proc) {
			for (int i = 0; i < ctx->current_proc->param_count; i++) {
				ParamSummary *p = &ctx->current_proc->params[i];
				if (p->name && strcmp(p->name, arch_param) == 0 && p->type && p->type->kind == TYPE_ARCHETYPE) {
					arch_param_ok = 1;
					break;
				}
			}
		}
		if (!arch_param_ok)
			sem_emit_each_field_invalid_rhs(ctx, loc, arch_param);
		push_scope(ctx);
		add_variable(ctx, binding_name, NULL);
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
		Statement *mb = cst_build_stmt(v);
		if (!mb) {
			break;
		}
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
		if (sv_present(mb_value))
			analyze_expression(ctx, mb_value);
		ctx->stmt_call_ok = 0;

		/* resolve callee proc for the inout-redundant lint + out-param typing */
		if (sv_present(mb_value) && sv_kind(mb_value) == SN_CALL_EXPR) {
			const char *cn = ctx->model ? sem_model_callee_name(ctx->model, sv_id(mb_value)) : NULL;
			char *cnf = cn ? sem_dupz(cn) : sem_cv_dup(sv_child(mb_value, SN_CALLEE_NAME));
			mb_callee_proc = find_proc_sig(ctx, cnf);
			free(cnf);
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
				for (int t = 0; t < mb->data.multi_bind.target_count; t++)
					if (mb->data.multi_bind.targets[t].name && strcmp(mb->data.multi_bind.targets[t].name, an) == 0) {
						sem_emit_lint_inout_redundant_arg(ctx, sem_node_loc(a.node), an);
						break;
					}
				free(an);
			}
		}

		/* bind the targets (new shadows; existing must be live) */
		for (int i = 0; i < mb->data.multi_bind.target_count; i++) {
			BindingTarget *t = &mb->data.multi_bind.targets[i];
			TypeRef *bind_type = t->type;
			if (!bind_type && mb_callee_proc && i < mb_callee_proc->out_param_count)
				bind_type = mb_callee_proc->out_params[i].type;
			if (t->is_new) {
				/* already added above for "_"-filtered new targets; re-add to capture type/nominal */
				if (t->name && strcmp(t->name, "_") != 0) {
					add_variable(ctx, t->name, bind_type);
					if (bind_type && bind_type->kind == TYPE_NAME && ctx->scope_count > 0) {
						Scope *sc = &ctx->scopes[ctx->scope_count - 1];
						if (sc->var_count > 0) {
							VariableInfo *vv = sc->vars[sc->var_count - 1];
							vv->inferred_type = resolve_type_alias(ctx, bind_type->data.name);
							if (is_type_alias(ctx, bind_type->data.name))
								vv->nominal_type = bind_type->data.name;
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
		/* bind_type may borrow from mb's targets or the proc out-params; the proc out-param types are
		 * AST-owned (stable), and target types are freed with mb — but add_variable already consumed
		 * what it needed (var->type points at it). To avoid a UAF, own any target type on the ctx. */
		for (int i = 0; i < mb->data.multi_bind.target_count; i++)
			if (mb->data.multi_bind.targets[i].type) {
				analysis_own_type(ctx, mb->data.multi_bind.targets[i].type);
				mb->data.multi_bind.targets[i].type = NULL; /* detach so statement_free won't free it */
			}
		statement_free(mb);
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
		reject_meta_type(ctx, arch->fields[i].type, "archetype component type");

	/* proc/func types can't be archetype components — archetypes are data; per-row behavior
	 * dispatch is the anti-pattern (Stage D dropped). Use `match` or a system instead. */
	for (int i = 0; i < arch->field_count; i++) {
		TypeRef *t = arch->fields[i].type;
		if (!t)
			continue;
		/* inline `on_hit :: proc()()`, or a named callable-type alias `on_hit :: handler`. */
		int callable = (t->kind == TYPE_PROC || t->kind == TYPE_FUNC) ||
		               (t->kind == TYPE_NAME && callable_type_alias_ref(ctx, t->data.name) != NULL);
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

	char *sig = compute_shape_signature(arch->fields, arch->field_count);
	ArchetypeInfo *shape = find_archetype_by_signature(ctx, sig);

	if (!shape) {
		/* New unique shape — create it */
		shape = malloc(sizeof(ArchetypeInfo));
		shape->signature = sig;
		shape->is_allocated = 0;
		shape->alloc_capacity = 0;
		shape->min_rows = 0;
		shape->req_count = 0;
		shape->req_first = NULL;

		/* Count total fields after expanding tuples */
		int expanded_field_count = 0;
		for (int i = 0; i < arch->field_count; i++) {
			if (arch->fields[i].type->kind == TYPE_TUPLE) {
				expanded_field_count += arch->fields[i].type->data.tuple.field_count;
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
			if (field->type->kind == TYPE_TUPLE) {
				/* Expand tuple into component fields */
				for (int j = 0; j < field->type->data.tuple.field_count; j++) {
					FieldInfo *fi = malloc(sizeof(FieldInfo));
					char expanded_name[512];
					snprintf(expanded_name, sizeof(expanded_name), "%s_%s", field->name,
					         field->type->data.tuple.field_names[j]);
					fi->name = malloc(strlen(expanded_name) + 1);
					strcpy(fi->name, expanded_name);
					fi->type = field->type->data.tuple.field_types[j];
					fi->kind = field->kind;
					shape->fields[field_idx++] = fi;
				}
			} else {
				FieldInfo *fi = malloc(sizeof(FieldInfo));
				fi->name = malloc(strlen(field->name) + 1);
				strcpy(fi->name, field->name);
				fi->type = field->type;
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
		TypeRef *ft = arch->fields[i].type;
		if (ft->kind != TYPE_HANDLE)
			continue;
		const char *target = ft->data.handle.archetype_name;
		if (!find_archetype(ctx, target)) {
			fprintf(stderr, "Error: unknown archetype '%s' in handle type for field '%s'\n", target,
			        arch->fields[i].name);
			ctx->error_count++;
		}
	}

	/* Register alias */
	AliasEntry *entry = malloc(sizeof(AliasEntry));
	entry->name = malloc(strlen(arch->name) + 1);
	strcpy(entry->name, arch->name);
	entry->archetype = shape;
	ctx->aliases = realloc(ctx->aliases, (ctx->alias_count + 1) * sizeof(AliasEntry *));
	ctx->aliases[ctx->alias_count++] = entry;
}

static void analyze_static_array_decl(SemanticContext *ctx, DeclSummary *s) {
	if (!s)
		return;

	/* Validate element type is a scalar */
	if (!s->static_type) {
		fprintf(stderr, "Error: static array '%s' missing element type\n", s->name);
		ctx->error_count++;
		return;
	}

	if (s->static_type->kind != TYPE_NAME) {
		fprintf(stderr, "Error: static array '%s' element type must be scalar (int, float, char, etc.)\n", s->name);
		ctx->error_count++;
		return;
	}

	const char *type_name = s->static_type->data.name;
	if (strcmp(type_name, "int") != 0 && strcmp(type_name, "float") != 0 && strcmp(type_name, "char") != 0 &&
	    strcmp(type_name, "double") != 0) {
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

	/* Constant array initializers (`buf : T[N] = {…}`) are not lowered yet — zero-init only. */
	if (s->static_has_init) {
		fprintf(stderr,
		        "Error: global '%s' array initializers are not yet implemented; declare it zero-initialized "
		        "(`%s : T[N]`)\n",
		        s->name, s->name);
		ctx->error_count++;
		return;
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
	return 0;
}

static void analyze_static_scalar_decl(SemanticContext *ctx, DeclSummary *s) {
	if (!s)
		return;
	if (!s->static_type || s->static_type->kind != TYPE_NAME) {
		fprintf(stderr, "Error: global '%s' has an invalid scalar type\n", s->name);
		ctx->error_count++;
		return;
	}
	if (!sem_is_const_init(ctx, s->static_init)) {
		fprintf(stderr, "Error: global '%s' initializer must be a compile-time constant (a literal or const)\n",
		        s->name);
		ctx->error_count++;
		return;
	}
	/* Registration is hoisted (see the pre-pass); nothing to add here. */
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
		fprintf(stderr, "Error: alloc count must be a literal; dynamic counts not yet supported\n");
		ctx->error_count++;
		return;
	}
	/* Record the driver pool's capacity so the final sweep can check it against datasheet minimums. */
	arch->alloc_capacity = alloc->static_pool_count;

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

/* Rule 3: a device's impl (`.arche` of a unit with a `.ds.arche`) is BEHAVIOR-ONLY. It may not define
 * a type/enum/archetype or allocate a pool — types/archetypes belong in the datasheet, and allocation
 * is the driver's alone. Decls from a device impl were tagged `from_device_impl` at module collection.
 * (Value consts + buffers/scalars are allowed; only type/archetype definitions + archetype pools are
 * rejected.) */
static const char *sem_decl_name(Decl *d); /* fwd (defined with the module-collection helpers) */
static void sem_check_device_impl_decls(SemanticContext *ctx) {
	for (int i = 0; i < ctx->decl_count; i++) {
		DeclSummary *d = ctx->decls[i];
		if (!d->from_device_impl)
			continue;
		const char *nm0 = d->name;
		const char *what = NULL;
		if (d->kind == DECL_ARCHETYPE)
			what = "define an archetype";
		else if (d->kind == DECL_ENUM)
			what = "define a type";
		else if (d->kind == DECL_CONST && nm0 && is_type_alias(ctx, nm0)) /* type alias / opaque (not a value const) */
			what = "define a type";
		else if (d->kind == DECL_STATIC && d->static_kind == STATIC_KIND_ARCHETYPE)
			what = "allocate a pool";
		if (!what)
			continue;
		fprintf(stderr,
		        "Error: a device's impl cannot %s ('%s') — types/archetypes belong in its .ds.arche "
		        "datasheet, and allocation is the driver's\n",
		        what, nm0 ? nm0 : "?");
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
		TypeRef *t = p->type;
		if (!t)
			return 0;
		if (t->kind == TYPE_PROC)
			return 1;
		if (t->kind == TYPE_FUNC)
			return 0;
		if (t->kind == TYPE_NAME && t->data.name) {
			TypeRef *al = semantic_callable_type_alias(ctx, t->data.name);
			if (al && al->kind == TYPE_PROC)
				return 1;
		}
		return 0;
	}
	return 0;
}

/* Returns the leftmost identifier in an lvalue-ish expression chain, or NULL.
 * For `Transaction.price[i]` returns "Transaction"; for `x` returns "x". */
static const char *lvalue_leftmost_name(Expression *expr) {
	while (expr) {
		switch (expr->type) {
		case EXPR_NAME:
			return expr->data.name.name;
		case EXPR_FIELD:
			expr = expr->data.field.base;
			break;
		case EXPR_INDEX:
			expr = expr->data.index.base;
			break;
		default:
			return NULL;
		}
	}
	return NULL;
}

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

	/* Pure body. A `func` returns EXACTLY ONE value, so only a SINGLE-out pure proc could be a func;
	 * a multi-out pure proc is legitimately a (multi-return) proc — no lint. `main` is the entry point,
	 * never nudged toward `func`. A zero-out pure proc does nothing observable — flag for removal. The
	 * purity test is the SAME predicate enforce_func_purity uses, so "could be a func" means exactly
	 * "would compile as a func". */
	int is_main = proc->name && strcmp(proc->name, "main") == 0;
	if (proc->out_param_count == 1 && !is_main) {
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
			else if (is_static_name(ctx, nm))
				rr = "reads a mutable global";
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
	if (!func || func->is_extern)
		return;
	const char *reason = func_purity_body_view(ctx, func->body_node);
	if (reason) {
		sem_emit_func_not_pure(ctx, func->loc, func->name ? func->name : "<unknown>", reason);
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
		TypeRef *pt = p->type;
		reject_meta_type(ctx, pt, "parameter type");
		if (proc->is_extern) {
			if (pt && pt->kind == TYPE_NAME) {
				const char *tname = pt->data.name;
				if (!is_primitive_type_name(tname) && !find_archetype(ctx, tname) && !is_type_alias(ctx, tname)) {
					sem_emit_extern_proc_bad_type(ctx, pt->loc, tname, proc->name);
				}
			}
			/* An extern is assumed to mutate every in-param (no body to verify). A mutated borrow
			 * breaks the read-only contract, so a by-ref array in-param must be `own` — UNLESS an
			 * out-param shadows it (in-out), in which case the write targets the out place. */
			if (type_is_byref_aggregate(pt) && !p->is_own && !proc_param_is_inout(proc, i)) {
				sem_emit_extern_array_param_needs_own(ctx, p->loc, p->name ? p->name : "?", proc->name);
			}
			/* `consume` is valid on any param type (consume consumes — not opaque-special). */
		}
	}

	/* For extern procs, validate the out-param types too (parity with extern func return). An
	 * out-only out-param maps to the C return value; an in-out one to an in-place pointer write. */
	if (proc->is_extern) {
		for (int i = 0; i < proc->out_param_count; i++) {
			TypeRef *rt = proc->out_params[i].type;
			if (rt && rt->kind == TYPE_NAME) {
				const char *tname = rt->data.name;
				if (!is_primitive_type_name(tname) && !find_archetype(ctx, tname) && !is_type_alias(ctx, tname)) {
					sem_emit_extern_proc_bad_return(ctx, rt->loc, tname, proc->name);
				}
			}
		}
		return;
	}

	/* Validate `archetype` parameter constraints: at most one per proc. */
	int archetype_param_count = 0;
	for (int i = 0; i < proc->param_count; i++) {
		if (proc->params[i].type && proc->params[i].type->kind == TYPE_ARCHETYPE) {
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
		TypeRef *param_type = proc->params[i].type;

		/* Check if param type is an archetype */
		const char *type_name = (param_type && param_type->kind == TYPE_NAME) ? param_type->data.name : NULL;
		const char *arch_name = NULL;
		if (type_name && find_archetype(ctx, type_name)) {
			arch_name = type_name;
		}

		if (arch_name) {
			add_variable_with_archetype(ctx, param_name, param_type, arch_name);
		} else {
			add_variable(ctx, param_name, param_type);
		}
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
		add_variable(ctx, on, proc->out_params[i].type);
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
			if (field && field->type && field->type->kind == TYPE_HANDLE) {
				sem_emit_handle_in_sys_param(ctx, sys->params[p].loc, sys->params[p].name);
			}
		}
	}

	/* add parameters as variables, using field types from archetype if available */
	for (int i = 0; i < sys->param_count; i++) {
		TypeRef *param_type = sys->params[i].type;
		reject_meta_type(ctx, param_type, "sys parameter type");
		/* If no explicit type and we found the archetype, use the field's type */
		if (!param_type && arch_info) {
			FieldInfo *field = find_field(arch_info, sys->params[i].name);
			if (field) {
				param_type = field->type;
			}
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
				if (!type_ref_equal(resolved[i]->params[k].type, resolved[j]->params[k].type)) {
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
		reject_meta_type(ctx, func->params[i].type, "parameter type");
		if (func->params[i].type && func->params[i].type->kind == TYPE_ARCHETYPE) {
			sem_emit_archetype_funcs_only(ctx, func->params[i].type->loc, func->name);
			break;
		}
	}
	for (int i = 0; i < func->return_type_count; i++) {
		reject_meta_type(ctx, func->return_types[i], "return type");
		if (func->return_types[i] && func->return_types[i]->kind == TYPE_ARCHETYPE) {
			sem_emit_archetype_not_return_type(ctx, func->return_types[i]->loc, func->name);
			break;
		}
	}
	/* Validate parameters and return type for extern vs non-extern rules. */
	for (int i = 0; i < func->param_count; i++) {
		ParamSummary *p = &func->params[i];
		TypeRef *pt = p->type;
		if (func->is_extern) {
			if (pt && pt->kind == TYPE_NAME) {
				const char *tname = pt->data.name;
				if (!is_primitive_type_name(tname) && !is_type_alias(ctx, tname)) {
					sem_emit_extern_func_bad_type(ctx, pt->loc, tname, func->name);
				}
			}
			/* `consume` is valid on any param type (consume consumes — not opaque-special). */
		}
	}

	/* Validate return types: extern funcs must use known types. */
	if (func->is_extern) {
		for (int i = 0; i < func->return_type_count; i++) {
			TypeRef *rt = func->return_types[i];
			if (rt && rt->kind == TYPE_NAME) {
				const char *tname = rt->data.name;
				if (!is_primitive_type_name(tname) && !is_type_alias(ctx, tname)) {
					sem_emit_extern_func_bad_return(ctx, rt->loc, tname, func->name);
				}
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
		add_variable(ctx, func->params[i].name, func->params[i].type);
		mark_last_param(ctx, func->params[i].is_own);
	}

	DeclSummary *prev_func = ctx->current_func;
	ctx->current_func = func;
	for (int i = 0, n = sem_stmt_count(func->body_node); i < n; i++)
		analyze_statement(ctx, sem_stmt_at(func->body_node, i));
	ctx->current_func = prev_func;

	enforce_func_purity(ctx, func); /* a `func` must be pure — hard error if not */

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

/* ---- type reconstruction (syntax tree type node -> TypeRef) ---- */
static TypeRef *cst_build_type(SyntaxView t);
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

static TypeRef *cst_build_type(SyntaxView t) {
	if (!sv_present(t))
		return NULL;
	TypeRef *tr = malloc(sizeof(TypeRef));
	tr->loc.line = 0;
	tr->loc.column = 0;
	tr->is_transparent = 0;
	switch (sv_kind(t)) {
	case SN_TYPE_REF: {
		char *raw = sem_type_ref_name(t);
		if (strcmp(raw, "archetype") == 0) {
			tr->kind = TYPE_ARCHETYPE;
			free(raw);
		} else if (strcmp(raw, "opaque") == 0) {
			tr->kind = TYPE_OPAQUE;
			free(raw);
		} else if (strcmp(raw, "type") == 0) {
			tr->kind = TYPE_TYPE;
			free(raw);
		} else {
			tr->kind = TYPE_NAME;
			tr->data.name = raw;                               /* owned */
			tr->is_transparent = sem_type_ref_alias_marked(t); /* `:: alias T` → tier-1 transparent */
		}
		break;
	}
	case SN_TYPE_ARRAY: {
		tr->kind = TYPE_ARRAY;
		TypeRef *elem = malloc(sizeof(TypeRef));
		elem->loc = tr->loc;
		char *en = sem_txt_dup(sv_token(t, TOK_IDENT));
		if (strcmp(en, "opaque") == 0) {
			elem->kind = TYPE_OPAQUE;
			free(en);
		} else {
			elem->kind = TYPE_NAME;
			elem->data.name = en;
		}
		tr->data.array.element_type = elem;
		break;
	}
	case SN_TYPE_SHAPED_ARRAY: {
		/* `T[a][b]…` — innermost element is the named type; each `[n]` adds a rank. */
		char *en = sem_txt_dup(sv_token(t, TOK_IDENT));
		TypeRef *elem = malloc(sizeof(TypeRef));
		elem->loc = tr->loc;
		if (strcmp(en, "opaque") == 0) {
			elem->kind = TYPE_OPAQUE;
			free(en);
		} else {
			elem->kind = TYPE_NAME;
			elem->data.name = en;
		}
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
		TypeRef *cur = elem;
		for (int i = nr - 1; i >= 0; i--) {
			TypeRef *sh = malloc(sizeof(TypeRef));
			sh->loc = tr->loc;
			sh->kind = TYPE_SHAPED_ARRAY;
			sh->data.shaped_array.element_type = cur;
			sh->data.shaped_array.rank = ranks[i];
			cur = sh;
		}
		free(tr);
		return cur;
	}
	case SN_TYPE_HANDLE: {
		tr->kind = TYPE_HANDLE;
		tr->data.handle.archetype_name = syntax_handle_name(t);
		break;
	}
	case SN_TYPE_TUPLE: {
		/* `(x: T, y: U)` — field names are IDENTs, field types are the SN_TYPE_* children. */
		tr->kind = TYPE_TUPLE;
		int n = sv_type_count_sem(t);
		tr->data.tuple.field_count = n;
		tr->data.tuple.field_names = malloc((n > 0 ? n : 1) * sizeof(char *));
		tr->data.tuple.field_types = malloc((n > 0 ? n : 1) * sizeof(TypeRef *));
		/* field names: IDENT tokens preceding each `:`; collect in order */
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
					tr->data.tuple.field_names[fi] = sem_txt_dup((SynText){pend, pend ? pend_len : 0});
					tr->data.tuple.field_types[fi] = cst_build_type((SyntaxView){ch->as.node, t.src});
					fi++;
					pend = NULL;
				}
			}
		}
		tr->data.tuple.field_count = fi;
		break;
	}
	case SN_TYPE_PROC:
	case SN_TYPE_FUNC: {
		int is_proc = (sv_kind(t) == SN_TYPE_PROC);
		tr->kind = is_proc ? TYPE_PROC : TYPE_FUNC;
		tr->data.callable.is_proc = is_proc;
		int np = sv_count(t, SN_PARAM);
		tr->data.callable.param_count = np;
		tr->data.callable.param_types = malloc((np ? np : 1) * sizeof(TypeRef *));
		for (int i = 0; i < np; i++)
			tr->data.callable.param_types[i] = cst_build_type(sem_type_at(sv_child_at(t, SN_PARAM, i), 0));
		if (is_proc) {
			int no = sv_count(t, SN_OUT_PARAM);
			tr->data.callable.result_count = no;
			tr->data.callable.result_types = malloc((no ? no : 1) * sizeof(TypeRef *));
			for (int i = 0; i < no; i++)
				tr->data.callable.result_types[i] = cst_build_type(sem_type_at(sv_child_at(t, SN_OUT_PARAM, i), 0));
		} else {
			/* a func's single return is the SN_TYPE_FUNC's direct type-node child (params are in SN_PARAM) */
			tr->data.callable.result_count = 1;
			tr->data.callable.result_types = malloc(sizeof(TypeRef *));
			tr->data.callable.result_types[0] = cst_build_type(sem_type_at(t, 0));
		}
		break;
	}
	default:
		tr->kind = TYPE_NAME;
		tr->data.name = sem_dupz("");
		break;
	}
	return tr;
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
	default:
		return OP_NONE;
	}
}
static Operator sem_assign_op(TokenKind k) {
	switch (k) {
	case TOK_PLUS_EQ:
		return OP_ADD;
	case TOK_MINUS_EQ:
		return OP_SUB;
	case TOK_STAR_EQ:
		return OP_MUL;
	case TOK_SLASH_EQ:
		return OP_DIV;
	default:
		return OP_NONE;
	}
}

/* Decode a string literal's content (quotes + escapes), like parse_primary_expr. */
static char *syntax_decode_str(SynText raw, int *out_len) {
	const char *s = raw.ptr;
	size_t len = raw.len;
	char *value = malloc(len + 1);
	int p = 0;
	for (size_t i = 1; i + 1 < len; i++) {
		if (s[i] == '\\' && i + 2 < len) {
			i++;
			switch (s[i]) {
			case 'n':
				value[p++] = '\n';
				break;
			case 't':
				value[p++] = '\t';
				break;
			case 'r':
				value[p++] = '\r';
				break;
			case '\\':
				value[p++] = '\\';
				break;
			case '"':
				value[p++] = '"';
				break;
			default:
				value[p++] = s[i];
				break;
			}
		} else {
			value[p++] = s[i];
		}
	}
	value[p] = '\0';
	*out_len = p;
	return value;
}

/* Name string of an SN_NAME_EXPR view. `table<Name>` in value position resolves to the bare
 * archetype name (the 2nd IDENT); otherwise the sole IDENT. Caller frees. Shared by
 * cst_build_expr and the view-driven analysis so the two never disagree. */
static char *sv_name_expr_dup(SyntaxView e) {
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

/* ---- expression reconstruction ---- */
static Expression *cst_build_expr(SyntaxView e) {
	if (!sv_present(e))
		return NULL;
	Expression *ax = expression_create(EXPR_LITERAL);
	ax->cst_id = sv_id(e) + 1;
	ax->sn = e.node;
	ax->sn_src = e.src;
	ax->loc = sem_node_loc(e.node);

	switch (sv_kind(e)) {
	case SN_PAREN_EXPR: {
		Expression *inner = cst_build_expr(sem_first_expr(e));
		expression_free(ax);
		return inner;
	}
	case SN_LITERAL_EXPR:
		ax->type = EXPR_LITERAL;
		ax->data.literal.lexeme = sem_cv_dup_first_token(e);
		break;
	case SN_STRING_EXPR: {
		ax->type = EXPR_STRING;
		int n = 0;
		ax->data.string.value = syntax_decode_str(sv_text(e), &n);
		ax->data.string.length = n;
		break;
	}
	case SN_NAME_EXPR: {
		ax->type = EXPR_NAME;
		ax->data.name.is_table_ref = sv_has_token(e, TOK_LT);
		ax->data.name.name = sv_name_expr_dup(e);
		break;
	}
	case SN_FIELD_EXPR: {
		/* `base.f1.f2…[idx]` flat: base IDENT, then (DOT FIELD_NAME)+, optional trailing index. The
		 * sub-expr nodes are built directly here (not via cst_build_expr), so propagate the source
		 * loc onto each one — otherwise diagnostics on them (undefined base, missing member) report
		 * at line 1,1. */
		SourceLoc floc = ax->loc;
		Expression *base = expression_create(EXPR_NAME);
		base->loc = floc;
		base->data.name.name = sem_txt_dup(sv_token(e, TOK_IDENT));
		base->data.name.is_table_ref = 0;
		Expression *cur = base;
		int nfields = sv_count(e, SN_FIELD_NAME);
		for (int i = 0; i < nfields; i++) {
			Expression *f = expression_create(EXPR_FIELD);
			f->loc = floc;
			f->data.field.base = cur;
			f->data.field.field_name = sem_cv_dup(sv_child_at(e, SN_FIELD_NAME, i));
			cur = f;
		}
		if (sv_has_token(e, TOK_LBRACKET)) {
			Expression *idx = expression_create(EXPR_INDEX);
			idx->loc = floc;
			idx->data.index.base = cur;
			int ic = 0;
			for (int i = 0; i < e.node->child_count; i++)
				if (e.node->children[i].tag == SE_NODE) {
					SyntaxNodeKind k = e.node->children[i].as.node->kind;
					if (k >= SN_LITERAL_EXPR && k <= SN_PAREN_EXPR)
						ic++;
				}
			idx->data.index.indices = calloc(ic ? ic : 1, sizeof(Expression *));
			idx->data.index.index_count = 0;
			for (int i = 0; i < e.node->child_count; i++)
				if (e.node->children[i].tag == SE_NODE) {
					SyntaxNodeKind k = e.node->children[i].as.node->kind;
					if (k >= SN_LITERAL_EXPR && k <= SN_PAREN_EXPR)
						idx->data.index.indices[idx->data.index.index_count++] =
						    cst_build_expr((SyntaxView){e.node->children[i].as.node, e.src});
				}
			cur = idx;
		}
		cur->cst_id = sv_id(e) + 1;
		cur->sn = e.node; /* AST-kill bridge: the returned field node is `cur`, not `ax` */
		cur->sn_src = e.src;
		expression_free(ax);
		return cur;
	}
	case SN_INDEX_EXPR: {
		ax->type = EXPR_INDEX;
		Expression *base = expression_create(EXPR_NAME);
		base->loc = ax->loc;
		base->data.name.name = sem_txt_dup(sv_token(e, TOK_IDENT));
		base->data.name.is_table_ref = 0;
		int nfields = sv_count(e, SN_FIELD_NAME);
		for (int i = 0; i < nfields; i++) {
			Expression *f = expression_create(EXPR_FIELD);
			f->loc = ax->loc;
			f->data.field.base = base;
			f->data.field.field_name = sem_cv_dup(sv_child_at(e, SN_FIELD_NAME, i));
			base = f;
		}
		ax->data.index.base = base;
		int ic = 0;
		for (int i = 0; i < e.node->child_count; i++)
			if (e.node->children[i].tag == SE_NODE) {
				SyntaxNodeKind k = e.node->children[i].as.node->kind;
				if (k >= SN_LITERAL_EXPR && k <= SN_PAREN_EXPR)
					ic++;
			}
		ax->data.index.indices = calloc(ic ? ic : 1, sizeof(Expression *));
		ax->data.index.index_count = 0;
		for (int i = 0; i < e.node->child_count; i++)
			if (e.node->children[i].tag == SE_NODE) {
				SyntaxNodeKind k = e.node->children[i].as.node->kind;
				if (k >= SN_LITERAL_EXPR && k <= SN_PAREN_EXPR)
					ax->data.index.indices[ax->data.index.index_count++] =
					    cst_build_expr((SyntaxView){e.node->children[i].as.node, e.src});
			}
		break;
	}
	case SN_SLICE_EXPR: {
		/* `base[lo:hi]` — base is IDENT + fields (as for index); the expr child(ren) split on the
		 * `:` token: before → lo, after → hi (either may be absent). */
		ax->type = EXPR_SLICE;
		Expression *base = expression_create(EXPR_NAME);
		base->loc = ax->loc;
		base->data.name.name = sem_txt_dup(sv_token(e, TOK_IDENT));
		base->data.name.is_table_ref = 0;
		int nfields = sv_count(e, SN_FIELD_NAME);
		for (int i = 0; i < nfields; i++) {
			Expression *f = expression_create(EXPR_FIELD);
			f->loc = ax->loc;
			f->data.field.base = base;
			f->data.field.field_name = sem_cv_dup(sv_child_at(e, SN_FIELD_NAME, i));
			base = f;
		}
		ax->data.slice.base = base;
		ax->data.slice.lo = NULL;
		ax->data.slice.hi = NULL;
		int seen_colon = 0;
		for (int i = 0; i < e.node->child_count; i++) {
			SyntaxElem *ch = &e.node->children[i];
			if (ch->tag == SE_TOKEN && ch->as.token.kind == TOK_COLON) {
				seen_colon = 1;
				continue;
			}
			if (ch->tag == SE_NODE) {
				SyntaxNodeKind k = ch->as.node->kind;
				if (k >= SN_LITERAL_EXPR && k <= SN_PAREN_EXPR) {
					Expression *ex = cst_build_expr((SyntaxView){ch->as.node, e.src});
					if (!seen_colon)
						ax->data.slice.lo = ex;
					else
						ax->data.slice.hi = ex;
				}
			}
		}
		break;
	}
	case SN_BINARY_EXPR: {
		ax->type = EXPR_BINARY;
		for (int i = 0; i < e.node->child_count; i++)
			if (e.node->children[i].tag == SE_TOKEN) {
				Operator op = sem_tok_to_op(e.node->children[i].as.token.kind);
				if (op != OP_NONE) {
					ax->data.binary.op = op;
					break;
				}
			}
		ax->data.binary.left = cst_build_expr(sem_node_at_expr(e, 0));
		ax->data.binary.right = cst_build_expr(sem_node_at_expr(e, 1));
		break;
	}
	case SN_UNARY_EXPR: {
		ax->type = EXPR_UNARY;
		ax->data.unary.op = UNARY_NEG;
		for (int i = 0; i < e.node->child_count; i++)
			if (e.node->children[i].tag == SE_TOKEN) {
				TokenKind tk = e.node->children[i].as.token.kind;
				if (tk == TOK_MINUS)
					ax->data.unary.op = UNARY_NEG;
				else if (tk == TOK_BANG)
					ax->data.unary.op = UNARY_NOT;
				else if (tk == TOK_MOVE)
					ax->data.unary.op = UNARY_MOVE;
				else if (tk == TOK_COPY)
					ax->data.unary.op = UNARY_COPY;
				else
					continue;
				break;
			}
		ax->data.unary.operand = cst_build_expr(sem_first_expr(e));
		break;
	}
	case SN_CALL_EXPR: {
		ax->type = EXPR_CALL;
		Expression *callee;
		int callee_nfields = sv_count(e, SN_FIELD_NAME);
		if (callee_nfields > 0) {
			/* Qualified callee `mod.name`: rebuild the field access; the qualify pass binds it to the
			 * member's identity. Propagate loc so a `module has no member` diagnostic locates right. */
			Expression *base = expression_create(EXPR_NAME);
			base->loc = ax->loc;
			base->data.name.name = sem_txt_dup(sv_token(e, TOK_IDENT));
			base->data.name.is_table_ref = 0;
			Expression *cur = base;
			for (int i = 0; i < callee_nfields; i++) {
				Expression *f = expression_create(EXPR_FIELD);
				f->loc = ax->loc;
				f->data.field.base = cur;
				f->data.field.field_name = sem_cv_dup(sv_child_at(e, SN_FIELD_NAME, i));
				cur = f;
			}
			callee = cur;
		} else {
			callee = expression_create(EXPR_NAME);
			callee->loc = ax->loc;
			callee->data.name.name = sem_cv_dup(sv_child(e, SN_CALLEE_NAME));
			callee->data.name.is_table_ref = 0;
		}
		ax->data.call.callee = callee;
		int ac = 0;
		for (int i = 0; i < e.node->child_count; i++)
			if (e.node->children[i].tag == SE_NODE) {
				SyntaxNodeKind k = e.node->children[i].as.node->kind;
				if (k >= SN_LITERAL_EXPR && k <= SN_PAREN_EXPR)
					ac++;
			}
		ax->data.call.args = calloc(ac ? ac : 1, sizeof(Expression *));
		ax->data.call.arg_count = 0;
		for (int i = 0; i < e.node->child_count; i++)
			if (e.node->children[i].tag == SE_NODE) {
				SyntaxNodeKind k = e.node->children[i].as.node->kind;
				if (k >= SN_LITERAL_EXPR && k <= SN_PAREN_EXPR)
					ax->data.call.args[ax->data.call.arg_count++] =
					    cst_build_expr((SyntaxView){e.node->children[i].as.node, e.src});
			}
		break;
	}
	case SN_ARRAY_LIT_EXPR: {
		ax->type = EXPR_ARRAY_LITERAL;
		int n = 0;
		for (int i = 0; i < e.node->child_count; i++)
			if (e.node->children[i].tag == SE_NODE) {
				SyntaxNodeKind k = e.node->children[i].as.node->kind;
				if (k >= SN_LITERAL_EXPR && k <= SN_PAREN_EXPR)
					n++;
			}
		ax->data.array_literal.elements = calloc(n ? n : 1, sizeof(Expression *));
		ax->data.array_literal.element_count = 0;
		for (int i = 0; i < e.node->child_count; i++)
			if (e.node->children[i].tag == SE_NODE) {
				SyntaxNodeKind k = e.node->children[i].as.node->kind;
				if (k >= SN_LITERAL_EXPR && k <= SN_PAREN_EXPR)
					ax->data.array_literal.elements[ax->data.array_literal.element_count++] =
					    cst_build_expr((SyntaxView){e.node->children[i].as.node, e.src});
			}
		break;
	}
	default:
		ax->type = EXPR_LITERAL;
		ax->data.literal.lexeme = sem_cv_dup(e);
		break;
	}
	return ax;
}

/* ---- statement reconstruction ---- */
static Statement *cst_build_stmt(SyntaxView s);

/* Build statements from the direct children of `parent` whose child index is in [lo, hi).
 * Used to split an if's flat then/else child list on the `else` token. */
static Statement **cst_build_body_split(SyntaxView parent, int lo, int hi, int *out_count) {
	if (lo < 0)
		lo = 0;
	if (hi > parent.node->child_count)
		hi = parent.node->child_count;
	int n = 0;
	for (int i = lo; i < hi; i++)
		if (parent.node->children[i].tag == SE_NODE) {
			SyntaxNodeKind k = parent.node->children[i].as.node->kind;
			if (k >= SN_BIND_STMT && k <= SN_MATCH_STMT)
				n++;
		}
	*out_count = n;
	if (n == 0)
		return NULL;
	Statement **out = calloc(n, sizeof(Statement *));
	int j = 0;
	for (int i = lo; i < hi; i++)
		if (parent.node->children[i].tag == SE_NODE) {
			SyntaxNodeKind k = parent.node->children[i].as.node->kind;
			if (k >= SN_BIND_STMT && k <= SN_MATCH_STMT)
				out[j++] = cst_build_stmt((SyntaxView){parent.node->children[i].as.node, parent.src});
		}
	return out;
}

/* Lower the statement-kind child nodes of `parent` into a Statement array. */
static Statement **cst_build_body(SyntaxView parent, int *out_count) {
	int n = 0;
	for (int i = 0; i < parent.node->child_count; i++)
		if (parent.node->children[i].tag == SE_NODE) {
			SyntaxNodeKind k = parent.node->children[i].as.node->kind;
			if (k >= SN_BIND_STMT && k <= SN_MATCH_STMT)
				n++;
		}
	*out_count = n;
	if (n == 0)
		return NULL;
	Statement **out = calloc(n, sizeof(Statement *));
	int j = 0;
	for (int i = 0; i < parent.node->child_count; i++)
		if (parent.node->children[i].tag == SE_NODE) {
			SyntaxNodeKind k = parent.node->children[i].as.node->kind;
			if (k >= SN_BIND_STMT && k <= SN_MATCH_STMT)
				out[j++] = cst_build_stmt((SyntaxView){parent.node->children[i].as.node, parent.src});
		}
	return out;
}

static Statement *cst_build_stmt(SyntaxView s) {
	Statement *as = statement_create(STMT_EXPR);
	as->cst_id = sv_id(s) + 1;
	as->sn = s.node;
	as->sn_src = s.src;
	as->loc = sem_node_loc(s.node);

	switch (sv_kind(s)) {
	case SN_BIND_STMT: {
		as->type = STMT_BIND;
		SyntaxView target = sem_node_at_expr(s, 0);
		as->data.bind_stmt.name = sem_cv_dup(target);
		as->data.bind_stmt.names = NULL;
		as->data.bind_stmt.name_count = 0;
		as->data.bind_stmt.type = NULL;
		as->data.bind_stmt.value = NULL;
		as->data.bind_stmt.type_value = NULL;
		as->data.bind_stmt.is_type_alias = 0;
		/* `:` introduces the value ⇒ constant; `=` ⇒ variable. The bind node holds the
		 * separator tokens directly: `x := e` / `x : T = e` have an `=`; `x :: e` /
		 * `x : T : e` use only `:` (>=2 of them). Mirror parse_binding_tail's classification. */
		int n_colon = 0, has_eq = 0;
		for (int i = 0; i < s.node->child_count; i++)
			if (s.node->children[i].tag == SE_TOKEN) {
				TokenKind tk = s.node->children[i].as.token.kind;
				if (tk == TOK_COLON)
					n_colon++;
				else if (tk == TOK_EQ)
					has_eq = 1;
			}
		int is_const = (!has_eq && n_colon >= 2);
		as->data.bind_stmt.is_const = is_const;
		/* The first type node (if any) is the declared type T. For `x : type : <T>` the parser
		 * keeps type=NULL, type_value=<T>; detect that via the meta-type `type` as the first
		 * type node, with a second type node as the alias backing. */
		SyntaxView t0 = sem_type_at(s, 0);
		SyntaxView t1 = sem_type_at(s, 1);
		SynText t0name = sv_present(t0) ? sv_token(t0, TOK_IDENT) : (SynText){NULL, 0};
		int t0_is_meta = t0name.ptr && t0name.len == 4 && memcmp(t0name.ptr, "type", 4) == 0;
		if (is_const && t0_is_meta && sv_present(t1)) {
			/* `x : type : <T>` — local type alias; RHS is a type. */
			as->data.bind_stmt.type_value = cst_build_type(t1);
		} else {
			as->data.bind_stmt.type = cst_build_type(t0);
			as->data.bind_stmt.value = cst_build_expr(sem_node_at_expr(s, 1));
		}
		break;
	}
	case SN_ASSIGN_STMT: {
		as->type = STMT_ASSIGN;
		as->data.assign_stmt.op = OP_NONE;
		for (int i = 0; i < s.node->child_count; i++)
			if (s.node->children[i].tag == SE_TOKEN) {
				TokenKind tk = s.node->children[i].as.token.kind;
				if (tk == TOK_EQ || tk == TOK_PLUS_EQ || tk == TOK_MINUS_EQ || tk == TOK_STAR_EQ ||
				    tk == TOK_SLASH_EQ) {
					as->data.assign_stmt.op = sem_assign_op(tk);
					break;
				}
			}
		as->data.assign_stmt.target = cst_build_expr(sem_node_at_expr(s, 0));
		as->data.assign_stmt.value = cst_build_expr(sem_node_at_expr(s, 1));
		break;
	}
	case SN_EXPR_STMT:
		as->type = STMT_EXPR;
		as->data.expr_stmt.expr = cst_build_expr(sem_node_at_expr(s, 0));
		break;
	case SN_BREAK_STMT:
		as->type = STMT_BREAK;
		break;
	case SN_CONTINUE_STMT:
		as->type = STMT_CONTINUE;
		break;
	case SN_RUN_STMT: {
		as->type = STMT_RUN;
		/* `run sys` / `run device.system`: `run` is a keyword, so the only IDENTs are the
		 * (possibly qualified) system-name segments — join them with `.` (mirrors lower.c). */
		char namebuf[256];
		int nl = 0;
		for (int i = 0; i < s.node->child_count; i++)
			if (s.node->children[i].tag == SE_TOKEN && s.node->children[i].as.token.kind == TOK_IDENT) {
				if (nl > 0 && nl < (int)sizeof(namebuf) - 1)
					namebuf[nl++] = '.';
				int seg = (int)s.node->children[i].as.token.length;
				for (int k = 0; k < seg && nl < (int)sizeof(namebuf) - 1; k++)
					namebuf[nl++] = s.src[s.node->children[i].as.token.offset + k];
			}
		namebuf[nl] = '\0';
		as->data.run_stmt.system_name = sem_dupz(namebuf);
		as->data.run_stmt.world_name = NULL;
		break;
	}
	case SN_RETURN_STMT: {
		as->type = STMT_RETURN;
		int c = 0;
		for (int i = 0; i < s.node->child_count; i++)
			if (s.node->children[i].tag == SE_NODE) {
				SyntaxNodeKind k = s.node->children[i].as.node->kind;
				if (k >= SN_LITERAL_EXPR && k <= SN_PAREN_EXPR)
					c++;
			}
		as->data.return_stmt.count = c;
		as->data.return_stmt.values = calloc(c ? c : 1, sizeof(Expression *));
		for (int i = 0; i < c; i++)
			as->data.return_stmt.values[i] = cst_build_expr(sem_node_at_expr(s, i));
		break;
	}
	case SN_IF_STMT: {
		as->type = STMT_IF;
		as->data.if_stmt.cond = cst_build_expr(sem_node_at_expr(s, 0));
		/* The parser emits the if's then- and else-statements as a FLAT child list separated by
		 * the `else` token (no wrapper node). Split on that token: statement children before it
		 * are the then-body, those after are the else-body. */
		int else_tok = -1;
		for (int i = 0; i < s.node->child_count; i++)
			if (s.node->children[i].tag == SE_TOKEN && s.node->children[i].as.token.kind == TOK_ELSE) {
				else_tok = i;
				break;
			}
		as->data.if_stmt.then_body =
		    cst_build_body_split(s, 0, else_tok >= 0 ? else_tok : s.node->child_count, &as->data.if_stmt.then_count);
		as->data.if_stmt.else_body = NULL;
		as->data.if_stmt.else_count = 0;
		if (else_tok >= 0)
			as->data.if_stmt.else_body =
			    cst_build_body_split(s, else_tok, s.node->child_count, &as->data.if_stmt.else_count);
		break;
	}
	case SN_FOR_STMT: {
		as->type = STMT_FOR;
		as->data.for_stmt.var_name = NULL;
		as->data.for_stmt.iterable = NULL;
		as->data.for_stmt.init = NULL;
		as->data.for_stmt.condition = NULL;
		as->data.for_stmt.increment = NULL;
		as->data.for_stmt.body = NULL;
		as->data.for_stmt.body_count = 0;
		if (sv_has_token(s, TOK_LPAREN)) {
			/* C-style: `for ( init ; cond ; incr ) { body }` */
			int seen_brace = 0, seg = 0, nbody = 0;
			for (int i = 0; i < s.node->child_count; i++) {
				SyntaxElem *ch = &s.node->children[i];
				if (ch->tag == SE_TOKEN) {
					if (ch->as.token.kind == TOK_LBRACE)
						seen_brace = 1;
					else if (ch->as.token.kind == TOK_SEMI && !seen_brace && seg < 2)
						seg++;
					continue;
				}
				SyntaxNodeKind k = ch->as.node->kind;
				SyntaxView cv = {ch->as.node, s.src};
				if (seen_brace) {
					if (k >= SN_BIND_STMT && k <= SN_MATCH_STMT)
						nbody++;
					continue;
				}
				if (seg == 0 && k >= SN_BIND_STMT && k <= SN_MATCH_STMT)
					as->data.for_stmt.init = cst_build_stmt(cv);
				else if (seg == 1 && k >= SN_LITERAL_EXPR && k <= SN_PAREN_EXPR)
					as->data.for_stmt.condition = cst_build_expr(cv);
				else if (seg == 2 && k >= SN_BIND_STMT && k <= SN_MATCH_STMT)
					as->data.for_stmt.increment = cst_build_stmt(cv);
			}
			as->data.for_stmt.body = calloc(nbody ? nbody : 1, sizeof(Statement *));
			seen_brace = 0;
			for (int i = 0; i < s.node->child_count; i++) {
				SyntaxElem *ch = &s.node->children[i];
				if (ch->tag == SE_TOKEN) {
					if (ch->as.token.kind == TOK_LBRACE)
						seen_brace = 1;
					continue;
				}
				if (!seen_brace)
					continue;
				SyntaxNodeKind k = ch->as.node->kind;
				if (k >= SN_BIND_STMT && k <= SN_MATCH_STMT)
					as->data.for_stmt.body[as->data.for_stmt.body_count++] =
					    cst_build_stmt((SyntaxView){ch->as.node, s.src});
			}
			break;
		}
		/* range form `for IDENT in IDENT { body }`, or infinite `for { body }`. */
		int ni = 0;
		char *vname = NULL, *iname = NULL;
		for (int i = 0; i < s.node->child_count; i++)
			if (s.node->children[i].tag == SE_TOKEN && s.node->children[i].as.token.kind == TOK_IDENT) {
				SynText t = {s.src + s.node->children[i].as.token.offset, s.node->children[i].as.token.length};
				if (ni == 0)
					vname = sem_txt_dup(t);
				else if (ni == 1)
					iname = sem_txt_dup(t);
				ni++;
			}
		as->data.for_stmt.var_name = vname;
		if (iname) {
			Expression *it = expression_create(EXPR_NAME);
			it->data.name.name = iname;
			it->data.name.is_table_ref = 0;
			as->data.for_stmt.iterable = it;
		}
		as->data.for_stmt.body = cst_build_body(s, &as->data.for_stmt.body_count);
		break;
	}
	case SN_EACH_FIELD_STMT: {
		as->type = STMT_EACH_FIELD;
		as->data.each_field.binding_name = NULL;
		as->data.each_field.arch_param_name = NULL;
		int ni = 0;
		for (int i = 0; i < s.node->child_count; i++)
			if (s.node->children[i].tag == SE_TOKEN && s.node->children[i].as.token.kind == TOK_IDENT) {
				SynText t = {s.src + s.node->children[i].as.token.offset, s.node->children[i].as.token.length};
				if (ni == 0)
					as->data.each_field.binding_name = sem_txt_dup(t);
				else if (ni == 1)
					as->data.each_field.arch_param_name = sem_txt_dup(t);
				ni++;
			}
		if (!as->data.each_field.binding_name)
			as->data.each_field.binding_name = sem_dupz("");
		if (!as->data.each_field.arch_param_name)
			as->data.each_field.arch_param_name = sem_dupz("");
		as->data.each_field.filter_type = cst_build_type(sem_type_at(s, 0));
		as->data.each_field.body = cst_build_body(s, &as->data.each_field.body_count);
		break;
	}
	case SN_MULTI_BIND_STMT: {
		as->type = STMT_MULTI_BIND;
		int eq_idx = -1, lparen_idx = -1;
		for (int i = 0; i < s.node->child_count; i++)
			if (s.node->children[i].tag == SE_TOKEN) {
				TokenKind tk = s.node->children[i].as.token.kind;
				if (tk == TOK_LPAREN && lparen_idx < 0)
					lparen_idx = i;
				if (tk == TOK_EQ) {
					eq_idx = i;
					break;
				}
			}
		int paren = (lparen_idx >= 0 && lparen_idx < eq_idx);
		as->data.multi_bind.from_shorthand = paren ? 0 : 1;
		as->data.multi_bind.targets = calloc(s.node->child_count, sizeof(BindingTarget));
		as->data.multi_bind.target_count = 0;
		as->data.multi_bind.value = NULL;
		const char *pend = NULL;
		int pend_len = 0, pend_active = 0, pend_new = 0;
		TypeRef *pend_type = NULL;
#define SEM_MB_FLUSH()                                                                                                 \
	do {                                                                                                               \
		if (pend_active) {                                                                                             \
			int ti = as->data.multi_bind.target_count++;                                                               \
			as->data.multi_bind.targets[ti].name = sem_txt_dup((SynText){pend, pend_len});                             \
			as->data.multi_bind.targets[ti].is_new = paren ? pend_new : 1;                                             \
			as->data.multi_bind.targets[ti].type = pend_type;                                                          \
			pend_active = 0;                                                                                           \
			pend_new = 0;                                                                                              \
			pend_type = NULL;                                                                                          \
		}                                                                                                              \
	} while (0)
		for (int i = 0; i < eq_idx; i++) {
			SyntaxElem *ch = &s.node->children[i];
			if (ch->tag == SE_NODE) {
				SyntaxNodeKind k = ch->as.node->kind;
				if (k == SN_NAME_EXPR) {
					SEM_MB_FLUSH();
					SynText t = sv_token((SyntaxView){ch->as.node, s.src}, TOK_IDENT);
					pend = t.ptr;
					pend_len = (int)t.len;
					pend_active = 1;
				} else if (k >= SN_TYPE_REF && k <= SN_TYPE_FUNC && pend_active) {
					pend_type = cst_build_type((SyntaxView){ch->as.node, s.src});
				}
				continue;
			}
			TokenKind tk = ch->as.token.kind;
			if (tk == TOK_IDENT) {
				SEM_MB_FLUSH();
				pend = s.src + ch->as.token.offset;
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
		for (int i = eq_idx + 1; i < s.node->child_count; i++)
			if (s.node->children[i].tag == SE_NODE) {
				SyntaxNodeKind k = s.node->children[i].as.node->kind;
				if (k >= SN_LITERAL_EXPR && k <= SN_PAREN_EXPR) {
					as->data.multi_bind.value = cst_build_expr((SyntaxView){s.node->children[i].as.node, s.src});
					break;
				}
			}
		break;
	}
	case SN_PROC_CALL_STMT: {
		/* `foo(in)(out)` — modeled as a multi-bind whose value is the call `foo(in)` and whose
		 * targets are the out-args. Codegen passes the targets' addresses as the proc's out-pointers.
		 * An out-arg is `name` (existing place), `name:` (declare, type inferred from the out-param),
		 * or `name: T` (declare, typed). */
		as->type = STMT_MULTI_BIND;
		as->data.multi_bind.from_shorthand = 0;
		int nout = 0;
		for (int i = 0; i < s.node->child_count; i++)
			if (s.node->children[i].tag == SE_NODE && s.node->children[i].as.node->kind == SN_OUT_ARG)
				nout++;
		as->data.multi_bind.targets = calloc(nout ? nout : 1, sizeof(BindingTarget));
		as->data.multi_bind.target_count = 0;
		as->data.multi_bind.value = NULL;
		for (int i = 0; i < s.node->child_count; i++) {
			if (s.node->children[i].tag != SE_NODE)
				continue;
			SyntaxNode *cn = s.node->children[i].as.node;
			SyntaxView cnv = (SyntaxView){cn, s.src};
			if (cn->kind == SN_CALL_EXPR) {
				as->data.multi_bind.value = cst_build_expr(cnv);
			} else if (cn->kind == SN_OUT_ARG) {
				int ti = as->data.multi_bind.target_count++;
				as->data.multi_bind.targets[ti].name = sem_txt_dup(sv_token(cnv, TOK_IDENT));
				as->data.multi_bind.targets[ti].is_new = sv_has_token(cnv, TOK_COLON);
				as->data.multi_bind.targets[ti].type =
				    sv_type_count_sem(cnv) > 0 ? cst_build_type(sem_type_at(cnv, 0)) : NULL;
			}
		}
		break;
	}
	case SN_BLOCK: {
		/* A standalone `{ … }` block is a nested scope. Analyze-only: model it as `if (1) { body }` so
		 * every semantic pass VISITS the block's statements — without this a block was an empty STMT_EXPR
		 * and its inner exprs went untyped (a name arg defaulted to i32). Lower handles the real scope. */
		as->type = STMT_IF;
		Expression *one = expression_create(EXPR_LITERAL);
		one->data.literal.lexeme = sem_dupz("1");
		as->data.if_stmt.cond = one;
		as->data.if_stmt.then_body = cst_build_body(s, &as->data.if_stmt.then_count);
		as->data.if_stmt.else_body = NULL;
		as->data.if_stmt.else_count = 0;
		break;
	}
	case SN_MATCH_STMT: {
		/* Analyze-only desugar: `match scrut { p0: b0; p1: b1; … }` → an if-chain
		 *   if (scrut) { b0 } else if (scrut) { b1 } …
		 * The condition is the scrutinee itself, NOT a real pattern comparison — this exists purely so
		 * every semantic pass (type annotation, tycheck, RAII, name resolution) VISITS the scrutinee and
		 * each arm's body. Pattern matching + exhaustiveness live elsewhere (lower desugars for codegen;
		 * walk_matches checks exhaustiveness on the syntax tree). Without this a match was an empty STMT_EXPR, so
		 * arm-body exprs were never typed (opaque/float args defaulted to i32) and a local used only in
		 * an arm drew a false unused-local lint. */
		SyntaxView scrut = sem_node_at_expr(s, 0);
		int narm = sv_count(s, SN_MATCH_ARM);
		if (narm == 0) {
			as->type = STMT_EXPR;
			as->data.expr_stmt.expr = cst_build_expr(scrut);
			break;
		}
		Statement *chain = NULL; /* the else-body built so far (one nested if) */
		for (int i = narm - 1; i >= 0; i--) {
			SyntaxView arm = sv_child_at(s, SN_MATCH_ARM, i);
			int bc = 0;
			Statement **body = cst_build_body(arm, &bc);
			Statement *iff = (i == 0) ? as : statement_create(STMT_IF);
			iff->type = STMT_IF;
			iff->data.if_stmt.cond = cst_build_expr(scrut);
			iff->data.if_stmt.then_body = body;
			iff->data.if_stmt.then_count = bc;
			if (chain) {
				iff->data.if_stmt.else_body = calloc(1, sizeof(Statement *));
				iff->data.if_stmt.else_body[0] = chain;
				iff->data.if_stmt.else_count = 1;
			} else {
				iff->data.if_stmt.else_body = NULL;
				iff->data.if_stmt.else_count = 0;
			}
			chain = iff;
		}
		break;
	}
	default:
		as->type = STMT_EXPR;
		as->data.expr_stmt.expr = NULL;
		break;
	}
	return as;
}

/* ---- parameter reconstruction ---- */
static Parameter *cst_build_param(SyntaxView p) {
	Parameter *ap = parameter_create(sem_cv_dup(sv_child(p, SN_PARAM_NAME)), NULL);
	ap->type = cst_build_type(sem_type_at(p, 0)); /* NULL for sys params */
	ap->is_own = sv_has_token(p, TOK_OWN);
	ap->loc.line = 0;
	ap->loc.column = 0;
	return ap;
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

/* ---- declaration reconstruction (syntax tree decl node -> Decl) ---- */
static Decl *cst_build_decl_inner(SyntaxView d);

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

static Decl *cst_build_decl(SyntaxView d) {
	Decl *ad = cst_build_decl_inner(d);
	if (ad) {
		ad->sn = d.node;
		ad->sn_src = d.src;
		syntax_extract_allow_slugs(d, &ad->allow_slugs, &ad->allow_slug_count);
		ad->is_drop = syntax_has_drop_decorator(d);
		ad->drop_type = syntax_drop_type(d);
	}
	return ad;
}

/* ===== Unified-grammar RHS value forms (P2 classification) =====
 * In the unified grammar a declaration is a binding `name :: <form>`. These helpers build the
 * abstract decl from the RHS value-form node `f` (an SN_PROC_EXPR / SN_FUNC_EXPR / …) with the
 * name taken from the binding LHS. The extraction mirrors the legacy keyword-led decl cases
 * below (which become dead code once old syntax is removed); the only differences are the name
 * source and that children are read from `f` rather than the decl node. `name` is owned. */

static Decl *build_proc_from(SyntaxView f, char *name) {
	Decl *ad = decl_create(DECL_PROC);
	ad->loc = sem_node_loc(f.node);
	ProcDecl *ap = proc_decl_create(name);
	ap->loc = sem_direct_token_loc(f.node, TOK_LPAREN);
	/* Foreign (FFI-bodied): a proc value-form with no `{` body block. The parser only emits a
	 * bodiless proc value-form inside a `#foreign` region (otherwise it's a proc type). */
	ap->is_extern = !sv_has_token(f, TOK_LBRACE);
	ap->is_variadic = sv_has_token(f, TOK_DOTDOTDOT);
	ap->allow_pure_proc = sv_has_token(f, TOK_AT);
	int np = sv_count(f, SN_PARAM);
	ap->params = calloc(np ? np : 1, sizeof(Parameter *));
	for (int i = 0; i < np; i++)
		ap->params[i] = cst_build_param(sv_child_at(f, SN_PARAM, i));
	ap->param_count = np;
	int nout = sv_count(f, SN_OUT_PARAM);
	ap->out_params = calloc(nout ? nout : 1, sizeof(Parameter *));
	for (int i = 0; i < nout; i++)
		ap->out_params[i] = cst_build_param(sv_child_at(f, SN_OUT_PARAM, i));
	ap->out_param_count = nout;
	ap->statements = cst_build_body(f, &ap->statement_count);
	ad->data.proc = ap;
	return ad;
}

static Decl *build_func_from(SyntaxView f, char *name) {
	Decl *ad = decl_create(DECL_FUNC);
	ad->loc = sem_node_loc(f.node);
	FuncDecl *af = func_decl_create(name);
	af->loc = sem_direct_token_loc(f.node, TOK_LPAREN);
	af->is_extern = 0; /* funcs are never foreign — FFI bodies are procs */
	af->is_variadic = sv_has_token(f, TOK_DOTDOTDOT);
	int np = sv_count(f, SN_PARAM);
	af->params = calloc(np ? np : 1, sizeof(Parameter *));
	for (int i = 0; i < np; i++)
		af->params[i] = cst_build_param(sv_child_at(f, SN_PARAM, i));
	af->param_count = np;
	int nt = sv_type_count_sem(f);
	af->return_types = calloc(nt ? nt : 1, sizeof(TypeRef *));
	af->return_type_count = 0;
	for (int i = 0; i < nt; i++)
		af->return_types[af->return_type_count++] = cst_build_type(sem_type_at(f, i));
	af->statements = cst_build_body(f, &af->statement_count);
	ad->data.func = af;
	return ad;
}

static Decl *build_sys_from(SyntaxView f, char *name) {
	Decl *ad = decl_create(DECL_SYS);
	ad->loc = sem_node_loc(f.node);
	SysDecl *as = sys_decl_create(name);
	as->loc = sem_direct_token_loc(f.node, TOK_LPAREN);
	int np = sv_count(f, SN_PARAM);
	as->params = calloc(np ? np : 1, sizeof(Parameter *));
	for (int i = 0; i < np; i++)
		as->params[i] = cst_build_param(sv_child_at(f, SN_PARAM, i));
	as->param_count = np;
	as->statements = cst_build_body(f, &as->statement_count);
	ad->data.sys = as;
	return ad;
}

static Decl *build_func_group_from(SyntaxView f, char *name) {
	Decl *ad = decl_create(DECL_FUNC_GROUP);
	ad->loc = sem_node_loc(f.node);
	FuncGroup *fg = func_group_create(name);
	fg->loc = sem_direct_token_loc(f.node, TOK_LBRACE);
	int nmem = 0;
	for (int i = 0; i < f.node->child_count; i++)
		if (f.node->children[i].tag == SE_TOKEN && f.node->children[i].as.token.kind == TOK_IDENT)
			nmem++;
	fg->member_names = calloc(nmem ? nmem : 1, sizeof(char *));
	fg->member_count = 0;
	for (int i = 0; i < f.node->child_count; i++)
		if (f.node->children[i].tag == SE_TOKEN && f.node->children[i].as.token.kind == TOK_IDENT) {
			SynText t = {f.src + f.node->children[i].as.token.offset, f.node->children[i].as.token.length};
			fg->member_names[fg->member_count++] = sem_txt_dup(t);
		}
	ad->data.func_group = fg;
	return ad;
}

static Decl *build_enum_from(SyntaxView f, char *name) {
	Decl *ad = decl_create(DECL_ENUM);
	ad->loc = sem_node_loc(f.node);
	EnumDecl *e = calloc(1, sizeof(EnumDecl));
	e->name = name;
	int nv = sv_count(f, SN_ENUM_VARIANT);
	e->variant_names = calloc(nv ? nv : 1, sizeof(char *));
	e->variant_values = calloc(nv ? nv : 1, sizeof(long));
	e->variant_count = 0;
	long next = 0;
	for (int i = 0; i < nv; i++) {
		SyntaxView v = sv_child_at(f, SN_ENUM_VARIANT, i);
		long val = next;
		for (int c = 0; c < v.node->child_count; c++)
			if (v.node->children[c].tag == SE_TOKEN && v.node->children[c].as.token.kind == TOK_NUMBER) {
				char buf[32];
				int l = (int)v.node->children[c].as.token.length;
				if (l > 31)
					l = 31;
				memcpy(buf, v.src + v.node->children[c].as.token.offset, l);
				buf[l] = '\0';
				val = atol(buf);
				break;
			}
		e->variant_names[e->variant_count] = sem_txt_dup(sv_token(v, TOK_IDENT));
		e->variant_values[e->variant_count] = val;
		e->variant_count++;
		next = val + 1;
	}
	ad->data.enum_decl = e;
	return ad;
}

static Decl *build_archetype_from(SyntaxView f, char *name) {
	Decl *ad = decl_create(DECL_ARCHETYPE);
	ArchetypeDecl *aa = archetype_decl_create(name);
	int nf = sv_count(f, SN_FIELD_NAME);
	aa->fields = calloc(nf > 0 ? nf : 1, sizeof(FieldDecl *));
	aa->field_count = 0;
	for (int i = 0; i < f.node->child_count; i++) {
		if (f.node->children[i].tag != SE_NODE || f.node->children[i].as.node->kind != SN_FIELD_NAME)
			continue;
		SyntaxView fn = {f.node->children[i].as.node, f.src};
		SyntaxView ty = {NULL, f.src};
		int meta_explicit = 0;
		for (int k = i + 1; k < f.node->child_count; k++) {
			if (f.node->children[k].tag == SE_TOKEN) {
				if (f.node->children[k].as.token.kind == TOK_IDENT && f.node->children[k].as.token.length == 4 &&
				    strncmp(f.src + f.node->children[k].as.token.offset, "type", 4) == 0)
					meta_explicit = 1;
				continue;
			}
			SyntaxNodeKind kk = f.node->children[k].as.node->kind;
			if (kk == SN_FIELD_NAME)
				break;
			if (kk >= SN_TYPE_REF && kk <= SN_TYPE_FUNC) {
				ty.node = f.node->children[k].as.node;
				break;
			}
		}
		char *fname = sem_cv_dup(fn);
		TypeRef *ft;
		if (sv_present(ty)) {
			ft = cst_build_type(ty);
		} else {
			ft = malloc(sizeof(TypeRef));
			ft->kind = TYPE_NAME;
			ft->loc.line = 0;
			ft->loc.column = 0;
			ft->data.name = sem_dupz(fname);
		}
		FieldDecl *fd = field_decl_create(FIELD_COLUMN, fname, ft);
		fd->meta_explicit = meta_explicit;
		aa->fields[aa->field_count++] = fd;
	}
	ad->data.archetype = aa;
	return ad;
}

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
		if (k == SN_PROC_EXPR || k == SN_FUNC_EXPR || k == SN_SYS_EXPR || k == SN_ARCH_EXPR || k == SN_GROUP_EXPR ||
		    k == SN_ENUM_EXPR || k == SN_TYPE_PROC || k == SN_TYPE_FUNC) {
			SyntaxView v = {d.node->children[i].as.node, d.src};
			return v;
		}
	}
	SyntaxView none = {NULL, d.src};
	return none;
}

static Decl *cst_build_decl_inner(SyntaxView d) {
	switch (sv_kind(d)) {
	case SN_USE_DECL:
		return NULL; /* modules are inlined separately */
	case SN_WORLD_DECL: {
		Decl *ad = decl_create(DECL_WORLD);
		ad->data.world = world_decl_create(sem_txt_dup(sv_token(d, TOK_IDENT)));
		return ad;
	}
	case SN_ARCHETYPE_DECL: {
		Decl *ad = decl_create(DECL_ARCHETYPE);
		ArchetypeDecl *aa = archetype_decl_create(sem_cv_dup(sv_child(d, SN_TYPE_DEF_NAME)));
		int nf = sv_count(d, SN_FIELD_NAME);
		aa->fields = calloc(nf > 0 ? nf : 1, sizeof(FieldDecl *));
		aa->field_count = 0;
		for (int i = 0; i < d.node->child_count; i++) {
			if (d.node->children[i].tag != SE_NODE || d.node->children[i].as.node->kind != SN_FIELD_NAME)
				continue;
			SyntaxView fn = {d.node->children[i].as.node, d.src};
			/* the inline component type is a type node before the next FIELD_NAME; else a bare
			 * field whose component type is the field's own name. Between the name and the type,
			 * a `type` keyword token marks the explicit meta longhand `name : type : T` (vs the
			 * inferred `name :: T`); preserve it so the formatter round-trips concrete syntax. */
			SyntaxView ty = {NULL, d.src};
			int meta_explicit = 0;
			for (int k = i + 1; k < d.node->child_count; k++) {
				if (d.node->children[k].tag == SE_TOKEN) {
					if (d.node->children[k].as.token.kind == TOK_IDENT && d.node->children[k].as.token.length == 4 &&
					    strncmp(d.src + d.node->children[k].as.token.offset, "type", 4) == 0)
						meta_explicit = 1;
					continue;
				}
				SyntaxNodeKind kk = d.node->children[k].as.node->kind;
				if (kk == SN_FIELD_NAME)
					break;
				if (kk >= SN_TYPE_REF && kk <= SN_TYPE_FUNC) {
					ty.node = d.node->children[k].as.node;
					break;
				}
			}
			char *fname = sem_cv_dup(fn);
			TypeRef *ft;
			if (sv_present(ty)) {
				ft = cst_build_type(ty);
			} else {
				ft = malloc(sizeof(TypeRef));
				ft->kind = TYPE_NAME;
				ft->loc.line = 0;
				ft->loc.column = 0;
				ft->data.name = sem_dupz(fname);
			}
			FieldDecl *fd = field_decl_create(FIELD_COLUMN, fname, ft);
			fd->meta_explicit = meta_explicit;
			aa->fields[aa->field_count++] = fd;
		}
		ad->data.archetype = aa;
		return ad;
	}
	case SN_PROC_DECL: {
		Decl *ad = decl_create(DECL_PROC);
		ad->loc = sem_node_loc(d.node);
		ProcDecl *ap = proc_decl_create(sem_cv_dup(sv_child(d, SN_FUNC_DEF_NAME)));
		ap->loc = sem_direct_token_loc(d.node, TOK_LPAREN); /* lint location: the `(`, like the parser */
		ap->is_extern = !sv_has_token(d, TOK_LBRACE);
		ap->is_variadic = sv_has_token(d, TOK_DOTDOTDOT);
		ap->allow_pure_proc = sv_has_token(d, TOK_AT);
		int np = sv_count(d, SN_PARAM);
		ap->params = calloc(np ? np : 1, sizeof(Parameter *));
		for (int i = 0; i < np; i++)
			ap->params[i] = cst_build_param(sv_child_at(d, SN_PARAM, i));
		ap->param_count = np;
		int nout = sv_count(d, SN_OUT_PARAM); /* the `(out)` list: results written in place (0 = no outputs) */
		ap->out_params = calloc(nout ? nout : 1, sizeof(Parameter *));
		for (int i = 0; i < nout; i++)
			ap->out_params[i] = cst_build_param(sv_child_at(d, SN_OUT_PARAM, i));
		ap->out_param_count = nout;
		ap->statements = cst_build_body(d, &ap->statement_count);
		ad->data.proc = ap;
		return ad;
	}
	case SN_SYS_DECL: {
		Decl *ad = decl_create(DECL_SYS);
		ad->loc = sem_node_loc(d.node);
		SysDecl *as = sys_decl_create(sem_cv_dup(sv_child(d, SN_FUNC_DEF_NAME)));
		as->loc = sem_direct_token_loc(d.node, TOK_LPAREN);
		int np = sv_count(d, SN_PARAM);
		as->params = calloc(np ? np : 1, sizeof(Parameter *));
		for (int i = 0; i < np; i++)
			as->params[i] = cst_build_param(sv_child_at(d, SN_PARAM, i));
		as->param_count = np;
		as->statements = cst_build_body(d, &as->statement_count);
		ad->data.sys = as;
		return ad;
	}
	case SN_FUNC_DECL: {
		Decl *ad = decl_create(DECL_FUNC);
		ad->loc = sem_node_loc(d.node);
		FuncDecl *af = func_decl_create(sem_cv_dup(sv_child(d, SN_FUNC_DEF_NAME)));
		af->loc = sem_direct_token_loc(d.node, TOK_LPAREN);
		af->is_extern = 0; /* funcs are never foreign */
		af->is_variadic = sv_has_token(d, TOK_DOTDOTDOT);
		int np = sv_count(d, SN_PARAM);
		af->params = calloc(np ? np : 1, sizeof(Parameter *));
		for (int i = 0; i < np; i++)
			af->params[i] = cst_build_param(sv_child_at(d, SN_PARAM, i));
		af->param_count = np;
		int nt = sv_type_count_sem(d); /* return types are the direct type-node children */
		af->return_types = calloc(nt ? nt : 1, sizeof(TypeRef *));
		af->return_type_count = 0;
		for (int i = 0; i < nt; i++)
			af->return_types[af->return_type_count++] = cst_build_type(sem_type_at(d, i));
		af->statements = cst_build_body(d, &af->statement_count);
		ad->data.func = af;
		return ad;
	}
	case SN_FUNC_GROUP_DECL: {
		Decl *ad = decl_create(DECL_FUNC_GROUP);
		ad->loc = sem_node_loc(d.node);
		FuncGroup *fg = func_group_create(sem_cv_dup(sv_child(d, SN_FUNC_DEF_NAME)));
		fg->loc = sem_direct_token_loc(d.node, TOK_LBRACE);
		int nmem = 0;
		for (int i = 0; i < d.node->child_count; i++)
			if (d.node->children[i].tag == SE_TOKEN && d.node->children[i].as.token.kind == TOK_IDENT)
				nmem++;
		fg->member_names = calloc(nmem ? nmem : 1, sizeof(char *));
		fg->member_count = 0;
		for (int i = 0; i < d.node->child_count; i++)
			if (d.node->children[i].tag == SE_TOKEN && d.node->children[i].as.token.kind == TOK_IDENT) {
				SynText t = {d.src + d.node->children[i].as.token.offset, d.node->children[i].as.token.length};
				fg->member_names[fg->member_count++] = sem_txt_dup(t);
			}
		ad->data.func_group = fg;
		return ad;
	}
	case SN_CONST_DECL: {
		/* Unified grammar: a binding `name :: <value form>` declares that kind, named by the LHS.
		 * Bodiless proc/func type forms (SN_TYPE_PROC/SN_TYPE_FUNC) fall through to the type-alias
		 * path below (handled by the type system in a later phase). */
		SyntaxView rhs = sem_rhs_form(d);
		if (sv_present(rhs)) {
			SyntaxNodeKind rk = sv_kind(rhs);
			if (rk == SN_PROC_EXPR || rk == SN_FUNC_EXPR || rk == SN_SYS_EXPR || rk == SN_ARCH_EXPR ||
			    rk == SN_GROUP_EXPR || rk == SN_ENUM_EXPR) {
				char *nm = sem_txt_dup(sem_binding_name(d));
				switch (rk) {
				case SN_ENUM_EXPR:
					return build_enum_from(rhs, nm);
				case SN_PROC_EXPR: {
					Decl *ad = build_proc_from(rhs, nm);
					/* decorators live at the binding level (`@allow_pure_proc name :: proc…`) */
					if (sv_has_token(d, TOK_AT))
						ad->data.proc->allow_pure_proc = 1;
					return ad;
				}
				case SN_FUNC_EXPR:
					return build_func_from(rhs, nm);
				case SN_SYS_EXPR:
					return build_sys_from(rhs, nm);
				case SN_ARCH_EXPR:
					return build_archetype_from(rhs, nm);
				case SN_GROUP_EXPR:
					return build_func_group_from(rhs, nm);
				default:
					break;
				}
			}
			/* Bodiless callable RHS `name :: proc()(…)` / `func(…)->T` — a named proc/func TYPE.
			 * Recorded as a const whose type_value is the callable signature; the classifier
			 * registers it as a callable-type alias. */
			if (rk == SN_TYPE_PROC || rk == SN_TYPE_FUNC) {
				Decl *adc = decl_create(DECL_CONST);
				ConstDecl *acc = const_decl_create(sem_txt_dup(sem_binding_name(d)), NULL);
				acc->decl_type = NULL;
				acc->type_value = cst_build_type(rhs);
				acc->value = NULL;
				adc->data.constant = acc;
				return adc;
			}
		}
		Decl *ad = decl_create(DECL_CONST);
		/* A decorated decl's first IDENT is the decorator (`@implements`), not the binding — use the
		 * binding-name helper; and its `(` is a decorator paren, not a tuple group. (For a plain
		 * tuple group `pos (x,y) :: T` the binding name IS the first IDENT, so keep sv_token there.) */
		int decorated = sem_decl_is_decorated(d.node);
		char *cname = sem_txt_dup(decorated ? sem_binding_name(d) : sv_token(d, TOK_IDENT));
		ConstDecl *ac = const_decl_create(cname, NULL);
		ac->decl_type = NULL;
		ac->type_value = NULL;
		ac->value = NULL;
		ac->is_transparent = syntax_const_alias_marked(d); /* `name :: alias T` → tier-1 transparent */
		if (!decorated && sv_has_token(d, TOK_LPAREN)) {
			/* tuple group `name (a, b, …) :: T`: a nominal type alias whose RHS is a TYPE_TUPLE
			 * built from the parenthesized suffixes, each typed by the shared type after `::`. */
			SyntaxView memberty = sem_type_at(d, 0);
			TypeRef *shared = sv_present(memberty) ? cst_build_type(memberty) : NULL;
			/* collect suffix names inside the parens */
			int in_paren = 0, n = 0;
			for (int i = 0; i < d.node->child_count; i++)
				if (d.node->children[i].tag == SE_TOKEN) {
					TokenKind tk = d.node->children[i].as.token.kind;
					if (tk == TOK_LPAREN)
						in_paren = 1;
					else if (tk == TOK_RPAREN)
						in_paren = 0;
					else if (tk == TOK_IDENT && in_paren)
						n++;
				}
			TypeRef *tt = malloc(sizeof(TypeRef));
			tt->kind = TYPE_TUPLE;
			tt->loc.line = 0;
			tt->loc.column = 0;
			tt->data.tuple.field_count = n;
			tt->data.tuple.field_names = malloc((n > 0 ? n : 1) * sizeof(char *));
			tt->data.tuple.field_types = malloc((n > 0 ? n : 1) * sizeof(TypeRef *));
			int fi = 0;
			in_paren = 0;
			for (int i = 0; i < d.node->child_count && fi < n; i++)
				if (d.node->children[i].tag == SE_TOKEN) {
					TokenKind tk = d.node->children[i].as.token.kind;
					if (tk == TOK_LPAREN)
						in_paren = 1;
					else if (tk == TOK_RPAREN)
						in_paren = 0;
					else if (tk == TOK_IDENT && in_paren) {
						SynText t = {d.src + d.node->children[i].as.token.offset, d.node->children[i].as.token.length};
						tt->data.tuple.field_names[fi] = sem_txt_dup(t);
						TypeRef *ct = malloc(sizeof(TypeRef));
						if (shared) {
							*ct = *shared;
							if (shared->kind == TYPE_NAME && shared->data.name)
								ct->data.name = sem_dupz(shared->data.name);
						} else {
							ct->kind = TYPE_NAME;
							ct->loc.line = 0;
							ct->loc.column = 0;
							ct->data.name = sem_dupz("");
						}
						tt->data.tuple.field_types[fi] = ct;
						fi++;
					}
				}
			if (shared)
				type_ref_free(shared);
			ac->type_value = tt;
			ad->data.constant = ac;
			return ad;
		}
		/* Non-tuple const: classify by separator tokens + type/expr children.
		 *   `name :: value`       — value (or simple alias); value = expr.
		 *   `name : T : value`    — typed value const; decl_type = T, value = expr.
		 *   `name : type : <T>`   — nominal type alias; decl_type = TYPE_TYPE, type_value = T. */
		SyntaxView t0 = sem_type_at(d, 0);
		SyntaxView t1 = sem_type_at(d, 1);
		SynText t0name = sv_present(t0) ? sv_token(t0, TOK_IDENT) : (SynText){NULL, 0};
		int t0_is_meta = t0name.ptr && t0name.len == 4 && memcmp(t0name.ptr, "type", 4) == 0;
		if (t0_is_meta && sv_present(t1)) {
			ac->decl_type = cst_build_type(t0); /* TYPE_TYPE */
			ac->type_value = cst_build_type(t1);
		} else {
			if (sv_present(t0))
				ac->decl_type = cst_build_type(t0);
			ac->value = cst_build_expr(sem_node_at_expr(d, 0));
		}
		ad->data.constant = ac;
		return ad;
	}
	case SN_STATIC_DECL: {
		Decl *ad = decl_create(DECL_STATIC);
		if (sv_has_token(d, TOK_LBRACKET)) {
			/* Pool allocation `Name[C](N){V}`: archetype name is the (possibly qualified) head — the
			 * `.`-joined IDENT tokens before `[` (so `lib.Particle[N]` names the imported shape's
			 * canonical identity; mirrors lower.c). Capacity is the `[…]` expr; optional initial
			 * live-count the `(…)` expr; field inits the `{…}`. */
			char an_buf[256];
			int an_len = 0;
			for (int i = 0; i < d.node->child_count; i++) {
				SyntaxElem *ch = &d.node->children[i];
				if (ch->tag != SE_TOKEN)
					continue;
				if (ch->as.token.kind == TOK_LBRACKET)
					break;
				if (ch->as.token.kind != TOK_IDENT)
					continue;
				if (an_len > 0 && an_len < (int)sizeof(an_buf) - 1)
					an_buf[an_len++] = '.';
				int seg = (int)ch->as.token.length;
				for (int k = 0; k < seg && an_len < (int)sizeof(an_buf) - 1; k++)
					an_buf[an_len++] = d.src[ch->as.token.offset + k];
			}
			an_buf[an_len] = '\0';
			char *an = sem_dupz(an_buf);
			StaticDecl *sd = static_decl_archetype_create(an ? an : sem_dupz(""));
			int cap_alloc = d.node->child_count + 1;
			sd->archetype.field_names = calloc(cap_alloc, sizeof(char *));
			sd->archetype.field_values = calloc(cap_alloc, sizeof(Expression *));
			sd->archetype.field_count = 0;
			sd->archetype.init_length = NULL;
			enum { PH_NONE, PH_CAP, PH_LEN, PH_FIELDS } phase = PH_NONE;
			const char *pend = NULL;
			int pend_len = 0;
			for (int i = 0; i < d.node->child_count; i++) {
				SyntaxElem *ch = &d.node->children[i];
				if (ch->tag == SE_TOKEN) {
					switch (ch->as.token.kind) {
					case TOK_LBRACKET:
						phase = PH_CAP;
						break;
					case TOK_LPAREN:
						phase = PH_LEN;
						break;
					case TOK_LBRACE:
						phase = PH_FIELDS;
						break;
					case TOK_RBRACKET:
					case TOK_RPAREN:
					case TOK_RBRACE:
						phase = PH_NONE;
						break;
					case TOK_IDENT:
						if (phase == PH_FIELDS) {
							pend = d.src + ch->as.token.offset;
							pend_len = (int)ch->as.token.length;
						}
						break;
					default:
						break;
					}
					continue;
				}
				SyntaxNodeKind k = ch->as.node->kind;
				if (k < SN_LITERAL_EXPR || k > SN_PAREN_EXPR)
					continue;
				SyntaxView ev = {ch->as.node, d.src};
				if (phase == PH_CAP) {
					sd->archetype.field_values[0] = cst_build_expr(ev);
					sd->archetype.field_names[0] = NULL;
					sd->archetype.field_count = 1;
				} else if (phase == PH_LEN) {
					sd->archetype.init_length = cst_build_expr(ev);
				} else if (phase == PH_FIELDS && pend) {
					int fc = sd->archetype.field_count;
					sd->archetype.field_names[fc] = sem_txt_dup((SynText){pend, pend_len});
					sd->archetype.field_values[fc] = cst_build_expr(ev);
					sd->archetype.field_count++;
					pend = NULL;
				}
			}
			ad->data.static_decl = sd;
		} else {
			/* Storage form of the unified binding: `name : T` / `name : T = v` / `name := v`. A
			 * sized-array T is a buffer; any other (or absent) T is a scalar. The `= v` value is
			 * captured as the initializer; its absence is the implicit `= 0`. Name is the leading
			 * IDENT, the declared type (if any) the single type node, the initializer the expr node. */
			char *aname = sem_txt_dup(sv_token(d, TOK_IDENT));
			SyntaxView arr_ty = sem_type_at(d, 0);
			SyntaxView initv = sem_node_at_expr(d, 0);
			TypeRef *full = sv_present(arr_ty) ? cst_build_type(arr_ty) : NULL;
			int is_array = full && (full->kind == TYPE_SHAPED_ARRAY || full->kind == TYPE_ARRAY);
			if (is_array) {
				TypeRef *elem = (full->kind == TYPE_SHAPED_ARRAY) ? full->data.shaped_array.element_type
				                                                  : full->data.array.element_type;
				int size = 0;
				for (int i = 0; i < arr_ty.node->child_count; i++)
					if (arr_ty.node->children[i].tag == SE_TOKEN &&
					    arr_ty.node->children[i].as.token.kind == TOK_NUMBER) {
						char buf[32];
						int l = (int)arr_ty.node->children[i].as.token.length;
						if (l > 31)
							l = 31;
						memcpy(buf, arr_ty.src + arr_ty.node->children[i].as.token.offset, l);
						buf[l] = '\0';
						size = atoi(buf);
						break;
					}
				StaticDecl *sd = static_decl_array_create(aname, elem, size);
				/* static_decl_array_create takes ownership of `elem`; free the array wrapper only. */
				if (full != elem)
					free(full); /* shaped/array wrapper node; element ownership moved to sd */
				sd->array.init = sv_present(initv) ? cst_build_expr(initv) : NULL;
				ad->data.static_decl = sd;
			} else {
				/* Scalar. The inferred form `name := v` carries no type node — infer int/float from
				 * the literal initializer's lexeme (anything with a '.'/exponent is float). */
				Expression *ie = sv_present(initv) ? cst_build_expr(initv) : NULL;
				TypeRef *sty = full;
				if (!sty) {
					const char *tn = (ie && ie->type == EXPR_LITERAL && ie->data.literal.lexeme &&
					                  strpbrk(ie->data.literal.lexeme, ".eE"))
					                     ? "float"
					                     : "int";
					sty = type_name_create(sem_dupz(tn));
				}
				ad->data.static_decl = static_decl_scalar_create(aname, sty, ie);
			}
		}
		return ad;
	}
	default:
		return NULL;
	}
}

/* ---- module syntax tree registry (parallel to lower_add_module) ---- */
typedef struct {
	char *name;
	const SyntaxNode *root;
	const char *src;
	char *filename; /* source path; a `*.ds.arche` file is a device datasheet (decls stay global) */
} SemModule;
static SemModule g_sem_modules[64];
static int g_sem_module_count = 0;

/* A device datasheet file: its decls are shared global vocabulary, registered UNPREFIXED (mirror
 * of lower.c's is_datasheet_file). */
static int sem_is_datasheet_file(const char *fn) {
	if (!fn)
		return 0;
	size_t L = strlen(fn);
	return L >= 9 && strcmp(fn + L - 9, ".ds.arche") == 0;
}

void semantic_add_module(const char *name, const SyntaxNode *root, const char *src, const char *filename) {
	if (g_sem_module_count >= 64 || !name || !root)
		return;
	g_sem_modules[g_sem_module_count].name = sem_dupz(name);
	g_sem_modules[g_sem_module_count].root = root;
	g_sem_modules[g_sem_module_count].src = src;
	g_sem_modules[g_sem_module_count].filename = filename ? sem_dupz(filename) : NULL;
	g_sem_module_count++;
}

void semantic_reset_modules(void) {
	for (int i = 0; i < g_sem_module_count; i++) {
		free(g_sem_modules[i].name);
		free(g_sem_modules[i].filename);
	}
	g_sem_module_count = 0;
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
static void sem_rename_typeref(TypeRef *t, const char *prefix, char **set, int count);
static void sem_rename_expr(Expression *e, const char *prefix, char **set, int count);
static void sem_rename_stmt(Statement *s, const char *prefix, char **set, int count);

static void sem_rename_typeref(TypeRef *t, const char *prefix, char **set, int count) {
	if (!t)
		return;
	switch (t->kind) {
	case TYPE_NAME:
		sem_maybe_rename(&t->data.name, prefix, set, count);
		break;
	case TYPE_ARRAY:
		sem_rename_typeref(t->data.array.element_type, prefix, set, count);
		break;
	case TYPE_SHAPED_ARRAY:
		sem_rename_typeref(t->data.shaped_array.element_type, prefix, set, count);
		break;
	case TYPE_TUPLE:
		for (int i = 0; i < t->data.tuple.field_count; i++)
			sem_rename_typeref(t->data.tuple.field_types[i], prefix, set, count);
		break;
	case TYPE_HANDLE:
		sem_maybe_rename(&t->data.handle.archetype_name, prefix, set, count);
		break;
	default:
		break;
	}
}
static void sem_rename_expr(Expression *e, const char *prefix, char **set, int count) {
	if (!e)
		return;
	switch (e->type) {
	case EXPR_LITERAL:
	case EXPR_STRING:
		break;
	case EXPR_NAME:
		sem_maybe_rename(&e->data.name.name, prefix, set, count);
		break;
	case EXPR_FIELD:
		sem_rename_expr(e->data.field.base, prefix, set, count);
		break;
	case EXPR_INDEX:
		sem_rename_expr(e->data.index.base, prefix, set, count);
		for (int i = 0; i < e->data.index.index_count; i++)
			sem_rename_expr(e->data.index.indices[i], prefix, set, count);
		break;
	case EXPR_SLICE:
		sem_rename_expr(e->data.slice.base, prefix, set, count);
		sem_rename_expr(e->data.slice.lo, prefix, set, count);
		sem_rename_expr(e->data.slice.hi, prefix, set, count);
		break;
	case EXPR_BINARY:
		sem_rename_expr(e->data.binary.left, prefix, set, count);
		sem_rename_expr(e->data.binary.right, prefix, set, count);
		break;
	case EXPR_UNARY:
		sem_rename_expr(e->data.unary.operand, prefix, set, count);
		break;
	case EXPR_CALL:
		sem_rename_expr(e->data.call.callee, prefix, set, count);
		for (int i = 0; i < e->data.call.arg_count; i++)
			sem_rename_expr(e->data.call.args[i], prefix, set, count);
		break;
	case EXPR_ALLOC:
		sem_maybe_rename(&e->data.alloc.archetype_name, prefix, set, count);
		for (int i = 0; i < e->data.alloc.field_count; i++)
			sem_rename_expr(e->data.alloc.field_values[i], prefix, set, count);
		sem_rename_expr(e->data.alloc.init_length, prefix, set, count);
		break;
	case EXPR_ARRAY_LITERAL:
		for (int i = 0; i < e->data.array_literal.element_count; i++)
			sem_rename_expr(e->data.array_literal.elements[i], prefix, set, count);
		break;
	}
}
static void sem_rename_stmt(Statement *s, const char *prefix, char **set, int count) {
	if (!s)
		return;
	switch (s->type) {
	case STMT_BIND:
		sem_rename_typeref(s->data.bind_stmt.type, prefix, set, count);
		sem_rename_expr(s->data.bind_stmt.value, prefix, set, count);
		break;
	case STMT_ASSIGN:
		sem_rename_expr(s->data.assign_stmt.target, prefix, set, count);
		sem_rename_expr(s->data.assign_stmt.value, prefix, set, count);
		break;
	case STMT_FOR:
		sem_rename_expr(s->data.for_stmt.iterable, prefix, set, count);
		sem_rename_stmt(s->data.for_stmt.init, prefix, set, count);
		sem_rename_expr(s->data.for_stmt.condition, prefix, set, count);
		sem_rename_stmt(s->data.for_stmt.increment, prefix, set, count);
		for (int i = 0; i < s->data.for_stmt.body_count; i++)
			sem_rename_stmt(s->data.for_stmt.body[i], prefix, set, count);
		break;
	case STMT_IF:
		sem_rename_expr(s->data.if_stmt.cond, prefix, set, count);
		for (int i = 0; i < s->data.if_stmt.then_count; i++)
			sem_rename_stmt(s->data.if_stmt.then_body[i], prefix, set, count);
		for (int i = 0; i < s->data.if_stmt.else_count; i++)
			sem_rename_stmt(s->data.if_stmt.else_body[i], prefix, set, count);
		break;
	case STMT_EXPR:
		sem_rename_expr(s->data.expr_stmt.expr, prefix, set, count);
		break;
	case STMT_RETURN:
		for (int i = 0; i < s->data.return_stmt.count; i++)
			sem_rename_expr(s->data.return_stmt.values[i], prefix, set, count);
		break;
	case STMT_MULTI_BIND:
		for (int i = 0; i < s->data.multi_bind.target_count; i++)
			sem_rename_typeref(s->data.multi_bind.targets[i].type, prefix, set, count);
		sem_rename_expr(s->data.multi_bind.value, prefix, set, count);
		break;
	case STMT_EACH_FIELD:
		sem_rename_typeref(s->data.each_field.filter_type, prefix, set, count);
		for (int i = 0; i < s->data.each_field.body_count; i++)
			sem_rename_stmt(s->data.each_field.body[i], prefix, set, count);
		break;
	default:
		break;
	}
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
static void sem_qualify_expr(Expression *e, char **prefix, char ***set, int *count, int n) {
	if (!e)
		return;
	if (e->type == EXPR_FIELD && e->data.field.base && e->data.field.base->type == EXPR_NAME &&
	    e->data.field.field_name) {
		char mangled[256];
		const char *base = e->data.field.base->data.name.name;
		if (sem_qual_lookup(prefix, set, count, n, base, e->data.field.field_name, mangled, sizeof(mangled))) {
			e->type = EXPR_NAME;
			e->data.name.name = sem_dupz(mangled);
			e->data.name.is_table_ref = 0;
			return;
		}
		/* Base IS an imported module but it has no such member → a precise diagnostic (e.g.
		 * `fmt.nope` → "module 'fmt' has no member 'nope'"), instead of the misleading
		 * "Undefined variable 'fmt'" the value-field path would later emit. */
		if (g_sem_qualify_ctx && sem_base_is_module(prefix, n, base))
			sem_emit_module_no_member(g_sem_qualify_ctx, e->loc, base, e->data.field.field_name);
	}
	switch (e->type) {
	case EXPR_FIELD:
		sem_qualify_expr(e->data.field.base, prefix, set, count, n);
		break;
	case EXPR_INDEX:
		sem_qualify_expr(e->data.index.base, prefix, set, count, n);
		for (int i = 0; i < e->data.index.index_count; i++)
			sem_qualify_expr(e->data.index.indices[i], prefix, set, count, n);
		break;
	case EXPR_BINARY:
		sem_qualify_expr(e->data.binary.left, prefix, set, count, n);
		sem_qualify_expr(e->data.binary.right, prefix, set, count, n);
		break;
	case EXPR_UNARY:
		sem_qualify_expr(e->data.unary.operand, prefix, set, count, n);
		break;
	case EXPR_CALL:
		sem_qualify_expr(e->data.call.callee, prefix, set, count, n);
		/* Record the resolved callee name (qualify-mangled for `mod.f`, else the bare name) in the
		 * side model, keyed by the call's node id, so the syntax-tree-driven type resolver can read
		 * it without re-doing the module export lookup. */
		if (g_sem_qualify_ctx && g_sem_qualify_ctx->model && e->cst_id && e->data.call.callee &&
		    e->data.call.callee->type == EXPR_NAME)
			sem_model_set_callee_name(g_sem_qualify_ctx->model, e->cst_id - 1, e->data.call.callee->data.name.name);
		for (int i = 0; i < e->data.call.arg_count; i++)
			sem_qualify_expr(e->data.call.args[i], prefix, set, count, n);
		break;
	case EXPR_ALLOC:
		for (int i = 0; i < e->data.alloc.field_count; i++)
			sem_qualify_expr(e->data.alloc.field_values[i], prefix, set, count, n);
		sem_qualify_expr(e->data.alloc.init_length, prefix, set, count, n);
		break;
	case EXPR_ARRAY_LITERAL:
		for (int i = 0; i < e->data.array_literal.element_count; i++)
			sem_qualify_expr(e->data.array_literal.elements[i], prefix, set, count, n);
		break;
	default:
		break;
	}
	/* Record this reference's resolved leftmost name (module-inlined names are prefixed by the
	 * rename pass that already ran, e.g. `csv.load_cols`), keyed by node id, so the view-driven
	 * analysis looks names up under the identity the AST resolved them to, not the bare token. */
	if (g_sem_qualify_ctx && g_sem_qualify_ctx->model && e->cst_id &&
	    (e->type == EXPR_NAME || e->type == EXPR_FIELD || e->type == EXPR_INDEX || e->type == EXPR_SLICE)) {
		const char *ln = lvalue_leftmost_name(e);
		if (ln)
			sem_model_set_ref_name(g_sem_qualify_ctx->model, e->cst_id - 1, ln);
	}
}
static void sem_qualify_stmt(Statement *s, char **prefix, char ***set, int *count, int n) {
	if (!s)
		return;
	switch (s->type) {
	case STMT_BIND:
		sem_qualify_expr(s->data.bind_stmt.value, prefix, set, count, n);
		break;
	case STMT_ASSIGN:
		sem_qualify_expr(s->data.assign_stmt.target, prefix, set, count, n);
		sem_qualify_expr(s->data.assign_stmt.value, prefix, set, count, n);
		break;
	case STMT_FOR:
		sem_qualify_expr(s->data.for_stmt.iterable, prefix, set, count, n);
		sem_qualify_stmt(s->data.for_stmt.init, prefix, set, count, n);
		sem_qualify_expr(s->data.for_stmt.condition, prefix, set, count, n);
		sem_qualify_stmt(s->data.for_stmt.increment, prefix, set, count, n);
		for (int i = 0; i < s->data.for_stmt.body_count; i++)
			sem_qualify_stmt(s->data.for_stmt.body[i], prefix, set, count, n);
		break;
	case STMT_IF:
		sem_qualify_expr(s->data.if_stmt.cond, prefix, set, count, n);
		for (int i = 0; i < s->data.if_stmt.then_count; i++)
			sem_qualify_stmt(s->data.if_stmt.then_body[i], prefix, set, count, n);
		for (int i = 0; i < s->data.if_stmt.else_count; i++)
			sem_qualify_stmt(s->data.if_stmt.else_body[i], prefix, set, count, n);
		break;
	case STMT_EXPR:
		sem_qualify_expr(s->data.expr_stmt.expr, prefix, set, count, n);
		break;
	case STMT_RETURN:
		for (int i = 0; i < s->data.return_stmt.count; i++)
			sem_qualify_expr(s->data.return_stmt.values[i], prefix, set, count, n);
		break;
	case STMT_MULTI_BIND:
		sem_qualify_expr(s->data.multi_bind.value, prefix, set, count, n);
		break;
	case STMT_EACH_FIELD:
		for (int i = 0; i < s->data.each_field.body_count; i++)
			sem_qualify_stmt(s->data.each_field.body[i], prefix, set, count, n);
		break;
	default:
		break;
	}
}
static void sem_qualify_decl(Decl *d, char **prefix, char ***set, int *count, int n) {
	if (!d)
		return;
	if (d->kind == DECL_PROC)
		for (int i = 0; i < d->data.proc->statement_count; i++)
			sem_qualify_stmt(d->data.proc->statements[i], prefix, set, count, n);
	else if (d->kind == DECL_SYS)
		for (int i = 0; i < d->data.sys->statement_count; i++)
			sem_qualify_stmt(d->data.sys->statements[i], prefix, set, count, n);
	else if (d->kind == DECL_FUNC)
		for (int i = 0; i < d->data.func->statement_count; i++)
			sem_qualify_stmt(d->data.func->statements[i], prefix, set, count, n);
}

static const char *sem_decl_name(Decl *d) {
	switch (d->kind) {
	case DECL_ARCHETYPE:
		return d->data.archetype->name;
	case DECL_PROC:
		return d->data.proc->name;
	case DECL_SYS:
		return d->data.sys->name;
	case DECL_FUNC:
		return d->data.func->name;
	case DECL_FUNC_GROUP:
		return d->data.func_group->name;
	case DECL_STATIC:
		if (d->data.static_decl->kind == STATIC_KIND_ARRAY)
			return d->data.static_decl->array.name;
		if (d->data.static_decl->kind == STATIC_KIND_SCALAR)
			return d->data.static_decl->scalar.name;
		return d->data.static_decl->archetype.archetype_name;
	case DECL_CONST:
		return d->data.constant->name;
	case DECL_WORLD:
		return d->data.world->name;
	case DECL_ENUM:
		return d->data.enum_decl->name;
	default:
		return NULL;
	}
}
static void sem_rename_decl(Decl *d, const char *prefix, char **set, int count) {
	/* A `@drop(<T>)` decorator names an opaque type; when that type is a module-local opaque
	 * (e.g. `socket` inside `net`), it is prefixed like any other module symbol, so the
	 * decorator's type name must rename in lockstep with the destructor's parameter type. */
	if (d->is_drop)
		sem_maybe_rename(&d->drop_type, prefix, set, count);
	switch (d->kind) {
	case DECL_ARCHETYPE:
		sem_maybe_rename(&d->data.archetype->name, prefix, set, count);
		for (int i = 0; i < d->data.archetype->field_count; i++)
			sem_rename_typeref(d->data.archetype->fields[i]->type, prefix, set, count);
		break;
	case DECL_PROC:
		sem_maybe_rename(&d->data.proc->name, prefix, set, count);
		for (int i = 0; i < d->data.proc->param_count; i++)
			sem_rename_typeref(d->data.proc->params[i]->type, prefix, set, count);
		for (int i = 0; i < d->data.proc->out_param_count; i++)
			sem_rename_typeref(d->data.proc->out_params[i]->type, prefix, set, count);
		for (int i = 0; i < d->data.proc->statement_count; i++)
			sem_rename_stmt(d->data.proc->statements[i], prefix, set, count);
		break;
	case DECL_SYS:
		sem_maybe_rename(&d->data.sys->name, prefix, set, count);
		for (int i = 0; i < d->data.sys->param_count; i++)
			sem_rename_typeref(d->data.sys->params[i]->type, prefix, set, count);
		for (int i = 0; i < d->data.sys->statement_count; i++)
			sem_rename_stmt(d->data.sys->statements[i], prefix, set, count);
		break;
	case DECL_FUNC:
		sem_maybe_rename(&d->data.func->name, prefix, set, count);
		for (int i = 0; i < d->data.func->return_type_count; i++)
			sem_rename_typeref(d->data.func->return_types[i], prefix, set, count);
		for (int i = 0; i < d->data.func->param_count; i++)
			sem_rename_typeref(d->data.func->params[i]->type, prefix, set, count);
		for (int i = 0; i < d->data.func->statement_count; i++)
			sem_rename_stmt(d->data.func->statements[i], prefix, set, count);
		break;
	case DECL_FUNC_GROUP:
		sem_maybe_rename(&d->data.func_group->name, prefix, set, count);
		for (int i = 0; i < d->data.func_group->member_count; i++)
			sem_maybe_rename(&d->data.func_group->member_names[i], prefix, set, count);
		break;
	case DECL_STATIC:
		if (d->data.static_decl->kind == STATIC_KIND_ARRAY) {
			sem_maybe_rename(&d->data.static_decl->array.name, prefix, set, count);
			sem_rename_typeref(d->data.static_decl->array.element_type, prefix, set, count);
		} else if (d->data.static_decl->kind == STATIC_KIND_SCALAR) {
			sem_maybe_rename(&d->data.static_decl->scalar.name, prefix, set, count);
			sem_rename_typeref(d->data.static_decl->scalar.type, prefix, set, count);
			sem_rename_expr(d->data.static_decl->scalar.init, prefix, set, count);
		} else {
			sem_maybe_rename(&d->data.static_decl->archetype.archetype_name, prefix, set, count);
			for (int i = 0; i < d->data.static_decl->archetype.field_count; i++)
				sem_rename_expr(d->data.static_decl->archetype.field_values[i], prefix, set, count);
			sem_rename_expr(d->data.static_decl->archetype.init_length, prefix, set, count);
		}
		break;
	case DECL_CONST:
		sem_maybe_rename(&d->data.constant->name, prefix, set, count);
		sem_rename_expr(d->data.constant->value, prefix, set, count);
		break;
	case DECL_WORLD:
		sem_maybe_rename(&d->data.world->name, prefix, set, count);
		break;
	default:
		break;
	}
}

/* Expand a bare archetype component referencing a top-level tuple group into the inline tuple
 * form (mirrors main.c expand_archetype_tuple_groups), so flattening yields `pos_<member>`. */
static void sem_expand_tuple_groups(AstProgram *prog) {
	if (!prog)
		return;
	for (int a = 0; a < prog->decl_count; a++) {
		if (prog->decls[a]->kind != DECL_ARCHETYPE)
			continue;
		ArchetypeDecl *arch = prog->decls[a]->data.archetype;
		for (int f = 0; f < arch->field_count; f++) {
			FieldDecl *fd = arch->fields[f];
			if (!fd->type || fd->type->kind != TYPE_NAME || !fd->type->data.name)
				continue;
			const char *ref = fd->type->data.name;
			for (int dd = 0; dd < prog->decl_count; dd++) {
				if (prog->decls[dd]->kind != DECL_CONST)
					continue;
				ConstDecl *cd = prog->decls[dd]->data.constant;
				if (!cd || !cd->name || !cd->type_value || cd->type_value->kind != TYPE_TUPLE)
					continue;
				if (strcmp(cd->name, ref) != 0)
					continue;
				TypeRef *src = cd->type_value;
				int n = src->data.tuple.field_count;
				TypeRef *tt = malloc(sizeof(TypeRef));
				tt->kind = TYPE_TUPLE;
				tt->loc = fd->type->loc;
				tt->data.tuple.field_count = n;
				tt->data.tuple.field_names = malloc((n > 0 ? n : 1) * sizeof(char *));
				tt->data.tuple.field_types = malloc((n > 0 ? n : 1) * sizeof(TypeRef *));
				for (int j = 0; j < n; j++) {
					tt->data.tuple.field_names[j] = sem_dupz(src->data.tuple.field_names[j]);
					TypeRef *st = src->data.tuple.field_types[j];
					TypeRef *ct = malloc(sizeof(TypeRef));
					*ct = *st;
					if (st->kind == TYPE_NAME && st->data.name)
						ct->data.name = sem_dupz(st->data.name);
					tt->data.tuple.field_types[j] = ct;
				}
				type_ref_free(fd->type);
				fd->type = tt;
				break;
			}
		}
	}
}

/* Build one module decl from `node`, append it to prog, and record its name in the module's
 * `full` set (intra-module resolution) and — when `exported` — its `expset` (externally visible via
 * qualified `mod.name`). Non-externs are prefixed to `<mod>_<name>` and recorded under their source
 * name. Externs keep their unprefixed decl (the C ABI symbol), are NOT added to `full`, and go into
 * `expset` under the prefix-stripped visible name so `mod.<visible>` reconstructs the C symbol
 * (e.g. `net.listen` → `net_listen`). Shared by the top-level module loop and the recursion into
 * `#foreign { ... }` / `#module { ... }` block regions. */
static void sem_add_module_decl(const SyntaxNode *node, const char *msrc, const char *mod_name, AstProgram *prog,
                                char ***full, int *fulln, int *fullcap, char ***expset, int *expn, int *expcap,
                                int exported, int is_datasheet, int module_is_device, int file_local, char ***fileset,
                                int *filesetn, int *filesetcap) {
	Decl *md = cst_build_decl((SyntaxView){node, msrc});
	if (!md)
		return;
	/* Datasheet decls are shared global vocabulary: mark them so identical component/type redefs
	 * across two datasheets dedup (devices sharing a shape) instead of tripping define-once. */
	md->is_datasheet = is_datasheet;
	/* A decl from a device's IMPL (`.arche` of a unit that also has a `.ds.arche`) — the rule-3 sweep
	 * rejects type/archetype/allocation definitions here (a device's impl is behavior-only). */
	md->from_device_impl = module_is_device && !is_datasheet;
	/* A pool decl in a datasheet (`.ds.arche`) is a storage REQUIREMENT (min rows), not an allocation. */
	if (is_datasheet && md->kind == DECL_STATIC && md->data.static_decl &&
	    md->data.static_decl->kind == STATIC_KIND_ARCHETYPE)
		md->data.static_decl->is_requirement = 1;
	prog->decls[prog->decl_count++] = md;
	int is_ext =
	    (md->kind == DECL_PROC && md->data.proc->is_extern) || (md->kind == DECL_FUNC && md->data.func->is_extern);
	const char *nm = sem_decl_name(md);
	if (!nm)
		return;
	/* A member is accessed by its LITERAL declared name. A decl is registered FLAT (unprefixed, bare
	 * export) when it's foreign (C ABI symbol), a datasheet decl (shared global vocabulary), OR from a
	 * PLAIN module (no `.ds.arche`) — a plain/path module merges flat into the importer (Jai `#load`),
	 * so `helper()` not `mod.helper()`. Only a DEVICE's pure-Arche impl decls are prefixed to the
	 * qualified identity `<device>.<name>` (the device's namespaced contract). */
	int flat = is_ext || is_datasheet || !module_is_device;
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
	return k >= SN_WORLD_DECL && k <= SN_USE_DECL && k != SN_USE_DECL;
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
	for (int i = 0; i < root->child_count; i++) {
		if (root->children[i].tag != SE_NODE)
			continue;
		const SyntaxNode *cn = root->children[i].as.node;
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
static void sem_inline_module(const char *mod_name, AstProgram *prog, char **acc_prefix, char ***acc_set,
                              int *acc_count, int *acc_n) {
	for (int a = 0; a < *acc_n; a++)
		if (strcmp(acc_prefix[a], mod_name) == 0)
			return; /* already inlined (direct or via another transitive path) */
	int first = prog->decl_count;
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
		int file_first = prog->decl_count;                         /* this file's decl range, for the #file rename */
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
						sem_add_module_decl(cn->children[c].as.node, msrc, mod_name, prog, &full, &fulln, &fullcap,
						                    &expset, &expn, &expcap, child_exp, ds, module_is_device, child_fl,
						                    &fileset, &filesetn, &filesetcap);
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
			sem_add_module_decl(cn, msrc, mod_name, prog, &full, &fulln, &fullcap, &expset, &expn, &expcap, exported,
			                    ds, module_is_device, file_local, &fileset, &filesetn, &filesetcap);
		}
		/* Rename this file's `#file` decls (+ their intra-file references) to a file-unique identity
		 * `<mod>.__f<m>.<name>` so a sibling file's bare reference to the same name does NOT bind to
		 * them — `#file` = visible only within its own file. (`m` is unique per file.) */
		if (filesetn > 0) {
			char fprefix[300];
			snprintf(fprefix, sizeof(fprefix), "%s.__f%d", mod_name, m);
			for (int dd = file_first; dd < prog->decl_count; dd++)
				sem_rename_decl(prog->decls[dd], fprefix, fileset, filesetn);
			for (int x = 0; x < filesetn; x++)
				free(fileset[x]);
		}
		free(fileset);
	}
	if (!found) {
		free(full);
		free(expset);
		return;
	}
	/* Scope resolution: rename this module's pure-Arche decls + their intra-module references to the
	 * qualified identity `<mod>.<name>` (foreign decls keep their C-symbol name). `full` is the set
	 * of this module's pure-Arche names, so a bare reference inside the module binds to its own
	 * member. */
	for (int dd = first; dd < prog->decl_count; dd++)
		sem_rename_decl(prog->decls[dd], mod_name, full, fulln);
	for (int x = 0; x < fulln; x++)
		free(full[x]);
	free(full);
	if (*acc_n < 64) {
		acc_prefix[*acc_n] = sem_dupz(mod_name);
		acc_set[*acc_n] = expset;
		acc_count[*acc_n] = expn;
		(*acc_n)++;
	} else {
		for (int x = 0; x < expn; x++)
			free(expset[x]);
		free(expset);
	}
	/* Transitive: inline this module's own `#import`s (now that it's in acc → cycle-safe). */
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
				sem_inline_module(sub, prog, acc_prefix, acc_set, acc_count, acc_n);
				free(sub);
			}
		}
	}
}

/* Build the analyzable AstProgram from the main-file syntax tree plus all registered module syntax trees,
 * inlining + name-prefixing modules exactly as main.c's resolve_uses does, then expanding
 * top-level tuple groups. The returned AstProgram owns all its memory (free with ast_program_free). */
static AstProgram *cst_to_program(const SyntaxNode *root, const char *src) {
	AstProgram *prog = ast_program_create();
	SyntaxView r = sv_root(root, src);
	/* Deep count so decls nested in `#foreign { }` / `#module { }` block regions (collected by the
	 * region recursion below) can't overflow the array; over-estimates are harmless. */
	int cap = sv_node_count_deep(r) + 8;
	for (int m = 0; m < g_sem_module_count; m++)
		cap += sv_node_count_deep(sv_root(g_sem_modules[m].root, g_sem_modules[m].src)) + 1;
	prog->decls = calloc(cap ? cap : 1, sizeof(Decl *));
	prog->decl_count = 0;

	/* Per-module: prefix + exported (still-bare) names, for cross-module resolution. */
	char *acc_prefix[64];
	char **acc_set[64];
	int acc_count[64];
	int acc_n = 0;

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
					Decl *ad = cst_build_decl((SyntaxView){rn->children[c].as.node, src});
					if (ad)
						prog->decls[prog->decl_count++] = ad;
				}
			}
			continue;
		}
		if (k < SN_WORLD_DECL || k > SN_USE_DECL)
			continue;
		SyntaxView dv = {root->children[i].as.node, src};

		if (k == SN_USE_DECL) {
			/* One element per import: IDENT = device by name, STRING = module by path. Inline each —
			 * the helper recurses into the module's own transitive imports + dedups. */
			const SyntaxNode *un = dv.node;
			for (int t = 0; t < un->child_count; t++) {
				char *mod_name = sem_import_token_module_name(src, &un->children[t]);
				if (!mod_name)
					continue;
				sem_inline_module(mod_name, prog, acc_prefix, acc_set, acc_count, &acc_n);
				free(mod_name);
			}
			continue;
		}

		Decl *ad = cst_build_decl(dv);
		if (ad)
			prog->decls[prog->decl_count++] = ad;
	}

	/* Scope resolution: bind every `mod.member` reference to its member's qualified identity. A
	 * member is looked up by its literal name in the module's export set (no prefix stripping); an
	 * unknown member emits a precise `module has no member` diagnostic (ctx via g_sem_qualify_ctx,
	 * set by semantic_analyze_cst — NULL on the unit-test AST-only path, which skips the diag). */
	if (acc_n > 0)
		for (int dd = 0; dd < prog->decl_count; dd++)
			sem_qualify_decl(prog->decls[dd], acc_prefix, acc_set, acc_count, acc_n);

	/* Module exports are reachable ONLY via qualified access (handled by the qualify pass above);
	 * a bare `name` does NOT resolve to a module export. */
	for (int m = 0; m < acc_n; m++) {
		for (int e = 0; e < acc_count[m]; e++)
			free(acc_set[m][e]);
		free(acc_set[m]);
		free(acc_prefix[m]);
	}

	sem_expand_tuple_groups(prog);
	return prog;
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
		if (c->const_type_value) {
			TypeRef *tv = c->const_type_value;
			if (tv->kind == TYPE_PROC || tv->kind == TYPE_FUNC) {
				/* `name :: proc()(…)` — a named structural callable type. */
				register_callable_type_alias(ctx, c->name, tv);
				continue;
			}
			if (tv->kind == TYPE_TUPLE) {
				for (int f = 0; f < tv->data.tuple.field_count; f++) {
					TypeRef *ft = tv->data.tuple.field_types[f];
					const char *fbacking = type_backing_name(ft);
					if (!fbacking) {
						sem_emit_tuple_field_not_simple(ctx, ft->loc);
						continue;
					}
					const char *fn = tv->data.tuple.field_names[f];
					size_t L = strlen(c->name) + 1 + strlen(fn) + 1;
					char *aname = malloc(L);
					snprintf(aname, L, "%s_%s", c->name, fn);
					register_type_alias(ctx, aname, fbacking, ft->loc, dsheet); /* aname leaks like the old path */
				}
			} else {
				const char *backing = type_backing_name(tv);
				if (!backing)
					sem_emit_alias_backing_invalid(ctx, tv->loc);
				else
					register_type_alias_tiered(ctx, c->name, backing, tv->is_transparent, cloc, dsheet);
			}
			continue;
		}

		/* Literal RHS: a value const. Its type is the explicit declared type if present, else the
		 * literal's own type (`3.14`→float, `42`→int) — so the const resolves to its real type. */
		if (c->const_value_kind == EXPR_LITERAL) {
			const char *vt = NULL;
			if (c->const_decl_type && c->const_decl_type->kind == TYPE_NAME)
				vt = resolve_type_alias(ctx, normalize_type_name(c->const_decl_type->data.name));
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
			deferred_value_ctx[deferred_count] = (c->const_decl_type != NULL);
			deferred_transparent[deferred_count] = c->is_transparent;
			deferred_datasheet[deferred_count] = dsheet;
			deferred_loc[deferred_count] = cloc;
			deferred_count++;
			continue;
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
		/* An enum is a transparent (tier-1) int alias: variants are int values and `match`/comparison
		 * treat the enum as int, so it must interchange with int freely. */
		register_type_alias_tiered(ctx, sem_dupz(e->name), "int", 1, e->loc, e->is_datasheet);
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
			if (!fd->type || !fd->name)
				continue;
			const char *backing = NULL;
			if (fd->type->kind == TYPE_NAME) {
				if (strcmp(fd->type->data.name, fd->name) == 0)
					continue; /* bare reference, not an inline definition */
				backing = fd->type->data.name;
			} else if (fd->type->kind == TYPE_OPAQUE) {
				backing = "opaque";
			} else {
				continue; /* array/tuple component: column-only, no nominal alias */
			}
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
			add_variable(ctx, d->name, d->static_type);
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

	/* Unique-name rule (Rust/Go): a func and a proc — or two of either — may not share a name. The
	 * two kinds are still distinct (keyword, `-> T` vs out-list, call form), but the name namespace
	 * is single, so a clash is E0031. Scans real decls only (not builtins). */
	for (int i = 0; i < ctx->decl_count; i++) {
		DeclSummary *di = ctx->decls[i];
		if (di->kind != DECL_FUNC && di->kind != DECL_PROC)
			continue;
		const char *ni = di->name;
		if (!ni)
			continue;
		const char *ki = (di->kind == DECL_FUNC) ? "func" : "proc";
		for (int j = 0; j < i; j++) {
			DeclSummary *dj = ctx->decls[j];
			if (dj->kind != DECL_FUNC && dj->kind != DECL_PROC)
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
}

/* Allocate + zero-initialize a SemanticContext and register builtins. Shared by both
 * entry points. */
static SemanticContext *make_context(void) {
	SemanticContext *ctx = malloc(sizeof(SemanticContext));
	ctx->archetypes = NULL;
	ctx->archetype_count = 0;
	ctx->decls = NULL;
	ctx->decl_count = 0;
	ctx->owned_types = NULL;
	ctx->owned_type_count = 0;
	ctx->owned_type_cap = 0;
	ctx->aliases = NULL;
	ctx->alias_count = 0;
	ctx->known_funcs = NULL;
	ctx->known_func_count = 0;
	ctx->callable_alias_names = NULL;
	ctx->callable_alias_targets = NULL;
	ctx->callable_alias_count = 0;
	ctx->ctype_alias_names = NULL;
	ctx->ctype_alias_refs = NULL;
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
	ctx->prog = NULL;
	ctx->owned_prog = NULL;
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

static TypeRef *analysis_own_type(SemanticContext *ctx, TypeRef *t) {
	if (!t)
		return NULL;
	if (ctx->owned_type_count >= ctx->owned_type_cap) {
		ctx->owned_type_cap = ctx->owned_type_cap ? ctx->owned_type_cap * 2 : 16;
		ctx->owned_types = realloc(ctx->owned_types, (size_t)ctx->owned_type_cap * sizeof(TypeRef *));
	}
	ctx->owned_types[ctx->owned_type_count++] = t;
	return t;
}

/* The first pattern token of a match arm (variant / literal / `_`), or NULL. */
static const SyntaxElem *match_arm_pattern_tok(const SyntaxNode *arm) {
	for (int c = 0; c < arm->child_count; c++) {
		if (arm->children[c].tag != SE_TOKEN)
			continue;
		TokenKind k = arm->children[c].as.token.kind;
		if (k == TOK_IDENT || k == TOK_NUMBER || k == TOK_STRING || k == TOK_CHAR_LIT)
			return &arm->children[c];
		return NULL; /* something else came first */
	}
	return NULL;
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

/* Pure deep-copy of a TypeRef (no pooling). Mirrors type_ref_free's ownership: TYPE_HANDLE's
 * archetype_name is dup'd (type_ref_free never frees it — a pre-existing upstream leak we match). */
static TypeRef *sem_type_deep_copy(const TypeRef *t) {
	if (!t)
		return NULL;
	TypeRef *c = calloc(1, sizeof(TypeRef));
	c->kind = t->kind;
	c->loc = t->loc;
	c->is_transparent = t->is_transparent;
	switch (t->kind) {
	case TYPE_NAME:
		c->data.name = t->data.name ? sem_dupz(t->data.name) : NULL;
		break;
	case TYPE_ARRAY:
		c->data.array.element_type = sem_type_deep_copy(t->data.array.element_type);
		break;
	case TYPE_SHAPED_ARRAY:
		c->data.shaped_array.element_type = sem_type_deep_copy(t->data.shaped_array.element_type);
		c->data.shaped_array.rank = t->data.shaped_array.rank;
		break;
	case TYPE_TUPLE: {
		int n = t->data.tuple.field_count;
		c->data.tuple.field_count = n;
		c->data.tuple.field_names = calloc(n ? n : 1, sizeof(char *));
		c->data.tuple.field_types = calloc(n ? n : 1, sizeof(TypeRef *));
		for (int i = 0; i < n; i++) {
			c->data.tuple.field_names[i] = sem_dupz(t->data.tuple.field_names[i]);
			c->data.tuple.field_types[i] = sem_type_deep_copy(t->data.tuple.field_types[i]);
		}
		break;
	}
	case TYPE_HANDLE:
		c->data.handle.archetype_name = t->data.handle.archetype_name ? sem_dupz(t->data.handle.archetype_name) : NULL;
		break;
	case TYPE_PROC:
	case TYPE_FUNC: {
		int pn = t->data.callable.param_count, rn = t->data.callable.result_count;
		c->data.callable.param_count = pn;
		c->data.callable.result_count = rn;
		c->data.callable.is_proc = t->data.callable.is_proc;
		c->data.callable.param_types = calloc(pn ? pn : 1, sizeof(TypeRef *));
		c->data.callable.result_types = calloc(rn ? rn : 1, sizeof(TypeRef *));
		for (int i = 0; i < pn; i++)
			c->data.callable.param_types[i] = sem_type_deep_copy(t->data.callable.param_types[i]);
		for (int i = 0; i < rn; i++)
			c->data.callable.result_types[i] = sem_type_deep_copy(t->data.callable.result_types[i]);
		break;
	}
	default:
		break;
	}
	return c;
}

/* Deep-copy `t` into the context type pool (one pool root; type_ref_free frees the whole tree at
 * teardown). NULL-safe. */
static TypeRef *sem_pool_type_copy(SemanticContext *ctx, const TypeRef *t) {
	return t ? analysis_own_type(ctx, sem_type_deep_copy(t)) : NULL;
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
			if (k == SN_PROC_EXPR || k == SN_FUNC_EXPR || k == SN_SYS_EXPR)
				return (SyntaxView){dn.node->children[i].as.node, dn.src};
		}
	return dn;
}

static ParamSummary sem_param_summary(SemanticContext *ctx, const Parameter *p) {
	ParamSummary s = {0};
	if (!p)
		return s;
	s.name = p->name ? sem_dupz(p->name) : NULL;
	s.type = sem_pool_type_copy(ctx, p->type);
	s.is_own = p->is_own;
	s.loc = p->loc;
	return s;
}

/* Build one resolved DeclSummary from a (post rename/qualify) AstProgram Decl. */
static DeclSummary *decl_summary_from(SemanticContext *ctx, const Decl *d) {
	DeclSummary *ds = calloc(1, sizeof(DeclSummary));
	ds->kind = d->kind;
	ds->loc = d->loc;
	ds->node = (SyntaxView){d->sn, d->sn_src};
	ds->body_node = sem_decl_body_node(ds->node);
	ds->static_kind = -1;
	ds->from_device_impl = d->from_device_impl;
	ds->is_datasheet = d->is_datasheet;
	ds->is_drop = d->is_drop;
	ds->drop_type = d->drop_type ? sem_dupz(d->drop_type) : NULL;
	const char *nm = sem_decl_name((Decl *)d);
	ds->name = nm ? sem_dupz(nm) : NULL;
	if (d->allow_slug_count > 0) {
		ds->allow_slug_count = d->allow_slug_count;
		ds->allow_slugs = calloc(d->allow_slug_count, sizeof(char *));
		for (int i = 0; i < d->allow_slug_count; i++)
			ds->allow_slugs[i] = sem_dupz(d->allow_slugs[i]);
	}
	switch (d->kind) {
	case DECL_PROC: {
		ProcDecl *p = d->data.proc;
		ds->is_extern = p->is_extern;
		ds->is_variadic = p->is_variadic;
		ds->allow_pure_proc = p->allow_pure_proc;
		ds->param_count = p->param_count;
		ds->params = calloc(p->param_count ? p->param_count : 1, sizeof(ParamSummary));
		for (int i = 0; i < p->param_count; i++)
			ds->params[i] = sem_param_summary(ctx, p->params[i]);
		ds->out_param_count = p->out_param_count;
		ds->out_params = calloc(p->out_param_count ? p->out_param_count : 1, sizeof(ParamSummary));
		for (int i = 0; i < p->out_param_count; i++)
			ds->out_params[i] = sem_param_summary(ctx, p->out_params[i]);
		break;
	}
	case DECL_FUNC: {
		FuncDecl *f = d->data.func;
		ds->is_extern = f->is_extern;
		ds->param_count = f->param_count;
		ds->params = calloc(f->param_count ? f->param_count : 1, sizeof(ParamSummary));
		for (int i = 0; i < f->param_count; i++)
			ds->params[i] = sem_param_summary(ctx, f->params[i]);
		ds->return_type_count = f->return_type_count;
		ds->return_types = calloc(f->return_type_count ? f->return_type_count : 1, sizeof(TypeRef *));
		for (int i = 0; i < f->return_type_count; i++)
			ds->return_types[i] = sem_pool_type_copy(ctx, f->return_types[i]);
		break;
	}
	case DECL_SYS: {
		SysDecl *s = d->data.sys;
		ds->param_count = s->param_count;
		ds->params = calloc(s->param_count ? s->param_count : 1, sizeof(ParamSummary));
		for (int i = 0; i < s->param_count; i++)
			ds->params[i] = sem_param_summary(ctx, s->params[i]);
		break;
	}
	case DECL_FUNC_GROUP: {
		FuncGroup *g = d->data.func_group;
		ds->member_count = g->member_count;
		ds->member_names = calloc(g->member_count ? g->member_count : 1, sizeof(char *));
		for (int i = 0; i < g->member_count; i++)
			ds->member_names[i] = sem_dupz(g->member_names[i]);
		break;
	}
	case DECL_ARCHETYPE: {
		ArchetypeDecl *a = d->data.archetype;
		ds->field_count = a->field_count;
		ds->fields = calloc(a->field_count ? a->field_count : 1, sizeof(FieldSummary));
		for (int i = 0; i < a->field_count; i++) {
			ds->fields[i].name = a->fields[i]->name ? sem_dupz(a->fields[i]->name) : NULL;
			ds->fields[i].type = sem_pool_type_copy(ctx, a->fields[i]->type);
			ds->fields[i].kind = a->fields[i]->kind;
			ds->fields[i].meta_explicit = a->fields[i]->meta_explicit;
			ds->fields[i].loc = a->fields[i]->loc;
		}
		break;
	}
	case DECL_ENUM: {
		EnumDecl *e = d->data.enum_decl;
		ds->enum_variant_count = e->variant_count;
		ds->enum_variant_names = calloc(e->variant_count ? e->variant_count : 1, sizeof(char *));
		ds->enum_variant_values = calloc(e->variant_count ? e->variant_count : 1, sizeof(long));
		for (int i = 0; i < e->variant_count; i++) {
			ds->enum_variant_names[i] = sem_dupz(e->variant_names[i]);
			ds->enum_variant_values[i] = e->variant_values[i];
		}
		break;
	}
	case DECL_STATIC: {
		StaticDecl *s = d->data.static_decl;
		ds->static_kind = s->kind;
		ds->is_requirement = s->is_requirement;
		ds->static_pool_count = -1;
		if (s->kind == STATIC_KIND_ARRAY) {
			ds->static_type = sem_pool_type_copy(ctx, s->array.element_type);
			ds->static_size = s->array.size;
			ds->static_has_init = s->array.init != NULL;
		} else if (s->kind == STATIC_KIND_SCALAR) {
			ds->static_type = sem_pool_type_copy(ctx, s->scalar.type);
			ds->static_init =
			    s->scalar.init ? (SyntaxView){s->scalar.init->sn, s->scalar.init->sn_src} : (SyntaxView){NULL, NULL};
		} else { /* STATIC_KIND_ARCHETYPE — a pool */
			ds->static_field_count = s->archetype.field_count;
			ds->static_init_length_present = s->archetype.init_length != NULL;
			ds->static_fields = calloc(s->archetype.field_count ? s->archetype.field_count : 1, sizeof(SyntaxView));
			for (int i = 0; i < s->archetype.field_count; i++) {
				Expression *fv = s->archetype.field_values[i];
				ds->static_fields[i] = fv ? (SyntaxView){fv->sn, fv->sn_src} : (SyntaxView){NULL, NULL};
			}
			Expression *cnt = s->archetype.field_count > 0 ? s->archetype.field_values[0] : NULL;
			if (cnt && cnt->type == EXPR_LITERAL && cnt->data.literal.lexeme)
				ds->static_pool_count = atoi(cnt->data.literal.lexeme);
		}
		break;
	}
	case DECL_CONST: {
		ConstDecl *c = d->data.constant;
		ds->const_value_kind = c->value ? (int)c->value->type : -1;
		ds->const_value = c->value ? (SyntaxView){c->value->sn, c->value->sn_src} : (SyntaxView){NULL, NULL};
		ds->const_value_loc = c->value ? c->value->loc : (c->type_value ? c->type_value->loc : d->loc);
		if (c->value && c->value->type == EXPR_LITERAL && c->value->data.literal.lexeme)
			ds->const_value_lexeme = sem_dupz(c->value->data.literal.lexeme);
		if (c->value && c->value->type == EXPR_NAME && c->value->data.name.name)
			ds->const_value_name = sem_dupz(c->value->data.name.name);
		ds->const_type_value = sem_pool_type_copy(ctx, c->type_value);
		ds->const_decl_type = sem_pool_type_copy(ctx, c->decl_type);
		ds->is_transparent = c->is_transparent;
		break;
	}
	default:
		break;
	}
	return ds;
}

/* AST-kill step A: build the resolved decl-signature table from the fully resolved owned_prog
 * (post rename/qualify/tuple-expand). Additive — readers migrate onto it incrementally. */
static void build_decl_table(SemanticContext *ctx) {
	AstProgram *prog = ctx->owned_prog;
	if (!prog)
		return;
	ctx->decls = calloc(prog->decl_count ? prog->decl_count : 1, sizeof(DeclSummary *));
	ctx->decl_count = 0;
	for (int i = 0; i < prog->decl_count; i++)
		if (prog->decls[i])
			ctx->decls[ctx->decl_count++] = decl_summary_from(ctx, prog->decls[i]);
}

static void free_decl_table(SemanticContext *ctx) {
	for (int i = 0; i < ctx->decl_count; i++) {
		DeclSummary *ds = ctx->decls[i];
		if (!ds)
			continue;
		free(ds->name);
		free(ds->drop_type);
		for (int p = 0; p < ds->param_count; p++)
			free(ds->params[p].name);
		free(ds->params);
		for (int p = 0; p < ds->out_param_count; p++)
			free(ds->out_params[p].name);
		free(ds->out_params);
		free(ds->return_types);
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
	free(ctx->decls);
	ctx->decls = NULL;
	ctx->decl_count = 0;
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
TypeRef *semantic_type_from_view(SemanticContext *ctx, SyntaxView t) {
	return sem_view_type(ctx, t);
}

SemanticContext *semantic_analyze_cst(const SyntaxNode *root, const char *src) {
	SemanticContext *ctx = make_context();
	if (!root)
		return ctx;
	/* Reconstruct the analyzable AstProgram from the immutable syntax tree (+ module syntax
	 * trees), keep it alive on the context, and run the analysis core. */
	/* The qualify pass inside cst_to_program emits `module has no member` via this ctx. */
	g_sem_qualify_ctx = ctx;
	ctx->owned_prog = cst_to_program(root, src);
	g_sem_qualify_ctx = NULL;
	build_decl_table(ctx);
	/* AST-kill: the DeclTable holds only pooled types + dup'd names + TREE views — no pointers into
	 * the reconstructed AstProgram — and the SemModel channels were copied during qualify. So the
	 * AST is now dead weight; free it BEFORE analysis runs. Analysis + tycheck read only the table,
	 * the tree, and the side model. (Passing tests with the AST already gone proves the conversion.) */
	if (ctx->owned_prog) {
		ast_program_free(ctx->owned_prog);
		ctx->owned_prog = NULL;
	}
	ctx->prog = NULL;
	analyze_program_core(ctx);
	walk_matches(ctx, root, src); /* exhaustiveness: enums register during analyze, so check after */
	return ctx;
}

/* Test/helper entry: parse `src`, build the abstract `AstProgram` from the resulting lossless
 * syntax tree (via cst_to_program), then free the parse result + syntax tree. The reconstructed AstProgram owns
 * all its memory (every string is copied out of the source), so it outlives the syntax tree and can
 * be freed with ast_program_free. This is the only sanctioned way to obtain a `AstProgram` now that
 * the parser produces just the syntax tree — unit tests use it to validate cst_to_program faithfully
 * reconstructs each construct. Returns NULL on parse error. */
AstProgram *cst_to_program_from_source(const char *src) {
	ParseResult pr = parse_source(src);
	if (pr.error_count > 0 || !pr.syntax_root) {
		parse_result_free(&pr);
		return NULL;
	}
	AstProgram *prog = cst_to_program(pr.syntax_root, src);
	parse_result_free(&pr);
	return prog;
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

	/* free scopes and variables (but not TypeRef) */
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

	/* AST-kill: free the resolved decl-signature table (its TypeRefs live in owned_types, freed below). */
	free_decl_table(ctx);

	/* The analysis type pool is independent of the (now eagerly-freed) AstProgram — always release
	 * it. The DeclTable + view-driven analysis borrowed these for the context's lifetime. */
	for (int i = 0; i < ctx->owned_type_count; i++)
		type_ref_free(ctx->owned_types[i]);
	free(ctx->owned_types);
	/* owned_prog is normally freed right after build_decl_table (see semantic_analyze_cst); this
	 * guards the cst_to_program_from_source unit-test path where no context owns one. */
	if (ctx->owned_prog)
		ast_program_free(ctx->owned_prog);

	free(ctx);
}

int semantic_has_errors(SemanticContext *ctx) {
	return ctx->error_count > 0;
}

SemModel *sem_context_model(SemanticContext *ctx) {
	return ctx ? ctx->model : NULL;
}

SemHints *sem_context_hints(SemanticContext *ctx) {
	return ctx ? ctx->hints : NULL;
}

AstProgram *semantic_context_program(SemanticContext *ctx) {
	return ctx ? ctx->prog : NULL;
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

TypeRef *semantic_callable_type_alias(SemanticContext *ctx, const char *name) {
	return callable_type_alias_ref(ctx, name);
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
	if (!field || !field->type)
		return NULL;
	if (field->type->kind != TYPE_NAME)
		return NULL;
	return field->type->data.name;
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
