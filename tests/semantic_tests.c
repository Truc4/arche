#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "../lexer/lexer.h"
#include "../parser/parser.h"
#include "../ast/ast.h"
#include "../semantic/semantic.h"

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

#define ASSERT_TRUE(cond, msg) \
	if (!(cond)) { \
		test_fail_msg(msg); \
		return; \
	}

#define ASSERT_FALSE(cond, msg) \
	if ((cond)) { \
		test_fail_msg(msg); \
		return; \
	}

#define ASSERT_EQ(a, b, msg) \
	if ((a) != (b)) { \
		test_fail_msg(msg); \
		return; \
	}

/* Helper to parse and analyze a string
   NOTE: Keeps program alive to preserve semantic analysis data */
typedef struct {
	Program *prog;
	SemanticContext *ctx;
} AnalysisResult;

AnalysisResult analyze_string(const char *src) {
	AnalysisResult result = {NULL, NULL};

	/* prepend world declaration if not already present */
	char full_src[4096];
	if (strstr(src, "world ") == NULL) {
		snprintf(full_src, sizeof(full_src), "world DefaultWorld() %s", src);
	} else {
		strncpy(full_src, src, sizeof(full_src) - 1);
		full_src[sizeof(full_src) - 1] = '\0';
	}

	Lexer lexer;
	lexer_init(&lexer, full_src);
	Parser parser;
	parser_init(&parser, &lexer);
	Program *prog = parse_program(&parser);

	if (parser.had_error) {
		return result;
	}

	result.prog = prog;
	result.ctx = semantic_analyze(prog);
	return result;
}

void analysis_result_free(AnalysisResult *result) {
	if (!result) return;
	if (result->ctx) semantic_context_free(result->ctx);
	if (result->prog) program_free(result->prog);
}

/* ========== ARCHETYPE VALIDATION TESTS ========== */

void test_archetype_defined(void) {
	test_start("archetype is registered in symbol table");
	AnalysisResult result = analyze_string("arche Player in DefaultWorld { col x: Float }");
	ASSERT_TRUE(result.ctx != NULL, "context is null");
	ASSERT_TRUE(semantic_archetype_exists(result.ctx, "Player"), "Player archetype not found");
	ASSERT_FALSE(semantic_archetype_exists(result.ctx, "Enemy"), "Enemy shouldn't exist");
	analysis_result_free(&result);
	test_pass_msg();
}

void test_archetype_field_exists(void) {
	test_start("archetype fields are registered");
	AnalysisResult result = analyze_string("arche Player in DefaultWorld { col pos: Float, meta health: Float }");
	ASSERT_TRUE(result.ctx != NULL, "context is null");
	ASSERT_TRUE(semantic_field_exists(result.ctx, "Player", "pos"), "pos field not found");
	ASSERT_TRUE(semantic_field_exists(result.ctx, "Player", "health"), "health field not found");
	ASSERT_FALSE(semantic_field_exists(result.ctx, "Player", "velocity"), "velocity shouldn't exist");
	analysis_result_free(&result);
	test_pass_msg();
}

void test_archetype_field_kind(void) {
	test_start("archetype field kind is tracked");
	AnalysisResult result = analyze_string("arche Player in DefaultWorld { meta gravity: Float, col pos: Float }");
	ASSERT_TRUE(result.ctx != NULL, "context is null");
	ASSERT_EQ(semantic_field_kind(result.ctx, "Player", "gravity"), FIELD_META, "gravity should be meta");
	ASSERT_EQ(semantic_field_kind(result.ctx, "Player", "pos"), FIELD_COLUMN, "pos should be column");
	analysis_result_free(&result);
	test_pass_msg();
}

void test_archetype_field_type(void) {
	test_start("archetype field types are tracked");
	AnalysisResult result = analyze_string("arche Player in DefaultWorld { col pos: Vec3, col health: Float }");
	ASSERT_TRUE(result.ctx != NULL, "context is null");
	const char *pos_type = semantic_field_type_name(result.ctx, "Player", "pos");
	const char *health_type = semantic_field_type_name(result.ctx, "Player", "health");
	ASSERT_TRUE(pos_type != NULL, "pos type is null");
	ASSERT_TRUE(health_type != NULL, "health type is null");
	ASSERT_TRUE(strcmp(pos_type, "Vec3") == 0, "pos should be Vec3");
	ASSERT_TRUE(strcmp(health_type, "Float") == 0, "health should be Float");
	analysis_result_free(&result);
	test_pass_msg();
}

/* ========== FIELD ACCESS VALIDATION TESTS ========== */

void test_valid_field_access_in_proc(void) {
	test_start("valid field access in procedure");
	AnalysisResult result = analyze_string(
		"arche Player in DefaultWorld { col x: Float }\n"
		"proc test() { for p in Player { let v = p.x; } }"
	);
	ASSERT_FALSE(semantic_has_errors(result.ctx), "should have no errors");
	analysis_result_free(&result);
	test_pass_msg();
}

void test_invalid_field_access_in_proc(void) {
	test_start("invalid field access caught");
	AnalysisResult result = analyze_string(
		"arche Player in DefaultWorld { col x: Float }\n"
		"proc test() { for p in Player { let v = p.missing; } }"
	);
	ASSERT_TRUE(semantic_has_errors(result.ctx), "should have error for missing field");
	analysis_result_free(&result);
	test_pass_msg();
}

void test_field_access_on_undefined_archetype(void) {
	test_start("field access on undefined archetype caught");
	AnalysisResult result = analyze_string(
		"proc test() { let v = undefined.field; }"
	);
	ASSERT_TRUE(semantic_has_errors(result.ctx), "should have error for undefined archetype");
	analysis_result_free(&result);
	test_pass_msg();
}

/* ========== VARIABLE SCOPE TESTS ========== */

void test_let_binding_creates_local(void) {
	test_start("let binding creates local variable");
	AnalysisResult result = analyze_string(
		"proc test() { let x = 42; }"
	);
	ASSERT_FALSE(semantic_has_errors(result.ctx), "should have no errors");
	analysis_result_free(&result);
	test_pass_msg();
}

void test_undefined_variable_access(void) {
	test_start("undefined variable access caught");
	AnalysisResult result = analyze_string(
		"proc test() { let x = undefined_var; }"
	);
	ASSERT_TRUE(semantic_has_errors(result.ctx), "should have error for undefined variable");
	analysis_result_free(&result);
	test_pass_msg();
}

void test_variable_shadowing(void) {
	test_start("variable shadowing allowed");
	AnalysisResult result = analyze_string(
		"proc test() {\n"
		"  let x = 1;\n"
		"  let x = 2;\n"
		"}"
	);
	ASSERT_FALSE(semantic_has_errors(result.ctx), "shadowing should be allowed");
	analysis_result_free(&result);
	test_pass_msg();
}

/* ========== TYPE CHECKING TESTS ========== */

void test_assignment_valid_type(void) {
	test_start("valid type assignment");
	AnalysisResult result = analyze_string(
		"proc test() { let x = 42; x = 100; }"
	);
	ASSERT_FALSE(semantic_has_errors(result.ctx), "should have no errors");
	analysis_result_free(&result);
	test_pass_msg();
}

void test_binary_op_same_types(void) {
	test_start("binary op with same types");
	AnalysisResult result = analyze_string(
		"proc test() { let x = 1 + 2; }"
	);
	ASSERT_FALSE(semantic_has_errors(result.ctx), "should have no errors");
	analysis_result_free(&result);
	test_pass_msg();
}

void test_comparison_produces_number(void) {
	test_start("comparison expressions are valid");
	AnalysisResult result = analyze_string(
		"proc test() { let x = 1 < 2; }"
	);
	ASSERT_FALSE(semantic_has_errors(result.ctx), "should have no errors");
	analysis_result_free(&result);
	test_pass_msg();
}

/* ========== FOR LOOP TESTS ========== */

void test_for_loop_undefined_iterable(void) {
	test_start("for loop with undefined iterable caught");
	AnalysisResult result = analyze_string(
		"proc test() { for item in UndefinedPool { let x = 1; } }"
	);
	ASSERT_TRUE(semantic_has_errors(result.ctx), "should have error for undefined pool");
	analysis_result_free(&result);
	test_pass_msg();
}

void test_for_loop_valid_iterable(void) {
	test_start("for loop with defined archetype");
	AnalysisResult result = analyze_string(
		"arche Item in DefaultWorld { col value: Float }\n"
		"proc test() { for item in Item { let x = 1; } }"
	);
	ASSERT_FALSE(semantic_has_errors(result.ctx), "should have no errors");
	analysis_result_free(&result);
	test_pass_msg();
}

/* ========== FUNCTION TESTS ========== */

void test_function_parameter_scope(void) {
	test_start("function parameters are in scope");
	AnalysisResult result = analyze_string(
		"func double(x: Float) -> Float { x * 2 }"
	);
	ASSERT_FALSE(semantic_has_errors(result.ctx), "should have no errors");
	analysis_result_free(&result);
	test_pass_msg();
}

void test_function_undefined_parameter(void) {
	test_start("undefined parameter in function caught");
	AnalysisResult result = analyze_string(
		"func test() -> Float { undefined_param + 1 }"
	);
	ASSERT_TRUE(semantic_has_errors(result.ctx), "should have error for undefined parameter");
	analysis_result_free(&result);
	test_pass_msg();
}

/* ========== MULTIPLE DECLARATIONS TESTS ========== */

void test_multiple_archetypes(void) {
	test_start("multiple archetypes are all registered");
	AnalysisResult result = analyze_string(
		"arche Player in DefaultWorld { col x: Float }\n"
		"arche Enemy in DefaultWorld { col y: Float }"
	);
	ASSERT_TRUE(semantic_archetype_exists(result.ctx, "Player"), "Player not found");
	ASSERT_TRUE(semantic_archetype_exists(result.ctx, "Enemy"), "Enemy not found");
	analysis_result_free(&result);
	test_pass_msg();
}

void test_cross_archetype_field_access(void) {
	test_start("accessing correct archetype field");
	AnalysisResult result = analyze_string(
		"arche Player in DefaultWorld { col x: Float }\n"
		"arche Enemy in DefaultWorld { col y: Float }\n"
		"proc test() { for p in Player { let v1 = p.x; } for e in Enemy { let v2 = e.y; } }"
	);
	ASSERT_FALSE(semantic_has_errors(result.ctx), "should have no errors");
	analysis_result_free(&result);
	test_pass_msg();
}

void test_cross_archetype_field_mismatch(void) {
	test_start("accessing wrong archetype field caught");
	AnalysisResult result = analyze_string(
		"arche Player in DefaultWorld { col x: Float }\n"
		"arche Enemy in DefaultWorld { col y: Float }\n"
		"proc test() { for p in Player { let v = p.y; } }"
	);
	ASSERT_TRUE(semantic_has_errors(result.ctx), "should have error for wrong field");
	analysis_result_free(&result);
	test_pass_msg();
}

/* ========== MAIN TEST RUNNER ========== */

int main(void) {
	printf("Running semantic analysis tests...\n\n");

	/* Archetype validation */
	printf("Archetype validation tests:\n");
	test_archetype_defined();
	test_archetype_field_exists();
	test_archetype_field_kind();
	test_archetype_field_type();

	/* Field access */
	printf("\nField access validation tests:\n");
	test_valid_field_access_in_proc();
	test_invalid_field_access_in_proc();
	test_field_access_on_undefined_archetype();

	/* Variable scope */
	printf("\nVariable scope tests:\n");
	test_let_binding_creates_local();
	test_undefined_variable_access();
	test_variable_shadowing();

	/* Type checking */
	printf("\nType checking tests:\n");
	test_assignment_valid_type();
	test_binary_op_same_types();
	test_comparison_produces_number();

	/* For loops */
	printf("\nFor loop tests:\n");
	test_for_loop_undefined_iterable();
	test_for_loop_valid_iterable();

	/* Functions */
	printf("\nFunction tests:\n");
	test_function_parameter_scope();
	test_function_undefined_parameter();

	/* Multiple declarations */
	printf("\nMultiple declaration tests:\n");
	test_multiple_archetypes();
	test_cross_archetype_field_access();
	test_cross_archetype_field_mismatch();

	/* Results */
	printf("\n");
	printf("Results: %d/%d passed\n", test_pass, test_count);
	if (test_fail > 0) {
		printf("  %d failed\n", test_fail);
		return 1;
	}
	return 0;
}
