#include "codegen.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ========== DATA STRUCTURES ========== */

typedef struct {
	char *name;
	char *llvm_name;        /* allocated SSA value name */
	int type;               /* 0=i32, 1=i32*, 2=i8* (string), 3=arch*, 4=column ptr, 5=%struct.arche_array* */
	char *arch_name;        /* for type==3 or 4, nullable otherwise */
	int string_len;         /* for type==2 (string), the compile-time length (-1 if unknown) */
	const char *field_type; /* for type==4 (column ptr), the Arche type name (e.g. "float") */
	char *handle_archetype; /* if field_type=="handle", the target archetype name */
	int bit_width;          /* 32 (default) or 64 for SSA values */
} ValueInfo;

typedef struct {
	ValueInfo **values;
	int value_count;
} ValueScope;

typedef struct {
	char sys_name[256];
	char arch_name[256];
	char versioned_name[512];
} SysVersion;

struct CodegenContext {
	Program *prog;
	SemanticContext *sem_ctx;

	/* For tracking allocated values */
	ValueScope *scopes;
	int scope_count;

	/* SSA counter for generating unique names */
	int value_counter;
	int string_counter;

	/* Buffered output */
	char *output_buffer;
	size_t buffer_size;
	size_t buffer_pos;

	/* Global constants buffer (strings, etc.) */
	char *globals_buffer;
	size_t globals_size;
	size_t globals_pos;

	/* SIMD vectorization context */
	int vector_lanes; /* 0 = scalar mode, 4 = AVX2 double (256-bit / 64-bit = 4 lanes) */
	int in_sys;       /* 1 when generating inside a sys function body */

	/* Implicit loop context */
	char implicit_loop_index[64]; /* SSA reg name for current implicit loop ("" = not in loop) */

	/* Loop exit label stack for break statements */
	char **loop_exit_labels;
	int loop_exit_count;
	int loop_exit_capacity;

	/* System function version mapping: (sys_name, arch_name) -> versioned_name */
	SysVersion *sys_versions;
	int sys_version_count;
	int sys_version_capacity;

	/* Top-level allocations to initialize in main() */
	StaticDecl **top_level_allocs;
	int alloc_count;
	int alloc_capacity;

	/* Alloca hoisting: collect allocas during function body gen, emit at entry */
	char *alloca_buffer;
	size_t alloca_buf_size;
	size_t alloca_buf_pos;
	int hoisting_allocas;
};

/* ========== SYSTEM VERSION MAPPING ========== */

static void codegen_register_sys_version(CodegenContext *ctx, const char *sys_name, const char *arch_name) {
	if (ctx->sys_version_count >= ctx->sys_version_capacity) {
		ctx->sys_version_capacity = (ctx->sys_version_capacity == 0) ? 16 : ctx->sys_version_capacity * 2;
		ctx->sys_versions = realloc(ctx->sys_versions, ctx->sys_version_capacity * sizeof(ctx->sys_versions[0]));
	}

	SysVersion *entry = &ctx->sys_versions[ctx->sys_version_count];
	strncpy(entry->sys_name, sys_name, sizeof(entry->sys_name) - 1);
	entry->sys_name[sizeof(entry->sys_name) - 1] = '\0';
	strncpy(entry->arch_name, arch_name, sizeof(entry->arch_name) - 1);
	entry->arch_name[sizeof(entry->arch_name) - 1] = '\0';
	snprintf(entry->versioned_name, sizeof(entry->versioned_name), "%s_%s", sys_name, arch_name);
	ctx->sys_version_count++;
}

static const char *codegen_get_sys_version(CodegenContext *ctx, const char *sys_name, const char *arch_name) {
	for (int i = 0; i < ctx->sys_version_count; i++) {
		if (strcmp(ctx->sys_versions[i].sys_name, sys_name) == 0 &&
		    strcmp(ctx->sys_versions[i].arch_name, arch_name) == 0) {
			return ctx->sys_versions[i].versioned_name;
		}
	}
	return NULL;
}

/* ========== UTILITY FUNCTIONS ========== */

static void buffer_append(CodegenContext *ctx, const char *str) {
	size_t len = strlen(str);
	if (ctx->buffer_pos + len >= ctx->buffer_size) {
		ctx->buffer_size = (ctx->buffer_size + len) * 2;
		ctx->output_buffer = realloc(ctx->output_buffer, ctx->buffer_size);
	}
	strcpy(ctx->output_buffer + ctx->buffer_pos, str);
	ctx->buffer_pos += len;
}

static void buffer_append_fmt(CodegenContext *ctx, const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);

	char temp[1024];
	vsnprintf(temp, sizeof(temp), fmt, args);
	va_end(args);

	buffer_append(ctx, temp);
}

static void emit_alloca(CodegenContext *ctx, const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	char temp[256];
	vsnprintf(temp, sizeof(temp), fmt, args);
	va_end(args);

	if (ctx->hoisting_allocas) {
		size_t len = strlen(temp);
		if (ctx->alloca_buf_pos + len >= ctx->alloca_buf_size) {
			ctx->alloca_buf_size = (ctx->alloca_buf_size + len + 64) * 2;
			ctx->alloca_buffer = realloc(ctx->alloca_buffer, ctx->alloca_buf_size);
		}
		strcpy(ctx->alloca_buffer + ctx->alloca_buf_pos, temp);
		ctx->alloca_buf_pos += len;
	} else {
		buffer_append(ctx, temp);
	}
}

static const char *llvm_type_from_arche(const char *arche_type) {
	if (!arche_type)
		return "i32"; /* default to int */

	if (strcmp(arche_type, "float") == 0)
		return "double";
	if (strcmp(arche_type, "int") == 0)
		return "i32";
	if (strcmp(arche_type, "char") == 0)
		return "i8";
	if (strcmp(arche_type, "void") == 0)
		return "void";
	if (strcmp(arche_type, "handle") == 0)
		return "i64";

	/* For custom types (Vec3, archetypes, etc.), use opaque structures */
	static char buf[256];
	snprintf(buf, sizeof(buf), "%%struct.%s", arche_type);
	return buf;
}

static const char *llvm_vector_type(const char *scalar_type, int lanes) {
	static char buf[64];
	snprintf(buf, sizeof(buf), "<%d x %s>", lanes, scalar_type);
	return buf;
}

static const char *elem_llvm_type(CodegenContext *ctx, const char *arche_type) {
	const char *scalar = llvm_type_from_arche(arche_type);
	if (ctx->vector_lanes > 0)
		return llvm_vector_type(scalar, ctx->vector_lanes);
	return scalar;
}

static const char *field_base_type_name(TypeRef *type) {
	while (type->kind == TYPE_SHAPED_ARRAY)
		type = type->data.shaped_array.element_type;
	if (type->kind == TYPE_HANDLE)
		return "handle";
	return type->data.name;
}

static int field_total_elements(TypeRef *type) {
	if (type->kind == TYPE_SHAPED_ARRAY)
		return type->data.shaped_array.rank * field_total_elements(type->data.shaped_array.element_type);
	return 1;
}

static char *gen_value_name(CodegenContext *ctx) {
	char *name = malloc(32);
	snprintf(name, 32, "%%v%d", ctx->value_counter++);
	return name;
}

/* Emit a string constant global and return its name */
static char *emit_string_global(CodegenContext *ctx, const char *quoted_str) {
	char global_name[64];
	snprintf(global_name, sizeof(global_name), "@.str%d", ctx->string_counter++);

	/* quoted_str is "..." with quotes. Process escape sequences and build LLVM constant */
	char escaped[2048];
	size_t escaped_pos = 0;
	size_t str_len = strlen(quoted_str);

	for (size_t i = 1; i < str_len - 1 && escaped_pos < sizeof(escaped) - 10; i++) {
		char c = quoted_str[i];
		if (c == '\\' && i + 1 < str_len - 1) {
			i++;
			switch (quoted_str[i]) {
			case 'n':
				escaped[escaped_pos++] = '\n';
				break;
			case 't':
				escaped[escaped_pos++] = '\t';
				break;
			case 'r':
				escaped[escaped_pos++] = '\r';
				break;
			case '\\':
				escaped[escaped_pos++] = '\\';
				break;
			case '"':
				escaped[escaped_pos++] = '"';
				break;
			default:
				escaped[escaped_pos++] = quoted_str[i];
				break;
			}
		} else {
			escaped[escaped_pos++] = c;
		}
	}

	/* Build LLVM global constant declaration */
	char global_decl[4096];
	char llvm_escaped[2048] = "";
	size_t llvm_pos = 0;

	for (size_t i = 0; i < escaped_pos && llvm_pos < sizeof(llvm_escaped) - 10; i++) {
		unsigned char c = (unsigned char)escaped[i];
		if (c >= 32 && c < 127 && c != '"' && c != '\\') {
			llvm_escaped[llvm_pos++] = c;
		} else {
			llvm_pos += snprintf(llvm_escaped + llvm_pos, sizeof(llvm_escaped) - llvm_pos, "\\%02X", c);
		}
	}

	snprintf(global_decl, sizeof(global_decl), "%s = private unnamed_addr constant [%zu x i8] c\"%s\\00\"\n",
	         global_name, escaped_pos + 1, llvm_escaped);

	/* Append to globals buffer */
	size_t decl_len = strlen(global_decl);
	if (ctx->globals_pos + decl_len >= ctx->globals_size) {
		ctx->globals_size = (ctx->globals_size + decl_len) * 2;
		ctx->globals_buffer = realloc(ctx->globals_buffer, ctx->globals_size);
	}
	strcpy(ctx->globals_buffer + ctx->globals_pos, global_decl);
	ctx->globals_pos += decl_len;

	/* Return allocated name */
	char *ret = malloc(64);
	strcpy(ret, global_name);
	return ret;
}

static void push_value_scope(CodegenContext *ctx) {
	ctx->scopes = realloc(ctx->scopes, (ctx->scope_count + 1) * sizeof(ValueScope));
	ctx->scopes[ctx->scope_count].values = NULL;
	ctx->scopes[ctx->scope_count].value_count = 0;
	ctx->scope_count++;
}

static void pop_value_scope(CodegenContext *ctx) {
	if (ctx->scope_count > 0) {
		ValueScope *scope = &ctx->scopes[ctx->scope_count - 1];
		for (int i = 0; i < scope->value_count; i++) {
			free(scope->values[i]->name);
			free(scope->values[i]->llvm_name);
			free(scope->values[i]->arch_name);
			free(scope->values[i]);
		}
		free(scope->values);
		ctx->scope_count--;
	}
}

static ValueInfo *find_value(CodegenContext *ctx, const char *name) {
	for (int i = ctx->scope_count - 1; i >= 0; i--) {
		ValueScope *scope = &ctx->scopes[i];
		for (int j = scope->value_count - 1; j >= 0; j--) {
			if (strcmp(scope->values[j]->name, name) == 0) {
				return scope->values[j];
			}
		}
	}
	return NULL;
}

static void add_value(CodegenContext *ctx, const char *name, const char *llvm_name, int type) {
	if (ctx->scope_count == 0)
		return;

	ValueScope *scope = &ctx->scopes[ctx->scope_count - 1];
	ValueInfo *val = malloc(sizeof(ValueInfo));
	val->name = malloc(strlen(name) + 1);
	strcpy(val->name, name);
	val->llvm_name = malloc(strlen(llvm_name) + 1);
	strcpy(val->llvm_name, llvm_name);
	val->type = type;
	val->arch_name = NULL;
	val->string_len = -1;
	val->field_type = NULL;
	val->bit_width = 32;

	scope->values = realloc(scope->values, (scope->value_count + 1) * sizeof(ValueInfo *));
	scope->values[scope->value_count++] = val;
}

static void add_array_value(CodegenContext *ctx, const char *name, const char *llvm_name) {
	if (ctx->scope_count == 0)
		return;

	ValueScope *scope = &ctx->scopes[ctx->scope_count - 1];
	ValueInfo *val = malloc(sizeof(ValueInfo));
	val->name = malloc(strlen(name) + 1);
	strcpy(val->name, name);
	val->llvm_name = malloc(strlen(llvm_name) + 1);
	strcpy(val->llvm_name, llvm_name);
	val->type = 5; /* %struct.arche_array* */
	val->arch_name = NULL;
	val->string_len = -1;
	val->field_type = NULL;
	val->bit_width = 32;

	scope->values = realloc(scope->values, (scope->value_count + 1) * sizeof(ValueInfo *));
	scope->values[scope->value_count++] = val;
}

static void add_arch_value(CodegenContext *ctx, const char *name, const char *llvm_name, const char *arch_name) {
	if (ctx->scope_count == 0)
		return;

	ValueScope *scope = &ctx->scopes[ctx->scope_count - 1];
	ValueInfo *val = malloc(sizeof(ValueInfo));
	val->name = malloc(strlen(name) + 1);
	strcpy(val->name, name);
	val->llvm_name = malloc(strlen(llvm_name) + 1);
	strcpy(val->llvm_name, llvm_name);
	val->type = 3; /* arch pointer */
	val->arch_name = malloc(strlen(arch_name) + 1);
	strcpy(val->arch_name, arch_name);
	val->string_len = -1;
	val->field_type = NULL;
	val->bit_width = 32;

	scope->values = realloc(scope->values, (scope->value_count + 1) * sizeof(ValueInfo *));
	scope->values[scope->value_count++] = val;
}

/* Helper: find archetype declaration by name */
static ArchetypeDecl *find_archetype_decl(CodegenContext *ctx, const char *name) {
	for (int i = 0; i < ctx->prog->decl_count; i++) {
		Decl *decl = ctx->prog->decls[i];
		if (decl->kind == DECL_ARCHETYPE && strcmp(decl->data.archetype->name, name) == 0) {
			return decl->data.archetype;
		}
	}
	return NULL;
}

/* Return compile-time capacity for arch from static declaration, or 0 if not declared static */
static int get_arch_static_capacity(CodegenContext *ctx, const char *arch_name) {
	for (int i = 0; i < ctx->prog->decl_count; i++) {
		if (ctx->prog->decls[i]->kind == DECL_STATIC) {
			StaticDecl *s = ctx->prog->decls[i]->data.alloc;
			if (strcmp(s->archetype_name, arch_name) == 0 && s->field_count > 0 &&
			    s->field_values[0]->type == EXPR_LITERAL) {
				return atoi(s->field_values[0]->data.literal.lexeme);
			}
		}
	}
	return 0;
}

static SysDecl *find_sys_decl(CodegenContext *ctx, const char *name) {
	for (int i = 0; i < ctx->prog->decl_count; i++) {
		Decl *decl = ctx->prog->decls[i];
		if (decl->kind == DECL_SYS && strcmp(decl->data.sys->name, name) == 0) {
			return decl->data.sys;
		}
	}
	return NULL;
}

static ProcDecl *find_proc_decl(CodegenContext *ctx, const char *name) {
	for (int i = 0; i < ctx->prog->decl_count; i++) {
		Decl *decl = ctx->prog->decls[i];
		if (decl->kind == DECL_PROC && strcmp(decl->data.proc->name, name) == 0)
			return decl->data.proc;
	}
	return NULL;
}

static FuncDecl *find_func_decl(CodegenContext *ctx, const char *name) {
	for (int i = 0; i < ctx->prog->decl_count; i++) {
		Decl *decl = ctx->prog->decls[i];
		if (decl->kind == DECL_FUNC && strcmp(decl->data.func->name, name) == 0)
			return decl->data.func;
	}
	return NULL;
}

static const char *get_shaped_field_info(CodegenContext *ctx, Expression *field_expr, int *out_rank) {
	if (field_expr->type != EXPR_FIELD || field_expr->data.field.base->type != EXPR_NAME)
		return NULL;
	const char *vn = field_expr->data.field.base->data.name.name;
	const char *fn = field_expr->data.field.field_name;
	const char *arch_name = NULL;
	ValueInfo *vi = find_value(ctx, vn);
	if (vi && vi->arch_name) {
		arch_name = vi->arch_name;
	} else if (find_archetype_decl(ctx, vn)) {
		arch_name = vn;
	}
	if (!arch_name)
		return NULL;
	ArchetypeDecl *arch = find_archetype_decl(ctx, arch_name);
	if (!arch)
		return NULL;
	for (int j = 0; j < arch->field_count; j++) {
		if (strcmp(arch->fields[j]->name, fn) == 0 && arch->fields[j]->type->kind == TYPE_SHAPED_ARRAY) {
			if (out_rank)
				*out_rank = field_total_elements(arch->fields[j]->type);
			return field_base_type_name(arch->fields[j]->type);
		}
	}
	return NULL;
}

/* Check if archetype has a field with given name */
static int archetype_has_field(ArchetypeDecl *arch, const char *field_name) {
	for (int i = 0; i < arch->field_count; i++) {
		if (strcmp(arch->fields[i]->name, field_name) == 0) {
			return 1;
		}
	}
	return 0;
}

/* Check if archetype has all required fields for a system */
static int archetype_matches_system(ArchetypeDecl *arch, SysDecl *sys) {
	if (!arch || !sys || !sys->params) {
		return 0;
	}
	for (int i = 0; i < sys->param_count; i++) {
		if (!sys->params[i] || !sys->params[i]->name) {
			return 0;
		}
		const char *param_name = sys->params[i]->name;
		if (!archetype_has_field(arch, param_name)) {
			return 0;
		}
	}
	return 1;
}

/* Forward declarations */
static void codegen_expression(CodegenContext *ctx, Expression *expr, char *result_buf);
static void codegen_statement(CodegenContext *ctx, Statement *stmt);
static int resolve_index_arch(CodegenContext *ctx, Expression *base_expr, Expression *idx_expr,
                              const char **out_arch_name, const char **out_arch_ptr, int *out_count_idx,
                              int *out_idx_is_i64);
static void emit_bounds_check(CodegenContext *ctx, const char *arch_name, const char *arch_ptr, int count_field_idx,
                              const char *idx_buf, int idx_is_i64);

/* ========== EXPRESSION CODEGEN ========== */

static void codegen_expression(CodegenContext *ctx, Expression *expr, char *result_buf) {
	if (!expr) {
		strcpy(result_buf, "0");
		return;
	}

	switch (expr->type) {
	case EXPR_LITERAL: {
		const char *lex = expr->data.literal.lexeme;

		/* Check if it's a char literal (starts with ') */
		if (lex[0] == '\'') {
			int char_value = 0;
			if (lex[1] == '\\') {
				/* Escape sequence */
				switch (lex[2]) {
				case 'n':
					char_value = '\n';
					break;
				case 't':
					char_value = '\t';
					break;
				case 'r':
					char_value = '\r';
					break;
				case '\\':
					char_value = '\\';
					break;
				case '\'':
					char_value = '\'';
					break;
				default:
					char_value = lex[2];
					break;
				}
			} else {
				/* Regular character */
				char_value = lex[1];
			}
			snprintf(result_buf, sizeof(result_buf), "%d", char_value);
		} else if (lex[0] == '"') {
			/* String literal */
			char *global_name = emit_string_global(ctx, lex);

			/* Compute actual string length accounting for escape sequences */
			size_t str_len = 0;
			for (int i = 1; lex[i] != '"' && lex[i] != '\0'; i++) {
				if (lex[i] == '\\' && lex[i + 1] != '\0') {
					i++; /* skip escape sequence (count as 1 char) */
				}
				str_len++;
			}

			char *res_name = gen_value_name(ctx);
			buffer_append_fmt(ctx, "  %s = getelementptr [%zu x i8], [%zu x i8]* %s, i32 0, i32 0\n", res_name,
			                  str_len + 1, str_len + 1, global_name);
			strcpy(result_buf, res_name);
			free(global_name);
		} else if (strchr(lex, '.') != NULL) {
			/* Float literal */
			strcpy(result_buf, lex);
		} else {
			/* Integer literal */
			strcpy(result_buf, lex);
		}
		break;
	}

	case EXPR_NAME: {
		const char *name = expr->data.name.name;
		ValueInfo *val = find_value(ctx, name);
		if (val) {
			/* If inside implicit loop and this is a type-4 column param, auto-index */
			if (ctx->implicit_loop_index[0] && val->type == 4) {
				const char *arche_type = val->field_type ? val->field_type : "float";
				const char *scalar_type = llvm_type_from_arche(arche_type);
				const char *load_type = elem_llvm_type(ctx, arche_type);
				char *idx_gep = gen_value_name(ctx);
				buffer_append_fmt(ctx, "  %s = getelementptr %s, %s* %s, i64 %s\n", idx_gep, scalar_type, scalar_type,
				                  val->llvm_name, ctx->implicit_loop_index);
				char *elem = gen_value_name(ctx);
				int align = ctx->vector_lanes > 0 ? 8 : 4;

				if (ctx->vector_lanes > 0) {
					/* Vector load: bitcast pointer to vector type, then load */
					char *vec_ptr = gen_value_name(ctx);
					buffer_append_fmt(ctx, "  %s = bitcast %s* %s to %s*\n", vec_ptr, scalar_type, idx_gep, load_type);
					buffer_append_fmt(ctx, "  %s = load %s, %s* %s, align %d\n", elem, load_type, load_type, vec_ptr,
					                  align);
				} else {
					/* Scalar load */
					buffer_append_fmt(ctx, "  %s = load %s, %s* %s, align %d\n", elem, load_type, scalar_type, idx_gep,
					                  align);
				}
				strcpy(result_buf, elem);
			} else if (val->type == 1) {
				/* Type-1: regular allocated value (i32, float, handle, etc) - load from pointer */
				char *loaded = gen_value_name(ctx);
				const char *llvm_type = "i32";
				if (val->field_type) {
					if (strcmp(val->field_type, "double") == 0 || strcmp(val->field_type, "float") == 0) {
						llvm_type = "double";
					} else if (strcmp(val->field_type, "handle") == 0) {
						llvm_type = "i64";
					}
				}
				buffer_append_fmt(ctx, "  %s = load %s, %s* %s\n", loaded, llvm_type, llvm_type, val->llvm_name);
				strcpy(result_buf, loaded);
			} else {
				/* Type-2 (string), 3 (arch), 5 (array), 6 (i8* param): return pointer directly */
				strcpy(result_buf, val->llvm_name);
			}
		} else if (find_archetype_decl(ctx, name)) {
			/* Archetype name — static global is @X directly; dynamic is loaded from @archetype_X */
			if (get_arch_static_capacity(ctx, name) > 0) {
				char global_ref[256];
				snprintf(global_ref, sizeof(global_ref), "@%s", name);
				strcpy(result_buf, global_ref);
			} else {
				char *loaded = gen_value_name(ctx);
				buffer_append_fmt(ctx, "  %s = load %%struct.%s*, %%struct.%s** @archetype_%s\n", loaded, name, name,
				                  name);
				strcpy(result_buf, loaded);
			}
		} else {
			/* undefined variable, use 0 */
			strcpy(result_buf, "0");
		}
		break;
	}

	case EXPR_BINARY: {
		char left_buf[256], right_buf[256];
		codegen_expression(ctx, expr->data.binary.left, left_buf);
		codegen_expression(ctx, expr->data.binary.right, right_buf);

		const char *op;
		int is_float = 0;

		/* Use semantic resolved type if available */
		if (expr->resolved_type &&
		    (strcmp(expr->resolved_type, "float") == 0 || strcmp(expr->resolved_type, "double") == 0)) {
			is_float = 1;
		} else if (expr->data.binary.left->resolved_type &&
		           (strcmp(expr->data.binary.left->resolved_type, "float") == 0 ||
		            strcmp(expr->data.binary.left->resolved_type, "double") == 0)) {
			is_float = 1;
		} else if (expr->data.binary.right->resolved_type &&
		           (strcmp(expr->data.binary.right->resolved_type, "float") == 0 ||
		            strcmp(expr->data.binary.right->resolved_type, "double") == 0)) {
			is_float = 1;
		} else {
			/* Fallback: Try to infer float type from operands */
			if (strchr(left_buf, '.') != NULL || strchr(right_buf, '.') != NULL) {
				is_float = 1;
			}
		}

		switch (expr->data.binary.op) {
		case OP_ADD:
			op = is_float ? "fadd" : "add";
			break;
		case OP_SUB:
			op = is_float ? "fsub" : "sub";
			break;
		case OP_MUL:
			op = is_float ? "fmul" : "mul";
			break;
		case OP_DIV:
			op = is_float ? "fdiv" : "sdiv";
			break;
		case OP_EQ:
			op = is_float ? "oeq" : "eq";
			break;
		case OP_NEQ:
			op = is_float ? "one" : "ne";
			break;
		case OP_LT:
			op = is_float ? "olt" : "slt";
			break;
		case OP_GT:
			op = is_float ? "ogt" : "sgt";
			break;
		case OP_LTE:
			op = is_float ? "ole" : "sle";
			break;
		case OP_GTE:
			op = is_float ? "oge" : "sge";
			break;
		default:
			op = "add";
			break;
		}

		char *res_name = gen_value_name(ctx);
		const char *type;
		if (is_float && ctx->vector_lanes > 0) {
			type = llvm_vector_type("double", ctx->vector_lanes);
		} else {
			type = is_float ? "double" : "i32";
		}

		/* For float operations, convert integer literals to doubles */
		const char *left_val = left_buf;
		const char *right_val = right_buf;
		char *left_conv = NULL;
		char *right_conv = NULL;

		if (is_float) {
			/* Check if left operand needs conversion to double */
			int left_needs_conv = 0;
			if (expr->data.binary.left->resolved_type && strcmp(expr->data.binary.left->resolved_type, "int") == 0) {
				left_needs_conv = 1;
			} else if (strchr(left_buf, '.') == NULL && strchr(left_buf, 'v') == NULL &&
			           strchr(left_buf, '%') == NULL) {
				/* Integer literal, convert to double */
				left_needs_conv = 1;
			}
			if (left_needs_conv) {
				left_conv = gen_value_name(ctx);
				buffer_append_fmt(ctx, "  %s = sitofp i32 %s to double\n", left_conv, left_buf);
				left_val = left_conv;
			}

			/* Check if right operand needs conversion to double */
			int right_needs_conv = 0;
			if (expr->data.binary.right->resolved_type && strcmp(expr->data.binary.right->resolved_type, "int") == 0) {
				right_needs_conv = 1;
			} else if (strchr(right_buf, '.') == NULL && strchr(right_buf, 'v') == NULL &&
			           strchr(right_buf, '%') == NULL) {
				/* Integer literal, convert to double */
				right_needs_conv = 1;
			}
			if (right_needs_conv) {
				right_conv = gen_value_name(ctx);
				buffer_append_fmt(ctx, "  %s = sitofp i32 %s to double\n", right_conv, right_buf);
				right_val = right_conv;
			}
		}

		if (expr->data.binary.op >= OP_EQ && expr->data.binary.op <= OP_GTE) {
			/* comparison: produce i1, then zext to i32 (match C: comparisons return int 0/1) */
			const char *cmp_type = is_float ? "double" : "i32";
			char *cmp_i1 = gen_value_name(ctx);
			if (is_float) {
				buffer_append_fmt(ctx, "  %s = fcmp %s %s %s, %s\n", cmp_i1, op, cmp_type, left_val, right_val);
			} else {
				buffer_append_fmt(ctx, "  %s = icmp %s %s %s, %s\n", cmp_i1, op, cmp_type, left_val, right_val);
			}
			buffer_append_fmt(ctx, "  %s = zext i1 %s to i32\n", res_name, cmp_i1);
		} else {
			/* arithmetic */
			buffer_append_fmt(ctx, "  %s = %s %s %s, %s\n", res_name, op, type, left_val, right_val);
		}

		strcpy(result_buf, res_name);
		break;
	}

	case EXPR_UNARY: {
		char operand_buf[256];
		codegen_expression(ctx, expr->data.unary.operand, operand_buf);

		char *res_name = gen_value_name(ctx);
		if (expr->data.unary.op == UNARY_NEG) {
			buffer_append_fmt(ctx, "  %s = sub i32 0, %s\n", res_name, operand_buf);
		} else if (expr->data.unary.op == UNARY_NOT) {
			buffer_append_fmt(ctx, "  %s = xor i1 1, %s\n", res_name, operand_buf);
		}
		strcpy(result_buf, res_name);
		break;
	}

	case EXPR_FIELD: {
		/* fieldexpr like archetype.field */
		char base_buf[256];
		codegen_expression(ctx, expr->data.field.base, base_buf);

		const char *field_name = expr->data.field.field_name;

		/* Check if base is an arch pointer (type 3) or archetype name */
		ValueInfo *base_val = NULL;
		const char *arch_name_direct = NULL;
		if (expr->data.field.base->type == EXPR_NAME) {
			const char *name = expr->data.field.base->data.name.name;
			base_val = find_value(ctx, name);
			if (!base_val && find_archetype_decl(ctx, name)) {
				arch_name_direct = name;
			}
		}

		/* Handle .length property */
		if (strcmp(field_name, "length") == 0) {
			/* For archetype columns: load count field */
			if (base_val && base_val->type == 3 && base_val->arch_name) {
				/* count field is always the last field in archetype struct */
				ArchetypeDecl *arch = find_archetype_decl(ctx, base_val->arch_name);
				if (arch) {
					int count_idx = arch->field_count; /* count field index */
					char *gep = gen_value_name(ctx);
					buffer_append_fmt(ctx, "  %s = getelementptr %%struct.%s, %%struct.%s* %s, i32 0, i32 %d\n", gep,
					                  base_val->arch_name, base_val->arch_name, base_buf, count_idx);
					char *count = gen_value_name(ctx);
					buffer_append_fmt(ctx, "  %s = load i64, i64* %s\n", count, gep);
					strcpy(result_buf, count);
					break;
				}
			}

			/* For arche_array: load length or max_length field */
			if (base_val && base_val->type == 5) {
				int field_idx = (strcmp(field_name, "max_length") == 0) ? 2 : 1;
				char *gep = gen_value_name(ctx);
				buffer_append_fmt(
				    ctx, "  %s = getelementptr %%struct.arche_array, %%struct.arche_array* %s, i32 0, i32 %d\n", gep,
				    base_buf, field_idx);
				char *loaded = gen_value_name(ctx);
				buffer_append_fmt(ctx, "  %s = load i64, i64* %s\n", loaded, gep);
				char *truncated = gen_value_name(ctx);
				buffer_append_fmt(ctx, "  %s = trunc i64 %s to i32\n", truncated, loaded);
				strcpy(result_buf, truncated);
				break;
			}

			/* For archetype: max_length is the capacity field */
			if (strcmp(field_name, "max_length") == 0 && base_val && base_val->type == 3 && base_val->arch_name) {
				ArchetypeDecl *arch = find_archetype_decl(ctx, base_val->arch_name);
				if (arch) {
					int cap_idx = arch->field_count + 1;
					char *gep = gen_value_name(ctx);
					buffer_append_fmt(ctx, "  %s = getelementptr %%struct.%s, %%struct.%s* %s, i32 0, i32 %d\n", gep,
					                  base_val->arch_name, base_val->arch_name, base_buf, cap_idx);
					char *cap = gen_value_name(ctx);
					buffer_append_fmt(ctx, "  %s = load i64, i64* %s\n", cap, gep);
					strcpy(result_buf, cap);
					break;
				}
			}
		}

		if (base_val && base_val->type == 3 && base_val->arch_name) {
			/* Find field index in archetype */
			ArchetypeDecl *arch = find_archetype_decl(ctx, base_val->arch_name);
			if (arch) {
				int field_idx = -1;
				FieldDecl *fdecl = NULL;
				for (int i = 0; i < arch->field_count; i++) {
					if (strcmp(arch->fields[i]->name, field_name) == 0) {
						field_idx = i;
						fdecl = arch->fields[i];
						break;
					}
				}

				if (field_idx >= 0 && fdecl) {
					const char *llvm_type = llvm_type_from_arche(field_base_type_name(fdecl->type));
					int is_static = get_arch_static_capacity(ctx, base_val->arch_name) > 0;

					if (fdecl->kind == FIELD_META) {
						char *gep = gen_value_name(ctx);
						buffer_append_fmt(ctx, "  %s = getelementptr %%struct.%s, %%struct.%s* %s, i32 0, i32 %d\n",
						                  gep, base_val->arch_name, base_val->arch_name, base_buf, field_idx);
						char *loaded = gen_value_name(ctx);
						buffer_append_fmt(ctx, "  %s = load %s, %s* %s\n", loaded, llvm_type, llvm_type, gep);
						strcpy(result_buf, loaded);
					} else {
						/* Col field */
						char *ptr_val = gen_value_name(ctx);
						if (is_static) {
							buffer_append_fmt(
							    ctx, "  %s = getelementptr %%struct.%s, %%struct.%s* %s, i32 0, i32 %d, i64 0\n",
							    ptr_val, base_val->arch_name, base_val->arch_name, base_buf, field_idx);
						} else {
							char *gep = gen_value_name(ctx);
							buffer_append_fmt(ctx, "  %s = getelementptr %%struct.%s, %%struct.%s* %s, i32 0, i32 %d\n",
							                  gep, base_val->arch_name, base_val->arch_name, base_buf, field_idx);
							buffer_append_fmt(ctx, "  %s = load %s*, %s** %s\n", ptr_val, llvm_type, llvm_type, gep);
						}

						if (ctx->implicit_loop_index[0]) {
							const char *load_type = elem_llvm_type(ctx, field_base_type_name(fdecl->type));
							char *idx_gep = gen_value_name(ctx);
							buffer_append_fmt(ctx, "  %s = getelementptr %s, %s* %s, i64 %s\n", idx_gep, llvm_type,
							                  llvm_type, ptr_val, ctx->implicit_loop_index);
							char *elem = gen_value_name(ctx);
							int align = ctx->vector_lanes > 0 ? 8 : 4;

							if (ctx->vector_lanes > 0) {
								char *vec_ptr = gen_value_name(ctx);
								buffer_append_fmt(ctx, "  %s = bitcast %s* %s to %s*\n", vec_ptr, llvm_type, idx_gep,
								                  load_type);
								buffer_append_fmt(ctx, "  %s = load %s, %s* %s, align %d\n", elem, load_type, load_type,
								                  vec_ptr, align);
							} else {
								buffer_append_fmt(ctx, "  %s = load %s, %s* %s, align %d\n", elem, load_type, llvm_type,
								                  idx_gep, align);
							}
							strcpy(result_buf, elem);
						} else {
							strcpy(result_buf, ptr_val);
						}
					}
					break;
				}
			}
		} else if (arch_name_direct) {
			/* Direct archetype name reference */
			ArchetypeDecl *arch = find_archetype_decl(ctx, arch_name_direct);
			if (arch) {
				int field_idx = -1;
				FieldDecl *fdecl = NULL;
				for (int i = 0; i < arch->field_count; i++) {
					if (strcmp(arch->fields[i]->name, field_name) == 0) {
						field_idx = i;
						fdecl = arch->fields[i];
						break;
					}
				}

				if (field_idx >= 0 && fdecl) {
					const char *llvm_type = llvm_type_from_arche(field_base_type_name(fdecl->type));
					int is_static = get_arch_static_capacity(ctx, arch_name_direct) > 0;

					if (fdecl->kind == FIELD_META) {
						char *gep = gen_value_name(ctx);
						buffer_append_fmt(ctx, "  %s = getelementptr %%struct.%s, %%struct.%s* %s, i32 0, i32 %d\n",
						                  gep, arch_name_direct, arch_name_direct, base_buf, field_idx);
						char *loaded = gen_value_name(ctx);
						buffer_append_fmt(ctx, "  %s = load %s, %s* %s\n", loaded, llvm_type, llvm_type, gep);
						strcpy(result_buf, loaded);
					} else {
						/* Col field */
						char *ptr_val = gen_value_name(ctx);
						if (is_static) {
							buffer_append_fmt(
							    ctx, "  %s = getelementptr %%struct.%s, %%struct.%s* %s, i32 0, i32 %d, i64 0\n",
							    ptr_val, arch_name_direct, arch_name_direct, base_buf, field_idx);
						} else {
							char *gep = gen_value_name(ctx);
							buffer_append_fmt(ctx, "  %s = getelementptr %%struct.%s, %%struct.%s* %s, i32 0, i32 %d\n",
							                  gep, arch_name_direct, arch_name_direct, base_buf, field_idx);
							buffer_append_fmt(ctx, "  %s = load %s*, %s** %s\n", ptr_val, llvm_type, llvm_type, gep);
						}

						if (ctx->implicit_loop_index[0]) {
							const char *load_type = elem_llvm_type(ctx, field_base_type_name(fdecl->type));
							char *idx_gep = gen_value_name(ctx);
							buffer_append_fmt(ctx, "  %s = getelementptr %s, %s* %s, i64 %s\n", idx_gep, llvm_type,
							                  llvm_type, ptr_val, ctx->implicit_loop_index);
							char *elem = gen_value_name(ctx);
							int align = ctx->vector_lanes > 0 ? 8 : 4;

							if (ctx->vector_lanes > 0) {
								char *vec_ptr = gen_value_name(ctx);
								buffer_append_fmt(ctx, "  %s = bitcast %s* %s to %s*\n", vec_ptr, llvm_type, idx_gep,
								                  load_type);
								buffer_append_fmt(ctx, "  %s = load %s, %s* %s, align %d\n", elem, load_type, load_type,
								                  vec_ptr, align);
							} else {
								buffer_append_fmt(ctx, "  %s = load %s, %s* %s, align %d\n", elem, load_type, llvm_type,
								                  idx_gep, align);
							}
							strcpy(result_buf, elem);
						} else {
							strcpy(result_buf, ptr_val);
						}
					}
					break;
				}
			}
		}

		/* Fallback: just return field name (shouldn't reach here) */
		strcpy(result_buf, field_name);
		break;
	}

	case EXPR_INDEX: {
		/* index access: array[index] or field[index] */
		char base_buf[256], idx_buf[256];
		codegen_expression(ctx, expr->data.index.base, base_buf);

		/* Check if base is a type-7 char buffer (let buf: char[256];) */
		ValueInfo *type7_vi = NULL;
		if (expr->data.index.base->type == EXPR_NAME) {
			ValueInfo *vi = find_value(ctx, expr->data.index.base->data.name.name);
			if (vi && vi->type == 7) {
				type7_vi = vi;
			}
		}

		/* Check if base is a type-6 slice pointer variable */
		const char *type6_elem_type = NULL;
		if (expr->data.index.base->type == EXPR_NAME) {
			ValueInfo *vi = find_value(ctx, expr->data.index.base->data.name.name);
			if (vi && vi->type == 6 && vi->field_type) {
				type6_elem_type = vi->field_type;
			}
		}

		int shaped_rank = 1;
		const char *shaped_elem = (expr->data.index.index_count >= 1)
		                              ? get_shaped_field_info(ctx, expr->data.index.base, &shaped_rank)
		                              : NULL;

		if (expr->data.index.index_count == 2 && shaped_elem) {
			/* 2D: [entity, elem] → flat = entity * rank + elem */
			char row_buf[256], elem_buf[256];
			codegen_expression(ctx, expr->data.index.indices[0], row_buf);
			codegen_expression(ctx, expr->data.index.indices[1], elem_buf);
			char *scaled = gen_value_name(ctx);
			buffer_append_fmt(ctx, "  %s = mul i64 %s, %d\n", scaled, row_buf, shaped_rank);
			char *flat = gen_value_name(ctx);
			buffer_append_fmt(ctx, "  %s = add i64 %s, %s\n", flat, scaled, elem_buf);
			strcpy(idx_buf, flat);
		} else if (expr->data.index.index_count == 1 && shaped_elem) {
			/* Single-index on shaped array: entity * rank → returns slice pointer, no load */
			char row_buf[256];
			codegen_expression(ctx, expr->data.index.indices[0], row_buf);
			if (shaped_rank > 1) {
				char *scaled = gen_value_name(ctx);
				buffer_append_fmt(ctx, "  %s = mul i64 %s, %d\n", scaled, row_buf, shaped_rank);
				strcpy(idx_buf, scaled);
			} else {
				strcpy(idx_buf, row_buf);
			}
		} else if (expr->data.index.index_count > 0) {
			codegen_expression(ctx, expr->data.index.indices[0], idx_buf);
		}

		/* Determine element type from semantic analysis */
		const char *scalar_type = "i32"; /* default */
		const char *arche_type = NULL;

		if (type6_elem_type) {
			arche_type = type6_elem_type;
			scalar_type = llvm_type_from_arche(type6_elem_type);
		} else if (shaped_elem) {
			arche_type = shaped_elem;
			scalar_type = llvm_type_from_arche(shaped_elem);
		} else if (expr->resolved_type) {
			arche_type = expr->resolved_type;
			scalar_type = llvm_type_from_arche(arche_type);
		} else if (expr->data.index.base->resolved_type) {
			/* Use base's resolved type (e.g., for pos[i] where pos is double*) */
			arche_type = expr->data.index.base->resolved_type;
			scalar_type = llvm_type_from_arche(arche_type);
		}

		/* In vector mode, load uses vector type; GEP uses scalar pointer */
		const char *load_type = arche_type ? elem_llvm_type(ctx, arche_type) : scalar_type;

		/* Handle type-7 char buffer specially */
		if (type7_vi && expr->data.index.index_count > 0) {
			codegen_expression(ctx, expr->data.index.indices[0], idx_buf);

			/* Convert i32 index to i64 for getelementptr */
			char *idx_i64 = gen_value_name(ctx);
			buffer_append_fmt(ctx, "  %s = sext i32 %s to i64\n", idx_i64, idx_buf);

			char *res_name = gen_value_name(ctx);
			buffer_append_fmt(ctx, "  %s = getelementptr [%d x i8], [%d x i8]* %s, i64 0, i64 %s\n", res_name,
			                  type7_vi->string_len, type7_vi->string_len, base_buf, idx_i64);

			char *loaded = gen_value_name(ctx);
			buffer_append_fmt(ctx, "  %s = load i8, i8* %s, align 1\n", loaded, res_name);

			/* Zero-extend i8 to i32 for compatibility with rest of system */
			char *extended = gen_value_name(ctx);
			buffer_append_fmt(ctx, "  %s = zext i8 %s to i32\n", extended, loaded);
			strcpy(result_buf, extended);
			break;
		}

		/* Bounds check for archetype column accesses */
		const char *bc_arch_name = NULL, *bc_arch_ptr = NULL;
		int bc_count_idx = -1, bc_idx_is_i64 = 0;
		if (expr->data.index.index_count > 0 && !shaped_elem &&
		    resolve_index_arch(ctx, expr->data.index.base, expr->data.index.indices[0], &bc_arch_name, &bc_arch_ptr,
		                       &bc_count_idx, &bc_idx_is_i64)) {
			emit_bounds_check(ctx, bc_arch_name, bc_arch_ptr, bc_count_idx, idx_buf, bc_idx_is_i64);
		}

		/* Ensure index is i64 for getelementptr (simple heuristic: if not from shaped array ops, assume i32 and
		 * convert) */
		const char *final_idx = idx_buf;
		if (expr->data.index.index_count > 0 && !shaped_elem) {
			/* Index likely came from a variable or expression, probably i32. Convert to i64. */
			char *idx_i64 = gen_value_name(ctx);
			buffer_append_fmt(ctx, "  %s = sext i32 %s to i64\n", idx_i64, idx_buf);
			final_idx = idx_i64;
		}

		char *res_name = gen_value_name(ctx);
		buffer_append_fmt(ctx, "  %s = getelementptr %s, %s* %s, i64 %s\n", res_name, scalar_type, scalar_type,
		                  base_buf, final_idx);

		if (expr->data.index.index_count == 1 && shaped_elem) {
			/* Single-index on shaped field → return pointer slice, not loaded value */
			strcpy(result_buf, res_name);
		} else {
			char *loaded = gen_value_name(ctx);
			int align = ctx->vector_lanes > 0 ? 8 : 4;

			if (ctx->vector_lanes > 0 && arche_type) {
				/* Vector load: bitcast pointer to vector type, then load */
				char *vec_ptr = gen_value_name(ctx);
				buffer_append_fmt(ctx, "  %s = bitcast %s* %s to %s*\n", vec_ptr, scalar_type, res_name, load_type);
				buffer_append_fmt(ctx, "  %s = load %s, %s* %s, align %d\n", loaded, load_type, load_type, vec_ptr,
				                  align);
			} else {
				/* Scalar load */
				buffer_append_fmt(ctx, "  %s = load %s, %s* %s, align %d\n", loaded, load_type, scalar_type, res_name,
				                  align);
			}
			strcpy(result_buf, loaded);
		}
		break;
	}

	case EXPR_CALL: {
		/* function call */
		char *func_name = NULL;
		if (expr->data.call.callee->type == EXPR_NAME) {
			func_name = expr->data.call.callee->data.name.name;
		}

		/* Special handling for insert builtin */
		if (func_name && strcmp(func_name, "insert") == 0 && expr->data.call.arg_count > 0) {
			/* args[0] is archetype variable, args[1..] are field values */
			char arch_buf[256];
			codegen_expression(ctx, expr->data.call.args[0], arch_buf);

			/* Get arch_name from ValueInfo or archetype name of args[0] */
			const char *arch_name = NULL;
			ArchetypeDecl *arch = NULL;
			if (expr->data.call.args[0]->type == EXPR_NAME) {
				const char *name = expr->data.call.args[0]->data.name.name;
				ValueInfo *arch_var = find_value(ctx, name);
				if (arch_var && arch_var->arch_name) {
					arch_name = arch_var->arch_name;
					arch = find_archetype_decl(ctx, arch_name);
				} else if (find_archetype_decl(ctx, name)) {
					/* Direct archetype name */
					arch_name = name;
					arch = find_archetype_decl(ctx, arch_name);
				}
			}

			if (arch_name && arch) {
				/* Evaluate all field arguments first */
				char field_bufs[32][256];
				int field_idx = 0;
				int arg_count = 0;
				for (int i = 1; i < expr->data.call.arg_count && arg_count < 32; i++) {
					while (field_idx < arch->field_count && arch->fields[field_idx]->kind != FIELD_COLUMN) {
						field_idx++;
					}
					if (field_idx < arch->field_count) {
						codegen_expression(ctx, expr->data.call.args[i], field_bufs[arg_count]);
						arg_count++;
						field_idx++;
					}
				}

				/* Now emit the call with all arguments */
				char *handle_tmp = gen_value_name(ctx);
				buffer_append_fmt(ctx, "  %s = call i64 @arche_insert_%s(%%struct.%s* %s", handle_tmp, arch_name, arch_name, arch_buf);

				field_idx = 0;
				for (int i = 0; i < arg_count; i++) {
					while (field_idx < arch->field_count && arch->fields[field_idx]->kind != FIELD_COLUMN) {
						field_idx++;
					}
					if (field_idx < arch->field_count) {
						const char *field_type =
						    llvm_type_from_arche(field_base_type_name(arch->fields[field_idx]->type));
						buffer_append_fmt(ctx, ", %s %s", field_type, field_bufs[i]);
						field_idx++;
					}
				}
				buffer_append(ctx, ")\n");
				strcpy(result_buf, handle_tmp);
			}
			break;
		}

		/* Special handling for delete builtin */
		if (func_name && strcmp(func_name, "delete") == 0 && expr->data.call.arg_count >= 2) {
			/* args[0] is archetype variable, args[1] is handle */
			char arch_buf[256];
			codegen_expression(ctx, expr->data.call.args[0], arch_buf);

			char idx_buf[256];
			codegen_expression(ctx, expr->data.call.args[1], idx_buf);

			/* Get arch_name from ValueInfo or archetype name of args[0] */
			const char *arch_name = NULL;
			if (expr->data.call.args[0]->type == EXPR_NAME) {
				const char *name = expr->data.call.args[0]->data.name.name;
				ValueInfo *arch_var = find_value(ctx, name);
				if (arch_var && arch_var->arch_name) {
					arch_name = arch_var->arch_name;
				} else if (find_archetype_decl(ctx, name)) {
					/* Direct archetype name */
					arch_name = name;
				}
			}

			/* Check handle type matches delete archetype */
			if (expr->data.call.args[1]->type == EXPR_NAME) {
				const char *handle_var = expr->data.call.args[1]->data.name.name;
				ValueInfo *handle_vi = find_value(ctx, handle_var);
				if (handle_vi && handle_vi->field_type && strcmp(handle_vi->field_type, "handle") == 0) {
					if (handle_vi->handle_archetype && arch_name &&
					    strcmp(handle_vi->handle_archetype, arch_name) != 0) {
						fprintf(stderr, "Error: type mismatch in delete: handle for %s cannot delete %s\n",
							handle_vi->handle_archetype, arch_name);
						strcpy(result_buf, "0");
						break;
					}
				}
			}

			if (arch_name) {
				buffer_append_fmt(ctx, "  call void @arche_delete_%s(%%struct.%s* %s, i64 %s)\n", arch_name, arch_name,
				                  arch_buf, idx_buf);
				strcpy(result_buf, "0");
			}
			break;
		}

		/* Special handling for dealloc builtin */
		if (func_name && strcmp(func_name, "dealloc") == 0 && expr->data.call.arg_count >= 1) {
			/* args[0] is archetype variable to deallocate */
			char arch_buf[256];
			codegen_expression(ctx, expr->data.call.args[0], arch_buf);

			/* Get arch_name from ValueInfo or archetype name of args[0] */
			const char *arch_name = NULL;
			if (expr->data.call.args[0]->type == EXPR_NAME) {
				const char *name = expr->data.call.args[0]->data.name.name;
				ValueInfo *arch_var = find_value(ctx, name);
				if (arch_var && arch_var->arch_name) {
					arch_name = arch_var->arch_name;
				} else if (find_archetype_decl(ctx, name)) {
					/* Direct archetype name */
					arch_name = name;
				}
			}

			if (arch_name) {
				buffer_append_fmt(ctx, "  call void @arche_dealloc_%s(%%struct.%s* %s)\n", arch_name, arch_name,
				                  arch_buf);
				strcpy(result_buf, "0");
			}
			break;
		}

		/* Evaluate arguments and track their ValueInfo types */
		char **arg_bufs = malloc(expr->data.call.arg_count * sizeof(char *));
		int *arg_is_string = malloc(expr->data.call.arg_count * sizeof(int));
		int *arg_is_array_literal = malloc(expr->data.call.arg_count * sizeof(int));
		ValueInfo **arg_values = malloc(expr->data.call.arg_count * sizeof(ValueInfo *));
		for (int i = 0; i < expr->data.call.arg_count; i++) {
			arg_bufs[i] = malloc(256);
			arg_is_string[i] = 0;
			arg_is_array_literal[i] = 0;
			arg_values[i] = NULL;

			/* Check if this arg is a string literal */
			if (expr->data.call.args[i]->type == EXPR_STRING) {
				arg_is_string[i] = 1;
			}
			/* Check if this arg is an old-style string literal (EXPR_LITERAL with quotes) */
			else if (expr->data.call.args[i]->type == EXPR_LITERAL &&
			         expr->data.call.args[i]->data.literal.lexeme[0] == '"') {
				arg_is_string[i] = 1;
			}
			/* Check if this arg is an array literal */
			else if (expr->data.call.args[i]->type == EXPR_ARRAY_LITERAL) {
				arg_is_array_literal[i] = 1;
			}

			codegen_expression(ctx, expr->data.call.args[i], arg_bufs[i]);
		}

		/* Second pass: check if arguments are variables holding strings (type 2, 5, 6) */
		for (int i = 0; i < expr->data.call.arg_count; i++) {
			if (!arg_is_string[i] && !arg_is_array_literal[i] && expr->data.call.args[i]->type == EXPR_NAME) {
				ValueInfo *var = find_value(ctx, expr->data.call.args[i]->data.name.name);
				if (var) {
					arg_values[i] = var;
					if (var->type == 2 || var->type == 5 || var->type == 6) {
						arg_is_string[i] = 1;
					}
				}
			}
		}

		ProcDecl *callee_proc = find_proc_decl(ctx, func_name);
		FuncDecl *callee_func = find_func_decl(ctx, func_name);
		char **call_arg_vals = malloc(expr->data.call.arg_count * sizeof(char *));
		const char **call_arg_types = malloc(expr->data.call.arg_count * sizeof(const char *));

		/* Prepare arguments: emit conversions before the call, collect register names */
		for (int i = 0; i < expr->data.call.arg_count; i++) {
			call_arg_vals[i] = malloc(256);
			call_arg_types[i] = "i32"; /* Default type */

			/* Determine what callee param expects */
			int callee_wants_arr = 0;
			int callee_is_extern = (callee_proc && callee_proc->is_extern) || (callee_func && callee_func->is_extern);
			if (callee_proc && i < callee_proc->param_count) {
				TypeRef *pt = callee_proc->params[i]->type;
				if (pt && pt->kind == TYPE_ARRAY) {
					callee_wants_arr = 1;
				}
			}
			if (callee_func && i < callee_func->param_count) {
				TypeRef *pt = callee_func->params[i]->type;
				if (pt && pt->kind == TYPE_ARRAY) {
					callee_wants_arr = 1;
				}
			}

			/* Handle type conversions, emit code before call if needed */
			if (arg_values[i] && arg_values[i]->type == 7) {
				/* Arg is char buffer [N x i8]* — cast to i8* for C functions */
				char *bitcast = gen_value_name(ctx);
				buffer_append_fmt(ctx, "  %s = bitcast [%d x i8]* %s to i8*\n", bitcast, arg_values[i]->string_len,
				                  arg_bufs[i]);
				strcpy(call_arg_vals[i], bitcast);
				call_arg_types[i] = "i8*";
			} else if (arg_values[i] && arg_values[i]->type == 5) {
				/* Arg is arche_array struct */
				if (callee_is_extern && callee_wants_arr) {
					/* Extract i8* data ptr from struct (field 0) — C ABI */
					char *dp_gep = gen_value_name(ctx);
					buffer_append_fmt(
					    ctx, "  %s = getelementptr %%struct.arche_array, %%struct.arche_array* %s, i32 0, i32 0\n",
					    dp_gep, arg_bufs[i]);
					char *dp = gen_value_name(ctx);
					buffer_append_fmt(ctx, "  %s = load i8*, i8** %s\n", dp, dp_gep);
					strcpy(call_arg_vals[i], dp);
					call_arg_types[i] = "i8*";
				} else if (callee_wants_arr) {
					/* Pass struct ptr directly — non-extern call */
					strcpy(call_arg_vals[i], arg_bufs[i]);
					call_arg_types[i] = "%struct.arche_array*";
				} else {
					/* Default to i32 */
					strcpy(call_arg_vals[i], arg_bufs[i]);
					call_arg_types[i] = "i32";
				}
			} else if (arg_is_string[i]) {
				/* String literals are already i8*, string variables may be arche_array */
				int is_string_literal = (arg_values[i] == NULL);
				int is_array_struct = arg_values[i] && arg_values[i]->type == 5; /* arche_array struct */

				if (is_string_literal && callee_wants_arr && !callee_is_extern) {
					/* String literal passed to non-extern function expecting char[]: wrap in arche_array struct */
					char *str_ptr = arg_bufs[i]; /* i8* from getelementptr */

					/* Need to know string length */
					const char *lex = expr->data.call.args[i]->data.literal.lexeme;
					size_t str_len = 0;
					for (int j = 1; lex[j] != '"' && lex[j] != '\0'; j++) {
						if (lex[j] == '\\' && lex[j + 1] != '\0') {
							j++; /* skip escape sequence (count as 1 char) */
						}
						str_len++;
					}

					/* Create struct on stack */
					char *arr_alloca = gen_value_name(ctx);
					emit_alloca(ctx, "  %s = alloca %%struct.arche_array\n", arr_alloca);

					/* Store data pointer in field 0 */
					char *ptr_gep = gen_value_name(ctx);
					buffer_append_fmt(
					    ctx, "  %s = getelementptr %%struct.arche_array, %%struct.arche_array* %s, i32 0, i32 0\n",
					    ptr_gep, arr_alloca);
					buffer_append_fmt(ctx, "  store i8* %s, i8** %s\n", str_ptr, ptr_gep);

					/* Store length in field 1 */
					char *len_gep = gen_value_name(ctx);
					buffer_append_fmt(
					    ctx, "  %s = getelementptr %%struct.arche_array, %%struct.arche_array* %s, i32 0, i32 1\n",
					    len_gep, arr_alloca);
					buffer_append_fmt(ctx, "  store i64 %zu, i64* %s\n", str_len, len_gep);

					/* Store capacity in field 2 */
					char *cap_gep = gen_value_name(ctx);
					buffer_append_fmt(
					    ctx, "  %s = getelementptr %%struct.arche_array, %%struct.arche_array* %s, i32 0, i32 2\n",
					    cap_gep, arr_alloca);
					buffer_append_fmt(ctx, "  store i64 %zu, i64* %s\n", str_len, cap_gep);

					strcpy(call_arg_vals[i], arr_alloca);
					call_arg_types[i] = "%struct.arche_array*";
				} else if (is_string_literal) {
					/* String literal: already i8* from codegen_expression */
					strcpy(call_arg_vals[i], arg_bufs[i]);
					call_arg_types[i] = "i8*";
				} else if (is_array_struct) {
					/* String variable stored as arche_array struct: pass struct ptr (variadic handler will unwrap) */
					strcpy(call_arg_vals[i], arg_bufs[i]);
					call_arg_types[i] = "%struct.arche_array*";
				} else if (callee_is_extern && callee_wants_arr) {
					/* String variable from extern callee expecting char[]: extract i8* from struct */
					char *dp_gep = gen_value_name(ctx);
					buffer_append_fmt(
					    ctx, "  %s = getelementptr %%struct.arche_array, %%struct.arche_array* %s, i32 0, i32 0\n",
					    dp_gep, arg_bufs[i]);
					char *dp = gen_value_name(ctx);
					buffer_append_fmt(ctx, "  %s = load i8*, i8** %s\n", dp, dp_gep);
					strcpy(call_arg_vals[i], dp);
					call_arg_types[i] = "i8*";
				} else if (callee_wants_arr) {
					/* Pass struct ptr for internal Arche calls */
					strcpy(call_arg_vals[i], arg_bufs[i]);
					call_arg_types[i] = "%struct.arche_array*";
				} else {
					/* Pass bare pointer */
					strcpy(call_arg_vals[i], arg_bufs[i]);
					call_arg_types[i] = "i8*";
				}
			} else if (arg_is_array_literal[i]) {
				/* Array literal */
				if (callee_wants_arr) {
					/* Pass struct pointer directly */
					strcpy(call_arg_vals[i], arg_bufs[i]);
					call_arg_types[i] = "%struct.arche_array*";
				} else {
					/* Pass bare value */
					strcpy(call_arg_vals[i], arg_bufs[i]);
					call_arg_types[i] = "i32";
				}
			} else {
				/* Check if arg is float/double */
				if (expr->data.call.args[i]->resolved_type &&
				    (strcmp(expr->data.call.args[i]->resolved_type, "float") == 0 ||
				     strcmp(expr->data.call.args[i]->resolved_type, "double") == 0)) {
					strcpy(call_arg_vals[i], arg_bufs[i]);
					call_arg_types[i] = "double";
				} else {
					/* Default to i32 */
					strcpy(call_arg_vals[i], arg_bufs[i]);
					call_arg_types[i] = "i32";
				}
			}
		}

		char *res_name = gen_value_name(ctx);

		/* Special handling for print function with double arguments */
		const char *actual_func_name = func_name ? func_name : "unknown";
		if (func_name && strcmp(func_name, "print") == 0 && expr->data.call.arg_count == 1 && call_arg_types[0]) {
			if (strcmp(call_arg_types[0], "double") == 0) {
				actual_func_name = "print_double";
			} else if (strcmp(call_arg_types[0], "i32") == 0) {
				actual_func_name = "print_int";
			}
		}

		/* Emit the call with prepared arguments */
		/* Check if this is a variadic function like sprintf or printf */
		int is_variadic = func_name && (strcmp(func_name, "sprintf") == 0 || strcmp(func_name, "printf") == 0);
		int is_exit = func_name && strcmp(func_name, "exit") == 0;

		if (is_exit) {
			/* exit() is a void function that never returns */
			buffer_append_fmt(ctx, "  call void @%s(", actual_func_name);
			for (int i = 0; i < expr->data.call.arg_count; i++) {
				buffer_append_fmt(ctx, "%s %s", call_arg_types[i], call_arg_vals[i]);
				if (i < expr->data.call.arg_count - 1) {
					buffer_append(ctx, ", ");
				}
			}
			buffer_append(ctx, ")\n");
			buffer_append(ctx, "  unreachable\n");
			strcpy(result_buf, "0");
		} else if (is_variadic) {
			/* For variadic C functions, array struct args must be unwrapped to i8* */
			for (int i = 0; i < expr->data.call.arg_count; i++) {
				if (call_arg_types[i] && strcmp(call_arg_types[i], "%struct.arche_array*") == 0) {
					char *dp_gep = gen_value_name(ctx);
					buffer_append_fmt(
					    ctx, "  %s = getelementptr %%struct.arche_array, %%struct.arche_array* %s, i32 0, i32 0\n",
					    dp_gep, call_arg_vals[i]);
					char *dp = gen_value_name(ctx);
					buffer_append_fmt(ctx, "  %s = load i8*, i8** %s\n", dp, dp_gep);
					strcpy(call_arg_vals[i], dp);
					call_arg_types[i] = "i8*";
				}
			}
			/* Emit variadic signature based on function */
			if (strcmp(func_name, "sprintf") == 0) {
				buffer_append_fmt(ctx, "  %s = call i32 (i8*, i8*, ...)", res_name);
			} else {
				buffer_append_fmt(ctx, "  %s = call i32 (i8*, ...)", res_name);
			}
			buffer_append_fmt(ctx, " @%s(", actual_func_name);
			for (int i = 0; i < expr->data.call.arg_count; i++) {
				buffer_append_fmt(ctx, "%s %s", call_arg_types[i], call_arg_vals[i]);
				if (i < expr->data.call.arg_count - 1) {
					buffer_append(ctx, ", ");
				}
			}
			buffer_append(ctx, ")\n");
			strcpy(result_buf, res_name);
		} else {
			/* Normal non-variadic call - determine return type */
			const char *return_type = "i32"; /* default */

			/* Check if we have return type info from the func declaration */
			if (callee_func && callee_func->return_type) {
				return_type = llvm_type_from_arche(callee_func->return_type->data.name);
			} else if (callee_proc && callee_proc->is_extern) {
				/* For extern procs, check if they're in core lib */
				if (strcmp(func_name, "atof") == 0) {
					return_type = "double";
				} else if (strcmp(func_name, "atoi") == 0) {
					return_type = "i32";
				} else if (strcmp(func_name, "open") == 0 || strcmp(func_name, "read") == 0 ||
				           strcmp(func_name, "close") == 0) {
					return_type = "i32";
				}
				/* Add more extern functions as needed */
			} else if (callee_proc && callee_proc->is_extern == 0) {
				/* Non-extern proc: always void */
				return_type = "void";
			} else if (expr->resolved_type) {
				/* Fallback: use resolved type if available */
				return_type = llvm_type_from_arche(expr->resolved_type);
			}

			/* If return type is void, emit void call without assignment */
			if (strcmp(return_type, "void") == 0) {
				buffer_append_fmt(ctx, "  call void @%s(", actual_func_name);
				for (int i = 0; i < expr->data.call.arg_count; i++) {
					buffer_append_fmt(ctx, "%s %s", call_arg_types[i], call_arg_vals[i]);
					if (i < expr->data.call.arg_count - 1) {
						buffer_append(ctx, ", ");
					}
				}
				buffer_append(ctx, ")\n");
				strcpy(result_buf, "0");
			} else {
				buffer_append_fmt(ctx, "  %s = call %s @%s(", res_name, return_type, actual_func_name);
				for (int i = 0; i < expr->data.call.arg_count; i++) {
					buffer_append_fmt(ctx, "%s %s", call_arg_types[i], call_arg_vals[i]);
					if (i < expr->data.call.arg_count - 1) {
						buffer_append(ctx, ", ");
					}
				}
				buffer_append(ctx, ")\n");
				strcpy(result_buf, res_name);
			}
		}

		/* Cleanup */
		for (int i = 0; i < expr->data.call.arg_count; i++) {
			free(arg_bufs[i]);
			free(call_arg_vals[i]);
		}
		free(arg_bufs);
		free(arg_is_string);
		free(arg_is_array_literal);
		free(arg_values);
		free(call_arg_vals);
		free(call_arg_types);
		break;
	}

	case EXPR_ARRAY_LITERAL: {
		/* Array literal: {elem1, elem2, ...} */
		/* Generate global constant array and wrap in arche_array struct */
		Expression **elems = expr->data.array_literal.elements;
		int elem_count = expr->data.array_literal.element_count;

		if (elem_count == 0) {
			strcpy(result_buf, "0");
			break;
		}

		/* Create global constant array */
		char global_name[64];
		snprintf(global_name, sizeof(global_name), "@.arr%d", ctx->string_counter++);

		/* Build array constant declaration and add to globals */
		char global_decl[4096];
		char *decl_pos = global_decl;
		size_t decl_space = sizeof(global_decl);

		decl_pos += snprintf(decl_pos, decl_space, "%s = private constant [%d x i8] [", global_name, elem_count + 1);
		decl_space = sizeof(global_decl) - (decl_pos - global_decl);

		for (int i = 0; i < elem_count; i++) {
			char elem_buf[256];
			codegen_expression(ctx, elems[i], elem_buf);
			decl_pos += snprintf(decl_pos, decl_space, "i8 %s", elem_buf);
			decl_space = sizeof(global_decl) - (decl_pos - global_decl);
			if (i < elem_count - 1) {
				decl_pos += snprintf(decl_pos, decl_space, ", ");
				decl_space = sizeof(global_decl) - (decl_pos - global_decl);
			}
		}
		snprintf(decl_pos, decl_space, ", i8 0]\n");

		/* Append to globals buffer */
		size_t decl_len = strlen(global_decl);
		if (ctx->globals_pos + decl_len >= ctx->globals_size) {
			ctx->globals_size = (ctx->globals_size + decl_len) * 2;
			ctx->globals_buffer = realloc(ctx->globals_buffer, ctx->globals_size);
		}
		strcpy(ctx->globals_buffer + ctx->globals_pos, global_decl);
		ctx->globals_pos += decl_len;

		/* Create struct on stack and return pointer */
		char *arr_alloca = gen_value_name(ctx);
		emit_alloca(ctx, "  %s = alloca %%struct.arche_array\n", arr_alloca);

		/* Get pointer to global array */
		char *arr_ptr = gen_value_name(ctx);
		buffer_append_fmt(ctx, "  %s = getelementptr [%d x i8], [%d x i8]* %s, i32 0, i32 0\n", arr_ptr, elem_count + 1,
		                  elem_count + 1, global_name);

		/* Data pointer is already i8* */
		char *data_ptr = arr_ptr;

		/* Store data pointer in field 0 */
		char *ptr_gep = gen_value_name(ctx);
		buffer_append_fmt(ctx, "  %s = getelementptr %%struct.arche_array, %%struct.arche_array* %s, i32 0, i32 0\n",
		                  ptr_gep, arr_alloca);
		buffer_append_fmt(ctx, "  store i8* %s, i8** %s\n", data_ptr, ptr_gep);

		/* Store length in field 1 */
		char *len_gep = gen_value_name(ctx);
		buffer_append_fmt(ctx, "  %s = getelementptr %%struct.arche_array, %%struct.arche_array* %s, i32 0, i32 1\n",
		                  len_gep, arr_alloca);
		buffer_append_fmt(ctx, "  store i64 %d, i64* %s\n", elem_count, len_gep);

		/* Store capacity in field 2 */
		char *cap_gep = gen_value_name(ctx);
		buffer_append_fmt(ctx, "  %s = getelementptr %%struct.arche_array, %%struct.arche_array* %s, i32 0, i32 2\n",
		                  cap_gep, arr_alloca);
		buffer_append_fmt(ctx, "  store i64 %d, i64* %s\n", elem_count, cap_gep);

		strcpy(result_buf, arr_alloca);
		break;
	}

	case EXPR_ALLOC: {
		/* allocation expression: alloc ArchetypeName(count) */
		const char *arch_name = expr->data.alloc.archetype_name;

		/* Get capacity from first field_value */
		char count_buf[256] = "256";
		if (expr->data.alloc.field_count > 0) {
			codegen_expression(ctx, expr->data.alloc.field_values[0], count_buf);
		}

		/* Find archetype declaration */
		ArchetypeDecl *arch = find_archetype_decl(ctx, arch_name);
		if (!arch) {
			strcpy(result_buf, "0");
			break;
		}

		/* Struct layout: [pointers...][count][capacity][free_list*][free_count][gen_counters*] */
		/* Calculate struct header size: (field_count pointers + 5 metadata) * 8 = (field_count+5)*8 */
		int struct_sz_bytes = (arch->field_count + 5) * 8;

		/* Calculate byte size per element across all columns */
		int bytes_per_row = 0;
		for (int i = 0; i < arch->field_count; i++) {
			if (arch->fields[i]->kind == FIELD_COLUMN) {
				const char *elem_type = llvm_type_from_arche(field_base_type_name(arch->fields[i]->type));
				int n = field_total_elements(arch->fields[i]->type);
				int elem_sz = (elem_type[0] == 'd' || strcmp(elem_type, "i64") == 0) ? 8 : 4;
				bytes_per_row += elem_sz * n;
			}
		}
		/* Add 8 bytes per row for free_list entry (i64), 4 for gen_counters (i32) */
		int total_bytes_per_row = bytes_per_row + 12;

		/* Total size = struct_header + (count * bytes_per_row) + (count * 8 for free_list) */
		/* = struct_sz_bytes + count * total_bytes_per_row */
		char *total_bytes = gen_value_name(ctx);
		char *data_bytes = gen_value_name(ctx);
		buffer_append_fmt(ctx, "  %s = mul i64 %s, %d\n", data_bytes, count_buf, total_bytes_per_row);
		buffer_append_fmt(ctx, "  %s = add i64 %d, %s\n", total_bytes, struct_sz_bytes, data_bytes);

		/* Single calloc (zeros memory for gen_counters) */
		char *raw_ptr = gen_value_name(ctx);
		buffer_append_fmt(ctx, "  %s = call i8* @calloc(i64 1, i64 %s)\n", raw_ptr, total_bytes);

		/* Bitcast to struct pointer */
		char *struct_ptr = gen_value_name(ctx);
		buffer_append_fmt(ctx, "  %s = bitcast i8* %s to %%struct.%s*\n", struct_ptr, raw_ptr, arch_name);

		/* Initialize metadata fields */
		char *count_gep = gen_value_name(ctx);
		buffer_append_fmt(ctx, "  %s = getelementptr %%struct.%s, %%struct.%s* %s, i32 0, i32 %d\n", count_gep,
		                  arch_name, arch_name, struct_ptr, arch->field_count);
		buffer_append_fmt(ctx, "  store i64 0, i64* %s\n", count_gep);

		char *cap_gep = gen_value_name(ctx);
		buffer_append_fmt(ctx, "  %s = getelementptr %%struct.%s, %%struct.%s* %s, i32 0, i32 %d\n", cap_gep, arch_name,
		                  arch_name, struct_ptr, arch->field_count + 1);
		buffer_append_fmt(ctx, "  store i64 %s, i64* %s\n", count_buf, cap_gep);

		char *fc_gep = gen_value_name(ctx);
		buffer_append_fmt(ctx, "  %s = getelementptr %%struct.%s, %%struct.%s* %s, i32 0, i32 %d\n", fc_gep, arch_name,
		                  arch_name, struct_ptr, arch->field_count + 3);
		buffer_append_fmt(ctx, "  store i64 0, i64* %s\n", fc_gep);

		/* Set column pointers and free_list pointer to offsets in the allocated block */
		int col_offset = 0;
		for (int i = 0; i < arch->field_count; i++) {
			if (arch->fields[i]->kind == FIELD_COLUMN) {
				const char *elem_type = llvm_type_from_arche(field_base_type_name(arch->fields[i]->type));

				char *col_gep = gen_value_name(ctx);
				buffer_append_fmt(ctx, "  %s = getelementptr %%struct.%s, %%struct.%s* %s, i32 0, i32 %d\n", col_gep,
				                  arch_name, arch_name, struct_ptr, i);

				char *col_offset_llvm = gen_value_name(ctx);
				buffer_append_fmt(ctx, "  %s = mul i64 %s, %d\n", col_offset_llvm, count_buf, col_offset);

				char *col_data_base = gen_value_name(ctx);
				buffer_append_fmt(ctx, "  %s = getelementptr i8, i8* %s, i64 %d\n", col_data_base, raw_ptr,
				                  struct_sz_bytes);

				char *col_data = gen_value_name(ctx);
				buffer_append_fmt(ctx, "  %s = getelementptr i8, i8* %s, i64 %s\n", col_data, col_data_base,
				                  col_offset_llvm);

				char *col_ptr = gen_value_name(ctx);
				buffer_append_fmt(ctx, "  %s = bitcast i8* %s to %s*\n", col_ptr, col_data, elem_type);

				buffer_append_fmt(ctx, "  store %s* %s, %s** %s\n", elem_type, col_ptr, elem_type, col_gep);

				int n = field_total_elements(arch->fields[i]->type);
				int elem_size = ((elem_type[0] == 'd') ? 8 : 4) * n;
				col_offset += elem_size;
			}
		}

		/* Set free_list pointer: it comes after all column rows */
		char *fl_gep = gen_value_name(ctx);
		buffer_append_fmt(ctx, "  %s = getelementptr %%struct.%s, %%struct.%s* %s, i32 0, i32 %d\n", fl_gep, arch_name,
		                  arch_name, struct_ptr, arch->field_count + 2);

		/* free_list offset = struct_header + (all columns data) */
		char *fl_offset = gen_value_name(ctx);
		buffer_append_fmt(ctx, "  %s = mul i64 %s, %d\n", fl_offset, count_buf, bytes_per_row);
		char *fl_data = gen_value_name(ctx);
		buffer_append_fmt(ctx, "  %s = getelementptr i8, i8* %s, i64 %s\n", fl_data, raw_ptr, fl_offset);
		char *fl_add_header = gen_value_name(ctx);
		buffer_append_fmt(ctx, "  %s = getelementptr i8, i8* %s, i64 %d\n", fl_add_header, fl_data, struct_sz_bytes);

		char *fl_ptr = gen_value_name(ctx);
		buffer_append_fmt(ctx, "  %s = bitcast i8* %s to i64*\n", fl_ptr, fl_add_header);

		buffer_append_fmt(ctx, "  store i64* %s, i64** %s\n", fl_ptr, fl_gep);

		/* Set gen_counters pointer: it comes after free_list */
		char *gc_gep = gen_value_name(ctx);
		buffer_append_fmt(ctx, "  %s = getelementptr %%struct.%s, %%struct.%s* %s, i32 0, i32 %d\n", gc_gep, arch_name,
		                  arch_name, struct_ptr, arch->field_count + 4);

		/* gen_counters offset = struct_header + (all columns data) + (free_list data) */
		char *gc_offset = gen_value_name(ctx);
		buffer_append_fmt(ctx, "  %s = mul i64 %s, %d\n", gc_offset, count_buf, bytes_per_row + 8);
		char *gc_data = gen_value_name(ctx);
		buffer_append_fmt(ctx, "  %s = getelementptr i8, i8* %s, i64 %s\n", gc_data, raw_ptr, gc_offset);
		char *gc_add_header = gen_value_name(ctx);
		buffer_append_fmt(ctx, "  %s = getelementptr i8, i8* %s, i64 %d\n", gc_add_header, gc_data, struct_sz_bytes);

		char *gc_ptr = gen_value_name(ctx);
		buffer_append_fmt(ctx, "  %s = bitcast i8* %s to i32*\n", gc_ptr, gc_add_header);

		buffer_append_fmt(ctx, "  store i32* %s, i32** %s\n", gc_ptr, gc_gep);

		/* Handle field initialization: for each named field, fill with the init value */
		for (int init_idx = 1; init_idx < expr->data.alloc.field_count; init_idx++) {
			const char *field_name = expr->data.alloc.field_names[init_idx];
			Expression *init_value = expr->data.alloc.field_values[init_idx];

			/* Find field in archetype */
			int field_idx = -1;
			for (int i = 0; i < arch->field_count; i++) {
				if (strcmp(arch->fields[i]->name, field_name) == 0) {
					field_idx = i;
					break;
				}
			}
			if (field_idx == -1) {
				/* Field not found, semantic analysis should catch this */
				continue;
			}

			FieldDecl *field = arch->fields[field_idx];
			if (field->kind != FIELD_COLUMN) {
				/* Only fill column fields */
				continue;
			}

			/* Get field's element type */
			const char *elem_type = llvm_type_from_arche(field_base_type_name(field->type));
			int n = field_total_elements(field->type);

			/* Load column pointer from struct */
			char *field_col_ptr_ref = gen_value_name(ctx);
			buffer_append_fmt(ctx, "  %s = getelementptr %%struct.%s, %%struct.%s* %s, i32 0, i32 %d\n",
			                  field_col_ptr_ref, arch_name, arch_name, struct_ptr, field_idx);
			char *field_col_ptr = gen_value_name(ctx);
			buffer_append_fmt(ctx, "  %s = load %s*, %s** %s\n", field_col_ptr, elem_type, elem_type,
			                  field_col_ptr_ref);

			/* Generate loop to fill field[0..count) with init value */
			char loop_cond_label[64], loop_body_label[64], loop_end_label[64];
			char *loop_ctr_alloca = gen_value_name(ctx);
			snprintf(loop_cond_label, sizeof(loop_cond_label), "loop_cond_%d", ctx->value_counter);
			snprintf(loop_body_label, sizeof(loop_body_label), "loop_body_%d", ctx->value_counter);
			snprintf(loop_end_label, sizeof(loop_end_label), "loop_end_%d", ctx->value_counter);
			ctx->value_counter++;

			/* Allocate loop counter variable on stack */
			emit_alloca(ctx, "  %s = alloca i64\n", loop_ctr_alloca);
			buffer_append_fmt(ctx, "  store i64 0, i64* %s\n", loop_ctr_alloca);

			/* Jump to loop condition */
			buffer_append_fmt(ctx, "  br label %%%s\n", loop_cond_label);

			/* Loop condition block */
			buffer_append_fmt(ctx, "%s:\n", loop_cond_label);
			char *loop_counter = gen_value_name(ctx);
			buffer_append_fmt(ctx, "  %s = load i64, i64* %s\n", loop_counter, loop_ctr_alloca);
			char *cond = gen_value_name(ctx);
			buffer_append_fmt(ctx, "  %s = icmp slt i64 %s, %s\n", cond, loop_counter, count_buf);
			buffer_append_fmt(ctx, "  br i1 %s, label %%%s, label %%%s\n", cond, loop_body_label, loop_end_label);

			/* Loop body */
			buffer_append_fmt(ctx, "%s:\n", loop_body_label);

			/* Compute field pointer for this element */
			char *elem_ptr = gen_value_name(ctx);
			buffer_append_fmt(ctx, "  %s = getelementptr %s, %s* %s, i64 %s\n", elem_ptr, elem_type, elem_type,
			                  field_col_ptr, loop_counter);

			/* Compute init value */
			char init_val_buf[256];
			codegen_expression(ctx, init_value, init_val_buf);

			/* Convert type if needed (e.g., i32 to double) */
			const char *init_type = init_value->resolved_type ? init_value->resolved_type : "int";
			const char *init_llvm_type = llvm_type_from_arche(init_type);

			if (strcmp(init_llvm_type, elem_type) != 0) {
				char *converted = gen_value_name(ctx);
				if ((init_llvm_type[0] == 'i' || init_llvm_type[0] == 'd') &&
				    (elem_type[0] == 'i' || elem_type[0] == 'd')) {
					if (init_llvm_type[0] == 'i' && elem_type[0] == 'd') {
						buffer_append_fmt(ctx, "  %s = sitofp %s %s to %s\n", converted, init_llvm_type, init_val_buf,
						                  elem_type);
					} else if (init_llvm_type[0] == 'd' && elem_type[0] == 'i') {
						buffer_append_fmt(ctx, "  %s = fptosi %s %s to %s\n", converted, init_llvm_type, init_val_buf,
						                  elem_type);
					} else {
						converted = init_val_buf;
					}
				} else {
					converted = init_val_buf;
				}
				buffer_append_fmt(ctx, "  store %s %s, %s* %s\n", elem_type, converted, elem_type, elem_ptr);
			} else {
				buffer_append_fmt(ctx, "  store %s %s, %s* %s\n", elem_type, init_val_buf, elem_type, elem_ptr);
			}

			/* Increment loop counter */
			char *next_counter = gen_value_name(ctx);
			buffer_append_fmt(ctx, "  %s = add i64 %s, 1\n", next_counter, loop_counter);
			buffer_append_fmt(ctx, "  store i64 %s, i64* %s\n", next_counter, loop_ctr_alloca);
			buffer_append_fmt(ctx, "  br label %%%s\n", loop_cond_label);

			/* Loop end */
			buffer_append_fmt(ctx, "%s:\n", loop_end_label);
		}

		/* If we did any field initialization, update the count field */
		if (expr->data.alloc.field_count > 1) {
			char *final_count_gep = gen_value_name(ctx);
			buffer_append_fmt(ctx, "  %s = getelementptr %%struct.%s, %%struct.%s* %s, i32 0, i32 %d\n",
			                  final_count_gep, arch_name, arch_name, struct_ptr, arch->field_count);
			buffer_append_fmt(ctx, "  store i64 %s, i64* %s\n", count_buf, final_count_gep);
		}

		strcpy(result_buf, struct_ptr);
		break;
	}

	case EXPR_STRING: {
		/* Create global constant for string literal */
		char global_name[64];
		snprintf(global_name, sizeof(global_name), "@.str%d", ctx->string_counter++);

		/* Build LLVM global constant declaration (similar to emit_string_global) */
		char llvm_escaped[2048] = "";
		size_t llvm_pos = 0;

		for (int i = 0; i < expr->data.string.length && llvm_pos < sizeof(llvm_escaped) - 10; i++) {
			unsigned char c = (unsigned char)expr->data.string.value[i];
			if (c >= 32 && c < 127 && c != '"' && c != '\\') {
				llvm_escaped[llvm_pos++] = c;
			} else {
				llvm_pos += snprintf(llvm_escaped + llvm_pos, sizeof(llvm_escaped) - llvm_pos, "\\%02X", c);
			}
		}

		char global_decl[4096];
		snprintf(global_decl, sizeof(global_decl), "%s = private unnamed_addr constant [%d x i8] c\"%s\\00\"\n",
		         global_name, expr->data.string.length + 1, llvm_escaped);

		/* Append to globals buffer */
		size_t decl_len = strlen(global_decl);
		if (ctx->globals_pos + decl_len >= ctx->globals_size) {
			ctx->globals_size = (ctx->globals_size + decl_len) * 2;
			ctx->globals_buffer = realloc(ctx->globals_buffer, ctx->globals_size);
		}
		strcpy(ctx->globals_buffer + ctx->globals_pos, global_decl);
		ctx->globals_pos += decl_len;

		/* Get pointer to first element */
		char *res_name = gen_value_name(ctx);
		buffer_append_fmt(ctx, "  %s = getelementptr [%d x i8], [%d x i8]* %s, i32 0, i32 0\n", res_name,
		                  expr->data.string.length + 1, expr->data.string.length + 1, global_name);
		strcpy(result_buf, res_name);
		break;
	}
	}
}

/* ========== BOUNDS CHECK HELPERS ========== */

static int resolve_index_arch(CodegenContext *ctx, Expression *base_expr, Expression *idx_expr,
                              const char **out_arch_name, const char **out_arch_ptr, int *out_count_idx,
                              int *out_idx_is_i64) {
	*out_arch_name = NULL;
	*out_arch_ptr = NULL;
	*out_count_idx = -1;
	*out_idx_is_i64 = 0;

	/* Case 1: base is EXPR_FIELD with archetype backing (e.g., particles.mass[i]) */
	if (base_expr->type == EXPR_FIELD && base_expr->data.field.base->type == EXPR_NAME) {
		const char *var_name = base_expr->data.field.base->data.name.name;
		ValueInfo *vi = find_value(ctx, var_name);
		if (vi && vi->type == 3 && vi->arch_name) {
			ArchetypeDecl *arch = find_archetype_decl(ctx, vi->arch_name);
			if (arch) {
				*out_arch_name = vi->arch_name;
				*out_arch_ptr = vi->llvm_name;
				*out_count_idx = arch->field_count;
			}
		} else {
			/* Try direct archetype name (for static allocations) */
			ArchetypeDecl *arch = find_archetype_decl(ctx, var_name);
			if (arch) {
				static char global_buf[256];
				*out_arch_name = var_name;
				int cap = get_arch_static_capacity(ctx, var_name);
				if (cap > 0)
					snprintf(global_buf, sizeof(global_buf), "@%s", var_name);
				else
					snprintf(global_buf, sizeof(global_buf), "@archetype_%s", var_name);
				*out_arch_ptr = global_buf;
				*out_count_idx = arch->field_count;
			}
		}
	}
	/* Case 2: base is EXPR_NAME with type 4 (column param in sys) */
	else if (base_expr->type == EXPR_NAME) {
		ValueInfo *vi = find_value(ctx, base_expr->data.name.name);
		if (vi && vi->type == 4 && vi->arch_name) {
			ArchetypeDecl *arch = find_archetype_decl(ctx, vi->arch_name);
			if (arch) {
				*out_arch_name = vi->arch_name;
				*out_arch_ptr = "%archetype";
				*out_count_idx = arch->field_count;
			}
		}
	}

	/* Determine if index is i64 */
	if (idx_expr->type == EXPR_NAME) {
		ValueInfo *idx_vi = find_value(ctx, idx_expr->data.name.name);
		if (idx_vi && idx_vi->bit_width == 64) {
			*out_idx_is_i64 = 1;
		}
	}

	return (*out_arch_name != NULL && *out_arch_ptr != NULL && *out_count_idx >= 0);
}

static void emit_bounds_check(CodegenContext *ctx, const char *arch_name, const char *arch_ptr, int count_field_idx,
                              const char *idx_buf, int idx_is_i64) {
	/* If arch_ptr is a dynamic global (pointer-to-pointer), load the struct ptr first.
	   Static globals (@ArchName) are already %struct.X* — no load needed. */
	const char *struct_ptr = arch_ptr;
	char *loaded_ptr = NULL;
	if (strncmp(arch_ptr, "@archetype_", 11) == 0) {
		loaded_ptr = gen_value_name(ctx);
		buffer_append_fmt(ctx, "  %s = load %%struct.%s*, %%struct.%s** %s\n", loaded_ptr, arch_name, arch_name,
		                  arch_ptr);
		struct_ptr = loaded_ptr;
	}

	/* Load count from archetype struct */
	char *count_gep = gen_value_name(ctx);
	buffer_append_fmt(ctx, "  %s = getelementptr %%struct.%s, %%struct.%s* %s, i32 0, i32 %d\n", count_gep, arch_name,
	                  arch_name, struct_ptr, count_field_idx);
	char *count = gen_value_name(ctx);
	buffer_append_fmt(ctx, "  %s = load i64, i64* %s\n", count, count_gep);

	/* Extend index to i64 if needed */
	const char *idx64 = idx_buf;
	if (!idx_is_i64) {
		char *idx64_val = gen_value_name(ctx);
		buffer_append_fmt(ctx, "  %s = sext i32 %s to i64\n", idx64_val, idx_buf);
		idx64 = idx64_val;
	}

	/* Compare index < count */
	char *cmp = gen_value_name(ctx);
	buffer_append_fmt(ctx, "  %s = icmp ult i64 %s, %s\n", cmp, idx64, count);

	/* Generate branch labels */
	int chk_id = ctx->value_counter++;
	char ok_lbl[64], fail_lbl[64];
	snprintf(ok_lbl, sizeof(ok_lbl), "bounds_ok_%d", chk_id);
	snprintf(fail_lbl, sizeof(fail_lbl), "bounds_fail_%d", chk_id);

	buffer_append_fmt(ctx, "  br i1 %s, label %%%s, label %%%s\n\n", cmp, ok_lbl, fail_lbl);

	/* Emit error block */
	buffer_append_fmt(ctx, "%s:\n", fail_lbl);
	buffer_append(ctx,
	              "  call i32 @write(i32 2, i8* getelementptr ([28 x i8], [28 x i8]* @.arche_oob, i32 0, i32 0), i32 "
	              "27)\n");
	buffer_append(ctx, "  call void @abort()\n");
	buffer_append(ctx, "  unreachable\n\n");

	/* Emit ok block label (execution continues here) */
	buffer_append_fmt(ctx, "%s:\n", ok_lbl);
}

/* ========== WHOLE-COLUMN LOOP HELPER ========== */

/* Walk expression tree and pre-compute column base pointers to hoist them out of loop */
static void hoist_column_geps(CodegenContext *ctx, Expression *expr, const char *struct_ptr_val) {
	if (!expr)
		return;

	switch (expr->type) {
	case EXPR_FIELD: {
		Expression *base = expr->data.field.base;
		if (base && base->type == EXPR_NAME) {
			const char *arch_name = base->data.name.name;
			const char *field_name = expr->data.field.field_name;

			/* Find field index */
			ArchetypeDecl *arch = find_archetype_decl(ctx, arch_name);
			if (arch) {
				int field_idx = -1;
				for (int i = 0; i < arch->field_count; i++) {
					if (strcmp(arch->fields[i]->name, field_name) == 0) {
						field_idx = i;
						break;
					}
				}

				if (field_idx >= 0 && arch->fields[field_idx]->kind == FIELD_COLUMN) {
					/* Generate GEP for column base once (loop-invariant) */
					const char *llvm_type = llvm_type_from_arche(field_base_type_name(arch->fields[field_idx]->type));
					int is_static = get_arch_static_capacity(ctx, arch_name) > 0;

					char *col_ptr = gen_value_name(ctx);
					if (is_static) {
						buffer_append_fmt(ctx,
						                  "  %s = getelementptr %%struct.%s, %%struct.%s* %s, i32 0, i32 %d, i64 0\n",
						                  col_ptr, arch_name, arch_name, struct_ptr_val, field_idx);
					} else {
						char *field_gep = gen_value_name(ctx);
						buffer_append_fmt(ctx, "  %s = getelementptr %%struct.%s, %%struct.%s* %s, i32 0, i32 %d\n",
						                  field_gep, arch_name, arch_name, struct_ptr_val, field_idx);
						buffer_append_fmt(ctx, "  %s = load %s*, %s** %s\n", col_ptr, llvm_type, llvm_type, field_gep);
					}
				}
			}
		}
		if (base)
			hoist_column_geps(ctx, base, struct_ptr_val);
		break;
	}
	case EXPR_BINARY:
		hoist_column_geps(ctx, expr->data.binary.left, struct_ptr_val);
		hoist_column_geps(ctx, expr->data.binary.right, struct_ptr_val);
		break;
	case EXPR_UNARY:
		hoist_column_geps(ctx, expr->data.unary.operand, struct_ptr_val);
		break;
	default:
		break;
	}
}

static void emit_whole_column_loop(CodegenContext *ctx, const char *col_ptr, /* SSA reg: scalar* column data */
                                   const char *count,                        /* SSA reg: i64 element count */
                                   const char *scalar_type,                  /* "double" or "i32" */
                                   const char *arche_type,                   /* "float" or "int" */
                                   Expression *rhs,                          /* RHS expression */
                                   int op,                     /* OP_NONE = store, others = load+op+store */
                                   const char *struct_ptr_val) /* struct pointer for hoisting */
{
	/* Hoist column base GEPs before loop to avoid recalculating them */
	if (struct_ptr_val) {
		hoist_column_geps(ctx, rhs, struct_ptr_val);
	}

	/* Align count down to 4-element boundary for vector loop */
	char *count_aligned = gen_value_name(ctx);
	buffer_append_fmt(ctx, "  %s = and i64 %s, -4\n", count_aligned, count);

	/* Vector loop setup */
	char *v_ctr_alloca = gen_value_name(ctx);
	emit_alloca(ctx, "  %s = alloca i64\n", v_ctr_alloca);
	buffer_append_fmt(ctx, "  store i64 0, i64* %s\n", v_ctr_alloca);

	/* Generate label names (without % prefix) */
	char vec_loop_lbl[32], vec_body_lbl[32], scalar_setup_lbl[32];
	char scalar_check_lbl[32], scalar_body_lbl[32], done_lbl[32];
	snprintf(vec_loop_lbl, sizeof(vec_loop_lbl), "vec_loop_%d", ctx->value_counter++);
	snprintf(vec_body_lbl, sizeof(vec_body_lbl), "vec_body_%d", ctx->value_counter++);
	snprintf(scalar_setup_lbl, sizeof(scalar_setup_lbl), "scalar_setup_%d", ctx->value_counter++);
	snprintf(scalar_check_lbl, sizeof(scalar_check_lbl), "scalar_check_%d", ctx->value_counter++);
	snprintf(scalar_body_lbl, sizeof(scalar_body_lbl), "scalar_body_%d", ctx->value_counter++);
	snprintf(done_lbl, sizeof(done_lbl), "done_%d", ctx->value_counter++);

	buffer_append_fmt(ctx, "  br label %%%s\n\n", vec_loop_lbl);

	/* Vector loop header */
	buffer_append_fmt(ctx, "%s:\n", vec_loop_lbl);
	char *vi = gen_value_name(ctx);
	buffer_append_fmt(ctx, "  %s = load i64, i64* %s\n", vi, v_ctr_alloca);
	char *vcond = gen_value_name(ctx);
	buffer_append_fmt(ctx, "  %s = icmp slt i64 %s, %s\n", vcond, vi, count_aligned);
	buffer_append_fmt(ctx, "  br i1 %s, label %%%s, label %%%s\n\n", vcond, vec_body_lbl, scalar_setup_lbl);

	/* Vector loop body */
	buffer_append_fmt(ctx, "%s:\n", vec_body_lbl);
	ctx->vector_lanes = 0; /* Use scalar ops for simplicity (TODO: vectorize) */
	snprintf(ctx->implicit_loop_index, sizeof(ctx->implicit_loop_index), "%s", vi);

	/* Evaluate RHS with implicit loop index set */
	char rhs_buf[256];
	codegen_expression(ctx, rhs, rhs_buf);

	/* Handle compound ops */
	char *compute_result = malloc(256);
	strcpy(compute_result, rhs_buf);
	if (op != OP_NONE) {
		char *loaded = gen_value_name(ctx);
		char *gep = gen_value_name(ctx);
		buffer_append_fmt(ctx, "  %s = getelementptr %s, %s* %s, i64 %s\n", gep, scalar_type, scalar_type, col_ptr, vi);
		buffer_append_fmt(ctx, "  %s = load %s, %s* %s, align 8\n", loaded, scalar_type, scalar_type, gep);

		const char *op_str;
		switch (op) {
		case OP_ADD:
			op_str = "fadd";
			break;
		case OP_SUB:
			op_str = "fsub";
			break;
		case OP_MUL:
			op_str = "fmul";
			break;
		case OP_DIV:
			op_str = "fdiv";
			break;
		default:
			op_str = "fadd";
			break;
		}
		char *op_result = gen_value_name(ctx);
		buffer_append_fmt(ctx, "  %s = %s %s %s, %s\n", op_result, op_str, scalar_type, loaded, rhs_buf);
		strcpy(compute_result, op_result);
	}

	/* Store result */
	char *target_gep = gen_value_name(ctx);
	buffer_append_fmt(ctx, "  %s = getelementptr %s, %s* %s, i64 %s\n", target_gep, scalar_type, scalar_type, col_ptr,
	                  vi);
	/* For vector stores, bitcast pointer to vector type */
	if (ctx->vector_lanes > 0) {
		const char *vec_type = elem_llvm_type(ctx, arche_type);
		char *vec_ptr = gen_value_name(ctx);
		buffer_append_fmt(ctx, "  %s = bitcast %s* %s to %s*\n", vec_ptr, scalar_type, target_gep, vec_type);
		buffer_append_fmt(ctx, "  store %s %s, %s* %s, align 8\n", vec_type, compute_result, vec_type, vec_ptr);
	} else {
		buffer_append_fmt(ctx, "  store %s %s, %s* %s, align 8\n", scalar_type, compute_result, scalar_type,
		                  target_gep);
	}

	/* Loop increment - use 4 for vectorized ops, 1 for scalar */
	char *vi_new = gen_value_name(ctx);
	int increment = ctx->vector_lanes > 0 ? 4 : 1;
	buffer_append_fmt(ctx, "  %s = add i64 %s, %d\n", vi_new, vi, increment);
	buffer_append_fmt(ctx, "  store i64 %s, i64* %s\n", vi_new, v_ctr_alloca);
	buffer_append_fmt(ctx, "  br label %%%s\n\n", vec_loop_lbl);

	/* Scalar loop setup */
	buffer_append_fmt(ctx, "%s:\n", scalar_setup_lbl);
	char *s_ctr_alloca = gen_value_name(ctx);
	emit_alloca(ctx, "  %s = alloca i64\n", s_ctr_alloca);
	buffer_append_fmt(ctx, "  store i64 %s, i64* %s\n", count_aligned, s_ctr_alloca);
	buffer_append_fmt(ctx, "  br label %%%s\n\n", scalar_check_lbl);

	/* Scalar loop check */
	buffer_append_fmt(ctx, "%s:\n", scalar_check_lbl);
	char *si = gen_value_name(ctx);
	buffer_append_fmt(ctx, "  %s = load i64, i64* %s\n", si, s_ctr_alloca);
	char *scond = gen_value_name(ctx);
	buffer_append_fmt(ctx, "  %s = icmp slt i64 %s, %s\n", scond, si, count);
	buffer_append_fmt(ctx, "  br i1 %s, label %%%s, label %%%s\n\n", scond, scalar_body_lbl, done_lbl);

	/* Scalar loop body */
	buffer_append_fmt(ctx, "%s:\n", scalar_body_lbl);
	ctx->vector_lanes = 0;
	snprintf(ctx->implicit_loop_index, sizeof(ctx->implicit_loop_index), "%s", si);

	/* Evaluate RHS with implicit loop index set */
	codegen_expression(ctx, rhs, rhs_buf);

	/* Handle compound ops */
	strcpy(compute_result, rhs_buf);
	if (op != OP_NONE) {
		char *loaded = gen_value_name(ctx);
		char *gep = gen_value_name(ctx);
		buffer_append_fmt(ctx, "  %s = getelementptr %s, %s* %s, i64 %s\n", gep, scalar_type, scalar_type, col_ptr, si);
		buffer_append_fmt(ctx, "  %s = load %s, %s* %s, align 4\n", loaded, scalar_type, scalar_type, gep);

		const char *op_str;
		switch (op) {
		case OP_ADD:
			op_str = "fadd";
			break;
		case OP_SUB:
			op_str = "fsub";
			break;
		case OP_MUL:
			op_str = "fmul";
			break;
		case OP_DIV:
			op_str = "fdiv";
			break;
		default:
			op_str = "fadd";
			break;
		}
		char *op_result = gen_value_name(ctx);
		buffer_append_fmt(ctx, "  %s = %s %s %s, %s\n", op_result, op_str, scalar_type, loaded, rhs_buf);
		strcpy(compute_result, op_result);
	}

	/* Store result */
	target_gep = gen_value_name(ctx);
	buffer_append_fmt(ctx, "  %s = getelementptr %s, %s* %s, i64 %s\n", target_gep, scalar_type, scalar_type, col_ptr,
	                  si);
	buffer_append_fmt(ctx, "  store %s %s, %s* %s, align 4\n", scalar_type, compute_result, scalar_type, target_gep);

	/* Loop increment */
	char *si_new = gen_value_name(ctx);
	buffer_append_fmt(ctx, "  %s = add i64 %s, 1\n", si_new, si);
	buffer_append_fmt(ctx, "  store i64 %s, i64* %s\n", si_new, s_ctr_alloca);
	buffer_append_fmt(ctx, "  br label %%%s\n\n", scalar_check_lbl);

	/* Done */
	buffer_append_fmt(ctx, "%s:\n", done_lbl);
	ctx->implicit_loop_index[0] = '\0';
	ctx->vector_lanes = 0;

	free(compute_result);
}

/* ========== STATEMENT CODEGEN ========== */

static void codegen_statement(CodegenContext *ctx, Statement *stmt) {
	if (!stmt)
		return;

	switch (stmt->type) {
	case STMT_LET: {
		const char *var_name = stmt->data.let_stmt.name;
		char value_buf[256];

		/* Multi-value let: let a, b, c = func(...) with out params */
		if (stmt->data.let_stmt.name_count > 0) {
			Expression *rhs = stmt->data.let_stmt.value;
			if (rhs && rhs->type == EXPR_CALL) {
				char *func_name = NULL;
				if (rhs->data.call.callee && rhs->data.call.callee->type == EXPR_NAME) {
					func_name = rhs->data.call.callee->data.name.name;
				}

				FuncDecl *callee_func = func_name ? find_func_decl(ctx, func_name) : NULL;
				if (callee_func && callee_func->param_count >= 0) {
					/* Collect out param info: size from TypeRef and SSA names */
					int *out_buf_sizes = malloc(callee_func->param_count * sizeof(int));
					char **out_buf_names = malloc(callee_func->param_count * sizeof(char *));
					int out_param_count = 0;

					for (int i = 0; i < callee_func->param_count; i++) {
						if (callee_func->params[i] && callee_func->params[i]->is_out) {
							/* Extract size from param type (e.g., out buf: char[256]) */
							TypeRef *pt = callee_func->params[i]->type;
							int size = 256; /* default */

							if (pt && pt->kind == TYPE_SHAPED_ARRAY && pt->data.shaped_array.element_type &&
							    pt->data.shaped_array.element_type->kind == TYPE_NAME &&
							    strcmp(pt->data.shaped_array.element_type->data.name, "char") == 0) {
								size = pt->data.shaped_array.rank;
							}

							/* Allocate zero-initialized buffer on stack */
							char *buf_name = gen_value_name(ctx);
							emit_alloca(ctx, "  %s = alloca [%d x i8]\n", buf_name, size);
							char *ptr_for_memset = gen_value_name(ctx);
							buffer_append_fmt(ctx, "  %s = bitcast [%d x i8]* %s to i8*\n", ptr_for_memset, size,
							                  buf_name);
							buffer_append_fmt(ctx,
							                  "  call void @llvm.memset.p0i8.i64(i8* %s, i8 0, i64 %d, i1 false)\n",
							                  ptr_for_memset, size);

							out_buf_names[out_param_count] = buf_name;
							out_buf_sizes[out_param_count] = size;
							out_param_count++;
						}
					}

					/* Evaluate arguments */
					char **arg_bufs = malloc(rhs->data.call.arg_count * sizeof(char *));
					for (int i = 0; i < rhs->data.call.arg_count; i++) {
						arg_bufs[i] = malloc(256);
						codegen_expression(ctx, rhs->data.call.args[i], arg_bufs[i]);
					}

					/* Build call args: use out buffers for out params, regular args otherwise */
					char **call_arg_vals = malloc(rhs->data.call.arg_count * sizeof(char *));
					const char **call_arg_types = malloc(rhs->data.call.arg_count * sizeof(const char *));
					int out_idx = 0;

					for (int i = 0; i < rhs->data.call.arg_count; i++) {
						call_arg_vals[i] = malloc(256);

						int is_out =
						    (i < callee_func->param_count && callee_func->params[i] && callee_func->params[i]->is_out);

						if (is_out && out_idx < out_param_count) {
							/* Bitcast [N x i8]* to i8* */
							char *ptr_cast = gen_value_name(ctx);
							buffer_append_fmt(ctx, "  %s = bitcast [%d x i8]* %s to i8*\n", ptr_cast,
							                  out_buf_sizes[out_idx], out_buf_names[out_idx]);
							strcpy(call_arg_vals[i], ptr_cast);
							call_arg_types[i] = "i8*";
							out_idx++;
						} else {
							strcpy(call_arg_vals[i], arg_bufs[i]);
							call_arg_types[i] = "i32";
						}
					}

					/* Emit call */
					char *res_name = gen_value_name(ctx);
					const char *return_type = "i32";
					if (callee_func->return_type && callee_func->return_type->kind == TYPE_NAME &&
					    callee_func->return_type->data.name) {
						return_type = llvm_type_from_arche(callee_func->return_type->data.name);
					}

					buffer_append_fmt(ctx, "  %s = call %s @%s(", res_name, return_type,
					                  func_name ? func_name : "unknown");
					for (int i = 0; i < rhs->data.call.arg_count; i++) {
						buffer_append_fmt(ctx, "%s %s", call_arg_types[i], call_arg_vals[i]);
						if (i < rhs->data.call.arg_count - 1)
							buffer_append(ctx, ", ");
					}
					buffer_append(ctx, ")\n");

					/* Assign returned values to variables */
					out_idx = 0;
					for (int i = 0; i < stmt->data.let_stmt.name_count && stmt->data.let_stmt.names; i++) {
						const char *var_name = stmt->data.let_stmt.names[i];
						if (!var_name || strcmp(var_name, "_") == 0) {
							if (i < out_param_count)
								out_idx++;
							continue;
						}

						if (i < out_param_count) {
							/* Assign out param (zero-init buffer) to variable */
							ValueInfo *vi = malloc(sizeof(ValueInfo));
							vi->name = malloc(strlen(var_name) + 1);
							strcpy(vi->name, var_name);
							vi->llvm_name = malloc(strlen(out_buf_names[out_idx]) + 1);
							strcpy(vi->llvm_name, out_buf_names[out_idx]);
							vi->type = 7; /* char buffer */
							vi->arch_name = NULL;
							vi->string_len = out_buf_sizes[out_idx];
							vi->field_type = "char";
							vi->bit_width = 8;

							if (ctx->scope_count > 0) {
								ValueScope *scope = &ctx->scopes[ctx->scope_count - 1];
								scope->values = realloc(scope->values, (scope->value_count + 1) * sizeof(ValueInfo *));
								scope->values[scope->value_count++] = vi;
							}
							out_idx++;
						} else {
							/* Assign return value to variable */
							char *alloca_name = gen_value_name(ctx);
							emit_alloca(ctx, "  %s = alloca %s\n", alloca_name, return_type);
							buffer_append_fmt(ctx, "  store %s %s, %s* %s\n", return_type, res_name, return_type,
							                  alloca_name);

							ValueInfo *vi = malloc(sizeof(ValueInfo));
							vi->name = malloc(strlen(var_name) + 1);
							strcpy(vi->name, var_name);
							vi->llvm_name = malloc(strlen(alloca_name) + 1);
							strcpy(vi->llvm_name, alloca_name);
							vi->type = 1;
							vi->arch_name = NULL;
							vi->string_len = -1;
							if (return_type[0] == 'd') {
								vi->field_type = "float";
								vi->bit_width = 64;
							} else if (strcmp(return_type, "i64") == 0) {
								vi->field_type = "handle";
								vi->bit_width = 64;
							} else {
								vi->field_type = "int";
								vi->bit_width = 32;
							}

							if (ctx->scope_count > 0) {
								ValueScope *scope = &ctx->scopes[ctx->scope_count - 1];
								scope->values = realloc(scope->values, (scope->value_count + 1) * sizeof(ValueInfo *));
								scope->values[scope->value_count++] = vi;
							}
						}
					}

					/* Cleanup */
					free(out_buf_sizes);
					free(out_buf_names);
					for (int i = 0; i < rhs->data.call.arg_count; i++)
						free(arg_bufs[i]);
					free(arg_bufs);
					for (int i = 0; i < rhs->data.call.arg_count; i++)
						free(call_arg_vals[i]);
					free(call_arg_vals);
					free(call_arg_types);
					break;
				}
			}
			break;
		}

		/* Handle type-annotated declaration without initialization */
		if (stmt->data.let_stmt.type && !stmt->data.let_stmt.value) {
			TypeRef *type = stmt->data.let_stmt.type;

			/* Check for array type */
			if (type->kind == TYPE_ARRAY) {
				char *arr_alloca = gen_value_name(ctx);
				emit_alloca(ctx, "  %s = alloca %%struct.arche_array\n", arr_alloca);

				char *ptr_gep = gen_value_name(ctx);
				buffer_append_fmt(ctx,
				                  "  %s = getelementptr %%struct.arche_array, %%struct.arche_array* %s, i32 0, i32 0\n",
				                  ptr_gep, arr_alloca);
				buffer_append_fmt(ctx, "  store i8* null, i8** %s\n", ptr_gep);

				char *len_gep = gen_value_name(ctx);
				buffer_append_fmt(ctx,
				                  "  %s = getelementptr %%struct.arche_array, %%struct.arche_array* %s, i32 0, i32 1\n",
				                  len_gep, arr_alloca);
				buffer_append_fmt(ctx, "  store i64 0, i64* %s\n", len_gep);

				char *cap_gep = gen_value_name(ctx);
				buffer_append_fmt(ctx,
				                  "  %s = getelementptr %%struct.arche_array, %%struct.arche_array* %s, i32 0, i32 2\n",
				                  cap_gep, arr_alloca);
				buffer_append_fmt(ctx, "  store i64 0, i64* %s\n", cap_gep);

				add_array_value(ctx, var_name, arr_alloca);
			} else if (type->kind == TYPE_SHAPED_ARRAY && type->data.shaped_array.element_type &&
			           type->data.shaped_array.element_type->kind == TYPE_NAME &&
			           strcmp(type->data.shaped_array.element_type->data.name, "char") == 0) {
				/* Stack-allocated char array: let buf: char[256]; */
				int rank = type->data.shaped_array.rank;
				char *alloca_name = gen_value_name(ctx);
				emit_alloca(ctx, "  %s = alloca [%d x i8]\n", alloca_name, rank);

				ValueInfo *vi = malloc(sizeof(ValueInfo));
				vi->name = malloc(strlen(var_name) + 1);
				strcpy(vi->name, var_name);
				vi->llvm_name = malloc(strlen(alloca_name) + 1);
				strcpy(vi->llvm_name, alloca_name);
				vi->type = 7; /* type 7 = char buffer array [N x i8]* */
				vi->arch_name = NULL;
				vi->string_len = rank;
				vi->field_type = "char";
				vi->bit_width = 8;

				if (ctx->scope_count > 0) {
					ValueScope *scope = &ctx->scopes[ctx->scope_count - 1];
					scope->values = realloc(scope->values, (scope->value_count + 1) * sizeof(ValueInfo *));
					scope->values[scope->value_count++] = vi;
				}
			} else {
				/* Scalar type: allocate and zero-initialize */
				const char *alloc_type = "i32";
				if (type->data.name && strcmp(type->data.name, "float") == 0) {
					alloc_type = "double";
				}

				char *alloca_name = gen_value_name(ctx);
				emit_alloca(ctx, "  %s = alloca %s\n", alloca_name, alloc_type);
				buffer_append_fmt(ctx, "  store %s 0, %s* %s\n", alloc_type, alloc_type, alloca_name);

				ValueInfo *vi = malloc(sizeof(ValueInfo));
				vi->name = malloc(strlen(var_name) + 1);
				strcpy(vi->name, var_name);
				vi->llvm_name = malloc(strlen(alloca_name) + 1);
				strcpy(vi->llvm_name, alloca_name);
				vi->type = 1;
				vi->arch_name = NULL;
				vi->string_len = -1;
				vi->field_type = type->data.name ? type->data.name : "int";
				vi->bit_width = strcmp(alloc_type, "double") == 0 ? 64 : 32;

				if (ctx->scope_count > 0) {
					ValueScope *scope = &ctx->scopes[ctx->scope_count - 1];
					scope->values = realloc(scope->values, (scope->value_count + 1) * sizeof(ValueInfo *));
					scope->values[scope->value_count++] = vi;
				}
			}
			break;
		}

		/* Check if the value is a string literal (old style) or array literal (new style from string expansion) */
		int is_string = 0;
		if (stmt->data.let_stmt.value) {
			if (stmt->data.let_stmt.value->type == EXPR_LITERAL &&
			    stmt->data.let_stmt.value->data.literal.lexeme[0] == '"') {
				is_string = 1;
			} else if (stmt->data.let_stmt.value->type == EXPR_STRING) {
				/* EXPR_STRING is also a string */
				is_string = 1;
			} else if (stmt->data.let_stmt.value->type == EXPR_ARRAY_LITERAL) {
				/* Array literal from string expansion */
				is_string = 1;
			}
		}

		/* Check if value is an alloc expression */
		int is_alloc = 0;
		const char *alloc_arch_name = NULL;
		if (stmt->data.let_stmt.value && stmt->data.let_stmt.value->type == EXPR_ALLOC) {
			is_alloc = 1;
			alloc_arch_name = stmt->data.let_stmt.value->data.alloc.archetype_name;
		}

		/* Detect: let row = arch.field[entity] where field is shaped array */
		int is_multidim_slice = 0;
		const char *slice_elem_type = NULL;
		if (stmt->data.let_stmt.value && stmt->data.let_stmt.value->type == EXPR_INDEX &&
		    stmt->data.let_stmt.value->data.index.index_count == 1) {
			slice_elem_type = get_shaped_field_info(ctx, stmt->data.let_stmt.value->data.index.base, NULL);
			if (slice_elem_type)
				is_multidim_slice = 1;
		}

		if (stmt->data.let_stmt.value) {
			codegen_expression(ctx, stmt->data.let_stmt.value, value_buf);
		} else {
			strcpy(value_buf, "0");
		}

		if (is_multidim_slice) {
			/* value_buf is a raw pointer — store as type-6 ValueInfo */
			ValueInfo *vi = malloc(sizeof(ValueInfo));
			vi->name = malloc(strlen(var_name) + 1);
			strcpy(vi->name, var_name);
			vi->llvm_name = malloc(strlen(value_buf) + 1);
			strcpy(vi->llvm_name, value_buf);
			vi->type = 6; /* typed column slice pointer */
			vi->arch_name = NULL;
			vi->string_len = -1;
			vi->field_type = slice_elem_type;
			vi->bit_width = 64;
			if (ctx->scope_count > 0) {
				ValueScope *scope = &ctx->scopes[ctx->scope_count - 1];
				scope->values = realloc(scope->values, (scope->value_count + 1) * sizeof(ValueInfo *));
				scope->values[scope->value_count++] = vi;
			}
		} else if (is_alloc && alloc_arch_name) {
			/* Track as arch pointer (type 3) */
			add_arch_value(ctx, var_name, value_buf, alloc_arch_name);
		} else if (is_string) {
			/* For strings, handle based on type */
			if (stmt->data.let_stmt.value->type == EXPR_ARRAY_LITERAL) {
				/* Array literal: value_buf is already a struct pointer, just use it */
				add_array_value(ctx, var_name, value_buf);
			} else {
				/* String literal (EXPR_LITERAL or EXPR_STRING): store as i8* pointer (type 2) */
				/* Only string literals should be full "Strings"; variables are just char arrays */
				ValueInfo *vi = malloc(sizeof(ValueInfo));
				vi->name = malloc(strlen(var_name) + 1);
				strcpy(vi->name, var_name);
				vi->llvm_name = malloc(strlen(value_buf) + 1);
				strcpy(vi->llvm_name, value_buf);
				vi->type = 2; /* i8* string pointer */
				vi->arch_name = NULL;
				vi->string_len = -1;
				vi->field_type = "char";
				vi->bit_width = 64;

				if (ctx->scope_count > 0) {
					ValueScope *scope = &ctx->scopes[ctx->scope_count - 1];
					scope->values = realloc(scope->values, (scope->value_count + 1) * sizeof(ValueInfo *));
					scope->values[scope->value_count++] = vi;
				} else {
					free(vi->name);
					free(vi->llvm_name);
					free(vi);
				}
			}
		} else {
			/* For scalars (int/float/handle), allocate and store based on type */
			const char *alloc_type = "i32";
			const char *store_type = "i32";
			int bit_width = 32;
			const char *resolved_type = NULL;

			/* Check if RHS is an insert call (returns handle/i64) */
			int is_insert_call = 0;
			const char *insert_archetype = NULL;
			if (stmt->data.let_stmt.value && stmt->data.let_stmt.value->type == EXPR_CALL) {
				const char *func_name = NULL;
				if (stmt->data.let_stmt.value->data.call.callee &&
				    stmt->data.let_stmt.value->data.call.callee->type == EXPR_NAME) {
					func_name = stmt->data.let_stmt.value->data.call.callee->data.name.name;
				}
				if (func_name && strcmp(func_name, "insert") == 0) {
					is_insert_call = 1;
					alloc_type = "i64";
					store_type = "i64";
					bit_width = 64;
					resolved_type = "handle";
					/* Extract archetype from insert's first argument */
					if (stmt->data.let_stmt.value->data.call.arg_count > 0 &&
					    stmt->data.let_stmt.value->data.call.args[0]->type == EXPR_NAME) {
						insert_archetype = stmt->data.let_stmt.value->data.call.args[0]->data.name.name;
					}
				}
			}

			/* Check the resolved type of the value expression */
			if (!is_insert_call && stmt->data.let_stmt.value && stmt->data.let_stmt.value->resolved_type) {
				resolved_type = stmt->data.let_stmt.value->resolved_type;
				if (strcmp(resolved_type, "double") == 0 || strcmp(resolved_type, "float") == 0) {
					alloc_type = "double";
					store_type = "double";
					bit_width = 64;
				} else if (strcmp(resolved_type, "handle") == 0) {
					alloc_type = "i64";
					store_type = "i64";
					bit_width = 64;
				}
			}

			char *alloca_name = gen_value_name(ctx);
			emit_alloca(ctx, "  %s = alloca %s\n", alloca_name, alloc_type);
			buffer_append_fmt(ctx, "  store %s %s, %s* %s\n", store_type, value_buf, store_type, alloca_name);

			ValueInfo *vi = malloc(sizeof(ValueInfo));
			vi->name = malloc(strlen(var_name) + 1);
			strcpy(vi->name, var_name);
			vi->llvm_name = malloc(strlen(alloca_name) + 1);
			strcpy(vi->llvm_name, alloca_name);
			vi->type = 1;
			vi->arch_name = NULL;
			vi->string_len = -1;
			vi->field_type = resolved_type ? resolved_type : "int";
			vi->handle_archetype = NULL;
			if (insert_archetype) {
				vi->handle_archetype = malloc(strlen(insert_archetype) + 1);
				strcpy(vi->handle_archetype, insert_archetype);
			}
			vi->bit_width = bit_width;

			if (ctx->scope_count > 0) {
				ValueScope *scope = &ctx->scopes[ctx->scope_count - 1];
				scope->values = realloc(scope->values, (scope->value_count + 1) * sizeof(ValueInfo *));
				scope->values[scope->value_count++] = vi;
			} else {
				free(vi->name);
				free(vi->llvm_name);
				free(vi);
			}
		}
		break;
	}

	case STMT_ASSIGN: {
		/* Check if this is a whole-column operation (Path A or B) */
		int is_whole_column = 0;

		/* Check Path A: target is EXPR_FIELD of FIELD_COLUMN (p.pos = p.pos + p.vel) */
		if (stmt->data.assign_stmt.target->type == EXPR_FIELD &&
		    stmt->data.assign_stmt.target->data.field.base->type == EXPR_NAME) {
			const char *inst_name = stmt->data.assign_stmt.target->data.field.base->data.name.name;
			ValueInfo *inst = find_value(ctx, inst_name);
			const char *arch_name_direct = NULL;
			if (!inst && find_archetype_decl(ctx, inst_name)) {
				arch_name_direct = inst_name;
			}
			if ((inst && inst->type == 3 && inst->arch_name) || arch_name_direct) {
				const char *arch_check_name = inst ? inst->arch_name : arch_name_direct;
				ArchetypeDecl *arch = find_archetype_decl(ctx, arch_check_name);
				if (arch) {
					const char *fname = stmt->data.assign_stmt.target->data.field.field_name;
					/* Check if fname is a direct field or tuple base */
					int found = 0;
					for (int i = 0; i < arch->field_count; i++) {
						if (strcmp(arch->fields[i]->name, fname) == 0 && arch->fields[i]->kind == FIELD_COLUMN) {
							is_whole_column = 1;
							found = 1;
							break;
						}
					}
					/* Check if fname is a tuple base (e.g., "pos" when fields are "pos_x", "pos_y") */
					if (!found) {
						size_t prefix_len = strlen(fname);
						for (int i = 0; i < arch->field_count; i++) {
							const char *aname = arch->fields[i]->name;
							if (strncmp(aname, fname, prefix_len) == 0 && aname[prefix_len] == '_' &&
							    arch->fields[i]->kind == FIELD_COLUMN) {
								is_whole_column = 1;
								break;
							}
						}
					}
				}
			}
		}

		/* Check Path B: target is EXPR_NAME type-4 (sys parameter) */
		if (stmt->data.assign_stmt.target->type == EXPR_NAME) {
			const char *var_name = stmt->data.assign_stmt.target->data.name.name;
			ValueInfo *val = find_value(ctx, var_name);
			if (val && val->type == 4) {
				is_whole_column = 1;
			}
		}

		/* Only evaluate RHS for scalar operations; whole-column paths evaluate it internally */
		char value_buf[256];
		if (!is_whole_column) {
			codegen_expression(ctx, stmt->data.assign_stmt.value, value_buf);
		}

		/* assignment - find the target variable */
		if (stmt->data.assign_stmt.target->type == EXPR_NAME) {
			const char *var_name = stmt->data.assign_stmt.target->data.name.name;
			ValueInfo *val = find_value(ctx, var_name);
			if (val && val->type == 1) { /* type 1 = i32* pointer */
				int is_float = val->field_type &&
				               (strcmp(val->field_type, "float") == 0 || strcmp(val->field_type, "double") == 0);
				const char *llvm_t = is_float ? "double" : "i32";

				if (stmt->data.assign_stmt.op == OP_NONE) {
					buffer_append_fmt(ctx, "  store %s %s, %s* %s\n", llvm_t, value_buf, llvm_t, val->llvm_name);
				} else {
					/* Compound assignment: load, compute, store */
					char *loaded = gen_value_name(ctx);
					buffer_append_fmt(ctx, "  %s = load %s, %s* %s\n", loaded, llvm_t, llvm_t, val->llvm_name);

					const char *op;
					switch (stmt->data.assign_stmt.op) {
					case OP_ADD:
						op = is_float ? "fadd" : "add";
						break;
					case OP_SUB:
						op = is_float ? "fsub" : "sub";
						break;
					case OP_MUL:
						op = is_float ? "fmul" : "mul";
						break;
					case OP_DIV:
						op = is_float ? "fdiv" : "sdiv";
						break;
					default:
						op = is_float ? "fadd" : "add";
						break;
					}

					char *result = gen_value_name(ctx);
					buffer_append_fmt(ctx, "  %s = %s %s %s, %s\n", result, op, llvm_t, loaded, value_buf);
					buffer_append_fmt(ctx, "  store %s %s, %s* %s\n", llvm_t, result, llvm_t, val->llvm_name);
				}
			}

			/* Path B: target is EXPR_NAME type-4 (sys parameter) */
			if (val && val->type == 4) {
				/* Column parameter: emit whole-column loop */
				const char *arche_type = val->field_type ? val->field_type : "float";
				/* Skip handle columns — cannot use in sys operations */
				if (strcmp(arche_type, "handle") == 0) {
					break;
				}
				const char *scalar_type = llvm_type_from_arche(arche_type);

				/* Get count from archetype struct (stored in %archetype for sys) */
				char *count_gep = gen_value_name(ctx);
				ArchetypeDecl *arch = find_archetype_decl(ctx, val->arch_name);
				if (arch) {
					int count_idx = arch->field_count;
					buffer_append_fmt(ctx,
					                  "  %s = getelementptr %%struct.%s, %%struct.%s* %%archetype, i32 0, i32 %d\n",
					                  count_gep, val->arch_name, val->arch_name, count_idx);
					char *count = gen_value_name(ctx);
					buffer_append_fmt(ctx, "  %s = load i64, i64* %s\n", count, count_gep);

					emit_whole_column_loop(ctx, val->llvm_name, count, scalar_type, arche_type,
					                       stmt->data.assign_stmt.value, stmt->data.assign_stmt.op, "%archetype");
				}
			}
		} else if (stmt->data.assign_stmt.target->type == EXPR_FIELD) {
			/* Path A: target is EXPR_FIELD of FIELD_COLUMN (p.pos = p.pos + p.vel) */
			if (stmt->data.assign_stmt.target->data.field.base->type == EXPR_NAME) {
				const char *inst_name = stmt->data.assign_stmt.target->data.field.base->data.name.name;
				ValueInfo *inst = find_value(ctx, inst_name);
				const char *arch_name_direct = NULL;
				if (!inst && find_archetype_decl(ctx, inst_name)) {
					arch_name_direct = inst_name;
				}

				if ((inst && inst->type == 3 && inst->arch_name) || arch_name_direct) {
					const char *arch_check_name = inst ? inst->arch_name : arch_name_direct;
					char loaded_global[256] = {0};
					ArchetypeDecl *arch = find_archetype_decl(ctx, arch_check_name);
					const char *fname = stmt->data.assign_stmt.target->data.field.field_name;

					if (arch) {
						/* Find field in archetype */
						int field_idx = -1;
						FieldDecl *fdecl = NULL;
						for (int i = 0; i < arch->field_count; i++) {
							if (strcmp(arch->fields[i]->name, fname) == 0) {
								field_idx = i;
								fdecl = arch->fields[i];
								break;
							}
						}

						if (field_idx >= 0 && fdecl && fdecl->kind == FIELD_COLUMN) {
							/* Load column pointer and count from struct */
							const char *llvm_type = llvm_type_from_arche(field_base_type_name(fdecl->type));
							char *field_gep = gen_value_name(ctx);
							const char *struct_ptr_val;
							if (arch_name_direct) {
								/* Load from global — static use @X directly, dynamic load from @archetype_X */
								if (get_arch_static_capacity(ctx, arch_check_name) > 0) {
									snprintf(loaded_global, sizeof(loaded_global), "@%s", arch_check_name);
								} else {
									char *loaded = gen_value_name(ctx);
									buffer_append_fmt(ctx, "  %s = load %%struct.%s*, %%struct.%s** @archetype_%s\n",
									                  loaded, arch_check_name, arch_check_name, arch_check_name);
									strcpy(loaded_global, loaded);
								}
								struct_ptr_val = loaded_global;
							} else {
								struct_ptr_val = inst->llvm_name;
							}
							int is_static = get_arch_static_capacity(ctx, arch_check_name) > 0;
							char *col_ptr = gen_value_name(ctx);
							if (is_static) {
								buffer_append_fmt(
								    ctx, "  %s = getelementptr %%struct.%s, %%struct.%s* %s, i32 0, i32 %d, i64 0\n",
								    col_ptr, arch_check_name, arch_check_name, struct_ptr_val, field_idx);
							} else {
								buffer_append_fmt(
								    ctx, "  %s = getelementptr %%struct.%s, %%struct.%s* %s, i32 0, i32 %d\n",
								    field_gep, arch_check_name, arch_check_name, struct_ptr_val, field_idx);
								buffer_append_fmt(ctx, "  %s = load %s*, %s** %s\n", col_ptr, llvm_type, llvm_type,
								                  field_gep);
							}

							/* Load count from archetype */
							char *count_gep = gen_value_name(ctx);
							int count_idx = arch->field_count;
							buffer_append_fmt(ctx, "  %s = getelementptr %%struct.%s, %%struct.%s* %s, i32 0, i32 %d\n",
							                  count_gep, arch_check_name, arch_check_name, struct_ptr_val, count_idx);
							char *count = gen_value_name(ctx);
							buffer_append_fmt(ctx, "  %s = load i64, i64* %s\n", count, count_gep);

							/* Emit whole-column loop */
							const char *scalar_type = llvm_type_from_arche(field_base_type_name(fdecl->type));
							emit_whole_column_loop(ctx, col_ptr, count, scalar_type, field_base_type_name(fdecl->type),
							                       stmt->data.assign_stmt.value, stmt->data.assign_stmt.op,
							                       struct_ptr_val);
						} else {
							/* Tuple field: emit loop for each component */
							FieldDecl **tuple_components = NULL;
							int tuple_count = 0;
							size_t prefix_len = strlen(fname);

							for (int i = 0; i < arch->field_count; i++) {
								const char *aname = arch->fields[i]->name;
								if (strncmp(aname, fname, prefix_len) == 0 && aname[prefix_len] == '_') {
									tuple_components =
									    realloc(tuple_components, (tuple_count + 1) * sizeof(FieldDecl *));
									tuple_components[tuple_count++] = arch->fields[i];
								}
							}

							for (int t = 0; t < tuple_count; t++) {
								FieldDecl *comp = tuple_components[t];
								int comp_idx = -1;
								for (int i = 0; i < arch->field_count; i++) {
									if (strcmp(arch->fields[i]->name, comp->name) == 0) {
										comp_idx = i;
										break;
									}
								}

								if (comp_idx >= 0 && comp->kind == FIELD_COLUMN) {
									const char *llvm_type = llvm_type_from_arche(comp->type->data.name);
									char *field_gep = gen_value_name(ctx);
									const char *struct_ptr_val;
									char loaded_global_comp[256];
									if (arch_name_direct && !loaded_global[0]) {
										/* Load from global — static use @X directly, dynamic load from @archetype_X */
										if (get_arch_static_capacity(ctx, arch_check_name) > 0) {
											snprintf(loaded_global_comp, sizeof(loaded_global_comp), "@%s",
											         arch_check_name);
										} else {
											char *loaded = gen_value_name(ctx);
											buffer_append_fmt(
											    ctx, "  %s = load %%struct.%s*, %%struct.%s** @archetype_%s\n", loaded,
											    arch_check_name, arch_check_name, arch_check_name);
											strcpy(loaded_global_comp, loaded);
										}
										struct_ptr_val = loaded_global_comp;
									} else if (arch_name_direct) {
										struct_ptr_val = loaded_global;
									} else {
										struct_ptr_val = inst->llvm_name;
									}
									int is_static = get_arch_static_capacity(ctx, arch_check_name) > 0;
									char *col_ptr = gen_value_name(ctx);
									if (is_static) {
										buffer_append_fmt(
										    ctx,
										    "  %s = getelementptr %%struct.%s, %%struct.%s* %s, i32 0, i32 %d, i64 0\n",
										    col_ptr, arch_check_name, arch_check_name, struct_ptr_val, comp_idx);
									} else {
										buffer_append_fmt(
										    ctx, "  %s = getelementptr %%struct.%s, %%struct.%s* %s, i32 0, i32 %d\n",
										    field_gep, arch_check_name, arch_check_name, struct_ptr_val, comp_idx);
										buffer_append_fmt(ctx, "  %s = load %s*, %s** %s\n", col_ptr, llvm_type,
										                  llvm_type, field_gep);
									}

									char *count_gep = gen_value_name(ctx);
									int count_idx = arch->field_count;
									buffer_append_fmt(
									    ctx, "  %s = getelementptr %%struct.%s, %%struct.%s* %s, i32 0, i32 %d\n",
									    count_gep, arch_check_name, arch_check_name, struct_ptr_val, count_idx);
									char *count = gen_value_name(ctx);
									buffer_append_fmt(ctx, "  %s = load i64, i64* %s\n", count, count_gep);

									/* Check if RHS is tuple binary op - if so, expand component names */
									Expression *rhs_expr = stmt->data.assign_stmt.value;
									const char *scalar_type = llvm_type_from_arche(comp->type->data.name);

									if (rhs_expr->type == EXPR_BINARY &&
									    rhs_expr->data.binary.left->type == EXPR_FIELD &&
									    rhs_expr->data.binary.right->type == EXPR_FIELD) {
										const char *suffix = strchr(comp->name, '_') + 1;
										const char *left_base = rhs_expr->data.binary.left->data.field.field_name;
										const char *right_base = rhs_expr->data.binary.right->data.field.field_name;

										/* Create modified RHS with component names */
										char left_comp[256], right_comp[256];
										snprintf(left_comp, sizeof(left_comp), "%s_%s", left_base, suffix);
										snprintf(right_comp, sizeof(right_comp), "%s_%s", right_base, suffix);

										/* Create temporary binary expression with component field names */
										Expression temp_rhs = *rhs_expr;
										Expression temp_left = *rhs_expr->data.binary.left;
										Expression temp_right = *rhs_expr->data.binary.right;
										temp_left.data.field.field_name = left_comp;
										temp_right.data.field.field_name = right_comp;
										temp_rhs.data.binary.left = &temp_left;
										temp_rhs.data.binary.right = &temp_right;
										/* Preserve resolved type for operation */
										temp_rhs.resolved_type = comp->type->data.name;

										emit_whole_column_loop(ctx, col_ptr, count, scalar_type, comp->type->data.name,
										                       &temp_rhs, stmt->data.assign_stmt.op, struct_ptr_val);
									} else if (rhs_expr->type == EXPR_FIELD) {
										/* Check if RHS is tuple field reference - match components by position */
										const char *rhs_base = rhs_expr->data.field.field_name;

										/* Find which component position this is (t) and find RHS component at same
										 * position */
										FieldDecl **rhs_tuple_components = NULL;
										int rhs_tuple_count = 0;
										size_t rhs_prefix_len = strlen(rhs_base);

										for (int i = 0; i < arch->field_count; i++) {
											const char *aname = arch->fields[i]->name;
											if (strncmp(aname, rhs_base, rhs_prefix_len) == 0 &&
											    aname[rhs_prefix_len] == '_') {
												rhs_tuple_components = realloc(
												    rhs_tuple_components, (rhs_tuple_count + 1) * sizeof(FieldDecl *));
												rhs_tuple_components[rhs_tuple_count++] = arch->fields[i];
											}
										}

										if (t < rhs_tuple_count) {
											/* Use RHS component at same position */
											const char *rhs_comp_name = rhs_tuple_components[t]->name;

											/* Create temporary field expression with component field name */
											Expression temp_rhs = *rhs_expr;
											temp_rhs.data.field.field_name = rhs_comp_name;
											/* Preserve resolved type */
											temp_rhs.resolved_type = comp->type->data.name;

											emit_whole_column_loop(ctx, col_ptr, count, scalar_type,
											                       comp->type->data.name, &temp_rhs,
											                       stmt->data.assign_stmt.op, struct_ptr_val);
										}
										if (rhs_tuple_components)
											free(rhs_tuple_components);
									} else {
										emit_whole_column_loop(ctx, col_ptr, count, scalar_type, comp->type->data.name,
										                       stmt->data.assign_stmt.value, stmt->data.assign_stmt.op,
										                       struct_ptr_val);
									}
								}
							}
							if (tuple_components)
								free(tuple_components);
						}
					}
				}
			}
		} else if (stmt->data.assign_stmt.target->type == EXPR_INDEX) {
			/* Field indexing assignment: pos[i] = value or pos[i] += value */
			char base_buf[256], idx_buf[256];
			codegen_expression(ctx, stmt->data.assign_stmt.target->data.index.base, base_buf);
			if (stmt->data.assign_stmt.target->data.index.index_count > 0) {
				codegen_expression(ctx, stmt->data.assign_stmt.target->data.index.indices[0], idx_buf);
			}

			/* Check if target is type-7 char buffer */
			ValueInfo *type7_target = NULL;
			if (stmt->data.assign_stmt.target->data.index.base->type == EXPR_NAME) {
				ValueInfo *vi = find_value(ctx, stmt->data.assign_stmt.target->data.index.base->data.name.name);
				if (vi && vi->type == 7) {
					type7_target = vi;
				}
			}

			if (type7_target && stmt->data.assign_stmt.target->data.index.index_count > 0) {
				/* Type-7 char buffer assignment */
				/* Convert i32 index to i64 for getelementptr */
				char *idx_i64 = gen_value_name(ctx);
				buffer_append_fmt(ctx, "  %s = sext i32 %s to i64\n", idx_i64, idx_buf);

				char *target_addr = gen_value_name(ctx);
				buffer_append_fmt(ctx, "  %s = getelementptr [%d x i8], [%d x i8]* %s, i64 0, i64 %s\n", target_addr,
				                  type7_target->string_len, type7_target->string_len, base_buf, idx_i64);

				/* Truncate i32 value to i8 for storing */
				char *trunc_val = gen_value_name(ctx);
				buffer_append_fmt(ctx, "  %s = trunc i32 %s to i8\n", trunc_val, value_buf);
				buffer_append_fmt(ctx, "  store i8 %s, i8* %s, align 1\n", trunc_val, target_addr);
				break;
			}

			/* Determine element type (scalar for GEP, may be vectorized for load/store) */
			const char *scalar_type = "i32"; /* default */
			const char *arche_type = NULL;

			/* Try to get element type from resolved type info */
			Expression *base_expr = stmt->data.assign_stmt.target->data.index.base;
			if (base_expr->resolved_type) {
				arche_type = base_expr->resolved_type;
				scalar_type = llvm_type_from_arche(arche_type);
			} else if (base_expr->type == EXPR_FIELD) {
				/* Fallback: lookup field type from archetype */
				const char *field_name = base_expr->data.field.field_name;
				ValueInfo *base_val = NULL;

				if (base_expr->data.field.base->type == EXPR_NAME) {
					base_val = find_value(ctx, base_expr->data.field.base->data.name.name);
				}

				if (base_val && base_val->type == 3 && base_val->arch_name) {
					ArchetypeDecl *arch = find_archetype_decl(ctx, base_val->arch_name);
					if (arch) {
						for (int i = 0; i < arch->field_count; i++) {
							if (strcmp(arch->fields[i]->name, field_name) == 0) {
								arche_type = field_base_type_name(arch->fields[i]->type);
								scalar_type = llvm_type_from_arche(arche_type);
								break;
							}
						}
					}
				}
			}

			/* In vector mode, load/store use vector type; GEP uses scalar pointer */
			const char *load_type = arche_type ? elem_llvm_type(ctx, arche_type) : scalar_type;

			/* Bounds check for archetype column accesses */
			const char *bc_arch_name = NULL, *bc_arch_ptr = NULL;
			int bc_count_idx = -1, bc_idx_is_i64 = 0;
			if (stmt->data.assign_stmt.target->data.index.index_count > 0 &&
			    resolve_index_arch(ctx, base_expr, stmt->data.assign_stmt.target->data.index.indices[0], &bc_arch_name,
			                       &bc_arch_ptr, &bc_count_idx, &bc_idx_is_i64)) {
				emit_bounds_check(ctx, bc_arch_name, bc_arch_ptr, bc_count_idx, idx_buf, bc_idx_is_i64);
			}

			/* Ensure index is i64 for getelementptr (simple heuristic: if not from bounds check, assume i32 and
			 * convert) */
			const char *final_idx = idx_buf;
			if (stmt->data.assign_stmt.target->data.index.index_count > 0 && !bc_idx_is_i64) {
				/* Index likely came from a variable or expression, probably i32. Convert to i64. */
				char *idx_i64 = gen_value_name(ctx);
				buffer_append_fmt(ctx, "  %s = sext i32 %s to i64\n", idx_i64, idx_buf);
				final_idx = idx_i64;
			}

			/* Compute target address (always uses scalar pointer) */
			char *target_addr = gen_value_name(ctx);
			buffer_append_fmt(ctx, "  %s = getelementptr %s, %s* %s, i64 %s\n", target_addr, scalar_type, scalar_type,
			                  base_buf, final_idx);

			/* Store or compound operation */
			if (stmt->data.assign_stmt.op == OP_NONE) {
				int align = ctx->vector_lanes > 0 ? 8 : 4;
				buffer_append_fmt(ctx, "  store %s %s, %s* %s, align %d\n", load_type, value_buf, scalar_type,
				                  target_addr, align);
			} else {
				/* Compound assignment: load, compute, store */
				char *loaded = gen_value_name(ctx);
				int align = ctx->vector_lanes > 0 ? 8 : 4;
				buffer_append_fmt(ctx, "  %s = load %s, %s* %s, align %d\n", loaded, load_type, scalar_type,
				                  target_addr, align);

				/* Detect if float type for choosing fadd vs add */
				int is_float = (scalar_type[0] == 'd' || (scalar_type[0] == '<' && strstr(scalar_type, "double")));
				const char *op;
				switch (stmt->data.assign_stmt.op) {
				case OP_ADD:
					op = is_float ? "fadd" : "add";
					break;
				case OP_SUB:
					op = is_float ? "fsub" : "sub";
					break;
				case OP_MUL:
					op = is_float ? "fmul" : "mul";
					break;
				case OP_DIV:
					op = is_float ? "fdiv" : "sdiv";
					break;
				default:
					op = "add";
					break;
				}

				char *result = gen_value_name(ctx);
				buffer_append_fmt(ctx, "  %s = %s %s %s, %s\n", result, op, load_type, loaded, value_buf);
				buffer_append_fmt(ctx, "  store %s %s, %s* %s, align %d\n", load_type, result, scalar_type, target_addr,
				                  align);
			}
		}
		break;
	}

	case STMT_FOR: {
		/* Check for parenthesized for loop: for (init; cond; incr) */
		if (stmt->data.for_stmt.init || stmt->data.for_stmt.increment) {
			/* Parenthesized for loop */
			char *loop_label = gen_value_name(ctx);
			char *body_label = gen_value_name(ctx);
			char *exit_label = gen_value_name(ctx);

			push_value_scope(ctx);

			/* Generate init */
			if (stmt->data.for_stmt.init) {
				codegen_statement(ctx, stmt->data.for_stmt.init);
			}

			/* Push exit label for break statements */
			if (ctx->loop_exit_count >= ctx->loop_exit_capacity) {
				ctx->loop_exit_capacity = (ctx->loop_exit_capacity == 0) ? 8 : ctx->loop_exit_capacity * 2;
				ctx->loop_exit_labels = realloc(ctx->loop_exit_labels, ctx->loop_exit_capacity * sizeof(char *));
			}
			ctx->loop_exit_labels[ctx->loop_exit_count] = exit_label;
			ctx->loop_exit_count++;

			/* Jump to condition check */
			buffer_append_fmt(ctx, "  br label %s\n", loop_label);
			buffer_append_fmt(ctx, "%s:\n", loop_label + 1);

			if (stmt->data.for_stmt.condition) {
				/* Condition-based: evaluate condition */
				char cond_buf[256];
				codegen_expression(ctx, stmt->data.for_stmt.condition, cond_buf);

				/* Truncate i32 to i1 for branch */
				char *cond_i1 = gen_value_name(ctx);
				buffer_append_fmt(ctx, "  %s = trunc i32 %s to i1\n", cond_i1, cond_buf);
				buffer_append_fmt(ctx, "  br i1 %s, label %s, label %s\n", cond_i1, body_label, exit_label);
			} else {
				/* No condition: always branch to body (infinite) */
				buffer_append_fmt(ctx, "  br label %s\n", body_label);
			}

			buffer_append_fmt(ctx, "%s:\n", body_label + 1);

			/* Generate body */
			for (int i = 0; i < stmt->data.for_stmt.body_count; i++) {
				codegen_statement(ctx, stmt->data.for_stmt.body[i]);
			}

			/* Generate increment */
			if (stmt->data.for_stmt.increment) {
				codegen_statement(ctx, stmt->data.for_stmt.increment);
			}

			/* Jump back to condition */
			buffer_append_fmt(ctx, "  br label %s\n", loop_label);
			buffer_append_fmt(ctx, "%s:\n", exit_label + 1);

			/* Pop exit label */
			ctx->loop_exit_count--;
			pop_value_scope(ctx);
			break;
		}

		/* Check if this is a condition-based for (for cond { }) or infinite for (for { })
		 * vs range-based for (for var in iterable { }) */
		if (!stmt->data.for_stmt.var_name) {
			/* Condition-based or infinite for loop */
			char *loop_label = gen_value_name(ctx);
			char *body_label = gen_value_name(ctx);
			char *exit_label = gen_value_name(ctx);

			/* Push exit label for break statements */
			if (ctx->loop_exit_count >= ctx->loop_exit_capacity) {
				ctx->loop_exit_capacity = (ctx->loop_exit_capacity == 0) ? 8 : ctx->loop_exit_capacity * 2;
				ctx->loop_exit_labels = realloc(ctx->loop_exit_labels, ctx->loop_exit_capacity * sizeof(char *));
			}
			ctx->loop_exit_labels[ctx->loop_exit_count] = exit_label;
			ctx->loop_exit_count++;

			buffer_append_fmt(ctx, "  br label %s\n", loop_label);
			buffer_append_fmt(ctx, "%s:\n", loop_label + 1);

			if (stmt->data.for_stmt.condition) {
				/* Condition-based: evaluate condition */
				char cond_buf[256];
				codegen_expression(ctx, stmt->data.for_stmt.condition, cond_buf);

				/* Truncate i32 to i1 for branch */
				char *cond_i1 = gen_value_name(ctx);
				buffer_append_fmt(ctx, "  %s = trunc i32 %s to i1\n", cond_i1, cond_buf);
				buffer_append_fmt(ctx, "  br i1 %s, label %s, label %s\n", cond_i1, body_label, exit_label);
			} else {
				/* Infinite loop: always branch to body */
				buffer_append_fmt(ctx, "  br label %s\n", body_label);
			}

			buffer_append_fmt(ctx, "%s:\n", body_label + 1);

			push_value_scope(ctx);
			for (int i = 0; i < stmt->data.for_stmt.body_count; i++) {
				codegen_statement(ctx, stmt->data.for_stmt.body[i]);
			}
			pop_value_scope(ctx);

			buffer_append_fmt(ctx, "  br label %s\n", loop_label);
			buffer_append_fmt(ctx, "%s:\n", exit_label + 1);

			/* Pop exit label */
			ctx->loop_exit_count--;
			break;
		}

		/* Range-based for loop */
		const char *var_name = stmt->data.for_stmt.var_name;
		char iter_buf[256];

		/* Get the iterable - usually an archetype instance */
		if (stmt->data.for_stmt.iterable->type == EXPR_NAME) {
			const char *iter_name = stmt->data.for_stmt.iterable->data.name.name;
			ValueInfo *iter_val = find_value(ctx, iter_name);

			/* Get count bound */
			char *count_bound = gen_value_name(ctx);
			if (iter_val && (iter_val->type == 3 || iter_val->type == 4) && iter_val->arch_name) {
				/* Load count from archetype's count field */
				ArchetypeDecl *arch = find_archetype_decl(ctx, iter_val->arch_name);
				if (arch) {
					char *count_gep = gen_value_name(ctx);
					/* For type 4 (column pointer in sys), use %archetype; for type 3, use iter_val->llvm_name */
					const char *struct_ptr = (iter_val->type == 4) ? "%archetype" : iter_val->llvm_name;
					buffer_append_fmt(ctx, "  %s = getelementptr %%struct.%s, %%struct.%s* %s, i32 0, i32 %d\n",
					                  count_gep, iter_val->arch_name, iter_val->arch_name, struct_ptr,
					                  arch->field_count);
					buffer_append_fmt(ctx, "  %s = load i64, i64* %s\n", count_bound, count_gep);
				} else {
					/* Fallback: use constant */
					strcpy(count_bound, "10");
				}
			} else {
				/* Fallback: use constant for non-archetype iterables */
				strcpy(count_bound, "10");
			}

			/* Check if we should vectorize this loop (in sys body with column pointer) */
			if (ctx->in_sys && iter_val && iter_val->type == 4 && iter_val->arch_name) {
				/* STRIP-MINED VECTORIZED LOOP: vector + scalar tail */

				/* Compute aligned count: count_aligned = count & -4 (round down to multiple of 4) */
				char *count_aligned = gen_value_name(ctx);
				buffer_append_fmt(ctx, "  %s = and i64 %s, -4\n", count_aligned, count_bound);

				/* Vector loop (step 4) */
				char *v_counter = gen_value_name(ctx);
				emit_alloca(ctx, "  %s = alloca i64\n", v_counter);
				buffer_append_fmt(ctx, "  store i64 0, i64* %s\n", v_counter);

				char *v_loop_label = gen_value_name(ctx);
				char *v_body_label = gen_value_name(ctx);
				char *scalar_loop_label = gen_value_name(ctx);

				buffer_append_fmt(ctx, "  br label %s\n", v_loop_label);
				buffer_append_fmt(ctx, "%s:\n", v_loop_label + 1); /* Vector loop header */

				char *vi = gen_value_name(ctx);
				buffer_append_fmt(ctx, "  %s = load i64, i64* %s\n", vi, v_counter);

				char *v_cond = gen_value_name(ctx);
				buffer_append_fmt(ctx, "  %s = icmp slt i64 %s, %s\n", v_cond, vi, count_aligned);

				buffer_append_fmt(ctx, "  br i1 %s, label %s, label %s\n", v_cond, v_body_label, scalar_loop_label);

				buffer_append_fmt(ctx, "%s:\n", v_body_label + 1); /* Vector body */

				push_value_scope(ctx);
				fprintf(stderr, "DEBUG: Adding loop var %s to vector body scope\n", var_name);
				add_value(ctx, var_name, vi, 0);
				find_value(ctx, var_name)->bit_width = 64;
				ctx->vector_lanes = 4;

				for (int i = 0; i < stmt->data.for_stmt.body_count; i++) {
					codegen_statement(ctx, stmt->data.for_stmt.body[i]);
				}

				ctx->vector_lanes = 0;
				pop_value_scope(ctx);

				/* Increment by 4 */
				char *vi_next = gen_value_name(ctx);
				buffer_append_fmt(ctx, "  %s = add i64 %s, 4\n", vi_next, vi);
				buffer_append_fmt(ctx, "  store i64 %s, i64* %s\n", vi_next, v_counter);

				buffer_append_fmt(ctx, "  br label %s\n", v_loop_label);

				/* Scalar tail loop (step 1) */
				char *s_counter = gen_value_name(ctx);
				buffer_append_fmt(ctx, "%s:\n", scalar_loop_label + 1); /* Scalar loop header */
				emit_alloca(ctx, "  %s = alloca i64\n", s_counter);
				buffer_append_fmt(ctx, "  store i64 %s, i64* %s\n", count_aligned, s_counter);

				char *scalar_body_label = gen_value_name(ctx);
				char *exit_label = gen_value_name(ctx);

				char *si = gen_value_name(ctx);
				buffer_append_fmt(ctx, "  %s = load i64, i64* %s\n", si, s_counter);

				char *s_cond = gen_value_name(ctx);
				buffer_append_fmt(ctx, "  %s = icmp slt i64 %s, %s\n", s_cond, si, count_bound);

				buffer_append_fmt(ctx, "  br i1 %s, label %s, label %s\n", s_cond, scalar_body_label, exit_label);

				buffer_append_fmt(ctx, "%s:\n", scalar_body_label + 1); /* Scalar body */

				push_value_scope(ctx);
				add_value(ctx, var_name, si, 0);
				find_value(ctx, var_name)->bit_width = 64;
				ctx->vector_lanes = 0;

				for (int i = 0; i < stmt->data.for_stmt.body_count; i++) {
					codegen_statement(ctx, stmt->data.for_stmt.body[i]);
				}

				pop_value_scope(ctx);

				/* Increment by 1 */
				char *si_next = gen_value_name(ctx);
				buffer_append_fmt(ctx, "  %s = add i64 %s, 1\n", si_next, si);
				buffer_append_fmt(ctx, "  store i64 %s, i64* %s\n", si_next, s_counter);

				buffer_append_fmt(ctx, "  br label %s\n", scalar_loop_label);

				buffer_append_fmt(ctx, "%s:\n", exit_label + 1); /* Exit label */

			} else {
				/* SCALAR LOOP (original version) */

				char *counter = gen_value_name(ctx);
				emit_alloca(ctx, "  %s = alloca i64\n", counter);
				buffer_append_fmt(ctx, "  store i64 0, i64* %s\n", counter);

				char *loop_label = gen_value_name(ctx);
				char *body_label = gen_value_name(ctx);
				char *exit_label = gen_value_name(ctx);

				buffer_append_fmt(ctx, "  br label %s\n", loop_label);
				buffer_append_fmt(ctx, "%s:\n", loop_label + 1);

				/* Load counter and compare */
				char *cond_val = gen_value_name(ctx);
				buffer_append_fmt(ctx, "  %s = load i64, i64* %s\n", cond_val, counter);

				char *cmp_result = gen_value_name(ctx);
				buffer_append_fmt(ctx, "  %s = icmp slt i64 %s, %s\n", cmp_result, cond_val, count_bound);

				/* Branch to body only if condition is true */
				buffer_append_fmt(ctx, "  br i1 %s, label %s, label %s\n", cmp_result, body_label, exit_label);

				buffer_append_fmt(ctx, "%s:\n", body_label + 1);
				push_value_scope(ctx);
				add_value(ctx, var_name, cond_val, 0);
				find_value(ctx, var_name)->bit_width = 64;

				for (int i = 0; i < stmt->data.for_stmt.body_count; i++) {
					codegen_statement(ctx, stmt->data.for_stmt.body[i]);
				}

				/* Increment counter */
				char *next_val = gen_value_name(ctx);
				buffer_append_fmt(ctx, "  %s = add i64 %s, 1\n", next_val, cond_val);
				buffer_append_fmt(ctx, "  store i64 %s, i64* %s\n", next_val, counter);

				/* Loop back */
				buffer_append_fmt(ctx, "  br label %s\n", loop_label);
				buffer_append_fmt(ctx, "%s:\n", exit_label + 1);

				pop_value_scope(ctx);
			}
		} else {
			/* Fallback for non-name iterables */
			codegen_expression(ctx, stmt->data.for_stmt.iterable, iter_buf);
			char *counter = gen_value_name(ctx);
			emit_alloca(ctx, "  %s = alloca i32\n", counter);
			buffer_append_fmt(ctx, "  store i32 0, i32* %s\n", counter);

			char *loop_label = gen_value_name(ctx);
			char *exit_label = gen_value_name(ctx);

			buffer_append_fmt(ctx, "  br label %s\n", loop_label);
			buffer_append_fmt(ctx, "%s:\n", loop_label + 1); /* Skip the '%' prefix for label def */

			char *cond = gen_value_name(ctx);
			buffer_append_fmt(ctx, "  %s = load i32, i32* %s\n", cond, counter);
			char *cmp = gen_value_name(ctx);
			buffer_append_fmt(ctx, "  %s = icmp slt i32 %s, 10\n", cmp, cond);

			push_value_scope(ctx);
			add_value(ctx, var_name, cond, 0);
			find_value(ctx, var_name)->bit_width = 64;

			for (int i = 0; i < stmt->data.for_stmt.body_count; i++) {
				codegen_statement(ctx, stmt->data.for_stmt.body[i]);
			}

			buffer_append_fmt(ctx, "  %s = add i32 %s, 1\n", gen_value_name(ctx), cond);
			buffer_append_fmt(ctx, "  br i1 %s, label %s, label %s\n", cmp, loop_label, exit_label);
			buffer_append_fmt(ctx, "%s:\n", exit_label + 1); /* Skip the '%' prefix for label def */

			pop_value_scope(ctx);
		}
		break;
	}

	case STMT_IF: {
		char cond_buf[256];
		codegen_expression(ctx, stmt->data.if_stmt.cond, cond_buf);

		/* Truncate i32 comparison to i1 for branch */
		const char *branch_cond = cond_buf;
		if (stmt->data.if_stmt.cond->type == EXPR_BINARY && stmt->data.if_stmt.cond->data.binary.op >= OP_EQ &&
		    stmt->data.if_stmt.cond->data.binary.op <= OP_GTE) {
			char *truncated = gen_value_name(ctx);
			buffer_append_fmt(ctx, "  %s = trunc i32 %s to i1\n", truncated, cond_buf);
			branch_cond = truncated;
		}

		char *then_label = gen_value_name(ctx);
		char *exit_label = gen_value_name(ctx);

		buffer_append_fmt(ctx, "  br i1 %s, label %s, label %s\n", branch_cond, then_label, exit_label);
		buffer_append_fmt(ctx, "%s:\n", then_label + 1);

		for (int i = 0; i < stmt->data.if_stmt.then_count; i++) {
			codegen_statement(ctx, stmt->data.if_stmt.then_body[i]);
		}

		buffer_append_fmt(ctx, "  br label %s\n", exit_label);
		buffer_append_fmt(ctx, "%s:\n", exit_label + 1);
		break;
	}

	case STMT_RUN: {
		/* run system - dispatch to all matching archetypes in world */
		const char *system_name = stmt->data.run_stmt.system_name;

		/* Find the system definition */
		SysDecl *sys = find_sys_decl(ctx, system_name);
		if (!sys) {
			buffer_append_fmt(ctx, "  ; ERROR: undefined system '%s'\n", system_name);
			break;
		}

		int found_any = 0;

		/* Dispatch to all archetypes in the program (world state) */
		for (int ai = 0; ai < ctx->prog->decl_count; ai++) {
			Decl *decl = ctx->prog->decls[ai];
			if (decl->kind != DECL_ARCHETYPE) {
				continue;
			}

			ArchetypeDecl *arch = decl->data.archetype;
			if (!arch) {
				continue;
			}

			/* Check if this archetype matches the system */
			if (!archetype_matches_system(arch, sys)) {
				continue;
			}

			found_any = 1;
			/* Load global archetype pointer and call versioned system function */
			const char *versioned = codegen_get_sys_version(ctx, system_name, arch->name);
			if (versioned) {
				const char *arch_ptr;
				char arch_ptr_buf[256];
				if (get_arch_static_capacity(ctx, arch->name) > 0) {
					snprintf(arch_ptr_buf, sizeof(arch_ptr_buf), "@%s", arch->name);
					arch_ptr = arch_ptr_buf;
				} else {
					char *loaded = gen_value_name(ctx);
					buffer_append_fmt(ctx, "  %s = load %%struct.%s*, %%struct.%s** @archetype_%s\n", loaded,
					                  arch->name, arch->name, arch->name);
					arch_ptr = loaded;
				}
				buffer_append_fmt(ctx, "  call void @%s(%%struct.%s* %s)\n", versioned, arch->name, arch_ptr);
			}
		}

		if (!found_any) {
			buffer_append_fmt(ctx, "  ; WARNING: no matching archetypes for system '%s'\n", system_name);
		}

		break;
	}

	case STMT_EXPR: {
		/* Handle archetype allocation as statement: alloc Particle(5); */
		if (stmt->data.expr_stmt.expr->type == EXPR_ALLOC) {
			const char *arch_name = stmt->data.expr_stmt.expr->data.alloc.archetype_name;
			char expr_buf[256];
			codegen_expression(ctx, stmt->data.expr_stmt.expr, expr_buf);

			/* Store allocated pointer in global variable for this archetype */
			char arch_var[256];
			snprintf(arch_var, sizeof(arch_var), "%%archetype_%s", arch_name);
			buffer_append_fmt(ctx, "  store %%struct.%s* %s, %%struct.%s** @archetype_%s\n", arch_name, expr_buf,
			                  arch_name, arch_name);

			/* Add archetype to scope so sys functions can find it */
			add_arch_value(ctx, arch_name, expr_buf, arch_name);
		} else {
			char expr_buf[256];
			codegen_expression(ctx, stmt->data.expr_stmt.expr, expr_buf);
		}
		break;
	}

	case STMT_FREE: {
		char value_buf[256];
		codegen_expression(ctx, stmt->data.free_stmt.value, value_buf);
		buffer_append_fmt(ctx, "  call void @free(i8* %s)\n", value_buf);
		break;
	}

	case STMT_BREAK: {
		if (ctx->loop_exit_count > 0) {
			char *exit_label = ctx->loop_exit_labels[ctx->loop_exit_count - 1];
			buffer_append_fmt(ctx, "  br label %s\n", exit_label);
			/* Emit unreachable block label so LLVM doesn't complain about empty BB */
			char *unreachable_label = gen_value_name(ctx);
			buffer_append_fmt(ctx, "%s:\n", unreachable_label + 1);
			buffer_append(ctx, "  unreachable\n");
		} else {
			fprintf(stderr, "Error: break statement outside of loop\n");
		}
		break;
	}

	case STMT_RETURN: {
		char value_buf[256];
		codegen_expression(ctx, stmt->data.return_stmt.value, value_buf);
		buffer_append_fmt(ctx, "  ret i32 %s\n", value_buf);
		break;
	}
	}
}

/* ========== DECLARATION CODEGEN ========== */

static void codegen_archetype_decl(CodegenContext *ctx, ArchetypeDecl *arch) {
	int static_cap = get_arch_static_capacity(ctx, arch->name);

	/* Generate struct definition for archetype */
	buffer_append_fmt(ctx, "%%struct.%s = type {\n", arch->name);

	for (int i = 0; i < arch->field_count; i++) {
		const char *base_type = llvm_type_from_arche(field_base_type_name(arch->fields[i]->type));

		if (arch->fields[i]->kind == FIELD_COLUMN) {
			if (static_cap > 0)
				buffer_append_fmt(ctx, "  [%d x %s]", static_cap, base_type);
			else
				buffer_append_fmt(ctx, "  %s*", base_type);
		} else {
			buffer_append_fmt(ctx, "  %s", base_type);
		}

		buffer_append(ctx, ",\n");
	}

	/* count field */
	buffer_append(ctx, "  i64,\n");

	if (static_cap > 0) {
		/* Static: inline free_list array + free_count + gen_counters; no capacity field */
		buffer_append_fmt(ctx, "  [%d x i64],\n", static_cap);
		buffer_append_fmt(ctx, "  i64,\n");
		buffer_append_fmt(ctx, "  [%d x i32]\n", static_cap);
	} else {
		/* Dynamic: capacity + free_list pointer + free_count + gen_counters pointer */
		buffer_append(ctx, "  i64,\n");
		buffer_append(ctx, "  i64*,\n");
		buffer_append(ctx, "  i64,\n");
		buffer_append(ctx, "  i32*\n");
	}

	buffer_append(ctx, "}\n\n");

	/* Emit insert helper function */
	buffer_append_fmt(ctx, "define i64 @arche_insert_%s(%%struct.%s* %%arch", arch->name, arch->name);
	for (int i = 0; i < arch->field_count; i++) {
		if (arch->fields[i]->kind == FIELD_COLUMN) {
			const char *base_type = llvm_type_from_arche(field_base_type_name(arch->fields[i]->type));
			buffer_append_fmt(ctx, ", %s %%f%d", base_type, i);
		}
	}
	buffer_append(ctx, ") {\n");
	buffer_append(ctx, "entry:\n");

	/* Setup allocas for slot and increment flag */
	buffer_append(ctx, "  %slot_var = alloca i64\n");
	buffer_append(ctx, "  %do_incr = alloca i1\n");

	int count_idx = arch->field_count;
	int fl_idx = static_cap > 0 ? arch->field_count + 1 : arch->field_count + 2;
	int fc_idx = static_cap > 0 ? arch->field_count + 2 : arch->field_count + 3;
	int gc_idx = static_cap > 0 ? arch->field_count + 3 : arch->field_count + 4;
	int cap_idx = arch->field_count + 1; /* dynamic only */

	buffer_append_fmt(ctx, "  %%fc_ptr = getelementptr %%struct.%s, %%struct.%s* %%arch, i32 0, i32 %d\n", arch->name,
	                  arch->name, fc_idx);
	buffer_append(ctx, "  %free_count = load i64, i64* %fc_ptr\n");
	buffer_append(ctx, "  %has_free = icmp sgt i64 %free_count, 1\n");
	buffer_append(ctx, "  br i1 %has_free, label %pop_free, label %check_capacity\n\n");

	/* Pop from free_list */
	buffer_append(ctx, "pop_free:\n");
	buffer_append(ctx, "  %new_fc = sub i64 %free_count, 1\n");
	if (static_cap > 0) {
		/* Inline array: 3-index GEP */
		buffer_append_fmt(
		    ctx, "  %%slot_ptr = getelementptr %%struct.%s, %%struct.%s* %%arch, i32 0, i32 %d, i64 %%new_fc\n",
		    arch->name, arch->name, fl_idx);
	} else {
		buffer_append_fmt(ctx, "  %%fl_ptr = getelementptr %%struct.%s, %%struct.%s* %%arch, i32 0, i32 %d\n",
		                  arch->name, arch->name, fl_idx);
		buffer_append(ctx, "  %free_list = load i64*, i64** %fl_ptr\n");
		buffer_append(ctx, "  %slot_ptr = getelementptr i64, i64* %free_list, i64 %new_fc\n");
	}
	buffer_append(ctx, "  %slot = load i64, i64* %slot_ptr\n");
	buffer_append(ctx, "  store i64 %new_fc, i64* %fc_ptr\n");
	buffer_append(ctx, "  store i64 %slot, i64* %slot_var\n");
	buffer_append(ctx, "  store i1 0, i1* %do_incr\n");
	buffer_append(ctx, "  br label %write_fields\n\n");

	/* Check if count is at capacity (fixed budget, no growth) */
	buffer_append(ctx, "check_capacity:\n");
	buffer_append_fmt(ctx, "  %%count_ptr = getelementptr %%struct.%s, %%struct.%s* %%arch, i32 0, i32 %d\n",
	                  arch->name, arch->name, count_idx);
	buffer_append(ctx, "  %count = load i64, i64* %count_ptr\n");
	if (static_cap > 0) {
		buffer_append_fmt(ctx, "  %%at_capacity = icmp sge i64 %%count, %d\n", static_cap);
	} else {
		buffer_append_fmt(ctx, "  %%cap_ptr = getelementptr %%struct.%s, %%struct.%s* %%arch, i32 0, i32 %d\n",
		                  arch->name, arch->name, cap_idx);
		buffer_append(ctx, "  %cap = load i64, i64* %cap_ptr\n");
		buffer_append(ctx, "  %at_capacity = icmp sge i64 %count, %cap\n");
	}
	buffer_append(ctx, "  br i1 %at_capacity, label %overflow, label %use_count\n\n");

	/* Overflow: abort (fixed budget exceeded) */
	buffer_append(ctx, "overflow:\n");
	buffer_append(ctx, "  call void @abort()\n");
	buffer_append(ctx, "  unreachable\n\n");

	/* Use count as slot */
	buffer_append(ctx, "use_count:\n");
	buffer_append(ctx, "  store i64 %count, i64* %slot_var\n");
	buffer_append(ctx, "  store i1 1, i1* %do_incr\n");
	buffer_append(ctx, "  br label %write_fields\n\n");

	/* Write fields block */
	buffer_append(ctx, "write_fields:\n");
	buffer_append(ctx, "  %final_slot = load i64, i64* %slot_var\n");
	int col_idx = 0;
	for (int i = 0; i < arch->field_count; i++) {
		if (arch->fields[i]->kind == FIELD_COLUMN) {
			const char *base_type = llvm_type_from_arche(field_base_type_name(arch->fields[i]->type));
			int col_n = field_total_elements(arch->fields[i]->type);
			const char *flat_slot = "%final_slot";
			char flat_slot_buf[64];
			if (col_n > 1) {
				snprintf(flat_slot_buf, sizeof(flat_slot_buf), "%%flat_slot_%d", col_idx);
				buffer_append_fmt(ctx, "  %s = mul i64 %%final_slot, %d\n", flat_slot_buf, col_n);
				flat_slot = flat_slot_buf;
			}
			if (static_cap > 0) {
				buffer_append_fmt(
				    ctx, "  %%slot%d = getelementptr %%struct.%s, %%struct.%s* %%arch, i32 0, i32 %d, i64 %s\n",
				    col_idx, arch->name, arch->name, i, flat_slot);
			} else {
				buffer_append_fmt(ctx,
				                  "  %%col_pp2_%d = getelementptr %%struct.%s, %%struct.%s* %%arch, i32 0, i32 %d\n",
				                  col_idx, arch->name, arch->name, i);
				buffer_append_fmt(ctx, "  %%col_p2_%d = load %s*, %s** %%col_pp2_%d\n", col_idx, base_type, base_type,
				                  col_idx);
				buffer_append_fmt(ctx, "  %%slot%d = getelementptr %s, %s* %%col_p2_%d, i64 %s\n", col_idx, base_type,
				                  base_type, col_idx, flat_slot);
			}
			buffer_append_fmt(ctx, "  store %s %%f%d, %s* %%slot%d\n", base_type, i, base_type, col_idx);
			col_idx++;
		}
	}

	/* Conditionally increment count */
	buffer_append(ctx, "  %should_incr = load i1, i1* %do_incr\n");
	buffer_append(ctx, "  br i1 %should_incr, label %do_incr_count, label %done\n\n");

	buffer_append(ctx, "do_incr_count:\n");
	buffer_append_fmt(ctx, "  %%count_ptr2 = getelementptr %%struct.%s, %%struct.%s* %%arch, i32 0, i32 %d\n",
	                  arch->name, arch->name, count_idx);
	buffer_append(ctx, "  %curr_count = load i64, i64* %count_ptr2\n");
	buffer_append(ctx, "  %new_count = add i64 %curr_count, 1\n");
	buffer_append(ctx, "  store i64 %new_count, i64* %count_ptr2\n");
	buffer_append(ctx, "  br label %done\n\n");

	buffer_append(ctx, "done:\n");
	/* Load gen_counters[final_slot] */
	if (static_cap > 0) {
		buffer_append_fmt(ctx, "  %%gc_elem = getelementptr %%struct.%s, %%struct.%s* %%arch, i32 0, i32 %d, i64 %%final_slot\n",
		                  arch->name, arch->name, gc_idx);
	} else {
		buffer_append_fmt(ctx, "  %%gc_ptr_field = getelementptr %%struct.%s, %%struct.%s* %%arch, i32 0, i32 %d\n",
		                  arch->name, arch->name, gc_idx);
		buffer_append(ctx, "  %gc_arr = load i32*, i32** %gc_ptr_field\n");
		buffer_append(ctx, "  %gc_elem = getelementptr i32, i32* %gc_arr, i64 %final_slot\n");
	}
	buffer_append(ctx, "  %gen_i32 = load i32, i32* %gc_elem\n");
	buffer_append(ctx, "  %gen_i64 = zext i32 %gen_i32 to i64\n");
	buffer_append(ctx, "  %gen_shifted = shl i64 %gen_i64, 32\n");
	buffer_append(ctx, "  %slot_i32 = trunc i64 %final_slot to i32\n");
	buffer_append(ctx, "  %slot_i64 = zext i32 %slot_i32 to i64\n");
	buffer_append(ctx, "  %handle = or i64 %slot_i64, %gen_shifted\n");
	buffer_append(ctx, "  ret i64 %handle\n");
	buffer_append(ctx, "}\n\n");

	/* Emit delete helper function */
	buffer_append_fmt(ctx, "define void @arche_delete_%s(%%struct.%s* %%arch, i64 %%handle) {\n", arch->name, arch->name);
	buffer_append(ctx, "entry:\n");

	/* Unpack slot and generation from handle */
	buffer_append(ctx, "  %slot_i32 = trunc i64 %handle to i32\n");
	buffer_append(ctx, "  %slot = zext i32 %slot_i32 to i64\n");
	buffer_append(ctx, "  %hgen_raw = lshr i64 %handle, 32\n");
	buffer_append(ctx, "  %hgen = trunc i64 %hgen_raw to i32\n");

	/* Load gen_counters[slot] */
	if (static_cap > 0) {
		buffer_append_fmt(ctx, "  %%gc_elem = getelementptr %%struct.%s, %%struct.%s* %%arch, i32 0, i32 %d, i64 %%slot\n",
		                  arch->name, arch->name, gc_idx);
	} else {
		buffer_append_fmt(ctx, "  %%gc_ptr_field = getelementptr %%struct.%s, %%struct.%s* %%arch, i32 0, i32 %d\n",
		                  arch->name, arch->name, gc_idx);
		buffer_append(ctx, "  %gc_arr = load i32*, i32** %gc_ptr_field\n");
		buffer_append(ctx, "  %gc_elem = getelementptr i32, i32* %gc_arr, i64 %slot\n");
	}
	buffer_append(ctx, "  %stored_gen = load i32, i32* %gc_elem\n");

	/* Validate generation */
	buffer_append(ctx, "  %gen_ok = icmp eq i32 %hgen, %stored_gen\n");
	buffer_append(ctx, "  br i1 %gen_ok, label %valid, label %stale\n\n");

	buffer_append(ctx, "stale:\n");
	buffer_append(ctx, "  call void @abort()\n");
	buffer_append(ctx, "  unreachable\n\n");

	buffer_append(ctx, "valid:\n");
	/* Increment generation */
	buffer_append(ctx, "  %new_gen = add i32 %stored_gen, 1\n");
	buffer_append(ctx, "  store i32 %new_gen, i32* %gc_elem\n");

	/* Load free_count */
	buffer_append_fmt(ctx, "  %%fc_ptr = getelementptr %%struct.%s, %%struct.%s* %%arch, i32 0, i32 %d\n", arch->name,
	                  arch->name, fc_idx);
	buffer_append(ctx, "  %free_count = load i64, i64* %fc_ptr\n");

	/* Push slot to free_list[free_count] */
	if (static_cap > 0) {
		buffer_append_fmt(
		    ctx, "  %%slot_ptr = getelementptr %%struct.%s, %%struct.%s* %%arch, i32 0, i32 %d, i64 %%free_count\n",
		    arch->name, arch->name, fl_idx);
	} else {
		buffer_append_fmt(ctx, "  %%fl_ptr = getelementptr %%struct.%s, %%struct.%s* %%arch, i32 0, i32 %d\n",
		                  arch->name, arch->name, fl_idx);
		buffer_append(ctx, "  %free_list = load i64*, i64** %fl_ptr\n");
		buffer_append(ctx, "  %slot_ptr = getelementptr i64, i64* %free_list, i64 %free_count\n");
	}
	buffer_append(ctx, "  store i64 %slot, i64* %slot_ptr\n");

	/* Increment free_count */
	buffer_append(ctx, "  %new_fc = add i64 %free_count, 1\n");
	buffer_append(ctx, "  store i64 %new_fc, i64* %fc_ptr\n");

	buffer_append(ctx, "  ret void\n");
	buffer_append(ctx, "}\n\n");

	/* Emit dealloc helper function */
	buffer_append_fmt(ctx, "define void @arche_dealloc_%s(%%struct.%s* %%arch) {\n", arch->name, arch->name);
	buffer_append(ctx, "entry:\n");
	if (static_cap > 0) {
		/* Static global — nothing to free */
	} else {
		buffer_append(ctx, "  %arch_i8 = bitcast %struct.");
		buffer_append_fmt(ctx, "%s* %%arch to i8*\n", arch->name);
		buffer_append(ctx, "  call void @free(i8* %arch_i8)\n");
	}
	buffer_append(ctx, "  ret void\n");
	buffer_append(ctx, "}\n\n");

	/* Emit global variable for this archetype (after struct type is defined) */
	if (static_cap > 0) {
		buffer_append_fmt(ctx, "@%s = global %%struct.%s zeroinitializer\n\n", arch->name, arch->name);
	} else {
		buffer_append_fmt(ctx, "@archetype_%s = global %%struct.%s* null\n\n", arch->name, arch->name);
	}
}

static void codegen_static_decl(CodegenContext *ctx, StaticDecl *alloc) {
	/* Register the allocation for initialization in main() */
	if (ctx->alloc_count >= ctx->alloc_capacity) {
		ctx->alloc_capacity = (ctx->alloc_capacity == 0) ? 16 : ctx->alloc_capacity * 2;
		ctx->top_level_allocs = realloc(ctx->top_level_allocs, ctx->alloc_capacity * sizeof(ctx->top_level_allocs[0]));
	}
	ctx->top_level_allocs[ctx->alloc_count++] = alloc;
}

/* Generate allocation initialization code (for use in main/init) */
static void codegen_emit_alloc_init(CodegenContext *ctx, StaticDecl *alloc) {
	const char *arch_name = alloc->archetype_name;

	/* Get capacity from first field_value */
	char capacity_buf[256] = "256";
	if (alloc->field_count > 0) {
		codegen_expression(ctx, alloc->field_values[0], capacity_buf);
	}

	/* Get init_length from second parameter; default based on whether init block exists */
	char length_buf[256];
	if (alloc->init_length) {
		codegen_expression(ctx, alloc->init_length, length_buf);
	} else if (alloc->field_count > 1) {
		strcpy(length_buf, capacity_buf);
	} else {
		strcpy(length_buf, "0");
	}

	/* Find archetype declaration */
	ArchetypeDecl *arch = find_archetype_decl(ctx, arch_name);
	if (!arch) {
		return;
	}

	int static_cap = get_arch_static_capacity(ctx, arch_name);

	if (static_cap > 0) {
		/* Static global — no malloc. @arch_name is %struct.arch_name* in BSS.
		   Just initialize count and (optionally) field values. */
		int count_idx = arch->field_count;
		int fc_idx = arch->field_count + 2; /* after count + inline free_list array */

		char *count_gep = gen_value_name(ctx);
		buffer_append_fmt(ctx, "  %s = getelementptr %%struct.%s, %%struct.%s* @%s, i32 0, i32 %d\n", count_gep,
		                  arch_name, arch_name, arch_name, count_idx);
		buffer_append_fmt(ctx, "  store i64 %s, i64* %s\n", length_buf, count_gep);

		char *fc_gep = gen_value_name(ctx);
		buffer_append_fmt(ctx, "  %s = getelementptr %%struct.%s, %%struct.%s* @%s, i32 0, i32 %d\n", fc_gep, arch_name,
		                  arch_name, arch_name, fc_idx);
		buffer_append(ctx, "  store i64 1, i64* ");
		buffer_append_fmt(ctx, "%s\n", fc_gep);

		/* Field initialization loops */
		for (int init_idx = 1; init_idx < alloc->field_count; init_idx++) {
			const char *field_name = alloc->field_names[init_idx];
			Expression *init_value = alloc->field_values[init_idx];

			int field_idx = -1;
			for (int i = 0; i < arch->field_count; i++) {
				if (strcmp(arch->fields[i]->name, field_name) == 0) {
					field_idx = i;
					break;
				}
			}
			if (field_idx == -1)
				continue;

			FieldDecl *field = arch->fields[field_idx];
			if (field->kind != FIELD_COLUMN)
				continue;

			const char *elem_type = llvm_type_from_arche(field_base_type_name(field->type));

			char loop_cond_label[64], loop_body_label[64], loop_end_label[64];
			char *loop_ctr_alloca = gen_value_name(ctx);
			snprintf(loop_cond_label, sizeof(loop_cond_label), "loop_cond_%d", ctx->value_counter);
			snprintf(loop_body_label, sizeof(loop_body_label), "loop_body_%d", ctx->value_counter);
			snprintf(loop_end_label, sizeof(loop_end_label), "loop_end_%d", ctx->value_counter);
			ctx->value_counter++;

			emit_alloca(ctx, "  %s = alloca i64\n", loop_ctr_alloca);
			buffer_append_fmt(ctx, "  store i64 0, i64* %s\n", loop_ctr_alloca);
			buffer_append_fmt(ctx, "  br label %%%s\n", loop_cond_label);

			buffer_append_fmt(ctx, "%s:\n", loop_cond_label);
			char *loop_counter = gen_value_name(ctx);
			buffer_append_fmt(ctx, "  %s = load i64, i64* %s\n", loop_counter, loop_ctr_alloca);
			char *cond = gen_value_name(ctx);
			buffer_append_fmt(ctx, "  %s = icmp slt i64 %s, %s\n", cond, loop_counter, length_buf);
			buffer_append_fmt(ctx, "  br i1 %s, label %%%s, label %%%s\n", cond, loop_body_label, loop_end_label);

			buffer_append_fmt(ctx, "%s:\n", loop_body_label);

			/* 3-index GEP into inline column array */
			char *elem_ptr = gen_value_name(ctx);
			buffer_append_fmt(ctx, "  %s = getelementptr %%struct.%s, %%struct.%s* @%s, i32 0, i32 %d, i64 %s\n",
			                  elem_ptr, arch_name, arch_name, arch_name, field_idx, loop_counter);

			char init_val_buf[256];
			codegen_expression(ctx, init_value, init_val_buf);

			const char *init_type = init_value->resolved_type ? init_value->resolved_type : "int";
			const char *init_llvm_type = llvm_type_from_arche(init_type);

			if (strcmp(init_llvm_type, elem_type) != 0) {
				char *converted = gen_value_name(ctx);
				if (init_llvm_type[0] == 'i' && elem_type[0] == 'd') {
					buffer_append_fmt(ctx, "  %s = sitofp %s %s to %s\n", converted, init_llvm_type, init_val_buf,
					                  elem_type);
				} else if (init_llvm_type[0] == 'd' && elem_type[0] == 'i') {
					buffer_append_fmt(ctx, "  %s = fptosi %s %s to %s\n", converted, init_llvm_type, init_val_buf,
					                  elem_type);
				} else {
					converted = init_val_buf;
				}
				buffer_append_fmt(ctx, "  store %s %s, %s* %s\n", elem_type, converted, elem_type, elem_ptr);
			} else {
				buffer_append_fmt(ctx, "  store %s %s, %s* %s\n", elem_type, init_val_buf, elem_type, elem_ptr);
			}

			char *next_counter = gen_value_name(ctx);
			buffer_append_fmt(ctx, "  %s = add i64 %s, 1\n", next_counter, loop_counter);
			buffer_append_fmt(ctx, "  store i64 %s, i64* %s\n", next_counter, loop_ctr_alloca);
			buffer_append_fmt(ctx, "  br label %%%s\n", loop_cond_label);

			buffer_append_fmt(ctx, "%s:\n", loop_end_label);
		}

		if (alloc->field_count > 1) {
			char *final_count_gep = gen_value_name(ctx);
			buffer_append_fmt(ctx, "  %s = getelementptr %%struct.%s, %%struct.%s* @%s, i32 0, i32 %d\n",
			                  final_count_gep, arch_name, arch_name, arch_name, count_idx);
			buffer_append_fmt(ctx, "  store i64 %s, i64* %s\n", length_buf, final_count_gep);
		}
		return;
	}

	/* Dynamic path: single malloc + layout setup */

	/* Struct layout: [pointers...][count][capacity][free_list*][free_count] */
	int struct_sz_bytes = (arch->field_count + 4) * 8;

	int bytes_per_row = 0;
	for (int i = 0; i < arch->field_count; i++) {
		if (arch->fields[i]->kind == FIELD_COLUMN) {
			const char *elem_type = llvm_type_from_arche(field_base_type_name(arch->fields[i]->type));
			int n = field_total_elements(arch->fields[i]->type);
			bytes_per_row += ((elem_type[0] == 'd') ? 8 : 4) * n;
		}
	}
	int total_bytes_per_row = bytes_per_row + 8;

	char *total_bytes = gen_value_name(ctx);
	char *data_bytes = gen_value_name(ctx);
	buffer_append_fmt(ctx, "  %s = mul i64 %s, %d\n", data_bytes, capacity_buf, total_bytes_per_row);
	buffer_append_fmt(ctx, "  %s = add i64 %d, %s\n", total_bytes, struct_sz_bytes, data_bytes);

	char *raw_ptr = gen_value_name(ctx);
	buffer_append_fmt(ctx, "  %s = call i8* @malloc(i64 %s)\n", raw_ptr, total_bytes);

	char *struct_ptr = gen_value_name(ctx);
	buffer_append_fmt(ctx, "  %s = bitcast i8* %s to %%struct.%s*\n", struct_ptr, raw_ptr, arch_name);

	buffer_append_fmt(ctx, "  store %%struct.%s* %s, %%struct.%s** @archetype_%s\n", arch_name, struct_ptr, arch_name,
	                  arch_name);

	char *count_gep = gen_value_name(ctx);
	buffer_append_fmt(ctx, "  %s = getelementptr %%struct.%s, %%struct.%s* %s, i32 0, i32 %d\n", count_gep, arch_name,
	                  arch_name, struct_ptr, arch->field_count);
	buffer_append_fmt(ctx, "  store i64 %s, i64* %s\n", length_buf, count_gep);

	char *cap_gep = gen_value_name(ctx);
	buffer_append_fmt(ctx, "  %s = getelementptr %%struct.%s, %%struct.%s* %s, i32 0, i32 %d\n", cap_gep, arch_name,
	                  arch_name, struct_ptr, arch->field_count + 1);
	buffer_append_fmt(ctx, "  store i64 %s, i64* %s\n", capacity_buf, cap_gep);

	char *fc_gep = gen_value_name(ctx);
	buffer_append_fmt(ctx, "  %s = getelementptr %%struct.%s, %%struct.%s* %s, i32 0, i32 %d\n", fc_gep, arch_name,
	                  arch_name, struct_ptr, arch->field_count + 3);
	buffer_append_fmt(ctx, "  store i64 0, i64* %s\n", fc_gep);

	int col_offset = 0;
	for (int i = 0; i < arch->field_count; i++) {
		if (arch->fields[i]->kind == FIELD_COLUMN) {
			const char *elem_type = llvm_type_from_arche(field_base_type_name(arch->fields[i]->type));

			char *col_gep = gen_value_name(ctx);
			buffer_append_fmt(ctx, "  %s = getelementptr %%struct.%s, %%struct.%s* %s, i32 0, i32 %d\n", col_gep,
			                  arch_name, arch_name, struct_ptr, i);

			char *col_offset_llvm = gen_value_name(ctx);
			buffer_append_fmt(ctx, "  %s = mul i64 %s, %d\n", col_offset_llvm, capacity_buf, col_offset);

			char *col_data_base = gen_value_name(ctx);
			buffer_append_fmt(ctx, "  %s = getelementptr i8, i8* %s, i64 %d\n", col_data_base, raw_ptr,
			                  struct_sz_bytes);

			char *col_data = gen_value_name(ctx);
			buffer_append_fmt(ctx, "  %s = getelementptr i8, i8* %s, i64 %s\n", col_data, col_data_base,
			                  col_offset_llvm);

			char *col_ptr = gen_value_name(ctx);
			buffer_append_fmt(ctx, "  %s = bitcast i8* %s to %s*\n", col_ptr, col_data, elem_type);

			buffer_append_fmt(ctx, "  store %s* %s, %s** %s\n", elem_type, col_ptr, elem_type, col_gep);

			int n = field_total_elements(arch->fields[i]->type);
			int elem_size = ((elem_type[0] == 'd') ? 8 : 4) * n;
			col_offset += elem_size;
		}
	}

	char *fl_gep = gen_value_name(ctx);
	buffer_append_fmt(ctx, "  %s = getelementptr %%struct.%s, %%struct.%s* %s, i32 0, i32 %d\n", fl_gep, arch_name,
	                  arch_name, struct_ptr, arch->field_count + 2);

	char *fl_offset = gen_value_name(ctx);
	buffer_append_fmt(ctx, "  %s = mul i64 %s, %d\n", fl_offset, capacity_buf, bytes_per_row);
	char *fl_data = gen_value_name(ctx);
	buffer_append_fmt(ctx, "  %s = getelementptr i8, i8* %s, i64 %s\n", fl_data, raw_ptr, fl_offset);
	char *fl_add_header = gen_value_name(ctx);
	buffer_append_fmt(ctx, "  %s = getelementptr i8, i8* %s, i64 %d\n", fl_add_header, fl_data, struct_sz_bytes);

	char *fl_ptr = gen_value_name(ctx);
	buffer_append_fmt(ctx, "  %s = bitcast i8* %s to i64*\n", fl_ptr, fl_add_header);

	buffer_append_fmt(ctx, "  store i64* %s, i64** %s\n", fl_ptr, fl_gep);

	for (int init_idx = 1; init_idx < alloc->field_count; init_idx++) {
		const char *field_name = alloc->field_names[init_idx];
		Expression *init_value = alloc->field_values[init_idx];

		int field_idx = -1;
		for (int i = 0; i < arch->field_count; i++) {
			if (strcmp(arch->fields[i]->name, field_name) == 0) {
				field_idx = i;
				break;
			}
		}
		if (field_idx == -1)
			continue;

		FieldDecl *field = arch->fields[field_idx];
		if (field->kind != FIELD_COLUMN)
			continue;

		const char *elem_type = llvm_type_from_arche(field_base_type_name(field->type));

		char *field_col_ptr_ref = gen_value_name(ctx);
		buffer_append_fmt(ctx, "  %s = getelementptr %%struct.%s, %%struct.%s* %s, i32 0, i32 %d\n", field_col_ptr_ref,
		                  arch_name, arch_name, struct_ptr, field_idx);
		char *field_col_ptr = gen_value_name(ctx);
		buffer_append_fmt(ctx, "  %s = load %s*, %s** %s\n", field_col_ptr, elem_type, elem_type, field_col_ptr_ref);

		char loop_cond_label[64], loop_body_label[64], loop_end_label[64];
		char *loop_ctr_alloca = gen_value_name(ctx);
		snprintf(loop_cond_label, sizeof(loop_cond_label), "loop_cond_%d", ctx->value_counter);
		snprintf(loop_body_label, sizeof(loop_body_label), "loop_body_%d", ctx->value_counter);
		snprintf(loop_end_label, sizeof(loop_end_label), "loop_end_%d", ctx->value_counter);
		ctx->value_counter++;

		emit_alloca(ctx, "  %s = alloca i64\n", loop_ctr_alloca);
		buffer_append_fmt(ctx, "  store i64 0, i64* %s\n", loop_ctr_alloca);
		buffer_append_fmt(ctx, "  br label %%%s\n", loop_cond_label);

		buffer_append_fmt(ctx, "%s:\n", loop_cond_label);
		char *loop_counter = gen_value_name(ctx);
		buffer_append_fmt(ctx, "  %s = load i64, i64* %s\n", loop_counter, loop_ctr_alloca);
		char *cond = gen_value_name(ctx);
		buffer_append_fmt(ctx, "  %s = icmp slt i64 %s, %s\n", cond, loop_counter, length_buf);
		buffer_append_fmt(ctx, "  br i1 %s, label %%%s, label %%%s\n", cond, loop_body_label, loop_end_label);

		buffer_append_fmt(ctx, "%s:\n", loop_body_label);

		char *elem_ptr = gen_value_name(ctx);
		buffer_append_fmt(ctx, "  %s = getelementptr %s, %s* %s, i64 %s\n", elem_ptr, elem_type, elem_type,
		                  field_col_ptr, loop_counter);

		char init_val_buf[256];
		codegen_expression(ctx, init_value, init_val_buf);

		const char *init_type = init_value->resolved_type ? init_value->resolved_type : "int";
		const char *init_llvm_type = llvm_type_from_arche(init_type);

		if (strcmp(init_llvm_type, elem_type) != 0) {
			char *converted = gen_value_name(ctx);
			if (init_llvm_type[0] == 'i' && elem_type[0] == 'd') {
				buffer_append_fmt(ctx, "  %s = sitofp %s %s to %s\n", converted, init_llvm_type, init_val_buf,
				                  elem_type);
			} else if (init_llvm_type[0] == 'd' && elem_type[0] == 'i') {
				buffer_append_fmt(ctx, "  %s = fptosi %s %s to %s\n", converted, init_llvm_type, init_val_buf,
				                  elem_type);
			} else {
				converted = init_val_buf;
			}
			buffer_append_fmt(ctx, "  store %s %s, %s* %s\n", elem_type, converted, elem_type, elem_ptr);
		} else {
			buffer_append_fmt(ctx, "  store %s %s, %s* %s\n", elem_type, init_val_buf, elem_type, elem_ptr);
		}

		char *next_counter = gen_value_name(ctx);
		buffer_append_fmt(ctx, "  %s = add i64 %s, 1\n", next_counter, loop_counter);
		buffer_append_fmt(ctx, "  store i64 %s, i64* %s\n", next_counter, loop_ctr_alloca);
		buffer_append_fmt(ctx, "  br label %%%s\n", loop_cond_label);

		buffer_append_fmt(ctx, "%s:\n", loop_end_label);
	}

	if (alloc->field_count > 1) {
		char *final_count_gep = gen_value_name(ctx);
		buffer_append_fmt(ctx, "  %s = getelementptr %%struct.%s, %%struct.%s* %s, i32 0, i32 %d\n", final_count_gep,
		                  arch_name, arch_name, struct_ptr, arch->field_count);
		buffer_append_fmt(ctx, "  store i64 %s, i64* %s\n", length_buf, final_count_gep);
	}
}

typedef struct {
	char *saved_output_buffer;
	size_t saved_buffer_size;
	size_t saved_buffer_pos;
} FunctionBodyState;

static FunctionBodyState begin_function_body(CodegenContext *ctx) {
	FunctionBodyState state = {
	    .saved_output_buffer = ctx->output_buffer,
	    .saved_buffer_size = ctx->buffer_size,
	    .saved_buffer_pos = ctx->buffer_pos,
	};
	ctx->output_buffer = malloc(4096);
	ctx->buffer_size = 4096;
	ctx->buffer_pos = 0;
	ctx->output_buffer[0] = '\0';

	if (!ctx->alloca_buffer) {
		ctx->alloca_buf_size = 1024;
		ctx->alloca_buffer = malloc(ctx->alloca_buf_size);
	}
	ctx->alloca_buf_pos = 0;
	ctx->alloca_buffer[0] = '\0';
	ctx->hoisting_allocas = 1;
	return state;
}

static void end_function_body(CodegenContext *ctx, FunctionBodyState state) {
	ctx->hoisting_allocas = 0;
	char *body_buf = ctx->output_buffer;

	ctx->output_buffer = state.saved_output_buffer;
	ctx->buffer_size = state.saved_buffer_size;
	ctx->buffer_pos = state.saved_buffer_pos;

	if (ctx->alloca_buf_pos > 0)
		buffer_append(ctx, ctx->alloca_buffer);
	buffer_append(ctx, body_buf);
	free(body_buf);
}

static void codegen_func_decl(CodegenContext *ctx, FuncDecl *func) {
	/* For extern funcs, emit declare stub */
	if (func->is_extern) {
		const char *return_type = llvm_type_from_arche(func->return_type ? func->return_type->data.name : "int");
		buffer_append_fmt(ctx, "declare %s @%s(", return_type, func->name);
		for (int i = 0; i < func->param_count; i++) {
			TypeRef *param_type = func->params[i]->type;
			const char *type_name = (param_type && param_type->kind == TYPE_NAME) ? param_type->data.name : "int";
			const char *base_type = llvm_type_from_arche(type_name);

			/* Check if type is char[] (i8*) or an archetype (struct*) */
			if (param_type && param_type->kind == TYPE_ARRAY) {
				buffer_append_fmt(ctx, "i8*"); /* C ABI: T[] = raw ptr */
			} else if (find_archetype_decl(ctx, type_name)) {
				buffer_append_fmt(ctx, "%%struct.%s*", type_name);
			} else {
				buffer_append(ctx, base_type);
			}

			if (i < func->param_count - 1) {
				buffer_append(ctx, ", ");
			}
		}
		buffer_append(ctx, ")\n");
		return;
	}

	/* Generate function definition */
	const char *return_type = llvm_type_from_arche(func->return_type ? func->return_type->data.name : "int");

	buffer_append_fmt(ctx, "define %s @%s(", return_type, func->name);

	for (int i = 0; i < func->param_count; i++) {
		TypeRef *param_type = func->params[i]->type;
		const char *type_name = (param_type && param_type->kind == TYPE_NAME) ? param_type->data.name : "int";
		const char *llvm_type = llvm_type_from_arche(type_name);

		/* Check if type is char[] (i8*) */
		if (param_type && param_type->kind == TYPE_ARRAY) {
			buffer_append_fmt(ctx, "i8* %%arg%d", i);
		} else {
			buffer_append_fmt(ctx, "%s %%arg%d", llvm_type, i);
		}
		if (i < func->param_count - 1) {
			buffer_append(ctx, ", ");
		}
	}
	buffer_append(ctx, ") {\n");
	buffer_append(ctx, "entry:\n");

	FunctionBodyState fbs_func = begin_function_body(ctx);
	push_value_scope(ctx);

	/* Add parameters to scope */
	for (int i = 0; i < func->param_count; i++) {
		char param_name[32];
		snprintf(param_name, sizeof(param_name), "%%arg%d", i);

		/* For char[] params, mark as i8* (type 6) */
		int param_type = 0;
		if (func->params[i]->type && func->params[i]->type->kind == TYPE_ARRAY) {
			param_type = 6; /* i8* parameter */
		}

		add_value(ctx, func->params[i]->name, param_name, param_type);
	}

	/* Generate function body */
	for (int i = 0; i < func->statement_count; i++) {
		codegen_statement(ctx, func->statements[i]);
	}

	pop_value_scope(ctx);

	/* Generate appropriate return value */
	const char *ret_value = "0";
	if (strcmp(return_type, "double") == 0) {
		ret_value = "0.0";
	}
	buffer_append_fmt(ctx, "  ret %s %s\n", return_type, ret_value);
	end_function_body(ctx, fbs_func);
	buffer_append(ctx, "}\n\n");
}

static void codegen_proc_decl(CodegenContext *ctx, ProcDecl *proc) {
	/* For extern procs, emit declare stub */
	if (proc->is_extern) {
		/* exit() returns void; other functions return i32 */
		int is_void_func = strcmp(proc->name, "exit") == 0;
		buffer_append_fmt(ctx, "declare %s @%s(", is_void_func ? "void" : "i32", proc->name);
		for (int i = 0; i < proc->param_count; i++) {
			TypeRef *param_type = proc->params[i]->type;
			const char *type_name = (param_type && param_type->kind == TYPE_NAME) ? param_type->data.name : "int";
			const char *base_type = llvm_type_from_arche(type_name);

			/* Check if type is char[] (i8*) or an archetype (struct*) */
			if (param_type && param_type->kind == TYPE_ARRAY) {
				buffer_append_fmt(ctx, "i8*"); /* C ABI: T[] = raw ptr */
			} else if (find_archetype_decl(ctx, type_name)) {
				buffer_append_fmt(ctx, "%%struct.%s*", type_name);
			} else {
				buffer_append(ctx, base_type);
			}

			if (i < proc->param_count - 1) {
				buffer_append(ctx, ", ");
			}
		}
		/* Add variadic marker for known variadic functions */
		if (strcmp(proc->name, "sprintf") == 0 || strcmp(proc->name, "printf") == 0) {
			buffer_append(ctx, ", ...");
		}
		buffer_append(ctx, ")\n");
		return;
	}

	/* Generate procedure - rename user main to main_user to allow our wrapper */
	int is_user_main = (strcmp(proc->name, "main") == 0);
	const char *proc_name = is_user_main ? "main_user" : proc->name;
	buffer_append_fmt(ctx, "define void @%s(", proc_name);

	/* Emit parameter types and names */
	for (int i = 0; i < proc->param_count; i++) {
		TypeRef *param_type = proc->params[i]->type;
		const char *type_name = (param_type && param_type->kind == TYPE_NAME) ? param_type->data.name : "int";
		const char *base_type = llvm_type_from_arche(type_name);

		/* Check if type is char[] (struct*) or an archetype (struct*) */
		if (param_type && param_type->kind == TYPE_ARRAY) {
			buffer_append_fmt(ctx, "%%struct.arche_array* %%arg%d", i);
		} else if (find_archetype_decl(ctx, type_name)) {
			buffer_append_fmt(ctx, "%%struct.%s* %%arg%d", type_name, i);
		} else {
			buffer_append_fmt(ctx, "%s %%arg%d", base_type, i);
		}

		if (i < proc->param_count - 1) {
			buffer_append(ctx, ", ");
		}
	}

	buffer_append(ctx, ") {\n");
	buffer_append(ctx, "entry:\n");

	FunctionBodyState fbs_proc = begin_function_body(ctx);
	push_value_scope(ctx);

	/* Register parameters in scope */
	for (int i = 0; i < proc->param_count; i++) {
		char param_llvm[32];
		snprintf(param_llvm, sizeof(param_llvm), "%%arg%d", i);
		TypeRef *param_type = proc->params[i]->type;
		const char *type_name = (param_type && param_type->kind == TYPE_NAME) ? param_type->data.name : "int";

		/* If param type is an archetype, track it as arch pointer (type 3) */
		if (find_archetype_decl(ctx, type_name)) {
			add_arch_value(ctx, proc->params[i]->name, param_llvm, type_name);
		} else if (param_type && param_type->kind == TYPE_ARRAY) {
			/* T[] is a struct pointer (type 5) */
			add_array_value(ctx, proc->params[i]->name, param_llvm);
		} else {
			/* Default to i32 for Int, Float, etc. */
			add_value(ctx, proc->params[i]->name, param_llvm, 0);
		}
	}

	for (int i = 0; i < proc->statement_count; i++) {
		codegen_statement(ctx, proc->statements[i]);
	}

	pop_value_scope(ctx);

	buffer_append(ctx, "  ret void\n");
	end_function_body(ctx, fbs_proc);
	buffer_append(ctx, "}\n\n");
}

static void codegen_sys_version(CodegenContext *ctx, SysDecl *sys, const char *arch_name) {
	/* Generate a single versioned system function for a specific archetype */
	char versioned_name[512];
	snprintf(versioned_name, sizeof(versioned_name), "%s_%s", sys->name, arch_name);

	/* Register this version in the mapping */
	codegen_register_sys_version(ctx, sys->name, arch_name);

	/* Generate function signature */
	buffer_append_fmt(ctx, "define void @%s(%%struct.%s* %%archetype) #0 {\n", versioned_name, arch_name);
	buffer_append(ctx, "entry:\n");

	FunctionBodyState fbs_sys = begin_function_body(ctx);
	push_value_scope(ctx);

	/* Bind field parameters to their column pointers */
	ArchetypeDecl *arch = find_archetype_decl(ctx, arch_name);
	if (arch) {
		for (int p = 0; p < sys->param_count; p++) {
			const char *param_name = sys->params[p]->name;

			/* Find this field in the archetype */
			for (int f = 0; f < arch->field_count; f++) {
				if (strcmp(arch->fields[f]->name, param_name) == 0) {
					const char *elem_type = llvm_type_from_arche(field_base_type_name(arch->fields[f]->type));

					if (arch->fields[f]->kind == FIELD_COLUMN) {
						/* Column pointer: for static use 3-index GEP to [0], for dynamic load pointer */
						int is_static = get_arch_static_capacity(ctx, arch_name) > 0;
						char *field_ptr = gen_value_name(ctx);
						if (is_static) {
							buffer_append_fmt(
							    ctx,
							    "  %s = getelementptr %%struct.%s, %%struct.%s* %%archetype, i32 0, i32 %d, i64 0\n",
							    field_ptr, arch_name, arch_name, f);
						} else {
							char *field_gep = gen_value_name(ctx);
							buffer_append_fmt(
							    ctx, "  %s = getelementptr %%struct.%s, %%struct.%s* %%archetype, i32 0, i32 %d\n",
							    field_gep, arch_name, arch_name, f);
							buffer_append_fmt(ctx, "  %s = load %s*, %s** %s\n", field_ptr, elem_type, elem_type,
							                  field_gep);
						}

						/* Add to scope as a column pointer (type 4) */
						add_value(ctx, param_name, field_ptr, 4); /* type 4 = column pointer */
						/* Also track the archetype and element type for vectorization detection */
						ValueInfo *col_val = find_value(ctx, param_name);
						if (col_val) {
							col_val->arch_name = malloc(strlen(arch_name) + 1);
							strcpy(col_val->arch_name, arch_name);
							col_val->field_type = field_base_type_name(arch->fields[f]->type);
						}
					} else {
						/* Meta field: load the scalar value */
						char *field_gep = gen_value_name(ctx);
						buffer_append_fmt(ctx,
						                  "  %s = getelementptr %%struct.%s, %%struct.%s* %%archetype, i32 0, i32 %d\n",
						                  field_gep, arch_name, arch_name, f);

						char *field_val = gen_value_name(ctx);
						buffer_append_fmt(ctx, "  %s = load %s, %s* %s\n", field_val, elem_type, elem_type, field_gep);

						/* Add to scope */
						add_value(ctx, param_name, field_val, 0); /* type 0 = scalar */
					}
					break;
				}
			}
		}
	}

	ctx->in_sys = 1;
	for (int i = 0; i < sys->statement_count; i++) {
		codegen_statement(ctx, sys->statements[i]);
	}
	ctx->in_sys = 0;

	pop_value_scope(ctx);

	buffer_append(ctx, "  ret void\n");
	end_function_body(ctx, fbs_sys);
	buffer_append(ctx, "}\n\n");
}

static void codegen_sys_decl(CodegenContext *ctx, SysDecl *sys) {
	/* Generate system function versions for ALL matching archetypes */
	/* Collect all archetypes that have the required fields */
	const char *matching_archs[256];
	int matching_count = 0;

	if (sys->param_count > 0 && sys->params[0] && sys->params[0]->name) {
		/* Find all archetypes that have this field */
		for (int d = 0; d < ctx->prog->decl_count; d++) {
			Decl *decl = ctx->prog->decls[d];
			if (decl->kind == DECL_ARCHETYPE) {
				ArchetypeDecl *arch = decl->data.archetype;

				/* Check if archetype has ALL required fields */
				int has_all_fields = 1;
				for (int p = 0; p < sys->param_count; p++) {
					int found_field = 0;
					for (int f = 0; f < arch->field_count; f++) {
						if (strcmp(arch->fields[f]->name, sys->params[p]->name) == 0) {
							found_field = 1;
							break;
						}
					}
					if (!found_field) {
						has_all_fields = 0;
						break;
					}
				}

				if (has_all_fields && matching_count < 256) {
					matching_archs[matching_count++] = arch->name;
				}
			}
		}
	}

	/* Generate a function version for each matching archetype */
	for (int i = 0; i < matching_count; i++) {
		codegen_sys_version(ctx, sys, matching_archs[i]);
	}
}

/* ========== PUBLIC API ========== */

CodegenContext *codegen_create(Program *prog, SemanticContext *sem_ctx) {
	CodegenContext *ctx = malloc(sizeof(CodegenContext));
	ctx->prog = prog;
	ctx->sem_ctx = sem_ctx;
	ctx->scopes = NULL;
	ctx->scope_count = 0;
	ctx->value_counter = 0;
	ctx->string_counter = 0;
	ctx->buffer_size = 8192;
	ctx->output_buffer = malloc(ctx->buffer_size);
	ctx->buffer_pos = 0;
	ctx->globals_size = 4096;
	ctx->globals_buffer = malloc(ctx->globals_size);
	ctx->globals_pos = 0;
	ctx->vector_lanes = 0;
	ctx->in_sys = 0;
	ctx->implicit_loop_index[0] = '\0'; /* Initialize to empty (not in loop) */
	ctx->loop_exit_labels = NULL;
	ctx->loop_exit_count = 0;
	ctx->loop_exit_capacity = 0;
	ctx->sys_versions = NULL;
	ctx->sys_version_count = 0;
	ctx->sys_version_capacity = 0;
	ctx->top_level_allocs = NULL;
	ctx->alloc_count = 0;
	ctx->alloc_capacity = 0;
	ctx->alloca_buffer = NULL;
	ctx->alloca_buf_size = 0;
	ctx->alloca_buf_pos = 0;
	ctx->hoisting_allocas = 0;
	return ctx;
}

void codegen_generate(CodegenContext *ctx, FILE *output) {
	/* Preamble: declare external functions */
	buffer_append(ctx, "; Target datalayout and triple would go here\n");
	buffer_append(ctx,
	              "target datalayout = \"e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128\"\n");
	buffer_append(ctx, "target triple = \"x86_64-unknown-linux-gnu\"\n\n");

	/* Declare custom types as opaque structures */
	buffer_append(ctx, "; Type definitions\n");
	buffer_append(ctx, "%struct.Vec3 = type { double, double, double }\n");
	buffer_append(ctx, "%struct.arche_array = type { i8*, i64, i64 }\n\n");

	/* External C library function declarations */
	buffer_append(ctx, "declare i8* @malloc(i32)\n");
	buffer_append(ctx, "declare i8* @calloc(i64, i64)\n");
	buffer_append(ctx, "declare void @free(i8*)\n");
	buffer_append(ctx, "declare void @abort()\n\n");

	/* Global error message for bounds check failures */
	buffer_append(
	    ctx,
	    "@.arche_oob = private unnamed_addr constant [28 x i8] c\"arche: index out of bounds\\0A\\00\", align 1\n\n");

	/* Generate code for all declarations (this will populate globals_buffer with string constants) */
	int has_init_proc = 0;
	for (int i = 0; i < ctx->prog->decl_count; i++) {
		Decl *decl = ctx->prog->decls[i];

		switch (decl->kind) {
		case DECL_ARCHETYPE:
			codegen_archetype_decl(ctx, decl->data.archetype);
			break;
		case DECL_STATIC:
			codegen_static_decl(ctx, decl->data.alloc);
			break;
		case DECL_FUNC:
			codegen_func_decl(ctx, decl->data.func);
			break;
		case DECL_PROC:
			if (strcmp(decl->data.proc->name, "init") == 0) {
				has_init_proc = 1;
			}
			codegen_proc_decl(ctx, decl->data.proc);
			break;
		case DECL_SYS:
			codegen_sys_decl(ctx, decl->data.sys);
			break;
		}
	}

	/* Emit global constants (strings, etc.) */
	if (ctx->globals_pos > 0) {
		buffer_append(ctx, "\n; Global constants\n");
		char temp[ctx->globals_pos + 1];
		strcpy(temp, ctx->globals_buffer);
		buffer_append(ctx, temp);
		buffer_append(ctx, "\n");
	}

	/* Generate main entry point (always needed for initialization) */
	int has_main_proc = 0;
	for (int i = 0; i < ctx->prog->decl_count; i++) {
		if (ctx->prog->decls[i]->kind == DECL_PROC && strcmp(ctx->prog->decls[i]->data.proc->name, "main") == 0) {
			has_main_proc = 1;
			break;
		}
	}

	buffer_append(ctx, "\ndefine i32 @main() {\n");
	buffer_append(ctx, "entry:\n");

	/* Emit allocation initialization code (always, regardless of user main) */
	for (int i = 0; i < ctx->alloc_count; i++) {
		codegen_emit_alloc_init(ctx, ctx->top_level_allocs[i]);
	}

	if (has_init_proc) {
		buffer_append(ctx, "  call void @init()\n");
	}

	if (has_main_proc) {
		buffer_append(ctx, "  call void @main_user()\n");
	}

	buffer_append(ctx, "  ret i32 0\n");
	buffer_append(ctx, "}\n");

	/* Print for double values */
	buffer_append(ctx, "\ndefine i32 @print_double(double %val) {\n");
	buffer_append(ctx, "entry:\n");
	buffer_append(ctx, "  %fmt = getelementptr [3 x i8], [3 x i8]* @.print_fmt_double, i32 0, i32 0\n");
	buffer_append(ctx, "  %res = call i32 (i8*, ...) @printf(i8* %fmt, double %val)\n");
	buffer_append(ctx, "  ret i32 %res\n");
	buffer_append(ctx, "}\n");

	buffer_append(ctx, "\ndefine i32 @print_int(i32 %val) {\n");
	buffer_append(ctx, "entry:\n");
	buffer_append(ctx, "  %fmt = getelementptr [3 x i8], [3 x i8]* @.print_fmt_int, i32 0, i32 0\n");
	buffer_append(ctx, "  %res = call i32 (i8*, ...) @printf(i8* %fmt, i32 %val)\n");
	buffer_append(ctx, "  ret i32 %res\n");
	buffer_append(ctx, "}\n");

	/* Global format strings for printing */
	buffer_append(ctx, "\n@.print_fmt_double = private unnamed_addr constant [3 x i8] c\"%g\\00\", align 1\n");
	buffer_append(ctx, "@.print_fmt_int = private unnamed_addr constant [3 x i8] c\"%d\\00\", align 1\n");

	/* Emit AVX2 function attributes */
	buffer_append(ctx, "\nattributes #0 = { \"target-features\"=\"+avx2,+avx\" }\n");

	/* Output the generated IR */
	fprintf(output, "%s", ctx->output_buffer);
}

void codegen_free(CodegenContext *ctx) {
	if (!ctx)
		return;

	while (ctx->scope_count > 0) {
		pop_value_scope(ctx);
	}
	free(ctx->scopes);
	free(ctx->output_buffer);
	free(ctx->globals_buffer);
	free(ctx->alloca_buffer);
	free(ctx->sys_versions);
	free(ctx->loop_exit_labels);
	free(ctx->top_level_allocs);
	free(ctx);
}
