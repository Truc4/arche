#include "../../../cst/cst.h"
#include "../../../lexer/lexer.h"
#include "../../../parser/parser.h"
#include "../../../semantic/semantic.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Test harness */
int test_count = 0;
int test_pass = 0;
int test_fail = 0;

void test_start(const char *name) {
	test_count++;
	printf("  [%d] %s ", test_count, name);
	fflush(stdout);
}

void test_pass_msg(void) {
	test_pass++;
	printf("✓\n");
}

void test_fail_msg(const char *reason) {
	test_fail++;
	printf("✗ (%s)\n", reason);
}

#define ASSERT_NOT_NULL(ptr, msg)                                                                                      \
	if (!(ptr)) {                                                                                                      \
		test_fail_msg(msg);                                                                                            \
		return;                                                                                                        \
	}

#define ASSERT_EQ(a, b, msg)                                                                                           \
	if ((a) != (b)) {                                                                                                  \
		test_fail_msg(msg);                                                                                            \
		return;                                                                                                        \
	}

#define ASSERT_TRUE(cond, msg)                                                                                         \
	if (!(cond)) {                                                                                                     \
		test_fail_msg(msg);                                                                                            \
		return;                                                                                                        \
	}

/* Helper to parse a string. The parser now produces only the lossless CST; the abstract
 * AstProgram is reconstructed from it via cst_to_program_from_source. These assertions therefore
 * validate that cst_to_program faithfully rebuilds each construct (the parser's old output). */
AstProgram *parse_string(const char *src) {
	return cst_to_program_from_source(src);
}

/* ========== ARCHETYPE TESTS ========== */

void test_archetype_empty(void) {
	test_start("archetype empty");
	AstProgram *prog = parse_string("Player :: arche {}");
	ASSERT_NOT_NULL(prog, "program is null");
	ASSERT_EQ(prog->decl_count, 1, "expected 1 decl");
	ASSERT_EQ(prog->decls[0]->kind, DECL_ARCHETYPE, "expected DECL_ARCHETYPE");
	ASSERT_NOT_NULL(prog->decls[0]->data.archetype, "archetype is null");
	ASSERT_EQ(strcmp(prog->decls[0]->data.archetype->name, "Player"), 0, "wrong name");
	ASSERT_EQ(prog->decls[0]->data.archetype->field_count, 0, "expected 0 fields");
	ast_program_free(prog);
	test_pass_msg();
}

void test_archetype_meta_field(void) {
	test_start("archetype with meta field");
	AstProgram *prog = parse_string("Player :: arche {\n  drag :: Float\n}");
	ASSERT_NOT_NULL(prog, "program is null");
	ASSERT_EQ(prog->decl_count, 1, "expected 1 decl");
	ArchetypeDecl *arch = prog->decls[0]->data.archetype;
	ASSERT_EQ(arch->field_count, 1, "expected 1 field");
	ASSERT_EQ(arch->fields[0]->kind, FIELD_COLUMN, "expected FIELD_COLUMN");
	ASSERT_EQ(strcmp(arch->fields[0]->name, "drag"), 0, "wrong field name");
	ast_program_free(prog);
	test_pass_msg();
}

void test_archetype_col_field(void) {
	test_start("archetype with col field");
	AstProgram *prog = parse_string("Particle :: arche {\n  pos :: Float\n}");
	ASSERT_NOT_NULL(prog, "program is null");
	ArchetypeDecl *arch = prog->decls[0]->data.archetype;
	ASSERT_EQ(arch->field_count, 1, "expected 1 field");
	ASSERT_EQ(arch->fields[0]->kind, FIELD_COLUMN, "expected FIELD_COLUMN");
	ast_program_free(prog);
	test_pass_msg();
}

void test_archetype_multiple_fields(void) {
	test_start("archetype with multiple fields");
	AstProgram *prog = parse_string("Body :: arche {\n"
	                                "  drag :: Float,\n"
	                                "  pos :: Vec3,\n"
	                                "  vel :: Vec3\n"
	                                "}");
	ASSERT_NOT_NULL(prog, "program is null");
	ArchetypeDecl *arch = prog->decls[0]->data.archetype;
	ASSERT_EQ(arch->field_count, 3, "expected 3 fields");
	ASSERT_EQ(arch->fields[0]->kind, FIELD_COLUMN, "field 0 should be col");
	ASSERT_EQ(arch->fields[1]->kind, FIELD_COLUMN, "field 1 should be col");
	ASSERT_EQ(arch->fields[2]->kind, FIELD_COLUMN, "field 2 should be col");
	ast_program_free(prog);
	test_pass_msg();
}

/* ========== PROCEDURE TESTS ========== */

void test_proc_no_params_empty(void) {
	test_start("proc with no params, empty body");
	AstProgram *prog = parse_string("init :: proc() {}");
	ASSERT_NOT_NULL(prog, "program is null");
	ASSERT_EQ(prog->decl_count, 1, "expected 1 decl");
	ASSERT_EQ(prog->decls[0]->kind, DECL_PROC, "expected DECL_PROC");
	ProcDecl *proc = prog->decls[0]->data.proc;
	ASSERT_EQ(strcmp(proc->name, "init"), 0, "wrong proc name");
	ASSERT_EQ(proc->statement_count, 0, "expected 0 statements");
	ast_program_free(prog);
	test_pass_msg();
}

void test_proc_with_let_statement(void) {
	test_start("proc with statement");
	AstProgram *prog = parse_string("test :: proc() {\n"
	                                "  x := 42;\n"
	                                "}");
	ASSERT_NOT_NULL(prog, "program is null");
	ProcDecl *proc = prog->decls[0]->data.proc;
	ASSERT_EQ(proc->statement_count, 1, "expected 1 statement");
	ASSERT_EQ(proc->statements[0]->type, STMT_BIND, "expected STMT_BIND");
	ast_program_free(prog);
	test_pass_msg();
}

void test_proc_with_assignment(void) {
	test_start("proc with assignment statement");
	AstProgram *prog = parse_string("test :: proc() {\n"
	                                "  x = 42;\n"
	                                "}");
	ASSERT_NOT_NULL(prog, "program is null");
	ProcDecl *proc = prog->decls[0]->data.proc;
	ASSERT_EQ(proc->statement_count, 1, "expected 1 statement");
	ASSERT_EQ(proc->statements[0]->type, STMT_ASSIGN, "expected STMT_ASSIGN");
	ast_program_free(prog);
	test_pass_msg();
}

/* ========== SYSTEM TESTS ========== */

void test_sys_no_params_empty(void) {
	test_start("sys with no params, empty body");
	AstProgram *prog = parse_string("update :: sys() {}");
	ASSERT_NOT_NULL(prog, "program is null");
	ASSERT_EQ(prog->decls[0]->kind, DECL_SYS, "expected DECL_SYS");
	SysDecl *sys = prog->decls[0]->data.sys;
	ASSERT_EQ(strcmp(sys->name, "update"), 0, "wrong sys name");
	ASSERT_EQ(sys->param_count, 0, "expected 0 params");
	ASSERT_EQ(sys->statement_count, 0, "expected 0 statements");
	ast_program_free(prog);
	test_pass_msg();
}

void test_sys_with_params(void) {
	test_start("sys with params");
	AstProgram *prog = parse_string("integrate :: sys(pos, vel) {}");
	ASSERT_NOT_NULL(prog, "program is null");
	SysDecl *sys = prog->decls[0]->data.sys;
	ASSERT_EQ(sys->param_count, 2, "expected 2 params");
	ASSERT_EQ(strcmp(sys->params[0]->name, "pos"), 0, "wrong param 0");
	ASSERT_EQ(strcmp(sys->params[1]->name, "vel"), 0, "wrong param 1");
	ast_program_free(prog);
	test_pass_msg();
}

void test_sys_with_body(void) {
	test_start("sys with body statement");
	AstProgram *prog = parse_string("integrate :: sys(pos, vel) {\n"
	                                "  pos = pos + vel;\n"
	                                "}");
	ASSERT_NOT_NULL(prog, "program is null");
	SysDecl *sys = prog->decls[0]->data.sys;
	ASSERT_EQ(sys->statement_count, 1, "expected 1 statement");
	ast_program_free(prog);
	test_pass_msg();
}

/* ========== FUNCTION TESTS ========== */

void test_func_simple(void) {
	test_start("func simple");
	AstProgram *prog = parse_string("double :: func(x: Float) -> Float { x * 2; }");
	ASSERT_NOT_NULL(prog, "program is null");
	ASSERT_EQ(prog->decls[0]->kind, DECL_FUNC, "expected DECL_FUNC");
	FuncDecl *func = prog->decls[0]->data.func;
	ASSERT_EQ(strcmp(func->name, "double"), 0, "wrong func name");
	ASSERT_EQ(func->param_count, 1, "expected 1 param");
	ASSERT_EQ(strcmp(func->params[0]->name, "x"), 0, "wrong param name");
	ast_program_free(prog);
	test_pass_msg();
}

void test_func_multiple_params(void) {
	test_start("func with multiple params");
	AstProgram *prog = parse_string("add :: func(x: Float, y: Float) -> Float { x + y; }");
	ASSERT_NOT_NULL(prog, "program is null");
	FuncDecl *func = prog->decls[0]->data.func;
	ASSERT_EQ(func->param_count, 2, "expected 2 params");
	ast_program_free(prog);
	test_pass_msg();
}

/* ========== EXPRESSION TESTS ========== */

void test_expr_literal(void) {
	test_start("expression literal");
	AstProgram *prog = parse_string("test :: proc() { x := 42; }");
	ASSERT_NOT_NULL(prog, "program is null");
	ProcDecl *proc = prog->decls[0]->data.proc;
	Expression *expr = proc->statements[0]->data.bind_stmt.value;
	ASSERT_EQ(expr->type, EXPR_LITERAL, "expected EXPR_LITERAL");
	ast_program_free(prog);
	test_pass_msg();
}

void test_expr_field_access(void) {
	test_start("expression field access");
	AstProgram *prog = parse_string("test :: proc() { x := player.pos; }");
	ASSERT_NOT_NULL(prog, "program is null");
	ProcDecl *proc = prog->decls[0]->data.proc;
	Expression *expr = proc->statements[0]->data.bind_stmt.value;
	ASSERT_EQ(expr->type, EXPR_FIELD, "expected EXPR_FIELD");
	ast_program_free(prog);
	test_pass_msg();
}

void test_expr_index(void) {
	test_start("expression indexing");
	AstProgram *prog = parse_string("test :: proc() { x := arr[0]; }");
	ASSERT_NOT_NULL(prog, "program is null");
	ProcDecl *proc = prog->decls[0]->data.proc;
	Expression *expr = proc->statements[0]->data.bind_stmt.value;
	ASSERT_EQ(expr->type, EXPR_INDEX, "expected EXPR_INDEX");
	ast_program_free(prog);
	test_pass_msg();
}

void test_expr_binary_op(void) {
	test_start("expression binary operation");
	AstProgram *prog = parse_string("test :: proc() { x := a + b; }");
	ASSERT_NOT_NULL(prog, "program is null");
	ProcDecl *proc = prog->decls[0]->data.proc;
	Expression *expr = proc->statements[0]->data.bind_stmt.value;
	ASSERT_EQ(expr->type, EXPR_BINARY, "expected EXPR_BINARY");
	ast_program_free(prog);
	test_pass_msg();
}

/* ========== LET TYPE ANNOTATION TESTS ========== */

void test_let_type_annotation_with_value(void) {
	test_start("with type annotation and value");
	AstProgram *prog = parse_string("test :: proc() { x: int = 5; }");
	ASSERT_NOT_NULL(prog, "program is null");
	ProcDecl *proc = prog->decls[0]->data.proc;
	ASSERT_EQ(proc->statement_count, 1, "expected 1 statement");
	Statement *stmt = proc->statements[0];
	ASSERT_EQ(stmt->type, STMT_BIND, "expected STMT_BIND");
	ASSERT_NOT_NULL(stmt->data.bind_stmt.type, "type annotation should not be null");
	ASSERT_EQ(stmt->data.bind_stmt.type->kind, TYPE_NAME, "expected TYPE_NAME");
	ASSERT_NOT_NULL(stmt->data.bind_stmt.value, "value should not be null");
	ast_program_free(prog);
	test_pass_msg();
}

void test_let_type_annotation_no_value(void) {
	test_start("with type annotation, no value");
	AstProgram *prog = parse_string("test :: proc() { x: int; }");
	ASSERT_NOT_NULL(prog, "program is null");
	ProcDecl *proc = prog->decls[0]->data.proc;
	ASSERT_EQ(proc->statement_count, 1, "expected 1 statement");
	Statement *stmt = proc->statements[0];
	ASSERT_EQ(stmt->type, STMT_BIND, "expected STMT_BIND");
	ASSERT_NOT_NULL(stmt->data.bind_stmt.type, "type annotation should not be null");
	ASSERT_EQ(stmt->data.bind_stmt.type->kind, TYPE_NAME, "expected TYPE_NAME");
	ASSERT_EQ(stmt->data.bind_stmt.value, NULL, "value should be null");
	ast_program_free(prog);
	test_pass_msg();
}

void test_let_array_type_annotation(void) {
	test_start("with array type annotation");
	AstProgram *prog = parse_string("test :: proc() { buf: char[]; }");
	ASSERT_NOT_NULL(prog, "program is null");
	ProcDecl *proc = prog->decls[0]->data.proc;
	ASSERT_EQ(proc->statement_count, 1, "expected 1 statement");
	Statement *stmt = proc->statements[0];
	ASSERT_EQ(stmt->type, STMT_BIND, "expected STMT_BIND");
	ASSERT_NOT_NULL(stmt->data.bind_stmt.type, "type annotation should not be null");
	ASSERT_EQ(stmt->data.bind_stmt.type->kind, TYPE_ARRAY, "expected TYPE_ARRAY");
	ASSERT_EQ(stmt->data.bind_stmt.value, NULL, "value should be null");
	ast_program_free(prog);
	test_pass_msg();
}

void test_let_float_type_annotation(void) {
	test_start("with float type annotation");
	AstProgram *prog = parse_string("test :: proc() { f: float = 1.5; }");
	ASSERT_NOT_NULL(prog, "program is null");
	ProcDecl *proc = prog->decls[0]->data.proc;
	ASSERT_EQ(proc->statement_count, 1, "expected 1 statement");
	Statement *stmt = proc->statements[0];
	ASSERT_EQ(stmt->type, STMT_BIND, "expected STMT_BIND");
	ASSERT_NOT_NULL(stmt->data.bind_stmt.type, "type annotation should not be null");
	ASSERT_NOT_NULL(stmt->data.bind_stmt.value, "value should not be null");
	ast_program_free(prog);
	test_pass_msg();
}

/* ========== PRINTF VARIADIC TESTS ========== */

void test_printf_with_float(void) {
	test_start("printf with float argument");
	AstProgram *prog = parse_string("test :: proc() { printf(\"Value: %f\\n\", 3.14); }");
	ASSERT_NOT_NULL(prog, "program is null");
	ProcDecl *proc = prog->decls[0]->data.proc;
	ASSERT_EQ(proc->statement_count, 1, "expected 1 statement");
	Statement *stmt = proc->statements[0];
	ASSERT_EQ(stmt->type, STMT_EXPR, "expected STMT_EXPR");
	ASSERT_NOT_NULL(stmt->data.expr_stmt.expr, "expression should not be null");
	ASSERT_EQ(stmt->data.expr_stmt.expr->type, EXPR_CALL, "expected EXPR_CALL");
	ASSERT_EQ(stmt->data.expr_stmt.expr->data.call.arg_count, 2, "expected 2 arguments");
	ast_program_free(prog);
	test_pass_msg();
}

void test_printf_with_variable_float(void) {
	test_start("printf with variable float argument");
	AstProgram *prog = parse_string("test :: proc() { f: float = 1.5; printf(\"Float: %f\\n\", f); }");
	ASSERT_NOT_NULL(prog, "program is null");
	ProcDecl *proc = prog->decls[0]->data.proc;
	ASSERT_EQ(proc->statement_count, 2, "expected 2 statements");
	Statement *stmt = proc->statements[1];
	ASSERT_EQ(stmt->type, STMT_EXPR, "expected STMT_EXPR");
	ASSERT_EQ(stmt->data.expr_stmt.expr->type, EXPR_CALL, "expected EXPR_CALL");
	ASSERT_EQ(stmt->data.expr_stmt.expr->data.call.arg_count, 2, "expected 2 arguments");
	ast_program_free(prog);
	test_pass_msg();
}

void test_sprintf_with_float(void) {
	test_start("sprintf with float argument");
	AstProgram *prog = parse_string("test :: proc() { sprintf(\"buf\", \"Value: %f\\n\", 3.14); }");
	ASSERT_NOT_NULL(prog, "program is null");
	ProcDecl *proc = prog->decls[0]->data.proc;
	ASSERT_EQ(proc->statement_count, 1, "expected 1 statement");
	Statement *stmt = proc->statements[0];
	ASSERT_EQ(stmt->type, STMT_EXPR, "expected STMT_EXPR");
	ASSERT_EQ(stmt->data.expr_stmt.expr->type, EXPR_CALL, "expected EXPR_CALL");
	ASSERT_EQ(stmt->data.expr_stmt.expr->data.call.arg_count, 3, "expected 3 arguments");
	ast_program_free(prog);
	test_pass_msg();
}

/* ========== PARSER REGRESSION TESTS ========== */

static int parse_succeeds(const char *src) {
	ParseResult result = parse_source(src);
	int ok = (result.error_count == 0);
	parse_result_free(&result);
	return ok;
}

void test_else_break(void) {
	test_start("else clause with break in nested for");
	int ok = parse_succeeds("main :: proc() {\n"
	                        "  idx := 0;\n"
	                        "  for (;idx < 10;) {\n"
	                        "    line_pos := 0;\n"
	                        "    for (;line_pos < 5;) {\n"
	                        "      if (1 == 1) { line_pos = 1; }\n"
	                        "      else { break; }\n"
	                        "    }\n"
	                        "  }\n"
	                        "}\n");
	if (!ok) {
		test_fail_msg("parse failed");
		return;
	}
	test_pass_msg();
}

void test_else_let(void) {
	test_start("else clause with in nested for");
	int ok = parse_succeeds("main :: proc() {\n"
	                        "  idx := 0;\n"
	                        "  for (;idx < 10;) {\n"
	                        "    line_pos := 0;\n"
	                        "    for (;line_pos < 5;) {\n"
	                        "      if (1 == 1) { line_pos = 1; }\n"
	                        "      else { x := 1; }\n"
	                        "    }\n"
	                        "  }\n"
	                        "}\n");
	if (!ok) {
		test_fail_msg("parse failed");
		return;
	}
	test_pass_msg();
}

void test_else_assign(void) {
	test_start("else clause with assignment in nested for");
	int ok = parse_succeeds("main :: proc() {\n"
	                        "  idx := 0;\n"
	                        "  for (;idx < 10;) {\n"
	                        "    line_pos := 0;\n"
	                        "    for (;line_pos < 5;) {\n"
	                        "      if (1 == 1) { line_pos = 1; }\n"
	                        "      else { line_pos = 2; }\n"
	                        "    }\n"
	                        "  }\n"
	                        "}\n");
	if (!ok) {
		test_fail_msg("parse failed");
		return;
	}
	test_pass_msg();
}

void test_nested_for_no_array_index(void) {
	test_start("nested for with else, no array indexing");
	int ok = parse_succeeds("main :: proc() {\n"
	                        "  idx := 0;\n"
	                        "  for (;idx < 10;) {\n"
	                        "    n := 5;\n"
	                        "    line_pos := 0;\n"
	                        "    field_idx := 0;\n"
	                        "    for (;line_pos < n;) {\n"
	                        "      if (field_idx == 1) { line_pos = line_pos + 1; }\n"
	                        "      else { line_pos = line_pos + 1; }\n"
	                        "    }\n"
	                        "    idx = idx + 1;\n"
	                        "  }\n"
	                        "}\n");
	if (!ok) {
		test_fail_msg("parse failed");
		return;
	}
	test_pass_msg();
}

void test_nested_for_else_assign(void) {
	test_start("nested for with assignment in else");
	int ok = parse_succeeds("main :: proc() {\n"
	                        "  idx := 0;\n"
	                        "  for (;idx < 10;) {\n"
	                        "    n := 5;\n"
	                        "    line_pos := 0;\n"
	                        "    for (;line_pos < n;) {\n"
	                        "      if (1 == 1) { line_pos = line_pos + 1; }\n"
	                        "      else { line_pos = line_pos + 1; }\n"
	                        "    }\n"
	                        "    idx = idx + 1;\n"
	                        "  }\n"
	                        "}\n");
	if (!ok) {
		test_fail_msg("parse failed");
		return;
	}
	test_pass_msg();
}

void test_nested_let_simple(void) {
	test_start("declaration inside for loop");
	ParseResult result = parse_source("main :: proc() {\n"
	                                  "  for (;1 > 0;) {\n"
	                                  "    x := 5;\n"
	                                  "    break;\n"
	                                  "  }\n"
	                                  "}\n");
	if (result.error_count != 0) {
		parse_result_free(&result);
		test_fail_msg("parse errors");
		return;
	}
	parse_result_free(&result);
	test_pass_msg();
}

void test_nested_let_in_for_loop(void) {
	test_start("+ nested for inside for loop");
	AstProgram *prog = parse_string("main :: proc() {\n"
	                                "  for (;1 > 0;) {\n"
	                                "    x := 0;\n"
	                                "    for (;x < 5;) {\n"
	                                "      x = x + 1;\n"
	                                "    }\n"
	                                "    break;\n"
	                                "  }\n"
	                                "}\n");
	if (!prog) {
		test_fail_msg("parse errors");
		return;
	}
	ForStmt *for_loop = &prog->decls[0]->data.proc->statements[0]->data.for_stmt;
	if (for_loop->body_count < 2 || for_loop->body[0]->type != STMT_BIND || for_loop->body[1]->type != STMT_FOR) {
		ast_program_free(prog);
		test_fail_msg("unexpected body structure");
		return;
	}
	ast_program_free(prog);
	test_pass_msg();
}

void test_single_for_else_break(void) {
	test_start("single for with if-else-break");
	int ok = parse_succeeds("main :: proc() {\n"
	                        "  idx := 0;\n"
	                        "  for (;idx < 10;) {\n"
	                        "    if (1 == 1) { idx = 1; }\n"
	                        "    else { break; }\n"
	                        "  }\n"
	                        "}\n");
	if (!ok) {
		test_fail_msg("parse failed");
		return;
	}
	test_pass_msg();
}

void test_two_for_else_inner(void) {
	test_start("two nested fors with if-else at inner level");
	int ok = parse_succeeds("main :: proc() {\n"
	                        "  idx := 0;\n"
	                        "  for (;idx < 10;) {\n"
	                        "    pos := 0;\n"
	                        "    for (;pos < 5;) {\n"
	                        "      if (1 == 1) { pos = 1; }\n"
	                        "      else { break; }\n"
	                        "    }\n"
	                        "  }\n"
	                        "}\n");
	if (!ok) {
		test_fail_msg("parse failed");
		return;
	}
	test_pass_msg();
}

void test_printf_after_complex_nesting(void) {
	test_start("printf after deeply nested for loops");
	int ok = parse_succeeds("main :: proc() {\n"
	                        "  sum := 0.0;\n"
	                        "  for (;0 < 1;) {\n"
	                        "    idx := 0;\n"
	                        "    for (;idx < 5;) {\n"
	                        "      x := 1.0;\n"
	                        "      sum = sum + x;\n"
	                        "      idx = idx + 1;\n"
	                        "    }\n"
	                        "    break;\n"
	                        "  }\n"
	                        "  printf(\"sum: %g\\n\", sum);\n"
	                        "}\n");
	if (!ok) {
		test_fail_msg("parse failed");
		return;
	}
	test_pass_msg();
}

void test_toplevel_else(void) {
	test_start("if-else at proc top level");
	int ok = parse_succeeds("main :: proc() {\n"
	                        "  if (1 == 1) { x := 1; }\n"
	                        "  else { break; }\n"
	                        "}\n");
	if (!ok) {
		test_fail_msg("parse failed");
		return;
	}
	test_pass_msg();
}

void test_task1_structure(void) {
	test_start("complex ETL program structure");
	int ok = parse_succeeds("Transaction :: arche {\n"
	                        "  price :: float,\n"
	                        "  quantity :: int,\n"
	                        "  revenue :: float,\n"
	                        "}\n"
	                        "Transaction[1000](1000) {\n"
	                        "  price: 0.0,\n"
	                        "  quantity: 0,\n"
	                        "  revenue: 0.0,\n"
	                        "};\n"
	                        "main :: proc() {\n"
	                        "  fd := 1;\n"
	                        "  line: char[128];\n"
	                        "  idx := 0;\n"
	                        "  for (;idx < 1000;) {\n"
	                        "    n := 10;\n"
	                        "    if (n <= 0) { break; }\n"
	                        "    line_pos := 0;\n"
	                        "    field_idx := 0;\n"
	                        "    for (;line_pos < n;) {\n"
	                        "      if (line[line_pos] == ',') {\n"
	                        "        field_idx = field_idx + 1;\n"
	                        "        line_pos = line_pos + 1;\n"
	                        "      } else {\n"
	                        "        line_pos = line_pos + 1;\n"
	                        "      }\n"
	                        "    }\n"
	                        "    idx = idx + 1;\n"
	                        "  }\n"
	                        "  sum := 0.0;\n"
	                        "  m := 0;\n"
	                        "  for (;m < 1000;) {\n"
	                        "    sum = sum + 1.0;\n"
	                        "    m = m + 1;\n"
	                        "  }\n"
	                        "  printf(\"result: %g\\n\", sum);\n"
	                        "}\n");
	if (!ok) {
		test_fail_msg("parse failed");
		return;
	}
	test_pass_msg();
}

/* ========== FUNC GROUP TESTS ========== */

void test_parse_func_group(void) {
	test_start("parser: parse func group declaration");
	AstProgram *prog = parse_string("a :: func(x: int) -> int { return x; }\n"
	                                "b :: func(x: float) -> float { return x; }\n"
	                                "g :: func{ a, b }\n");
	if (!prog || prog->decl_count != 3) {
		if (prog)
			ast_program_free(prog);
		test_fail_msg("Expected 3 declarations");
		return;
	}
	if (prog->decls[2]->kind != DECL_FUNC_GROUP) {
		ast_program_free(prog);
		test_fail_msg("Third decl must be DECL_FUNC_GROUP");
		return;
	}
	FuncGroup *g = prog->decls[2]->data.func_group;
	if (!g) {
		ast_program_free(prog);
		test_fail_msg("func_group is NULL");
		return;
	}
	if (strcmp(g->name, "g") != 0) {
		ast_program_free(prog);
		test_fail_msg("group name mismatch");
		return;
	}
	if (g->member_count != 2) {
		ast_program_free(prog);
		test_fail_msg("expected 2 members");
		return;
	}
	if (strcmp(g->member_names[0], "a") != 0) {
		ast_program_free(prog);
		test_fail_msg("member[0] mismatch");
		return;
	}
	if (strcmp(g->member_names[1], "b") != 0) {
		ast_program_free(prog);
		test_fail_msg("member[1] mismatch");
		return;
	}
	ast_program_free(prog);
	test_pass_msg();
}

void test_parse_func_group_empty_rejected(void) {
	test_start("parser: empty func group rejected");
	ParseResult pr = parse_source("g :: func{ }\n");
	if (pr.error_count == 0) {
		parse_result_free(&pr);
		test_fail_msg("Expected parse error for empty group");
		return;
	}
	parse_result_free(&pr);
	test_pass_msg();
}

void test_parse_func_group_trailing_comma(void) {
	test_start("parser: func group accepts a trailing comma");
	/* Trailing commas are accepted in every comma-list (the formatter emits them when a list
	 * breaks across lines, then drops them when it collapses inline). */
	ParseResult pr = parse_source("a :: func(x: int) -> int { return x; }\n"
	                              "g :: func{ a, }\n");
	if (pr.error_count != 0) {
		parse_result_free(&pr);
		test_fail_msg("trailing comma in a func group should now parse");
		return;
	}
	parse_result_free(&pr);
	test_pass_msg();
}

/* ========== MULTIPLE DECLARATIONS ========== */

void test_multiple_decls(void) {
	test_start("multiple declarations");
	AstProgram *prog = parse_string("Player :: arche { x :: Float }\n"
	                                "init :: proc() {}\n"
	                                "integrate :: sys(pos) {}\n");
	ASSERT_NOT_NULL(prog, "program is null");
	ASSERT_EQ(prog->decl_count, 3, "expected 3 decls");
	ASSERT_EQ(prog->decls[0]->kind, DECL_ARCHETYPE, "decl 0 should be archetype");
	ASSERT_EQ(prog->decls[1]->kind, DECL_PROC, "decl 1 should be proc");
	ASSERT_EQ(prog->decls[2]->kind, DECL_SYS, "decl 2 should be sys");
	ast_program_free(prog);
	test_pass_msg();
}

/* ========== LEXER TESTS ========== */

void test_lex_move_keyword(void) {
	test_start("lex move keyword");
	Lexer lex;
	lexer_init(&lex, "move foo");
	Token t1 = lexer_next_token(&lex);
	ASSERT_EQ(t1.kind, TOK_MOVE, "first token should be TOK_MOVE");
	Token t2 = lexer_next_token(&lex);
	ASSERT_EQ(t2.kind, TOK_IDENT, "second token should be TOK_IDENT");
	lexer_free(&lex);
	test_pass_msg();
}

void test_lex_own_keyword(void) {
	test_start("lex own keyword");
	Lexer lex;
	lexer_init(&lex, "own foo");
	Token t1 = lexer_next_token(&lex);
	ASSERT_EQ(t1.kind, TOK_OWN, "first token should be TOK_OWN");
	Token t2 = lexer_next_token(&lex);
	ASSERT_EQ(t2.kind, TOK_IDENT, "second token should be TOK_IDENT");
	lexer_free(&lex);
	test_pass_msg();
}

/* ========== HANDLE TYPE TESTS ========== */

void test_parser_handle_type_is_typename(void) {
	test_start("handle(Window) over an archetype -> TYPE_HANDLE");
	AstProgram *prog = parse_string("Window :: arche { id :: int }\n"
	                                "window_open :: func(w: int, h: int) -> handle(Window) { }\n");
	ASSERT_NOT_NULL(prog, "program is null");
	ASSERT_EQ(prog->decl_count, 2, "expected 2 decls");
	ASSERT_EQ(prog->decls[1]->kind, DECL_FUNC, "expected DECL_FUNC");
	FuncDecl *f = prog->decls[1]->data.func;
	ASSERT_EQ(f->return_type_count, 1, "expected 1 return type");
	ASSERT_EQ(f->return_types[0]->kind, TYPE_HANDLE, "handle(Window) is TYPE_HANDLE");
	ASSERT_EQ(strcmp(f->return_types[0]->data.handle.archetype_name, "Window"), 0, "wrong target");
	ast_program_free(prog);
	test_pass_msg();
}

/* ========== OWN PARAMETER MODIFIER TESTS ========== */

void test_own_param_modifier(void) {
	test_start("own parameter modifier");
	AstProgram *prog = parse_string("window :: opaque\n"
	                                "#foreign { window_close :: proc(own w: window) }\n");
	ASSERT_NOT_NULL(prog, "program is null");
	ASSERT_EQ(prog->decl_count, 2, "expected 2 decls");
	ASSERT_EQ(prog->decls[1]->kind, DECL_PROC, "expected DECL_PROC");
	ProcDecl *p = prog->decls[1]->data.proc;
	ASSERT_EQ(p->param_count, 1, "expected 1 param");
	ASSERT_EQ(p->params[0]->is_own, 1, "param should be own");
	ast_program_free(prog);
	test_pass_msg();
}

void test_out_keyword_rejected(void) {
	test_start("removed `out` keyword is rejected");
	ParseResult result = parse_source("window :: opaque\n"
	                                  "#foreign { bad :: proc(out w: window) }\n");
	ASSERT_TRUE(result.error_count >= 1, "expected a parse error for `out` parameter");
	parse_result_free(&result);
	test_pass_msg();
}

/* ========== ASSIGNMENT OPERATORS ========== */

void test_assign_op_eq(void) {
	test_start("assignment operator =");
	AstProgram *prog = parse_string("test :: proc() { x = 5; }");
	ASSERT_NOT_NULL(prog, "program is null");
	ASSERT_EQ(prog->decls[0]->data.proc->statements[0]->type, STMT_ASSIGN, "wrong stmt");
	ast_program_free(prog);
	test_pass_msg();
}

void test_assign_op_plus_eq(void) {
	test_start("assignment operator +=");
	AstProgram *prog = parse_string("test :: proc() { x += 5; }");
	ASSERT_NOT_NULL(prog, "program is null");
	ast_program_free(prog);
	test_pass_msg();
}

/* ========== MAIN TEST RUNNER ========== */

int main(void) {
	printf("Running parser tests...\n\n");

	/* Lexer tests */
	printf("Lexer tests:\n");
	test_lex_move_keyword();
	test_lex_own_keyword();

	/* Archetype tests */
	printf("Archetype tests:\n");
	test_archetype_empty();
	test_archetype_meta_field();
	test_archetype_col_field();
	test_archetype_multiple_fields();

	/* Procedure tests */
	printf("\nProcedure tests:\n");
	test_proc_no_params_empty();
	test_proc_with_let_statement();
	test_proc_with_assignment();

	/* System tests */
	printf("\nSystem tests:\n");
	test_sys_no_params_empty();
	test_sys_with_params();
	test_sys_with_body();

	/* Function tests */
	printf("\nFunction tests:\n");
	test_func_simple();
	test_func_multiple_params();
	test_parse_func_group();
	test_parse_func_group_empty_rejected();
	test_parse_func_group_trailing_comma();

	/* Expression tests */
	printf("\nExpression tests:\n");
	test_expr_literal();
	test_expr_field_access();
	test_expr_index();
	test_expr_binary_op();

	/* Let type annotation tests */
	printf("\nLet type annotation tests:\n");
	test_let_type_annotation_with_value();
	test_let_type_annotation_no_value();
	test_let_array_type_annotation();
	test_let_float_type_annotation();

	/* Printf variadic tests */
	printf("\nPrintf variadic tests:\n");
	test_printf_with_float();
	test_printf_with_variable_float();
	test_sprintf_with_float();

	/* Multiple declarations */
	printf("\nMultiple declarations tests:\n");
	test_multiple_decls();

	/* Handle type tests */
	printf("\nHandle type tests:\n");
	test_parser_handle_type_is_typename();

	/* Consume parameter modifier tests */
	printf("\nConsume parameter modifier tests:\n");
	test_own_param_modifier();
	test_out_keyword_rejected();

	/* Assignment operators */
	printf("\nAssignment operator tests:\n");
	test_assign_op_eq();
	test_assign_op_plus_eq();

	/* Regression tests */
	printf("\nRegression tests:\n");
	test_else_break();
	test_else_let();
	test_else_assign();
	test_nested_for_no_array_index();
	test_nested_for_else_assign();
	test_nested_let_simple();
	test_nested_let_in_for_loop();
	test_single_for_else_break();
	test_two_for_else_inner();
	test_printf_after_complex_nesting();
	test_toplevel_else();
	test_task1_structure();

	/* Results */
	printf("\n");
	printf("Results: %d/%d passed\n", test_pass, test_count);
	if (test_fail > 0) {
		printf("  %d failed\n", test_fail);
		return 1;
	}
	return 0;
}
