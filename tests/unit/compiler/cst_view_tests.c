/* Unit tests for the CST view layer (cst/cst_view.{h,c}) and node ids.
 * Validates that typed views navigate the structurally-complete CST correctly,
 * including precedence nesting and error flagging. */
#include "../../../cst/cst_view.h"
#include "../../../parser/parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static int cvtext_eq(CvText t, const char *s) {
	size_t n = strlen(s);
	return t.ptr && t.len == n && memcmp(t.ptr, s, n) == 0;
}

int main(void) {
	/* 1. Declaration + params + types + statement + precedence nesting. */
	{
		const char *src = "add :: proc(a: int, b: int) {\n  x := a + b * c;\n}\n";
		ParseResult r = parse_source(src);
		CstView root = cv_root(r.cst_root, src);
		/* Unified grammar: `add :: proc(...)` is a const-decl binding whose value is an
		 * SN_PROC_EXPR; the name is the decl's IDENT token (no SN_FUNC_DEF_NAME). */
		CstView decl = cv_child(root, SN_CONST_DECL);
		CHECK(cv_present(decl), "const decl present");
		CHECK(cvtext_eq(cv_token(decl, TOK_IDENT), "add"), "binding name == add");
		CstView proc = cv_child(decl, SN_PROC_EXPR);
		CHECK(cv_present(proc), "proc value form present");
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
		const char *src = "m :: proc() {\n  print(\"hi\");\n}\n";
		ParseResult r = parse_source(src);
		CstView root = cv_root(r.cst_root, src);
		CstView call =
		    cv_child(cv_child(cv_child(cv_child(root, SN_CONST_DECL), SN_PROC_EXPR), SN_EXPR_STMT), SN_CALL_EXPR);
		CHECK(cv_present(call), "call expr present");
		CHECK(cv_text_eq(cv_child(call, SN_CALLEE_NAME), "print"), "callee == print");
		CHECK(cv_present(cv_child(call, SN_STRING_EXPR)), "string argument present");

		AstProgram *p = r.ast;
		parse_result_free(&r);
		ast_program_free(p);
	}

	/* 3. Node ids are dense: the root has the largest id == node_count - 1. */
	{
		const char *src = "m :: proc() {\n  x := 1;\n}\n";
		ParseResult r = parse_source(src);
		CHECK(r.cst_root && r.cst_root->id > 0, "root id assigned (post-order, nonzero)");
		AstProgram *p = r.ast;
		parse_result_free(&r);
		ast_program_free(p);
	}

	/* 4. Malformed input is flagged (parse error and/or an error node). */
	{
		const char *src = "m :: proc( {\n";
		ParseResult r = parse_source(src);
		CstView root = cv_root(r.cst_root, src);
		CHECK(r.error_count > 0 || cv_has_error(root), "malformed input flagged");
		AstProgram *p = r.ast;
		parse_result_free(&r);
		ast_program_free(p);
	}

	/* 5. Doc-comment classifiers (the single source of truth for the doc marker). */
	{
		CHECK(arche_is_doc_comment("/// x", 5), "/// is a doc comment");
		CHECK(!arche_is_doc_comment("// x", 4), "// is not a doc comment");
		CHECK(!arche_is_doc_comment("//// banner", 11), "//// banner is not a doc comment");
		CHECK(!arche_is_doc_comment("//! x", 5), "//! is not an outer doc comment");
		CHECK(arche_is_inner_doc_comment("//! x", 5), "//! is an inner doc comment");
		CHECK(!arche_is_inner_doc_comment("/// x", 5), "/// is not an inner doc comment");
	}

	/* 6. Doc lines attach to the FOLLOWING decl — including the 2nd decl, whose
	 * doc the parser absorbs as a trailing leaf of the 1st (regression). */
	{
		const char *src = "/// First line.\n/// Second line.\na :: func() -> int { return 0; }\n\n"
		                  "/// Doc for b.\nb :: func() -> int { return 1; }\n";
		ParseResult r = parse_source(src);
		CstView root = cv_root(r.cst_root, src);
		CvText la[8];
		int lna[8];
		int na = cv_decl_doc_lines(root, cv_node_at(root, 0), la, lna, 8);
		CHECK(na == 2, "decl a has 2 doc lines");
		CHECK(na == 2 && cvtext_eq(la[0], "First line.") && cvtext_eq(la[1], "Second line."),
		      "a doc text + marker strip");
		CHECK(na == 2 && lna[0] == 1 && lna[1] == 2, "a doc line numbers reported");
		CvText lb[8];
		int lnb[8];
		int nb = cv_decl_doc_lines(root, cv_node_at(root, 1), lb, lnb, 8);
		CHECK(nb == 1 && cvtext_eq(lb[0], "Doc for b."), "decl b doc attaches (absorbed-comment regression)");
		CHECK(nb == 1 && lnb[0] == 5, "decl b doc line number reported");
		AstProgram *p = r.ast;
		parse_result_free(&r);
		ast_program_free(p);
	}

	/* 7. A blank line, and a plain //, both break attachment. */
	{
		const char *src = "/// detached\n\na :: func() -> int { return 0; }\n";
		ParseResult r = parse_source(src);
		CstView root = cv_root(r.cst_root, src);
		CvText l[8];
		CHECK(cv_decl_doc_lines(root, cv_node_at(root, 0), l, NULL, 8) == 0, "blank line detaches doc comment");
		AstProgram *p = r.ast;
		parse_result_free(&r);
		ast_program_free(p);
	}
	{
		const char *src = "// plain\na :: func() -> int { return 0; }\n";
		ParseResult r = parse_source(src);
		CstView root = cv_root(r.cst_root, src);
		CvText l[8];
		CHECK(cv_decl_doc_lines(root, cv_node_at(root, 0), l, NULL, 8) == 0, "plain // does not attach");
		AstProgram *p = r.ast;
		parse_result_free(&r);
		ast_program_free(p);
	}

	/* 8. Module-level //! inner-doc lines. */
	{
		const char *src = "//! Module doc.\n//! Line two.\na :: func() -> int { return 0; }\n";
		ParseResult r = parse_source(src);
		CstView root = cv_root(r.cst_root, src);
		CvText l[8];
		int n = cv_module_doc_lines(root, l, 8);
		CHECK(n == 2 && cvtext_eq(l[0], "Module doc.") && cvtext_eq(l[1], "Line two."), "module //! doc lines");
		AstProgram *p = r.ast;
		parse_result_free(&r);
		ast_program_free(p);
	}

	printf("\nResults: %d/%d passed\n", pass, pass + fail);
	return fail ? 1 : 0;
}
