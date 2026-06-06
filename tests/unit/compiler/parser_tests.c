#include "../../../lexer/lexer.h"
#include "../../../parser/parser.h"
#include "../../../syntax/syntax_tree.h"
#include "../../../syntax/syntax_view.h"
#include <stdio.h>
#include <string.h>

/* These tests assert directly on the lossless syntax tree the parser produces
 * (SyntaxNode / SyntaxView), not on any reconstructed AST. The navigation here
 * mirrors how semantic.c's view-driven reader (decl_summary_from_node) walks
 * each construct: a top-level declaration is an
 * SN_CONST_DECL whose binding name is its first TOK_IDENT and whose value-form
 * RHS is a node (SN_ARCH_EXPR / SN_PROC_EXPR / SN_FUNC_EXPR / SN_SYS_EXPR /
 * SN_GROUP_EXPR). Fields, params, statements and arguments are counted as the
 * corresponding SN_* child nodes. */

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

#define ASSERT_NOT_NULL(ptr, msg)                                                                                      \
	if (!(ptr)) {                                                                                                      \
		test_fail_msg(msg);                                                                                            \
		parse_result_free(&pr);                                                                                        \
		return;                                                                                                        \
	}

#define ASSERT_EQ(a, b, msg)                                                                                           \
	if ((a) != (b)) {                                                                                                  \
		test_fail_msg(msg);                                                                                            \
		parse_result_free(&pr);                                                                                        \
		return;                                                                                                        \
	}

#define ASSERT_TRUE(cond, msg)                                                                                         \
	if (!(cond)) {                                                                                                     \
		test_fail_msg(msg);                                                                                            \
		parse_result_free(&pr);                                                                                        \
		return;                                                                                                        \
	}

/* ---- syntax-tree navigation helpers ---- */

/* parse_source() lexes the caller's `src` directly, so every token offset in the resulting tree
 * indexes into THAT string — the syntax view must carry it. ParseResult doesn't retain it, so a
 * thin wrapper records the source of the most recent parse for root_view(). Tests run serially. */
static const char *g_parse_src;
static ParseResult parse_src(const char *src) {
	g_parse_src = src;
	return parse_source(src);
}

/* The root view of a parse. The caller owns `pr` and must free it. */
static SyntaxView root_view(ParseResult *pr) {
	return sv_root(pr->syntax_root, g_parse_src);
}

/* Does `v`'s identifier text equal `s`? Uses the node's own source span. */

/* nth top-level declaration node (any SN_*_DECL / SN_REGION) under the source file. */
static SyntaxView decl_at(SyntaxView root, int n) {
	int seen = 0;
	for (int i = 0; i < root.node->child_count; i++) {
		if (root.node->children[i].tag != SE_NODE)
			continue;
		if (seen == n)
			return (SyntaxView){root.node->children[i].as.node, root.src};
		seen++;
	}
	return (SyntaxView){NULL, root.src};
}

/* number of top-level declaration nodes. */
static int decl_count(SyntaxView root) {
	int n = 0;
	for (int i = 0; i < root.node->child_count; i++)
		if (root.node->children[i].tag == SE_NODE)
			n++;
	return n;
}

/* The value-form RHS node of a binding decl (SN_*_EXPR), or absent. */
static SyntaxView rhs_form(SyntaxView decl) {
	if (!decl.node)
		return (SyntaxView){NULL, decl.src};
	for (int i = 0; i < decl.node->child_count; i++) {
		if (decl.node->children[i].tag != SE_NODE)
			continue;
		SyntaxNodeKind k = decl.node->children[i].as.node->kind;
		if (k == SN_ARCH_EXPR || k == SN_PROC_EXPR || k == SN_FUNC_EXPR || k == SN_SYS_EXPR || k == SN_GROUP_EXPR ||
		    k == SN_ENUM_EXPR)
			return (SyntaxView){decl.node->children[i].as.node, decl.src};
	}
	return (SyntaxView){NULL, decl.src};
}

/* The binding name of a decl: its first direct-child IDENT token. */
static int decl_name_eq(SyntaxView decl, const char *s) {
	SynText t = sv_token(decl, TOK_IDENT);
	size_t n = strlen(s);
	return t.ptr && t.len == n && memcmp(t.ptr, s, n) == 0;
}

/* The name of a param node (its SN_PARAM_NAME child's IDENT). */
static int param_name_eq(SyntaxView param, const char *s) {
	SynText t = sv_token(sv_child(param, SN_PARAM_NAME), TOK_IDENT);
	size_t n = strlen(s);
	return t.ptr && t.len == n && memcmp(t.ptr, s, n) == 0;
}

/* The name of an SN_FIELD_NAME node. */
static int field_name_eq(SyntaxView field, const char *s) {
	SynText t = sv_token(field, TOK_IDENT);
	size_t n = strlen(s);
	return t.ptr && t.len == n && memcmp(t.ptr, s, n) == 0;
}

static int is_stmt_kind(SyntaxNodeKind k) {
	return k >= SN_BIND_STMT && k <= SN_MATCH_STMT;
}

/* Count direct-child statement nodes of a body-bearing form. */
static int stmt_count(SyntaxView form) {
	int n = 0;
	if (!form.node)
		return 0;
	for (int i = 0; i < form.node->child_count; i++)
		if (form.node->children[i].tag == SE_NODE && is_stmt_kind(form.node->children[i].as.node->kind))
			n++;
	return n;
}

/* The nth direct-child statement node of a body-bearing form. */
static SyntaxView stmt_at(SyntaxView form, int idx) {
	int seen = 0;
	for (int i = 0; i < form.node->child_count; i++) {
		if (form.node->children[i].tag != SE_NODE || !is_stmt_kind(form.node->children[i].as.node->kind))
			continue;
		if (seen == idx)
			return (SyntaxView){form.node->children[i].as.node, form.src};
		seen++;
	}
	return (SyntaxView){NULL, form.src};
}

/* The value of a bind statement. A bind's expr-position children are [target-name, value?]:
 * the leftmost is the bound name (an SN_NAME_EXPR), so the value — when present — is child 1
 * (mirrors semantic.c's `sem_node_at_expr(v, 1)` for the bind value). Absent node if value-less. */
static SyntaxView bind_value(SyntaxView stmt) {
	return sv_expr_at(stmt, 1);
}

/* Number of argument expressions in a call-expr (expr-position children; the
 * callee is an SN_CALLEE_NAME role leaf and is excluded). */
static int call_arg_count(SyntaxView call) {
	int n = 0;
	while (sv_expr_at(call, n).node)
		n++;
	return n;
}

/* ========== ARCHETYPE TESTS ========== */

void test_archetype_empty(void) {
	test_start("archetype empty");
	ParseResult pr = parse_src("Player :: arche {}");
	SyntaxView root = root_view(&pr);
	ASSERT_NOT_NULL(root.node, "no syntax root");
	ASSERT_EQ(decl_count(root), 1, "expected 1 decl");
	SyntaxView d = decl_at(root, 0);
	SyntaxView arch = rhs_form(d);
	ASSERT_NOT_NULL(arch.node, "archetype form is null");
	ASSERT_EQ(sv_kind(arch), SN_ARCH_EXPR, "expected SN_ARCH_EXPR");
	ASSERT_TRUE(decl_name_eq(d, "Player"), "wrong name");
	ASSERT_EQ(sv_count(arch, SN_FIELD_NAME), 0, "expected 0 fields");
	parse_result_free(&pr);
	test_pass_msg();
}

void test_archetype_meta_field(void) {
	test_start("archetype with meta field");
	ParseResult pr = parse_src("Player :: arche {\n  drag :: Float\n}");
	SyntaxView root = root_view(&pr);
	ASSERT_NOT_NULL(root.node, "no syntax root");
	ASSERT_EQ(decl_count(root), 1, "expected 1 decl");
	SyntaxView arch = rhs_form(decl_at(root, 0));
	ASSERT_EQ(sv_count(arch, SN_FIELD_NAME), 1, "expected 1 field");
	ASSERT_TRUE(field_name_eq(sv_child(arch, SN_FIELD_NAME), "drag"), "wrong field name");
	parse_result_free(&pr);
	test_pass_msg();
}

void test_archetype_col_field(void) {
	test_start("archetype with col field");
	ParseResult pr = parse_src("Particle :: arche {\n  pos :: Float\n}");
	SyntaxView root = root_view(&pr);
	ASSERT_NOT_NULL(root.node, "no syntax root");
	SyntaxView arch = rhs_form(decl_at(root, 0));
	ASSERT_EQ(sv_count(arch, SN_FIELD_NAME), 1, "expected 1 field");
	ASSERT_TRUE(field_name_eq(sv_child(arch, SN_FIELD_NAME), "pos"), "wrong field name");
	parse_result_free(&pr);
	test_pass_msg();
}

void test_archetype_multiple_fields(void) {
	test_start("archetype with multiple fields");
	ParseResult pr = parse_src("Body :: arche {\n"
	                           "  drag :: Float,\n"
	                           "  pos :: Vec3,\n"
	                           "  vel :: Vec3\n"
	                           "}");
	SyntaxView root = root_view(&pr);
	ASSERT_NOT_NULL(root.node, "no syntax root");
	SyntaxView arch = rhs_form(decl_at(root, 0));
	ASSERT_EQ(sv_count(arch, SN_FIELD_NAME), 3, "expected 3 fields");
	ASSERT_TRUE(field_name_eq(sv_child_at(arch, SN_FIELD_NAME, 0), "drag"), "field 0 name");
	ASSERT_TRUE(field_name_eq(sv_child_at(arch, SN_FIELD_NAME, 1), "pos"), "field 1 name");
	ASSERT_TRUE(field_name_eq(sv_child_at(arch, SN_FIELD_NAME, 2), "vel"), "field 2 name");
	parse_result_free(&pr);
	test_pass_msg();
}

/* ========== PROCEDURE TESTS ========== */

void test_proc_no_params_empty(void) {
	test_start("proc with no params, empty body");
	ParseResult pr = parse_src("init :: proc() {}");
	SyntaxView root = root_view(&pr);
	ASSERT_NOT_NULL(root.node, "no syntax root");
	ASSERT_EQ(decl_count(root), 1, "expected 1 decl");
	SyntaxView d = decl_at(root, 0);
	SyntaxView proc = rhs_form(d);
	ASSERT_EQ(sv_kind(proc), SN_PROC_EXPR, "expected SN_PROC_EXPR");
	ASSERT_TRUE(decl_name_eq(d, "init"), "wrong proc name");
	ASSERT_EQ(stmt_count(proc), 0, "expected 0 statements");
	parse_result_free(&pr);
	test_pass_msg();
}

void test_proc_with_let_statement(void) {
	test_start("proc with statement");
	ParseResult pr = parse_src("test :: proc() {\n"
	                           "  x := 42;\n"
	                           "}");
	SyntaxView root = root_view(&pr);
	ASSERT_NOT_NULL(root.node, "no syntax root");
	SyntaxView proc = rhs_form(decl_at(root, 0));
	ASSERT_EQ(stmt_count(proc), 1, "expected 1 statement");
	ASSERT_EQ(sv_kind(stmt_at(proc, 0)), SN_BIND_STMT, "expected SN_BIND_STMT");
	parse_result_free(&pr);
	test_pass_msg();
}

void test_proc_with_assignment(void) {
	test_start("proc with assignment statement");
	ParseResult pr = parse_src("test :: proc() {\n"
	                           "  x = 42;\n"
	                           "}");
	SyntaxView root = root_view(&pr);
	ASSERT_NOT_NULL(root.node, "no syntax root");
	SyntaxView proc = rhs_form(decl_at(root, 0));
	ASSERT_EQ(stmt_count(proc), 1, "expected 1 statement");
	ASSERT_EQ(sv_kind(stmt_at(proc, 0)), SN_ASSIGN_STMT, "expected SN_ASSIGN_STMT");
	parse_result_free(&pr);
	test_pass_msg();
}

/* ========== SYSTEM TESTS ========== */

void test_sys_no_params_empty(void) {
	test_start("sys with no params, empty body");
	ParseResult pr = parse_src("update :: sys() {}");
	SyntaxView root = root_view(&pr);
	ASSERT_NOT_NULL(root.node, "no syntax root");
	SyntaxView d = decl_at(root, 0);
	SyntaxView sys = rhs_form(d);
	ASSERT_EQ(sv_kind(sys), SN_SYS_EXPR, "expected SN_SYS_EXPR");
	ASSERT_TRUE(decl_name_eq(d, "update"), "wrong sys name");
	ASSERT_EQ(sv_count(sys, SN_PARAM), 0, "expected 0 params");
	ASSERT_EQ(stmt_count(sys), 0, "expected 0 statements");
	parse_result_free(&pr);
	test_pass_msg();
}

void test_sys_with_params(void) {
	test_start("sys with params");
	ParseResult pr = parse_src("integrate :: sys(pos, vel) {}");
	SyntaxView root = root_view(&pr);
	ASSERT_NOT_NULL(root.node, "no syntax root");
	SyntaxView sys = rhs_form(decl_at(root, 0));
	ASSERT_EQ(sv_count(sys, SN_PARAM), 2, "expected 2 params");
	ASSERT_TRUE(param_name_eq(sv_child_at(sys, SN_PARAM, 0), "pos"), "wrong param 0");
	ASSERT_TRUE(param_name_eq(sv_child_at(sys, SN_PARAM, 1), "vel"), "wrong param 1");
	parse_result_free(&pr);
	test_pass_msg();
}

void test_sys_with_body(void) {
	test_start("sys with body statement");
	ParseResult pr = parse_src("integrate :: sys(pos, vel) {\n"
	                           "  pos = pos + vel;\n"
	                           "}");
	SyntaxView root = root_view(&pr);
	ASSERT_NOT_NULL(root.node, "no syntax root");
	SyntaxView sys = rhs_form(decl_at(root, 0));
	ASSERT_EQ(stmt_count(sys), 1, "expected 1 statement");
	parse_result_free(&pr);
	test_pass_msg();
}

/* ========== FUNCTION TESTS ========== */

void test_func_simple(void) {
	test_start("func simple");
	ParseResult pr = parse_src("double :: func(x: Float) -> Float { x * 2; }");
	SyntaxView root = root_view(&pr);
	ASSERT_NOT_NULL(root.node, "no syntax root");
	SyntaxView d = decl_at(root, 0);
	SyntaxView func = rhs_form(d);
	ASSERT_EQ(sv_kind(func), SN_FUNC_EXPR, "expected SN_FUNC_EXPR");
	ASSERT_TRUE(decl_name_eq(d, "double"), "wrong func name");
	ASSERT_EQ(sv_count(func, SN_PARAM), 1, "expected 1 param");
	ASSERT_TRUE(param_name_eq(sv_child(func, SN_PARAM), "x"), "wrong param name");
	parse_result_free(&pr);
	test_pass_msg();
}

void test_func_multiple_params(void) {
	test_start("func with multiple params");
	ParseResult pr = parse_src("add :: func(x: Float, y: Float) -> Float { x + y; }");
	SyntaxView root = root_view(&pr);
	ASSERT_NOT_NULL(root.node, "no syntax root");
	SyntaxView func = rhs_form(decl_at(root, 0));
	ASSERT_EQ(sv_count(func, SN_PARAM), 2, "expected 2 params");
	parse_result_free(&pr);
	test_pass_msg();
}

/* ========== EXPRESSION TESTS ========== */

void test_expr_literal(void) {
	test_start("expression literal");
	ParseResult pr = parse_src("test :: proc() { x := 42; }");
	SyntaxView root = root_view(&pr);
	ASSERT_NOT_NULL(root.node, "no syntax root");
	SyntaxView proc = rhs_form(decl_at(root, 0));
	SyntaxView val = bind_value(stmt_at(proc, 0));
	ASSERT_EQ(sv_kind(val), SN_LITERAL_EXPR, "expected SN_LITERAL_EXPR");
	parse_result_free(&pr);
	test_pass_msg();
}

void test_expr_field_access(void) {
	test_start("expression field access");
	ParseResult pr = parse_src("test :: proc() { x := player.pos; }");
	SyntaxView root = root_view(&pr);
	ASSERT_NOT_NULL(root.node, "no syntax root");
	SyntaxView proc = rhs_form(decl_at(root, 0));
	SyntaxView val = bind_value(stmt_at(proc, 0));
	ASSERT_EQ(sv_kind(val), SN_FIELD_EXPR, "expected SN_FIELD_EXPR");
	parse_result_free(&pr);
	test_pass_msg();
}

void test_expr_index(void) {
	test_start("expression indexing");
	ParseResult pr = parse_src("test :: proc() { x := arr[0]; }");
	SyntaxView root = root_view(&pr);
	ASSERT_NOT_NULL(root.node, "no syntax root");
	SyntaxView proc = rhs_form(decl_at(root, 0));
	SyntaxView val = bind_value(stmt_at(proc, 0));
	ASSERT_EQ(sv_kind(val), SN_INDEX_EXPR, "expected SN_INDEX_EXPR");
	parse_result_free(&pr);
	test_pass_msg();
}

void test_expr_binary_op(void) {
	test_start("expression binary operation");
	ParseResult pr = parse_src("test :: proc() { x := a + b; }");
	SyntaxView root = root_view(&pr);
	ASSERT_NOT_NULL(root.node, "no syntax root");
	SyntaxView proc = rhs_form(decl_at(root, 0));
	SyntaxView val = bind_value(stmt_at(proc, 0));
	ASSERT_EQ(sv_kind(val), SN_BINARY_EXPR, "expected SN_BINARY_EXPR");
	parse_result_free(&pr);
	test_pass_msg();
}

/* ========== LET TYPE ANNOTATION TESTS ========== */

void test_let_type_annotation_with_value(void) {
	test_start("with type annotation and value");
	ParseResult pr = parse_src("test :: proc() { x: int = 5; }");
	SyntaxView root = root_view(&pr);
	ASSERT_NOT_NULL(root.node, "no syntax root");
	SyntaxView proc = rhs_form(decl_at(root, 0));
	ASSERT_EQ(stmt_count(proc), 1, "expected 1 statement");
	SyntaxView stmt = stmt_at(proc, 0);
	ASSERT_EQ(sv_kind(stmt), SN_BIND_STMT, "expected SN_BIND_STMT");
	ASSERT_TRUE(sv_type_count(stmt) >= 1, "type annotation should be present");
	ASSERT_EQ(sv_kind(sv_type_at(stmt, 0)), SN_TYPE_REF, "expected SN_TYPE_REF");
	ASSERT_TRUE(bind_value(stmt).node != NULL, "value should be present");
	parse_result_free(&pr);
	test_pass_msg();
}

void test_let_type_annotation_no_value(void) {
	test_start("with type annotation, no value");
	ParseResult pr = parse_src("test :: proc() { x: int; }");
	SyntaxView root = root_view(&pr);
	ASSERT_NOT_NULL(root.node, "no syntax root");
	SyntaxView proc = rhs_form(decl_at(root, 0));
	ASSERT_EQ(stmt_count(proc), 1, "expected 1 statement");
	SyntaxView stmt = stmt_at(proc, 0);
	ASSERT_EQ(sv_kind(stmt), SN_BIND_STMT, "expected SN_BIND_STMT");
	ASSERT_TRUE(sv_type_count(stmt) >= 1, "type annotation should be present");
	ASSERT_EQ(sv_kind(sv_type_at(stmt, 0)), SN_TYPE_REF, "expected SN_TYPE_REF");
	ASSERT_TRUE(bind_value(stmt).node == NULL, "value should be absent");
	parse_result_free(&pr);
	test_pass_msg();
}

void test_let_array_type_annotation(void) {
	test_start("with array type annotation");
	ParseResult pr = parse_src("test :: proc() { buf: char[]; }");
	SyntaxView root = root_view(&pr);
	ASSERT_NOT_NULL(root.node, "no syntax root");
	SyntaxView proc = rhs_form(decl_at(root, 0));
	ASSERT_EQ(stmt_count(proc), 1, "expected 1 statement");
	SyntaxView stmt = stmt_at(proc, 0);
	ASSERT_EQ(sv_kind(stmt), SN_BIND_STMT, "expected SN_BIND_STMT");
	ASSERT_TRUE(sv_type_count(stmt) >= 1, "type annotation should be present");
	ASSERT_EQ(sv_kind(sv_type_at(stmt, 0)), SN_TYPE_ARRAY, "expected SN_TYPE_ARRAY");
	ASSERT_TRUE(bind_value(stmt).node == NULL, "value should be absent");
	parse_result_free(&pr);
	test_pass_msg();
}

void test_let_float_type_annotation(void) {
	test_start("with float type annotation");
	ParseResult pr = parse_src("test :: proc() { f: float = 1.5; }");
	SyntaxView root = root_view(&pr);
	ASSERT_NOT_NULL(root.node, "no syntax root");
	SyntaxView proc = rhs_form(decl_at(root, 0));
	ASSERT_EQ(stmt_count(proc), 1, "expected 1 statement");
	SyntaxView stmt = stmt_at(proc, 0);
	ASSERT_EQ(sv_kind(stmt), SN_BIND_STMT, "expected SN_BIND_STMT");
	ASSERT_TRUE(sv_type_count(stmt) >= 1, "type annotation should be present");
	ASSERT_TRUE(bind_value(stmt).node != NULL, "value should be present");
	parse_result_free(&pr);
	test_pass_msg();
}

/* ========== PRINTF VARIADIC TESTS ========== */

void test_printf_with_float(void) {
	test_start("printf with float argument");
	ParseResult pr = parse_src("test :: proc() { printf(\"Value: %f\\n\", 3.14); }");
	SyntaxView root = root_view(&pr);
	ASSERT_NOT_NULL(root.node, "no syntax root");
	SyntaxView proc = rhs_form(decl_at(root, 0));
	ASSERT_EQ(stmt_count(proc), 1, "expected 1 statement");
	SyntaxView stmt = stmt_at(proc, 0);
	ASSERT_EQ(sv_kind(stmt), SN_EXPR_STMT, "expected SN_EXPR_STMT");
	SyntaxView call = sv_child(stmt, SN_CALL_EXPR);
	ASSERT_NOT_NULL(call.node, "expected SN_CALL_EXPR");
	ASSERT_EQ(call_arg_count(call), 2, "expected 2 arguments");
	parse_result_free(&pr);
	test_pass_msg();
}

void test_printf_with_variable_float(void) {
	test_start("printf with variable float argument");
	ParseResult pr = parse_src("test :: proc() { f: float = 1.5; printf(\"Float: %f\\n\", f); }");
	SyntaxView root = root_view(&pr);
	ASSERT_NOT_NULL(root.node, "no syntax root");
	SyntaxView proc = rhs_form(decl_at(root, 0));
	ASSERT_EQ(stmt_count(proc), 2, "expected 2 statements");
	SyntaxView stmt = stmt_at(proc, 1);
	ASSERT_EQ(sv_kind(stmt), SN_EXPR_STMT, "expected SN_EXPR_STMT");
	SyntaxView call = sv_child(stmt, SN_CALL_EXPR);
	ASSERT_NOT_NULL(call.node, "expected SN_CALL_EXPR");
	ASSERT_EQ(call_arg_count(call), 2, "expected 2 arguments");
	parse_result_free(&pr);
	test_pass_msg();
}

void test_sprintf_with_float(void) {
	test_start("sprintf with float argument");
	ParseResult pr = parse_src("test :: proc() { sprintf(\"buf\", \"Value: %f\\n\", 3.14); }");
	SyntaxView root = root_view(&pr);
	ASSERT_NOT_NULL(root.node, "no syntax root");
	SyntaxView proc = rhs_form(decl_at(root, 0));
	ASSERT_EQ(stmt_count(proc), 1, "expected 1 statement");
	SyntaxView stmt = stmt_at(proc, 0);
	ASSERT_EQ(sv_kind(stmt), SN_EXPR_STMT, "expected SN_EXPR_STMT");
	SyntaxView call = sv_child(stmt, SN_CALL_EXPR);
	ASSERT_NOT_NULL(call.node, "expected SN_CALL_EXPR");
	ASSERT_EQ(call_arg_count(call), 3, "expected 3 arguments");
	parse_result_free(&pr);
	test_pass_msg();
}

/* ========== PARSER REGRESSION TESTS ========== */

static int parse_succeeds(const char *src) {
	ParseResult result = parse_src(src);
	int ok = (result.error_count == 0);
	parse_result_free(&result);
	return ok;
}

void test_else_break(void) {
	test_start("else clause with break in nested for");
	int ok = parse_succeeds("main :: proc() {\n"
	                        "  idx := 0;\n"
	                        "  for (;idx < 10;) {\n"
	                        "    line_pos := 0;\n"
	                        "    for (;line_pos < 5;) {\n"
	                        "      if (1 == 1) { line_pos = 1; }\n"
	                        "      else { break; }\n"
	                        "    }\n"
	                        "  }\n"
	                        "}\n");
	if (!ok) {
		test_fail_msg("parse failed");
		return;
	}
	test_pass_msg();
}

void test_else_let(void) {
	test_start("else clause with in nested for");
	int ok = parse_succeeds("main :: proc() {\n"
	                        "  idx := 0;\n"
	                        "  for (;idx < 10;) {\n"
	                        "    line_pos := 0;\n"
	                        "    for (;line_pos < 5;) {\n"
	                        "      if (1 == 1) { line_pos = 1; }\n"
	                        "      else { x := 1; }\n"
	                        "    }\n"
	                        "  }\n"
	                        "}\n");
	if (!ok) {
		test_fail_msg("parse failed");
		return;
	}
	test_pass_msg();
}

void test_else_assign(void) {
	test_start("else clause with assignment in nested for");
	int ok = parse_succeeds("main :: proc() {\n"
	                        "  idx := 0;\n"
	                        "  for (;idx < 10;) {\n"
	                        "    line_pos := 0;\n"
	                        "    for (;line_pos < 5;) {\n"
	                        "      if (1 == 1) { line_pos = 1; }\n"
	                        "      else { line_pos = 2; }\n"
	                        "    }\n"
	                        "  }\n"
	                        "}\n");
	if (!ok) {
		test_fail_msg("parse failed");
		return;
	}
	test_pass_msg();
}

void test_nested_for_no_array_index(void) {
	test_start("nested for with else, no array indexing");
	int ok = parse_succeeds("main :: proc() {\n"
	                        "  idx := 0;\n"
	                        "  for (;idx < 10;) {\n"
	                        "    n := 5;\n"
	                        "    line_pos := 0;\n"
	                        "    field_idx := 0;\n"
	                        "    for (;line_pos < n;) {\n"
	                        "      if (field_idx == 1) { line_pos = line_pos + 1; }\n"
	                        "      else { line_pos = line_pos + 1; }\n"
	                        "    }\n"
	                        "    idx = idx + 1;\n"
	                        "  }\n"
	                        "}\n");
	if (!ok) {
		test_fail_msg("parse failed");
		return;
	}
	test_pass_msg();
}

void test_nested_for_else_assign(void) {
	test_start("nested for with assignment in else");
	int ok = parse_succeeds("main :: proc() {\n"
	                        "  idx := 0;\n"
	                        "  for (;idx < 10;) {\n"
	                        "    n := 5;\n"
	                        "    line_pos := 0;\n"
	                        "    for (;line_pos < n;) {\n"
	                        "      if (1 == 1) { line_pos = line_pos + 1; }\n"
	                        "      else { line_pos = line_pos + 1; }\n"
	                        "    }\n"
	                        "    idx = idx + 1;\n"
	                        "  }\n"
	                        "}\n");
	if (!ok) {
		test_fail_msg("parse failed");
		return;
	}
	test_pass_msg();
}

void test_nested_let_simple(void) {
	test_start("declaration inside for loop");
	int ok = parse_succeeds("main :: proc() {\n"
	                        "  for (;1 > 0;) {\n"
	                        "    x := 5;\n"
	                        "    break;\n"
	                        "  }\n"
	                        "}\n");
	if (!ok) {
		test_fail_msg("parse errors");
		return;
	}
	test_pass_msg();
}

void test_nested_let_in_for_loop(void) {
	test_start("+ nested for inside for loop");
	ParseResult pr = parse_src("main :: proc() {\n"
	                           "  for (;1 > 0;) {\n"
	                           "    x := 0;\n"
	                           "    for (;x < 5;) {\n"
	                           "      x = x + 1;\n"
	                           "    }\n"
	                           "    break;\n"
	                           "  }\n"
	                           "}\n");
	SyntaxView root = root_view(&pr);
	ASSERT_NOT_NULL(root.node, "parse errors");
	ASSERT_EQ(pr.error_count, 0, "parse errors");
	SyntaxView proc = rhs_form(decl_at(root, 0));
	SyntaxView outer_for = stmt_at(proc, 0);
	ASSERT_EQ(sv_kind(outer_for), SN_FOR_STMT, "outer should be SN_FOR_STMT");
	/* the outer for's body: first a bind, then a nested for */
	ASSERT_TRUE(stmt_count(outer_for) >= 2, "expected >=2 body statements");
	ASSERT_EQ(sv_kind(stmt_at(outer_for, 0)), SN_BIND_STMT, "body[0] should be SN_BIND_STMT");
	ASSERT_EQ(sv_kind(stmt_at(outer_for, 1)), SN_FOR_STMT, "body[1] should be SN_FOR_STMT");
	parse_result_free(&pr);
	test_pass_msg();
}

void test_single_for_else_break(void) {
	test_start("single for with if-else-break");
	int ok = parse_succeeds("main :: proc() {\n"
	                        "  idx := 0;\n"
	                        "  for (;idx < 10;) {\n"
	                        "    if (1 == 1) { idx = 1; }\n"
	                        "    else { break; }\n"
	                        "  }\n"
	                        "}\n");
	if (!ok) {
		test_fail_msg("parse failed");
		return;
	}
	test_pass_msg();
}

void test_two_for_else_inner(void) {
	test_start("two nested fors with if-else at inner level");
	int ok = parse_succeeds("main :: proc() {\n"
	                        "  idx := 0;\n"
	                        "  for (;idx < 10;) {\n"
	                        "    pos := 0;\n"
	                        "    for (;pos < 5;) {\n"
	                        "      if (1 == 1) { pos = 1; }\n"
	                        "      else { break; }\n"
	                        "    }\n"
	                        "  }\n"
	                        "}\n");
	if (!ok) {
		test_fail_msg("parse failed");
		return;
	}
	test_pass_msg();
}

void test_printf_after_complex_nesting(void) {
	test_start("printf after deeply nested for loops");
	int ok = parse_succeeds("main :: proc() {\n"
	                        "  sum := 0.0;\n"
	                        "  for (;0 < 1;) {\n"
	                        "    idx := 0;\n"
	                        "    for (;idx < 5;) {\n"
	                        "      x := 1.0;\n"
	                        "      sum = sum + x;\n"
	                        "      idx = idx + 1;\n"
	                        "    }\n"
	                        "    break;\n"
	                        "  }\n"
	                        "  printf(\"sum: %g\\n\", sum);\n"
	                        "}\n");
	if (!ok) {
		test_fail_msg("parse failed");
		return;
	}
	test_pass_msg();
}

void test_toplevel_else(void) {
	test_start("if-else at proc top level");
	int ok = parse_succeeds("main :: proc() {\n"
	                        "  if (1 == 1) { x := 1; }\n"
	                        "  else { break; }\n"
	                        "}\n");
	if (!ok) {
		test_fail_msg("parse failed");
		return;
	}
	test_pass_msg();
}

void test_task1_structure(void) {
	test_start("complex ETL program structure");
	int ok = parse_succeeds("Transaction :: arche {\n"
	                        "  price :: float,\n"
	                        "  quantity :: int,\n"
	                        "  revenue :: float,\n"
	                        "}\n"
	                        "Transaction[1000](1000) {\n"
	                        "  price: 0.0,\n"
	                        "  quantity: 0,\n"
	                        "  revenue: 0.0,\n"
	                        "};\n"
	                        "main :: proc() {\n"
	                        "  fd := 1;\n"
	                        "  line: char[128];\n"
	                        "  idx := 0;\n"
	                        "  for (;idx < 1000;) {\n"
	                        "    n := 10;\n"
	                        "    if (n <= 0) { break; }\n"
	                        "    line_pos := 0;\n"
	                        "    field_idx := 0;\n"
	                        "    for (;line_pos < n;) {\n"
	                        "      if (line[line_pos] == ',') {\n"
	                        "        field_idx = field_idx + 1;\n"
	                        "        line_pos = line_pos + 1;\n"
	                        "      } else {\n"
	                        "        line_pos = line_pos + 1;\n"
	                        "      }\n"
	                        "    }\n"
	                        "    idx = idx + 1;\n"
	                        "  }\n"
	                        "  sum := 0.0;\n"
	                        "  m := 0;\n"
	                        "  for (;m < 1000;) {\n"
	                        "    sum = sum + 1.0;\n"
	                        "    m = m + 1;\n"
	                        "  }\n"
	                        "  printf(\"result: %g\\n\", sum);\n"
	                        "}\n");
	if (!ok) {
		test_fail_msg("parse failed");
		return;
	}
	test_pass_msg();
}

/* ========== FUNC GROUP TESTS ========== */

void test_parse_func_group(void) {
	test_start("parser: parse func group declaration");
	ParseResult pr = parse_src("a :: func(x: int) -> int { return x; }\n"
	                           "b :: func(x: float) -> float { return x; }\n"
	                           "g :: func{ a, b }\n");
	SyntaxView root = root_view(&pr);
	ASSERT_NOT_NULL(root.node, "no syntax root");
	ASSERT_EQ(decl_count(root), 3, "Expected 3 declarations");
	SyntaxView d = decl_at(root, 2);
	SyntaxView grp = rhs_form(d);
	ASSERT_NOT_NULL(grp.node, "func group form is null");
	ASSERT_EQ(sv_kind(grp), SN_GROUP_EXPR, "Third decl must be a group form");
	ASSERT_TRUE(decl_name_eq(d, "g"), "group name mismatch");
	/* members are the IDENT tokens inside the group braces */
	int members = 0;
	for (int i = 0; i < grp.node->child_count; i++)
		if (grp.node->children[i].tag == SE_TOKEN && grp.node->children[i].as.token.kind == TOK_IDENT)
			members++;
	ASSERT_EQ(members, 2, "expected 2 members");
	parse_result_free(&pr);
	test_pass_msg();
}

void test_parse_func_group_empty_rejected(void) {
	test_start("parser: empty func group rejected");
	ParseResult pr = parse_src("g :: func{ }\n");
	if (pr.error_count == 0) {
		parse_result_free(&pr);
		test_fail_msg("Expected parse error for empty group");
		return;
	}
	parse_result_free(&pr);
	test_pass_msg();
}

void test_parse_func_group_trailing_comma(void) {
	test_start("parser: func group accepts a trailing comma");
	/* Trailing commas are accepted in every comma-list (the formatter emits them when a list
	 * breaks across lines, then drops them when it collapses inline). */
	ParseResult pr = parse_src("a :: func(x: int) -> int { return x; }\n"
	                           "g :: func{ a, }\n");
	if (pr.error_count != 0) {
		parse_result_free(&pr);
		test_fail_msg("trailing comma in a func group should now parse");
		return;
	}
	parse_result_free(&pr);
	test_pass_msg();
}

/* ========== MULTIPLE DECLARATIONS ========== */

void test_multiple_decls(void) {
	test_start("multiple declarations");
	ParseResult pr = parse_src("Player :: arche { x :: Float }\n"
	                           "init :: proc() {}\n"
	                           "integrate :: sys(pos) {}\n");
	SyntaxView root = root_view(&pr);
	ASSERT_NOT_NULL(root.node, "no syntax root");
	ASSERT_EQ(decl_count(root), 3, "expected 3 decls");
	ASSERT_EQ(sv_kind(rhs_form(decl_at(root, 0))), SN_ARCH_EXPR, "decl 0 should be archetype");
	ASSERT_EQ(sv_kind(rhs_form(decl_at(root, 1))), SN_PROC_EXPR, "decl 1 should be proc");
	ASSERT_EQ(sv_kind(rhs_form(decl_at(root, 2))), SN_SYS_EXPR, "decl 2 should be sys");
	parse_result_free(&pr);
	test_pass_msg();
}

/* ========== LEXER TESTS ========== */

void test_lex_move_keyword(void) {
	test_start("lex move keyword");
	Lexer lex;
	lexer_init(&lex, "move foo");
	Token t1 = lexer_next_token(&lex);
	if (t1.kind != TOK_MOVE) {
		lexer_free(&lex);
		test_fail_msg("first token should be TOK_MOVE");
		return;
	}
	Token t2 = lexer_next_token(&lex);
	if (t2.kind != TOK_IDENT) {
		lexer_free(&lex);
		test_fail_msg("second token should be TOK_IDENT");
		return;
	}
	lexer_free(&lex);
	test_pass_msg();
}

void test_lex_own_keyword(void) {
	test_start("lex own keyword");
	Lexer lex;
	lexer_init(&lex, "own foo");
	Token t1 = lexer_next_token(&lex);
	if (t1.kind != TOK_OWN) {
		lexer_free(&lex);
		test_fail_msg("first token should be TOK_OWN");
		return;
	}
	Token t2 = lexer_next_token(&lex);
	if (t2.kind != TOK_IDENT) {
		lexer_free(&lex);
		test_fail_msg("second token should be TOK_IDENT");
		return;
	}
	lexer_free(&lex);
	test_pass_msg();
}

/* ========== OWN PARAMETER MODIFIER TESTS ========== */

void test_own_param_modifier(void) {
	test_start("own parameter modifier");
	ParseResult pr = parse_src("window :: opaque\n"
	                           "#foreign { window_close :: proc(own w: window) }\n");
	SyntaxView root = root_view(&pr);
	ASSERT_NOT_NULL(root.node, "no syntax root");
	ASSERT_EQ(decl_count(root), 2, "expected 2 decls");
	/* the proc lives inside the #foreign region (SN_REGION) */
	SyntaxView region = decl_at(root, 1);
	ASSERT_EQ(sv_kind(region), SN_REGION, "decl 1 should be SN_REGION");
	SyntaxView proc_decl = sv_child(region, SN_CONST_DECL);
	ASSERT_NOT_NULL(proc_decl.node, "expected a proc decl inside the region");
	SyntaxView proc = rhs_form(proc_decl);
	ASSERT_EQ(sv_kind(proc), SN_PROC_EXPR, "expected SN_PROC_EXPR");
	ASSERT_EQ(sv_count(proc, SN_PARAM), 1, "expected 1 param");
	ASSERT_TRUE(sv_has_token(sv_child(proc, SN_PARAM), TOK_OWN), "param should be own");
	parse_result_free(&pr);
	test_pass_msg();
}

void test_out_keyword_rejected(void) {
	test_start("removed `out` keyword is rejected");
	ParseResult pr = parse_src("window :: opaque\n"
	                           "#foreign { bad :: proc(out w: window) }\n");
	ASSERT_TRUE(pr.error_count >= 1, "expected a parse error for `out` parameter");
	parse_result_free(&pr);
	test_pass_msg();
}

/* ========== ASSIGNMENT OPERATORS ========== */

void test_assign_op_eq(void) {
	test_start("assignment operator =");
	ParseResult pr = parse_src("test :: proc() { x = 5; }");
	SyntaxView root = root_view(&pr);
	ASSERT_NOT_NULL(root.node, "no syntax root");
	SyntaxView proc = rhs_form(decl_at(root, 0));
	ASSERT_EQ(sv_kind(stmt_at(proc, 0)), SN_ASSIGN_STMT, "wrong stmt");
	parse_result_free(&pr);
	test_pass_msg();
}

void test_assign_op_plus_eq(void) {
	test_start("assignment operator +=");
	ParseResult pr = parse_src("test :: proc() { x += 5; }");
	SyntaxView root = root_view(&pr);
	ASSERT_NOT_NULL(root.node, "no syntax root");
	SyntaxView proc = rhs_form(decl_at(root, 0));
	ASSERT_EQ(sv_kind(stmt_at(proc, 0)), SN_ASSIGN_STMT, "wrong stmt");
	ASSERT_TRUE(sv_has_token(stmt_at(proc, 0), TOK_PLUS_EQ), "expected += token");
	parse_result_free(&pr);
	test_pass_msg();
}

/* ========== MAIN TEST RUNNER ========== */

int main(void) {
	printf("Running parser tests...\n\n");

	/* Lexer tests */
	printf("Lexer tests:\n");
	test_lex_move_keyword();
	test_lex_own_keyword();

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

	/* System tests */
	printf("\nSystem tests:\n");
	test_sys_no_params_empty();
	test_sys_with_params();
	test_sys_with_body();

	/* Function tests */
	printf("\nFunction tests:\n");
	test_func_simple();
	test_func_multiple_params();
	test_parse_func_group();
	test_parse_func_group_empty_rejected();
	test_parse_func_group_trailing_comma();

	/* Expression tests */
	printf("\nExpression tests:\n");
	test_expr_literal();
	test_expr_field_access();
	test_expr_index();
	test_expr_binary_op();

	/* Let type annotation tests */
	printf("\nLet type annotation tests:\n");
	test_let_type_annotation_with_value();
	test_let_type_annotation_no_value();
	test_let_array_type_annotation();
	test_let_float_type_annotation();

	/* Printf variadic tests */
	printf("\nPrintf variadic tests:\n");
	test_printf_with_float();
	test_printf_with_variable_float();
	test_sprintf_with_float();

	/* Multiple declarations */
	printf("\nMultiple declarations tests:\n");
	test_multiple_decls();

	/* Own parameter modifier tests */
	printf("\nOwn parameter modifier tests:\n");
	test_own_param_modifier();
	test_out_keyword_rejected();

	/* Assignment operators */
	printf("\nAssignment operator tests:\n");
	test_assign_op_eq();
	test_assign_op_plus_eq();

	/* Regression tests */
	printf("\nRegression tests:\n");
	test_else_break();
	test_else_let();
	test_else_assign();
	test_nested_for_no_array_index();
	test_nested_for_else_assign();
	test_nested_let_simple();
	test_nested_let_in_for_loop();
	test_single_for_else_break();
	test_two_for_else_inner();
	test_printf_after_complex_nesting();
	test_toplevel_else();
	test_task1_structure();

	/* Results */
	printf("\n");
	printf("Results: %d/%d passed\n", test_pass, test_count);
	if (test_fail > 0) {
		printf("  %d failed\n", test_fail);
		return 1;
	}
	return 0;
}
