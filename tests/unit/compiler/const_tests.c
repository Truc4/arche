#include "../../../ast/ast.h"
#include "../../../lexer/lexer.h"
#include "../../../parser/parser.h"
#include "../../../semantic/semantic.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

typedef struct {
	Program *prog;
	SemanticContext *ctx;
} AnalysisResult;

AnalysisResult analyze_string(const char *src) {
	AnalysisResult result = {NULL, NULL};

	ParseResult parse_result = parse_source(src);
	Program *prog = parse_result.ast;

	if (parse_result.error_count > 0) {
		parse_result_free(&parse_result);
		return result;
	}

	parse_result_free(&parse_result);

	result.prog = prog;
	result.ctx = semantic_analyze(prog);
	return result;
}

void analysis_result_free(AnalysisResult *result) {
	if (!result)
		return;
	if (result->ctx)
		semantic_context_free(result->ctx);
	if (result->prog)
		program_free(result->prog);
}

void test_const_basic_int(void) {
	test_start("const int declaration");
	AnalysisResult result = analyze_string("FOO :: 42");
	ASSERT_TRUE(result.ctx != NULL, "context is null");
	ASSERT_FALSE(semantic_has_errors(result.ctx), "should not error");
	const char *val = semantic_get_const_value(result.ctx, "FOO");
	ASSERT_TRUE(val != NULL, "const FOO not found");
	ASSERT_TRUE(strcmp(val, "42") == 0, "FOO should be 42");
	analysis_result_free(&result);
	test_pass_msg();
}

void test_const_basic_float(void) {
	test_start("const float declaration");
	AnalysisResult result = analyze_string("PI :: 3.14159");
	ASSERT_TRUE(result.ctx != NULL, "context is null");
	ASSERT_FALSE(semantic_has_errors(result.ctx), "should not error");
	const char *val = semantic_get_const_value(result.ctx, "PI");
	ASSERT_TRUE(val != NULL, "const PI not found");
	ASSERT_TRUE(strcmp(val, "3.14159") == 0, "PI should be 3.14159");
	analysis_result_free(&result);
	test_pass_msg();
}

void test_const_use_in_expr(void) {
	test_start("const used in expression");
	AnalysisResult result = analyze_string("FOO :: 10\nproc main() { let x := FOO; }");
	ASSERT_TRUE(result.ctx != NULL, "context is null");
	ASSERT_FALSE(semantic_has_errors(result.ctx), "should not error");
	analysis_result_free(&result);
	test_pass_msg();
}

void test_const_duplicate_error(void) {
	test_start("duplicate const error");
	AnalysisResult result = analyze_string("FOO :: 10\nFOO :: 20");
	ASSERT_TRUE(semantic_has_errors(result.ctx), "should error on duplicate const");
	analysis_result_free(&result);
	test_pass_msg();
}

void test_const_non_literal_error(void) {
	test_start("non-literal const error");
	AnalysisResult result = analyze_string("FOO :: some_var");
	ASSERT_TRUE(semantic_has_errors(result.ctx), "should error on non-literal const");
	analysis_result_free(&result);
	test_pass_msg();
}

int main(void) {
	printf("Const Type Tests\n");
	printf("==================\n\n");

	test_const_basic_int();
	test_const_basic_float();
	test_const_use_in_expr();
	test_const_duplicate_error();
	test_const_non_literal_error();

	printf("\n==================\n");
	printf("Passed: %d/%d\n", test_pass, test_count);
	if (test_fail > 0) {
		printf("Failed: %d\n", test_fail);
		return 1;
	}
	return 0;
}
