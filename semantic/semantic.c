#include "semantic.h"
#include "../cst/cst_view.h"
#include "../parser/parser.h"
#include "sem_hints.h"
#include "sem_model.h"
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
	int is_allocated; /* 1 once any alias for this shape has been alloc'd */
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
	int is_const;              /* 1 if an immutable local constant (`k :: e` / `k : T : e`) */
	SourceLoc loc;             /* declaration site, for the must-consume diagnostic */
} VariableInfo;

typedef struct {
	VariableInfo **vars;
	int var_count;
} Scope;

typedef struct {
	char *name;     /* group name (owned) */
	char **members; /* member func names (borrowed; pointers into CST FuncGroup) */
	int member_count;
	SourceLoc loc;
} GroupInfo;

struct SemanticContext {
	ArchetypeInfo **archetypes; /* one per unique shape */
	int archetype_count;

	AliasEntry **aliases; /* one per arche declaration */
	int alias_count;

	char **known_funcs;
	int known_func_count;

	GroupInfo *groups;
	int group_count;

	char **const_names;             /* compile-time constant names */
	const char **const_values;      /* literal lexeme strings */
	const char **const_value_types; /* each const's resolved type name ("int"/"float"/"char"/…) */
	int const_count;

	/* Nominal type aliases: `name :: <type>` (a `::` decl whose RHS names a type, not a
	 * literal). Identity is the name; `backing` is the RHS name (possibly another alias —
	 * resolved through the chain by resolve_type_alias). Erased to the backing before
	 * lowering, so codegen never sees an alias. */
	char **type_alias_names;
	char **type_alias_backings;
	int type_alias_count;

	Scope *scopes;
	int scope_count;

	int error_count;

	/* Track which archetype we're analyzing a sys for (NULL if not in sys) */
	const char *current_sys_archetype;

	/* Track the proc currently being analyzed (NULL if not in a proc body).
	 * Used by each_field to verify its RHS is an `archetype` parameter of this proc. */
	ProcDecl *current_proc;

	/* Track if inside proc/sys body (for alloc enforcement) */
	int in_body;

	/* 1 while analyzing the body of an `unsafe` proc/func; gates unsafe builtins
	 * (currently `syscall`) so they cannot be called from ordinary safe code. */
	int in_unsafe;

	/* 1 only while analyzing a call that sits in a statement / bind-RHS position (where an
	 * *action* is allowed). A proc or extern call is an action, not a value, so it may appear
	 * only there — never nested inside another expression. Set by STMT_EXPR / STMT_BIND /
	 * STMT_ASSIGN, captured-and-cleared at the top of EXPR_CALL so the call's own args are
	 * value positions. */
	int stmt_call_ok;

	/* AstProgram for looking up declarations */
	AstProgram *prog;

	/* CST path only: the AstProgram reconstructed from the CST (owned here; freed with the
	 * context). The side model borrows type-name strings that live in it, so it must
	 * outlive lowering+codegen — exactly like main.c keeps the parser-built `prog` alive. */
	AstProgram *owned_prog;

	/* MIGRATION: resolved types keyed by CST node id, kept out of the tree.
	 * Populated alongside Expression.resolved_type; lowering reads it from here. */
	SemModel *model;

	/* Editor-facing inferred facts keyed by CST node id (call-site param resolution).
	 * Populated during analysis; read by the analyzer's explicit view, not by lowering. */
	SemHints *hints;

	/* Structured diagnostics: appended by error()/lint_emit() so editors can iterate
	 * them after analysis (alongside the existing stderr prints, which stay for CLI). */
	SemDiag *diags;
	int diag_count, diag_cap;

	/* Ambient source position for error(): analyze_expression / analyze_statement set
	 * this to the node they're currently analyzing, so all the existing error() call
	 * sites in those frames pick up a real loc without manual migration. {0,0} = none. */
	SourceLoc current_loc;
};

/* ========== UTILITY FUNCTIONS ========== */

static int sig_part_cmp(const void *a, const void *b) {
	return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* Archetype identity is the *set* of component type names — unordered. We build one
 * "name:type:kind;" part per field and **sort** them, so `{a,b}` and `{b,a}` produce the
 * same signature and share a shape. (Codegen is keyed by archetype name + per-decl column
 * order, so the sort only affects shape dedup, not memory layout.) */
static char *compute_shape_signature(FieldDecl **fields, int field_count) {
	char **parts = field_count > 0 ? malloc(field_count * sizeof(char *)) : NULL;
	size_t total = 1;
	for (int i = 0; i < field_count; i++) {
		FieldDecl *f = fields[i];
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

static void register_func(SemanticContext *ctx, const char *name) {
	if (find_known_func(ctx, name)) {
		return; /* already registered */
	}
	ctx->known_funcs = realloc(ctx->known_funcs, (ctx->known_func_count + 1) * sizeof(char *));
	ctx->known_funcs[ctx->known_func_count] = malloc(strlen(name) + 1);
	strcpy(ctx->known_funcs[ctx->known_func_count], name);
	ctx->known_func_count++;
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
	}
	return 0;
}

static FuncDecl *find_func_decl_cst(AstProgram *prog, const char *name) {
	if (!prog)
		return NULL;
	for (int i = 0; i < prog->decl_count; i++) {
		Decl *d = prog->decls[i];
		if (d->kind == DECL_FUNC && strcmp(d->data.func->name, name) == 0)
			return d->data.func;
	}
	return NULL;
}

/* The first declared return type, or NULL. Single-return funcs have exactly one; this is
 * what scalar-position type inference uses (a multi-return func is only valid in a multi-bind). */
static TypeRef *func_first_return_type(const FuncDecl *f) {
	return (f && f->return_type_count > 0) ? f->return_types[0] : NULL;
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
 * --dump and the warm server share the same in-memory list without doubling output. */
static void diag_push(SemanticContext *ctx, int severity, int has_loc, SourceLoc loc, const char *name,
                      const char *msg) {
	if (!ctx)
		return;
	if (ctx->diag_count >= ctx->diag_cap) {
		ctx->diag_cap = ctx->diag_cap ? ctx->diag_cap * 2 : 16;
		ctx->diags = realloc(ctx->diags, (size_t)ctx->diag_cap * sizeof(SemDiag));
	}
	SemDiag *d = &ctx->diags[ctx->diag_count++];
	d->severity = severity ? 1 : 0;
	d->has_loc = has_loc ? 1 : 0;
	d->loc = loc;
	d->name = name ? name : "semantic";
	d->message = strdup(msg ? msg : "");
}

static void error(SemanticContext *ctx, const char *msg) {
	ctx->error_count++;
	SourceLoc loc = ctx->current_loc;
	int has_loc = loc.line != 0;
	if (has_loc)
		fprintf(stderr, "Semantic error at line %d, col %d: %s\n", loc.line, loc.column, msg);
	else
		fprintf(stderr, "Semantic error: %s\n", msg);
	fflush(stderr);
	diag_push(ctx, /*severity=*/1, has_loc, loc, "semantic", msg);
}

/* ========== LINT CONFIGURATION ========== */
/* Both lints enabled by default. CLI flags can disable or promote to error. */
static struct {
	int proc_could_be_func_enabled;
	int proc_could_be_func_werror;
	int proc_no_effect_enabled;
	int proc_no_effect_werror;
	int func_impure_enabled; /* a `func` body must be pure (no effects) */
	int func_impure_werror;
} g_lint_config = {
    .proc_could_be_func_enabled = 1,
    .proc_could_be_func_werror = 0,
    .proc_no_effect_enabled = 1,
    .proc_no_effect_werror = 0,
    .func_impure_enabled = 1,
    .func_impure_werror = 0,
};

void semantic_set_lint_proc_could_be_func(int enabled, int werror) {
	g_lint_config.proc_could_be_func_enabled = enabled;
	g_lint_config.proc_could_be_func_werror = werror;
}

void semantic_set_lint_proc_no_effect(int enabled, int werror) {
	g_lint_config.proc_no_effect_enabled = enabled;
	g_lint_config.proc_no_effect_werror = werror;
}

void semantic_set_lint_func_impure(int enabled, int werror) {
	g_lint_config.func_impure_enabled = enabled;
	g_lint_config.func_impure_werror = werror;
}

/* Emit a lint diagnostic. Promotes to a hard error if the corresponding
 * --Werror=... flag is set. */
static void lint_emit(SemanticContext *ctx, int werror, SourceLoc loc, const char *name, const char *msg) {
	const char *kind = werror ? "error" : "warning";
	fprintf(stderr, "Lint %s [%s] at line %d, col %d: %s\n", kind, name, loc.line, loc.column, msg);
	fflush(stderr);
	if (werror) {
		ctx->error_count++;
	}
	diag_push(ctx, werror ? 1 : 0, /*has_loc=*/1, loc, name, msg);
}

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
			 * insert) before its scope ends. Borrowed params are exempt. */
			if (var_is_opaque(ctx, v) && !v->is_param && !v->is_consumed) {
				char msg[256];
				snprintf(msg, sizeof(msg),
				         "opaque value '%s' not consumed before scope end (move/close/return/insert it)", v->name);
				error(ctx, msg);
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
	var->is_const = 0;
	var->loc.line = 0;
	var->loc.column = 0;

	scope->vars = realloc(scope->vars, (scope->var_count + 1) * sizeof(VariableInfo *));
	scope->vars[scope->var_count++] = var;
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

static void analyze_expression(SemanticContext *ctx, Expression *expr);
static void analyze_statement(SemanticContext *ctx, Statement *stmt);
static const char *resolve_expression_type(SemanticContext *ctx, Expression *expr);
static const char *lvalue_leftmost_name(Expression *expr);

/* By-reference aggregate param types: arrays are passed by reference (borrowed read-only by
 * default), so mutating one through a non-`move` param is a purity violation. Scalars are by
 * value (a freely-mutable local copy); opaque can't be indexed/assigned at all. */
static int type_is_byref_aggregate(const TypeRef *t) {
	return t && (t->kind == TYPE_ARRAY || t->kind == TYPE_SHAPED_ARRAY);
}

/* A `char[]` / `char[N]` array type — the kind `copy` can currently duplicate (a flat byte
 * buffer with a statically-known size when it's a local). */
static int type_is_char_array(const TypeRef *t) {
	const TypeRef *elem = NULL;
	if (!t)
		return 0;
	if (t->kind == TYPE_SHAPED_ARRAY)
		elem = t->data.shaped_array.element_type;
	else if (t->kind == TYPE_ARRAY)
		elem = t->data.array.element_type;
	else
		return 0;
	return elem && elem->kind == TYPE_NAME && elem->data.name && strcmp(elem->data.name, "char") == 0;
}

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
static const char *resolve_type_alias(SemanticContext *ctx, const char *name) {
	if (!ctx || !name)
		return name;
	for (int guard = 0; guard <= ctx->type_alias_count; guard++) {
		int found = 0;
		for (int i = 0; i < ctx->type_alias_count; i++) {
			if (strcmp(ctx->type_alias_names[i], name) == 0) {
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

/* 1 if `name` is a registered nominal type alias. */
static int is_type_alias(SemanticContext *ctx, const char *name) {
	if (!ctx || !name)
		return 0;
	for (int i = 0; i < ctx->type_alias_count; i++)
		if (strcmp(ctx->type_alias_names[i], name) == 0)
			return 1;
	return 0;
}

/* ---- constant / type-alias registration (pass 0 helpers) ----
 * A `name : [meta] : value` declaration is a *type alias* when its RHS denotes a type, or a
 * *value const* when its RHS denotes a value. The classification is by denotation (what the RHS
 * names), not by syntactic node kind — so `tau :: pi` (pi a value) is a value const, not an alias.
 * `name`/`backing`/`lexeme` are stored by pointer and must outlive ctx (CST or static strings). */

/* Register a nominal alias `name → backing`; redefinition must AGREE (same backing). */
static void register_type_alias(SemanticContext *ctx, const char *name, const char *backing) {
	for (int j = 0; j < ctx->type_alias_count; j++) {
		if (strcmp(ctx->type_alias_names[j], name) == 0) {
			if (strcmp(ctx->type_alias_backings[j], backing) != 0) {
				char msg[256];
				snprintf(msg, sizeof(msg), "type '%s' redefined with a different backing", name);
				error(ctx, msg);
			}
			return; /* agrees — share the one nominal type */
		}
	}
	ctx->type_alias_names = realloc(ctx->type_alias_names, (ctx->type_alias_count + 1) * sizeof(char *));
	ctx->type_alias_backings = realloc(ctx->type_alias_backings, (ctx->type_alias_count + 1) * sizeof(char *));
	ctx->type_alias_names[ctx->type_alias_count] = (char *)name;
	ctx->type_alias_backings[ctx->type_alias_count] = (char *)backing;
	ctx->type_alias_count++;
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
 * const reference resolve to its real type (so a float const is a float, not a default int). */
static void register_value_const(SemanticContext *ctx, const char *name, const char *lexeme, const char *type) {
	for (int j = 0; j < ctx->const_count; j++) {
		if (strcmp(ctx->const_names[j], name) == 0) {
			char msg[256];
			snprintf(msg, sizeof(msg), "constant '%s' already defined", name);
			error(ctx, msg);
			return;
		}
	}
	ctx->const_names = realloc(ctx->const_names, (ctx->const_count + 1) * sizeof(char *));
	ctx->const_values = realloc(ctx->const_values, (ctx->const_count + 1) * sizeof(const char *));
	ctx->const_value_types = realloc(ctx->const_value_types, (ctx->const_count + 1) * sizeof(const char *));
	ctx->const_names[ctx->const_count] = (char *)name;
	ctx->const_values[ctx->const_count] = lexeme;
	ctx->const_value_types[ctx->const_count] = type;
	ctx->const_count++;
}

/* 1 if the bare name `r` currently denotes a type (a primitive, opaque, or a registered alias). */
static int name_denotes_type(SemanticContext *ctx, const char *r) {
	return is_primitive_type_name(r) || strcmp(r, "opaque") == 0 || is_type_alias(ctx, r);
}

/* For a typed value const `name : T : <literal>`, check the literal is compatible with the declared
 * type T. Only concrete primitive types are checked; a clear category mismatch (a float literal for
 * an int, a string for a number) is an error. So an explicit annotation is no longer inert. */
static void check_const_literal_type(SemanticContext *ctx, ConstDecl *c) {
	if (!c || !c->decl_type || c->decl_type->kind != TYPE_NAME)
		return; /* only concrete named declared types */
	if (!c->value || c->value->type != EXPR_LITERAL)
		return; /* only literal RHS (a name RHS is a value-const chain, checked elsewhere) */
	const char *want = resolve_type_alias(ctx, normalize_type_name(c->decl_type->data.name));
	if (!is_primitive_type_name(want))
		return; /* non-primitive declared type: out of scope for the literal check */
	const char *got = resolve_expression_type(ctx, c->value); /* "int" / "float" / "char" / "char_array" */
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
		char msg[256];
		snprintf(msg, sizeof(msg), "constant '%s' is declared `%s` but its value is a %s literal", c->name, want,
		         strcmp(got, "char_array") == 0 ? "string" : got);
		error(ctx, msg);
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
		char msg[256];
		snprintf(msg, sizeof(msg),
		         "the meta-type `type` is only valid as a declaration's type (`name : type : T`); "
		         "type parameters (generics) are not supported yet (%s)",
		         where);
		error(ctx, msg);
		return 1;
	}
	return 0;
}

static const char *resolve_expression_type(SemanticContext *ctx, Expression *expr) {
	if (!expr)
		return NULL;

	switch (expr->type) {
	case EXPR_LITERAL: {
		/* Infer type from lexeme format */
		const char *lex = expr->data.literal.lexeme;

		/* String literal: char array */
		if (lex[0] == '"') {
			/* Type is char array - store length in a way semantic can track */
			/* For now, return a marker that codegen can recognize */
			return "char_array";
		}

		/* Char literal: single-quoted character */
		if (lex[0] == '\'') {
			return "char";
		}

		/* Numeric literal */
		if (strchr(lex, '.') || strchr(lex, 'e') || strchr(lex, 'E')) {
			return "float"; /* Will be converted to double by codegen */
		}
		return "int";
	}

	case EXPR_NAME: {
		const char *name = expr->data.name.name;
		VariableInfo *var = find_variable(ctx, name);
		if (var) {
			if (var->type) {
				if (var->type->kind == TYPE_HANDLE)
					return var->type->data.handle.archetype_name;
				if (var->type->kind == TYPE_NAME)
					return resolve_type_alias(ctx, normalize_type_name(var->type->data.name));
			}
			/* Fallback to inferred type */
			if (var->inferred_type) {
				return resolve_type_alias(ctx, normalize_type_name(var->inferred_type));
			}
		}
		/* A reference to a top-level value const resolves to that const's type — so a float const
		 * is a float, not a defaulted int. (A local var of the same name shadows it, handled above.) */
		const char *ct = value_const_type(ctx, name);
		if (ct)
			return ct;
		/* Check if it's an archetype being referenced */
		ArchetypeInfo *arch = find_archetype(ctx, name);
		if (arch) {
			return name; /* Type is the archetype name */
		}
		return NULL;
	}

	case EXPR_FIELD: {
		/* Handle metadata properties on arrays and archetypes */
		if (strcmp(expr->data.field.field_name, "length") == 0 ||
		    strcmp(expr->data.field.field_name, "max_length") == 0) {
			return "int";
		}

		/* Field access type is the field's type */
		if (expr->data.field.base->type == EXPR_NAME) {
			const char *base_name = expr->data.field.base->data.name.name;
			const char *field_name = expr->data.field.field_name;

			ArchetypeInfo *arch = find_archetype(ctx, base_name);
			if (!arch) {
				VariableInfo *var = find_variable(ctx, base_name);
				if (var && var->archetype_name) {
					arch = find_archetype(ctx, var->archetype_name);
				}
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
		}
		return NULL;
	}

	case EXPR_INDEX: {
		/* An index yields the base's ELEMENT type. A char buffer resolves to "char_array"
		 * (unbounded) or "char" (sized); indexing either gives a single `char`. Without this
		 * reduction an unbounded `char[]` index would inherit "char_array" and be treated as a
		 * string (i8*) at call sites (e.g. printf). */
		const char *base_type = resolve_expression_type(ctx, expr->data.index.base);
		if (base_type && strcmp(base_type, "char_array") == 0)
			return "char";
		return base_type;
	}

	case EXPR_BINARY: {
		/* Comparison operators always return int (boolean result) */
		if (expr->data.binary.op >= OP_EQ && expr->data.binary.op <= OP_GTE) {
			return "int";
		}

		/* Infer from operands - for now, promote to float if either side is float */
		const char *left_type = resolve_expression_type(ctx, expr->data.binary.left);
		const char *right_type = resolve_expression_type(ctx, expr->data.binary.right);

		/* Promote to double if either operand is double */
		if (left_type && strcmp(left_type, "double") == 0)
			return "double";
		if (right_type && strcmp(right_type, "double") == 0)
			return "double";
		/* Fall back to float if either is float */
		if (left_type && strcmp(left_type, "float") == 0)
			return "float";
		if (right_type && strcmp(right_type, "float") == 0)
			return "float";
		return left_type ? left_type : right_type;
	}

	case EXPR_UNARY: {
		return resolve_expression_type(ctx, expr->data.unary.operand);
	}

	case EXPR_CALL: {
		const char *func_name = NULL;
		if (expr->data.call.callee && expr->data.call.callee->type == EXPR_NAME) {
			func_name = expr->data.call.callee->data.name.name;
		}
		if (!func_name)
			return NULL;

		/* Width-type cast i64(x): result type is the target width name. */
		if (is_width_int_name(func_name))
			return func_name;

		/* `insert(table<X>, …)` yields a handle into X's table (i64). Resolving it
		 * lets `let h := insert(...)` carry a handle type, so copies like
		 * `let alias := h` inherit it instead of defaulting to int. */
		if (strcmp(func_name, "insert") == 0)
			return "handle";

		if (!ctx->prog)
			return NULL;

		/* If this name is a group, pick the matching member by static arg types. */
		GroupInfo *gi = find_group(ctx, func_name);
		if (gi) {
			for (int m = 0; m < gi->member_count; m++) {
				FuncDecl *fd = find_func_decl_cst(ctx->prog, gi->members[m]);
				if (!fd)
					continue;
				if (fd->param_count != expr->data.call.arg_count)
					continue;
				int ok = 1;
				for (int j = 0; j < expr->data.call.arg_count; j++) {
					const char *rt = resolve_expression_type(ctx, expr->data.call.args[j]);
					if (!rt) {
						ok = 0;
						break;
					}
					TypeRef *pt = fd->params[j]->type;
					if (!pt || pt->kind != TYPE_NAME) {
						ok = 0;
						break;
					}
					const char *pn = normalize_type_name(pt->data.name);
					if (strcmp(pn, normalize_type_name(rt)) != 0) {
						ok = 0;
						break;
					}
				}
				TypeRef *frt = func_first_return_type(fd);
				if (ok && frt && frt->kind == TYPE_NAME) {
					return normalize_type_name(frt->data.name);
				}
			}
			return NULL;
		}

		/* Plain func/proc path: a returning proc yields its first return type exactly like a func. */
		for (int i = 0; i < ctx->prog->decl_count; i++) {
			Decl *decl = ctx->prog->decls[i];
			TypeRef *rt = NULL;
			if (decl->kind == DECL_FUNC && decl->data.func->name && strcmp(decl->data.func->name, func_name) == 0) {
				rt = func_first_return_type(decl->data.func);
			} else if (decl->kind == DECL_PROC && decl->data.proc->name &&
			           strcmp(decl->data.proc->name, func_name) == 0) {
				rt = (decl->data.proc->return_type_count > 0) ? decl->data.proc->return_types[0] : NULL;
			} else {
				continue;
			}
			if (!rt)
				return NULL;
			if (rt->kind == TYPE_HANDLE)
				return rt->data.handle.archetype_name;
			if (rt->kind == TYPE_NAME)
				return resolve_type_alias(ctx, normalize_type_name(rt->data.name));
			if (rt->kind == TYPE_ARRAY)
				return "char_array"; /* returning char[] (raw byte view) */
			if (rt->kind == TYPE_OPAQUE)
				return "opaque"; /* foreign resource value (pointer-width i64) */
			return NULL;
		}
		return NULL;
	}

	case EXPR_ALLOC: {
		/* Type is the archetype being allocated */
		return expr->data.alloc.archetype_name;
	}

	case EXPR_STRING: {
		/* String literal: char array */
		return "char_array";
	}

	default:
		return NULL;
	}
}

/* The *nominal* type of an expression for distinctness checks — the unresolved alias name
 * (e.g. "file"), NOT the backing. NULL when the expression has no known nominal alias
 * (a literal, a raw primitive, an unknown). Distinct from resolve_expression_type, which
 * resolves through to the backing for ops/codegen. */
static const char *nominal_type_of_expr(SemanticContext *ctx, Expression *e) {
	if (!e)
		return NULL;
	/* `move x` / `copy x` are transparent markers — their nominal type is the operand's. */
	if (e->type == EXPR_UNARY && (e->data.unary.op == UNARY_MOVE || e->data.unary.op == UNARY_COPY))
		return nominal_type_of_expr(ctx, e->data.unary.operand);
	if (e->type == EXPR_NAME) {
		VariableInfo *v = find_variable(ctx, e->data.name.name);
		return v ? v->nominal_type : NULL;
	}
	if (e->type == EXPR_CALL && e->data.call.callee && e->data.call.callee->type == EXPR_NAME) {
		FuncDecl *fd = find_func_decl_cst(ctx->prog, e->data.call.callee->data.name.name);
		TypeRef *frt = func_first_return_type(fd);
		if (frt && frt->kind == TYPE_NAME && is_type_alias(ctx, frt->data.name))
			return frt->data.name;
	}
	return NULL;
}

/* ========== EXPRESSION ANALYSIS ========== */

static void analyze_expression(SemanticContext *ctx, Expression *expr) {
	if (!expr)
		return;
	/* Ambient loc for any error() call in this frame — see SemanticContext.current_loc. */
	SourceLoc saved_loc = ctx->current_loc;
	if (expr->loc.line)
		ctx->current_loc = expr->loc;

	switch (expr->type) {
	case EXPR_LITERAL:
		/* literals are always valid */
		break;

	case EXPR_STRING:
		/* string literals are always valid */
		break;

	case EXPR_ARRAY_LITERAL:
		/* Array literals have no name to resolve here; their elements are handled where the
		 * literal is consumed (binding / call / store). */
		break;

	case EXPR_NAME: {
		const char *name = expr->data.name.name;

		/* Check if it's a known function, variable, archetype, or constant */
		int is_known_func = find_known_func(ctx, name);
		VariableInfo *name_var = find_variable(ctx, name);
		int is_var = name_var != NULL;
		int is_arch = find_archetype(ctx, name) != NULL;
		int is_const = semantic_get_const_value(ctx, name) != NULL;

		if (!is_known_func && !is_var && !is_arch && !is_const) {
			char msg[256];
			snprintf(msg, sizeof(msg), "Undefined symbol '%s'", name);
			error(ctx, msg);
		} else if (is_var && name_var->is_consumed) {
			/* Use-after-consume: this binding was passed to a consume parameter earlier.
			 * NOTE: v1 limitation — tracking is function-scope only (not branch-sensitive).
			 * A consume inside an if-branch marks the binding consumed for the entire rest
			 * of the proc body, which may over-reject some valid code. Revisit if needed. */
			char msg[256];
			snprintf(msg, sizeof(msg), "use of consumed handle '%s'", name);
			error(ctx, msg);
		}
		break;
	}

	case EXPR_FIELD: {
		/* expr.field - need to know what expr resolves to */
		analyze_expression(ctx, expr->data.field.base);

		/* Handle nested field access: archetype.tuple_field.component → archetype.tuple_field_component */
		if (expr->data.field.base->type == EXPR_FIELD) {
			Expression *inner_field = expr->data.field.base;
			const char *component_name = expr->data.field.field_name;
			const char *tuple_base_name = inner_field->data.field.field_name;

			if (inner_field->data.field.base->type == EXPR_NAME) {
				const char *arch_var_name = inner_field->data.field.base->data.name.name;

				/* Find the archetype */
				ArchetypeInfo *arch = find_archetype(ctx, arch_var_name);
				VariableInfo *var = NULL;
				if (!arch) {
					var = find_variable(ctx, arch_var_name);
					if (var && var->archetype_name) {
						arch = find_archetype(ctx, var->archetype_name);
					}
				}

				if (arch) {
					/* Check if tuple_base exists and has component */
					char expanded_name[256];
					snprintf(expanded_name, sizeof(expanded_name), "%s_%s", tuple_base_name, component_name);

					if (find_field(arch, expanded_name)) {
						/* Replace nested field access with direct access to expanded name */
						expr->data.field.base = inner_field->data.field.base;
						expr->data.field.field_name = malloc(strlen(expanded_name) + 1);
						strcpy(expr->data.field.field_name, expanded_name);
						/* Don't analyze the old nested structure further */
						break;
					}
				}
			}
		}

		/* if base is a simple name, check if it's an archetype or a variable referring to one */
		if (expr->data.field.base->type == EXPR_NAME) {
			const char *base_name = expr->data.field.base->data.name.name;
			const char *field_name = expr->data.field.field_name;

			/* first check if it's directly an archetype */
			ArchetypeInfo *arch = find_archetype(ctx, base_name);

			if (!arch) {
				/* try to find it as a variable */
				VariableInfo *var = find_variable(ctx, base_name);
				if (!var) {
					char msg[256];
					snprintf(msg, sizeof(msg), "Undefined variable '%s'", base_name);
					error(ctx, msg);
					break;
				}
				/* `h.comp` — reading a component through a HANDLE value is not supported
				 * (a handle is a lifetime token, not a row view); use column access
				 * `Foo.comp[i]`. An archetype-struct parameter (`p: Player`) is a different
				 * thing and stays valid. Handles arrive two ways: an annotated `handle<Foo>`
				 * (TYPE_HANDLE) or an inferred `:= insert(...)` (inferred_type "handle").
				 * Reject cleanly here (else codegen emits a bare token). */
				int base_is_handle = (var->type && var->type->kind == TYPE_HANDLE) ||
				                     (var->inferred_type && strcmp(var->inferred_type, "handle") == 0);
				if (base_is_handle) {
					const char *an =
					    (var->type && var->type->kind == TYPE_HANDLE) ? var->type->data.handle.archetype_name : NULL;
					char msg[256];
					snprintf(msg, sizeof(msg),
					         "cannot read component '%s' through handle '%s': a handle is a lifetime token, "
					         "not a row view — use column access `%s.%s[i]`",
					         field_name, base_name, an ? an : "the archetype", field_name);
					error(ctx, msg);
					break;
				}
				/* check if variable refers to an archetype entry */
				if (var->archetype_name) {
					arch = find_archetype(ctx, var->archetype_name);
				}
			}

			/* now check if field exists on this archetype */
			if (arch) {
				/* First try direct field access */
				FieldInfo *found_field = find_field(arch, field_name);
				if (!found_field) {
					/* Try tuple component access: pos.x → pos_x */
					char expanded_name[256];
					snprintf(expanded_name, sizeof(expanded_name), "%s_%s", field_name, field_name);
					/* Actually this was wrong - let me try a different approach */

					/* Check if this is a tuple base: look for fields named field_name_* */
					int is_tuple_base = 0;
					for (int i = 0; i < arch->field_count; i++) {
						if (strncmp(arch->fields[i]->name, field_name, strlen(field_name)) == 0 &&
						    arch->fields[i]->name[strlen(field_name)] == '_') {
							is_tuple_base = 1;
							break;
						}
					}

					if (is_tuple_base) {
						/* Mark this expression as a tuple field access (no substitution needed yet) */
						/* The codegen will handle expanding tuple operations */
					} else if (expr->data.field.base->type == EXPR_NAME) {
						/* Try tuple component access with base_name: p.pos where p is archetype → pos_x, pos_y */
						char expanded_name2[256];
						snprintf(expanded_name2, sizeof(expanded_name2), "%s_%s", base_name, field_name);
						found_field = find_field(arch, expanded_name2);

						if (found_field) {
							/* Replace the field expression with expanded name */
							expr->data.field.field_name = malloc(strlen(expanded_name2) + 1);
							strcpy(expr->data.field.field_name, expanded_name2);
						} else {
							char msg[256];
							snprintf(msg, sizeof(msg), "Archetype '%s' has no field '%s'",
							         archetype_any_alias(ctx, arch), field_name);
							error(ctx, msg);
						}
					} else {
						char msg[256];
						snprintf(msg, sizeof(msg), "Archetype '%s' has no field '%s'", archetype_any_alias(ctx, arch),
						         field_name);
						error(ctx, msg);
					}
				}
			}
		}
		break;
	}

	case EXPR_INDEX:
		analyze_expression(ctx, expr->data.index.base);
		for (int i = 0; i < expr->data.index.index_count; i++) {
			analyze_expression(ctx, expr->data.index.indices[i]);
		}
		break;

	case EXPR_BINARY:
		analyze_expression(ctx, expr->data.binary.left);
		analyze_expression(ctx, expr->data.binary.right);
		break;

	case EXPR_UNARY:
		analyze_expression(ctx, expr->data.unary.operand);
		/* `move x` transfers ownership: mark x consumed (use-after-move is an error).
		 * TODO(implicit-move): infer `move` at a binding's last use / self-rebind so the keyword
		 * can be omitted (FBIP — Koka/Roc/Hylo do this via uniqueness/last-use analysis). */
		if (expr->data.unary.op == UNARY_MOVE && expr->data.unary.operand->type == EXPR_NAME) {
			VariableInfo *mv = find_variable(ctx, expr->data.unary.operand->data.name.name);
			if (mv) {
				/* Can't move out of a borrow: a read-only (non-`move`) array parameter is
				 * borrowed by reference, not owned. Moving it would hand the caller's buffer to a
				 * callee that may mutate it — a purity leak. Owned bindings (locals, `move` params)
				 * move freely. */
				if (mv->is_param && !mv->is_own && type_is_byref_aggregate(mv->type)) {
					char msg[256];
					snprintf(msg, sizeof(msg),
					         "cannot move read-only parameter '%s' — it is borrowed, not owned; take "
					         "it `move` to own it, or copy it into a local",
					         expr->data.unary.operand->data.name.name);
					error(ctx, msg);
				}
				mv->is_consumed = 1;
			}
		}
		/* `copy x` duplicates x into a fresh owned buffer; x is NOT consumed (caller keeps it).
		 * Opaque is non-copyable (move-only). Duplication is currently implemented only for local
		 * `char[N]` buffers; other array kinds (forwarded params, non-char arrays) are not yet
		 * supported — error rather than silently aliasing (which would break copy semantics). */
		if (expr->data.unary.op == UNARY_COPY && expr->data.unary.operand->type == EXPR_NAME) {
			VariableInfo *cv = find_variable(ctx, expr->data.unary.operand->data.name.name);
			if (cv) {
				if (var_is_opaque(ctx, cv)) {
					char msg[256];
					snprintf(msg, sizeof(msg), "cannot copy opaque value '%s' — it is move-only; use `move`",
					         expr->data.unary.operand->data.name.name);
					error(ctx, msg);
				} else if (type_is_byref_aggregate(cv->type) && !(type_is_char_array(cv->type) && !cv->is_param)) {
					char msg[256];
					snprintf(msg, sizeof(msg),
					         "copy of '%s' is not yet supported (only a local `char[N]` buffer can be "
					         "copied); copy it into a local first, or use `move`",
					         expr->data.unary.operand->data.name.name);
					error(ctx, msg);
				}
			}
		}
		break;

	case EXPR_CALL: {
		/* A proc/extern call is an action, allowed only as a statement or a whole bind/assign RHS.
		 * Capture whether THIS call is in such a position, then clear the flag so the call's own
		 * arguments (and any nested calls) are value positions. */
		int call_stmt_ok = ctx->stmt_call_ok;
		ctx->stmt_call_ok = 0;
		/* Width-type cast: i64(x), u8(x), etc. The callee is a type name, not a
		 * function — analyze only the argument(s) and stop. */
		if (expr->data.call.callee && expr->data.call.callee->type == EXPR_NAME &&
		    is_width_int_name(expr->data.call.callee->data.name.name)) {
			for (int i = 0; i < expr->data.call.arg_count; i++)
				analyze_expression(ctx, expr->data.call.args[i]);
			break;
		}
		analyze_expression(ctx, expr->data.call.callee);
		for (int i = 0; i < expr->data.call.arg_count; i++) {
			analyze_expression(ctx, expr->data.call.args[i]);
		}
		const char *func_name = NULL;
		if (expr->data.call.callee && expr->data.call.callee->type == EXPR_NAME) {
			func_name = expr->data.call.callee->data.name.name;
		}
		if (!func_name)
			break;

		/* insert(Foo, v1, …) moves its value args into the pool — counts as consumption
		 * for any opaque-backed value argument. Arg 0 is the archetype name, skip it. */
		if (strcmp(func_name, "insert") == 0) {
			for (int i = 1; i < expr->data.call.arg_count; i++) {
				if (expr->data.call.args[i] && expr->data.call.args[i]->type == EXPR_NAME) {
					VariableInfo *iv = find_variable(ctx, expr->data.call.args[i]->data.name.name);
					if (iv && var_is_opaque(ctx, iv))
						iv->is_consumed = 1;
				}
			}
		}

		/* Unsafe-builtin gate: `syscall` bypasses bounds/alloc/handle safety, so it
		 * may only be called from an explicitly-marked `unsafe` proc/func. */
		if (strcmp(func_name, "syscall") == 0 && !ctx->in_unsafe) {
			error(ctx, "`syscall` may only be called from an `unsafe` proc or func");
			break;
		}

		GroupInfo *gi = find_group(ctx, func_name);
		if (!gi) {
			/* Not a group: check extern-type argument distinctness at call sites.
			 * Only applies when the callee is an extern proc or extern func with
			 * extern-type parameters. */
			if (ctx->prog) {
				for (int i = 0; i < ctx->prog->decl_count; i++) {
					Decl *d = ctx->prog->decls[i];
					if (!d)
						continue;

					/* Collect param list from DECL_FUNC or DECL_PROC */
					int param_count = 0;
					Parameter **params = NULL;
					int is_extern = 0;

					if (d->kind == DECL_FUNC && d->data.func && d->data.func->name &&
					    strcmp(d->data.func->name, func_name) == 0) {
						param_count = d->data.func->param_count;
						params = d->data.func->params;
						is_extern = d->data.func->is_extern;
					} else if (d->kind == DECL_PROC && d->data.proc && d->data.proc->name &&
					           strcmp(d->data.proc->name, func_name) == 0) {
						param_count = d->data.proc->param_count;
						params = d->data.proc->params;
						is_extern = d->data.proc->is_extern;
					} else {
						continue;
					}

					/* Value/action boundary: a `proc`, or an `extern` (a foreign-bodied proc), is an
					 * action — usable only as a standalone statement or the whole RHS of a bind/assign,
					 * never nested inside another expression. A `func` is a value and may appear
					 * anywhere. */
					if ((d->kind == DECL_PROC || is_extern) && !call_stmt_ok) {
						char msg[320];
						snprintf(msg, sizeof(msg),
						         "a %s call is an action, not a value — bind it (`x := %s(...)`) or call "
						         "it as a statement; it can't appear inside an expression",
						         is_extern ? "extern" : "proc", func_name);
						error(ctx, msg);
					}

					/* Editor explicit view: record the resolved parameter (name + `own`) each
					 * argument binds to, keyed by the argument's CST node. This needs the call
					 * resolved to a signature, so it can't be a syntactic lens. (Overloaded-group
					 * calls resolve elsewhere — see the group branch — and are a follow-up.) */
					{
						int ac = expr->data.call.arg_count;
						int n = param_count < ac ? param_count : ac;
						for (int j = 0; j < n; j++) {
							Parameter *p = params[j];
							Expression *a = expr->data.call.args[j];
							if (p && p->name && a && a->cst_id)
								sem_hints_set_param(ctx->hints, a->cst_id - 1, p->name, p->is_own);
						}
					}

					/* An `own` parameter takes ownership: a named binding handed to it must be
					 * passed `move x` (donate — consumed) or `copy x` (duplicate — kept); no silent
					 * copy. `move`/`copy` are UNARY exprs (handled in analyze_expression), so a bare
					 * name reaching here is the error. (Rvalues — e.g. a call result — have no binding
					 * to provide, so neither is required there.) For opaque, only `move` works (it is
					 * non-copyable). Runs for BOTH extern and non-extern callees.
					 * TODO(implicit-move): when the bare binding is provably dead at this call (its
					 * last use), infer `move` instead of erroring (FBIP). */
					{
						int ac = expr->data.call.arg_count;
						int n = param_count < ac ? param_count : ac;
						for (int j = 0; j < n; j++) {
							Parameter *p = params[j];
							if (!p || !p->is_own || !expr->data.call.args[j])
								continue;
							Expression *a = expr->data.call.args[j];
							if (a->type == EXPR_NAME) {
								VariableInfo *cv = find_variable(ctx, a->data.name.name);
								if (cv) {
									char msg[256];
									snprintf(msg, sizeof(msg),
									         "value '%s' must be moved or copied into `own` parameter '%s' of '%s' "
									         "(write `move %s` or `copy %s`)",
									         a->data.name.name, p->name ? p->name : "?", func_name, a->data.name.name,
									         a->data.name.name);
									error(ctx, msg);
								}
							}
						}
					}

					if (!is_extern)
						break; /* non-extern: no extern-type distinctness to check */

					/* For each argument, if the formal param is a foreign handle
					 * (handle(X) where X is an extern table), the argument's resolved
					 * type must be exactly the same extern handle. Integer literal 0
					 * is accepted as a null handle. */
					int arg_count = expr->data.call.arg_count;
					int check_count = param_count < arg_count ? param_count : arg_count;
					for (int j = 0; j < check_count; j++) {
						Parameter *p = params[j];
						if (!p)
							continue;

						/* Nominal distinctness: an alias-typed formal rejects an argument whose
						 * nominal type is a *different* alias (file vs socket), even though both
						 * back to opaque. Untyped/unknown args (nominal NULL) are lenient. */
						if (p->type && p->type->kind == TYPE_NAME && is_type_alias(ctx, p->type->data.name)) {
							const char *arg_nominal = nominal_type_of_expr(ctx, expr->data.call.args[j]);
							if (arg_nominal && strcmp(arg_nominal, p->type->data.name) != 0) {
								char msg[256];
								snprintf(msg, sizeof(msg),
								         "type mismatch: '%s' parameter '%s' expects '%s' but got '%s'", func_name,
								         p->name ? p->name : "?", p->type->data.name, arg_nominal);
								error(ctx, msg);
							}
						}
					}
					break; /* found the callee decl */
				}
			}
			break; /* not a group; nothing further to diagnose here */
		}

		/* Only diagnose when every arg has a concrete primitive type. */
		int can_diagnose = 1;
		for (int j = 0; j < expr->data.call.arg_count; j++) {
			const char *rt = resolve_expression_type(ctx, expr->data.call.args[j]);
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
		if (!can_diagnose)
			break;

		int match_count = 0;
		for (int m = 0; m < gi->member_count; m++) {
			FuncDecl *fd = find_func_decl_cst(ctx->prog, gi->members[m]);
			if (!fd || fd->param_count != expr->data.call.arg_count)
				continue;
			int ok = 1;
			for (int j = 0; j < expr->data.call.arg_count; j++) {
				const char *rt = resolve_expression_type(ctx, expr->data.call.args[j]);
				TypeRef *pt = fd->params[j]->type;
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
		if (match_count == 0) {
			char msg[256];
			snprintf(msg, sizeof(msg), "no member of group '%s' matches the argument types", func_name);
			error(ctx, msg);
		} else if (match_count > 1) {
			char msg[256];
			snprintf(msg, sizeof(msg), "call to '%s' is ambiguous among group members", func_name);
			error(ctx, msg);
		}
		break;
	}

	case EXPR_ALLOC: {
		/* alloc only allowed at top-level, not inside proc/sys */
		if (ctx->in_body) {
			error(ctx, "alloc only allowed at top-level, not inside proc or sys body");
			break;
		}

		/* alloc count must be a literal for static allocation (dynamic not yet supported) */
		if (expr->data.alloc.field_count > 0 && expr->data.alloc.field_values[0]) {
			Expression *count_expr = expr->data.alloc.field_values[0];
			if (count_expr->type != EXPR_LITERAL) {
				error(ctx, "alloc count must be a literal; dynamic counts not yet supported");
				break;
			}
		}

		ArchetypeInfo *alloc_shape = find_archetype(ctx, expr->data.alloc.archetype_name);
		if (!alloc_shape) {
			char msg[256];
			snprintf(msg, sizeof(msg), "Undefined archetype '%s'", expr->data.alloc.archetype_name);
			error(ctx, msg);
		} else if (alloc_shape->is_allocated) {
			char msg[256];
			snprintf(msg, sizeof(msg), "Shape already allocated (alias '%s' shares shape with an earlier alloc)",
			         expr->data.alloc.archetype_name);
			error(ctx, msg);
		} else {
			alloc_shape->is_allocated = 1;
		}
		for (int i = 0; i < expr->data.alloc.field_count; i++) {
			analyze_expression(ctx, expr->data.alloc.field_values[i]);
		}
		break;
	}
	}

	/* Resolve the type of this expression and record it in the side model, keyed by
	 * the CST node. The model owns its copy; the tree is not mutated (lowering reads
	 * the model, not Expression.resolved_type). Call resolve unconditionally to keep
	 * its side effects; parser-built expressions always link to a CST node. */
	const char *resolved = resolve_expression_type(ctx, expr);
	if (ctx->model && expr->cst_id)
		sem_model_set_expr_type(ctx->model, expr->cst_id - 1, resolved);

	ctx->current_loc = saved_loc;
}

/* ========== STATEMENT ANALYSIS ========== */

static void analyze_statement(SemanticContext *ctx, Statement *stmt) {
	if (!stmt)
		return;
	SourceLoc saved_loc = ctx->current_loc;
	if (stmt->loc.line)
		ctx->current_loc = stmt->loc;

	switch (stmt->type) {
	case STMT_BIND: {
		/* A local constant (`k :: e` / `k : T : e`) whose RHS denotes a TYPE is a local nominal
		 * type alias: register it (globally, by nominal name — nominal identity is global) and
		 * erase it (no runtime value). A constant whose RHS is a value falls through to the normal
		 * binding path below and is marked immutable. */
		if (stmt->data.bind_stmt.is_const) {
			BindStmt *b = &stmt->data.bind_stmt;
			const char *backing = NULL;
			if (b->type_value)
				backing = type_backing_name(b->type_value);
			else if (b->value && b->value->type == EXPR_NAME && name_denotes_type(ctx, b->value->data.name.name))
				backing = b->value->data.name.name;
			if (backing || b->type_value) {
				/* This constant's RHS is a type — a local nominal type alias. */
				if (!backing) {
					error(ctx, "a local type alias backing must be a type name or `opaque`");
				} else {
					register_type_alias(ctx, b->name, backing);
					const char *resolved = resolve_type_alias(ctx, b->name);
					if (!is_primitive_type_name(resolved) && strcmp(resolved, "opaque") != 0) {
						char msg[256];
						snprintf(msg, sizeof(msg), "type alias '%s' has unknown backing type '%s'", b->name, resolved);
						error(ctx, msg);
					}
				}
				b->is_type_alias = 1;
				if (ctx->model && stmt->cst_id)
					sem_model_set_bind_alias(ctx->model, stmt->cst_id - 1);
				break; /* compile-time only: no runtime binding */
			}
			/* else: a value const — fall through to the normal binding path (marked const below). */
		}
		/* `archetype` is only valid as a parameter type. */
		if (stmt->data.bind_stmt.type && stmt->data.bind_stmt.type->kind == TYPE_ARCHETYPE) {
			error(ctx, "`archetype` is only valid as a parameter type");
			break;
		}
		if (reject_meta_type(ctx, stmt->data.bind_stmt.type, "variable type"))
			break;
		/* For multivalue let with function calls, add out param variables BEFORE analyzing the call */
		int is_multivalue_call = (stmt->data.bind_stmt.name_count > 0 && stmt->data.bind_stmt.names &&
		                          stmt->data.bind_stmt.value && stmt->data.bind_stmt.value->type == EXPR_CALL);

		if (is_multivalue_call) {
			/* Add all variables first so out parameters can reference them */
			for (int i = 0; i < stmt->data.bind_stmt.name_count; i++) {
				const char *var_name = stmt->data.bind_stmt.names[i];
				if (var_name && strcmp(var_name, "_") != 0) {
					add_variable(ctx, var_name, NULL);
				}
			}
			/* Now analyze the call expression after variables are defined. A multi-bind RHS IS the
			 * whole call, so a proc/extern there is allowed (it's an action whose result is bound). */
			ctx->stmt_call_ok = 1;
			analyze_expression(ctx, stmt->data.bind_stmt.value);
			ctx->stmt_call_ok = 0;
		} else {
			/* Single-value let or non-call multivalue expressions: analyze value first. A proc/extern
			 * call is allowed only when it is the whole RHS (`x := p(...)`), not nested in it. */
			ctx->stmt_call_ok = (stmt->data.bind_stmt.value && stmt->data.bind_stmt.value->type == EXPR_CALL);
			analyze_expression(ctx, stmt->data.bind_stmt.value);
			ctx->stmt_call_ok = 0;

			/* Multi-value let (non-call): add all variables from names array */
			if (stmt->data.bind_stmt.name_count > 0 && stmt->data.bind_stmt.names) {
				for (int i = 0; i < stmt->data.bind_stmt.name_count; i++) {
					const char *var_name = stmt->data.bind_stmt.names[i];
					if (var_name && strcmp(var_name, "_") != 0) {
						/* Add variable (no type annotation for multi-value let) */
						add_variable(ctx, var_name, NULL);

						/* Try to infer type from expression if it's callable with multiple returns */
						if (stmt->data.bind_stmt.value && i < 10) { /* arbitrary limit */
							/* For now, just skip type inference for multi-value let */
							/* This would require analyzing the function's return signature */
						}
					}
				}
			} else if (stmt->data.bind_stmt.name) {
				/* Single-value let */
				/* Check if value is an alloc expression */
				const char *archetype_name = NULL;
				if (stmt->data.bind_stmt.value && stmt->data.bind_stmt.value->type == EXPR_ALLOC) {
					archetype_name = stmt->data.bind_stmt.value->data.alloc.archetype_name;
					if (!find_archetype(ctx, archetype_name)) {
						char msg[256];
						snprintf(msg, sizeof(msg), "Archetype '%s' not defined", archetype_name);
						error(ctx, msg);
						archetype_name = NULL;
					}
				}

				/* create local variable */
				VariableInfo *var = NULL;
				if (archetype_name) {
					add_variable_with_archetype(ctx, stmt->data.bind_stmt.name, stmt->data.bind_stmt.type,
					                            archetype_name);
				} else {
					add_variable(ctx, stmt->data.bind_stmt.name, stmt->data.bind_stmt.type);
				}

				/* Handle type annotations and type inference */
				if (ctx->scope_count > 0) {
					Scope *scope = &ctx->scopes[ctx->scope_count - 1];
					if (scope->var_count > 0) {
						var = scope->vars[scope->var_count - 1];
						if (stmt->data.bind_stmt.type) {
							/* Type annotation: convert TypeRef to string type name */
							TypeRef *t = stmt->data.bind_stmt.type;
							if (t->kind == TYPE_HANDLE)
								var->inferred_type = t->data.handle.archetype_name;
							else if (t->kind == TYPE_NAME) {
								var->inferred_type = resolve_type_alias(ctx, t->data.name);
								if (is_type_alias(ctx, t->data.name))
									var->nominal_type = t->data.name;
							} else
								var->inferred_type = t->data.name;
						} else if (stmt->data.bind_stmt.value) {
							/* No annotation: infer from value expression */
							const char *inferred = resolve_expression_type(ctx, stmt->data.bind_stmt.value);
							if (inferred) {
								var->inferred_type = inferred;
							}
							/* keep the nominal alias name (if any) for distinctness checks */
							var->nominal_type = nominal_type_of_expr(ctx, stmt->data.bind_stmt.value);
						}
					}
				}
			}
		}
		/* No implicit numeric conversion (arche is strict — proven by `y: float = 3` failing): a
		 * binding's explicit type must agree with its value for the int/float pair. Catches a float
		 * const used as an int (`x: int = PI`) and vice versa (`y: float = N`), with a clean error
		 * instead of a downstream LLVM crash. Conservative: only the int↔float mismatch. */
		if (stmt->data.bind_stmt.type && stmt->data.bind_stmt.type->kind == TYPE_NAME && stmt->data.bind_stmt.value) {
			const char *want = resolve_type_alias(ctx, normalize_type_name(stmt->data.bind_stmt.type->data.name));
			const char *got = resolve_expression_type(ctx, stmt->data.bind_stmt.value);
			if (want && got &&
			    ((strcmp(want, "int") == 0 && strcmp(got, "float") == 0) ||
			     (strcmp(want, "float") == 0 && strcmp(got, "int") == 0))) {
				char msg[256];
				snprintf(msg, sizeof(msg),
				         "cannot bind a %s value to '%s' declared `%s` — arche has no implicit numeric conversion", got,
				         stmt->data.bind_stmt.name, want);
				error(ctx, msg);
			}
		}
		if (stmt->data.bind_stmt.is_const)
			mark_last_const(ctx); /* immutable: reject later assignment */
		break;
	}

	case STMT_MULTI_BIND: {
		/* Multi-bind: `x, y, n := expr`. Analyze the RHS FIRST so a `move x` inside it
		 * refers to the existing binding (e.g. a buffer being passed by reference and
		 * returned), not a target we are about to introduce. The RHS is the whole (multi-return)
		 * call, so a proc/extern there is allowed — an action whose results are bound. */
		ctx->stmt_call_ok = (stmt->data.multi_bind.value && stmt->data.multi_bind.value->type == EXPR_CALL);
		analyze_expression(ctx, stmt->data.multi_bind.value);
		ctx->stmt_call_ok = 0;

		/* Then bind the targets. A new target (`x` / `x:`) introduces a FRESH binding that
		 * shadows any same-named one — so `buf := f(move buf)` rebinds the moved buffer with
		 * no special-casing. An existing target (assignment-style) must be declared and live;
		 * assigning to a moved (dead) binding is an error — use `:=`. */
		for (int i = 0; i < stmt->data.multi_bind.target_count; i++) {
			BindingTarget *target = &stmt->data.multi_bind.targets[i];
			if (target->is_new) {
				add_variable(ctx, target->name, target->type);
			} else {
				VariableInfo *existing = find_variable(ctx, target->name);
				if (!existing) {
					char msg[256];
					snprintf(msg, sizeof(msg), "Variable '%s' not declared", target->name);
					error(ctx, msg);
				} else if (existing->is_consumed) {
					char msg[256];
					snprintf(msg, sizeof(msg), "cannot assign to '%s' after it was moved — rebind with `:=`",
					         target->name);
					error(ctx, msg);
				}
			}
		}
		break;
	}

	case STMT_ASSIGN:
		analyze_expression(ctx, stmt->data.assign_stmt.target);
		/* A proc/extern call is allowed only as the whole RHS of the assign (`x = p(...)`). */
		ctx->stmt_call_ok = (stmt->data.assign_stmt.value && stmt->data.assign_stmt.value->type == EXPR_CALL);
		analyze_expression(ctx, stmt->data.assign_stmt.value);
		ctx->stmt_call_ok = 0;
		/* You cannot assign to a binding that was moved (it's dead): `buf = foo(move buf)` must
		 * be written `buf := foo(move buf)` (a fresh binding). The move in the RHS consumes the
		 * target above, so check it here. */
		if (stmt->data.assign_stmt.target->type == EXPR_NAME) {
			VariableInfo *t = find_variable(ctx, stmt->data.assign_stmt.target->data.name.name);
			if (t && t->is_const) {
				char msg[256];
				snprintf(msg, sizeof(msg), "cannot assign to constant '%s' (declared with `::`)",
				         stmt->data.assign_stmt.target->data.name.name);
				error(ctx, msg);
			} else if (t && t->is_consumed) {
				char msg[256];
				snprintf(msg, sizeof(msg), "cannot assign to '%s' after it was moved — rebind with `:=`",
				         stmt->data.assign_stmt.target->data.name.name);
				error(ctx, msg);
			}
		}
		/* Purity: a borrowed (non-`move`) array parameter is read-only — `p = …`, `p[i] = …`,
		 * `p.f = …` are all rejected. Functions are pure by use; to mutate, take it `move` and
		 * return it (same-name in/out), or copy it into a local. Uses the leftmost name so index
		 * and field writes are caught, not just bare `p`. */
		{
			const char *ln = lvalue_leftmost_name(stmt->data.assign_stmt.target);
			VariableInfo *pv = ln ? find_variable(ctx, ln) : NULL;
			if (pv && pv->is_param && !pv->is_own && type_is_byref_aggregate(pv->type)) {
				char msg[320];
				snprintf(msg, sizeof(msg),
				         "cannot mutate read-only parameter '%s' — array parameters are borrowed "
				         "(read-only) by default; to mutate, take it `move` and return it (same-name "
				         "in/out), or copy it into a local",
				         ln);
				error(ctx, msg);
			}
		}
		break;

	case STMT_FOR: {
		/* Check for parenthesized or range-based for loop */
		if (stmt->data.for_stmt.init || stmt->data.for_stmt.increment) {
			/* Parenthesized for loop: for (init; cond; incr) */
			push_scope(ctx);

			if (stmt->data.for_stmt.init) {
				analyze_statement(ctx, stmt->data.for_stmt.init);
			}

			if (stmt->data.for_stmt.condition) {
				analyze_expression(ctx, stmt->data.for_stmt.condition);
			}

			for (int i = 0; i < stmt->data.for_stmt.body_count; i++) {
				analyze_statement(ctx, stmt->data.for_stmt.body[i]);
			}

			if (stmt->data.for_stmt.increment) {
				analyze_statement(ctx, stmt->data.for_stmt.increment);
			}

			pop_scope(ctx);
			break;
		}

		/* Check for infinite or condition-based for loop (no init/incr, no var_name) */
		if (!stmt->data.for_stmt.var_name) {
			/* Infinite or condition-based for loop */
			if (stmt->data.for_stmt.condition) {
				/* Condition-based: analyze condition */
				analyze_expression(ctx, stmt->data.for_stmt.condition);
			}
			/* Both infinite and condition-based loops: analyze body in new scope */
			push_scope(ctx);
			for (int i = 0; i < stmt->data.for_stmt.body_count; i++) {
				analyze_statement(ctx, stmt->data.for_stmt.body[i]);
			}
			pop_scope(ctx);
			break;
		}

		/* check iterable exists (should be archetype) */
		analyze_expression(ctx, stmt->data.for_stmt.iterable);

		/* push new scope for loop body */
		push_scope(ctx);

		/* determine what archetype the loop iterates over */
		const char *archetype_name = NULL;
		if (stmt->data.for_stmt.iterable->type == EXPR_NAME) {
			const char *iterable_name = stmt->data.for_stmt.iterable->data.name.name;

			/* check if this is a direct archetype reference */
			if (find_archetype(ctx, iterable_name)) {
				archetype_name = iterable_name;
			}
			/* check if this is a variable that holds an archetype instance */
			else if (find_variable(ctx, iterable_name)) {
				VariableInfo *var = find_variable(ctx, iterable_name);
				if (var && var->archetype_name) {
					archetype_name = var->archetype_name;
				} else {
					char msg[256];
					snprintf(msg, sizeof(msg), "Variable '%s' does not refer to an archetype instance", iterable_name);
					error(ctx, msg);
					archetype_name = NULL;
				}
			}
			/* check if we're in a sys and this is a parameter name (matches current sys archetype) */
			else if (ctx->current_sys_archetype) {
				archetype_name = ctx->current_sys_archetype;
			} else {
				char msg[256];
				snprintf(msg, sizeof(msg), "For loop iterates over undefined archetype '%s'", iterable_name);
				error(ctx, msg);
				archetype_name = NULL;
			}
		}

		/* loop variable is in scope and refers to the archetype */
		add_variable_with_archetype(ctx, stmt->data.for_stmt.var_name, NULL, archetype_name);

		for (int i = 0; i < stmt->data.for_stmt.body_count; i++) {
			analyze_statement(ctx, stmt->data.for_stmt.body[i]);
		}

		pop_scope(ctx);
		break;
	}

	case STMT_IF: {
		/* analyze condition */
		analyze_expression(ctx, stmt->data.if_stmt.cond);

		/* push new scope for if body */
		push_scope(ctx);

		for (int i = 0; i < stmt->data.if_stmt.then_count; i++) {
			analyze_statement(ctx, stmt->data.if_stmt.then_body[i]);
		}

		pop_scope(ctx);

		/* analyze else body in its own scope */
		push_scope(ctx);

		for (int i = 0; i < stmt->data.if_stmt.else_count; i++) {
			analyze_statement(ctx, stmt->data.if_stmt.else_body[i]);
		}

		pop_scope(ctx);
		break;
	}

	case STMT_RUN:
		/* no world validation needed - worlds are planned but not yet implemented */
		break;

	case STMT_EXPR:
		/* A standalone expression statement: a proc/extern call here is allowed — it's an action
		 * performed for its effect (its return value, if any, is discarded). */
		ctx->stmt_call_ok = 1;
		analyze_expression(ctx, stmt->data.expr_stmt.expr);
		ctx->stmt_call_ok = 0;
		break;

	case STMT_RETURN:
		/* Returning an opaque binding moves it out — counts as consumption. (Data and
		 * handles copy, so returning them must NOT kill the binding.) */
		for (int i = 0; i < stmt->data.return_stmt.count; i++) {
			Expression *rval = stmt->data.return_stmt.values[i];
			analyze_expression(ctx, rval);
			if (rval && rval->type == EXPR_NAME) {
				VariableInfo *rv = find_variable(ctx, rval->data.name.name);
				if (rv && var_is_opaque(ctx, rv))
					rv->is_consumed = 1;
			}
		}
		break;

	case STMT_FREE:
		analyze_expression(ctx, stmt->data.free_stmt.value);
		break;

	case STMT_BREAK:
		break;

	case STMT_EACH_FIELD: {
		EachFieldStmt *ef = &stmt->data.each_field;

		/* Filter type, if present, must be a primitive (int/float/char). */
		if (ef->filter_type) {
			if (ef->filter_type->kind != TYPE_NAME) {
				error(ctx, "each_field filter type must be a primitive type");
			} else {
				const char *fn = normalize_type_name(ef->filter_type->data.name);
				if (!fn || (strcmp(fn, "int") != 0 && strcmp(fn, "float") != 0 && strcmp(fn, "char") != 0)) {
					error(ctx, "each_field filter type must be a primitive type (int, float, or char)");
				}
			}
		}

		/* RHS must name an `archetype` parameter of the current proc. */
		int arch_param_ok = 0;
		if (ctx->current_proc) {
			for (int i = 0; i < ctx->current_proc->param_count; i++) {
				Parameter *p = ctx->current_proc->params[i];
				if (p && p->name && strcmp(p->name, ef->arch_param_name) == 0 && p->type &&
				    p->type->kind == TYPE_ARCHETYPE) {
					arch_param_ok = 1;
					break;
				}
			}
		}
		if (!arch_param_ok) {
			char msg[256];
			snprintf(msg, sizeof(msg),
			         "each_field RHS '%s' must be an `archetype`-typed parameter of the enclosing proc",
			         ef->arch_param_name);
			error(ctx, msg);
		}

		/* Analyze body in a pushed scope where the binding is declared opaquely.
		 * `f`'s real type varies per expansion; codegen substitutes the concrete
		 * column reference per emitted copy. */
		push_scope(ctx);
		add_variable(ctx, ef->binding_name, NULL);
		for (int i = 0; i < ef->body_count; i++) {
			analyze_statement(ctx, ef->body[i]);
		}
		pop_scope(ctx);
		break;
	}
	}

	ctx->current_loc = saved_loc;
}

/* ========== DECLARATION ANALYSIS ========== */

static void analyze_archetype_decl(SemanticContext *ctx, ArchetypeDecl *arch) {
	if (!arch)
		return;

	/* A `type`-typed component (`arche A { f :: type }`) is a generic component — not supported. */
	for (int i = 0; i < arch->field_count; i++)
		reject_meta_type(ctx, arch->fields[i]->type, "archetype component type");

	/* Set semantics: a component type may appear at most once in an archetype. The
	 * component's type name IS its access path, so a repeat would be unreachable. */
	for (int i = 0; i < arch->field_count; i++) {
		for (int j = i + 1; j < arch->field_count; j++) {
			if (arch->fields[i]->name && arch->fields[j]->name &&
			    strcmp(arch->fields[i]->name, arch->fields[j]->name) == 0) {
				char msg[256];
				snprintf(msg, sizeof(msg),
				         "duplicate component '%s' in archetype (a component type may appear only once)",
				         arch->fields[i]->name);
				error(ctx, msg);
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

		/* Count total fields after expanding tuples */
		int expanded_field_count = 0;
		for (int i = 0; i < arch->field_count; i++) {
			if (arch->fields[i]->type->kind == TYPE_TUPLE) {
				expanded_field_count += arch->fields[i]->type->data.tuple.field_count;
			} else {
				expanded_field_count++;
			}
		}

		shape->fields = malloc(expanded_field_count * sizeof(FieldInfo *));
		shape->field_count = expanded_field_count;

		/* Populate fields, expanding tuples into virtual fields */
		int field_idx = 0;
		for (int i = 0; i < arch->field_count; i++) {
			FieldDecl *field = arch->fields[i];
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

	/* Tuple fields are flattened to scalar columns in lowering (CST->AST), not
	   here: the AST is tuple-free, the CST keeps tuples. The flat `shape` above
	   is still built from the tuple fields for type checking. */

	/* Validate handle types: must reference a known archetype. */
	for (int i = 0; i < arch->field_count; i++) {
		TypeRef *ft = arch->fields[i]->type;
		if (ft->kind != TYPE_HANDLE)
			continue;
		const char *target = ft->data.handle.archetype_name;
		if (!find_archetype(ctx, target)) {
			fprintf(stderr, "Error: unknown archetype '%s' in handle type for field '%s'\n", target,
			        arch->fields[i]->name);
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

static void analyze_static_array_decl(SemanticContext *ctx, StaticDecl *s) {
	if (!s)
		return;

	/* Validate element type is a scalar */
	if (!s->array.element_type) {
		fprintf(stderr, "Error: static array '%s' missing element type\n", s->array.name);
		ctx->error_count++;
		return;
	}

	if (s->array.element_type->kind != TYPE_NAME) {
		fprintf(stderr, "Error: static array '%s' element type must be scalar (int, float, char, etc.)\n",
		        s->array.name);
		ctx->error_count++;
		return;
	}

	const char *type_name = s->array.element_type->data.name;
	if (strcmp(type_name, "int") != 0 && strcmp(type_name, "float") != 0 && strcmp(type_name, "char") != 0 &&
	    strcmp(type_name, "double") != 0) {
		fprintf(stderr, "Error: static array '%s' has unsupported element type '%s'\n", s->array.name, type_name);
		ctx->error_count++;
		return;
	}

	/* Validate size is positive */
	if (s->array.size <= 0) {
		fprintf(stderr, "Error: static array '%s' has invalid size %d\n", s->array.name, s->array.size);
		ctx->error_count++;
		return;
	}

	/* Register as a variable in the global scope so find_variable works everywhere */
	add_variable(ctx, s->array.name, s->array.element_type);
}

static void analyze_static_decl(SemanticContext *ctx, StaticDecl *alloc) {
	if (!alloc)
		return;

	/* Validate archetype exists */
	ArchetypeInfo *arch = find_archetype(ctx, alloc->archetype.archetype_name);
	if (!arch) {
		fprintf(stderr, "Error: unknown archetype '%s' in alloc\n", alloc->archetype.archetype_name);
		ctx->error_count++;
		return;
	}

	/* Check if this shape has already been allocated. Each shape (field structure)
	   can have multiple archetype handles/names pointing to it, but only one can
	   allocate/initialize it. Once allocated, the shape is live in the world. */
	if (arch->is_allocated) {
		fprintf(stderr, "Error: Shape already allocated (archetype '%s' shares shape with an earlier allocation)\n",
		        alloc->archetype.archetype_name);
		ctx->error_count++;
		return;
	}
	arch->is_allocated = 1;

	/* Validate count is provided and is a literal */
	if (alloc->archetype.field_count == 0 || !alloc->archetype.field_values[0]) {
		fprintf(stderr, "Error: alloc missing count expression\n");
		ctx->error_count++;
		return;
	}

	Expression *count_expr = alloc->archetype.field_values[0];
	if (count_expr->type != EXPR_LITERAL) {
		fprintf(stderr, "Error: alloc count must be a literal; dynamic counts not yet supported\n");
		ctx->error_count++;
		return;
	}

	/* Validate: init block requires explicit init_size parameter */
	if (alloc->archetype.field_count > 1 && !alloc->archetype.init_length) {
		fprintf(stderr,
		        "Error: init block requires explicit init_size parameter: static %s(capacity, init_size) { ... }\n",
		        alloc->archetype.archetype_name);
		ctx->error_count++;
		return;
	}

	/* Analyze field initialization expressions */
	for (int i = 1; i < alloc->archetype.field_count; i++) {
		analyze_expression(ctx, alloc->archetype.field_values[i]);
	}
}

/* ========== PROC LINT HELPERS ========== */

/* Builtins that mutate archetype state (registered in semantic_analyze). */
static int name_is_archetype_mutating_builtin(const char *name) {
	if (!name)
		return 0;
	return strcmp(name, "insert") == 0 || strcmp(name, "delete") == 0 || strcmp(name, "dealloc") == 0;
}

/* Returns 1 if a call to `name` is a potential side effect. True when the
 * callee is (a) a proc / extern proc, (b) an extern func (anything could
 * happen in C), (c) a regular func with at least one `out` parameter, or
 * (d) an archetype-mutating builtin (insert / delete / dealloc). */
static int name_is_effectful_callee(SemanticContext *ctx, const char *name) {
	if (!ctx || !ctx->prog || !name)
		return 0;
	if (name_is_archetype_mutating_builtin(name))
		return 1;
	for (int i = 0; i < ctx->prog->decl_count; i++) {
		Decl *d = ctx->prog->decls[i];
		if (!d)
			continue;
		if (d->kind == DECL_PROC && d->data.proc && d->data.proc->name && strcmp(d->data.proc->name, name) == 0) {
			return 1; /* any proc is effectful */
		}
		if (d->kind == DECL_FUNC && d->data.func && d->data.func->name && strcmp(d->data.func->name, name) == 0) {
			if (d->data.func->is_extern)
				return 1; /* extern func: opaque C side effects */
			if (d->data.func->return_type_count > 1)
				return 1; /* multi-return func fills caller-passed buffers in place */
			return 0;     /* pure-ish func */
		}
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

/* Forward declarations for mutual recursion */
static int expr_has_side_effects(SemanticContext *ctx, Expression *expr, ProcDecl *proc);
static int body_has_side_effects(SemanticContext *ctx, Statement **stmts, int count, ProcDecl *proc);

static int expr_has_side_effects(SemanticContext *ctx, Expression *expr, ProcDecl *proc) {
	if (!expr)
		return 0;
	switch (expr->type) {
	case EXPR_CALL:
		if (expr->data.call.callee && expr->data.call.callee->type == EXPR_NAME &&
		    name_is_effectful_callee(ctx, expr->data.call.callee->data.name.name)) {
			return 1;
		}
		for (int i = 0; i < expr->data.call.arg_count; i++) {
			if (expr_has_side_effects(ctx, expr->data.call.args[i], proc))
				return 1;
		}
		return 0;
	case EXPR_BINARY:
		return expr_has_side_effects(ctx, expr->data.binary.left, proc) ||
		       expr_has_side_effects(ctx, expr->data.binary.right, proc);
	case EXPR_UNARY:
		return expr_has_side_effects(ctx, expr->data.unary.operand, proc);
	case EXPR_FIELD:
		return expr_has_side_effects(ctx, expr->data.field.base, proc);
	case EXPR_INDEX:
		if (expr_has_side_effects(ctx, expr->data.index.base, proc))
			return 1;
		for (int i = 0; i < expr->data.index.index_count; i++) {
			if (expr_has_side_effects(ctx, expr->data.index.indices[i], proc))
				return 1;
		}
		return 0;
	case EXPR_ALLOC:
		return 1; /* allocation = side effect */
	default:
		return 0;
	}
}

static int stmt_has_side_effects(SemanticContext *ctx, Statement *stmt, ProcDecl *proc) {
	if (!stmt)
		return 0;
	switch (stmt->type) {
	case STMT_BIND:
		return stmt->data.bind_stmt.value ? expr_has_side_effects(ctx, stmt->data.bind_stmt.value, proc) : 0;
	case STMT_ASSIGN: {
		const char *target_name = lvalue_leftmost_name(stmt->data.assign_stmt.target);
		if (target_name) {
			if (find_archetype(ctx, target_name))
				return 1; /* writing through an archetype = column write */
		}
		return expr_has_side_effects(ctx, stmt->data.assign_stmt.value, proc) ||
		       expr_has_side_effects(ctx, stmt->data.assign_stmt.target, proc);
	}
	case STMT_FOR:
		if (stmt->data.for_stmt.init && stmt_has_side_effects(ctx, stmt->data.for_stmt.init, proc))
			return 1;
		if (stmt->data.for_stmt.condition && expr_has_side_effects(ctx, stmt->data.for_stmt.condition, proc))
			return 1;
		if (stmt->data.for_stmt.increment && stmt_has_side_effects(ctx, stmt->data.for_stmt.increment, proc))
			return 1;
		return body_has_side_effects(ctx, stmt->data.for_stmt.body, stmt->data.for_stmt.body_count, proc);
	case STMT_IF:
		if (stmt->data.if_stmt.cond && expr_has_side_effects(ctx, stmt->data.if_stmt.cond, proc))
			return 1;
		if (body_has_side_effects(ctx, stmt->data.if_stmt.then_body, stmt->data.if_stmt.then_count, proc))
			return 1;
		return body_has_side_effects(ctx, stmt->data.if_stmt.else_body, stmt->data.if_stmt.else_count, proc);
	case STMT_BREAK:
	case STMT_RETURN:
		return 0;
	case STMT_RUN:
		return 1; /* running a system mutates archetype state */
	case STMT_EXPR:
		return expr_has_side_effects(ctx, stmt->data.expr_stmt.expr, proc);
	case STMT_FREE:
		return 1; /* deallocation */
	case STMT_MULTI_BIND:
		return stmt->data.multi_bind.value ? expr_has_side_effects(ctx, stmt->data.multi_bind.value, proc) : 0;
	case STMT_EACH_FIELD: {
		for (int i = 0; i < stmt->data.each_field.body_count; i++) {
			if (stmt_has_side_effects(ctx, stmt->data.each_field.body[i], proc))
				return 1;
		}
		return 0;
	}
	}
	return 0;
}

static int body_has_side_effects(SemanticContext *ctx, Statement **stmts, int count, ProcDecl *proc) {
	if (!stmts)
		return 0;
	for (int i = 0; i < count; i++) {
		if (stmt_has_side_effects(ctx, stmts[i], proc))
			return 1;
	}
	return 0;
}

/* Run the proc-could-be-func and proc-no-effect lints on a non-extern proc. */
/* A proc "could be a func" iff its body would pass func-purity — the SAME predicate
 * `enforce_func_purity` uses — so the lint and the hard error agree on what "pure" means. */
static const char *func_purity_body(SemanticContext *ctx, Statement **stmts, int count);

static void lint_proc_decl(SemanticContext *ctx, ProcDecl *proc) {
	if (!proc || proc->is_extern || proc->allow_pure_proc)
		return;

	/* A proc whose body has effects is legitimately a proc — nothing to lint. */
	if (func_purity_body(ctx, proc->statements, proc->statement_count) != NULL)
		return;

	/* Pure body. If it returns a value it computes something — it should be a `func` (procs are
	 * for effects). If it returns nothing, it's a no-op proc (does nothing observable) — flag for
	 * removal. The purity test is the SAME predicate `enforce_func_purity` uses, so "could be a
	 * func" means exactly "would compile as a func". */
	if (proc->return_type_count > 0) {
		if (g_lint_config.proc_could_be_func_enabled) {
			char msg[320];
			snprintf(msg, sizeof(msg),
			         "proc '%s' has a pure body and returns a value — it should be a 'func' "
			         "(suppress with @allow_pure_proc)",
			         proc->name ? proc->name : "<unknown>");
			lint_emit(ctx, g_lint_config.proc_could_be_func_werror, proc->loc, "proc-could-be-func", msg);
		}
	} else if (g_lint_config.proc_no_effect_enabled) {
		char msg[256];
		snprintf(msg, sizeof(msg), "proc '%s' has an empty or effect-free body; remove it or add the intended logic",
		         proc->name ? proc->name : "<unknown>");
		lint_emit(ctx, g_lint_config.proc_no_effect_werror, proc->loc, "proc-no-effect", msg);
	}
}

/* ===== func-purity (`func-impure` lint) =====
 * A `func` must be functional. Unlike `name_is_effectful_callee` (the proc-could-be-func lint),
 * we do NOT treat a multi-return func as effectful — calling another (enforced-pure) func is fine.
 * A call is an effect iff the callee is a proc, an extern, or an archetype-mutating builtin. */
static const char *func_call_effect_reason(SemanticContext *ctx, const char *name) {
	static char buf[160];
	if (!ctx || !ctx->prog || !name)
		return NULL;
	if (name_is_archetype_mutating_builtin(name)) {
		snprintf(buf, sizeof(buf), "calls archetype-mutating builtin '%s'", name);
		return buf;
	}
	for (int i = 0; i < ctx->prog->decl_count; i++) {
		Decl *d = ctx->prog->decls[i];
		if (!d)
			continue;
		if (d->kind == DECL_PROC && d->data.proc && d->data.proc->name && strcmp(d->data.proc->name, name) == 0) {
			snprintf(buf, sizeof(buf), d->data.proc->is_extern ? "calls extern '%s'" : "calls proc '%s'", name);
			return buf;
		}
		if (d->kind == DECL_FUNC && d->data.func && d->data.func->name && strcmp(d->data.func->name, name) == 0) {
			if (d->data.func->is_extern) {
				snprintf(buf, sizeof(buf), "calls extern '%s'", name);
				return buf;
			}
			return NULL; /* a non-extern func is (being) enforced pure — fine to call */
		}
	}
	return NULL;
}

static const char *func_purity_body(SemanticContext *ctx, Statement **stmts, int count);

static const char *func_purity_expr(SemanticContext *ctx, Expression *e) {
	const char *r;
	if (!e)
		return NULL;
	switch (e->type) {
	case EXPR_CALL:
		if (e->data.call.callee && e->data.call.callee->type == EXPR_NAME &&
		    (r = func_call_effect_reason(ctx, e->data.call.callee->data.name.name)))
			return r;
		for (int i = 0; i < e->data.call.arg_count; i++)
			if ((r = func_purity_expr(ctx, e->data.call.args[i])))
				return r;
		return NULL;
	case EXPR_BINARY:
		if ((r = func_purity_expr(ctx, e->data.binary.left)))
			return r;
		return func_purity_expr(ctx, e->data.binary.right);
	case EXPR_UNARY:
		return func_purity_expr(ctx, e->data.unary.operand);
	case EXPR_FIELD:
		return func_purity_expr(ctx, e->data.field.base);
	case EXPR_INDEX:
		if ((r = func_purity_expr(ctx, e->data.index.base)))
			return r;
		for (int i = 0; i < e->data.index.index_count; i++)
			if ((r = func_purity_expr(ctx, e->data.index.indices[i])))
				return r;
		return NULL;
	case EXPR_ALLOC:
		return "allocates (`alloc`)";
	default:
		return NULL;
	}
}

static const char *func_purity_stmt(SemanticContext *ctx, Statement *s) {
	const char *r;
	if (!s)
		return NULL;
	switch (s->type) {
	case STMT_BIND:
		return s->data.bind_stmt.value ? func_purity_expr(ctx, s->data.bind_stmt.value) : NULL;
	case STMT_MULTI_BIND:
		return s->data.multi_bind.value ? func_purity_expr(ctx, s->data.multi_bind.value) : NULL;
	case STMT_ASSIGN: {
		const char *tn = lvalue_leftmost_name(s->data.assign_stmt.target);
		if (tn && find_archetype(ctx, tn))
			return "writes static memory (an archetype column)";
		if ((r = func_purity_expr(ctx, s->data.assign_stmt.value)))
			return r;
		return func_purity_expr(ctx, s->data.assign_stmt.target);
	}
	case STMT_FOR:
		if (s->data.for_stmt.init && (r = func_purity_stmt(ctx, s->data.for_stmt.init)))
			return r;
		if (s->data.for_stmt.condition && (r = func_purity_expr(ctx, s->data.for_stmt.condition)))
			return r;
		if (s->data.for_stmt.increment && (r = func_purity_stmt(ctx, s->data.for_stmt.increment)))
			return r;
		return func_purity_body(ctx, s->data.for_stmt.body, s->data.for_stmt.body_count);
	case STMT_IF:
		if (s->data.if_stmt.cond && (r = func_purity_expr(ctx, s->data.if_stmt.cond)))
			return r;
		if ((r = func_purity_body(ctx, s->data.if_stmt.then_body, s->data.if_stmt.then_count)))
			return r;
		return func_purity_body(ctx, s->data.if_stmt.else_body, s->data.if_stmt.else_count);
	case STMT_EXPR:
		return func_purity_expr(ctx, s->data.expr_stmt.expr);
	case STMT_RETURN:
		for (int i = 0; i < s->data.return_stmt.count; i++)
			if ((r = func_purity_expr(ctx, s->data.return_stmt.values[i])))
				return r;
		return NULL;
	case STMT_RUN:
		return "runs a system (`run`)";
	case STMT_FREE:
		return "frees memory (`free`)";
	case STMT_EACH_FIELD:
		return func_purity_body(ctx, s->data.each_field.body, s->data.each_field.body_count);
	default:
		return NULL;
	}
}

static const char *func_purity_body(SemanticContext *ctx, Statement **stmts, int count) {
	const char *r;
	for (int i = 0; i < count; i++)
		if ((r = func_purity_stmt(ctx, stmts[i])))
			return r;
	return NULL;
}

/* Enforce func purity: a non-extern `func` body must be pure (no effects). This is a RULE, not a
 * lint — a violation is a hard compile error. Effects belong in a proc. */
static void enforce_func_purity(SemanticContext *ctx, FuncDecl *func) {
	if (!func || func->is_extern)
		return;
	const char *reason = func_purity_body(ctx, func->statements, func->statement_count);
	if (reason) {
		char msg[400];
		snprintf(msg, sizeof(msg), "func '%s' is not pure — %s (effects belong in a proc)",
		         func->name ? func->name : "<unknown>", reason);
		error(ctx, msg);
	}
}

static void analyze_proc_decl(SemanticContext *ctx, ProcDecl *proc) {
	if (!proc)
		return;

	/* Register proc name as a known function */
	register_func(ctx, proc->name);

	/* Validate parameters for extern vs non-extern rules. */
	for (int i = 0; i < proc->param_count; i++) {
		Parameter *p = proc->params[i];
		if (!p)
			continue;
		TypeRef *pt = p->type;
		reject_meta_type(ctx, pt, "parameter type");
		if (proc->is_extern) {
			if (pt && pt->kind == TYPE_NAME) {
				const char *tname = pt->data.name;
				if (!is_primitive_type_name(tname) && !find_archetype(ctx, tname) && !is_type_alias(ctx, tname)) {
					char msg[256];
					snprintf(msg, sizeof(msg), "unknown type '%s' in extern proc '%s' signature", tname, proc->name);
					error(ctx, msg);
				}
			}
			/* `consume` is valid on any param type (consume consumes — not opaque-special). */
		}
	}

	/* For extern procs, no body to analyze */
	if (proc->is_extern) {
		return;
	}

	/* Validate `archetype` parameter constraints: at most one per proc. */
	int archetype_param_count = 0;
	for (int i = 0; i < proc->param_count; i++) {
		if (proc->params[i]->type && proc->params[i]->type->kind == TYPE_ARCHETYPE) {
			archetype_param_count++;
		}
	}
	if (archetype_param_count > 1) {
		char msg[256];
		snprintf(msg, sizeof(msg), "proc '%s': only one `archetype` parameter is allowed per proc", proc->name);
		error(ctx, msg);
	}

	push_scope(ctx);

	/* Add parameters as variables in proc scope */
	for (int i = 0; i < proc->param_count; i++) {
		const char *param_name = proc->params[i]->name;
		TypeRef *param_type = proc->params[i]->type;

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
		mark_last_param(ctx, proc->params[i]->is_own);
	}

	ProcDecl *prev_proc = ctx->current_proc;
	ctx->current_proc = proc;
	ctx->in_body = 1;
	ctx->in_unsafe = proc->is_unsafe;
	for (int i = 0; i < proc->statement_count; i++) {
		analyze_statement(ctx, proc->statements[i]);
	}
	ctx->in_unsafe = 0;
	ctx->in_body = 0;
	ctx->current_proc = prev_proc;

	pop_scope(ctx);

	/* Run the proc-vs-func lints after typechecking the body. */
	lint_proc_decl(ctx, proc);
}

static void analyze_sys_decl(SemanticContext *ctx, SysDecl *sys) {
	if (!sys)
		return;

	push_scope(ctx);

	/* infer which archetype this sys operates on by matching parameter names to fields */
	const char *sys_archetype = NULL;
	ArchetypeInfo *arch_info = NULL;
	for (int a = 0; a < ctx->archetype_count; a++) {
		int matches = 0;
		for (int p = 0; p < sys->param_count; p++) {
			if (find_field(ctx->archetypes[a], sys->params[p]->name)) {
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
			FieldInfo *field = find_field(arch_info, sys->params[p]->name);
			if (field && field->type && field->type->kind == TYPE_HANDLE) {
				char msg[256];
				snprintf(msg, sizeof(msg), "handle column '%s' cannot be sys parameter", sys->params[p]->name);
				error(ctx, msg);
			}
		}
	}

	/* add parameters as variables, using field types from archetype if available */
	for (int i = 0; i < sys->param_count; i++) {
		TypeRef *param_type = sys->params[i]->type;
		reject_meta_type(ctx, param_type, "sys parameter type");
		/* If no explicit type and we found the archetype, use the field's type */
		if (!param_type && arch_info) {
			FieldInfo *field = find_field(arch_info, sys->params[i]->name);
			if (field) {
				param_type = field->type;
			}
		}
		add_variable(ctx, sys->params[i]->name, param_type);
		mark_last_param(ctx, sys->params[i]->is_own);
	}

	const char *old_sys_archetype = ctx->current_sys_archetype;
	ctx->current_sys_archetype = sys_archetype;

	ctx->in_body = 1;
	for (int i = 0; i < sys->statement_count; i++) {
		analyze_statement(ctx, sys->statements[i]);
	}
	ctx->in_body = 0;

	ctx->current_sys_archetype = old_sys_archetype;
	pop_scope(ctx);
}

static void analyze_func_group(SemanticContext *ctx, FuncGroup *group) {
	if (!group)
		return;

	/* Name collision: group name must not match a prior func, proc, extern, or group. */
	if (find_known_func(ctx, group->name) || find_group(ctx, group->name)) {
		char msg[256];
		snprintf(msg, sizeof(msg), "name '%s' is already declared", group->name);
		error(ctx, msg);
		return;
	}

	if (group->member_count == 0) {
		error(ctx, "func group must have at least one member");
		return;
	}

	FuncDecl **resolved = calloc(group->member_count, sizeof(FuncDecl *));
	for (int i = 0; i < group->member_count; i++) {
		FuncDecl *fd = find_func_decl_cst(ctx->prog, group->member_names[i]);
		if (!fd) {
			char msg[256];
			snprintf(msg, sizeof(msg), "unknown member '%s' in func group '%s'", group->member_names[i], group->name);
			error(ctx, msg);
			continue;
		}
		if (fd->is_extern) {
			char msg[256];
			snprintf(msg, sizeof(msg), "member '%s' of func group '%s' is extern; extern funcs cannot be group members",
			         group->member_names[i], group->name);
			error(ctx, msg);
		}
		/* Forward-reference check: members declared earlier in source are already in known_funcs. */
		if (!find_known_func(ctx, group->member_names[i])) {
			char msg[256];
			snprintf(msg, sizeof(msg), "member '%s' of func group '%s' must be declared before the group",
			         group->member_names[i], group->name);
			error(ctx, msg);
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
				if (!type_ref_equal(resolved[i]->params[k]->type, resolved[j]->params[k]->type)) {
					all_eq = 0;
					break;
				}
			}
			if (all_eq) {
				char msg[256];
				snprintf(msg, sizeof(msg), "members '%s' and '%s' of group '%s' share a parameter signature",
				         group->member_names[i], group->member_names[j], group->name);
				error(ctx, msg);
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

static void analyze_func_decl(SemanticContext *ctx, FuncDecl *func) {
	if (!func)
		return;

	/* Register func name as a known function */
	register_func(ctx, func->name);

	/* `archetype` is a proc-only parameter type in v1; funcs cannot have one. */
	for (int i = 0; i < func->param_count; i++) {
		reject_meta_type(ctx, func->params[i]->type, "parameter type");
		if (func->params[i]->type && func->params[i]->type->kind == TYPE_ARCHETYPE) {
			char msg[256];
			snprintf(msg, sizeof(msg), "func '%s': `archetype` parameter type is only allowed on procs, not funcs",
			         func->name);
			error(ctx, msg);
			break;
		}
	}
	for (int i = 0; i < func->return_type_count; i++) {
		reject_meta_type(ctx, func->return_types[i], "return type");
		if (func->return_types[i] && func->return_types[i]->kind == TYPE_ARCHETYPE) {
			char msg[256];
			snprintf(msg, sizeof(msg), "func '%s': `archetype` is only valid as a parameter type, not a return type",
			         func->name);
			error(ctx, msg);
			break;
		}
	}
	/* Validate parameters and return type for extern vs non-extern rules. */
	for (int i = 0; i < func->param_count; i++) {
		Parameter *p = func->params[i];
		if (!p)
			continue;
		TypeRef *pt = p->type;
		if (func->is_extern) {
			if (pt && pt->kind == TYPE_NAME) {
				const char *tname = pt->data.name;
				if (!is_primitive_type_name(tname) && !is_type_alias(ctx, tname)) {
					char msg[256];
					snprintf(msg, sizeof(msg), "unknown type '%s' in extern func '%s' signature", tname, func->name);
					error(ctx, msg);
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
					char msg[256];
					snprintf(msg, sizeof(msg), "unknown return type '%s' in extern func '%s' signature", tname,
					         func->name);
					error(ctx, msg);
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
		add_variable(ctx, func->params[i]->name, func->params[i]->type);
		mark_last_param(ctx, func->params[i]->is_own);
	}

	ctx->in_unsafe = func->is_unsafe;
	for (int i = 0; i < func->statement_count; i++) {
		analyze_statement(ctx, func->statements[i]);
	}
	ctx->in_unsafe = 0;

	enforce_func_purity(ctx, func); /* a `func` must be pure — hard error if not */

	pop_scope(ctx);
}

static void analyze_decl(SemanticContext *ctx, Decl *decl) {
	if (!decl)
		return;

	switch (decl->kind) {
	case DECL_ARCHETYPE:
		analyze_archetype_decl(ctx, decl->data.archetype);
		break;
	case DECL_STATIC: {
		StaticDecl *s = decl->data.static_decl;
		if (s->kind == STATIC_KIND_ARCHETYPE) {
			analyze_static_decl(ctx, s);
		} else {
			analyze_static_array_decl(ctx, s);
		}
		break;
	}
	case DECL_USE:
		/* Module use — resolved before semantic analysis */
		break;
	case DECL_PROC:
		analyze_proc_decl(ctx, decl->data.proc);
		break;
	case DECL_SYS:
		analyze_sys_decl(ctx, decl->data.sys);
		break;
	case DECL_FUNC:
		analyze_func_decl(ctx, decl->data.func);
		break;
	case DECL_FUNC_GROUP:
		analyze_func_group(ctx, decl->data.func_group);
		break;
	case DECL_CONST:
		/* Value consts and nominal type aliases are collected in pass 0 (and aliases erased before
		 * lowering); there is nothing to analyze per-declaration here. */
		break;
	case DECL_WORLD:
		/* Worlds carry no analyzable body in v1. */
		break;
	}
}

/* ========== TYPE-ALIAS ERASURE ========== */
/* After all checks pass, nominal aliases have done their job (distinctness was enforced
 * at substitution boundaries). Rewrite every alias type-name in the CST to its backing
 * so lowering/codegen never see an alias — the alias is zero-cost. */

static void erase_aliases_typeref(SemanticContext *ctx, TypeRef *t) {
	if (!t)
		return;
	switch (t->kind) {
	case TYPE_NAME: {
		const char *b = resolve_type_alias(ctx, t->data.name);
		if (b != t->data.name) {
			/* Became its backing. `opaque` is its own TypeRef kind (not a named type),
			 * so rewrite the kind to match a native `opaque`; other backings are named. */
			if (strcmp(b, "opaque") == 0) {
				t->kind = TYPE_OPAQUE;
			} else {
				char *dup = malloc(strlen(b) + 1);
				strcpy(dup, b);
				t->data.name = dup;
			}
		}
		break;
	}
	case TYPE_ARRAY:
		erase_aliases_typeref(ctx, t->data.array.element_type);
		break;
	case TYPE_SHAPED_ARRAY:
		erase_aliases_typeref(ctx, t->data.shaped_array.element_type);
		break;
	case TYPE_TUPLE:
		for (int i = 0; i < t->data.tuple.field_count; i++)
			erase_aliases_typeref(ctx, t->data.tuple.field_types[i]);
		break;
	default:
		break;
	}
}

static void erase_aliases_stmt(SemanticContext *ctx, Statement *s) {
	if (!s)
		return;
	switch (s->type) {
	case STMT_BIND:
		erase_aliases_typeref(ctx, s->data.bind_stmt.type);
		break;
	case STMT_FOR:
		erase_aliases_stmt(ctx, s->data.for_stmt.init);
		erase_aliases_stmt(ctx, s->data.for_stmt.increment);
		for (int i = 0; i < s->data.for_stmt.body_count; i++)
			erase_aliases_stmt(ctx, s->data.for_stmt.body[i]);
		break;
	case STMT_IF:
		for (int i = 0; i < s->data.if_stmt.then_count; i++)
			erase_aliases_stmt(ctx, s->data.if_stmt.then_body[i]);
		for (int i = 0; i < s->data.if_stmt.else_count; i++)
			erase_aliases_stmt(ctx, s->data.if_stmt.else_body[i]);
		break;
	case STMT_MULTI_BIND:
		for (int i = 0; i < s->data.multi_bind.target_count; i++)
			erase_aliases_typeref(ctx, s->data.multi_bind.targets[i].type);
		break;
	case STMT_EACH_FIELD:
		erase_aliases_typeref(ctx, s->data.each_field.filter_type);
		for (int i = 0; i < s->data.each_field.body_count; i++)
			erase_aliases_stmt(ctx, s->data.each_field.body[i]);
		break;
	default:
		break;
	}
}

static void erase_aliases_decl(SemanticContext *ctx, Decl *d) {
	if (!d)
		return;
	switch (d->kind) {
	case DECL_ARCHETYPE:
		for (int i = 0; i < d->data.archetype->field_count; i++)
			erase_aliases_typeref(ctx, d->data.archetype->fields[i]->type);
		break;
	case DECL_PROC:
		for (int i = 0; i < d->data.proc->param_count; i++)
			erase_aliases_typeref(ctx, d->data.proc->params[i]->type);
		for (int i = 0; i < d->data.proc->statement_count; i++)
			erase_aliases_stmt(ctx, d->data.proc->statements[i]);
		break;
	case DECL_SYS:
		for (int i = 0; i < d->data.sys->param_count; i++)
			erase_aliases_typeref(ctx, d->data.sys->params[i]->type);
		for (int i = 0; i < d->data.sys->statement_count; i++)
			erase_aliases_stmt(ctx, d->data.sys->statements[i]);
		break;
	case DECL_FUNC:
		for (int i = 0; i < d->data.func->return_type_count; i++)
			erase_aliases_typeref(ctx, d->data.func->return_types[i]);
		for (int i = 0; i < d->data.func->param_count; i++)
			erase_aliases_typeref(ctx, d->data.func->params[i]->type);
		for (int i = 0; i < d->data.func->statement_count; i++)
			erase_aliases_stmt(ctx, d->data.func->statements[i]);
		break;
	case DECL_STATIC:
		if (d->data.static_decl->kind == STATIC_KIND_ARRAY)
			erase_aliases_typeref(ctx, d->data.static_decl->array.element_type);
		break;
	default:
		break;
	}
}

static void erase_type_aliases(SemanticContext *ctx, AstProgram *prog) {
	if (ctx->type_alias_count == 0)
		return;
	for (int i = 0; i < prog->decl_count; i++)
		erase_aliases_decl(ctx, prog->decls[i]);
}

/* ========== CST -> AstProgram reconstruction (semantic_analyze_cst) ==========
 *
 * Rather than rewrite the ~3000-line analysis traversal onto views, the CST path
 * reconstructs an analyzable `AstProgram` directly from the immutable lossless CST
 * (mirroring lower/lower.c's lower_*_cst walk), then runs the SAME analysis core.
 * This guarantees the side-model + error contract is byte-identical to the
 * AstProgram path: each Expression/Statement carries cst_id = (CST node id + 1), so
 * the side model — keyed by `cst_id - 1` — is keyed by the exact node id the CST
 * lowerer reads back. Module CSTs are inlined + name-prefixed exactly as main.c's
 * resolve_uses does, and top-level tuple groups are expanded into archetype fields
 * exactly as main.c's expand_archetype_tuple_groups does. */

/* ---- small text helpers ---- */
static char *sem_txt_dup(CvText t) {
	char *s = malloc(t.len + 1);
	if (t.ptr)
		memcpy(s, t.ptr, t.len);
	s[t.len] = '\0';
	return s;
}
static char *sem_cv_dup(CstView v) {
	return sem_txt_dup(cv_text(v));
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
static SourceLoc sem_node_loc(const SyntaxNode *n) {
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

/* ---- type reconstruction (CST type node -> TypeRef) ---- */
static TypeRef *cst_build_type(CstView t);
static int cv_type_count_sem(CstView v);

/* The archetype name inside `handle<X>` / `handle(X)` (the IDENT that isn't "handle"). */
static char *cst_handle_name(CstView t) {
	for (int i = 0; i < t.node->child_count; i++)
		if (t.node->children[i].tag == SE_TOKEN && t.node->children[i].as.token.kind == TOK_IDENT) {
			CvText nm = {t.src + t.node->children[i].as.token.offset, t.node->children[i].as.token.length};
			if (!(nm.len == 6 && memcmp(nm.ptr, "handle", 6) == 0))
				return sem_txt_dup(nm);
		}
	return sem_dupz("");
}

static TypeRef *cst_build_type(CstView t) {
	if (!cv_present(t))
		return NULL;
	TypeRef *tr = malloc(sizeof(TypeRef));
	tr->loc.line = 0;
	tr->loc.column = 0;
	switch (cv_kind(t)) {
	case SN_TYPE_REF: {
		char *raw = sem_txt_dup(cv_token(t, TOK_IDENT));
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
			tr->data.name = raw; /* owned */
		}
		break;
	}
	case SN_TYPE_ARRAY: {
		tr->kind = TYPE_ARRAY;
		TypeRef *elem = malloc(sizeof(TypeRef));
		elem->loc = tr->loc;
		char *en = sem_txt_dup(cv_token(t, TOK_IDENT));
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
		char *en = sem_txt_dup(cv_token(t, TOK_IDENT));
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
		tr->data.handle.archetype_name = cst_handle_name(t);
		break;
	}
	case SN_TYPE_TUPLE: {
		/* `(x: T, y: U)` — field names are IDENTs, field types are the SN_TYPE_* children. */
		tr->kind = TYPE_TUPLE;
		int n = cv_type_count_sem(t);
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
				if (k >= SN_TYPE_REF && k <= SN_TYPE_HANDLE) {
					tr->data.tuple.field_names[fi] = sem_txt_dup((CvText){pend, pend ? pend_len : 0});
					tr->data.tuple.field_types[fi] = cst_build_type((CstView){ch->as.node, t.src});
					fi++;
					pend = NULL;
				}
			}
		}
		tr->data.tuple.field_count = fi;
		break;
	}
	default:
		tr->kind = TYPE_NAME;
		tr->data.name = sem_dupz("");
		break;
	}
	return tr;
}

/* ---- CST navigation (same shapes as lower/lower.c) ---- */
static CstView sem_first_expr(CstView v) {
	for (int i = 0; i < v.node->child_count; i++)
		if (v.node->children[i].tag == SE_NODE) {
			SyntaxNodeKind k = v.node->children[i].as.node->kind;
			if (k >= SN_LITERAL_EXPR && k <= SN_PAREN_EXPR)
				return (CstView){v.node->children[i].as.node, v.src};
		}
	return (CstView){NULL, v.src};
}
static CstView sem_type_at(CstView v, int idx) {
	int c = 0;
	for (int i = 0; i < v.node->child_count; i++)
		if (v.node->children[i].tag == SE_NODE) {
			SyntaxNodeKind k = v.node->children[i].as.node->kind;
			if (k >= SN_TYPE_REF && k <= SN_TYPE_HANDLE) {
				if (c == idx)
					return (CstView){v.node->children[i].as.node, v.src};
				c++;
			}
		}
	return (CstView){NULL, v.src};
}
static int cv_type_count_sem(CstView v) {
	int c = 0;
	for (int i = 0; i < v.node->child_count; i++)
		if (v.node->children[i].tag == SE_NODE) {
			SyntaxNodeKind k = v.node->children[i].as.node->kind;
			if (k >= SN_TYPE_REF && k <= SN_TYPE_HANDLE)
				c++;
		}
	return c;
}
static CstView sem_node_at_expr(CstView v, int idx) {
	int c = 0;
	for (int i = 0; i < v.node->child_count; i++)
		if (v.node->children[i].tag == SE_NODE) {
			SyntaxNodeKind k = v.node->children[i].as.node->kind;
			if (k >= SN_LITERAL_EXPR && k <= SN_PAREN_EXPR) {
				if (c == idx)
					return (CstView){v.node->children[i].as.node, v.src};
				c++;
			}
		}
	return (CstView){NULL, v.src};
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
static char *cst_decode_str(CvText raw, int *out_len) {
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

/* ---- expression reconstruction ---- */
static Expression *cst_build_expr(CstView e) {
	if (!cv_present(e))
		return NULL;
	Expression *ax = expression_create(EXPR_LITERAL);
	ax->cst_id = cv_id(e) + 1;
	ax->loc = sem_node_loc(e.node);

	switch (cv_kind(e)) {
	case SN_PAREN_EXPR: {
		Expression *inner = cst_build_expr(sem_first_expr(e));
		expression_free(ax);
		return inner;
	}
	case SN_LITERAL_EXPR:
		ax->type = EXPR_LITERAL;
		ax->data.literal.lexeme = sem_cv_dup(e);
		break;
	case SN_STRING_EXPR: {
		ax->type = EXPR_STRING;
		int n = 0;
		ax->data.string.value = cst_decode_str(cv_text(e), &n);
		ax->data.string.length = n;
		break;
	}
	case SN_NAME_EXPR: {
		ax->type = EXPR_NAME;
		ax->data.name.is_table_ref = 0;
		if (cv_has_token(e, TOK_LT)) {
			/* table<Name> in value position resolves to the bare archetype name */
			char *nm = NULL;
			int seen = 0;
			for (int i = 0; i < e.node->child_count; i++)
				if (e.node->children[i].tag == SE_TOKEN && e.node->children[i].as.token.kind == TOK_IDENT) {
					CvText t = {e.src + e.node->children[i].as.token.offset, e.node->children[i].as.token.length};
					if (seen++) {
						nm = sem_txt_dup(t);
						break;
					}
				}
			ax->data.name.name = nm ? nm : sem_cv_dup(e);
			ax->data.name.is_table_ref = 1;
		} else {
			ax->data.name.name = sem_txt_dup(cv_token(e, TOK_IDENT));
		}
		break;
	}
	case SN_FIELD_EXPR: {
		/* `base.f1.f2…[idx]` flat: base IDENT, then (DOT FIELD_NAME)+, optional trailing index. */
		Expression *base = expression_create(EXPR_NAME);
		base->data.name.name = sem_txt_dup(cv_token(e, TOK_IDENT));
		base->data.name.is_table_ref = 0;
		Expression *cur = base;
		int nfields = cv_count(e, SN_FIELD_NAME);
		for (int i = 0; i < nfields; i++) {
			Expression *f = expression_create(EXPR_FIELD);
			f->data.field.base = cur;
			f->data.field.field_name = sem_cv_dup(cv_child_at(e, SN_FIELD_NAME, i));
			cur = f;
		}
		if (cv_has_token(e, TOK_LBRACKET)) {
			Expression *idx = expression_create(EXPR_INDEX);
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
						    cst_build_expr((CstView){e.node->children[i].as.node, e.src});
				}
			cur = idx;
		}
		cur->cst_id = cv_id(e) + 1;
		expression_free(ax);
		return cur;
	}
	case SN_INDEX_EXPR: {
		ax->type = EXPR_INDEX;
		Expression *base = expression_create(EXPR_NAME);
		base->data.name.name = sem_txt_dup(cv_token(e, TOK_IDENT));
		base->data.name.is_table_ref = 0;
		int nfields = cv_count(e, SN_FIELD_NAME);
		for (int i = 0; i < nfields; i++) {
			Expression *f = expression_create(EXPR_FIELD);
			f->data.field.base = base;
			f->data.field.field_name = sem_cv_dup(cv_child_at(e, SN_FIELD_NAME, i));
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
					    cst_build_expr((CstView){e.node->children[i].as.node, e.src});
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
		Expression *callee = expression_create(EXPR_NAME);
		callee->data.name.name = sem_cv_dup(cv_child(e, SN_CALLEE_NAME));
		callee->data.name.is_table_ref = 0;
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
					    cst_build_expr((CstView){e.node->children[i].as.node, e.src});
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
					    cst_build_expr((CstView){e.node->children[i].as.node, e.src});
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
static Statement *cst_build_stmt(CstView s);

/* Lower the statement-kind child nodes of `parent` into a Statement array. */
static Statement **cst_build_body(CstView parent, int *out_count) {
	int n = 0;
	for (int i = 0; i < parent.node->child_count; i++)
		if (parent.node->children[i].tag == SE_NODE) {
			SyntaxNodeKind k = parent.node->children[i].as.node->kind;
			if (k >= SN_BIND_STMT && k <= SN_EACH_FIELD_STMT)
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
			if (k >= SN_BIND_STMT && k <= SN_EACH_FIELD_STMT)
				out[j++] = cst_build_stmt((CstView){parent.node->children[i].as.node, parent.src});
		}
	return out;
}

static Statement *cst_build_stmt(CstView s) {
	Statement *as = statement_create(STMT_EXPR);
	as->cst_id = cv_id(s) + 1;
	as->loc = sem_node_loc(s.node);

	switch (cv_kind(s)) {
	case SN_BIND_STMT: {
		as->type = STMT_BIND;
		CstView target = sem_node_at_expr(s, 0);
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
		CstView t0 = sem_type_at(s, 0);
		CstView t1 = sem_type_at(s, 1);
		CvText t0name = cv_present(t0) ? cv_token(t0, TOK_IDENT) : (CvText){NULL, 0};
		int t0_is_meta = t0name.ptr && t0name.len == 4 && memcmp(t0name.ptr, "type", 4) == 0;
		if (is_const && t0_is_meta && cv_present(t1)) {
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
	case SN_FREE_STMT:
		as->type = STMT_FREE;
		as->data.free_stmt.value = cst_build_expr(sem_node_at_expr(s, 0));
		break;
	case SN_BREAK_STMT:
		as->type = STMT_BREAK;
		break;
	case SN_RUN_STMT: {
		as->type = STMT_RUN;
		char *names[3] = {NULL, NULL, NULL};
		int ni = 0;
		for (int i = 0; i < s.node->child_count && ni < 3; i++)
			if (s.node->children[i].tag == SE_TOKEN && s.node->children[i].as.token.kind == TOK_IDENT) {
				CvText t = {s.src + s.node->children[i].as.token.offset, s.node->children[i].as.token.length};
				names[ni++] = sem_txt_dup(t);
			}
		as->data.run_stmt.system_name = names[1] ? names[1] : sem_dupz("");
		as->data.run_stmt.world_name = names[2];
		free(names[0]);
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
		CstView elsec = cv_child(s, SN_ELSE_CLAUSE);
		as->data.if_stmt.then_body = cst_build_body(s, &as->data.if_stmt.then_count);
		as->data.if_stmt.else_body = NULL;
		as->data.if_stmt.else_count = 0;
		if (cv_present(elsec))
			as->data.if_stmt.else_body = cst_build_body(elsec, &as->data.if_stmt.else_count);
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
		if (cv_has_token(s, TOK_LPAREN)) {
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
				CstView cv = {ch->as.node, s.src};
				if (seen_brace) {
					if (k >= SN_BIND_STMT && k <= SN_EACH_FIELD_STMT)
						nbody++;
					continue;
				}
				if (seg == 0 && k >= SN_BIND_STMT && k <= SN_EACH_FIELD_STMT)
					as->data.for_stmt.init = cst_build_stmt(cv);
				else if (seg == 1 && k >= SN_LITERAL_EXPR && k <= SN_PAREN_EXPR)
					as->data.for_stmt.condition = cst_build_expr(cv);
				else if (seg == 2 && k >= SN_BIND_STMT && k <= SN_EACH_FIELD_STMT)
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
				if (k >= SN_BIND_STMT && k <= SN_EACH_FIELD_STMT)
					as->data.for_stmt.body[as->data.for_stmt.body_count++] =
					    cst_build_stmt((CstView){ch->as.node, s.src});
			}
			break;
		}
		/* range form `for IDENT in IDENT { body }`, or infinite `for { body }`. */
		int ni = 0;
		char *vname = NULL, *iname = NULL;
		for (int i = 0; i < s.node->child_count; i++)
			if (s.node->children[i].tag == SE_TOKEN && s.node->children[i].as.token.kind == TOK_IDENT) {
				CvText t = {s.src + s.node->children[i].as.token.offset, s.node->children[i].as.token.length};
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
				CvText t = {s.src + s.node->children[i].as.token.offset, s.node->children[i].as.token.length};
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
			as->data.multi_bind.targets[ti].name = sem_txt_dup((CvText){pend, pend_len});                              \
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
					CvText t = cv_token((CstView){ch->as.node, s.src}, TOK_IDENT);
					pend = t.ptr;
					pend_len = (int)t.len;
					pend_active = 1;
				} else if (k >= SN_TYPE_REF && k <= SN_TYPE_HANDLE && pend_active) {
					pend_type = cst_build_type((CstView){ch->as.node, s.src});
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
					as->data.multi_bind.value = cst_build_expr((CstView){s.node->children[i].as.node, s.src});
					break;
				}
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
static Parameter *cst_build_param(CstView p) {
	Parameter *ap = parameter_create(sem_cv_dup(cv_child(p, SN_PARAM_NAME)), NULL);
	ap->type = cst_build_type(sem_type_at(p, 0)); /* NULL for sys params */
	ap->is_own = cv_has_token(p, TOK_OWN);
	ap->loc.line = 0;
	ap->loc.column = 0;
	return ap;
}

/* ---- declaration reconstruction (CST decl node -> Decl) ---- */
static Decl *cst_build_decl(CstView d) {
	switch (cv_kind(d)) {
	case SN_USE_DECL:
		return NULL; /* modules are inlined separately */
	case SN_WORLD_DECL: {
		Decl *ad = decl_create(DECL_WORLD);
		ad->data.world = world_decl_create(sem_txt_dup(cv_token(d, TOK_IDENT)));
		return ad;
	}
	case SN_ARCHETYPE_DECL: {
		Decl *ad = decl_create(DECL_ARCHETYPE);
		ArchetypeDecl *aa = archetype_decl_create(sem_cv_dup(cv_child(d, SN_TYPE_DEF_NAME)));
		int nf = cv_count(d, SN_FIELD_NAME);
		aa->fields = calloc(nf > 0 ? nf : 1, sizeof(FieldDecl *));
		aa->field_count = 0;
		for (int i = 0; i < d.node->child_count; i++) {
			if (d.node->children[i].tag != SE_NODE || d.node->children[i].as.node->kind != SN_FIELD_NAME)
				continue;
			CstView fn = {d.node->children[i].as.node, d.src};
			/* the inline component type is a type node before the next FIELD_NAME; else a bare
			 * field whose component type is the field's own name. Between the name and the type,
			 * a `type` keyword token marks the explicit meta longhand `name : type : T` (vs the
			 * inferred `name :: T`); preserve it so the formatter round-trips concrete syntax. */
			CstView ty = {NULL, d.src};
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
				if (kk >= SN_TYPE_REF && kk <= SN_TYPE_HANDLE) {
					ty.node = d.node->children[k].as.node;
					break;
				}
			}
			char *fname = sem_cv_dup(fn);
			TypeRef *ft;
			if (cv_present(ty)) {
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
		ProcDecl *ap = proc_decl_create(sem_cv_dup(cv_child(d, SN_FUNC_DEF_NAME)));
		ap->loc = sem_direct_token_loc(d.node, TOK_LPAREN); /* lint location: the `(`, like the parser */
		ap->is_extern = cv_has_token(d, TOK_EXTERN);
		ap->is_unsafe = cv_has_token(d, TOK_UNSAFE);
		ap->allow_pure_proc = cv_has_token(d, TOK_AT);
		int np = cv_count(d, SN_PARAM);
		ap->params = calloc(np ? np : 1, sizeof(Parameter *));
		for (int i = 0; i < np; i++)
			ap->params[i] = cst_build_param(cv_child_at(d, SN_PARAM, i));
		ap->param_count = np;
		int pnt = cv_type_count_sem(d); /* return types are the direct type-node children (0 = void) */
		ap->return_types = calloc(pnt ? pnt : 1, sizeof(TypeRef *));
		ap->return_type_count = 0;
		for (int i = 0; i < pnt; i++)
			ap->return_types[ap->return_type_count++] = cst_build_type(sem_type_at(d, i));
		ap->statements = cst_build_body(d, &ap->statement_count);
		ad->data.proc = ap;
		return ad;
	}
	case SN_SYS_DECL: {
		Decl *ad = decl_create(DECL_SYS);
		ad->loc = sem_node_loc(d.node);
		SysDecl *as = sys_decl_create(sem_cv_dup(cv_child(d, SN_FUNC_DEF_NAME)));
		as->loc = sem_direct_token_loc(d.node, TOK_LPAREN);
		int np = cv_count(d, SN_PARAM);
		as->params = calloc(np ? np : 1, sizeof(Parameter *));
		for (int i = 0; i < np; i++)
			as->params[i] = cst_build_param(cv_child_at(d, SN_PARAM, i));
		as->param_count = np;
		as->statements = cst_build_body(d, &as->statement_count);
		ad->data.sys = as;
		return ad;
	}
	case SN_FUNC_DECL: {
		Decl *ad = decl_create(DECL_FUNC);
		ad->loc = sem_node_loc(d.node);
		FuncDecl *af = func_decl_create(sem_cv_dup(cv_child(d, SN_FUNC_DEF_NAME)));
		af->loc = sem_direct_token_loc(d.node, TOK_LPAREN);
		af->is_extern = cv_has_token(d, TOK_EXTERN);
		af->is_unsafe = cv_has_token(d, TOK_UNSAFE);
		int np = cv_count(d, SN_PARAM);
		af->params = calloc(np ? np : 1, sizeof(Parameter *));
		for (int i = 0; i < np; i++)
			af->params[i] = cst_build_param(cv_child_at(d, SN_PARAM, i));
		af->param_count = np;
		int nt = cv_type_count_sem(d); /* return types are the direct type-node children */
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
		FuncGroup *fg = func_group_create(sem_cv_dup(cv_child(d, SN_FUNC_DEF_NAME)));
		fg->loc = sem_direct_token_loc(d.node, TOK_LBRACE);
		int nmem = 0;
		for (int i = 0; i < d.node->child_count; i++)
			if (d.node->children[i].tag == SE_TOKEN && d.node->children[i].as.token.kind == TOK_IDENT)
				nmem++;
		fg->member_names = calloc(nmem ? nmem : 1, sizeof(char *));
		fg->member_count = 0;
		for (int i = 0; i < d.node->child_count; i++)
			if (d.node->children[i].tag == SE_TOKEN && d.node->children[i].as.token.kind == TOK_IDENT) {
				CvText t = {d.src + d.node->children[i].as.token.offset, d.node->children[i].as.token.length};
				fg->member_names[fg->member_count++] = sem_txt_dup(t);
			}
		ad->data.func_group = fg;
		return ad;
	}
	case SN_CONST_DECL: {
		Decl *ad = decl_create(DECL_CONST);
		char *cname = sem_txt_dup(cv_token(d, TOK_IDENT));
		ConstDecl *ac = const_decl_create(cname, NULL);
		ac->decl_type = NULL;
		ac->type_value = NULL;
		ac->value = NULL;
		if (cv_has_token(d, TOK_LPAREN)) {
			/* tuple group `name (a, b, …) :: T`: a nominal type alias whose RHS is a TYPE_TUPLE
			 * built from the parenthesized suffixes, each typed by the shared type after `::`. */
			CstView memberty = sem_type_at(d, 0);
			TypeRef *shared = cv_present(memberty) ? cst_build_type(memberty) : NULL;
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
						CvText t = {d.src + d.node->children[i].as.token.offset, d.node->children[i].as.token.length};
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
		CstView t0 = sem_type_at(d, 0);
		CstView t1 = sem_type_at(d, 1);
		CvText t0name = cv_present(t0) ? cv_token(t0, TOK_IDENT) : (CvText){NULL, 0};
		int t0_is_meta = t0name.ptr && t0name.len == 4 && memcmp(t0name.ptr, "type", 4) == 0;
		if (t0_is_meta && cv_present(t1)) {
			ac->decl_type = cst_build_type(t0); /* TYPE_TYPE */
			ac->type_value = cst_build_type(t1);
		} else {
			if (cv_present(t0))
				ac->decl_type = cst_build_type(t0);
			ac->value = cst_build_expr(sem_node_at_expr(d, 0));
		}
		ad->data.constant = ac;
		return ad;
	}
	case SN_STATIC_DECL: {
		Decl *ad = decl_create(DECL_STATIC);
		if (cv_has_token(d, TOK_LPAREN)) {
			/* `static Name(count[, init])` / `static pool<Name>(count) { field: v, … }` */
			CstView ty = sem_type_at(d, 0);
			char *an = NULL;
			if (cv_present(ty) && cv_has_token(ty, TOK_LT)) {
				int seen = 0;
				for (int i = 0; i < ty.node->child_count; i++)
					if (ty.node->children[i].tag == SE_TOKEN && ty.node->children[i].as.token.kind == TOK_IDENT) {
						CvText t = {ty.src + ty.node->children[i].as.token.offset,
						            ty.node->children[i].as.token.length};
						if (seen++) {
							an = sem_txt_dup(t);
							break;
						}
					}
			} else if (cv_present(ty)) {
				an = sem_txt_dup(cv_token(ty, TOK_IDENT));
			}
			StaticDecl *sd = static_decl_archetype_create(an ? an : sem_dupz(""));
			int cap_alloc = d.node->child_count + 1;
			sd->archetype.field_names = calloc(cap_alloc, sizeof(char *));
			sd->archetype.field_values = calloc(cap_alloc, sizeof(Expression *));
			sd->archetype.field_count = 0;
			sd->archetype.init_length = NULL;
			int phase = 0; /* 0=outside, 1=in (), 2=in {} */
			int paren_arg = 0;
			const char *pend = NULL;
			int pend_len = 0;
			for (int i = 0; i < d.node->child_count; i++) {
				SyntaxElem *ch = &d.node->children[i];
				if (ch->tag == SE_TOKEN) {
					switch (ch->as.token.kind) {
					case TOK_LPAREN:
						phase = 1;
						break;
					case TOK_RPAREN:
						phase = 0;
						break;
					case TOK_LBRACE:
						phase = 2;
						break;
					case TOK_RBRACE:
						phase = 0;
						break;
					case TOK_IDENT:
						if (phase == 2) {
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
				CstView ev = {ch->as.node, d.src};
				if (phase == 1) {
					if (paren_arg == 0) {
						sd->archetype.field_values[0] = cst_build_expr(ev);
						sd->archetype.field_names[0] = NULL;
						sd->archetype.field_count = 1;
					} else if (paren_arg == 1) {
						sd->archetype.init_length = cst_build_expr(ev);
					}
					paren_arg++;
				} else if (phase == 2 && pend) {
					int fc = sd->archetype.field_count;
					sd->archetype.field_names[fc] = sem_txt_dup((CvText){pend, pend_len});
					sd->archetype.field_values[fc] = cst_build_expr(ev);
					sd->archetype.field_count++;
					pend = NULL;
				}
			}
			ad->data.static_decl = sd;
		} else {
			/* `static name : T[size]` — static array. name in the first type node; array type 2nd. */
			CstView name_ty = sem_type_at(d, 0);
			char *aname = sem_txt_dup(cv_token(name_ty, TOK_IDENT));
			CstView arr_ty = sem_type_at(d, 1);
			TypeRef *full = cst_build_type(arr_ty);
			TypeRef *elem = full;
			if (full && full->kind == TYPE_SHAPED_ARRAY)
				elem = full->data.shaped_array.element_type;
			else if (full && full->kind == TYPE_ARRAY)
				elem = full->data.array.element_type;
			int size = 0;
			if (cv_present(arr_ty))
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
			if (full && full != elem)
				free(full); /* shaped/array wrapper node; element ownership moved to sd */
			ad->data.static_decl = sd;
		}
		return ad;
	}
	default:
		return NULL;
	}
}

/* ---- module CST registry (parallel to lower_add_module) ---- */
typedef struct {
	char *name;
	const SyntaxNode *root;
	const char *src;
} SemModule;
static SemModule g_sem_modules[64];
static int g_sem_module_count = 0;

void semantic_add_module(const char *name, const SyntaxNode *root, const char *src) {
	if (g_sem_module_count >= 64 || !name || !root)
		return;
	g_sem_modules[g_sem_module_count].name = sem_dupz(name);
	g_sem_modules[g_sem_module_count].root = root;
	g_sem_modules[g_sem_module_count].src = src;
	g_sem_module_count++;
}

/* ---- module name-prefixing (mirrors main.c resolve_uses / prefix_module) ---- */
static int sem_name_in_set(char **set, int count, const char *name) {
	for (int i = 0; i < count; i++)
		if (strcmp(set[i], name) == 0)
			return 1;
	return 0;
}
static char *sem_prefix_name(const char *prefix, const char *name) {
	int p = (int)strlen(prefix), n = (int)strlen(name);
	char *r = malloc(p + 1 + n + 1);
	memcpy(r, prefix, p);
	r[p] = '_';
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
	case STMT_FREE:
		sem_rename_expr(s->data.free_stmt.value, prefix, set, count);
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
		return d->data.static_decl->kind == STATIC_KIND_ARRAY ? d->data.static_decl->array.name
		                                                      : d->data.static_decl->archetype.archetype_name;
	case DECL_CONST:
		return d->data.constant->name;
	case DECL_WORLD:
		return d->data.world->name;
	default:
		return NULL;
	}
}
static void sem_rename_decl(Decl *d, const char *prefix, char **set, int count) {
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

/* Build the analyzable AstProgram from the main-file CST plus all registered module CSTs,
 * inlining + name-prefixing modules exactly as main.c's resolve_uses does, then expanding
 * top-level tuple groups. The returned AstProgram owns all its memory (free with ast_program_free). */
static AstProgram *cst_to_program(const SyntaxNode *root, const char *src) {
	AstProgram *prog = ast_program_create();
	CstView r = cv_root(root, src);
	int cap = cv_node_count(r) + 8;
	for (int m = 0; m < g_sem_module_count; m++)
		cap += cv_node_count(cv_root(g_sem_modules[m].root, g_sem_modules[m].src)) + 1;
	prog->decls = calloc(cap ? cap : 1, sizeof(Decl *));
	prog->decl_count = 0;

	/* Per-module: prefix + exported (still-bare) names, for cross-module resolution. */
	char *acc_prefix[64];
	char **acc_set[64];
	int acc_count[64];
	int acc_n = 0;

	for (int i = 0; i < root->child_count; i++) {
		if (root->children[i].tag != SE_NODE)
			continue;
		SyntaxNodeKind k = root->children[i].as.node->kind;
		if (k < SN_WORLD_DECL || k > SN_USE_DECL)
			continue;
		CstView dv = {root->children[i].as.node, src};

		if (k == SN_USE_DECL) {
			char *mod_name = sem_txt_dup(cv_token(dv, TOK_IDENT));
			SemModule *mod = NULL;
			for (int m = 0; m < g_sem_module_count; m++)
				if (strcmp(g_sem_modules[m].name, mod_name) == 0) {
					mod = &g_sem_modules[m];
					break;
				}
			if (!mod) {
				free(mod_name);
				continue;
			}
			int first = prog->decl_count;
			const SyntaxNode *mr = mod->root;
			for (int j = 0; j < mr->child_count; j++) {
				if (mr->children[j].tag != SE_NODE)
					continue;
				SyntaxNodeKind mk = mr->children[j].as.node->kind;
				if (mk < SN_WORLD_DECL || mk > SN_USE_DECL || mk == SN_USE_DECL)
					continue;
				Decl *md = cst_build_decl((CstView){mr->children[j].as.node, mod->src});
				if (md)
					prog->decls[prog->decl_count++] = md;
			}
			/* collect this module's exported (bare) names, then prefix them. */
			int ndefs = prog->decl_count - first;
			char **exports = calloc(ndefs ? ndefs : 1, sizeof(char *));
			int en = 0;
			for (int dd = first; dd < prog->decl_count; dd++) {
				const char *nm = sem_decl_name(prog->decls[dd]);
				if (nm)
					exports[en++] = sem_dupz(nm);
			}
			for (int dd = first; dd < prog->decl_count; dd++)
				sem_rename_decl(prog->decls[dd], mod->name, exports, en);
			if (acc_n < 64) {
				acc_prefix[acc_n] = sem_dupz(mod->name);
				acc_set[acc_n] = exports;
				acc_count[acc_n] = en;
				acc_n++;
			} else {
				for (int e = 0; e < en; e++)
					free(exports[e]);
				free(exports);
			}
			free(mod_name);
			continue;
		}

		Decl *ad = cst_build_decl(dv);
		if (ad)
			prog->decls[prog->decl_count++] = ad;
	}

	/* Cross-module bare-reference resolution: rewrite bare refs to a module export that has
	 * no top-level definition into the prefixed name (collisions left alone). */
	for (int m = 0; m < acc_n; m++) {
		char *dangling[256];
		int dn = 0;
		for (int s2 = 0; s2 < acc_count[m] && dn < 256; s2++) {
			int defined = 0;
			for (int dd = 0; dd < prog->decl_count; dd++) {
				const char *nm = sem_decl_name(prog->decls[dd]);
				if (nm && strcmp(nm, acc_set[m][s2]) == 0) {
					defined = 1;
					break;
				}
			}
			if (!defined)
				dangling[dn++] = acc_set[m][s2];
		}
		if (dn > 0)
			for (int dd = 0; dd < prog->decl_count; dd++)
				sem_rename_decl(prog->decls[dd], acc_prefix[m], dangling, dn);
	}
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
 * Both entry points run this — semantic_analyze on the parser-built AstProgram, and
 * semantic_analyze_cst on a AstProgram reconstructed from the CST. `erase` controls the
 * final alias-erasure pass: the AstProgram lowerer needs it; the CST lowerer does not
 * (it reads aliases from the side model), so the CST path passes erase=0. */
static void analyze_program_core(SemanticContext *ctx, AstProgram *prog, int erase) {
	if (!prog)
		return;
	ctx->prog = prog;

	/* pass 0: collect constants and nominal type aliases. A `name : [meta] : value` decl is a
	 * value const when its RHS denotes a value, or a type alias when its RHS denotes a type.
	 * Unambiguous forms (type-form RHS, literal RHS) are classified directly; a bare-name RHS is
	 * deferred and resolved by a small fixpoint below — so `tau :: pi` is a value const (not an
	 * alias), and chains / forward refs resolve regardless of declaration order. */
	int deferred_cap = prog->decl_count > 0 ? prog->decl_count : 1;
	const char **deferred_name = malloc(sizeof(char *) * deferred_cap);
	const char **deferred_rhs = malloc(sizeof(char *) * deferred_cap);
	int *deferred_value_ctx = malloc(sizeof(int) * deferred_cap);
	int *deferred_done = calloc(deferred_cap, sizeof(int));
	int deferred_count = 0;

	for (int i = 0; i < prog->decl_count; i++) {
		if (prog->decls[i]->kind != DECL_CONST)
			continue;
		ConstDecl *c = prog->decls[i]->data.constant;

		/* Type-form RHS (`name : type : T`, or the tuple `name :: (x,y:..)`): a nominal alias. */
		if (c->type_value) {
			if (c->type_value->kind == TYPE_TUPLE) {
				for (int f = 0; f < c->type_value->data.tuple.field_count; f++) {
					const char *fbacking = type_backing_name(c->type_value->data.tuple.field_types[f]);
					if (!fbacking) {
						error(ctx, "tuple field must be a simple type (nested tuples are not allowed)");
						continue;
					}
					const char *fn = c->type_value->data.tuple.field_names[f];
					size_t L = strlen(c->name) + 1 + strlen(fn) + 1;
					char *aname = malloc(L);
					snprintf(aname, L, "%s_%s", c->name, fn);
					register_type_alias(ctx, aname, fbacking); /* aname leaks like the old path */
				}
			} else {
				const char *backing = type_backing_name(c->type_value);
				if (!backing)
					error(ctx, "a type alias backing must be a type name, `opaque`, or a tuple");
				else
					register_type_alias(ctx, c->name, backing);
			}
			continue;
		}

		/* Literal RHS: a value const. Its type is the explicit declared type if present, else the
		 * literal's own type (`3.14`→float, `42`→int) — so the const resolves to its real type. */
		if (c->value && c->value->type == EXPR_LITERAL) {
			const char *vt = NULL;
			if (c->decl_type && c->decl_type->kind == TYPE_NAME)
				vt = resolve_type_alias(ctx, normalize_type_name(c->decl_type->data.name));
			if (!vt)
				vt = resolve_expression_type(ctx, c->value);
			register_value_const(ctx, c->name, c->value->data.literal.lexeme, vt);
			continue;
		}

		/* Bare-name RHS: ambiguous (type or value) — defer for the fixpoint. An explicit concrete
		 * declared type (`PI : float : x`) forces value context. */
		if (c->value && c->value->type == EXPR_NAME) {
			deferred_name[deferred_count] = c->name;
			deferred_rhs[deferred_count] = c->value->data.name.name;
			deferred_value_ctx[deferred_count] = (c->decl_type != NULL);
			deferred_count++;
			continue;
		}

		error(ctx, "a constant's value must be a literal, a name, or a type");
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
					char msg[256];
					snprintf(msg, sizeof(msg), "constant '%s' has a value type but its RHS '%s' names a type",
					         deferred_name[d], r);
					error(ctx, msg);
				} else {
					register_type_alias(ctx, deferred_name[d], r);
				}
				deferred_done[d] = 1;
				progress = 1;
				continue;
			}
			const char *lex = value_const_lexeme(ctx, r);
			if (lex) {
				/* `tau :: pi` — tau takes pi's value AND pi's type. */
				register_value_const(ctx, deferred_name[d], lex, value_const_type(ctx, r));
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
			char msg[256];
			snprintf(msg, sizeof(msg), "unknown value '%s' in declaration of '%s'", deferred_rhs[d], deferred_name[d]);
			error(ctx, msg);
		} else {
			register_type_alias(ctx, deferred_name[d], deferred_rhs[d]);
		}
	}
	free(deferred_name);
	free(deferred_rhs);
	free(deferred_value_ctx);
	free(deferred_done);

	/* Inline component definitions: `arche Foo { hp :: int, … }` mints the nominal type `hp`
	 * (≡ a top-level `hp :: int`) and includes it as a component — so the component is a real
	 * type, not a bare field label. A bare reference (`arche Foo { hp }`) has field type == its
	 * own name and is skipped here. Array/tuple components are the column's own type, not a
	 * registerable alias. Redefinition must agree, exactly as for top-level aliases. */
	for (int i = 0; i < prog->decl_count; i++) {
		if (prog->decls[i]->kind != DECL_ARCHETYPE)
			continue;
		ArchetypeDecl *a = prog->decls[i]->data.archetype;
		for (int f = 0; f < a->field_count; f++) {
			FieldDecl *fd = a->fields[f];
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
					if (strcmp(ctx->type_alias_backings[j], backing) != 0) {
						char msg[256];
						snprintf(msg, sizeof(msg), "type '%s' redefined with a different backing", fd->name);
						error(ctx, msg);
					}
					done = 1;
					break;
				}
			}
			if (done)
				continue;
			ctx->type_alias_names = realloc(ctx->type_alias_names, (ctx->type_alias_count + 1) * sizeof(char *));
			ctx->type_alias_backings = realloc(ctx->type_alias_backings, (ctx->type_alias_count + 1) * sizeof(char *));
			ctx->type_alias_names[ctx->type_alias_count] = fd->name;
			ctx->type_alias_backings[ctx->type_alias_count] = (char *)backing;
			ctx->type_alias_count++;
		}
	}

	/* Validate each alias resolves (through any chain) to a real backing type. */
	for (int i = 0; i < ctx->type_alias_count; i++) {
		const char *b = resolve_type_alias(ctx, ctx->type_alias_names[i]);
		if (!is_primitive_type_name(b) && strcmp(b, "opaque") != 0) {
			char msg[256];
			snprintf(msg, sizeof(msg), "type alias '%s' has unknown backing type '%s'", ctx->type_alias_names[i], b);
			error(ctx, msg);
		}
	}

	/* Typed value consts: the explicit declared type now constrains the literal (no longer inert). */
	for (int i = 0; i < prog->decl_count; i++) {
		if (prog->decls[i]->kind == DECL_CONST)
			check_const_literal_type(ctx, prog->decls[i]->data.constant);
	}

	/* global scope: holds module-level variables (static arrays, etc.) */
	push_scope(ctx);

	/* pass 1: collect all archetypes */
	for (int i = 0; i < prog->decl_count; i++) {
		if (prog->decls[i]->kind == DECL_ARCHETYPE) {
			analyze_decl(ctx, prog->decls[i]);
		}
	}

	/* pass 2: analyze other declarations */
	for (int i = 0; i < prog->decl_count; i++) {
		if (prog->decls[i]->kind != DECL_ARCHETYPE && prog->decls[i]->kind != DECL_CONST) {
			analyze_decl(ctx, prog->decls[i]);
		}
	}

	/* pass 3: erase nominal aliases to their backing (zero-cost; codegen never sees them).
	 * Only the legacy AstProgram lowerer needs this tree mutation; the CST lowerer reads
	 * aliases from the side model, so the CST path skips erasure (erase=0). */
	if (erase)
		erase_type_aliases(ctx, prog);
}

/* Allocate + zero-initialize a SemanticContext and register builtins. Shared by both
 * entry points. */
static SemanticContext *make_context(void) {
	SemanticContext *ctx = malloc(sizeof(SemanticContext));
	ctx->archetypes = NULL;
	ctx->archetype_count = 0;
	ctx->aliases = NULL;
	ctx->alias_count = 0;
	ctx->known_funcs = NULL;
	ctx->known_func_count = 0;
	ctx->groups = NULL;
	ctx->group_count = 0;
	ctx->const_names = NULL;
	ctx->const_values = NULL;
	ctx->const_value_types = NULL;
	ctx->const_count = 0;
	ctx->type_alias_names = NULL;
	ctx->type_alias_backings = NULL;
	ctx->type_alias_count = 0;
	ctx->scopes = NULL;
	ctx->scope_count = 0;
	ctx->error_count = 0;
	ctx->current_sys_archetype = NULL;
	ctx->current_proc = NULL;
	ctx->in_body = 0;
	ctx->in_unsafe = 0;
	ctx->prog = NULL;
	ctx->owned_prog = NULL;
	ctx->model = sem_model_new();
	ctx->hints = sem_hints_new();
	ctx->diags = NULL;
	ctx->diag_count = 0;
	ctx->diag_cap = 0;
	ctx->current_loc = (SourceLoc){0, 0};

	register_func(ctx, "write");
	register_func(ctx, "insert");
	register_func(ctx, "delete");
	register_func(ctx, "dealloc");
	return ctx;
}

SemanticContext *semantic_analyze_cst(const SyntaxNode *root, const char *src) {
	SemanticContext *ctx = make_context();
	if (!root)
		return ctx;
	/* Reconstruct the analyzable AstProgram from the immutable CST (+ module CSTs), keep it
	 * alive on the context, and run the SAME analysis core. erase=0: the CST lowerer reads
	 * aliases from the side model, so the alias-erasure tree mutation is not needed. */
	ctx->owned_prog = cst_to_program(root, src);
	analyze_program_core(ctx, ctx->owned_prog, /*erase=*/0);
	return ctx;
}

/* Test/helper entry: parse `src`, build the abstract `AstProgram` from the resulting lossless
 * CST (via cst_to_program), then free the parse result + CST. The reconstructed AstProgram owns
 * all its memory (every string is copied out of the source), so it outlives the CST and can
 * be freed with ast_program_free. This is the only sanctioned way to obtain a `AstProgram` now that
 * the parser produces just the CST — unit tests use it to validate cst_to_program faithfully
 * reconstructs each construct. Returns NULL on parse error. */
AstProgram *cst_to_program_from_source(const char *src) {
	ParseResult pr = parse_source(src);
	if (pr.error_count > 0 || !pr.cst_root) {
		parse_result_free(&pr);
		return NULL;
	}
	AstProgram *prog = cst_to_program(pr.cst_root, src);
	parse_result_free(&pr);
	return prog;
}

void semantic_context_free(SemanticContext *ctx) {
	if (!ctx)
		return;

	sem_model_free(ctx->model);
	sem_hints_free(ctx->hints);

	for (int i = 0; i < ctx->diag_count; i++)
		free(ctx->diags[i].message);
	free(ctx->diags);

	/* free constants (names only, values are owned by AST) */
	free(ctx->const_names);
	free(ctx->const_values);
	free(ctx->const_value_types);

	/* free type-alias tables (name/backing strings are owned by the CST) */
	free(ctx->type_alias_names);
	free(ctx->type_alias_backings);

	/* free shapes (one per unique column structure) */
	for (int i = 0; i < ctx->archetype_count; i++) {
		ArchetypeInfo *arch = ctx->archetypes[i];
		free(arch->signature);
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

	/* free group table (members are borrowed from CST) */
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

	/* CST path: free the AstProgram we reconstructed (and kept alive for the side model). */
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

int sem_diag_count(const SemanticContext *ctx) {
	return ctx ? ctx->diag_count : 0;
}

const SemDiag *sem_diag_at(const SemanticContext *ctx, int i) {
	if (!ctx || i < 0 || i >= ctx->diag_count)
		return NULL;
	return &ctx->diags[i];
}

const char *semantic_resolve_type_alias(SemanticContext *ctx, const char *name) {
	if (!ctx || !name)
		return name;
	return resolve_type_alias(ctx, name);
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
