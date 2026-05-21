#include "../../../cst/cst.h"
#include "../../../lexer/lexer.h"
#include "../../../parser/parser.h"
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

/* Helper to parse a string */
Program *parse_string(const char *src) {
	ParseResult result = parse_source(src);
	Program *prog = result.ast;
	parse_result_free(&result);
	return prog;
}

/* ========== ARCHETYPE TESTS ========== */

void test_archetype_empty(void) {
	test_start("archetype empty");
	Program *prog = parse_string("arche Player {}");
	ASSERT_NOT_NULL(prog, "program is null");
	ASSERT_EQ(prog->decl_count, 1, "expected 1 decl");
	ASSERT_EQ(prog->decls[0]->kind, DECL_ARCHETYPE, "expected DECL_ARCHETYPE");
	ASSERT_NOT_NULL(prog->decls[0]->data.archetype, "archetype is null");
	ASSERT_EQ(strcmp(prog->decls[0]->data.archetype->name, "Player"), 0, "wrong name");
	ASSERT_EQ(prog->decls[0]->data.archetype->field_count, 0, "expected 0 fields");
	program_free(prog);
	test_pass_msg();
}

void test_archetype_meta_field(void) {
	test_start("archetype with meta field");
	Program *prog = parse_string("arche Player {\n  drag: Float\n}");
	ASSERT_NOT_NULL(prog, "program is null");
	ASSERT_EQ(prog->decl_count, 1, "expected 1 decl");
	ArchetypeDecl *arch = prog->decls[0]->data.archetype;
	ASSERT_EQ(arch->field_count, 1, "expected 1 field");
	ASSERT_EQ(arch->fields[0]->kind, FIELD_COLUMN, "expected FIELD_COLUMN");
	ASSERT_EQ(strcmp(arch->fields[0]->name, "drag"), 0, "wrong field name");
	program_free(prog);
	test_pass_msg();
}

void test_archetype_col_field(void) {
	test_start("archetype with col field");
	Program *prog = parse_string("arche Particle {\n  pos: Float\n}");
	ASSERT_NOT_NULL(prog, "program is null");
	ArchetypeDecl *arch = prog->decls[0]->data.archetype;
	ASSERT_EQ(arch->field_count, 1, "expected 1 field");
	ASSERT_EQ(arch->fields[0]->kind, FIELD_COLUMN, "expected FIELD_COLUMN");
	program_free(prog);
	test_pass_msg();
}

void test_archetype_multiple_fields(void) {
	test_start("archetype with multiple fields");
	Program *prog = parse_string("arche Body {\n"
	                             "  drag: Float,\n"
	                             "  pos: Vec3,\n"
	                             "  vel: Vec3\n"
	                             "}");
	ASSERT_NOT_NULL(prog, "program is null");
	ArchetypeDecl *arch = prog->decls[0]->data.archetype;
	ASSERT_EQ(arch->field_count, 3, "expected 3 fields");
	ASSERT_EQ(arch->fields[0]->kind, FIELD_COLUMN, "field 0 should be col");
	ASSERT_EQ(arch->fields[1]->kind, FIELD_COLUMN, "field 1 should be col");
	ASSERT_EQ(arch->fields[2]->kind, FIELD_COLUMN, "field 2 should be col");
	program_free(prog);
	test_pass_msg();
}

/* ========== PROCEDURE TESTS ========== */

void test_proc_no_params_empty(void) {
	test_start("proc with no params, empty body");
	Program *prog = parse_string("proc init() {}");
	ASSERT_NOT_NULL(prog, "program is null");
	ASSERT_EQ(prog->decl_count, 1, "expected 1 decl");
	ASSERT_EQ(prog->decls[0]->kind, DECL_PROC, "expected DECL_PROC");
	ProcDecl *proc = prog->decls[0]->data.proc;
	ASSERT_EQ(strcmp(proc->name, "init"), 0, "wrong proc name");
	ASSERT_EQ(proc->statement_count, 0, "expected 0 statements");
	program_free(prog);
	test_pass_msg();
}

void test_proc_with_let_statement(void) {
	test_start("proc with let statement");
	Program *prog = parse_string("proc test() {\n"
	                             "  let x := 42;\n"
	                             "}");
	ASSERT_NOT_NULL(prog, "program is null");
	ProcDecl *proc = prog->decls[0]->data.proc;
	ASSERT_EQ(proc->statement_count, 1, "expected 1 statement");
	ASSERT_EQ(proc->statements[0]->type, STMT_LET, "expected STMT_LET");
	program_free(prog);
	test_pass_msg();
}

void test_proc_with_assignment(void) {
	test_start("proc with assignment statement");
	Program *prog = parse_string("proc test() {\n"
	                             "  x = 42;\n"
	                             "}");
	ASSERT_NOT_NULL(prog, "program is null");
	ProcDecl *proc = prog->decls[0]->data.proc;
	ASSERT_EQ(proc->statement_count, 1, "expected 1 statement");
	ASSERT_EQ(proc->statements[0]->type, STMT_ASSIGN, "expected STMT_ASSIGN");
	program_free(prog);
	test_pass_msg();
}

void test_proc_with_for_loop(void) {
	test_start("proc with for loop");
	Program *prog = parse_string("proc iterate() {\n"
	                             "  for item in Collection {\n"
	                             "    let x := 1;\n"
	                             "  }\n"
	                             "}");
	ASSERT_NOT_NULL(prog, "program is null");
	ProcDecl *proc = prog->decls[0]->data.proc;
	ASSERT_EQ(proc->statement_count, 1, "expected 1 statement");
	ASSERT_EQ(proc->statements[0]->type, STMT_FOR, "expected STMT_FOR");
	ForStmt *for_stmt = &proc->statements[0]->data.for_stmt;
	ASSERT_EQ(strcmp(for_stmt->var_name, "item"), 0, "wrong loop var");
	ASSERT_EQ(for_stmt->body_count, 1, "expected 1 body stmt");
	program_free(prog);
	test_pass_msg();
}

/* ========== SYSTEM TESTS ========== */

void test_sys_no_params_empty(void) {
	test_start("sys with no params, empty body");
	Program *prog = parse_string("sys update() {}");
	ASSERT_NOT_NULL(prog, "program is null");
	ASSERT_EQ(prog->decls[0]->kind, DECL_SYS, "expected DECL_SYS");
	SysDecl *sys = prog->decls[0]->data.sys;
	ASSERT_EQ(strcmp(sys->name, "update"), 0, "wrong sys name");
	ASSERT_EQ(sys->param_count, 0, "expected 0 params");
	ASSERT_EQ(sys->statement_count, 0, "expected 0 statements");
	program_free(prog);
	test_pass_msg();
}

void test_sys_with_params(void) {
	test_start("sys with params");
	Program *prog = parse_string("sys move(pos, vel) {}");
	ASSERT_NOT_NULL(prog, "program is null");
	SysDecl *sys = prog->decls[0]->data.sys;
	ASSERT_EQ(sys->param_count, 2, "expected 2 params");
	ASSERT_EQ(strcmp(sys->params[0]->name, "pos"), 0, "wrong param 0");
	ASSERT_EQ(strcmp(sys->params[1]->name, "vel"), 0, "wrong param 1");
	program_free(prog);
	test_pass_msg();
}

void test_sys_with_body(void) {
	test_start("sys with body statement");
	Program *prog = parse_string("sys move(pos, vel) {\n"
	                             "  pos = pos + vel;\n"
	                             "}");
	ASSERT_NOT_NULL(prog, "program is null");
	SysDecl *sys = prog->decls[0]->data.sys;
	ASSERT_EQ(sys->statement_count, 1, "expected 1 statement");
	program_free(prog);
	test_pass_msg();
}

/* ========== FUNCTION TESTS ========== */

void test_func_simple(void) {
	test_start("func simple");
	Program *prog = parse_string("func double(x: Float) -> Float { x * 2 }");
	ASSERT_NOT_NULL(prog, "program is null");
	ASSERT_EQ(prog->decls[0]->kind, DECL_FUNC, "expected DECL_FUNC");
	FuncDecl *func = prog->decls[0]->data.func;
	ASSERT_EQ(strcmp(func->name, "double"), 0, "wrong func name");
	ASSERT_EQ(func->param_count, 1, "expected 1 param");
	ASSERT_EQ(strcmp(func->params[0]->name, "x"), 0, "wrong param name");
	program_free(prog);
	test_pass_msg();
}

void test_func_multiple_params(void) {
	test_start("func with multiple params");
	Program *prog = parse_string("func add(x: Float, y: Float) -> Float { x + y }");
	ASSERT_NOT_NULL(prog, "program is null");
	FuncDecl *func = prog->decls[0]->data.func;
	ASSERT_EQ(func->param_count, 2, "expected 2 params");
	program_free(prog);
	test_pass_msg();
}

/* ========== EXPRESSION TESTS ========== */

void test_expr_literal(void) {
	test_start("expression literal");
	Program *prog = parse_string("proc test() { let x := 42; }");
	ASSERT_NOT_NULL(prog, "program is null");
	ProcDecl *proc = prog->decls[0]->data.proc;
	Expression *expr = proc->statements[0]->data.let_stmt.value;
	ASSERT_EQ(expr->type, EXPR_LITERAL, "expected EXPR_LITERAL");
	program_free(prog);
	test_pass_msg();
}

void test_expr_field_access(void) {
	test_start("expression field access");
	Program *prog = parse_string("proc test() { let x := player.pos; }");
	ASSERT_NOT_NULL(prog, "program is null");
	ProcDecl *proc = prog->decls[0]->data.proc;
	Expression *expr = proc->statements[0]->data.let_stmt.value;
	ASSERT_EQ(expr->type, EXPR_FIELD, "expected EXPR_FIELD");
	program_free(prog);
	test_pass_msg();
}

void test_expr_index(void) {
	test_start("expression indexing");
	Program *prog = parse_string("proc test() { let x := arr[0]; }");
	ASSERT_NOT_NULL(prog, "program is null");
	ProcDecl *proc = prog->decls[0]->data.proc;
	Expression *expr = proc->statements[0]->data.let_stmt.value;
	ASSERT_EQ(expr->type, EXPR_INDEX, "expected EXPR_INDEX");
	program_free(prog);
	test_pass_msg();
}

void test_expr_binary_op(void) {
	test_start("expression binary operation");
	Program *prog = parse_string("proc test() { let x := a + b; }");
	ASSERT_NOT_NULL(prog, "program is null");
	ProcDecl *proc = prog->decls[0]->data.proc;
	Expression *expr = proc->statements[0]->data.let_stmt.value;
	ASSERT_EQ(expr->type, EXPR_BINARY, "expected EXPR_BINARY");
	program_free(prog);
	test_pass_msg();
}

/* ========== LET TYPE ANNOTATION TESTS ========== */

void test_let_type_annotation_with_value(void) {
	test_start("let with type annotation and value");
	Program *prog = parse_string("proc test() { let x: int = 5; }");
	ASSERT_NOT_NULL(prog, "program is null");
	ProcDecl *proc = prog->decls[0]->data.proc;
	ASSERT_EQ(proc->statement_count, 1, "expected 1 statement");
	Statement *stmt = proc->statements[0];
	ASSERT_EQ(stmt->type, STMT_LET, "expected STMT_LET");
	ASSERT_NOT_NULL(stmt->data.let_stmt.type, "type annotation should not be null");
	ASSERT_EQ(stmt->data.let_stmt.type->kind, TYPE_NAME, "expected TYPE_NAME");
	ASSERT_NOT_NULL(stmt->data.let_stmt.value, "value should not be null");
	program_free(prog);
	test_pass_msg();
}

void test_let_type_annotation_no_value(void) {
	test_start("let with type annotation, no value");
	Program *prog = parse_string("proc test() { let x: int; }");
	ASSERT_NOT_NULL(prog, "program is null");
	ProcDecl *proc = prog->decls[0]->data.proc;
	ASSERT_EQ(proc->statement_count, 1, "expected 1 statement");
	Statement *stmt = proc->statements[0];
	ASSERT_EQ(stmt->type, STMT_LET, "expected STMT_LET");
	ASSERT_NOT_NULL(stmt->data.let_stmt.type, "type annotation should not be null");
	ASSERT_EQ(stmt->data.let_stmt.type->kind, TYPE_NAME, "expected TYPE_NAME");
	ASSERT_EQ(stmt->data.let_stmt.value, NULL, "value should be null");
	program_free(prog);
	test_pass_msg();
}

void test_let_array_type_annotation(void) {
	test_start("let with array type annotation");
	Program *prog = parse_string("proc test() { let buf: char[]; }");
	ASSERT_NOT_NULL(prog, "program is null");
	ProcDecl *proc = prog->decls[0]->data.proc;
	ASSERT_EQ(proc->statement_count, 1, "expected 1 statement");
	Statement *stmt = proc->statements[0];
	ASSERT_EQ(stmt->type, STMT_LET, "expected STMT_LET");
	ASSERT_NOT_NULL(stmt->data.let_stmt.type, "type annotation should not be null");
	ASSERT_EQ(stmt->data.let_stmt.type->kind, TYPE_ARRAY, "expected TYPE_ARRAY");
	ASSERT_EQ(stmt->data.let_stmt.value, NULL, "value should be null");
	program_free(prog);
	test_pass_msg();
}

void test_let_float_type_annotation(void) {
	test_start("let with float type annotation");
	Program *prog = parse_string("proc test() { let f: float = 1.5; }");
	ASSERT_NOT_NULL(prog, "program is null");
	ProcDecl *proc = prog->decls[0]->data.proc;
	ASSERT_EQ(proc->statement_count, 1, "expected 1 statement");
	Statement *stmt = proc->statements[0];
	ASSERT_EQ(stmt->type, STMT_LET, "expected STMT_LET");
	ASSERT_NOT_NULL(stmt->data.let_stmt.type, "type annotation should not be null");
	ASSERT_NOT_NULL(stmt->data.let_stmt.value, "value should not be null");
	program_free(prog);
	test_pass_msg();
}

/* ========== PRINTF VARIADIC TESTS ========== */

void test_printf_with_float(void) {
	test_start("printf with float argument");
	Program *prog = parse_string("proc test() { printf(\"Value: %f\\n\", 3.14); }");
	ASSERT_NOT_NULL(prog, "program is null");
	ProcDecl *proc = prog->decls[0]->data.proc;
	ASSERT_EQ(proc->statement_count, 1, "expected 1 statement");
	Statement *stmt = proc->statements[0];
	ASSERT_EQ(stmt->type, STMT_EXPR, "expected STMT_EXPR");
	ASSERT_NOT_NULL(stmt->data.expr_stmt.expr, "expression should not be null");
	ASSERT_EQ(stmt->data.expr_stmt.expr->type, EXPR_CALL, "expected EXPR_CALL");
	ASSERT_EQ(stmt->data.expr_stmt.expr->data.call.arg_count, 2, "expected 2 arguments");
	program_free(prog);
	test_pass_msg();
}

void test_printf_with_variable_float(void) {
	test_start("printf with variable float argument");
	Program *prog = parse_string("proc test() { let f: float = 1.5; printf(\"Float: %f\\n\", f); }");
	ASSERT_NOT_NULL(prog, "program is null");
	ProcDecl *proc = prog->decls[0]->data.proc;
	ASSERT_EQ(proc->statement_count, 2, "expected 2 statements");
	Statement *stmt = proc->statements[1];
	ASSERT_EQ(stmt->type, STMT_EXPR, "expected STMT_EXPR");
	ASSERT_EQ(stmt->data.expr_stmt.expr->type, EXPR_CALL, "expected EXPR_CALL");
	ASSERT_EQ(stmt->data.expr_stmt.expr->data.call.arg_count, 2, "expected 2 arguments");
	program_free(prog);
	test_pass_msg();
}

void test_sprintf_with_float(void) {
	test_start("sprintf with float argument");
	Program *prog = parse_string("proc test() { sprintf(\"buf\", \"Value: %f\\n\", 3.14); }");
	ASSERT_NOT_NULL(prog, "program is null");
	ProcDecl *proc = prog->decls[0]->data.proc;
	ASSERT_EQ(proc->statement_count, 1, "expected 1 statement");
	Statement *stmt = proc->statements[0];
	ASSERT_EQ(stmt->type, STMT_EXPR, "expected STMT_EXPR");
	ASSERT_EQ(stmt->data.expr_stmt.expr->type, EXPR_CALL, "expected EXPR_CALL");
	ASSERT_EQ(stmt->data.expr_stmt.expr->data.call.arg_count, 3, "expected 3 arguments");
	program_free(prog);
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
	int ok = parse_succeeds("proc main() {\n"
	                        "  let idx := 0;\n"
	                        "  for (;idx < 10;) {\n"
	                        "    let line_pos := 0;\n"
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
	test_start("else clause with let in nested for");
	int ok = parse_succeeds("proc main() {\n"
	                        "  let idx := 0;\n"
	                        "  for (;idx < 10;) {\n"
	                        "    let line_pos := 0;\n"
	                        "    for (;line_pos < 5;) {\n"
	                        "      if (1 == 1) { line_pos = 1; }\n"
	                        "      else { let x := 1; }\n"
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
	int ok = parse_succeeds("proc main() {\n"
	                        "  let idx := 0;\n"
	                        "  for (;idx < 10;) {\n"
	                        "    let line_pos := 0;\n"
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
	int ok = parse_succeeds("proc main() {\n"
	                        "  let idx := 0;\n"
	                        "  for (;idx < 10;) {\n"
	                        "    let n := 5;\n"
	                        "    let line_pos := 0;\n"
	                        "    let field_idx := 0;\n"
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
	int ok = parse_succeeds("proc main() {\n"
	                        "  let idx := 0;\n"
	                        "  for (;idx < 10;) {\n"
	                        "    let n := 5;\n"
	                        "    let line_pos := 0;\n"
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
	test_start("let declaration inside for loop");
	ParseResult result = parse_source("proc main() {\n"
	                                  "  for (;1 > 0;) {\n"
	                                  "    let x := 5;\n"
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
	test_start("let + nested for inside for loop");
	ParseResult result = parse_source("proc main() {\n"
	                                  "  for (;1 > 0;) {\n"
	                                  "    let x := 0;\n"
	                                  "    for (;x < 5;) {\n"
	                                  "      x = x + 1;\n"
	                                  "    }\n"
	                                  "    break;\n"
	                                  "  }\n"
	                                  "}\n");
	if (result.error_count != 0) {
		parse_result_free(&result);
		test_fail_msg("parse errors");
		return;
	}
	Program *prog = result.ast;
	ForStmt *for_loop = &prog->decls[0]->data.proc->statements[0]->data.for_stmt;
	if (for_loop->body_count < 2 || for_loop->body[0]->type != STMT_LET || for_loop->body[1]->type != STMT_FOR) {
		parse_result_free(&result);
		test_fail_msg("unexpected body structure");
		return;
	}
	parse_result_free(&result);
	test_pass_msg();
}

void test_single_for_else_break(void) {
	test_start("single for with if-else-break");
	int ok = parse_succeeds("proc main() {\n"
	                        "  let idx := 0;\n"
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
	int ok = parse_succeeds("proc main() {\n"
	                        "  let idx := 0;\n"
	                        "  for (;idx < 10;) {\n"
	                        "    let pos := 0;\n"
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
	int ok = parse_succeeds("proc main() {\n"
	                        "  let sum := 0.0;\n"
	                        "  for (;0 < 1;) {\n"
	                        "    let idx := 0;\n"
	                        "    for (;idx < 5;) {\n"
	                        "      let x := 1.0;\n"
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
	int ok = parse_succeeds("proc main() {\n"
	                        "  if (1 == 1) { let x := 1; }\n"
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
	int ok = parse_succeeds("arche Transaction {\n"
	                        "  price: float,\n"
	                        "  quantity: int,\n"
	                        "  revenue: float,\n"
	                        "}\n"
	                        "static Transaction(1000, 1000) {\n"
	                        "  price: 0.0,\n"
	                        "  quantity: 0,\n"
	                        "  revenue: 0.0,\n"
	                        "};\n"
	                        "proc main() {\n"
	                        "  let fd := 1;\n"
	                        "  let line: char[128];\n"
	                        "  let idx := 0;\n"
	                        "  for (;idx < 1000;) {\n"
	                        "    let n := 10;\n"
	                        "    if (n <= 0) { break; }\n"
	                        "    let line_pos := 0;\n"
	                        "    let field_idx := 0;\n"
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
	                        "  let sum := 0.0;\n"
	                        "  let m := 0;\n"
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
	ParseResult pr = parse_source("func a(x: int) -> int { return x; }\n"
	                              "func b(x: float) -> float { return x; }\n"
	                              "func g = { a, b };\n");
	if (pr.error_count != 0) {
		parse_result_free(&pr);
		test_fail_msg("Parse errors");
		return;
	}
	if (!pr.ast || pr.ast->decl_count != 3) {
		parse_result_free(&pr);
		test_fail_msg("Expected 3 declarations");
		return;
	}
	if (pr.ast->decls[2]->kind != DECL_FUNC_GROUP) {
		parse_result_free(&pr);
		test_fail_msg("Third decl must be DECL_FUNC_GROUP");
		return;
	}
	FuncGroup *g = pr.ast->decls[2]->data.func_group;
	if (!g) {
		parse_result_free(&pr);
		test_fail_msg("func_group is NULL");
		return;
	}
	if (strcmp(g->name, "g") != 0) {
		parse_result_free(&pr);
		test_fail_msg("group name mismatch");
		return;
	}
	if (g->member_count != 2) {
		parse_result_free(&pr);
		test_fail_msg("expected 2 members");
		return;
	}
	if (strcmp(g->member_names[0], "a") != 0) {
		parse_result_free(&pr);
		test_fail_msg("member[0] mismatch");
		return;
	}
	if (strcmp(g->member_names[1], "b") != 0) {
		parse_result_free(&pr);
		test_fail_msg("member[1] mismatch");
		return;
	}
	parse_result_free(&pr);
	test_pass_msg();
}

void test_parse_func_group_empty_rejected(void) {
	test_start("parser: empty func group rejected");
	ParseResult pr = parse_source("func g = { };\n");
	if (pr.error_count == 0) {
		parse_result_free(&pr);
		test_fail_msg("Expected parse error for empty group");
		return;
	}
	parse_result_free(&pr);
	test_pass_msg();
}

void test_parse_func_group_trailing_comma(void) {
	test_start("parser: func group with trailing comma rejected");
	ParseResult pr = parse_source("func a(x: int) -> int { return x; }\n"
	                              "func g = { a, };\n");
	if (pr.error_count == 0) {
		parse_result_free(&pr);
		test_fail_msg("Expected parse error for trailing comma");
		return;
	}
	parse_result_free(&pr);
	test_pass_msg();
}

/* ========== MULTIPLE DECLARATIONS ========== */

void test_multiple_decls(void) {
	test_start("multiple declarations");
	Program *prog = parse_string("arche Player { x: Float }\n"
	                             "proc init() {}\n"
	                             "sys move(pos) {}\n");
	ASSERT_NOT_NULL(prog, "program is null");
	ASSERT_EQ(prog->decl_count, 3, "expected 3 decls");
	ASSERT_EQ(prog->decls[0]->kind, DECL_ARCHETYPE, "decl 0 should be archetype");
	ASSERT_EQ(prog->decls[1]->kind, DECL_PROC, "decl 1 should be proc");
	ASSERT_EQ(prog->decls[2]->kind, DECL_SYS, "decl 2 should be sys");
	program_free(prog);
	test_pass_msg();
}

/* ========== LEXER TESTS ========== */

void test_lex_consume_keyword(void) {
	test_start("lex consume keyword");
	Lexer lex;
	lexer_init(&lex, "consume foo");
	Token t1 = lexer_next_token(&lex);
	ASSERT_EQ(t1.kind, TOK_CONSUME, "first token should be TOK_CONSUME");
	Token t2 = lexer_next_token(&lex);
	ASSERT_EQ(t2.kind, TOK_IDENT, "second token should be TOK_IDENT");
	lexer_free(&lex);
	test_pass_msg();
}

/* ========== EXTERN TYPE TESTS ========== */

void test_extern_type_decl(void) {
	test_start("extern Window(8)");
	Program *prog = parse_string("extern Window(8);");
	ASSERT_NOT_NULL(prog, "program is null");
	ASSERT_EQ(prog->decl_count, 1, "expected 1 decl");
	ASSERT_EQ(prog->decls[0]->kind, DECL_EXTERN_TYPE, "expected DECL_EXTERN_TYPE");
	ExternTypeDecl *et = prog->decls[0]->data.extern_type;
	ASSERT_NOT_NULL(et, "extern_type is null");
	ASSERT_EQ(strcmp(et->name, "Window"), 0, "wrong name");
	ASSERT_EQ(et->capacity, 8, "wrong capacity");
	program_free(prog);
	test_pass_msg();
}

/* ========== EXTERN TYPE CONTRACT TESTS ========== */

void test_parser_treats_extern_type_as_typename(void) {
	test_start("extern table referenced via handle(Window) -> TYPE_HANDLE");
	Program *prog = parse_string("extern Window(8);\n"
	                             "extern func window_open(w: int, h: int) -> handle(Window);\n");
	ASSERT_NOT_NULL(prog, "program is null");
	ASSERT_EQ(prog->decl_count, 2, "expected 2 decls");
	ASSERT_EQ(prog->decls[1]->kind, DECL_FUNC, "expected DECL_FUNC");
	FuncDecl *f = prog->decls[1]->data.func;
	ASSERT_NOT_NULL(f->return_type, "no return type");
	ASSERT_EQ(f->return_type->kind, TYPE_HANDLE, "handle(Window) is TYPE_HANDLE");
	ASSERT_EQ(strcmp(f->return_type->data.handle.archetype_name, "Window"), 0, "wrong target");
	program_free(prog);
	test_pass_msg();
}

/* ========== CONSUME PARAMETER MODIFIER TESTS ========== */

void test_consume_param_modifier(void) {
	test_start("consume parameter modifier");
	Program *prog = parse_string("extern Window(8);\n"
	                             "extern proc window_close(consume w: handle(Window));\n");
	ASSERT_NOT_NULL(prog, "program is null");
	ASSERT_EQ(prog->decl_count, 2, "expected 2 decls");
	ASSERT_EQ(prog->decls[1]->kind, DECL_PROC, "expected DECL_PROC");
	ProcDecl *p = prog->decls[1]->data.proc;
	ASSERT_EQ(p->param_count, 1, "expected 1 param");
	ASSERT_EQ(p->params[0]->is_consume, 1, "param should be consume");
	ASSERT_EQ(p->params[0]->is_out, 0, "param should NOT be out");
	program_free(prog);
	test_pass_msg();
}

void test_consume_and_out_mutually_exclusive(void) {
	test_start("consume and out cannot both apply to same param");
	ParseResult result = parse_source("extern Window(8);\n"
	                                  "extern proc bad(consume out w: handle(Window));\n");
	ASSERT_TRUE(result.error_count >= 1, "expected at least one parse error");
	parse_result_free(&result);
	test_pass_msg();
}

/* ========== ASSIGNMENT OPERATORS ========== */

void test_assign_op_eq(void) {
	test_start("assignment operator =");
	Program *prog = parse_string("proc test() { x = 5; }");
	ASSERT_NOT_NULL(prog, "program is null");
	ASSERT_EQ(prog->decls[0]->data.proc->statements[0]->type, STMT_ASSIGN, "wrong stmt");
	program_free(prog);
	test_pass_msg();
}

void test_assign_op_plus_eq(void) {
	test_start("assignment operator +=");
	Program *prog = parse_string("proc test() { x += 5; }");
	ASSERT_NOT_NULL(prog, "program is null");
	program_free(prog);
	test_pass_msg();
}

/* ========== MAIN TEST RUNNER ========== */

int main(void) {
	printf("Running parser tests...\n\n");

	/* Lexer tests */
	printf("Lexer tests:\n");
	test_lex_consume_keyword();

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
	test_proc_with_for_loop();

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

	/* Extern type tests */
	printf("\nExtern type tests:\n");
	test_extern_type_decl();
	test_parser_treats_extern_type_as_typename();

	/* Consume parameter modifier tests */
	printf("\nConsume parameter modifier tests:\n");
	test_consume_param_modifier();
	test_consume_and_out_mutually_exclusive();

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
