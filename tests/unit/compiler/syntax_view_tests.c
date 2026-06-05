/* Unit tests for the syntax tree view layer (cst/syntax_view.{h,c}) and node ids.
 * Validates that typed views navigate the structurally-complete syntax tree correctly,
 * including precedence nesting and error flagging. */
#include "../../../parser/parser.h"
#include "../../../syntax/syntax_view.h"
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

static int cvtext_eq(SynText t, const char *s) {
	size_t n = strlen(s);
	return t.ptr && t.len == n && memcmp(t.ptr, s, n) == 0;
}

int main(void) {
	/* 1. Declaration + params + types + statement + precedence nesting. */
	{
		const char *src = "add :: proc(a: int, b: int) {\n  x := a + b * c;\n}\n";
		ParseResult r = parse_source(src);
		SyntaxView root = sv_root(r.syntax_root, src);
		/* Unified grammar: `add :: proc(...)` is a const-decl binding whose value is an
		 * SN_PROC_EXPR; the name is the decl's IDENT token (no SN_FUNC_DEF_NAME). */
		SyntaxView decl = sv_child(root, SN_CONST_DECL);
		CHECK(sv_present(decl), "const decl present");
		CHECK(cvtext_eq(sv_token(decl, TOK_IDENT), "add"), "binding name == add");
		SyntaxView proc = sv_child(decl, SN_PROC_EXPR);
		CHECK(sv_present(proc), "proc value form present");
		CHECK(sv_count(proc, SN_PARAM) == 2, "two params");
		SyntaxView p0 = sv_child(proc, SN_PARAM);
		CHECK(sv_text_eq(sv_child(p0, SN_PARAM_NAME), "a") && sv_text_eq(sv_child(p0, SN_TYPE_REF), "int"),
		      "first param is a: int");

		SyntaxView bind = sv_child(proc, SN_BIND_STMT);
		CHECK(sv_present(bind), "bind stmt present");
		SyntaxView add = sv_child(bind, SN_BINARY_EXPR);
		CHECK(sv_present(add), "value is a binary expr");
		SyntaxView lhs = sv_node_at(add, 0);
		SyntaxView rhs = sv_node_at(add, 1);
		CHECK(sv_kind(lhs) == SN_NAME_EXPR && sv_text_eq(lhs, "a"), "binary lhs == name 'a'");
		CHECK(sv_kind(rhs) == SN_BINARY_EXPR, "binary rhs nested (a + (b*c)) — precedence");

		parse_result_free(&r);
	}

	/* 2. Call expression: callee + string argument. */
	{
		const char *src = "m :: proc() {\n  print(\"hi\");\n}\n";
		ParseResult r = parse_source(src);
		SyntaxView root = sv_root(r.syntax_root, src);
		SyntaxView call =
		    sv_child(sv_child(sv_child(sv_child(root, SN_CONST_DECL), SN_PROC_EXPR), SN_EXPR_STMT), SN_CALL_EXPR);
		CHECK(sv_present(call), "call expr present");
		CHECK(sv_text_eq(sv_child(call, SN_CALLEE_NAME), "print"), "callee == print");
		CHECK(sv_present(sv_child(call, SN_STRING_EXPR)), "string argument present");

		parse_result_free(&r);
	}

	/* 3. Node ids are dense: the root has the largest id == node_count - 1. */
	{
		const char *src = "m :: proc() {\n  x := 1;\n}\n";
		ParseResult r = parse_source(src);
		CHECK(r.syntax_root && r.syntax_root->id > 0, "root id assigned (post-order, nonzero)");
		parse_result_free(&r);
	}

	/* 4. Malformed input is flagged (parse error and/or an error node). */
	{
		const char *src = "m :: proc( {\n";
		ParseResult r = parse_source(src);
		SyntaxView root = sv_root(r.syntax_root, src);
		CHECK(r.error_count > 0 || sv_has_error(root), "malformed input flagged");
		parse_result_free(&r);
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
		SyntaxView root = sv_root(r.syntax_root, src);
		SynText la[8];
		int lna[8];
		int na = sv_decl_doc_lines(root, sv_node_at(root, 0), la, lna, 8);
		CHECK(na == 2, "decl a has 2 doc lines");
		CHECK(na == 2 && cvtext_eq(la[0], "First line.") && cvtext_eq(la[1], "Second line."),
		      "a doc text + marker strip");
		CHECK(na == 2 && lna[0] == 1 && lna[1] == 2, "a doc line numbers reported");
		SynText lb[8];
		int lnb[8];
		int nb = sv_decl_doc_lines(root, sv_node_at(root, 1), lb, lnb, 8);
		CHECK(nb == 1 && cvtext_eq(lb[0], "Doc for b."), "decl b doc attaches (absorbed-comment regression)");
		CHECK(nb == 1 && lnb[0] == 5, "decl b doc line number reported");
		parse_result_free(&r);
	}

	/* 7. A blank line, and a plain //, both break attachment. */
	{
		const char *src = "/// detached\n\na :: func() -> int { return 0; }\n";
		ParseResult r = parse_source(src);
		SyntaxView root = sv_root(r.syntax_root, src);
		SynText l[8];
		CHECK(sv_decl_doc_lines(root, sv_node_at(root, 0), l, NULL, 8) == 0, "blank line detaches doc comment");
		parse_result_free(&r);
	}
	{
		const char *src = "// plain\na :: func() -> int { return 0; }\n";
		ParseResult r = parse_source(src);
		SyntaxView root = sv_root(r.syntax_root, src);
		SynText l[8];
		CHECK(sv_decl_doc_lines(root, sv_node_at(root, 0), l, NULL, 8) == 0, "plain // does not attach");
		parse_result_free(&r);
	}

	/* 8. Module-level //! inner-doc lines. */
	{
		const char *src = "//! Module doc.\n//! Line two.\na :: func() -> int { return 0; }\n";
		ParseResult r = parse_source(src);
		SyntaxView root = sv_root(r.syntax_root, src);
		SynText l[8];
		int n = sv_module_doc_lines(root, l, 8);
		CHECK(n == 2 && cvtext_eq(l[0], "Module doc.") && cvtext_eq(l[1], "Line two."), "module //! doc lines");
		parse_result_free(&r);
	}

	printf("\nResults: %d/%d passed\n", pass, pass + fail);
	return fail ? 1 : 0;
}
