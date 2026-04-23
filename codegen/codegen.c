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
} ValueInfo;

typedef struct {
	ValueInfo **values;
	int value_count;
} ValueScope;

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
};

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

static const char *llvm_type_from_arche(const char *arche_type) {
	if (!arche_type)
		return "i32"; /* default to int */

	/* Lowercase and capitalized variants */
	if (strcmp(arche_type, "float") == 0 || strcmp(arche_type, "Float") == 0)
		return "double";
	if (strcmp(arche_type, "int") == 0 || strcmp(arche_type, "Int") == 0)
		return "i32";
	if (strcmp(arche_type, "char") == 0 || strcmp(arche_type, "Char") == 0)
		return "i8";
	if (strcmp(arche_type, "void") == 0 || strcmp(arche_type, "Void") == 0)
		return "void";

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
		for (int j = 0; j < scope->value_count; j++) {
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

/* ========== EXPRESSION CODEGEN ========== */

static void codegen_expression(CodegenContext *ctx, Expression *expr, char *result_buf) {
	if (!expr) {
		strcpy(result_buf, "0");
		return;
	}

	switch (expr->type) {
	case EXPR_LITERAL: {
		const char *lex = expr->data.literal.lexeme;

		/* Check if it's a string literal (starts with ") */
		if (lex[0] == '"') {
			char *global_name = emit_string_global(ctx, lex);
			size_t str_len = strlen(lex) - 2; /* excluding quotes */

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
			} else {
				strcpy(result_buf, val->llvm_name);
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

		/* Use semantic resolved type if available (check for both source and LLVM type names) */
		if (expr->resolved_type &&
		    (strcmp(expr->resolved_type, "float") == 0 || strcmp(expr->resolved_type, "Float") == 0 ||
		     strcmp(expr->resolved_type, "double") == 0)) {
			is_float = 1;
		} else if (expr->data.binary.left->resolved_type &&
		           (strcmp(expr->data.binary.left->resolved_type, "float") == 0 ||
		            strcmp(expr->data.binary.left->resolved_type, "Float") == 0 ||
		            strcmp(expr->data.binary.left->resolved_type, "double") == 0)) {
			is_float = 1;
		} else if (expr->data.binary.right->resolved_type &&
		           (strcmp(expr->data.binary.right->resolved_type, "float") == 0 ||
		            strcmp(expr->data.binary.right->resolved_type, "Float") == 0 ||
		            strcmp(expr->data.binary.right->resolved_type, "double") == 0)) {
			is_float = 1;
		} else {
			/* Fallback: Try to infer float type from operands */
			if (strchr(left_buf, '.') != NULL || strchr(right_buf, '.') != NULL) {
				is_float = 1;
			}

			/* If one operand is a parameter (%argN) and the other is a literal integer, treat as double */
			if ((strstr(left_buf, "%arg") && right_buf[0] >= '0' && right_buf[0] <= '9' && !strchr(right_buf, '.')) ||
			    (strstr(right_buf, "%arg") && left_buf[0] >= '0' && left_buf[0] <= '9' && !strchr(left_buf, '.'))) {
				is_float = 1;
				/* Convert literals to float representation */
				if (right_buf[0] >= '0' && right_buf[0] <= '9' && !strchr(right_buf, '.')) {
					char temp[256];
					snprintf(temp, sizeof(temp), "%s.0", right_buf);
					strcpy(right_buf, temp);
				}
				if (left_buf[0] >= '0' && left_buf[0] <= '9' && !strchr(left_buf, '.')) {
					char temp[256];
					snprintf(temp, sizeof(temp), "%s.0", left_buf);
					strcpy(left_buf, temp);
				}
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
			op = "eq";
			break;
		case OP_NEQ:
			op = "ne";
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

		if (expr->data.binary.op >= OP_EQ && expr->data.binary.op <= OP_GTE) {
			/* comparison returns i1 */
			const char *cmp_type = is_float ? "double" : "i32";
			if (is_float) {
				buffer_append_fmt(ctx, "  %s = fcmp %s %s %s, %s\n", res_name, op, cmp_type, left_buf, right_buf);
			} else {
				buffer_append_fmt(ctx, "  %s = icmp %s %s %s, %s\n", res_name, op, cmp_type, left_buf, right_buf);
			}
		} else {
			/* arithmetic */
			buffer_append_fmt(ctx, "  %s = %s %s %s, %s\n", res_name, op, type, left_buf, right_buf);
		}

		strcpy(result_buf, res_name);
		if (expr->data.binary.left->type == EXPR_NAME) {
			add_value(ctx, expr->data.binary.left->data.name.name, res_name, 0);
		}
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

		/* Check if base is an arch pointer (type 3) */
		ValueInfo *base_val = NULL;
		if (expr->data.field.base->type == EXPR_NAME) {
			base_val = find_value(ctx, expr->data.field.base->data.name.name);
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
					const char *llvm_type = llvm_type_from_arche(fdecl->type->data.name);
					char *gep = gen_value_name(ctx);

					/* GEP to field */
					buffer_append_fmt(ctx, "  %s = getelementptr %%struct.%s, %%struct.%s* %s, i32 0, i32 %d\n", gep,
					                  base_val->arch_name, base_val->arch_name, base_buf, field_idx);

					if (fdecl->kind == FIELD_META) {
						/* Load the value */
						char *loaded = gen_value_name(ctx);
						buffer_append_fmt(ctx, "  %s = load %s, %s* %s\n", loaded, llvm_type, llvm_type, gep);
						strcpy(result_buf, loaded);
					} else {
						/* Col field: load the pointer */
						char *ptr_val = gen_value_name(ctx);
						buffer_append_fmt(ctx, "  %s = load %s*, %s** %s\n", ptr_val, llvm_type, llvm_type, gep);

						/* If inside implicit loop, auto-index by loop variable */
						if (ctx->implicit_loop_index[0]) {
							const char *load_type = elem_llvm_type(ctx, fdecl->type->data.name);
							char *idx_gep = gen_value_name(ctx);
							buffer_append_fmt(ctx, "  %s = getelementptr %s, %s* %s, i64 %s\n", idx_gep, llvm_type,
							                  llvm_type, ptr_val, ctx->implicit_loop_index);
							char *elem = gen_value_name(ctx);
							int align = ctx->vector_lanes > 0 ? 8 : 4;

							if (ctx->vector_lanes > 0) {
								/* Vector load: bitcast pointer to vector type, then load */
								char *vec_ptr = gen_value_name(ctx);
								buffer_append_fmt(ctx, "  %s = bitcast %s* %s to %s*\n", vec_ptr, llvm_type, idx_gep,
								                  load_type);
								buffer_append_fmt(ctx, "  %s = load %s, %s* %s, align %d\n", elem, load_type, load_type,
								                  vec_ptr, align);
							} else {
								/* Scalar load */
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
		if (expr->data.index.index_count > 0) {
			codegen_expression(ctx, expr->data.index.indices[0], idx_buf);
		}

		/* Determine element type from semantic analysis */
		const char *scalar_type = "i32"; /* default */
		const char *arche_type = NULL;

		if (expr->resolved_type) {
			arche_type = expr->resolved_type;
			scalar_type = llvm_type_from_arche(arche_type);
		} else if (expr->data.index.base->resolved_type) {
			/* Use base's resolved type (e.g., for pos[i] where pos is double*) */
			arche_type = expr->data.index.base->resolved_type;
			scalar_type = llvm_type_from_arche(arche_type);
		}

		/* In vector mode, load uses vector type; GEP uses scalar pointer */
		const char *load_type = arche_type ? elem_llvm_type(ctx, arche_type) : scalar_type;

		char *res_name = gen_value_name(ctx);
		buffer_append_fmt(ctx, "  %s = getelementptr %s, %s* %s, i64 %s\n", res_name, scalar_type, scalar_type,
		                  base_buf, idx_buf);

		char *loaded = gen_value_name(ctx);
		int align = ctx->vector_lanes > 0 ? 8 : 4;

		if (ctx->vector_lanes > 0 && arche_type) {
			/* Vector load: bitcast pointer to vector type, then load */
			char *vec_ptr = gen_value_name(ctx);
			buffer_append_fmt(ctx, "  %s = bitcast %s* %s to %s*\n", vec_ptr, scalar_type, res_name, load_type);
			buffer_append_fmt(ctx, "  %s = load %s, %s* %s, align %d\n", loaded, load_type, load_type, vec_ptr, align);
		} else {
			/* Scalar load */
			buffer_append_fmt(ctx, "  %s = load %s, %s* %s, align %d\n", loaded, load_type, scalar_type, res_name,
			                  align);
		}
		strcpy(result_buf, loaded);
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

			/* Get arch_name from ValueInfo of args[0] */
			const char *arch_name = NULL;
			ArchetypeDecl *arch = NULL;
			if (expr->data.call.args[0]->type == EXPR_NAME) {
				ValueInfo *arch_var = find_value(ctx, expr->data.call.args[0]->data.name.name);
				if (arch_var && arch_var->arch_name) {
					arch_name = arch_var->arch_name;
					arch = find_archetype_decl(ctx, arch_name);
				}
			}

			if (arch_name && arch) {
				/* Emit call to typed insert helper */
				buffer_append_fmt(ctx, "  call void @arche_insert_%s(%%struct.%s* %s", arch_name, arch_name, arch_buf);

				/* Evaluate and emit field values with types */
				int field_idx = 0;
				for (int i = 1; i < expr->data.call.arg_count; i++) {
					/* Find corresponding FIELD_COLUMN field */
					while (field_idx < arch->field_count && arch->fields[field_idx]->kind != FIELD_COLUMN) {
						field_idx++;
					}
					if (field_idx < arch->field_count) {
						char field_buf[256];
						codegen_expression(ctx, expr->data.call.args[i], field_buf);
						const char *field_type = llvm_type_from_arche(arch->fields[field_idx]->type->data.name);
						buffer_append_fmt(ctx, ", %s %s", field_type, field_buf);
						field_idx++;
					}
				}
				buffer_append(ctx, ")\n");
				strcpy(result_buf, "0");
			}
			break;
		}

		/* Special handling for delete builtin */
		if (func_name && strcmp(func_name, "delete") == 0 && expr->data.call.arg_count >= 2) {
			/* args[0] is archetype variable, args[1] is index */
			char arch_buf[256];
			codegen_expression(ctx, expr->data.call.args[0], arch_buf);

			char idx_buf[256];
			codegen_expression(ctx, expr->data.call.args[1], idx_buf);

			/* Get arch_name from ValueInfo of args[0] */
			const char *arch_name = NULL;
			if (expr->data.call.args[0]->type == EXPR_NAME) {
				ValueInfo *arch_var = find_value(ctx, expr->data.call.args[0]->data.name.name);
				if (arch_var && arch_var->arch_name) {
					arch_name = arch_var->arch_name;
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

			/* Get arch_name from ValueInfo of args[0] */
			const char *arch_name = NULL;
			if (expr->data.call.args[0]->type == EXPR_NAME) {
				ValueInfo *arch_var = find_value(ctx, expr->data.call.args[0]->data.name.name);
				if (arch_var && arch_var->arch_name) {
					arch_name = arch_var->arch_name;
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
			if (expr->data.call.args[i]->type == EXPR_LITERAL &&
			    expr->data.call.args[i]->data.literal.lexeme[0] == '"') {
				arg_is_string[i] = 1;
			}
			/* Check if this arg is an array literal */
			else if (expr->data.call.args[i]->type == EXPR_ARRAY_LITERAL) {
				arg_is_array_literal[i] = 1;
			}
			/* Check if this arg is a variable holding a string (type 2) or arche_array (type 5) */
			else if (expr->data.call.args[i]->type == EXPR_NAME) {
				ValueInfo *var = find_value(ctx, expr->data.call.args[i]->data.name.name);
				if (var && var->type == 2) {
					arg_is_string[i] = 1;
				}
				arg_values[i] = var;
			}

			codegen_expression(ctx, expr->data.call.args[i], arg_bufs[i]);
		}

		ProcDecl *callee_proc = find_proc_decl(ctx, func_name);
		char **call_arg_vals = malloc(expr->data.call.arg_count * sizeof(char *));
		const char **call_arg_types = malloc(expr->data.call.arg_count * sizeof(const char *));

		/* Prepare arguments: emit conversions before the call, collect register names */
		for (int i = 0; i < expr->data.call.arg_count; i++) {
			call_arg_vals[i] = malloc(256);
			call_arg_types[i] = NULL;

			/* Determine what callee param expects */
			int callee_wants_arr = 0;
			int callee_is_extern = callee_proc && callee_proc->is_extern;
			if (callee_proc && i < callee_proc->param_count) {
				TypeRef *pt = callee_proc->params[i]->type;
				if (pt && pt->kind == TYPE_ARRAY) {
					callee_wants_arr = 1;
				}
			}

			/* Handle type conversions, emit code before call if needed */
			if (arg_values[i] && arg_values[i]->type == 5) {
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
				/* String literal passed to non-array param */
				if (callee_wants_arr) {
					/* Already wrapped in struct, pass struct ptr */
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
				/* Default to i32 */
				strcpy(call_arg_vals[i], arg_bufs[i]);
				call_arg_types[i] = "i32";
			}
		}

		char *res_name = gen_value_name(ctx);

		/* Emit the call with prepared arguments */
		buffer_append_fmt(ctx, "  %s = call i32 @%s(", res_name, func_name ? func_name : "unknown");
		for (int i = 0; i < expr->data.call.arg_count; i++) {
			buffer_append_fmt(ctx, "%s %s", call_arg_types[i], call_arg_vals[i]);
			if (i < expr->data.call.arg_count - 1) {
				buffer_append(ctx, ", ");
			}
		}
		buffer_append(ctx, ")\n");

		strcpy(result_buf, res_name);

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

		decl_pos += snprintf(decl_pos, decl_space, "%s = private constant [%d x i8] [", global_name, elem_count);
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
		snprintf(decl_pos, decl_space, "]\n");

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
		buffer_append_fmt(ctx, "  %s = alloca %%struct.arche_array\n", arr_alloca);

		/* Get pointer to global array */
		char *arr_ptr = gen_value_name(ctx);
		buffer_append_fmt(ctx, "  %s = getelementptr [%d x i8], [%d x i8]* %s, i32 0, i32 0\n", arr_ptr, elem_count,
		                  elem_count, global_name);

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

		/* Struct layout: [pointers...][count][capacity][free_list*][free_count] */
		/* Calculate struct header size: (field_count pointers) * 8 + metadata (4*8) = (field_count+4)*8 */
		int struct_sz_bytes = (arch->field_count + 4) * 8;

		/* Calculate byte size per element across all columns */
		int bytes_per_row = 0;
		for (int i = 0; i < arch->field_count; i++) {
			if (arch->fields[i]->kind == FIELD_COLUMN) {
				const char *elem_type = llvm_type_from_arche(arch->fields[i]->type->data.name);
				bytes_per_row += (elem_type[0] == 'd') ? 8 : 4;
			}
		}
		/* Add 8 bytes per row for free_list entry (i64) */
		int total_bytes_per_row = bytes_per_row + 8;

		/* Total size = struct_header + (count * bytes_per_row) + (count * 8 for free_list) */
		/* = struct_sz_bytes + count * total_bytes_per_row */
		char *total_bytes = gen_value_name(ctx);
		char *data_bytes = gen_value_name(ctx);
		buffer_append_fmt(ctx, "  %s = mul i64 %s, %d\n", data_bytes, count_buf, total_bytes_per_row);
		buffer_append_fmt(ctx, "  %s = add i64 %d, %s\n", total_bytes, struct_sz_bytes, data_bytes);

		/* Single malloc */
		char *raw_ptr = gen_value_name(ctx);
		buffer_append_fmt(ctx, "  %s = call i8* @malloc(i64 %s)\n", raw_ptr, total_bytes);

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
				const char *elem_type = llvm_type_from_arche(arch->fields[i]->type->data.name);

				char *col_gep = gen_value_name(ctx);
				buffer_append_fmt(ctx, "  %s = getelementptr %%struct.%s, %%struct.%s* %s, i32 0, i32 %d\n", col_gep,
				                  arch_name, arch_name, struct_ptr, i);

				char *col_data = gen_value_name(ctx);
				buffer_append_fmt(ctx, "  %s = getelementptr i8, i8* %s, i64 %d\n", col_data, raw_ptr,
				                  struct_sz_bytes + col_offset);

				char *col_ptr = gen_value_name(ctx);
				buffer_append_fmt(ctx, "  %s = bitcast i8* %s to %s*\n", col_ptr, col_data, elem_type);

				buffer_append_fmt(ctx, "  store %s* %s, %s** %s\n", elem_type, col_ptr, elem_type, col_gep);

				int elem_size = (elem_type[0] == 'd') ? 8 : 4;
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

		strcpy(result_buf, struct_ptr);
		break;
	}
	}
}
/* ========== WHOLE-COLUMN LOOP HELPER ========== */

static void emit_whole_column_loop(CodegenContext *ctx, const char *col_ptr, /* SSA reg: scalar* column data */
                                   const char *count,                        /* SSA reg: i64 element count */
                                   const char *scalar_type,                  /* "double" or "i32" */
                                   const char *arche_type,                   /* "float" or "int" */
                                   Expression *rhs,                          /* RHS expression */
                                   int op) /* OP_NONE = store, others = load+op+store */
{
	/* Align count down to 4-element boundary for vector loop */
	char *count_aligned = gen_value_name(ctx);
	buffer_append_fmt(ctx, "  %s = and i64 %s, -4\n", count_aligned, count);

	/* Vector loop setup */
	char *v_ctr_alloca = gen_value_name(ctx);
	buffer_append_fmt(ctx, "  %s = alloca i64\n", v_ctr_alloca);
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

	/* Loop increment */
	char *vi_new = gen_value_name(ctx);
	buffer_append_fmt(ctx, "  %s = add i64 %s, 4\n", vi_new, vi);
	buffer_append_fmt(ctx, "  store i64 %s, i64* %s\n", vi_new, v_ctr_alloca);
	buffer_append_fmt(ctx, "  br label %%%s\n\n", vec_loop_lbl);

	/* Scalar loop setup */
	buffer_append_fmt(ctx, "%s:\n", scalar_setup_lbl);
	char *s_ctr_alloca = gen_value_name(ctx);
	buffer_append_fmt(ctx, "  %s = alloca i64\n", s_ctr_alloca);
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

		/* Check if the value is a string literal (old style) or array literal (new style from string expansion) */
		int is_string = 0;
		if (stmt->data.let_stmt.value) {
			if (stmt->data.let_stmt.value->type == EXPR_LITERAL &&
			    stmt->data.let_stmt.value->data.literal.lexeme[0] == '"') {
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

		if (stmt->data.let_stmt.value) {
			codegen_expression(ctx, stmt->data.let_stmt.value, value_buf);
		} else {
			strcpy(value_buf, "0");
		}

		if (is_alloc && alloc_arch_name) {
			/* Track as arch pointer (type 3) */
			add_arch_value(ctx, var_name, value_buf, alloc_arch_name);
		} else if (is_string) {
			/* For strings, handle based on whether it's already a struct or needs wrapping */
			if (stmt->data.let_stmt.value->type == EXPR_ARRAY_LITERAL) {
				/* Array literal: value_buf is already a struct pointer, just use it */
				add_array_value(ctx, var_name, value_buf);
			} else {
				/* Old-style string literal: create arche_array struct wrapper */
				int len = 0;
				const char *lex = stmt->data.let_stmt.value->data.literal.lexeme;
				int i = 1; /* skip opening quote */
				while (lex[i] && lex[i] != '"') {
					if (lex[i] == '\\' && lex[i + 1]) {
						i += 2; /* skip escape sequence */
					} else {
						i++;
					}
					len++;
				}

				/* alloca arche_array struct and populate {data_ptr, length, max_length} */
				char *arr_alloca = gen_value_name(ctx);
				buffer_append_fmt(ctx, "  %s = alloca %%struct.arche_array\n", arr_alloca);

				char *ptr_gep = gen_value_name(ctx);
				buffer_append_fmt(ctx,
				                  "  %s = getelementptr %%struct.arche_array, %%struct.arche_array* %s, i32 0, i32 0\n",
				                  ptr_gep, arr_alloca);
				buffer_append_fmt(ctx, "  store i8* %s, i8** %s\n", value_buf, ptr_gep);

				char *len_gep = gen_value_name(ctx);
				buffer_append_fmt(ctx,
				                  "  %s = getelementptr %%struct.arche_array, %%struct.arche_array* %s, i32 0, i32 1\n",
				                  len_gep, arr_alloca);
				buffer_append_fmt(ctx, "  store i64 %d, i64* %s\n", len, len_gep);

				char *cap_gep = gen_value_name(ctx);
				buffer_append_fmt(ctx,
				                  "  %s = getelementptr %%struct.arche_array, %%struct.arche_array* %s, i32 0, i32 2\n",
				                  cap_gep, arr_alloca);
				buffer_append_fmt(ctx, "  store i64 %d, i64* %s\n", len, cap_gep);

				add_array_value(ctx, var_name, arr_alloca);
			}
		} else {
			/* For integers, allocate and store */
			char *alloca_name = gen_value_name(ctx);
			buffer_append_fmt(ctx, "  %s = alloca i32\n", alloca_name);
			buffer_append_fmt(ctx, "  store i32 %s, i32* %s\n", value_buf, alloca_name);
			add_value(ctx, var_name, alloca_name, 1);
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
			if (inst && inst->type == 3 && inst->arch_name) {
				ArchetypeDecl *arch = find_archetype_decl(ctx, inst->arch_name);
				if (arch) {
					const char *fname = stmt->data.assign_stmt.target->data.field.field_name;
					for (int i = 0; i < arch->field_count; i++) {
						if (strcmp(arch->fields[i]->name, fname) == 0 && arch->fields[i]->kind == FIELD_COLUMN) {
							is_whole_column = 1;
							break;
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
				if (stmt->data.assign_stmt.op == OP_NONE) {
					buffer_append_fmt(ctx, "  store i32 %s, i32* %s\n", value_buf, val->llvm_name);
				} else {
					/* Compound assignment: load, compute, store */
					char *loaded = gen_value_name(ctx);
					buffer_append_fmt(ctx, "  %s = load i32, i32* %s\n", loaded, val->llvm_name);

					const char *op;
					switch (stmt->data.assign_stmt.op) {
					case OP_ADD:
						op = "add";
						break;
					case OP_SUB:
						op = "sub";
						break;
					case OP_MUL:
						op = "mul";
						break;
					case OP_DIV:
						op = "sdiv";
						break;
					default:
						op = "add";
						break;
					}

					char *result = gen_value_name(ctx);
					buffer_append_fmt(ctx, "  %s = %s i32 %s, %s\n", result, op, loaded, value_buf);
					buffer_append_fmt(ctx, "  store i32 %s, i32* %s\n", result, val->llvm_name);
				}
			}

			/* Path B: target is EXPR_NAME type-4 (sys parameter) */
			if (val && val->type == 4) {
				/* Column parameter: emit whole-column loop */
				const char *arche_type = val->field_type ? val->field_type : "float";
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
					                       stmt->data.assign_stmt.value, stmt->data.assign_stmt.op);
				}
			}
		} else if (stmt->data.assign_stmt.target->type == EXPR_FIELD) {
			/* Path A: target is EXPR_FIELD of FIELD_COLUMN (p.pos = p.pos + p.vel) */
			if (stmt->data.assign_stmt.target->data.field.base->type == EXPR_NAME) {
				const char *inst_name = stmt->data.assign_stmt.target->data.field.base->data.name.name;
				ValueInfo *inst = find_value(ctx, inst_name);

				if (inst && inst->type == 3 && inst->arch_name) {
					ArchetypeDecl *arch = find_archetype_decl(ctx, inst->arch_name);
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
							const char *llvm_type = llvm_type_from_arche(fdecl->type->data.name);
							char *field_gep = gen_value_name(ctx);
							buffer_append_fmt(ctx, "  %s = getelementptr %%struct.%s, %%struct.%s* %s, i32 0, i32 %d\n",
							                  field_gep, inst->arch_name, inst->arch_name, inst->llvm_name, field_idx);

							char *col_ptr = gen_value_name(ctx);
							buffer_append_fmt(ctx, "  %s = load %s*, %s** %s\n", col_ptr, llvm_type, llvm_type,
							                  field_gep);

							/* Load count from archetype */
							char *count_gep = gen_value_name(ctx);
							int count_idx = arch->field_count;
							buffer_append_fmt(ctx, "  %s = getelementptr %%struct.%s, %%struct.%s* %s, i32 0, i32 %d\n",
							                  count_gep, inst->arch_name, inst->arch_name, inst->llvm_name, count_idx);
							char *count = gen_value_name(ctx);
							buffer_append_fmt(ctx, "  %s = load i64, i64* %s\n", count, count_gep);

							/* Emit whole-column loop */
							const char *scalar_type = llvm_type_from_arche(fdecl->type->data.name);
							emit_whole_column_loop(ctx, col_ptr, count, scalar_type, fdecl->type->data.name,
							                       stmt->data.assign_stmt.value, stmt->data.assign_stmt.op);
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
								arche_type = arch->fields[i]->type->data.name;
								scalar_type = llvm_type_from_arche(arche_type);
								break;
							}
						}
					}
				}
			}

			/* In vector mode, load/store use vector type; GEP uses scalar pointer */
			const char *load_type = arche_type ? elem_llvm_type(ctx, arche_type) : scalar_type;

			/* Compute target address (always uses scalar pointer) */
			char *target_addr = gen_value_name(ctx);
			buffer_append_fmt(ctx, "  %s = getelementptr %s, %s* %s, i64 %s\n", target_addr, scalar_type, scalar_type,
			                  base_buf, idx_buf);

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
				buffer_append_fmt(ctx, "  %s = alloca i64\n", v_counter);
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
				add_value(ctx, var_name, vi, 0);
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
				buffer_append_fmt(ctx, "  %s = alloca i64\n", s_counter);
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
				buffer_append_fmt(ctx, "  %s = alloca i64\n", counter);
				buffer_append_fmt(ctx, "  store i64 0, i64* %s\n", counter);

				char *loop_label = gen_value_name(ctx);
				char *exit_label = gen_value_name(ctx);

				buffer_append_fmt(ctx, "  br label %s\n", loop_label);
				buffer_append_fmt(ctx, "%s:\n", loop_label + 1); /* Skip the '%' prefix for label def */

				/* Load counter and compare */
				char *cond_val = gen_value_name(ctx);
				buffer_append_fmt(ctx, "  %s = load i64, i64* %s\n", cond_val, counter);

				char *cmp_result = gen_value_name(ctx);
				buffer_append_fmt(ctx, "  %s = icmp slt i64 %s, %s\n", cmp_result, cond_val, count_bound);

				push_value_scope(ctx);
				add_value(ctx, var_name, cond_val, 0);

				for (int i = 0; i < stmt->data.for_stmt.body_count; i++) {
					codegen_statement(ctx, stmt->data.for_stmt.body[i]);
				}

				/* Increment counter */
				char *next_val = gen_value_name(ctx);
				buffer_append_fmt(ctx, "  %s = add i64 %s, 1\n", next_val, cond_val);
				buffer_append_fmt(ctx, "  store i64 %s, i64* %s\n", next_val, counter);

				/* Branch based on actual condition */
				buffer_append_fmt(ctx, "  br i1 %s, label %s, label %s\n", cmp_result, loop_label, exit_label);
				buffer_append_fmt(ctx, "%s:\n", exit_label + 1); /* Skip the '%' prefix for label def */

				pop_value_scope(ctx);
			}
		} else {
			/* Fallback for non-name iterables */
			codegen_expression(ctx, stmt->data.for_stmt.iterable, iter_buf);
			char *counter = gen_value_name(ctx);
			buffer_append_fmt(ctx, "  %s = alloca i32\n", counter);
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

	case STMT_RUN: {
		/* run system - dispatch to matching archetypes in scope */
		const char *system_name = stmt->data.run_stmt.system_name;

		/* Find the system definition */
		SysDecl *sys = find_sys_decl(ctx, system_name);
		if (!sys) {
			buffer_append_fmt(ctx, "  ; ERROR: undefined system '%s'\n", system_name);
			break;
		}

		/* Check scope exists */
		if (ctx->scope_count == 0) {
			buffer_append_fmt(ctx, "  ; ERROR: no scope for system '%s'\n", system_name);
			break;
		}

		/* Find all variables in scope that are archetype instances */
		ValueScope *scope = &ctx->scopes[ctx->scope_count - 1];
		int found_any = 0;

		for (int vi = 0; vi < scope->value_count; vi++) {
			ValueInfo *var = scope->values[vi];
			/* Skip non-archetype variables */
			if (var->type != 3 || !var->arch_name) {
				continue;
			}

			/* Find the archetype declaration */
			ArchetypeDecl *arch = find_archetype_decl(ctx, var->arch_name);
			if (!arch) {
				continue;
			}

			/* Check if this archetype matches the system */
			if (!archetype_matches_system(arch, sys)) {
				continue;
			}

			found_any = 1;
			/* Call system with this archetype instance */
			buffer_append_fmt(ctx, "  call void @%s(%%struct.%s* %s)\n", system_name, var->arch_name, var->llvm_name);
		}

		if (!found_any) {
			buffer_append_fmt(ctx, "  ; WARNING: no matching archetypes for system '%s'\n", system_name);
		}

		break;
	}

	case STMT_EXPR: {
		char expr_buf[256];
		codegen_expression(ctx, stmt->data.expr_stmt.expr, expr_buf);
		break;
	}

	case STMT_FREE: {
		char value_buf[256];
		codegen_expression(ctx, stmt->data.free_stmt.value, value_buf);
		buffer_append_fmt(ctx, "  call void @free(i8* %s)\n", value_buf);
		break;
	}
	}
}

/* ========== DECLARATION CODEGEN ========== */

static void codegen_archetype_decl(CodegenContext *ctx, ArchetypeDecl *arch) {
	/* Generate struct definition for archetype */
	buffer_append_fmt(ctx, "%%struct.%s = type {\n", arch->name);

	for (int i = 0; i < arch->field_count; i++) {
		const char *base_type = llvm_type_from_arche(arch->fields[i]->type->data.name);

		/* col fields are arrays → emit as pointer type */
		if (arch->fields[i]->kind == FIELD_COLUMN) {
			buffer_append_fmt(ctx, "  %s*", base_type);
		} else {
			buffer_append_fmt(ctx, "  %s", base_type);
		}

		buffer_append(ctx, ",\n");
	}

	/* Add count and capacity fields for tracking live/allocated entries */
	buffer_append(ctx, "  i64,\n");
	buffer_append(ctx, "  i64,\n");

	/* Add free_list and free_count for pooling */
	buffer_append(ctx, "  i64*,\n");
	buffer_append(ctx, "  i64\n");

	buffer_append(ctx, "}\n\n");

	/* Emit insert helper function */
	buffer_append_fmt(ctx, "define void @arche_insert_%s(%%struct.%s* %%arch", arch->name, arch->name);
	for (int i = 0; i < arch->field_count; i++) {
		if (arch->fields[i]->kind == FIELD_COLUMN) {
			const char *base_type = llvm_type_from_arche(arch->fields[i]->type->data.name);
			buffer_append_fmt(ctx, ", %s %%f%d", base_type, i);
		}
	}
	buffer_append(ctx, ") {\n");
	buffer_append(ctx, "entry:\n");

	/* Setup allocas for slot and increment flag */
	buffer_append(ctx, "  %slot_var = alloca i64\n");
	buffer_append(ctx, "  %do_incr = alloca i1\n");

	/* Load free_count, count, capacity, free_list */
	int count_idx = arch->field_count;
	int cap_idx = arch->field_count + 1;
	int fl_idx = arch->field_count + 2;
	int fc_idx = arch->field_count + 3;

	buffer_append_fmt(ctx, "  %%fc_ptr = getelementptr %%struct.%s, %%struct.%s* %%arch, i32 0, i32 %d\n", arch->name,
	                  arch->name, fc_idx);
	buffer_append(ctx, "  %free_count = load i64, i64* %fc_ptr\n");
	buffer_append(ctx, "  %has_free = icmp sgt i64 %free_count, 1\n");
	buffer_append(ctx, "  br i1 %has_free, label %pop_free, label %check_grow\n\n");

	/* Pop from free_list */
	buffer_append(ctx, "pop_free:\n");
	buffer_append_fmt(ctx, "  %%fl_ptr = getelementptr %%struct.%s, %%struct.%s* %%arch, i32 0, i32 %d\n", arch->name,
	                  arch->name, fl_idx);
	buffer_append(ctx, "  %free_list = load i64*, i64** %fl_ptr\n");
	buffer_append(ctx, "  %new_fc = sub i64 %free_count, 1\n");
	buffer_append(ctx, "  %slot_ptr = getelementptr i64, i64* %free_list, i64 %new_fc\n");
	buffer_append(ctx, "  %slot = load i64, i64* %slot_ptr\n");
	buffer_append(ctx, "  store i64 %new_fc, i64* %fc_ptr\n");
	buffer_append(ctx, "  store i64 %slot, i64* %slot_var\n");
	buffer_append(ctx, "  store i1 0, i1* %do_incr\n");
	buffer_append(ctx, "  br label %write_fields\n\n");

	/* Check if count needs grow */
	buffer_append(ctx, "check_grow:\n");
	buffer_append_fmt(ctx, "  %%count_ptr = getelementptr %%struct.%s, %%struct.%s* %%arch, i32 0, i32 %d\n",
	                  arch->name, arch->name, count_idx);
	buffer_append(ctx, "  %count = load i64, i64* %count_ptr\n");
	buffer_append_fmt(ctx, "  %%cap_ptr = getelementptr %%struct.%s, %%struct.%s* %%arch, i32 0, i32 %d\n", arch->name,
	                  arch->name, cap_idx);
	buffer_append(ctx, "  %cap = load i64, i64* %cap_ptr\n");
	buffer_append(ctx, "  %needs_grow = icmp sge i64 %count, %cap\n");
	buffer_append(ctx, "  br i1 %needs_grow, label %grow, label %use_count\n\n");

	/* Grow block */
	buffer_append(ctx, "grow:\n");
	buffer_append(ctx, "  %cap_zero = icmp eq i64 %cap, 0\n");
	buffer_append(ctx, "  %doubled = mul i64 %cap, 2\n");
	buffer_append(ctx, "  %new_cap = select i1 %cap_zero, i64 4, i64 %doubled\n");

	/* Realloc each column field */
	int col_idx = 0;
	for (int i = 0; i < arch->field_count; i++) {
		if (arch->fields[i]->kind == FIELD_COLUMN) {
			const char *base_type = llvm_type_from_arche(arch->fields[i]->type->data.name);
			int elem_size = strcmp(base_type, "double") == 0 ? 8 : 4;
			buffer_append_fmt(ctx, "  %%col_pp%d = getelementptr %%struct.%s, %%struct.%s* %%arch, i32 0, i32 %d\n",
			                  col_idx, arch->name, arch->name, i);
			buffer_append_fmt(ctx, "  %%col_p%d = load %s*, %s** %%col_pp%d\n", col_idx, base_type, base_type, col_idx);
			buffer_append_fmt(ctx, "  %%col_i8_%d = bitcast %s* %%col_p%d to i8*\n", col_idx, base_type, col_idx);
			buffer_append_fmt(ctx, "  %%bytes%d = mul i64 %%new_cap, %d\n", col_idx, elem_size);
			buffer_append_fmt(ctx, "  %%new_i8_%d = call i8* @realloc(i8* %%col_i8_%d, i64 %%bytes%d)\n", col_idx,
			                  col_idx, col_idx);
			buffer_append_fmt(ctx, "  %%new_p%d = bitcast i8* %%new_i8_%d to %s*\n", col_idx, col_idx, base_type);
			buffer_append_fmt(ctx, "  store %s* %%new_p%d, %s** %%col_pp%d\n", base_type, col_idx, base_type, col_idx);
			col_idx++;
		}
	}

	/* Realloc free_list */
	buffer_append(ctx, "  %fl_bytes = mul i64 %new_cap, 8\n");
	buffer_append_fmt(ctx, "  %%fl_ptr_grow = getelementptr %%struct.%s, %%struct.%s* %%arch, i32 0, i32 %d\n",
	                  arch->name, arch->name, fl_idx);
	buffer_append(ctx, "  %free_list_grow = load i64*, i64** %fl_ptr_grow\n");
	buffer_append(ctx, "  %fl_i8 = bitcast i64* %free_list_grow to i8*\n");
	buffer_append(ctx, "  %new_fl_i8 = call i8* @realloc(i8* %fl_i8, i64 %fl_bytes)\n");
	buffer_append(ctx, "  %new_fl = bitcast i8* %new_fl_i8 to i64*\n");
	buffer_append(ctx, "  store i64* %new_fl, i64** %fl_ptr_grow\n");

	buffer_append(ctx, "  store i64 %new_cap, i64* %cap_ptr\n");
	buffer_append(ctx, "  br label %use_count\n\n");

	/* Use count as slot */
	buffer_append(ctx, "use_count:\n");
	buffer_append(ctx, "  store i64 %count, i64* %slot_var\n");
	buffer_append(ctx, "  store i1 1, i1* %do_incr\n");
	buffer_append(ctx, "  br label %write_fields\n\n");

	/* Write fields block */
	buffer_append(ctx, "write_fields:\n");
	buffer_append(ctx, "  %final_slot = load i64, i64* %slot_var\n");
	col_idx = 0;
	for (int i = 0; i < arch->field_count; i++) {
		if (arch->fields[i]->kind == FIELD_COLUMN) {
			const char *base_type = llvm_type_from_arche(arch->fields[i]->type->data.name);
			buffer_append_fmt(ctx, "  %%col_pp2_%d = getelementptr %%struct.%s, %%struct.%s* %%arch, i32 0, i32 %d\n",
			                  col_idx, arch->name, arch->name, i);
			buffer_append_fmt(ctx, "  %%col_p2_%d = load %s*, %s** %%col_pp2_%d\n", col_idx, base_type, base_type,
			                  col_idx);
			buffer_append_fmt(ctx, "  %%slot%d = getelementptr %s, %s* %%col_p2_%d, i64 %%final_slot\n", col_idx,
			                  base_type, base_type, col_idx);
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
	buffer_append(ctx, "  ret void\n");
	buffer_append(ctx, "}\n\n");

	/* Emit delete helper function */
	buffer_append_fmt(ctx, "define void @arche_delete_%s(%%struct.%s* %%arch, i64 %%idx) {\n", arch->name, arch->name);
	buffer_append(ctx, "entry:\n");

	/* Load free_list and free_count */
	buffer_append_fmt(ctx, "  %%fl_ptr = getelementptr %%struct.%s, %%struct.%s* %%arch, i32 0, i32 %d\n", arch->name,
	                  arch->name, fl_idx);
	buffer_append(ctx, "  %free_list = load i64*, i64** %fl_ptr\n");
	buffer_append_fmt(ctx, "  %%fc_ptr = getelementptr %%struct.%s, %%struct.%s* %%arch, i32 0, i32 %d\n", arch->name,
	                  arch->name, fc_idx);
	buffer_append(ctx, "  %free_count = load i64, i64* %fc_ptr\n");

	/* Push idx to free_list[free_count] */
	buffer_append(ctx, "  %slot_ptr = getelementptr i64, i64* %free_list, i64 %free_count\n");
	buffer_append(ctx, "  store i64 %idx, i64* %slot_ptr\n");

	/* Increment free_count */
	buffer_append(ctx, "  %new_fc = add i64 %free_count, 1\n");
	buffer_append(ctx, "  store i64 %new_fc, i64* %fc_ptr\n");

	buffer_append(ctx, "  ret void\n");
	buffer_append(ctx, "}\n\n");

	/* Emit dealloc helper function */
	buffer_append_fmt(ctx, "define void @arche_dealloc_%s(%%struct.%s* %%arch) {\n", arch->name, arch->name);
	buffer_append(ctx, "entry:\n");
	buffer_append(ctx, "  %arch_i8 = bitcast %struct.");
	buffer_append_fmt(ctx, "%s* %%arch to i8*\n", arch->name);
	buffer_append(ctx, "  call void @free(i8* %arch_i8)\n");
	buffer_append(ctx, "  ret void\n");
	buffer_append(ctx, "}\n\n");
}

static void codegen_func_decl(CodegenContext *ctx, FuncDecl *func) {
	/* Generate function definition */
	const char *return_type = llvm_type_from_arche(func->return_type ? func->return_type->data.name : "int");

	buffer_append_fmt(ctx, "define %s @%s(", return_type, func->name);

	for (int i = 0; i < func->param_count; i++) {
		const char *param_type = llvm_type_from_arche(func->params[i]->type->data.name);
		buffer_append_fmt(ctx, "%s %%arg%d", param_type, i);
		if (i < func->param_count - 1) {
			buffer_append(ctx, ", ");
		}
	}
	buffer_append(ctx, ") {\n");
	buffer_append(ctx, "entry:\n");

	push_value_scope(ctx);

	/* Add parameters to scope */
	for (int i = 0; i < func->param_count; i++) {
		char param_name[32];
		snprintf(param_name, sizeof(param_name), "%%arg%d", i);
		add_value(ctx, func->params[i]->name, param_name, 0);
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
	buffer_append(ctx, "}\n\n");
}

static void codegen_proc_decl(CodegenContext *ctx, ProcDecl *proc) {
	/* For extern procs, emit declare stub */
	if (proc->is_extern) {
		buffer_append_fmt(ctx, "declare i32 @%s(", proc->name);
		for (int i = 0; i < proc->param_count; i++) {
			TypeRef *param_type = proc->params[i]->type;
			const char *type_name = (param_type && param_type->kind == TYPE_NAME) ? param_type->data.name : "Int";
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
		buffer_append(ctx, ")\n");
		return;
	}

	/* Generate procedure - main returns i32, others return void */
	int is_main = (strcmp(proc->name, "main") == 0);
	buffer_append_fmt(ctx, "define %s @%s(", is_main ? "i32" : "void", proc->name);

	/* Emit parameter types and names */
	for (int i = 0; i < proc->param_count; i++) {
		TypeRef *param_type = proc->params[i]->type;
		const char *type_name = (param_type && param_type->kind == TYPE_NAME) ? param_type->data.name : "Int";
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

	push_value_scope(ctx);

	/* Register parameters in scope */
	for (int i = 0; i < proc->param_count; i++) {
		char param_llvm[32];
		snprintf(param_llvm, sizeof(param_llvm), "%%arg%d", i);
		TypeRef *param_type = proc->params[i]->type;
		const char *type_name = (param_type && param_type->kind == TYPE_NAME) ? param_type->data.name : "Int";

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

	if (is_main) {
		buffer_append(ctx, "  ret i32 0\n");
	} else {
		buffer_append(ctx, "  ret void\n");
	}
	buffer_append(ctx, "}\n\n");
}

static void codegen_sys_decl(CodegenContext *ctx, SysDecl *sys) {
	/* Generate system function that takes an archetype instance */
	/* Infer archetype from parameters - look for any archetype field names */
	const char *arch_name = NULL;
	if (sys->param_count > 0 && sys->params[0] && sys->params[0]->name) {
		/* Try to find an archetype that has this field */
		for (int d = 0; d < ctx->prog->decl_count; d++) {
			Decl *decl = ctx->prog->decls[d];
			if (decl->kind == DECL_ARCHETYPE) {
				ArchetypeDecl *arch = decl->data.archetype;
				for (int f = 0; f < arch->field_count; f++) {
					if (strcmp(arch->fields[f]->name, sys->params[0]->name) == 0) {
						arch_name = arch->name;
						break;
					}
				}
				if (arch_name)
					break;
			}
		}
	}

	/* Generate function with archetype parameter */
	if (arch_name) {
		buffer_append_fmt(ctx, "define void @%s(%%struct.%s* %%archetype) #0 {\n", sys->name, arch_name);
	} else {
		buffer_append_fmt(ctx, "define void @%s() {\n", sys->name);
	}
	buffer_append(ctx, "entry:\n");

	push_value_scope(ctx);

	/* If we have an archetype, bind field parameters to their column pointers */
	if (arch_name) {
		ArchetypeDecl *arch = find_archetype_decl(ctx, arch_name);
		if (arch) {
			for (int p = 0; p < sys->param_count; p++) {
				const char *param_name = sys->params[p]->name;

				/* Find this field in the archetype */
				for (int f = 0; f < arch->field_count; f++) {
					if (strcmp(arch->fields[f]->name, param_name) == 0) {
						const char *elem_type = llvm_type_from_arche(arch->fields[f]->type->data.name);

						if (arch->fields[f]->kind == FIELD_COLUMN) {
							/* Load the column pointer from the struct */
							char *field_gep = gen_value_name(ctx);
							buffer_append_fmt(
							    ctx, "  %s = getelementptr %%struct.%s, %%struct.%s* %%archetype, i32 0, i32 %d\n",
							    field_gep, arch_name, arch_name, f);

							char *field_ptr = gen_value_name(ctx);
							buffer_append_fmt(ctx, "  %s = load %s*, %s** %s\n", field_ptr, elem_type, elem_type,
							                  field_gep);

							/* Add to scope as a column pointer (type 4) */
							add_value(ctx, param_name, field_ptr, 4); /* type 4 = column pointer */
							/* Also track the archetype and element type for vectorization detection */
							ValueInfo *col_val = find_value(ctx, param_name);
							if (col_val) {
								col_val->arch_name = malloc(strlen(arch_name) + 1);
								strcpy(col_val->arch_name, arch_name);
								col_val->field_type = arch->fields[f]->type->data.name;
							}
						} else {
							/* Meta field: load the scalar value */
							char *field_gep = gen_value_name(ctx);
							buffer_append_fmt(
							    ctx, "  %s = getelementptr %%struct.%s, %%struct.%s* %%archetype, i32 0, i32 %d\n",
							    field_gep, arch_name, arch_name, f);

							char *field_val = gen_value_name(ctx);
							buffer_append_fmt(ctx, "  %s = load %s, %s* %s\n", field_val, elem_type, elem_type,
							                  field_gep);

							/* Add to scope */
							add_value(ctx, param_name, field_val, 0); /* type 0 = scalar */
						}
						break;
					}
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
	buffer_append(ctx, "}\n\n");
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
	buffer_append(ctx, "declare void @free(i8*)\n");
	buffer_append(ctx, "declare i8* @realloc(i8*, i64)\n");
	buffer_append(ctx, "declare i32 @printf(i8*, ...)\n\n");

	/* Generate code for all declarations (this will populate globals_buffer with string constants) */
	int has_init_proc = 0;
	for (int i = 0; i < ctx->prog->decl_count; i++) {
		Decl *decl = ctx->prog->decls[i];

		switch (decl->kind) {
		case DECL_ARCHETYPE:
			codegen_archetype_decl(ctx, decl->data.archetype);
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

	/* Generate main entry point (unless a proc named 'main' was defined) */
	int has_main_proc = 0;
	for (int i = 0; i < ctx->prog->decl_count; i++) {
		if (ctx->prog->decls[i]->kind == DECL_PROC && strcmp(ctx->prog->decls[i]->data.proc->name, "main") == 0) {
			has_main_proc = 1;
			break;
		}
	}

	if (!has_main_proc) {
		buffer_append(ctx, "\ndefine i32 @main() {\n");
		buffer_append(ctx, "entry:\n");
		if (has_init_proc) {
			buffer_append(ctx, "  call void @init()\n");
		}
		buffer_append(ctx, "  ret i32 0\n");
		buffer_append(ctx, "}\n");
	}

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
	free(ctx);
}
