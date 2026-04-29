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
	const char *inferred_type; /* for variables without explicit type, stores inferred type name */
} VariableInfo;

typedef struct {
	VariableInfo **vars;
	int var_count;
} Scope;

struct SemanticContext {
	ArchetypeInfo **archetypes; /* one per unique shape */
	int archetype_count;

	AliasEntry **aliases; /* one per arche declaration */
	int alias_count;

	char **known_funcs;
	int known_func_count;

	Scope *scopes;
	int scope_count;

	int error_count;

	/* Track which archetype we're analyzing a sys for (NULL if not in sys) */
	const char *current_sys_archetype;

	/* Track if inside proc/sys body (for alloc enforcement) */
	int in_body;

	/* Program for looking up declarations */
	Program *prog;
};

/* ========== UTILITY FUNCTIONS ========== */

static char *compute_shape_signature(FieldDecl **fields, int field_count) {
	size_t sig_size = 256;
	char *sig = malloc(sig_size);
	sig[0] = '\0';
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
		}
		const char *kind_str = (f->kind == FIELD_META) ? "meta" : "col";
		char part[128];
		snprintf(part, sizeof(part), "%s:%s:%s;", f->name, type_name, kind_str);
		if (strlen(sig) + strlen(part) >= sig_size - 1) {
			sig_size = (strlen(sig) + strlen(part) + 1) * 2;
			sig = realloc(sig, sig_size);
		}
		strcat(sig, part);
	}
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
				return normalize_type_name(var->type->data.name);
			}
			/* Fallback to inferred type */
			if (var->inferred_type) {
				return normalize_type_name(var->inferred_type);
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
						return normalize_type_name(ft->data.name);
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
		/* Look up function to determine return type */
		const char *func_name = NULL;
		if (expr->data.call.callee && expr->data.call.callee->type == EXPR_NAME) {
			func_name = expr->data.call.callee->data.name.name;
		}

		if (!func_name || !ctx->prog)
			return NULL;

		/* Check if it's a func with explicit return type */
		for (int i = 0; i < ctx->prog->decl_count; i++) {
			Decl *decl = ctx->prog->decls[i];
			if (decl->kind == DECL_FUNC && strcmp(decl->data.func->name, func_name) == 0) {
				if (decl->data.func->return_type) {
					return normalize_type_name(decl->data.func->return_type->data.name);
				}
				return NULL;
			}
		}

		/* Procs don't return values */
		return NULL;
	}

	case EXPR_ALLOC: {
		/* Type is the archetype being allocated */
		return expr->data.alloc.archetype_name;
	}

	default:
		return NULL;
	}
}

/* ========== EXPRESSION ANALYSIS ========== */

static void analyze_expression(SemanticContext *ctx, Expression *expr) {
	if (!expr)
		return;

	switch (expr->type) {
	case EXPR_LITERAL:
		/* literals are always valid */
		break;

	case EXPR_NAME: {
		const char *name = expr->data.name.name;

		/* Check if it's a known function, variable, or archetype */
		int is_known_func = find_known_func(ctx, name);
		int is_var = find_variable(ctx, name) != NULL;
		int is_arch = find_archetype(ctx, name) != NULL;

		if (!is_known_func && !is_var && !is_arch) {
			char msg[256];
			snprintf(msg, sizeof(msg), "Undefined symbol '%s'", name);
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
		break;

	case EXPR_CALL:
		analyze_expression(ctx, expr->data.call.callee);
		for (int i = 0; i < expr->data.call.arg_count; i++) {
			analyze_expression(ctx, expr->data.call.args[i]);
		}
		break;

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
	case STMT_LET: {
		analyze_expression(ctx, stmt->data.let_stmt.value);

		/* Multi-value let: add all variables from names array */
		if (stmt->data.let_stmt.name_count > 0 && stmt->data.let_stmt.names) {
			for (int i = 0; i < stmt->data.let_stmt.name_count; i++) {
				const char *var_name = stmt->data.let_stmt.names[i];
				if (var_name && strcmp(var_name, "_") != 0) {
					/* Add variable (no type annotation for multi-value let) */
					add_variable(ctx, var_name, NULL);

					/* Try to infer type from expression if it's callable with multiple returns */
					if (stmt->data.let_stmt.value && i < 10) { /* arbitrary limit */
						/* For now, just skip type inference for multi-value let */
						/* This would require analyzing the function's return signature */
					}
				}
			}
		} else if (stmt->data.let_stmt.name) {
			/* Single-value let */
			/* Check if value is an alloc expression */
			const char *archetype_name = NULL;
			if (stmt->data.let_stmt.value && stmt->data.let_stmt.value->type == EXPR_ALLOC) {
				archetype_name = stmt->data.let_stmt.value->data.alloc.archetype_name;
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
				add_variable_with_archetype(ctx, stmt->data.let_stmt.name, stmt->data.let_stmt.type, archetype_name);
			} else {
				add_variable(ctx, stmt->data.let_stmt.name, stmt->data.let_stmt.type);
			}

			/* Handle type annotations and type inference */
			if (ctx->scope_count > 0) {
				Scope *scope = &ctx->scopes[ctx->scope_count - 1];
				if (scope->var_count > 0) {
					var = scope->vars[scope->var_count - 1];
					if (stmt->data.let_stmt.type) {
						/* Type annotation: convert TypeRef to string type name */
						var->inferred_type = stmt->data.let_stmt.type->data.name;
					} else if (stmt->data.let_stmt.value) {
						/* No annotation: infer from value expression */
						const char *inferred = resolve_expression_type(ctx, stmt->data.let_stmt.value);
						if (inferred) {
							var->inferred_type = inferred;
						}
					}
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
		/* Check for infinite or condition-based for loop */
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
		break;
	}

	case STMT_RUN:
		/* no world validation needed - worlds are planned but not yet implemented */
		break;

	case STMT_EXPR:
		analyze_expression(ctx, stmt->data.expr_stmt.expr);
		break;

	case STMT_FREE:
		analyze_expression(ctx, stmt->data.free_stmt.value);
		break;
	}
}

/* ========== DECLARATION ANALYSIS ========== */

static void analyze_archetype_decl(SemanticContext *ctx, ArchetypeDecl *arch) {
	if (!arch)
		return;

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

	/* Also expand tuples in the AST's field list so codegen sees expanded fields */
	int expanded_count = 0;
	for (int i = 0; i < arch->field_count; i++) {
		if (arch->fields[i]->type->kind == TYPE_TUPLE) {
			expanded_count += arch->fields[i]->type->data.tuple.field_count;
		} else {
			expanded_count++;
		}
	}

	if (expanded_count != arch->field_count) {
		/* Need to expand — reallocate arch->fields */
		FieldDecl **new_fields = malloc(expanded_count * sizeof(FieldDecl *));
		int field_idx = 0;
		for (int i = 0; i < arch->field_count; i++) {
			FieldDecl *field = arch->fields[i];
			if (field->type->kind == TYPE_TUPLE) {
				/* Create expanded field declarations */
				for (int j = 0; j < field->type->data.tuple.field_count; j++) {
					FieldDecl *expanded_field = malloc(sizeof(FieldDecl));
					char expanded_name[512];
					snprintf(expanded_name, sizeof(expanded_name), "%s_%s", field->name,
					         field->type->data.tuple.field_names[j]);
					expanded_field->name = malloc(strlen(expanded_name) + 1);
					strcpy(expanded_field->name, expanded_name);
					expanded_field->type = field->type->data.tuple.field_types[j];
					expanded_field->kind = field->kind;
					new_fields[field_idx++] = expanded_field;
				}
				free(field->name);
				free(field);
			} else {
				new_fields[field_idx++] = field;
			}
		}
		free(arch->fields);
		arch->fields = new_fields;
		arch->field_count = expanded_count;
	}

	/* Register alias */
	AliasEntry *entry = malloc(sizeof(AliasEntry));
	entry->name = malloc(strlen(arch->name) + 1);
	strcpy(entry->name, arch->name);
	entry->archetype = shape;
	ctx->aliases = realloc(ctx->aliases, (ctx->alias_count + 1) * sizeof(AliasEntry *));
	ctx->aliases[ctx->alias_count++] = entry;
}

static void analyze_alloc_decl(SemanticContext *ctx, AllocDecl *alloc) {
	if (!alloc)
		return;

	/* Validate archetype exists */
	ArchetypeInfo *arch = find_archetype(ctx, alloc->archetype_name);
	if (!arch) {
		fprintf(stderr, "Error: unknown archetype '%s' in alloc\n", alloc->archetype_name);
		ctx->error_count++;
		return;
	}

	/* Validate count is provided and is a literal */
	if (alloc->field_count == 0 || !alloc->field_values[0]) {
		fprintf(stderr, "Error: alloc missing count expression\n");
		ctx->error_count++;
		return;
	}

	Expression *count_expr = alloc->field_values[0];
	if (count_expr->type != EXPR_LITERAL) {
		fprintf(stderr, "Error: alloc count must be a literal; dynamic counts not yet supported\n");
		ctx->error_count++;
		return;
	}

	/* Analyze field initialization expressions */
	for (int i = 1; i < alloc->field_count; i++) {
		analyze_expression(ctx, alloc->field_values[i]);
	}
}

static void analyze_proc_decl(SemanticContext *ctx, ProcDecl *proc) {
	if (!proc)
		return;

	/* Register proc name as a known function */
	register_func(ctx, proc->name);

	/* For extern procs, no body to analyze */
	if (proc->is_extern) {
		return;
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
	}

	ctx->in_body = 1;
	for (int i = 0; i < proc->statement_count; i++) {
		analyze_statement(ctx, proc->statements[i]);
	}
	ctx->in_body = 0;

	pop_scope(ctx);
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

static void analyze_func_decl(SemanticContext *ctx, FuncDecl *func) {
	if (!func)
		return;

	/* Register func name as a known function */
	register_func(ctx, func->name);

	/* For extern funcs, no body to analyze */
	if (func->is_extern) {
		return;
	}

	push_scope(ctx);

	/* add parameters as variables */
	for (int i = 0; i < func->param_count; i++) {
		add_variable(ctx, func->params[i]->name, func->params[i]->type);
	}

	for (int i = 0; i < func->statement_count; i++) {
		analyze_statement(ctx, func->statements[i]);
	}

	pop_scope(ctx);
}

static void analyze_decl(SemanticContext *ctx, Decl *decl) {
	if (!decl)
		return;

	switch (decl->kind) {
	case DECL_ARCHETYPE:
		analyze_archetype_decl(ctx, decl->data.archetype);
		break;
	case DECL_ALLOC:
		analyze_alloc_decl(ctx, decl->data.alloc);
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
	}
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
	ctx->scopes = NULL;
	ctx->scope_count = 0;
	ctx->error_count = 0;
	ctx->current_sys_archetype = NULL;
	ctx->in_body = 0;
	ctx->prog = prog;

	/* Register builtins */
	register_func(ctx, "write");
	register_func(ctx, "insert");
	register_func(ctx, "delete");
	register_func(ctx, "dealloc");

	if (!prog)
		return ctx;

	/* first pass: collect all archetypes */
	for (int i = 0; i < prog->decl_count; i++) {
		if (prog->decls[i]->kind == DECL_ARCHETYPE) {
			analyze_decl(ctx, prog->decls[i]);
		}
	}

	/* second pass: analyze other declarations */
	for (int i = 0; i < prog->decl_count; i++) {
		if (prog->decls[i]->kind != DECL_ARCHETYPE) {
			analyze_decl(ctx, prog->decls[i]);
		}
	}

	return ctx;
}

void semantic_context_free(SemanticContext *ctx) {
	if (!ctx)
		return;

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
