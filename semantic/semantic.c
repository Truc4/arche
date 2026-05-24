#include "semantic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

	char **const_names;        /* compile-time constant names */
	const char **const_values; /* literal lexeme strings */
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

	/* Program for looking up declarations */
	Program *prog;
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
	}
	return 0;
}

static FuncDecl *find_func_decl_cst(Program *prog, const char *name) {
	if (!prog)
		return NULL;
	for (int i = 0; i < prog->decl_count; i++) {
		Decl *d = prog->decls[i];
		if (d->kind == DECL_FUNC && strcmp(d->data.func->name, name) == 0)
			return d->data.func;
	}
	return NULL;
}

static VariableInfo *find_variable(SemanticContext *ctx, const char *name) {
	/* search from innermost to outermost scope */
	for (int i = ctx->scope_count - 1; i >= 0; i--) {
		Scope *scope = &ctx->scopes[i];
		for (int j = 0; j < scope->var_count; j++) {
			if (strcmp(scope->vars[j]->name, name) == 0) {
				return scope->vars[j];
			}
		}
	}
	return NULL;
}

static void error(SemanticContext *ctx, const char *msg) {
	ctx->error_count++;
	fprintf(stderr, "Semantic error: %s\n", msg);
	fflush(stderr);
}

/* ========== LINT CONFIGURATION ========== */
/* Both lints enabled by default. CLI flags can disable or promote to error. */
static struct {
	int proc_could_be_func_enabled;
	int proc_could_be_func_werror;
	int proc_no_effect_enabled;
	int proc_no_effect_werror;
} g_lint_config = {
    .proc_could_be_func_enabled = 1,
    .proc_could_be_func_werror = 0,
    .proc_no_effect_enabled = 1,
    .proc_no_effect_werror = 0,
};

void semantic_set_lint_proc_could_be_func(int enabled, int werror) {
	g_lint_config.proc_could_be_func_enabled = enabled;
	g_lint_config.proc_could_be_func_werror = werror;
}

void semantic_set_lint_proc_no_effect(int enabled, int werror) {
	g_lint_config.proc_no_effect_enabled = enabled;
	g_lint_config.proc_no_effect_werror = werror;
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
		if (v->type->kind == TYPE_NAME &&
		    strcmp(resolve_type_alias(ctx, v->type->data.name), "opaque") == 0)
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

/* Flag the most-recently-added variable as a (borrowed) parameter — exempt from the
 * opaque must-consume check at scope exit. */
static void mark_last_param(SemanticContext *ctx) {
	if (ctx->scope_count > 0) {
		Scope *s = &ctx->scopes[ctx->scope_count - 1];
		if (s->var_count > 0)
			s->vars[s->var_count - 1]->is_param = 1;
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
	var->loc.line = 0;
	var->loc.column = 0;

	scope->vars = realloc(scope->vars, (scope->var_count + 1) * sizeof(VariableInfo *));
	scope->vars[scope->var_count++] = var;
}

/* ========== FORWARD DECLARATIONS ========== */

static void analyze_expression(SemanticContext *ctx, Expression *expr);
static void analyze_statement(SemanticContext *ctx, Statement *stmt);
static const char *resolve_expression_type(SemanticContext *ctx, Expression *expr);

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

/* A parameter may carry `consume` if it owns a foreign resource: an `opaque` cell — the
 * move-only resource value — including a nominal alias whose backing is `opaque`. */
static int is_consumable_ref(SemanticContext *ctx, const TypeRef *tr) {
	if (tr && tr->kind == TYPE_OPAQUE)
		return 1;
	/* A nominal alias whose backing is `opaque` (e.g. `resource :: opaque`) is a
	 * consumable foreign-resource value, same as a bare opaque. */
	if (tr && tr->kind == TYPE_NAME && strcmp(resolve_type_alias(ctx, tr->data.name), "opaque") == 0)
		return 1;
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
		/* Index expression has same type as base element */
		return resolve_expression_type(ctx, expr->data.index.base);
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
				if (ok && fd->return_type && fd->return_type->kind == TYPE_NAME) {
					return normalize_type_name(fd->return_type->data.name);
				}
			}
			return NULL;
		}

		/* Plain func path (unchanged from before). */
		for (int i = 0; i < ctx->prog->decl_count; i++) {
			Decl *decl = ctx->prog->decls[i];
			if (decl->kind == DECL_FUNC && strcmp(decl->data.func->name, func_name) == 0) {
				TypeRef *rt = decl->data.func->return_type;
				if (!rt)
					return NULL;
				if (rt->kind == TYPE_HANDLE)
					return rt->data.handle.archetype_name;
				if (rt->kind == TYPE_NAME)
					return resolve_type_alias(ctx, normalize_type_name(rt->data.name));
				if (rt->kind == TYPE_ARRAY)
					return "char_array"; /* extern func returning char[] (raw byte view) */
				if (rt->kind == TYPE_OPAQUE)
					return "opaque"; /* foreign resource value (pointer-width i64) */
				return NULL;
			}
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
	/* `move x` is a transparent ownership marker — its nominal type is the operand's. */
	if (e->type == EXPR_UNARY && e->data.unary.op == UNARY_MOVE)
		return nominal_type_of_expr(ctx, e->data.unary.operand);
	if (e->type == EXPR_NAME) {
		VariableInfo *v = find_variable(ctx, e->data.name.name);
		return v ? v->nominal_type : NULL;
	}
	if (e->type == EXPR_CALL && e->data.call.callee && e->data.call.callee->type == EXPR_NAME) {
		FuncDecl *fd = find_func_decl_cst(ctx->prog, e->data.call.callee->data.name.name);
		if (fd && fd->return_type && fd->return_type->kind == TYPE_NAME && is_type_alias(ctx, fd->return_type->data.name))
			return fd->return_type->data.name;
	}
	return NULL;
}

/* ========== EXPRESSION ANALYSIS ========== */

static void analyze_expression(SemanticContext *ctx, Expression *expr) {
	if (!expr)
		return;

	switch (expr->type) {
	case EXPR_LITERAL:
		/* literals are always valid */
		break;

	case EXPR_STRING:
		/* string literals are always valid */
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
		/* `move x` transfers ownership: mark x consumed (use-after-move is an error). */
		if (expr->data.unary.op == UNARY_MOVE && expr->data.unary.operand->type == EXPR_NAME) {
			VariableInfo *mv = find_variable(ctx, expr->data.unary.operand->data.name.name);
			if (mv)
				mv->is_consumed = 1;
		}
		break;

	case EXPR_CALL: {
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

					/* Ownership transfer into a `consume` parameter must be explicit: an
					 * opaque value cannot copy, so a named opaque binding handed to a consume
					 * param must be written `move x`. The `move` itself (UNARY_MOVE, handled in
					 * analyze_expression) marks the binding consumed; a bare name is an error.
					 * (Rvalues — e.g. a call result — have no binding to kill, so move is not
					 * required there.) Runs for BOTH extern and non-extern callees. */
					{
						int ac = expr->data.call.arg_count;
						int n = param_count < ac ? param_count : ac;
						for (int j = 0; j < n; j++) {
							Parameter *p = params[j];
							if (!p || !p->is_consume || !expr->data.call.args[j])
								continue;
							Expression *a = expr->data.call.args[j];
							if (a->type == EXPR_NAME) {
								VariableInfo *cv = find_variable(ctx, a->data.name.name);
								if (cv && var_is_opaque(ctx, cv)) {
									char msg[256];
									snprintf(msg, sizeof(msg),
									         "opaque value '%s' must be moved into consuming parameter '%s' of "
									         "'%s' (write `move %s`)",
									         a->data.name.name, p->name ? p->name : "?", func_name,
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

	/* Resolve and store the type of this expression */
	expr->resolved_type = (char *)resolve_expression_type(ctx, expr);
}

/* ========== STATEMENT ANALYSIS ========== */

static void analyze_statement(SemanticContext *ctx, Statement *stmt) {
	if (!stmt)
		return;

	switch (stmt->type) {
	case STMT_BIND: {
		/* `archetype` is only valid as a parameter type. */
		if (stmt->data.bind_stmt.type && stmt->data.bind_stmt.type->kind == TYPE_ARCHETYPE) {
			error(ctx, "`archetype` is only valid as a parameter type");
			break;
		}
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
			/* Now analyze the call expression after variables are defined */
			analyze_expression(ctx, stmt->data.bind_stmt.value);
		} else {
			/* Single-value let or non-call multivalue expressions: analyze value first */
			analyze_expression(ctx, stmt->data.bind_stmt.value);

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
		break;
	}

	case STMT_MULTI_BIND: {
		/* Multi-bind: (let x:, y) = expr */
		/* Add new variables FIRST so they're in scope for the RHS expression */
		for (int i = 0; i < stmt->data.multi_bind.target_count; i++) {
			BindingTarget *target = &stmt->data.multi_bind.targets[i];
			if (target->is_new) {
				add_variable(ctx, target->name, target->type);
			}
		}

		/* Now analyze the RHS expression (out params can reference newly declared vars) */
		analyze_expression(ctx, stmt->data.multi_bind.value);

		/* Validate that existing variables are actually defined */
		for (int i = 0; i < stmt->data.multi_bind.target_count; i++) {
			BindingTarget *target = &stmt->data.multi_bind.targets[i];
			if (!target->is_new) {
				VariableInfo *existing = find_variable(ctx, target->name);
				if (!existing) {
					char msg[256];
					snprintf(msg, sizeof(msg), "Variable '%s' not declared", target->name);
					error(ctx, msg);
				}
			}
		}
		break;
	}

	case STMT_ASSIGN:
		analyze_expression(ctx, stmt->data.assign_stmt.target);
		analyze_expression(ctx, stmt->data.assign_stmt.value);
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
		analyze_expression(ctx, stmt->data.expr_stmt.expr);
		break;

	case STMT_RETURN:
		analyze_expression(ctx, stmt->data.return_stmt.value);
		/* Returning an opaque binding moves it out — counts as consumption. (Data and
		 * handles copy, so returning them must NOT kill the binding.) */
		if (stmt->data.return_stmt.value && stmt->data.return_stmt.value->type == EXPR_NAME) {
			VariableInfo *rv = find_variable(ctx, stmt->data.return_stmt.value->data.name.name);
			if (rv && var_is_opaque(ctx, rv))
				rv->is_consumed = 1;
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
}

/* ========== DECLARATION ANALYSIS ========== */

static void analyze_archetype_decl(SemanticContext *ctx, ArchetypeDecl *arch) {
	if (!arch)
		return;

	/* Set semantics: a component type may appear at most once in an archetype. The
	 * component's type name IS its access path, so a repeat would be unreachable. */
	for (int i = 0; i < arch->field_count; i++) {
		for (int j = i + 1; j < arch->field_count; j++) {
			if (arch->fields[i]->name && arch->fields[j]->name &&
			    strcmp(arch->fields[i]->name, arch->fields[j]->name) == 0) {
				char msg[256];
				snprintf(msg, sizeof(msg), "duplicate component '%s' in archetype (a component type may appear only once)",
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
			return 0; /* pure-ish func */
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

/* True if the body is empty or contains only effect-free statements (lets with
 * pure initializers, returns, breaks). Stricter signal than "no side effects". */
static int body_is_effectively_empty(SemanticContext *ctx, ProcDecl *proc) {
	if (proc->statement_count == 0)
		return 1;
	for (int i = 0; i < proc->statement_count; i++) {
		Statement *s = proc->statements[i];
		if (!s)
			continue;
		switch (s->type) {
		case STMT_BIND:
			if (s->data.bind_stmt.value && expr_has_side_effects(ctx, s->data.bind_stmt.value, proc))
				return 0;
			break; /* let with pure init: still empty */
		case STMT_RETURN:
		case STMT_BREAK:
			break; /* control flow only */
		default:
			return 0;
		}
	}
	return 1;
}

/* Run the proc-could-be-func and proc-no-effect lints on a non-extern proc. */
static void lint_proc_decl(SemanticContext *ctx, ProcDecl *proc) {
	if (!proc || proc->is_extern || proc->allow_pure_proc)
		return;

	/* proc-no-effect is the stricter case; emit only that one if it applies. */
	if (body_is_effectively_empty(ctx, proc)) {
		if (g_lint_config.proc_no_effect_enabled) {
			char msg[256];
			snprintf(msg, sizeof(msg),
			         "proc '%s' has an empty or effect-free body; remove it or add the intended logic",
			         proc->name ? proc->name : "<unknown>");
			lint_emit(ctx, g_lint_config.proc_no_effect_werror, proc->loc, "proc-no-effect", msg);
		}
		return;
	}

	if (g_lint_config.proc_could_be_func_enabled &&
	    !body_has_side_effects(ctx, proc->statements, proc->statement_count, proc)) {
		char msg[320];
		snprintf(msg, sizeof(msg),
		         "proc '%s' has no detectable side effects; consider declaring it as 'func' "
		         "(suppress with @allow_pure_proc)",
		         proc->name ? proc->name : "<unknown>");
		lint_emit(ctx, g_lint_config.proc_could_be_func_werror, proc->loc, "proc-could-be-func", msg);
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
		if (proc->is_extern) {
			if (pt && pt->kind == TYPE_NAME) {
				const char *tname = pt->data.name;
				if (!is_primitive_type_name(tname) && !find_archetype(ctx, tname) && !is_type_alias(ctx, tname)) {
					char msg[256];
					snprintf(msg, sizeof(msg), "unknown type '%s' in extern proc '%s' signature", tname, proc->name);
					error(ctx, msg);
				}
			}
			/* 'consume' modifier only makes sense on opaque (resource) parameters. */
			if (p->is_consume && !is_consumable_ref(ctx, pt)) {
				char msg[256];
				snprintf(msg, sizeof(msg),
				         "'consume' may only modify a resource (opaque) parameter (proc '%s', param '%s')", proc->name,
				         p->name ? p->name : "?");
				error(ctx, msg);
			}
		} else {
			/* Non-extern procs: a plain `consume` close-fn is the canonical terminal sink
			 * for a linear opaque, so allow `consume` on any opaque-backed param. */
			if (p->is_consume && !is_consumable_ref(ctx, pt)) {
				char msg[256];
				snprintf(msg, sizeof(msg),
				         "'consume' may only modify a resource (opaque) parameter (proc '%s', param '%s')", proc->name,
				         p->name ? p->name : "?");
				error(ctx, msg);
			}
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
		mark_last_param(ctx);
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
		/* If no explicit type and we found the archetype, use the field's type */
		if (!param_type && arch_info) {
			FieldInfo *field = find_field(arch_info, sys->params[i]->name);
			if (field) {
				param_type = field->type;
			}
		}
		add_variable(ctx, sys->params[i]->name, param_type);
		mark_last_param(ctx);
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
		if (func->params[i]->type && func->params[i]->type->kind == TYPE_ARCHETYPE) {
			char msg[256];
			snprintf(msg, sizeof(msg), "func '%s': `archetype` parameter type is only allowed on procs, not funcs",
			         func->name);
			error(ctx, msg);
			break;
		}
	}
	if (func->return_type && func->return_type->kind == TYPE_ARCHETYPE) {
		char msg[256];
		snprintf(msg, sizeof(msg), "func '%s': `archetype` is only valid as a parameter type, not a return type",
		         func->name);
		error(ctx, msg);
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
			/* 'consume' modifier only makes sense on opaque (resource) parameters. */
			if (p->is_consume && !is_consumable_ref(ctx, pt)) {
				char msg[256];
				snprintf(msg, sizeof(msg),
				         "'consume' may only modify a resource (opaque) parameter (func '%s', param '%s')", func->name,
				         p->name ? p->name : "?");
				error(ctx, msg);
			}
		} else {
			/* Non-extern funcs: allow `consume` on any opaque-backed param (terminal sink). */
			if (p->is_consume && !is_consumable_ref(ctx, pt)) {
				char msg[256];
				snprintf(msg, sizeof(msg),
				         "'consume' may only modify a resource (opaque) parameter (func '%s', param '%s')", func->name,
				         p->name ? p->name : "?");
				error(ctx, msg);
			}
		}
	}

	/* Validate return type: extern funcs must use known types. */
	if (func->is_extern) {
		if (func->return_type && func->return_type->kind == TYPE_NAME) {
			const char *tname = func->return_type->data.name;
			if (!is_primitive_type_name(tname) && !is_type_alias(ctx, tname)) {
				char msg[256];
				snprintf(msg, sizeof(msg), "unknown return type '%s' in extern func '%s' signature", tname, func->name);
				error(ctx, msg);
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
		mark_last_param(ctx);
	}

	ctx->in_unsafe = func->is_unsafe;
	for (int i = 0; i < func->statement_count; i++) {
		analyze_statement(ctx, func->statements[i]);
	}
	ctx->in_unsafe = 0;

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
		erase_aliases_typeref(ctx, d->data.func->return_type);
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

static void erase_type_aliases(SemanticContext *ctx, Program *prog) {
	if (ctx->type_alias_count == 0)
		return;
	for (int i = 0; i < prog->decl_count; i++)
		erase_aliases_decl(ctx, prog->decls[i]);
}

/* ========== PUBLIC API ========== */

SemanticContext *semantic_analyze(Program *prog) {
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
	ctx->prog = prog;

	/* Register builtins */
	register_func(ctx, "write");
	register_func(ctx, "insert");
	register_func(ctx, "delete");
	register_func(ctx, "dealloc");

	if (!prog)
		return ctx;

	/* pass 0: collect constants and nominal type aliases. A `name :: <RHS>` decl is a
	 * value const when RHS is a literal, or a type alias when RHS is a bare name —
	 * consts are literal-only, so a name RHS is unambiguously a type (Odin's model). */
	for (int i = 0; i < prog->decl_count; i++) {
		if (prog->decls[i]->kind != DECL_CONST)
			continue;
		ConstDecl *c = prog->decls[i]->data.constant;

		/* Type alias: `count :: int`, `file :: opaque`. RHS is a bare name; the backing
		 * is validated after the loop (so aliases may reference later aliases). */
		/* Tuple type alias: `pos :: (x: int, y: int)` mints flat name-prefixed nominal
		 * types pos_x, pos_y (each a normal alias over the field's backing). */
		if (c->type_value && c->type_value->kind == TYPE_TUPLE) {
			for (int f = 0; f < c->type_value->data.tuple.field_count; f++) {
				TypeRef *ft = c->type_value->data.tuple.field_types[f];
				const char *fbacking = NULL;
				if (ft && ft->kind == TYPE_NAME)
					fbacking = ft->data.name;
				else if (ft && ft->kind == TYPE_OPAQUE)
					fbacking = "opaque";
				else {
					error(ctx, "tuple field must be a simple type (nested tuples are not allowed)");
					continue;
				}
				const char *fn = c->type_value->data.tuple.field_names[f];
				size_t L = strlen(c->name) + 1 + strlen(fn) + 1;
				char *aname = malloc(L);
				snprintf(aname, L, "%s_%s", c->name, fn);
				int dup = 0;
				for (int j = 0; j < ctx->type_alias_count; j++)
					if (strcmp(ctx->type_alias_names[j], aname) == 0) {
						dup = 1;
						break;
					}
				if (dup) {
					char msg[256];
					snprintf(msg, sizeof(msg), "type '%s' already defined", aname);
					error(ctx, msg);
					free(aname);
					continue;
				}
				ctx->type_alias_names = realloc(ctx->type_alias_names, (ctx->type_alias_count + 1) * sizeof(char *));
				ctx->type_alias_backings =
				    realloc(ctx->type_alias_backings, (ctx->type_alias_count + 1) * sizeof(char *));
				ctx->type_alias_names[ctx->type_alias_count] = aname;
				ctx->type_alias_backings[ctx->type_alias_count] = (char *)fbacking;
				ctx->type_alias_count++;
			}
			goto next_const;
		}

		if (c->value && c->value->type == EXPR_NAME) {
			/* Redefinition must AGREE: the same alias with the same backing is fine
			 * (e.g. core's `file :: opaque` and a user's `file :: opaque`); a different
			 * backing is a conflict. */
			for (int j = 0; j < ctx->type_alias_count; j++) {
				if (strcmp(ctx->type_alias_names[j], c->name) == 0) {
					if (strcmp(ctx->type_alias_backings[j], c->value->data.name.name) == 0)
						goto next_const; /* agrees — silently share */
					char msg[256];
					snprintf(msg, sizeof(msg), "type '%s' redefined with a different backing", c->name);
					error(ctx, msg);
					goto next_const;
				}
			}
			ctx->type_alias_names = realloc(ctx->type_alias_names, (ctx->type_alias_count + 1) * sizeof(char *));
			ctx->type_alias_backings =
			    realloc(ctx->type_alias_backings, (ctx->type_alias_count + 1) * sizeof(char *));
			ctx->type_alias_names[ctx->type_alias_count] = c->name;
			ctx->type_alias_backings[ctx->type_alias_count] = c->value->data.name.name;
			ctx->type_alias_count++;
			goto next_const;
		}

		/* Value const: RHS must be a literal. */
		if (!c->value || c->value->type != EXPR_LITERAL) {
			char msg[256];
			snprintf(msg, sizeof(msg), "constant '%s' must be a literal value", c->name);
			error(ctx, msg);
			goto next_const;
		}
		for (int j = 0; j < ctx->const_count; j++) {
			if (strcmp(ctx->const_names[j], c->name) == 0) {
				char msg[256];
				snprintf(msg, sizeof(msg), "constant '%s' already defined", c->name);
				error(ctx, msg);
				goto next_const;
			}
		}
		ctx->const_names = realloc(ctx->const_names, (ctx->const_count + 1) * sizeof(char *));
		ctx->const_values = realloc(ctx->const_values, (ctx->const_count + 1) * sizeof(const char *));
		ctx->const_names[ctx->const_count] = c->name;
		ctx->const_values[ctx->const_count] = c->value->data.literal.lexeme;
		ctx->const_count++;
	next_const:;
	}

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
			snprintf(msg, sizeof(msg), "type alias '%s' has unknown backing type '%s'", ctx->type_alias_names[i],
			         b);
			error(ctx, msg);
		}
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

	/* pass 3: erase nominal aliases to their backing (zero-cost; codegen never sees them). */
	erase_type_aliases(ctx, prog);

	return ctx;
}

void semantic_context_free(SemanticContext *ctx) {
	if (!ctx)
		return;

	/* free constants (names only, values are owned by AST) */
	free(ctx->const_names);
	free(ctx->const_values);

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

	free(ctx);
}

int semantic_has_errors(SemanticContext *ctx) {
	return ctx->error_count > 0;
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
