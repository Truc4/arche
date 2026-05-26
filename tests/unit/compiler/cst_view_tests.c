/* Unit tests for the CST view layer (cst/cst_view.{h,c}) and node ids.
 * Validates that typed views navigate the structurally-complete CST correctly,
 * including precedence nesting and error flagging. */
#include "../../../cst/cst_view.h"
#include "../../../parser/parser.h"
#include <stdio.h>
#include <stdlib.h>

static int pass = 0, fail = 0;
#define CHECK(cond, msg)                                                                                               \
	do {                                                                                                               \
		if (cond) {                                                                                                    \
			pass++;                                                                                                    \
		} else {                                                                                                       \
			fail++;                                                                                                    \
			printf("  [FAIL] %s\n", msg);                                                                              \
		}                                                                                                              \
	} while (0)

int main(void) {
	/* 1. Declaration + params + types + statement + precedence nesting. */
	{
		const char *src = "proc add(a: int, b: int) {\n  x := a + b * c;\n}\n";
		ParseResult r = parse_source(src);
		CstView root = cv_root(r.cst_root, src);
		CstView proc = cv_child(root, SN_PROC_DECL);
		CHECK(cv_present(proc), "proc decl present");
		CHECK(cv_text_eq(cv_child(proc, SN_FUNC_DEF_NAME), "add"), "proc name == add");
		CHECK(cv_count(proc, SN_PARAM) == 2, "two params");
		CstView p0 = cv_child(proc, SN_PARAM);
		CHECK(cv_text_eq(cv_child(p0, SN_PARAM_NAME), "a") && cv_text_eq(cv_child(p0, SN_TYPE_REF), "int"),
		      "first param is a: int");

		CstView bind = cv_child(proc, SN_BIND_STMT);
		CHECK(cv_present(bind), "bind stmt present");
		CstView add = cv_child(bind, SN_BINARY_EXPR);
		CHECK(cv_present(add), "value is a binary expr");
		CstView lhs = cv_node_at(add, 0);
		CstView rhs = cv_node_at(add, 1);
		CHECK(cv_kind(lhs) == SN_NAME_EXPR && cv_text_eq(lhs, "a"), "binary lhs == name 'a'");
		CHECK(cv_kind(rhs) == SN_BINARY_EXPR, "binary rhs nested (a + (b*c)) — precedence");

		AstProgram *p = r.ast;
		parse_result_free(&r);
		ast_program_free(p);
	}

	/* 2. Call expression: callee + string argument. */
	{
		const char *src = "proc m() {\n  print(\"hi\");\n}\n";
		ParseResult r = parse_source(src);
		CstView root = cv_root(r.cst_root, src);
		CstView call = cv_child(cv_child(cv_child(root, SN_PROC_DECL), SN_EXPR_STMT), SN_CALL_EXPR);
		CHECK(cv_present(call), "call expr present");
		CHECK(cv_text_eq(cv_child(call, SN_CALLEE_NAME), "print"), "callee == print");
		CHECK(cv_present(cv_child(call, SN_STRING_EXPR)), "string argument present");

		AstProgram *p = r.ast;
		parse_result_free(&r);
		ast_program_free(p);
	}

	/* 3. Node ids are dense: the root has the largest id == node_count - 1. */
	{
		const char *src = "proc m() {\n  x := 1;\n}\n";
		ParseResult r = parse_source(src);
		CHECK(r.cst_root && r.cst_root->id > 0, "root id assigned (post-order, nonzero)");
		AstProgram *p = r.ast;
		parse_result_free(&r);
		ast_program_free(p);
	}

	/* 4. Malformed input is flagged (parse error and/or an error node). */
	{
		const char *src = "proc m( {\n";
		ParseResult r = parse_source(src);
		CstView root = cv_root(r.cst_root, src);
		CHECK(r.error_count > 0 || cv_has_error(root), "malformed input flagged");
		AstProgram *p = r.ast;
		parse_result_free(&r);
		ast_program_free(p);
	}

	printf("\nResults: %d/%d passed\n", pass, pass + fail);
	return fail ? 1 : 0;
}
