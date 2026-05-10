#include "../../../ast/ast.h"
#include "../../../cst/cst.h"
#include "../../../lexer/lexer.h"
#include "../../../lower/lower.h"
#include "../../../parser/parser.h"
#include "../../../semantic/semantic.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int test_count = 0;
static int test_pass = 0;
static int test_fail = 0;

static void test_start(const char *name) {
	test_count++;
	printf("  [%d] %s ", test_count, name);
	fflush(stdout);
}

static void pass(void) {
	test_pass++;
	printf("✓\n");
}

static void fail(const char *reason) {
	test_fail++;
	printf("✗ (%s)\n", reason);
}

#define ASSERT(cond, msg)      \
	if (!(cond)) {             \
		fail(msg);             \
		return;                \
	}

static Program *parse_and_analyze(const char *src) {
	ParseResult result = parse_source(src);
	if (result.error_count > 0) {
		parse_result_free(&result);
		return NULL;
	}
	Program *prog = result.ast;
	parse_result_free(&result);
	SemanticContext *sem = semantic_analyze(prog);
	if (!sem || semantic_has_errors(sem)) {
		if (sem) semantic_context_free(sem);
		program_free(prog);
		return NULL;
	}
	semantic_context_free(sem);
	return prog;
}

/* ========== for loop kind tests ========== */

static void test_lower_range_for(void) {
	test_start("range for → AST_FOR_RANGE");
	Program *cst = parse_and_analyze(
	    "arche Particle { x: float, }\n"
	    "static Particle(100);\n"
	    "sys Move() {\n"
	    "  for p in Particle {\n"
	    "  }\n"
	    "}\n");
	ASSERT(cst, "parse/semantic failed");

	AstProgram *ast = lower_cst_to_ast(cst);
	ASSERT(ast, "lower returned NULL");
	ASSERT(ast->decl_count >= 3, "expected at least 3 decls");

	/* find sys decl */
	AstDecl *sys_decl = NULL;
	for (int i = 0; i < ast->decl_count; i++) {
		if (ast->decls[i]->kind == AST_DECL_SYS) {
			sys_decl = ast->decls[i];
			break;
		}
	}
	ASSERT(sys_decl, "no sys decl found");
	AstSysDecl *sys = sys_decl->data.sys;
	ASSERT(sys->stmt_count == 1, "expected 1 stmt");
	AstStmt *for_stmt = sys->stmts[0];
	ASSERT(for_stmt->kind == AST_STMT_FOR, "expected for stmt");
	ASSERT(for_stmt->data.for_stmt.kind == AST_FOR_RANGE, "expected AST_FOR_RANGE");
	ASSERT(for_stmt->data.for_stmt.range.var_name != NULL, "var_name is NULL");
	ASSERT(strcmp(for_stmt->data.for_stmt.range.var_name, "p") == 0, "wrong var_name");

	ast_program_free(ast);
	program_free(cst);
	pass();
}

static void test_lower_c_style_for(void) {
	test_start("c-style for → AST_FOR_C_STYLE");
	Program *cst = parse_and_analyze(
	    "proc Count() {\n"
	    "  for (let i: int = 0; i < 10; i += 1) {\n"
	    "  }\n"
	    "}\n");
	ASSERT(cst, "parse/semantic failed");

	AstProgram *ast = lower_cst_to_ast(cst);
	ASSERT(ast, "lower returned NULL");

	AstDecl *proc_decl = NULL;
	for (int i = 0; i < ast->decl_count; i++) {
		if (ast->decls[i]->kind == AST_DECL_PROC) {
			proc_decl = ast->decls[i];
			break;
		}
	}
	ASSERT(proc_decl, "no proc decl");
	AstProcDecl *proc = proc_decl->data.proc;
	ASSERT(proc->stmt_count == 1, "expected 1 stmt");
	AstStmt *for_stmt = proc->stmts[0];
	ASSERT(for_stmt->kind == AST_STMT_FOR, "expected for stmt");
	ASSERT(for_stmt->data.for_stmt.kind == AST_FOR_C_STYLE, "expected AST_FOR_C_STYLE");
	ASSERT(for_stmt->data.for_stmt.c_style.init != NULL, "init is NULL");
	ASSERT(for_stmt->data.for_stmt.c_style.cond != NULL, "cond is NULL");
	ASSERT(for_stmt->data.for_stmt.c_style.incr != NULL, "incr is NULL");

	ast_program_free(ast);
	program_free(cst);
	pass();
}

static void test_lower_while_for(void) {
	test_start("while-style for → AST_FOR_WHILE");
	Program *cst = parse_and_analyze(
	    "proc Loop() {\n"
	    "  let done: int = 0;\n"
	    "  for (;done < 5;) {\n"
	    "    done += 1;\n"
	    "  }\n"
	    "}\n");
	ASSERT(cst, "parse/semantic failed");

	AstProgram *ast = lower_cst_to_ast(cst);
	ASSERT(ast, "lower returned NULL");

	AstDecl *proc_decl = NULL;
	for (int i = 0; i < ast->decl_count; i++) {
		if (ast->decls[i]->kind == AST_DECL_PROC) {
			proc_decl = ast->decls[i];
			break;
		}
	}
	ASSERT(proc_decl, "no proc decl");
	AstProcDecl *proc = proc_decl->data.proc;
	ASSERT(proc->stmt_count == 2, "expected 2 stmts");
	AstStmt *for_stmt = proc->stmts[1];
	ASSERT(for_stmt->kind == AST_STMT_FOR, "expected for stmt");
	ASSERT(for_stmt->data.for_stmt.kind == AST_FOR_WHILE, "expected AST_FOR_WHILE");
	ASSERT(for_stmt->data.for_stmt.while_loop.cond != NULL, "cond is NULL");

	ast_program_free(ast);
	program_free(cst);
	pass();
}

static void test_lower_infinite_for(void) {
	test_start("infinite for → AST_FOR_INFINITE");
	Program *cst = parse_and_analyze(
	    "proc Loop() {\n"
	    "  for {\n"
	    "    break;\n"
	    "  }\n"
	    "}\n");
	ASSERT(cst, "parse/semantic failed");

	AstProgram *ast = lower_cst_to_ast(cst);
	ASSERT(ast, "lower returned NULL");

	AstDecl *proc_decl = NULL;
	for (int i = 0; i < ast->decl_count; i++) {
		if (ast->decls[i]->kind == AST_DECL_PROC) {
			proc_decl = ast->decls[i];
			break;
		}
	}
	ASSERT(proc_decl, "no proc decl");
	AstProcDecl *proc = proc_decl->data.proc;
	ASSERT(proc->stmt_count == 1, "expected 1 stmt");
	AstStmt *for_stmt = proc->stmts[0];
	ASSERT(for_stmt->kind == AST_STMT_FOR, "expected for stmt");
	ASSERT(for_stmt->data.for_stmt.kind == AST_FOR_INFINITE, "expected AST_FOR_INFINITE");

	ast_program_free(ast);
	program_free(cst);
	pass();
}

/* ========== let normalization ========== */

static void test_lower_single_let_normalized(void) {
	test_start("single let → names[0]");
	Program *cst = parse_and_analyze(
	    "proc Foo() {\n"
	    "  let x: int = 42;\n"
	    "}\n");
	ASSERT(cst, "parse/semantic failed");

	AstProgram *ast = lower_cst_to_ast(cst);
	ASSERT(ast, "lower returned NULL");

	AstDecl *proc_decl = NULL;
	for (int i = 0; i < ast->decl_count; i++) {
		if (ast->decls[i]->kind == AST_DECL_PROC) {
			proc_decl = ast->decls[i];
			break;
		}
	}
	ASSERT(proc_decl, "no proc decl");
	AstProcDecl *proc = proc_decl->data.proc;
	ASSERT(proc->stmt_count == 1, "expected 1 stmt");
	AstStmt *let = proc->stmts[0];
	ASSERT(let->kind == AST_STMT_LET, "expected let stmt");
	ASSERT(let->data.let_stmt.name_count == 1, "expected name_count==1");
	ASSERT(let->data.let_stmt.names != NULL, "names is NULL");
	ASSERT(strcmp(let->data.let_stmt.names[0], "x") == 0, "wrong name");

	ast_program_free(ast);
	program_free(cst);
	pass();
}

/* ========== type mapping ========== */

static void test_lower_type_int(void) {
	test_start("resolved_type int → AST_TYPE_INT");
	Program *cst = parse_and_analyze(
	    "proc Foo() {\n"
	    "  let x: int = 1;\n"
	    "  let y := x;\n"
	    "}\n");
	ASSERT(cst, "parse/semantic failed");

	AstProgram *ast = lower_cst_to_ast(cst);
	ASSERT(ast, "lower returned NULL");

	AstDecl *proc_decl = NULL;
	for (int i = 0; i < ast->decl_count; i++) {
		if (ast->decls[i]->kind == AST_DECL_PROC) {
			proc_decl = ast->decls[i];
			break;
		}
	}
	ASSERT(proc_decl, "no proc decl");
	AstProcDecl *proc = proc_decl->data.proc;
	ASSERT(proc->stmt_count == 2, "expected 2 stmts");

	/* second let: `let y := x` — x is int, so value expr resolved to int */
	AstStmt *let2 = proc->stmts[1];
	ASSERT(let2->kind == AST_STMT_LET, "expected let");
	ASSERT(let2->data.let_stmt.value != NULL, "value is NULL");
	ASSERT(let2->data.let_stmt.value->resolved.tag == AST_TYPE_INT, "expected AST_TYPE_INT");

	ast_program_free(ast);
	program_free(cst);
	pass();
}

static void test_lower_decl_use_skipped(void) {
	test_start("DECL_USE nodes skipped in AST");
	/* use csv is an existing module — inject a fake one via inline proc */
	Program *cst = parse_and_analyze(
	    "proc Foo() {\n"
	    "}\n");
	ASSERT(cst, "parse/semantic failed");

	/* Manually inject a DECL_USE into CST to verify it's stripped */
	Decl *use_decl = decl_create(DECL_USE);
	char *mod_name = malloc(9);
	strcpy(mod_name, "fake_mod");
	use_decl->data.use = use_decl_create(mod_name);

	int old_count = cst->decl_count;
	cst->decls = realloc(cst->decls, sizeof(Decl *) * (old_count + 1));
	cst->decls[old_count] = use_decl;
	cst->decl_count = old_count + 1;

	AstProgram *ast = lower_cst_to_ast(cst);
	ASSERT(ast, "lower returned NULL");

	for (int i = 0; i < ast->decl_count; i++) {
		/* No AST_DECL_* equivalent of DECL_USE should appear */
		int bad = (ast->decls[i]->kind != AST_DECL_WORLD &&
		           ast->decls[i]->kind != AST_DECL_ARCHETYPE &&
		           ast->decls[i]->kind != AST_DECL_PROC &&
		           ast->decls[i]->kind != AST_DECL_SYS &&
		           ast->decls[i]->kind != AST_DECL_FUNC &&
		           ast->decls[i]->kind != AST_DECL_STATIC &&
		           ast->decls[i]->kind != AST_DECL_CONST);
		if (bad) {
			ast_program_free(ast);
			program_free(cst);
			fail("unexpected decl kind in AST");
			return;
		}
	}

	/* AST decl count must be less than CST count (use was stripped) */
	ASSERT(ast->decl_count == old_count, "DECL_USE not stripped");

	ast_program_free(ast);
	program_free(cst);
	pass();
}

int main(void) {
	printf("lower tests\n");

	test_lower_range_for();
	test_lower_c_style_for();
	test_lower_while_for();
	test_lower_infinite_for();
	test_lower_single_let_normalized();
	test_lower_type_int();
	test_lower_decl_use_skipped();

	printf("\nResults: %d/%d passed\n", test_pass, test_count);
	return test_fail > 0 ? 1 : 0;
}
