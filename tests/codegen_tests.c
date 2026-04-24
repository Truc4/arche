#include "../ast/ast.h"
#include "../codegen/codegen.h"
#include "../lexer/lexer.h"
#include "../parser/parser.h"
#include "../semantic/semantic.h"
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
	free(combined_src);

	if (parse_result.error_count > 0) {
		snprintf(ir_buf, ir_len, "Parse errors: %zu", parse_result.error_count);
		parse_result_free(&parse_result);
		return 0;
	}

	Program *prog = parse_result.ast;
	parse_result_free(&parse_result);

	fprintf(stderr, "DEBUG: starting semantic\n");
	fflush(stderr);
	SemanticContext *sem_ctx = semantic_analyze(prog);
	fprintf(stderr, "DEBUG: semantic complete\n");
	fflush(stderr);
	if (semantic_has_errors(sem_ctx)) {
		snprintf(ir_buf, ir_len, "Semantic errors");
		semantic_context_free(sem_ctx);
		program_free(prog);
		return 0;
	}

	fprintf(stderr, "DEBUG: starting codegen_create\n");
	fflush(stderr);
	CodegenContext *codegen_ctx = codegen_create(prog, sem_ctx);
	fprintf(stderr, "DEBUG: codegen_create complete\n");
	fflush(stderr);
	FILE *ir_output = fopen("/tmp/test_codegen.ll", "w");
	if (!ir_output) {
		snprintf(ir_buf, ir_len, "Could not open temp file");
		codegen_free(codegen_ctx);
		semantic_context_free(sem_ctx);
		program_free(prog);
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
	program_free(prog);
	return 1;
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

int main(void) {
	printf("codegen tests\n");

	test_compile_simple();
	test_compile_hello_world();
	test_compile_with_params();
	test_compile_archetype();
	test_compile_archetype_verbose();
	test_compile_simple_with_print();

	printf("\nResults: %d/%d passed\n", test_pass, test_count);
	return test_fail == 0 ? 0 : 1;
}
