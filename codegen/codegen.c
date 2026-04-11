#include "codegen.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

/* ========== DATA STRUCTURES ========== */

typedef struct {
	char *name;
	char *llvm_name;	/* allocated SSA value name */
	int type;  /* 0=i32, 1=i32*, 2=i8* (string) */
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
	if (!arche_type) return "i32";  /* default to int */

	if (strcmp(arche_type, "float") == 0) return "double";
	if (strcmp(arche_type, "int") == 0) return "i32";
	if (strcmp(arche_type, "char") == 0) return "i8";

	/* For custom types (Vec3, etc.), use opaque structures for now */
	static char buf[256];
	snprintf(buf, sizeof(buf), "%%struct.%s", arche_type);
	return buf;
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

	snprintf(global_decl, sizeof(global_decl),
		"%s = private unnamed_addr constant [%zu x i8] c\"%s\\00\"\n",
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
	if (ctx->scope_count == 0) return;

	ValueScope *scope = &ctx->scopes[ctx->scope_count - 1];
	ValueInfo *val = malloc(sizeof(ValueInfo));
	val->name = malloc(strlen(name) + 1);
	strcpy(val->name, name);
	val->llvm_name = malloc(strlen(llvm_name) + 1);
	strcpy(val->llvm_name, llvm_name);
	val->type = type;

	scope->values = realloc(scope->values, (scope->value_count + 1) * sizeof(ValueInfo *));
	scope->values[scope->value_count++] = val;
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
			buffer_append_fmt(ctx, "  %s = getelementptr [%zu x i8], [%zu x i8]* %s, i32 0, i32 0\n",
				res_name, str_len + 1, str_len + 1, global_name);
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
			strcpy(result_buf, val->llvm_name);
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

		/* Try to infer float type from operands */
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

		switch (expr->data.binary.op) {
		case OP_ADD: op = is_float ? "fadd" : "add"; break;
		case OP_SUB: op = is_float ? "fsub" : "sub"; break;
		case OP_MUL: op = is_float ? "fmul" : "mul"; break;
		case OP_DIV: op = is_float ? "fdiv" : "sdiv"; break;
		case OP_EQ: op = "eq"; break;
		case OP_NEQ: op = "ne"; break;
		case OP_LT: op = is_float ? "olt" : "slt"; break;
		case OP_GT: op = is_float ? "ogt" : "sgt"; break;
		case OP_LTE: op = is_float ? "ole" : "sle"; break;
		case OP_GTE: op = is_float ? "oge" : "sge"; break;
		default: op = "add"; break;
		}

		char *res_name = gen_value_name(ctx);
		const char *type = is_float ? "double" : "i32";

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
		/* fieldexpr like archetype.field - for now return field name */
		strcpy(result_buf, expr->data.field.field_name);
		break;
	}

	case EXPR_INDEX: {
		/* index access: array[index] */
		char base_buf[256], idx_buf[256];
		codegen_expression(ctx, expr->data.index.base, base_buf);
		if (expr->data.index.index_count > 0) {
			codegen_expression(ctx, expr->data.index.indices[0], idx_buf);
		}

		char *res_name = gen_value_name(ctx);
		buffer_append_fmt(ctx, "  %s = getelementptr i32, i32* %s, i32 %s\n", res_name, base_buf, idx_buf);
		buffer_append_fmt(ctx, "  %s = load i32, i32* %s\n", gen_value_name(ctx), res_name);
		strcpy(result_buf, ctx->output_buffer + ctx->buffer_pos - 50);
		break;
	}

	case EXPR_CALL: {
		/* function call */
		char *func_name = NULL;
		if (expr->data.call.callee->type == EXPR_NAME) {
			func_name = expr->data.call.callee->data.name.name;
		}

		/* Evaluate arguments and track which are string literals or string variables */
		char **arg_bufs = malloc(expr->data.call.arg_count * sizeof(char *));
		int *arg_is_string = malloc(expr->data.call.arg_count * sizeof(int));
		for (int i = 0; i < expr->data.call.arg_count; i++) {
			arg_bufs[i] = malloc(256);
			arg_is_string[i] = 0;

			/* Check if this arg is a string literal */
			if (expr->data.call.args[i]->type == EXPR_LITERAL &&
			    expr->data.call.args[i]->data.literal.lexeme[0] == '"') {
				arg_is_string[i] = 1;
			}
			/* Check if this arg is a variable holding a string (i8* type) */
			else if (expr->data.call.args[i]->type == EXPR_NAME) {
				ValueInfo *var = find_value(ctx, expr->data.call.args[i]->data.name.name);
				if (var && var->type == 2) {  /* type 2 = i8* pointer (string) */
					arg_is_string[i] = 1;
				}
			}

			codegen_expression(ctx, expr->data.call.args[i], arg_bufs[i]);
		}

		char *res_name = gen_value_name(ctx);

		/* Call function with arguments */
		buffer_append_fmt(ctx, "  %s = call i32 @%s(", res_name, func_name ? func_name : "unknown");
		for (int i = 0; i < expr->data.call.arg_count; i++) {
			if (arg_is_string[i]) {
				buffer_append_fmt(ctx, "i8* %s", arg_bufs[i]);
			} else {
				buffer_append_fmt(ctx, "i32 %s", arg_bufs[i]);
			}
			if (i < expr->data.call.arg_count - 1) {
				buffer_append(ctx, ", ");
			}
		}
		buffer_append(ctx, ")\n");

		strcpy(result_buf, res_name);

		/* Cleanup */
		for (int i = 0; i < expr->data.call.arg_count; i++) {
			free(arg_bufs[i]);
		}
		free(arg_bufs);
		free(arg_is_string);
		break;
	}

	case EXPR_ALLOC: {
		/* allocation expression: alloc ArchetypeName(...) */
		const char *archetype_name = expr->data.alloc.archetype_name;
		char *res_name = gen_value_name(ctx);

		/* emit a call to malloc with appropriate size */
		buffer_append_fmt(ctx, "  %s = call i8* @malloc(i32 4096)\n", res_name);
		buffer_append_fmt(ctx, "  %s = bitcast i8* %s to %%struct.%s*\n", gen_value_name(ctx), res_name, archetype_name);
		strcpy(result_buf, ctx->output_buffer + ctx->buffer_pos - 50);
		break;
	}
	}
}

/* ========== STATEMENT CODEGEN ========== */

static void codegen_statement(CodegenContext *ctx, Statement *stmt) {
	if (!stmt) return;

	switch (stmt->type) {
	case STMT_LET: {
		const char *var_name = stmt->data.let_stmt.name;
		char value_buf[256];

		/* Check if the value is a string literal */
		int is_string = 0;
		if (stmt->data.let_stmt.value && stmt->data.let_stmt.value->type == EXPR_LITERAL &&
		    stmt->data.let_stmt.value->data.literal.lexeme[0] == '"') {
			is_string = 1;
		}

		if (stmt->data.let_stmt.value) {
			codegen_expression(ctx, stmt->data.let_stmt.value, value_buf);
		} else {
			strcpy(value_buf, "0");
		}

		if (is_string) {
			/* For strings, just track the i8* pointer directly (type 2) */
			add_value(ctx, var_name, value_buf, 2);
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
		char value_buf[256];
		codegen_expression(ctx, stmt->data.assign_stmt.value, value_buf);

		/* assignment - find the target variable */
		if (stmt->data.assign_stmt.target->type == EXPR_NAME) {
			const char *var_name = stmt->data.assign_stmt.target->data.name.name;
			ValueInfo *val = find_value(ctx, var_name);
			if (val && val->type == 1) {  /* type 1 = i32* pointer */
				buffer_append_fmt(ctx, "  store i32 %s, i32* %s\n", value_buf, val->llvm_name);
			} else {
				/* For now, silently skip assignments to non-pointer values (fields, etc.) */
				/* In a full implementation, we'd track field updates differently */
			}
		}
		break;
	}

	case STMT_FOR: {
		const char *var_name = stmt->data.for_stmt.var_name;
		char iter_buf[256];
		codegen_expression(ctx, stmt->data.for_stmt.iterable, iter_buf);

		/* for loop: simplified to iterate 10 times */
		char *counter = gen_value_name(ctx);
		buffer_append_fmt(ctx, "  %s = alloca i32\n", counter);
		buffer_append_fmt(ctx, "  store i32 0, i32* %s\n", counter);

		char *loop_label = gen_value_name(ctx);
		char *exit_label = gen_value_name(ctx);

		buffer_append_fmt(ctx, "  br label %s\n", loop_label);
		buffer_append_fmt(ctx, "%s:\n", loop_label);

		char *cond = gen_value_name(ctx);
		buffer_append_fmt(ctx, "  %s = load i32, i32* %s\n", cond, counter);
		buffer_append_fmt(ctx, "  %s = icmp slt i32 %s, 10\n", gen_value_name(ctx), cond);

		push_value_scope(ctx);
		add_value(ctx, var_name, cond, 0);

		for (int i = 0; i < stmt->data.for_stmt.body_count; i++) {
			codegen_statement(ctx, stmt->data.for_stmt.body[i]);
		}

		buffer_append_fmt(ctx, "  %s = load i32, i32* %s\n", gen_value_name(ctx), counter);
		buffer_append_fmt(ctx, "  %s = add i32 %s, 1\n", gen_value_name(ctx), cond);
		buffer_append_fmt(ctx, "  store i32 %s, i32* %s\n", cond, counter);
		buffer_append_fmt(ctx, "  br i1 1, label %s, label %s\n", loop_label, exit_label);
		buffer_append_fmt(ctx, "%s:\n", exit_label);

		pop_value_scope(ctx);
		break;
	}

	case STMT_RUN: {
		/* run system in world - generate call to system function */
		const char *system_name = stmt->data.run_stmt.system_name;
		const char *world_name = stmt->data.run_stmt.world_name;
		buffer_append_fmt(ctx, "  call void @%s_%s()\n", system_name, world_name);
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

static void codegen_world_decl(CodegenContext *ctx, WorldDecl *world) {
	/* Worlds are implicit in LLVM - no explicit code needed */
	(void)ctx;
	(void)world;
}

static void codegen_archetype_decl(CodegenContext *ctx, ArchetypeDecl *arch) {
	/* Generate struct definition for archetype */
	buffer_append_fmt(ctx, "%%struct.%s = type {\n", arch->name);

	for (int i = 0; i < arch->field_count; i++) {
		const char *field_type = llvm_type_from_arche(arch->fields[i]->type->data.name);
		buffer_append_fmt(ctx, "  %s", field_type);
		if (i < arch->field_count - 1) {
			buffer_append(ctx, ",\n");
		} else {
			buffer_append(ctx, "\n");
		}
	}
	buffer_append(ctx, "}\n\n");
}

static void codegen_func_decl(CodegenContext *ctx, FuncDecl *func) {
	/* Generate function definition */
	const char *return_type = llvm_type_from_arche(
		func->return_type ? func->return_type->data.name : "int"
	);

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
	/* Generate procedure as a function returning void */
	buffer_append_fmt(ctx, "define void @%s() {\n", proc->name);
	buffer_append(ctx, "entry:\n");

	push_value_scope(ctx);

	for (int i = 0; i < proc->statement_count; i++) {
		codegen_statement(ctx, proc->statements[i]);
	}

	pop_value_scope(ctx);

	buffer_append(ctx, "  ret void\n");
	buffer_append(ctx, "}\n\n");
}

static void codegen_sys_decl(CodegenContext *ctx, SysDecl *sys) {
	/* Generate system as a parameterless function - would be called per-world */
	buffer_append_fmt(ctx, "define void @%s() {\n", sys->name);
	buffer_append(ctx, "entry:\n");

	push_value_scope(ctx);

	for (int i = 0; i < sys->statement_count; i++) {
		codegen_statement(ctx, sys->statements[i]);
	}

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
	return ctx;
}

void codegen_generate(CodegenContext *ctx, FILE *output) {
	/* Preamble: declare external functions */
	buffer_append(ctx, "; Target datalayout and triple would go here\n");
	buffer_append(ctx, "target datalayout = \"e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128\"\n");
	buffer_append(ctx, "target triple = \"x86_64-unknown-linux-gnu\"\n\n");

	/* Declare custom types as opaque structures */
	buffer_append(ctx, "; Type definitions\n");
	buffer_append(ctx, "%struct.Vec3 = type { double, double, double }\n\n");

	/* External C library function declarations */
	buffer_append(ctx, "declare i8* @malloc(i32)\n");
	buffer_append(ctx, "declare void @free(i8*)\n");
	buffer_append(ctx, "declare i32 @printf(i8*, ...)\n");
	buffer_append(ctx, "declare i32 @open(i8*, i32, ...)\n");
	buffer_append(ctx, "declare i32 @close(i32)\n");
	buffer_append(ctx, "declare i32 @read(i32, i8*, i32)\n");
	buffer_append(ctx, "declare i32 @write(i32, i8*, i32)\n");
	buffer_append(ctx, "declare void @exit(i32)\n\n");

	/* Generate code for all declarations (this will populate globals_buffer with string constants) */
	int has_init_proc = 0;
	for (int i = 0; i < ctx->prog->decl_count; i++) {
		Decl *decl = ctx->prog->decls[i];

		switch (decl->kind) {
		case DECL_WORLD:
			codegen_world_decl(ctx, decl->data.world);
			break;
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

	/* Generate main entry point */
	buffer_append(ctx, "\ndefine i32 @main() {\n");
	buffer_append(ctx, "entry:\n");
	if (has_init_proc) {
		buffer_append(ctx, "  call void @init()\n");
	}
	buffer_append(ctx, "  ret i32 0\n");
	buffer_append(ctx, "}\n");

	/* Output the generated IR */
	fprintf(output, "%s", ctx->output_buffer);
}

void codegen_free(CodegenContext *ctx) {
	if (!ctx) return;

	while (ctx->scope_count > 0) {
		pop_value_scope(ctx);
	}
	free(ctx->scopes);
	free(ctx->output_buffer);
	free(ctx->globals_buffer);
	free(ctx);
}
