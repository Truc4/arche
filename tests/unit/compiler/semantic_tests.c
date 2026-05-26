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

#define ASSERT_EQ(a, b, msg)                                                                                           \
	if ((a) != (b)) {                                                                                                  \
		test_fail_msg(msg);                                                                                            \
		return;                                                                                                        \
	}

#define ASSERT_NOT_NULL(ptr, msg)                                                                                      \
	if ((ptr) == NULL) {                                                                                               \
		test_fail_msg(msg);                                                                                            \
		return;                                                                                                        \
	}

/* Helper to parse and analyze a string.
 * The parser produces only the lossless CST; analysis runs via semantic_analyze_cst, which
 * reconstructs its analyzable AstProgram from that CST (owned by the context). cst_to_program
 * copies every string out of the source, so the CST + source can be freed once analysis
 * returns. These tests therefore validate the CST-driven semantic path end to end. */
typedef struct {
	SemanticContext *ctx;
} AnalysisResult;

AnalysisResult analyze_string(const char *src) {
	AnalysisResult result = {NULL};

	ParseResult parse_result = parse_source(src);
	if (parse_result.error_count > 0 || !parse_result.cst_root) {
		parse_result_free(&parse_result);
		return result;
	}

	result.ctx = semantic_analyze_cst(parse_result.cst_root, src);
	parse_result_free(&parse_result);
	return result;
}

void analysis_result_free(AnalysisResult *result) {
	if (!result)
		return;
	if (result->ctx)
		semantic_context_free(result->ctx);
}

/* ========== ARCHETYPE VALIDATION TESTS ========== */

void test_archetype_defined(void) {
	test_start("archetype is registered in symbol table");
	AnalysisResult result = analyze_string("arche Player { x :: Float }");
	ASSERT_TRUE(result.ctx != NULL, "context is null");
	ASSERT_TRUE(semantic_archetype_exists(result.ctx, "Player"), "Player archetype not found");
	ASSERT_FALSE(semantic_archetype_exists(result.ctx, "Enemy"), "Enemy shouldn't exist");
	analysis_result_free(&result);
	test_pass_msg();
}

void test_archetype_field_exists(void) {
	test_start("archetype fields are registered");
	AnalysisResult result = analyze_string("arche Player { pos :: Float, health :: Float }");
	ASSERT_TRUE(result.ctx != NULL, "context is null");
	ASSERT_TRUE(semantic_field_exists(result.ctx, "Player", "pos"), "pos field not found");
	ASSERT_TRUE(semantic_field_exists(result.ctx, "Player", "health"), "health field not found");
	ASSERT_FALSE(semantic_field_exists(result.ctx, "Player", "velocity"), "velocity shouldn't exist");
	analysis_result_free(&result);
	test_pass_msg();
}

void test_archetype_field_kind(void) {
	test_start("archetype field kind is tracked");
	AnalysisResult result = analyze_string("arche Player { gravity :: Float, pos :: Float }");
	ASSERT_TRUE(result.ctx != NULL, "context is null");
	ASSERT_EQ(semantic_field_kind(result.ctx, "Player", "gravity"), FIELD_COLUMN, "gravity should be column");
	ASSERT_EQ(semantic_field_kind(result.ctx, "Player", "pos"), FIELD_COLUMN, "pos should be column");
	analysis_result_free(&result);
	test_pass_msg();
}

void test_archetype_field_type(void) {
	test_start("archetype field types are tracked");
	AnalysisResult result = analyze_string("arche Player { pos :: Vec3, health :: Float }");
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
	AnalysisResult result = analyze_string("arche Player { x :: Float }\n"
	                                       "proc test() { for p in Player { v := p.x; } }");
	ASSERT_FALSE(semantic_has_errors(result.ctx), "should have no errors");
	analysis_result_free(&result);
	test_pass_msg();
}

void test_invalid_field_access_in_proc(void) {
	test_start("invalid field access caught");
	AnalysisResult result = analyze_string("arche Player { x :: Float }\n"
	                                       "proc test() { for p in Player { v := p.missing; } }");
	ASSERT_TRUE(semantic_has_errors(result.ctx), "should have error for missing field");
	analysis_result_free(&result);
	test_pass_msg();
}

void test_field_access_on_undefined_archetype(void) {
	test_start("field access on undefined archetype caught");
	AnalysisResult result = analyze_string("proc test() { v := undefined.field; }");
	ASSERT_TRUE(semantic_has_errors(result.ctx), "should have error for undefined archetype");
	analysis_result_free(&result);
	test_pass_msg();
}

/* ========== VARIABLE SCOPE TESTS ========== */

void test_let_binding_creates_local(void) {
	test_start("binding creates local variable");
	AnalysisResult result = analyze_string("proc test() { x := 42; }");
	ASSERT_FALSE(semantic_has_errors(result.ctx), "should have no errors");
	analysis_result_free(&result);
	test_pass_msg();
}

void test_undefined_variable_access(void) {
	test_start("undefined variable access caught");
	AnalysisResult result = analyze_string("proc test() { x := undefined_var; }");
	ASSERT_TRUE(semantic_has_errors(result.ctx), "should have error for undefined variable");
	analysis_result_free(&result);
	test_pass_msg();
}

void test_variable_shadowing(void) {
	test_start("variable shadowing allowed");
	AnalysisResult result = analyze_string("proc test() {\n"
	                                       "  x := 1;\n"
	                                       "  x := 2;\n"
	                                       "}");
	ASSERT_FALSE(semantic_has_errors(result.ctx), "shadowing should be allowed");
	analysis_result_free(&result);
	test_pass_msg();
}

/* ========== TYPE CHECKING TESTS ========== */

void test_assignment_valid_type(void) {
	test_start("valid type assignment");
	AnalysisResult result = analyze_string("proc test() { x := 42; x = 100; }");
	ASSERT_FALSE(semantic_has_errors(result.ctx), "should have no errors");
	analysis_result_free(&result);
	test_pass_msg();
}

void test_binary_op_same_types(void) {
	test_start("binary op with same types");
	AnalysisResult result = analyze_string("proc test() { x := 1 + 2; }");
	ASSERT_FALSE(semantic_has_errors(result.ctx), "should have no errors");
	analysis_result_free(&result);
	test_pass_msg();
}

void test_comparison_produces_number(void) {
	test_start("comparison expressions are valid");
	AnalysisResult result = analyze_string("proc test() { x := 1 < 2; }");
	ASSERT_FALSE(semantic_has_errors(result.ctx), "should have no errors");
	analysis_result_free(&result);
	test_pass_msg();
}

/* ========== FOR LOOP TESTS ========== */

void test_for_loop_undefined_iterable(void) {
	test_start("for loop with undefined iterable caught");
	AnalysisResult result = analyze_string("proc test() { for item in UndefinedPool { x := 1; } }");
	ASSERT_TRUE(semantic_has_errors(result.ctx), "should have error for undefined pool");
	analysis_result_free(&result);
	test_pass_msg();
}

void test_for_loop_valid_iterable(void) {
	test_start("for loop with defined archetype");
	AnalysisResult result = analyze_string("arche Item { value :: Float }\n"
	                                       "proc test() { for item in Item { x := 1; } }");
	ASSERT_FALSE(semantic_has_errors(result.ctx), "should have no errors");
	analysis_result_free(&result);
	test_pass_msg();
}

/* ========== FUNCTION TESTS ========== */

void test_function_parameter_scope(void) {
	test_start("function parameters are in scope");
	AnalysisResult result = analyze_string("func double(x: Float) -> Float { return x * 2; }");
	ASSERT_FALSE(semantic_has_errors(result.ctx), "should have no errors");
	analysis_result_free(&result);
	test_pass_msg();
}

void test_function_undefined_parameter(void) {
	test_start("undefined parameter in function caught");
	AnalysisResult result = analyze_string("func test() -> Float { return undefined_param + 1; }");
	ASSERT_TRUE(semantic_has_errors(result.ctx), "should have error for undefined parameter");
	analysis_result_free(&result);
	test_pass_msg();
}

/* ========== MULTIPLE DECLARATIONS TESTS ========== */

void test_multiple_archetypes(void) {
	test_start("multiple archetypes are all registered");
	AnalysisResult result = analyze_string("arche Player { x :: Float }\n"
	                                       "arche Enemy { y :: Float }");
	ASSERT_TRUE(semantic_archetype_exists(result.ctx, "Player"), "Player not found");
	ASSERT_TRUE(semantic_archetype_exists(result.ctx, "Enemy"), "Enemy not found");
	analysis_result_free(&result);
	test_pass_msg();
}

void test_cross_archetype_field_access(void) {
	test_start("accessing correct archetype field");
	AnalysisResult result =
	    analyze_string("arche Player { x :: Float }\n"
	                   "arche Enemy { y :: Float }\n"
	                   "proc test() { for p in Player { v1 := p.x; } for e in Enemy { v2 := e.y; } }");
	ASSERT_FALSE(semantic_has_errors(result.ctx), "should have no errors");
	analysis_result_free(&result);
	test_pass_msg();
}

void test_cross_archetype_field_mismatch(void) {
	test_start("accessing wrong archetype field caught");
	AnalysisResult result = analyze_string("arche Player { x :: Float }\n"
	                                       "arche Enemy { y :: Float }\n"
	                                       "proc test() { for p in Player { v := p.y; } }");
	ASSERT_TRUE(semantic_has_errors(result.ctx), "should have error for wrong field");
	analysis_result_free(&result);
	test_pass_msg();
}

/* ========== FUNC GROUP TESTS ========== */

void test_group_with_distinct_members_ok(void) {
	test_start("semantic: group with two distinct-signature members ok");
	const char *src = "func a(x: int) -> int { return x; }\n"
	                  "func b(x: float) -> float { return x; }\n"
	                  "func g = { a, b };\n"
	                  "proc main() { i := g(1); f := g(2.0); }\n";
	ParseResult pr = parse_source(src);
	if (pr.error_count != 0) {
		test_fail_msg("Parse errors");
		return;
	}
	SemanticContext *sem = semantic_analyze_cst(pr.cst_root, src);
	if (semantic_has_errors(sem)) {
		test_fail_msg("Unexpected semantic errors");
		semantic_context_free(sem);
		parse_result_free(&pr);
		return;
	}
	semantic_context_free(sem);
	parse_result_free(&pr);
	test_pass_msg();
}

void test_group_member_unknown_errors(void) {
	test_start("semantic: group with unknown member errors");
	const char *src = "func a(x: int) -> int { return x; }\n"
	                  "func g = { a, missing };\n";
	ParseResult pr = parse_source(src);
	if (pr.error_count != 0) {
		test_fail_msg("Parse errors");
		return;
	}
	SemanticContext *sem = semantic_analyze_cst(pr.cst_root, src);
	if (!semantic_has_errors(sem)) {
		test_fail_msg("Expected error for unknown member");
		semantic_context_free(sem);
		parse_result_free(&pr);
		return;
	}
	semantic_context_free(sem);
	parse_result_free(&pr);
	test_pass_msg();
}

void test_group_duplicate_signature_errors(void) {
	test_start("semantic: group with two same-signature members errors");
	const char *src = "func a(x: int) -> int { return x; }\n"
	                  "func b(x: int) -> int { return x + 1; }\n"
	                  "func g = { a, b };\n";
	ParseResult pr = parse_source(src);
	if (pr.error_count != 0) {
		test_fail_msg("Parse errors");
		return;
	}
	SemanticContext *sem = semantic_analyze_cst(pr.cst_root, src);
	if (!semantic_has_errors(sem)) {
		test_fail_msg("Expected duplicate-signature error");
		semantic_context_free(sem);
		parse_result_free(&pr);
		return;
	}
	semantic_context_free(sem);
	parse_result_free(&pr);
	test_pass_msg();
}

void test_group_name_collision_errors(void) {
	test_start("semantic: group name colliding with existing func errors");
	const char *src = "func a(x: int) -> int { return x; }\n"
	                  "func b(x: float) -> float { return x; }\n"
	                  "func a = { a, b };\n";
	ParseResult pr = parse_source(src);
	if (pr.error_count != 0) {
		test_fail_msg("Parse errors");
		return;
	}
	SemanticContext *sem = semantic_analyze_cst(pr.cst_root, src);
	if (!semantic_has_errors(sem)) {
		test_fail_msg("Expected name-collision error");
		semantic_context_free(sem);
		parse_result_free(&pr);
		return;
	}
	semantic_context_free(sem);
	parse_result_free(&pr);
	test_pass_msg();
}

void test_call_no_matching_member_errors(void) {
	test_start("semantic: call to group with no matching member errors");
	const char *src = "func a(x: int) -> int { return x; }\n"
	                  "func b(x: float) -> float { return x; }\n"
	                  "func g = { a, b };\n"
	                  "proc main() { c := 'X'; r := g(c); }\n";
	ParseResult pr = parse_source(src);
	if (pr.error_count != 0) {
		test_fail_msg("Parse errors");
		return;
	}
	SemanticContext *sem = semantic_analyze_cst(pr.cst_root, src);
	if (!semantic_has_errors(sem)) {
		test_fail_msg("Expected no-matching-member error");
		semantic_context_free(sem);
		parse_result_free(&pr);
		return;
	}
	semantic_context_free(sem);
	parse_result_free(&pr);
	test_pass_msg();
}

/* ========== FOREIGN OPAQUE TYPE TESTS ========== */

void test_opaque_passthrough_in_proc_ok(void) {
	test_start("opaque value may pass through a non-extern proc param");
	AnalysisResult r = analyze_string("window :: opaque\n"
	                                  "extern proc window_close(own w: window);\n"
	                                  "proc wrap_close(w: window) { window_close(move w); }\n");
	ASSERT_EQ(semantic_error_count(r.ctx), 0, "should be no errors");
	semantic_context_free(r.ctx);
	test_pass_msg();
}

void test_unknown_type_name_still_errors(void) {
	test_start("unknown type name in extern signature is still an error");
	AnalysisResult r = analyze_string("extern func bad() -> Doesnotexist;\n");
	ASSERT_TRUE(semantic_error_count(r.ctx) >= 1, "expected unknown-type error");
	semantic_context_free(r.ctx);
	test_pass_msg();
}

void test_opaque_aliases_distinct(void) {
	test_start("window and sound opaque aliases are not interchangeable");
	AnalysisResult r = analyze_string("window :: opaque\n"
	                                  "sound :: opaque\n"
	                                  "extern proc window_close(own w: window);\n"
	                                  "extern func sound_open() -> sound;\n"
	                                  "proc main() {\n"
	                                  "  s := sound_open();\n"
	                                  "  window_close(move s);\n"
	                                  "}\n");
	ASSERT_TRUE(semantic_error_count(r.ctx) >= 1, "expected type-mismatch error");
	semantic_context_free(r.ctx);
	test_pass_msg();
}

/* ========== USE-AFTER-CONSUME TESTS ========== */

void test_use_after_consume_local_error(void) {
	test_start("use after consume in same scope is a compile error");
	AnalysisResult r = analyze_string("window :: opaque\n"
	                                  "extern func open_(t: char[], a: int, b: int) -> window;\n"
	                                  "extern proc close_(own w: window);\n"
	                                  "extern proc poll_(w: window);\n"
	                                  "proc main() {\n"
	                                  "  w := open_(\"\", 1, 1);\n"
	                                  "  close_(move w);\n"
	                                  "  poll_(w);\n"
	                                  "}\n");
	ASSERT_TRUE(semantic_error_count(r.ctx) >= 1, "expected use-after-consume error");
	semantic_context_free(r.ctx);
	test_pass_msg();
}

void test_no_false_positive_when_unconsumed(void) {
	test_start("normal borrow then consume is fine");
	AnalysisResult r = analyze_string("window :: opaque\n"
	                                  "extern func open_(t: char[], a: int, b: int) -> window;\n"
	                                  "extern proc close_(own w: window);\n"
	                                  "extern proc poll_(w: window);\n"
	                                  "proc main() {\n"
	                                  "  w := open_(\"\", 1, 1);\n"
	                                  "  poll_(w);\n"
	                                  "  close_(move w);\n"
	                                  "}\n");
	ASSERT_EQ(semantic_error_count(r.ctx), 0, "should be no errors");
	semantic_context_free(r.ctx);
	test_pass_msg();
}

/* ========== Read-only borrow / purity ========== */

void test_mutate_borrow_param_error(void) {
	test_start("mutating a borrowed (non-move) array param is a compile error");
	AnalysisResult r = analyze_string("func clobber(b: char[8]) -> int {\n"
	                                  "  b[0] = 'X';\n"
	                                  "  return 0;\n"
	                                  "}\n");
	ASSERT_TRUE(semantic_error_count(r.ctx) >= 1, "expected read-only-parameter error");
	semantic_context_free(r.ctx);
	test_pass_msg();
}

void test_move_param_can_mutate(void) {
	test_start("a `move` array param is owned and may be mutated");
	AnalysisResult r = analyze_string("func fill(own b: char[8]) -> int {\n"
	                                  "  b[0] = 'X';\n"
	                                  "  return 0;\n"
	                                  "}\n");
	ASSERT_EQ(semantic_error_count(r.ctx), 0, "owned move param mutation should be allowed");
	semantic_context_free(r.ctx);
	test_pass_msg();
}

void test_scalar_param_mutation_ok(void) {
	test_start("a scalar param is by value and may be reassigned locally");
	AnalysisResult r = analyze_string("func f(n: int) -> int {\n"
	                                  "  n = 5;\n"
	                                  "  return n;\n"
	                                  "}\n");
	ASSERT_EQ(semantic_error_count(r.ctx), 0, "scalar param reassignment should be allowed");
	semantic_context_free(r.ctx);
	test_pass_msg();
}

void test_move_out_of_borrow_error(void) {
	test_start("moving a borrowed array param out is a compile error");
	AnalysisResult r = analyze_string("proc sink(own b: char[8]) { b[0] = 'X'; }\n"
	                                  "func relay(b: char[8]) -> int {\n"
	                                  "  sink(move b);\n"
	                                  "  return 0;\n"
	                                  "}\n");
	ASSERT_TRUE(semantic_error_count(r.ctx) >= 1, "expected move-out-of-borrow error");
	semantic_context_free(r.ctx);
	test_pass_msg();
}

void test_copy_does_not_consume(void) {
	test_start("`copy` does not consume — the source stays usable");
	AnalysisResult r = analyze_string("func fill(own b: char[8]) -> char[8] {\n"
	                                  "  b[0] = 'X';\n"
	                                  "  return b;\n"
	                                  "}\n"
	                                  "proc main() {\n"
	                                  "  src: char[8];\n"
	                                  "  out := fill(copy src);\n"
	                                  "  src[0] = 'Y';\n"
	                                  "}\n");
	ASSERT_EQ(semantic_error_count(r.ctx), 0, "copy must not consume the source");
	semantic_context_free(r.ctx);
	test_pass_msg();
}

void test_own_param_bare_arg_error(void) {
	test_start("bare arg to an `own` param must be moved or copied");
	AnalysisResult r = analyze_string("func fill(own b: char[8]) -> char[8] {\n"
	                                  "  b[0] = 'X';\n"
	                                  "  return b;\n"
	                                  "}\n"
	                                  "proc main() {\n"
	                                  "  buf: char[8];\n"
	                                  "  out := fill(buf);\n"
	                                  "}\n");
	ASSERT_TRUE(semantic_error_count(r.ctx) >= 1, "expected must-be-moved-or-copied error");
	semantic_context_free(r.ctx);
	test_pass_msg();
}

void test_copy_opaque_error(void) {
	test_start("`copy` of an opaque value is a compile error (move-only)");
	AnalysisResult r = analyze_string("window :: opaque\n"
	                                  "extern func wopen() -> window;\n"
	                                  "extern proc wuse(own w: window);\n"
	                                  "proc main() {\n"
	                                  "  w := wopen();\n"
	                                  "  wuse(copy w);\n"
	                                  "}\n");
	ASSERT_TRUE(semantic_error_count(r.ctx) >= 1, "expected copy-of-opaque error");
	semantic_context_free(r.ctx);
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

	/* Func groups */
	printf("\nFunc group tests:\n");
	test_group_with_distinct_members_ok();
	test_group_member_unknown_errors();
	test_group_duplicate_signature_errors();
	test_group_name_collision_errors();
	test_call_no_matching_member_errors();

	/* Foreign opaque types */
	printf("\nForeign opaque type tests:\n");
	test_opaque_passthrough_in_proc_ok();
	test_unknown_type_name_still_errors();
	test_opaque_aliases_distinct();

	/* Use-after-consume tracking */
	printf("\nUse-after-consume tests:\n");
	test_use_after_consume_local_error();
	test_no_false_positive_when_unconsumed();

	/* Read-only borrow default / purity */
	printf("\nRead-only borrow / purity tests:\n");
	test_mutate_borrow_param_error();
	test_move_param_can_mutate();
	test_scalar_param_mutation_ok();
	test_move_out_of_borrow_error();
	test_copy_does_not_consume();
	test_own_param_bare_arg_error();
	test_copy_opaque_error();

	/* Results */
	printf("\n");
	printf("Results: %d/%d passed\n", test_pass, test_count);
	if (test_fail > 0) {
		printf("  %d failed\n", test_fail);
		return 1;
	}
	return 0;
}
