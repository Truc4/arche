#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "../lexer/lexer.h"
#include "../ast/ast.h"
#include "../parser/parser.h"

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

#define ASSERT_NOT_NULL(ptr, msg) \
	if (!(ptr)) { \
		test_fail_msg(msg); \
		return; \
	}

#define ASSERT_EQ(a, b, msg) \
	if ((a) != (b)) { \
		test_fail_msg(msg); \
		return; \
	}

/* Helper to parse a string */
Program *parse_string(const char *src) {
	Lexer lexer;
	lexer_init(&lexer, src);
	Parser parser;
	parser_init(&parser, &lexer);
	return parse_program(&parser);
}

/* ========== ARCHETYPE TESTS ========== */

void test_archetype_empty(void) {
	test_start("archetype empty");
	Program *prog = parse_string("world GameWorld() arche Player {}");
	ASSERT_NOT_NULL(prog, "program is null");
	ASSERT_EQ(prog->decl_count, 2, "expected 2 decls (world + archetype)");
	ASSERT_EQ(prog->decls[1]->kind, DECL_ARCHETYPE, "expected DECL_ARCHETYPE");
	ASSERT_NOT_NULL(prog->decls[1]->data.archetype, "archetype is null");
	ASSERT_EQ(strcmp(prog->decls[1]->data.archetype->name, "Player"), 0, "wrong name");
	ASSERT_EQ(prog->decls[1]->data.archetype->field_count, 0, "expected 0 fields");
	program_free(prog);
	test_pass_msg();
}

void test_archetype_meta_field(void) {
	test_start("archetype with meta field");
	Program *prog = parse_string("world W() arche Player {\n  meta drag: Float\n}");
	ASSERT_NOT_NULL(prog, "program is null");
	ASSERT_EQ(prog->decl_count, 2, "expected 2 decls");
	ArchetypeDecl *arch = prog->decls[1]->data.archetype;
	ASSERT_EQ(arch->field_count, 1, "expected 1 field");
	ASSERT_EQ(arch->fields[0]->kind, FIELD_META, "expected FIELD_META");
	ASSERT_EQ(strcmp(arch->fields[0]->name, "drag"), 0, "wrong field name");
	program_free(prog);
	test_pass_msg();
}

void test_archetype_col_field(void) {
	test_start("archetype with col field");
	Program *prog = parse_string("world W() arche Particle {\n  col pos: Float\n}");
	ASSERT_NOT_NULL(prog, "program is null");
	ArchetypeDecl *arch = prog->decls[1]->data.archetype;
	ASSERT_EQ(arch->field_count, 1, "expected 1 field");
	ASSERT_EQ(arch->fields[0]->kind, FIELD_COLUMN, "expected FIELD_COLUMN");
	program_free(prog);
	test_pass_msg();
}

void test_archetype_multiple_fields(void) {
	test_start("archetype with multiple fields");
	Program *prog = parse_string(
		"world W() arche Body {\n"
		"  meta drag: Float,\n"
		"  col pos: Vec3,\n"
		"  col vel: Vec3\n"
		"}"
	);
	ASSERT_NOT_NULL(prog, "program is null");
	ArchetypeDecl *arch = prog->decls[1]->data.archetype;
	ASSERT_EQ(arch->field_count, 3, "expected 3 fields");
	ASSERT_EQ(arch->fields[0]->kind, FIELD_META, "field 0 should be meta");
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
	Program *prog = parse_string(
		"proc test() {\n"
		"  let x = 42;\n"
		"}"
	);
	ASSERT_NOT_NULL(prog, "program is null");
	ProcDecl *proc = prog->decls[0]->data.proc;
	ASSERT_EQ(proc->statement_count, 1, "expected 1 statement");
	ASSERT_EQ(proc->statements[0]->type, STMT_LET, "expected STMT_LET");
	program_free(prog);
	test_pass_msg();
}

void test_proc_with_assignment(void) {
	test_start("proc with assignment statement");
	Program *prog = parse_string(
		"proc test() {\n"
		"  x = 42;\n"
		"}"
	);
	ASSERT_NOT_NULL(prog, "program is null");
	ProcDecl *proc = prog->decls[0]->data.proc;
	ASSERT_EQ(proc->statement_count, 1, "expected 1 statement");
	ASSERT_EQ(proc->statements[0]->type, STMT_ASSIGN, "expected STMT_ASSIGN");
	program_free(prog);
	test_pass_msg();
}

void test_proc_with_for_loop(void) {
	test_start("proc with for loop");
	Program *prog = parse_string(
		"proc iterate() {\n"
		"  for item in Collection {\n"
		"    let x = 1;\n"
		"  }\n"
		"}"
	);
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
	Program *prog = parse_string(
		"sys move(pos, vel) {\n"
		"  pos = pos + vel;\n"
		"}"
	);
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
	Program *prog = parse_string("proc test() { let x = 42; }");
	ASSERT_NOT_NULL(prog, "program is null");
	ProcDecl *proc = prog->decls[0]->data.proc;
	Expression *expr = proc->statements[0]->data.let_stmt.value;
	ASSERT_EQ(expr->type, EXPR_LITERAL, "expected EXPR_LITERAL");
	program_free(prog);
	test_pass_msg();
}

void test_expr_field_access(void) {
	test_start("expression field access");
	Program *prog = parse_string("proc test() { let x = player.pos; }");
	ASSERT_NOT_NULL(prog, "program is null");
	ProcDecl *proc = prog->decls[0]->data.proc;
	Expression *expr = proc->statements[0]->data.let_stmt.value;
	ASSERT_EQ(expr->type, EXPR_FIELD, "expected EXPR_FIELD");
	program_free(prog);
	test_pass_msg();
}

void test_expr_index(void) {
	test_start("expression indexing");
	Program *prog = parse_string("proc test() { let x = arr[0]; }");
	ASSERT_NOT_NULL(prog, "program is null");
	ProcDecl *proc = prog->decls[0]->data.proc;
	Expression *expr = proc->statements[0]->data.let_stmt.value;
	ASSERT_EQ(expr->type, EXPR_INDEX, "expected EXPR_INDEX");
	program_free(prog);
	test_pass_msg();
}

void test_expr_binary_op(void) {
	test_start("expression binary operation");
	Program *prog = parse_string("proc test() { let x = a + b; }");
	ASSERT_NOT_NULL(prog, "program is null");
	ProcDecl *proc = prog->decls[0]->data.proc;
	Expression *expr = proc->statements[0]->data.let_stmt.value;
	ASSERT_EQ(expr->type, EXPR_BINARY, "expected EXPR_BINARY");
	program_free(prog);
	test_pass_msg();
}

/* ========== MULTIPLE DECLARATIONS ========== */

void test_multiple_decls(void) {
	test_start("multiple declarations");
	Program *prog = parse_string(
		"world W() arche Player { col x: Float }\n"
		"proc init() {}\n"
		"sys move(pos) {}\n"
	);
	ASSERT_NOT_NULL(prog, "program is null");
	ASSERT_EQ(prog->decl_count, 4, "expected 4 decls");
	ASSERT_EQ(prog->decls[0]->kind, DECL_WORLD, "decl 0 should be world");
	ASSERT_EQ(prog->decls[1]->kind, DECL_ARCHETYPE, "decl 1 should be archetype");
	ASSERT_EQ(prog->decls[2]->kind, DECL_PROC, "decl 2 should be proc");
	ASSERT_EQ(prog->decls[3]->kind, DECL_SYS, "decl 3 should be sys");
	program_free(prog);
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

	/* Expression tests */
	printf("\nExpression tests:\n");
	test_expr_literal();
	test_expr_field_access();
	test_expr_index();
	test_expr_binary_op();

	/* Multiple declarations */
	printf("\nMultiple declarations tests:\n");
	test_multiple_decls();

	/* Assignment operators */
	printf("\nAssignment operator tests:\n");
	test_assign_op_eq();
	test_assign_op_plus_eq();

	/* Results */
	printf("\n");
	printf("Results: %d/%d passed\n", test_pass, test_count);
	if (test_fail > 0) {
		printf("  %d failed\n", test_fail);
		return 1;
	}
	return 0;
}
