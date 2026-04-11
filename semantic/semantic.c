#include "semantic.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ========== DATA STRUCTURES ========== */

typedef struct {
	char *name;
	TypeRef *type;
	FieldKind kind;
} FieldInfo;

typedef struct {
	char *name;
	FieldInfo **fields;
	int field_count;
} ArchetypeInfo;

typedef struct {
	char *name;
} WorldInfo;

typedef struct {
	char *name;
	TypeRef *type;
	char *archetype_name;  /* for variables that refer to archetype entries */
} VariableInfo;

typedef struct {
	VariableInfo **vars;
	int var_count;
} Scope;

struct SemanticContext {
	WorldInfo **worlds;
	int world_count;

	ArchetypeInfo **archetypes;
	int archetype_count;

	Scope *scopes;
	int scope_count;

	int error_count;
};

/* ========== UTILITY FUNCTIONS ========== */

static WorldInfo *find_world(SemanticContext *ctx, const char *name) {
	for (int i = 0; i < ctx->world_count; i++) {
		if (strcmp(ctx->worlds[i]->name, name) == 0) {
			return ctx->worlds[i];
		}
	}
	return NULL;
}

static ArchetypeInfo *find_archetype(SemanticContext *ctx, const char *name) {
	for (int i = 0; i < ctx->archetype_count; i++) {
		if (strcmp(ctx->archetypes[i]->name, name) == 0) {
			return ctx->archetypes[i];
		}
	}
	return NULL;
}

static FieldInfo *find_field(ArchetypeInfo *arch, const char *name) {
	if (!arch) return NULL;
	for (int i = 0; i < arch->field_count; i++) {
		if (strcmp(arch->fields[i]->name, name) == 0) {
			return arch->fields[i];
		}
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

static void add_variable_with_archetype(SemanticContext *ctx, const char *name, TypeRef *type, const char *archetype_name);

static void add_variable(SemanticContext *ctx, const char *name, TypeRef *type) {
	add_variable_with_archetype(ctx, name, type, NULL);
}

static void add_variable_with_archetype(SemanticContext *ctx, const char *name, TypeRef *type, const char *archetype_name) {
	if (ctx->scope_count == 0) return;

	Scope *scope = &ctx->scopes[ctx->scope_count - 1];
	VariableInfo *var = malloc(sizeof(VariableInfo));
	var->name = malloc(strlen(name) + 1);
	strcpy(var->name, name);
	var->type = type;
	var->archetype_name = archetype_name ? malloc(strlen(archetype_name) + 1) : NULL;
	if (var->archetype_name) strcpy(var->archetype_name, archetype_name);

	scope->vars = realloc(scope->vars, (scope->var_count + 1) * sizeof(VariableInfo *));
	scope->vars[scope->var_count++] = var;
}

/* ========== FORWARD DECLARATIONS ========== */

static void analyze_expression(SemanticContext *ctx, Expression *expr);
static void analyze_statement(SemanticContext *ctx, Statement *stmt);

/* ========== EXPRESSION ANALYSIS ========== */

static void analyze_expression(SemanticContext *ctx, Expression *expr) {
	if (!expr) return;

	switch (expr->type) {
	case EXPR_LITERAL:
		/* literals are always valid */
		break;

	case EXPR_NAME: {
		const char *name = expr->data.name.name;
		if (!find_variable(ctx, name)) {
			/* check if it's an archetype name (for iteration) */
			if (!find_archetype(ctx, name)) {
				char msg[256];
				snprintf(msg, sizeof(msg), "Undefined variable or archetype '%s'", name);
				error(ctx, msg);
			}
		}
		break;
	}

	case EXPR_FIELD: {
		/* expr.field - need to know what expr resolves to */
		analyze_expression(ctx, expr->data.field.base);

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
				if (!find_field(arch, field_name)) {
					char msg[256];
					snprintf(msg, sizeof(msg), "Archetype '%s' has no field '%s'",
						arch->name, field_name);
					error(ctx, msg);
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

	case EXPR_ALLOC:
		/* check archetype exists */
		if (!find_archetype(ctx, expr->data.alloc.archetype_name)) {
			char msg[256];
			snprintf(msg, sizeof(msg), "Undefined archetype '%s'",
				expr->data.alloc.archetype_name);
			error(ctx, msg);
		}
		for (int i = 0; i < expr->data.alloc.field_count; i++) {
			analyze_expression(ctx, expr->data.alloc.field_values[i]);
		}
		break;
	}
}

/* ========== STATEMENT ANALYSIS ========== */

static void analyze_statement(SemanticContext *ctx, Statement *stmt) {
	if (!stmt) return;

	switch (stmt->type) {
	case STMT_LET: {
		analyze_expression(ctx, stmt->data.let_stmt.value);
		/* create local variable */
		add_variable(ctx, stmt->data.let_stmt.name, stmt->data.let_stmt.type);
		break;
	}

	case STMT_ASSIGN:
		analyze_expression(ctx, stmt->data.assign_stmt.target);
		analyze_expression(ctx, stmt->data.assign_stmt.value);
		break;

	case STMT_FOR: {
		/* check iterable exists (should be archetype) */
		analyze_expression(ctx, stmt->data.for_stmt.iterable);

		/* push new scope for loop body */
		push_scope(ctx);

		/* determine what archetype the loop iterates over */
		const char *archetype_name = NULL;
		if (stmt->data.for_stmt.iterable->type == EXPR_NAME) {
			archetype_name = stmt->data.for_stmt.iterable->data.name.name;
			if (!find_archetype(ctx, archetype_name)) {
				char msg[256];
				snprintf(msg, sizeof(msg), "For loop iterates over undefined archetype '%s'", archetype_name);
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

	case STMT_RUN:
		/* check world exists */
		if (!find_world(ctx, stmt->data.run_stmt.world_name)) {
			char msg[256];
			snprintf(msg, sizeof(msg), "Undefined world '%s'", stmt->data.run_stmt.world_name);
			error(ctx, msg);
		}
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

static void analyze_world_decl(SemanticContext *ctx, WorldDecl *world) {
	if (!world) return;

	WorldInfo *info = malloc(sizeof(WorldInfo));
	info->name = malloc(strlen(world->name) + 1);
	strcpy(info->name, world->name);

	ctx->worlds = realloc(ctx->worlds, (ctx->world_count + 1) * sizeof(WorldInfo *));
	ctx->worlds[ctx->world_count++] = info;
}

static void analyze_archetype_decl(SemanticContext *ctx, ArchetypeDecl *arch) {
	if (!arch) return;

	/* validate world exists */
	if (!find_world(ctx, arch->world_name)) {
		char msg[256];
		snprintf(msg, sizeof(msg), "Archetype '%s' references undefined world '%s'",
			arch->name, arch->world_name);
		error(ctx, msg);
	}

	ArchetypeInfo *info = malloc(sizeof(ArchetypeInfo));
	info->name = malloc(strlen(arch->name) + 1);
	strcpy(info->name, arch->name);
	info->fields = malloc(arch->field_count * sizeof(FieldInfo *));
	info->field_count = arch->field_count;

	for (int i = 0; i < arch->field_count; i++) {
		FieldDecl *field = arch->fields[i];
		FieldInfo *field_info = malloc(sizeof(FieldInfo));
		field_info->name = malloc(strlen(field->name) + 1);
		strcpy(field_info->name, field->name);
		field_info->type = field->type;
		field_info->kind = field->kind;
		info->fields[i] = field_info;
	}

	ctx->archetypes = realloc(ctx->archetypes, (ctx->archetype_count + 1) * sizeof(ArchetypeInfo *));
	ctx->archetypes[ctx->archetype_count++] = info;
}

static void analyze_proc_decl(SemanticContext *ctx, ProcDecl *proc) {
	if (!proc) return;

	push_scope(ctx);

	for (int i = 0; i < proc->statement_count; i++) {
		analyze_statement(ctx, proc->statements[i]);
	}

	pop_scope(ctx);
}

static void analyze_sys_decl(SemanticContext *ctx, SysDecl *sys) {
	if (!sys) return;

	push_scope(ctx);

	/* add parameters as variables */
	for (int i = 0; i < sys->param_count; i++) {
		add_variable(ctx, sys->params[i]->name, sys->params[i]->type);
	}

	for (int i = 0; i < sys->statement_count; i++) {
		analyze_statement(ctx, sys->statements[i]);
	}

	pop_scope(ctx);
}

static void analyze_func_decl(SemanticContext *ctx, FuncDecl *func) {
	if (!func) return;

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
	if (!decl) return;

	switch (decl->kind) {
	case DECL_WORLD:
		analyze_world_decl(ctx, decl->data.world);
		break;
	case DECL_ARCHETYPE:
		analyze_archetype_decl(ctx, decl->data.archetype);
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
	ctx->worlds = NULL;
	ctx->world_count = 0;
	ctx->archetypes = NULL;
	ctx->archetype_count = 0;
	ctx->scopes = NULL;
	ctx->scope_count = 0;
	ctx->error_count = 0;

	if (!prog) return ctx;

	/* first pass: collect all worlds */
	for (int i = 0; i < prog->decl_count; i++) {
		if (prog->decls[i]->kind == DECL_WORLD) {
			analyze_decl(ctx, prog->decls[i]);
		}
	}

	/* second pass: collect all archetypes */
	for (int i = 0; i < prog->decl_count; i++) {
		if (prog->decls[i]->kind == DECL_ARCHETYPE) {
			analyze_decl(ctx, prog->decls[i]);
		}
	}

	/* third pass: analyze other declarations */
	for (int i = 0; i < prog->decl_count; i++) {
		if (prog->decls[i]->kind != DECL_WORLD && prog->decls[i]->kind != DECL_ARCHETYPE) {
			analyze_decl(ctx, prog->decls[i]);
		}
	}

	return ctx;
}

void semantic_context_free(SemanticContext *ctx) {
	if (!ctx) return;

	/* free worlds */
	for (int i = 0; i < ctx->world_count; i++) {
		free(ctx->worlds[i]->name);
		free(ctx->worlds[i]);
	}
	free(ctx->worlds);

	/* free archetypes (but not the TypeRef, which is owned by AST) */
	for (int i = 0; i < ctx->archetype_count; i++) {
		ArchetypeInfo *arch = ctx->archetypes[i];
		free(arch->name);
		for (int j = 0; j < arch->field_count; j++) {
			free(arch->fields[j]->name);
			/* don't free arch->fields[j]->type - owned by AST */
			free(arch->fields[j]);
		}
		free(arch->fields);
		free(arch);
	}
	free(ctx->archetypes);

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
	if (!field || !field->type) return NULL;
	if (field->type->kind != TYPE_NAME) return NULL;
	return field->type->data.name;
}
