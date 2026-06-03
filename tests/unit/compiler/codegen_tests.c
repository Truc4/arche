#include "../../../codegen/codegen.h"
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

#ifndef ARCHE_CORE_DIR
#define ARCHE_CORE_DIR "core"
#endif

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

#define ASSERT_TRUE(cond, msg)                                                                                         \
	if (!(cond)) {                                                                                                     \
		test_fail_msg(msg);                                                                                            \
		return;                                                                                                        \
	}

#define ASSERT_FALSE(cond, msg)                                                                                        \
	if ((cond)) {                                                                                                      \
		test_fail_msg(msg);                                                                                            \
		return;                                                                                                        \
	}

/* Helper: read file into string */
static char *read_file(const char *path) {
	FILE *f = fopen(path, "r");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *buf = malloc(size + 1);
	fread(buf, 1, size, f);
	fclose(f);
	buf[size] = '\0';
	return buf;
}

/* Register all stdlib modules so example fixtures that `#import { fmt }` (etc.) resolve — mirrors
 * the real frontend's resolve_uses via the public module-registration API. Module CST + source are
 * intentionally leaked (the test process is short-lived). Registering unused modules is harmless:
 * a module is only inlined when actually `#import`ed. */
static void register_stdlib_modules(void) {
	lower_reset_modules();
	semantic_reset_modules();
	static const char *mods[] = {"os", "io", "fmt", "net", "str", "parse", "csv", "router", "term"};
	for (size_t i = 0; i < sizeof(mods) / sizeof(mods[0]); i++) {
		char path[512];
		snprintf(path, sizeof(path), "%s/%s/%s.arche", ARCHE_STDLIB_DIR, mods[i], mods[i]);
		char *src = read_file(path);
		if (!src)
			continue;
		ParseResult pr = parse_source(src);
		if (pr.cst_root) {
			lower_add_module(mods[i], pr.cst_root, src);
			semantic_add_module(mods[i], pr.cst_root, src);
			pr.cst_root = NULL; /* keep the module CST alive past parse_result_free */
		}
		parse_result_free(&pr);
		/* `src` intentionally leaked — the registered module CST points into it. */
	}
}

/* Helper: compile source to LLVM IR, check for errors */
static int compile_source(const char *source, char *ir_buf, int ir_len) {
	/* Load core library */
	char core_path[512];
	snprintf(core_path, sizeof(core_path), "%s/core.arche", ARCHE_CORE_DIR);

	FILE *core_file = fopen(core_path, "r");
	char *core_src = NULL;
	if (core_file) {
		fseek(core_file, 0, SEEK_END);
		long core_size = ftell(core_file);
		fseek(core_file, 0, SEEK_SET);
		core_src = malloc(core_size + 1);
		fread(core_src, 1, core_size, core_file);
		core_src[core_size] = '\0';
		fclose(core_file);
	}

	/* Combine core + source */
	size_t combined_len = (core_src ? strlen(core_src) : 0) + strlen(source) + 2;
	char *combined_src = malloc(combined_len);
	if (core_src) {
		snprintf(combined_src, combined_len, "%s\n%s", core_src, source);
		free(core_src);
	} else {
		snprintf(combined_src, combined_len, "%s", source);
	}

	fprintf(stderr, "DEBUG: starting parse\n");
	fflush(stderr);
	ParseResult parse_result = parse_source(combined_src);
	fprintf(stderr, "DEBUG: parse complete\n");
	fflush(stderr);

	if (parse_result.error_count > 0 || !parse_result.cst_root) {
		snprintf(ir_buf, ir_len, "Parse errors: %zu", parse_result.error_count);
		parse_result_free(&parse_result);
		free(combined_src);
		return 0;
	}

	/* CST-driven path: analyze + lower from the lossless CST. The CST + its source text
	 * (combined_src) must outlive lowering (HIR_TYPE_NAMED names point into the CST). */
	SyntaxNode *cst_root = parse_result.cst_root;
	parse_result.cst_root = NULL; /* keep the CST past parse_result_free */
	parse_result_free(&parse_result);

	register_stdlib_modules();
	fprintf(stderr, "DEBUG: starting semantic\n");
	fflush(stderr);
	SemanticContext *sem_ctx = semantic_analyze_cst(cst_root, combined_src);
	fprintf(stderr, "DEBUG: semantic complete\n");
	fflush(stderr);
	if (semantic_has_errors(sem_ctx)) {
		snprintf(ir_buf, ir_len, "Semantic errors");
		semantic_context_free(sem_ctx);
		syntax_node_free(cst_root);
		free(combined_src);
		return 0;
	}

	lower_set_model(sem_context_model(sem_ctx));
	lower_set_sem(sem_ctx);
	HirProgram *ast = lower_to_hir(cst_root, combined_src);

	fprintf(stderr, "DEBUG: starting codegen_create\n");
	fflush(stderr);
	CodegenContext *codegen_ctx = codegen_create(ast, sem_ctx);
	fprintf(stderr, "DEBUG: codegen_create complete\n");
	fflush(stderr);
	FILE *ir_output = fopen("/tmp/test_codegen.ll", "w");
	if (!ir_output) {
		snprintf(ir_buf, ir_len, "Could not open temp file");
		codegen_free(codegen_ctx);
		semantic_context_free(sem_ctx);
		hir_program_free(ast);
		syntax_node_free(cst_root);
		free(combined_src);
		return 0;
	}

	fprintf(stderr, "DEBUG: starting codegen_generate\n");
	fflush(stderr);
	codegen_generate(codegen_ctx, ir_output);
	fprintf(stderr, "DEBUG: codegen_generate complete\n");
	fflush(stderr);
	fclose(ir_output);

	codegen_free(codegen_ctx);
	semantic_context_free(sem_ctx);
	hir_program_free(ast);
	syntax_node_free(cst_root);
	free(combined_src);
	return 1;
}

/* ASSERT_NOT_NULL macro */
#define ASSERT_NOT_NULL(ptr, msg)                                                                                      \
	if ((ptr) == NULL) {                                                                                               \
		test_fail_msg(msg);                                                                                            \
		return;                                                                                                        \
	}

/* Helper: compile source string to LLVM IR and return it as a malloc'd string.
 * Does NOT prepend core.arche; intended for minimal snippets that must stand
 * alone (e.g., `extern type Window(8);`).
 * Returns NULL on any error (parse/semantic/codegen/IO). Caller must free(). */
static char *compile_to_ir_string(const char *source) {
	ParseResult parse_result = parse_source(source);
	if (parse_result.error_count > 0 || !parse_result.cst_root) {
		parse_result_free(&parse_result);
		return NULL;
	}

	/* CST-driven path. `source` (the caller's argument) backs the CST leaf spans and stays
	 * valid for the whole call, so it can be used directly through lowering. */
	SyntaxNode *cst_root = parse_result.cst_root;
	parse_result.cst_root = NULL;
	parse_result_free(&parse_result);

	SemanticContext *sem_ctx = semantic_analyze_cst(cst_root, source);
	if (semantic_has_errors(sem_ctx)) {
		semantic_context_free(sem_ctx);
		syntax_node_free(cst_root);
		return NULL;
	}

	lower_set_model(sem_context_model(sem_ctx));
	lower_set_sem(sem_ctx);
	HirProgram *ast = lower_to_hir(cst_root, source);
	CodegenContext *codegen_ctx = codegen_create(ast, sem_ctx);

	/* Write IR to a temp file. Prefer the build directory (portable across
	 * Windows native + Unix shells). Fall back to a path in the system temp
	 * dir so that re-runs don't leave stale files in the source tree. */
	const char *ir_path = "build/test_extern_type.ll";
	FILE *ir_output = fopen(ir_path, "w");
	if (!ir_output) {
		codegen_free(codegen_ctx);
		semantic_context_free(sem_ctx);
		hir_program_free(ast);
		syntax_node_free(cst_root);
		return NULL;
	}

	codegen_generate(codegen_ctx, ir_output);
	fclose(ir_output);

	codegen_free(codegen_ctx);
	semantic_context_free(sem_ctx);
	hir_program_free(ast);
	syntax_node_free(cst_root);

	return read_file(ir_path);
}

/* Test: simple.arche compiles without errors */
void test_compile_simple(void) {
	test_start("compile simple.arche");
	char *source = read_file("examples/simple/simple.arche");
	ASSERT_TRUE(source != NULL, "Could not read simple.arche");

	char ir_buf[256];
	int ok = compile_source(source, ir_buf, sizeof(ir_buf));
	ASSERT_TRUE(ok, ir_buf);

	free(source);
	test_pass_msg();
}

/* Test: hello_world.arche compiles without errors */
void test_compile_hello_world(void) {
	test_start("compile hello_world.arche");
	char *source = read_file("examples/hello_world/hello_world.arche");
	ASSERT_TRUE(source != NULL, "Could not read hello_world.arche");

	char ir_buf[256];
	int ok = compile_source(source, ir_buf, sizeof(ir_buf));
	ASSERT_TRUE(ok, ir_buf);

	free(source);
	test_pass_msg();
}

/* Test: with_params.arche compiles without errors */
void test_compile_with_params(void) {
	test_start("compile with_params.arche");
	char *source = read_file("examples/with_params/with_params.arche");
	ASSERT_TRUE(source != NULL, "Could not read with_params.arche");

	char ir_buf[256];
	int ok = compile_source(source, ir_buf, sizeof(ir_buf));
	ASSERT_TRUE(ok, ir_buf);

	free(source);
	test_pass_msg();
}

/* Test: test_archetype.arche compiles without errors */
void test_compile_archetype(void) {
	test_start("compile test_archetype.arche");
	char *source = read_file("examples/archetype/test_archetype.arche");
	ASSERT_TRUE(source != NULL, "Could not read test_archetype.arche");

	char ir_buf[256];
	int ok = compile_source(source, ir_buf, sizeof(ir_buf));
	ASSERT_TRUE(ok, ir_buf);

	free(source);
	test_pass_msg();
}

/* Test: test_archetype_verbose.arche compiles without errors */
void test_compile_archetype_verbose(void) {
	test_start("compile test_archetype_verbose.arche");
	char *source = read_file("examples/archetype/test_archetype_verbose.arche");
	ASSERT_TRUE(source != NULL, "Could not read test_archetype_verbose.arche");

	char ir_buf[256];
	int ok = compile_source(source, ir_buf, sizeof(ir_buf));
	ASSERT_TRUE(ok, ir_buf);

	free(source);
	test_pass_msg();
}

/* Test: simple_with_print.arche compiles without errors */
void test_compile_simple_with_print(void) {
	test_start("compile simple_with_print.arche");
	char *source = read_file("examples/simple_with_print/simple_with_print.arche");
	ASSERT_TRUE(source != NULL, "Could not read simple_with_print.arche");

	char ir_buf[256];
	int ok = compile_source(source, ir_buf, sizeof(ir_buf));
	ASSERT_TRUE(ok, ir_buf);

	free(source);
	test_pass_msg();
}

/* Test: function-overloading smoke compiles without errors */
void test_compile_overloads_smoke(void) {
	test_start("compile overloads/two_funcs_smoke.arche");
	char *source = read_file("tests/unit/language/overloads/two_funcs_smoke.arche");
	ASSERT_TRUE(source != NULL, "Could not read two_funcs_smoke.arche");

	char ir_buf[256];
	int ok = compile_source(source, ir_buf, sizeof(ir_buf));
	ASSERT_TRUE(ok, ir_buf);

	free(source);
	test_pass_msg();
}

/* Test: delete emits the generation-exhaustion abort. A handle is i64 = slot(low 32) |
 * gen(high 32); a slot's i32 generation bumps on each delete. At 0xFFFFFFFF it would wrap and
 * a stale handle could alias a fresh entity (ABA), so delete aborts loudly instead. Reaching it
 * needs 2^32 deletes — untestable at runtime — so assert the guard branch is emitted. */
void test_codegen_gen_exhaustion_abort(void) {
	test_start("delete emits generation-exhaustion abort");
	char *ir = compile_to_ir_string("hp :: int\n"
	                                "Unit :: arche { hp }\n"
	                                "Unit[4];\n"
	                                "main :: proc() {\n"
	                                "  h := insert(Unit, 1);\n"
	                                "  delete(h);\n"
	                                "}\n");
	ASSERT_NOT_NULL(ir, "no IR produced");
	ASSERT_TRUE(strstr(ir, "gen_exhausted") != NULL, "no generation-exhaustion abort branch in delete");
	free(ir);
	test_pass_msg();
}

int main(void) {
	printf("codegen tests\n");

	test_codegen_gen_exhaustion_abort();
	test_compile_simple();
	test_compile_hello_world();
	test_compile_with_params();
	test_compile_archetype();
	test_compile_archetype_verbose();
	test_compile_simple_with_print();
	test_compile_overloads_smoke();

	printf("\nResults: %d/%d passed\n", test_pass, test_count);
	return test_fail == 0 ? 0 : 1;
}
