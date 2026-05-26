#include "../../../cst/cst.h"
#include "../../../hir/hir.h"
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

#define ASSERT(cond, msg)                                                                                              \
	if (!(cond)) {                                                                                                     \
		fail(msg);                                                                                                     \
		return;                                                                                                        \
	}

/* Lowering fixture: lowering now reads the lossless CST + the semantic side model directly
 * (lower_to_hir), so the test must keep the CST, its source text, and the semantic
 * context alive together. parse_and_analyze parses + analyzes (via the CST-driven path) and
 * returns a handle; lower_fixture lowers it; fixture_free tears it all down. */
typedef struct {
	SyntaxNode *cst_root;
	char *src;
	SemanticContext *sem;
} LowerFixture;

static LowerFixture *parse_and_analyze(const char *src) {
	ParseResult result = parse_source(src);
	if (result.error_count > 0 || !result.cst_root) {
		parse_result_free(&result);
		return NULL;
	}
	SemanticContext *sem = semantic_analyze_cst(result.cst_root, src);
	if (!sem || semantic_has_errors(sem)) {
		if (sem)
			semantic_context_free(sem);
		parse_result_free(&result);
		return NULL;
	}
	LowerFixture *fx = malloc(sizeof(LowerFixture));
	/* Own the CST + a private copy of the source so leaf spans stay valid through lowering. */
	fx->cst_root = result.cst_root;
	result.cst_root = NULL; /* ownership transferred to the fixture */
	fx->src = malloc(strlen(src) + 1);
	strcpy(fx->src, src);
	fx->sem = sem;
	parse_result_free(&result);
	return fx;
}

static HirProgram *lower_fixture(LowerFixture *fx) {
	lower_set_model(sem_context_model(fx->sem));
	lower_set_sem(fx->sem);
	return lower_to_hir(fx->cst_root, fx->src);
}

static void fixture_free(LowerFixture *fx) {
	if (!fx)
		return;
	semantic_context_free(fx->sem);
	syntax_node_free(fx->cst_root);
	free(fx->src);
	free(fx);
}

/* ========== for loop tests ========== */

static void test_lower_range_for(void) {
	test_start("range for: var_name set, iterable set");
	LowerFixture *cst = parse_and_analyze("arche Particle { x :: float, }\n"
	                                      "static Particle(100);\n"
	                                      "sys Move() {\n"
	                                      "  for p in Particle {\n"
	                                      "  }\n"
	                                      "}\n");
	ASSERT(cst, "parse/semantic failed");

	HirProgram *ast = lower_fixture(cst);
	ASSERT(ast, "lower returned NULL");
	ASSERT(ast->decl_count >= 3, "expected at least 3 decls");

	HirDecl *sys_decl = NULL;
	for (int i = 0; i < ast->decl_count; i++) {
		if (ast->decls[i]->kind == HIR_DECL_SYS) {
			sys_decl = ast->decls[i];
			break;
		}
	}
	ASSERT(sys_decl, "no sys decl found");
	HirSysDecl *sys = sys_decl->data.sys;
	ASSERT(sys->stmt_count == 1, "expected 1 stmt");
	HirStmt *for_stmt = sys->stmts[0];
	ASSERT(for_stmt->kind == HIR_STMT_FOR, "expected for stmt");
	ASSERT(for_stmt->data.for_stmt.var_name != NULL, "var_name is NULL");
	ASSERT(strcmp(for_stmt->data.for_stmt.var_name, "p") == 0, "wrong var_name");
	ASSERT(for_stmt->data.for_stmt.iterable != NULL, "iterable is NULL");

	hir_program_free(ast);
	fixture_free(cst);
	pass();
}

static void test_lower_c_style_for(void) {
	test_start("c-style for: init/cond/incr set");
	LowerFixture *cst = parse_and_analyze("proc Count() {\n"
	                                      "  for (i: int = 0; i < 10; i += 1) {\n"
	                                      "  }\n"
	                                      "}\n");
	ASSERT(cst, "parse/semantic failed");

	HirProgram *ast = lower_fixture(cst);
	ASSERT(ast, "lower returned NULL");

	HirDecl *proc_decl = NULL;
	for (int i = 0; i < ast->decl_count; i++) {
		if (ast->decls[i]->kind == HIR_DECL_PROC) {
			proc_decl = ast->decls[i];
			break;
		}
	}
	ASSERT(proc_decl, "no proc decl");
	HirProcDecl *proc = proc_decl->data.proc;
	ASSERT(proc->stmt_count == 1, "expected 1 stmt");
	HirStmt *for_stmt = proc->stmts[0];
	ASSERT(for_stmt->kind == HIR_STMT_FOR, "expected for stmt");
	ASSERT(for_stmt->data.for_stmt.init != NULL, "init is NULL");
	ASSERT(for_stmt->data.for_stmt.cond != NULL, "cond is NULL");
	ASSERT(for_stmt->data.for_stmt.incr != NULL, "incr is NULL");

	hir_program_free(ast);
	fixture_free(cst);
	pass();
}

static void test_lower_while_for(void) {
	test_start("while-style for: cond set, no init/incr/var");
	LowerFixture *cst = parse_and_analyze("proc Loop() {\n"
	                                      "  done: int = 0;\n"
	                                      "  for (;done < 5;) {\n"
	                                      "    done += 1;\n"
	                                      "  }\n"
	                                      "}\n");
	ASSERT(cst, "parse/semantic failed");

	HirProgram *ast = lower_fixture(cst);
	ASSERT(ast, "lower returned NULL");

	HirDecl *proc_decl = NULL;
	for (int i = 0; i < ast->decl_count; i++) {
		if (ast->decls[i]->kind == HIR_DECL_PROC) {
			proc_decl = ast->decls[i];
			break;
		}
	}
	ASSERT(proc_decl, "no proc decl");
	HirProcDecl *proc = proc_decl->data.proc;
	ASSERT(proc->stmt_count == 2, "expected 2 stmts");
	HirStmt *for_stmt = proc->stmts[1];
	ASSERT(for_stmt->kind == HIR_STMT_FOR, "expected for stmt");
	ASSERT(for_stmt->data.for_stmt.cond != NULL, "cond is NULL");
	ASSERT(for_stmt->data.for_stmt.init == NULL, "init should be NULL");
	ASSERT(for_stmt->data.for_stmt.incr == NULL, "incr should be NULL");
	ASSERT(for_stmt->data.for_stmt.var_name == NULL, "var_name should be NULL");

	hir_program_free(ast);
	fixture_free(cst);
	pass();
}

static void test_lower_infinite_for(void) {
	test_start("infinite for: all fields NULL");
	LowerFixture *cst = parse_and_analyze("proc Loop() {\n"
	                                      "  for {\n"
	                                      "    break;\n"
	                                      "  }\n"
	                                      "}\n");
	ASSERT(cst, "parse/semantic failed");

	HirProgram *ast = lower_fixture(cst);
	ASSERT(ast, "lower returned NULL");

	HirDecl *proc_decl = NULL;
	for (int i = 0; i < ast->decl_count; i++) {
		if (ast->decls[i]->kind == HIR_DECL_PROC) {
			proc_decl = ast->decls[i];
			break;
		}
	}
	ASSERT(proc_decl, "no proc decl");
	HirProcDecl *proc = proc_decl->data.proc;
	ASSERT(proc->stmt_count == 1, "expected 1 stmt");
	HirStmt *for_stmt = proc->stmts[0];
	ASSERT(for_stmt->kind == HIR_STMT_FOR, "expected for stmt");
	ASSERT(for_stmt->data.for_stmt.var_name == NULL, "var_name should be NULL");
	ASSERT(for_stmt->data.for_stmt.cond == NULL, "cond should be NULL");
	ASSERT(for_stmt->data.for_stmt.init == NULL, "init should be NULL");

	hir_program_free(ast);
	fixture_free(cst);
	pass();
}

/* ========== normalization ========== */

static void test_lower_single_let_normalized(void) {
	test_start("single → names[0]");
	LowerFixture *cst = parse_and_analyze("proc Foo() {\n"
	                                      "  x: int = 42;\n"
	                                      "}\n");
	ASSERT(cst, "parse/semantic failed");

	HirProgram *ast = lower_fixture(cst);
	ASSERT(ast, "lower returned NULL");

	HirDecl *proc_decl = NULL;
	for (int i = 0; i < ast->decl_count; i++) {
		if (ast->decls[i]->kind == HIR_DECL_PROC) {
			proc_decl = ast->decls[i];
			break;
		}
	}
	ASSERT(proc_decl, "no proc decl");
	HirProcDecl *proc = proc_decl->data.proc;
	ASSERT(proc->stmt_count == 1, "expected 1 stmt");
	HirStmt *bind = proc->stmts[0];
	ASSERT(bind->kind == HIR_STMT_BIND, "expected stmt");
	ASSERT(bind->data.bind_stmt.name_count == 1, "expected name_count==1");
	ASSERT(bind->data.bind_stmt.names != NULL, "names is NULL");
	ASSERT(strcmp(bind->data.bind_stmt.names[0], "x") == 0, "wrong name");

	hir_program_free(ast);
	fixture_free(cst);
	pass();
}

/* ========== type mapping ========== */

static void test_lower_type_int(void) {
	test_start("resolved_type int → HIR_TYPE_INT");
	LowerFixture *cst = parse_and_analyze("proc Foo() {\n"
	                                      "  x: int = 1;\n"
	                                      "  y := x;\n"
	                                      "}\n");
	ASSERT(cst, "parse/semantic failed");

	HirProgram *ast = lower_fixture(cst);
	ASSERT(ast, "lower returned NULL");

	HirDecl *proc_decl = NULL;
	for (int i = 0; i < ast->decl_count; i++) {
		if (ast->decls[i]->kind == HIR_DECL_PROC) {
			proc_decl = ast->decls[i];
			break;
		}
	}
	ASSERT(proc_decl, "no proc decl");
	HirProcDecl *proc = proc_decl->data.proc;
	ASSERT(proc->stmt_count == 2, "expected 2 stmts");

	/* second let: `y := x` — x is int, so value expr resolved to int */
	HirStmt *let2 = proc->stmts[1];
	ASSERT(let2->kind == HIR_STMT_BIND, "expected let");
	ASSERT(let2->data.bind_stmt.value != NULL, "value is NULL");
	ASSERT(let2->data.bind_stmt.value->resolved.tag == HIR_TYPE_INT, "expected HIR_TYPE_INT");

	hir_program_free(ast);
	fixture_free(cst);
	pass();
}

static void test_lower_decl_use_skipped(void) {
	test_start("SN_USE_DECL nodes skipped in AST");
	/* A `use` of a module that was never registered (no lower_add_module) inlines nothing,
	 * so the CST's SN_USE_DECL must produce no AST decl — only the proc remains. */
	LowerFixture *cst = parse_and_analyze("use fake_mod;\n"
	                                      "proc Foo() {\n"
	                                      "}\n");
	ASSERT(cst, "parse/semantic failed");

	HirProgram *ast = lower_fixture(cst);
	ASSERT(ast, "lower returned NULL");

	for (int i = 0; i < ast->decl_count; i++) {
		/* No HIR_DECL_* equivalent of a use declaration should appear */
		int bad = (ast->decls[i]->kind != HIR_DECL_WORLD && ast->decls[i]->kind != HIR_DECL_ARCHETYPE &&
		           ast->decls[i]->kind != HIR_DECL_PROC && ast->decls[i]->kind != HIR_DECL_SYS &&
		           ast->decls[i]->kind != HIR_DECL_FUNC && ast->decls[i]->kind != HIR_DECL_STATIC &&
		           ast->decls[i]->kind != HIR_DECL_CONST);
		if (bad) {
			hir_program_free(ast);
			fixture_free(cst);
			fail("unexpected decl kind in AST");
			return;
		}
	}

	/* Only the proc survives; the use produced nothing. */
	ASSERT(ast->decl_count == 1, "SN_USE_DECL not stripped");

	hir_program_free(ast);
	fixture_free(cst);
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
