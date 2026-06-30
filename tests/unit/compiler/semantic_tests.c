#include "../../../lexer/lexer.h"
#include "../../../parser/parser.h"
#include "../../../semantic/sem_types.h"
#include "../../../semantic/semantic.h"
#include "../../../syntax/type_ref.h"
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
 * The parser produces only the lossless syntax tree; analysis runs via semantic_analyze_cst, which
 * collects its resolved DeclSummary table directly from that syntax tree (owned by the context) and
 * copies every string out of the source, so the syntax tree + source can be freed once analysis
 * returns. These tests therefore validate the syntax-tree-driven semantic path end to end. */
typedef struct {
	SemanticContext *ctx;
} AnalysisResult;

AnalysisResult analyze_string(const char *src) {
	AnalysisResult result = {NULL};

	ParseResult parse_result = parse_source(src);
	if (parse_result.error_count > 0 || !parse_result.syntax_root) {
		parse_result_free(&parse_result);
		return result;
	}

	result.ctx = semantic_analyze_cst(parse_result.syntax_root, src);
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
	AnalysisResult result = analyze_string("Player :: arche { x :: Float }");
	ASSERT_TRUE(result.ctx != NULL, "context is null");
	ASSERT_TRUE(semantic_archetype_exists(result.ctx, "Player"), "Player archetype not found");
	ASSERT_FALSE(semantic_archetype_exists(result.ctx, "Enemy"), "Enemy shouldn't exist");
	analysis_result_free(&result);
	test_pass_msg();
}

void test_archetype_field_exists(void) {
	test_start("archetype fields are registered");
	AnalysisResult result = analyze_string("Player :: arche { pos :: Float, health :: Float }");
	ASSERT_TRUE(result.ctx != NULL, "context is null");
	ASSERT_TRUE(semantic_field_exists(result.ctx, "Player", "pos"), "pos field not found");
	ASSERT_TRUE(semantic_field_exists(result.ctx, "Player", "health"), "health field not found");
	ASSERT_FALSE(semantic_field_exists(result.ctx, "Player", "velocity"), "velocity shouldn't exist");
	analysis_result_free(&result);
	test_pass_msg();
}

void test_archetype_field_kind(void) {
	test_start("archetype field kind is tracked");
	AnalysisResult result = analyze_string("Player :: arche { gravity :: Float, pos :: Float }");
	ASSERT_TRUE(result.ctx != NULL, "context is null");
	ASSERT_EQ(semantic_field_kind(result.ctx, "Player", "gravity"), FIELD_COLUMN, "gravity should be column");
	ASSERT_EQ(semantic_field_kind(result.ctx, "Player", "pos"), FIELD_COLUMN, "pos should be column");
	analysis_result_free(&result);
	test_pass_msg();
}

void test_archetype_field_type(void) {
	test_start("archetype field types are tracked");
	AnalysisResult result = analyze_string("Player :: arche { pos :: Vec3, health :: Float }");
	ASSERT_TRUE(result.ctx != NULL, "context is null");
	const char *pos_type = semantic_field_type_name(result.ctx, "Player", "pos");
	const char *health_type = semantic_field_type_name(result.ctx, "Player", "health");
	ASSERT_TRUE(pos_type != NULL, "pos type is null");
	ASSERT_TRUE(health_type != NULL, "health type is null");
	ASSERT_TRUE(strcmp(pos_type, "Vec3") == 0, "pos should be Vec3");
	/* Phase 3: the field-type API reports the canonical interned name ("Float" → "float"). */
	ASSERT_TRUE(strcmp(health_type, "float") == 0, "health should be float");
	analysis_result_free(&result);
	test_pass_msg();
}

/* ========== FIELD ACCESS VALIDATION TESTS ========== */

void test_field_access_on_undefined_archetype(void) {
	test_start("field access on undefined archetype caught");
	AnalysisResult result = analyze_string("test :: system { v := undefined.field; }");
	ASSERT_TRUE(semantic_has_errors(result.ctx), "should have error for undefined archetype");
	analysis_result_free(&result);
	test_pass_msg();
}

/* ========== VARIABLE SCOPE TESTS ========== */

void test_let_binding_creates_local(void) {
	test_start("binding creates local variable");
	AnalysisResult result = analyze_string("test :: system { x := 42; }");
	ASSERT_FALSE(semantic_has_errors(result.ctx), "should have no errors");
	analysis_result_free(&result);
	test_pass_msg();
}

void test_undefined_variable_access(void) {
	test_start("undefined variable access caught");
	AnalysisResult result = analyze_string("test :: system { x := undefined_var; }");
	ASSERT_TRUE(semantic_has_errors(result.ctx), "should have error for undefined variable");
	analysis_result_free(&result);
	test_pass_msg();
}

void test_variable_shadowing(void) {
	test_start("variable shadowing allowed");
	AnalysisResult result = analyze_string("test :: system {\n"
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
	AnalysisResult result = analyze_string("test :: system { x := 42; x = 100; }");
	ASSERT_FALSE(semantic_has_errors(result.ctx), "should have no errors");
	analysis_result_free(&result);
	test_pass_msg();
}

void test_binary_op_same_types(void) {
	test_start("binary op with same types");
	AnalysisResult result = analyze_string("test :: system { x := 1 + 2; }");
	ASSERT_FALSE(semantic_has_errors(result.ctx), "should have no errors");
	analysis_result_free(&result);
	test_pass_msg();
}

void test_comparison_produces_number(void) {
	test_start("comparison expressions are valid");
	AnalysisResult result = analyze_string("test :: system { x := 1 < 2; }");
	ASSERT_FALSE(semantic_has_errors(result.ctx), "should have no errors");
	analysis_result_free(&result);
	test_pass_msg();
}

/* ========== FUNCTION TESTS ========== */

void test_function_parameter_scope(void) {
	test_start("function parameters are in scope");
	AnalysisResult result = analyze_string("double :: func(x: Float) -> Float { return x * 2; }");
	ASSERT_FALSE(semantic_has_errors(result.ctx), "should have no errors");
	analysis_result_free(&result);
	test_pass_msg();
}

void test_function_undefined_parameter(void) {
	test_start("undefined parameter in function caught");
	AnalysisResult result = analyze_string("test :: func() -> Float { return undefined_param + 1; }");
	ASSERT_TRUE(semantic_has_errors(result.ctx), "should have error for undefined parameter");
	analysis_result_free(&result);
	test_pass_msg();
}

/* ========== MULTIPLE DECLARATIONS TESTS ========== */

void test_multiple_archetypes(void) {
	test_start("multiple archetypes are all registered");
	AnalysisResult result = analyze_string("Player :: arche { x :: Float }\n"
	                                       "Enemy :: arche { y :: Float }");
	ASSERT_TRUE(semantic_archetype_exists(result.ctx, "Player"), "Player not found");
	ASSERT_TRUE(semantic_archetype_exists(result.ctx, "Enemy"), "Enemy not found");
	analysis_result_free(&result);
	test_pass_msg();
}

/* ========== FUNC GROUP TESTS ========== */

void test_group_with_distinct_members_ok(void) {
	test_start("semantic: group with two distinct-signature members ok");
	const char *src = "a :: func(x: int) -> int { return x; }\n"
	                  "b :: func(x: float) -> float { return x; }\n"
	                  "g :: func{ a, b }\n"
	                  "entry :: system { i := g(1); f := g(2.0); }\n";
	ParseResult pr = parse_source(src);
	if (pr.error_count != 0) {
		test_fail_msg("Parse errors");
		return;
	}
	SemanticContext *sem = semantic_analyze_cst(pr.syntax_root, src);
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
	const char *src = "a :: func(x: int) -> int { return x; }\n"
	                  "g :: func{ a, missing }\n";
	ParseResult pr = parse_source(src);
	if (pr.error_count != 0) {
		test_fail_msg("Parse errors");
		return;
	}
	SemanticContext *sem = semantic_analyze_cst(pr.syntax_root, src);
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
	const char *src = "a :: func(x: int) -> int { return x; }\n"
	                  "b :: func(x: int) -> int { return x + 1; }\n"
	                  "g :: func{ a, b }\n";
	ParseResult pr = parse_source(src);
	if (pr.error_count != 0) {
		test_fail_msg("Parse errors");
		return;
	}
	SemanticContext *sem = semantic_analyze_cst(pr.syntax_root, src);
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
	const char *src = "a :: func(x: int) -> int { return x; }\n"
	                  "b :: func(x: float) -> float { return x; }\n"
	                  "a :: func{ a, b }\n";
	ParseResult pr = parse_source(src);
	if (pr.error_count != 0) {
		test_fail_msg("Parse errors");
		return;
	}
	SemanticContext *sem = semantic_analyze_cst(pr.syntax_root, src);
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
	const char *src = "a :: func(x: int) -> int { return x; }\n"
	                  "b :: func(x: float) -> float { return x; }\n"
	                  "g :: func{ a, b }\n"
	                  "entry :: system { c := 'X'; r := g(c); }\n";
	ParseResult pr = parse_source(src);
	if (pr.error_count != 0) {
		test_fail_msg("Parse errors");
		return;
	}
	SemanticContext *sem = semantic_analyze_cst(pr.syntax_root, src);
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
	AnalysisResult r = analyze_string("window :: opaque;\n"
	                                  "#foreign { window_close :: proc(own w: window) }\n"
	                                  "@allow(proc_not_primitive)\n"
	                                  "wrap_close :: proc(w: window) { window_close(move w); }\n");
	ASSERT_EQ(semantic_error_count(r.ctx), 0, "should be no errors");
	semantic_context_free(r.ctx);
	test_pass_msg();
}

void test_unknown_type_name_still_errors(void) {
	test_start("unknown type name in extern signature is still an error");
	AnalysisResult r = analyze_string("#foreign { bad :: proc()(ret: Doesnotexist); }\n");
	ASSERT_TRUE(semantic_error_count(r.ctx) >= 1, "expected unknown-type error");
	semantic_context_free(r.ctx);
	test_pass_msg();
}

void test_opaque_aliases_distinct(void) {
	test_start("window and sound opaque aliases are not interchangeable");
	AnalysisResult r = analyze_string("window :: opaque;\n"
	                                  "sound :: opaque;\n"
	                                  "#foreign {\n"
	                                  "  window_close :: proc(own w: window);\n"
	                                  "  sound_open :: proc()(ret: sound);\n"
	                                  "}\n"
	                                  "entry :: system {\n"
	                                  "  sound_open()(s:);\n"
	                                  "  window_close(move s);\n"
	                                  "}\n");
	ASSERT_TRUE(semantic_error_count(r.ctx) >= 1, "expected type-mismatch error");
	semantic_context_free(r.ctx);
	test_pass_msg();
}

/* ========== USE-AFTER-CONSUME TESTS ========== */

void test_use_after_consume_local_error(void) {
	test_start("use after consume in same scope is a compile error");
	AnalysisResult r = analyze_string("window :: opaque;\n"
	                                  "#foreign {\n"
	                                  "  open_ :: proc(own t: []char, a: int, b: int)(ret: window);\n"
	                                  "  close_ :: proc(own w: window);\n"
	                                  "  poll_ :: proc(w: window);\n"
	                                  "}\n"
	                                  "entry :: system {\n"
	                                  "  open_(\"\", 1, 1)(w:);\n"
	                                  "  close_(move w);\n"
	                                  "  poll_(w);\n"
	                                  "}\n");
	ASSERT_TRUE(semantic_error_count(r.ctx) >= 1, "expected use-after-consume error");
	semantic_context_free(r.ctx);
	test_pass_msg();
}

void test_no_false_positive_when_unconsumed(void) {
	test_start("normal borrow then consume is fine");
	AnalysisResult r = analyze_string("window :: opaque;\n"
	                                  "#foreign {\n"
	                                  "  open_ :: proc(own t: []char, a: int, b: int)(ret: window);\n"
	                                  "  close_ :: proc(own w: window);\n"
	                                  "  poll_ :: proc(w: window);\n"
	                                  "}\n"
	                                  "entry :: system eff {\n"
	                                  "  open_(\"\", 1, 1)(w:);\n"
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
	AnalysisResult r = analyze_string("clobber :: func(b: [8]char) -> int {\n"
	                                  "  b[0] = 'X';\n"
	                                  "  return 0;\n"
	                                  "}\n");
	ASSERT_TRUE(semantic_error_count(r.ctx) >= 1, "expected read-only-parameter error");
	semantic_context_free(r.ctx);
	test_pass_msg();
}

void test_move_param_can_mutate(void) {
	test_start("a `move` array param is owned and may be mutated");
	AnalysisResult r = analyze_string("fill :: func(own b: [8]char) -> int {\n"
	                                  "  b[0] = 'X';\n"
	                                  "  return 0;\n"
	                                  "}\n");
	ASSERT_EQ(semantic_error_count(r.ctx), 0, "owned move param mutation should be allowed");
	semantic_context_free(r.ctx);
	test_pass_msg();
}

void test_scalar_param_mutation_ok(void) {
	test_start("a scalar param is by value and may be reassigned locally");
	AnalysisResult r = analyze_string("f :: func(n: int) -> int {\n"
	                                  "  n = 5;\n"
	                                  "  return n;\n"
	                                  "}\n");
	ASSERT_EQ(semantic_error_count(r.ctx), 0, "scalar param reassignment should be allowed");
	semantic_context_free(r.ctx);
	test_pass_msg();
}

void test_move_out_of_borrow_error(void) {
	test_start("moving a borrowed array param out is a compile error");
	AnalysisResult r = analyze_string("@allow(proc_not_primitive)\n"
	                                  "sink :: proc(own b: [8]char) { b[0] = 'X'; }\n"
	                                  "relay :: func(b: [8]char) -> int {\n"
	                                  "  sink(move b);\n"
	                                  "  return 0;\n"
	                                  "}\n");
	ASSERT_TRUE(semantic_error_count(r.ctx) >= 1, "expected move-out-of-borrow error");
	semantic_context_free(r.ctx);
	test_pass_msg();
}

void test_copy_does_not_consume(void) {
	test_start("`copy` does not consume — the source stays usable");
	AnalysisResult r = analyze_string("fill :: func(own b: [8]char) -> [8]char {\n"
	                                  "  b[0] = 'X';\n"
	                                  "  return b;\n"
	                                  "}\n"
	                                  "entry :: system {\n"
	                                  "  src: [8]char;\n"
	                                  "  out := fill(copy src);\n"
	                                  "  src[0] = 'Y';\n"
	                                  "}\n");
	ASSERT_EQ(semantic_error_count(r.ctx), 0, "copy must not consume the source");
	semantic_context_free(r.ctx);
	test_pass_msg();
}

void test_own_param_bare_arg_error(void) {
	test_start("bare arg to an `own` param is an implicit move (consumes the source)");
	/* A bare move-only name handed to an `own` param implicitly moves — no error on the call
	 * itself. The transfer consumes the source, so a LATER use of it is the error. */
	AnalysisResult ok = analyze_string("fill :: func(own b: [8]char) -> [8]char {\n"
	                                   "  b[0] = 'X';\n"
	                                   "  return b;\n"
	                                   "}\n"
	                                   "entry :: system {\n"
	                                   "  buf: [8]char;\n"
	                                   "  out := fill(buf);\n"
	                                   "}\n");
	ASSERT_TRUE(semantic_error_count(ok.ctx) == 0, "a bare arg implicitly moves — no error expected");
	semantic_context_free(ok.ctx);
	AnalysisResult reuse = analyze_string("fill :: func(own b: [8]char) -> [8]char {\n"
	                                      "  return b;\n"
	                                      "}\n"
	                                      "entry :: system {\n"
	                                      "  buf: [8]char;\n"
	                                      "  a := fill(buf);\n"
	                                      "  c := fill(buf);\n"
	                                      "}\n");
	ASSERT_TRUE(semantic_error_count(reuse.ctx) >= 1, "expected use-after-consume on the reused source");
	semantic_context_free(reuse.ctx);
	test_pass_msg();
}

void test_copy_opaque_error(void) {
	test_start("`copy` of an opaque value is a compile error (move-only)");
	AnalysisResult r = analyze_string("window :: opaque;\n"
	                                  "#foreign {\n"
	                                  "  wopen :: proc()(ret: window);\n"
	                                  "  wuse :: proc(own w: window);\n"
	                                  "}\n"
	                                  "entry :: system {\n"
	                                  "  wopen()(w:);\n"
	                                  "  wuse(copy w);\n"
	                                  "}\n");
	ASSERT_TRUE(semantic_error_count(r.ctx) >= 1, "expected copy-of-opaque error");
	semantic_context_free(r.ctx);
	test_pass_msg();
}

/* ========== TypeId arena: alias-tier encoding (Stage 0) ========== */

/* A `#run <Schedule>` program (scheduling as a value, no main) analyzes clean. */
void test_run_schedule_ok(void) {
	test_start("run: #run Schedule value analyzes clean");
	AnalysisResult result = analyze_string("v :: int;\n"
	                                       "C :: arche { v }\n"
	                                       "[1]C;\n"
	                                       "step :: map (query { v }) (v) { v = v + 1; }\n"
	                                       "frame :: system { C.v = { 0 }; }\n"
	                                       "#run seq({ frame, step })\n");
	ASSERT_TRUE(result.ctx != NULL, "context is null");
	ASSERT_FALSE(semantic_has_errors(result.ctx), "valid #run should analyze clean");
	analysis_result_free(&result);
	test_pass_msg();
}

void test_tyid_distinct_subtype(void) {
	test_start("tyid: distinct subtype != backing, usable-as one-way");
	TypeArena *a = ty_arena_new();
	TypeId f = tyid_of_prim(a, PRIM_FLOAT);
	TypeId meters = tyid_of_nominal_sub(a, "meters", f);
	ASSERT_FALSE(tyid_equal(meters, f), "meters must be a distinct id from float");
	ASSERT_TRUE(tyid_usable_as(a, meters, f), "meters usable as float (one-way)");
	ASSERT_FALSE(tyid_usable_as(a, f, meters), "float NOT usable as meters");
	ASSERT_TRUE(tyid_backing(a, meters) == f, "meters backing is float");
	ASSERT_TRUE(tyid_backing(a, f) == TYID_UNKNOWN, "prim has no backing");
	ty_arena_free(a);
	test_pass_msg();
}

void test_tyid_subtype_chain_and_intern(void) {
	test_start("tyid: backing chain + hash-cons");
	TypeArena *a = ty_arena_new();
	TypeId f = tyid_of_prim(a, PRIM_FLOAT);
	TypeId meters = tyid_of_nominal_sub(a, "meters", f);
	TypeId mm = tyid_of_nominal_sub(a, "mm", meters);
	ASSERT_TRUE(tyid_usable_as(a, mm, f), "mm usable as float through the backing chain");
	ASSERT_FALSE(tyid_equal(mm, meters), "mm distinct from meters");
	/* hash-consing: same (name, backing) interns once */
	ASSERT_TRUE(tyid_equal(meters, tyid_of_nominal_sub(a, "meters", f)), "identical subtype interns once");
	/* a standalone nominal (opaque tag) has no backing and is usable only as itself */
	TypeId op = tyid_of_nominal(a, "socket");
	ASSERT_TRUE(tyid_backing(a, op) == TYID_UNKNOWN, "standalone nominal has no backing");
	ASSERT_FALSE(tyid_usable_as(a, op, f), "opaque not usable as float");
	ty_arena_free(a);
	test_pass_msg();
}

/* ========== MAIN TEST RUNNER ========== */

int main(void) {
	printf("Running semantic analysis tests...\n\n");

	/* TypeId arena: alias-tier encoding (Stage 0) */
	printf("TypeId arena tests:\n");
	test_tyid_distinct_subtype();
	test_tyid_subtype_chain_and_intern();

	/* Archetype validation */
	printf("Archetype validation tests:\n");
	test_archetype_defined();
	test_archetype_field_exists();
	test_archetype_field_kind();
	test_archetype_field_type();

	/* Field access */
	printf("\nField access validation tests:\n");
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

	/* Functions */
	printf("\nFunction tests:\n");
	test_function_parameter_scope();
	test_function_undefined_parameter();

	/* Multiple declarations */
	printf("\nMultiple declaration tests:\n");
	test_multiple_archetypes();

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

	/* Scheduling (system / #schedule / tick) */
	printf("\nScheduling tests:\n");
	test_run_schedule_ok();

	/* Results */
	printf("\n");
	printf("Results: %d/%d passed\n", test_pass, test_count);
	if (test_fail > 0) {
		printf("  %d failed\n", test_fail);
		return 1;
	}
	return 0;
}
