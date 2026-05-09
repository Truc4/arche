/* Test for parser bug: nested let declarations in for loops
 *
 * Bug: When a 'let' statement appears inside a nested for loop,
 * the parser reports errors at non-existent line numbers (30+ lines
 * past EOF). This indicates improper state management or error
 * recovery when exiting nested scopes.
 *
 * Root cause: parse_statement() increments recursion_depth but
 * doesn't properly maintain parser state across nested contexts.
 */

#include "../../../ast/ast.h"
#include "../../../lexer/lexer.h"
#include "../../../parser/parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_nested_let_in_for_loop(void) {
	const char *source = "proc main() {\n"
	                     "  for (;1 > 0;) {\n"
	                     "    let x := 0;\n"
	                     "    for (;x < 5;) {\n"
	                     "      x = x + 1;\n"
	                     "    }\n"
	                     "    break;\n"
	                     "  }\n"
	                     "}\n";

	ParseResult result = parse_source(source);

	/* Verify no parse errors */
	if (result.error_count != 0) {
		printf("FAIL: nested_let_in_for_loop - parser reported %ld errors\n", result.error_count);
		if (result.errors) {
			for (size_t i = 0; i < result.error_count && i < 3; i++) {
				printf("  Error %ld: %s\n", i + 1, result.errors[i].message);
			}
		}
		parse_result_free(&result);
		return;
	}

	Program *prog = result.ast;

	/* Verify program structure */
	if (!prog || !prog->decls || prog->decl_count != 1) {
		printf("FAIL: nested_let_in_for_loop - expected 1 declaration\n");
		return;
	}

	Decl *decl = prog->decls[0];
	if (decl->kind != DECL_PROC) {
		printf("FAIL: nested_let_in_for_loop - expected DECL_PROC\n");
		return;
	}

	ProcDecl *proc = decl->data.proc;
	if (!proc->statements || proc->statement_count < 1) {
		printf("FAIL: nested_let_in_for_loop - expected for loop in main body\n");
		return;
	}

	Statement *for_stmt = proc->statements[0];
	if (for_stmt->type != STMT_FOR) {
		printf("FAIL: nested_let_in_for_loop - expected STMT_FOR\n");
		return;
	}

	ForStmt *for_loop = &for_stmt->data.for_stmt;
	if (!for_loop->body || for_loop->body_count < 2) {
		printf("FAIL: nested_let_in_for_loop - expected 2+ statements in for body\n");
		return;
	}

	/* First statement in for body should be let */
	if (for_loop->body[0]->type != STMT_LET) {
		printf("FAIL: nested_let_in_for_loop - expected STMT_LET in for body\n");
		return;
	}

	/* Second statement should be nested for loop */
	if (for_loop->body[1]->type != STMT_FOR) {
		printf("FAIL: nested_let_in_for_loop - expected nested STMT_FOR\n");
		parse_result_free(&result);
		return;
	}

	printf("PASS: nested_let_in_for_loop\n");
	parse_result_free(&result);
}

static void test_nested_let_simple(void) {
	/* Even simpler case: let inside a for loop (no nested loop) */
	const char *source = "proc main() {\n"
	                     "  for (;1 > 0;) {\n"
	                     "    let x := 5;\n"
	                     "    break;\n"
	                     "  }\n"
	                     "}\n";

	ParseResult result = parse_source(source);

	if (result.error_count != 0) {
		printf("FAIL: nested_let_simple - parser reported %ld errors\n", result.error_count);
		parse_result_free(&result);
		return;
	}

	Program *prog = result.ast;
	if (!prog || !prog->decls || prog->decl_count != 1) {
		printf("FAIL: nested_let_simple - expected 1 declaration\n");
		parse_result_free(&result);
		return;
	}

	printf("PASS: nested_let_simple\n");
	parse_result_free(&result);
}

int main(void) {
	printf("=== Parser Nested Let Tests ===\n");
	printf("Testing edge case: let declarations in nested for loops\n\n");

	test_nested_let_simple();
	test_nested_let_in_for_loop();

	printf("\n=== Summary ===\n");
	printf("If tests fail with \"Expected ';'\" or \"Expected declaration\" errors\n");
	printf("at non-existent line numbers, the parser bug is present.\n");

	return 0;
}
