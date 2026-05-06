#include "../../../ast/ast.h"
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

#define ASSERT_TRUE(cond, msg)                                                                                         \
	if (!(cond)) {                                                                                                     \
		test_fail_msg(msg);                                                                                            \
		return;                                                                                                        \
	}

#define ASSERT_FALSE(cond, msg)                                                                                       \
	if ((cond)) {                                                                                                      \
		test_fail_msg(msg);                                                                                            \
		return;                                                                                                        \
	}

#define ASSERT_EQ(a, b, msg)                                                                                           \
	if ((a) != (b)) {                                                                                                  \
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

/* ========== HANDLE BASIC SYNTAX TESTS ========== */

void test_handle_field_declared(void) {
	test_start("handle field syntax is accepted");
	AnalysisResult result = analyze_string("arche Player { pos: Float }\n"
	                                       "arche AliveList { player_ref: handle(Player) }");
	ASSERT_TRUE(result.ctx != NULL, "context is null");
	ASSERT_FALSE(semantic_has_errors(result.ctx), "should accept handle field");
	ASSERT_TRUE(semantic_field_exists(result.ctx, "AliveList", "player_ref"), "player_ref field not found");
	analysis_result_free(&result);
	test_pass_msg();
}

void test_handle_unknown_archetype_error(void) {
	test_start("handle to unknown archetype detected");
	AnalysisResult result = analyze_string("arche AliveList { player_ref: handle(UnknownArch) }");
	ASSERT_TRUE(semantic_has_errors(result.ctx), "should error for unknown archetype");
	analysis_result_free(&result);
	test_pass_msg();
}

/* ========== INSERT RETURNS HANDLE TESTS ========== */

void test_insert_returns_handle(void) {
	test_start("insert returns handle value");
	AnalysisResult result = analyze_string("arche Player { pos: Float }\n"
	                                       "proc main() { let h := insert(Player, 1.0); }");
	ASSERT_TRUE(result.ctx != NULL, "context is null");
	ASSERT_FALSE(semantic_has_errors(result.ctx), "insert should compile");
	analysis_result_free(&result);
	test_pass_msg();
}

void test_handle_stored_in_column(void) {
	test_start("handle stored in handle column");
	AnalysisResult result = analyze_string("arche Player { pos: Float }\n"
	                                       "arche AliveList { player_ref: handle(Player) }\n"
	                                       "proc main() {\n"
	                                       "  let p := insert(Player, 1.0);\n"
	                                       "  insert(AliveList, p);\n"
	                                       "}");
	ASSERT_TRUE(result.ctx != NULL, "context is null");
	ASSERT_FALSE(semantic_has_errors(result.ctx), "handle storage should compile");
	analysis_result_free(&result);
	test_pass_msg();
}

/* ========== SYS BLOCKING TESTS ========== */

void test_handle_column_blocks_sys(void) {
	test_start("handle column cannot be sys parameter");
	AnalysisResult result = analyze_string("arche Player { pos: Float }\n"
	                                       "arche AliveList { player_ref: handle(Player) }\n"
	                                       "sys process_alive(player_ref) { }");
	ASSERT_TRUE(semantic_has_errors(result.ctx), "sys with handle column should error");
	analysis_result_free(&result);
	test_pass_msg();
}

void test_regular_column_allowed_in_sys(void) {
	test_start("regular columns allowed in sys");
	AnalysisResult result = analyze_string("arche Player { pos: Float }\n"
	                                       "sys process(pos) { }");
	ASSERT_FALSE(semantic_has_errors(result.ctx), "sys with regular column should compile");
	analysis_result_free(&result);
	test_pass_msg();
}

/* ========== TYPE MISMATCH TESTS ========== */

void test_handle_type_mismatch_delete(void) {
	test_start("delete with wrong handle type errors");
	AnalysisResult result = analyze_string("arche Player { pos: Float }\n"
	                                       "arche Enemy { health: Float }\n"
	                                       "proc main() {\n"
	                                       "  let p := insert(Player, 1.0);\n"
	                                       "  delete(Enemy, p);\n"
	                                       "}");
	ASSERT_TRUE(semantic_has_errors(result.ctx), "delete with wrong handle type should error");
	analysis_result_free(&result);
	test_pass_msg();
}

void test_handle_type_correct_delete(void) {
	test_start("delete with correct handle type succeeds");
	AnalysisResult result = analyze_string("arche Player { pos: Float }\n"
	                                       "proc main() {\n"
	                                       "  let p := insert(Player, 1.0);\n"
	                                       "  delete(Player, p);\n"
	                                       "}");
	ASSERT_FALSE(semantic_has_errors(result.ctx), "delete with matching handle type should compile");
	analysis_result_free(&result);
	test_pass_msg();
}

/* ========== MAIN TEST RUNNER ========== */

int main(void) {
	printf("Handle Type Tests\n");
	printf("==================\n\n");

	test_handle_field_declared();
	test_handle_unknown_archetype_error();
	test_insert_returns_handle();
	test_handle_stored_in_column();
	test_handle_column_blocks_sys();
	test_regular_column_allowed_in_sys();
	test_handle_type_mismatch_delete();
	test_handle_type_correct_delete();

	printf("\n==================\n");
	printf("Passed: %d/%d\n", test_pass, test_count);
	if (test_fail > 0) {
		printf("Failed: %d\n", test_fail);
		return 1;
	}
	return 0;
}
