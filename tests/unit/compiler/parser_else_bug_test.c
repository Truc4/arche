/* Narrow down: is it else block or assignment in else? */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../../parser/parser.h"

/* Just else with break - should pass */
static void test_else_break(void) {
	const char *source = "proc main() {\n"
	                      "  let idx := 0;\n"
	                      "  for (;idx < 10;) {\n"
	                      "    let line_pos := 0;\n"
	                      "    for (;line_pos < 5;) {\n"
	                      "      if (1 == 1) { line_pos = 1; }\n"
	                      "      else { break; }\n"
	                      "    }\n"
	                      "  }\n"
	                      "}\n";

	ParseResult result = parse_source(source);
	if (result.error_count != 0) {
		printf("FAIL: else_break - %ld errors\n", result.error_count);
	} else {
		printf("PASS: else_break\n");
	}
	parse_result_free(&result);
}

/* Else with let */
static void test_else_let(void) {
	const char *source = "proc main() {\n"
	                      "  let idx := 0;\n"
	                      "  for (;idx < 10;) {\n"
	                      "    let line_pos := 0;\n"
	                      "    for (;line_pos < 5;) {\n"
	                      "      if (1 == 1) { line_pos = 1; }\n"
	                      "      else { let x := 1; }\n"
	                      "    }\n"
	                      "  }\n"
	                      "}\n";

	ParseResult result = parse_source(source);
	if (result.error_count != 0) {
		printf("FAIL: else_let - %ld errors\n", result.error_count);
	} else {
		printf("PASS: else_let\n");
	}
	parse_result_free(&result);
}

/* Else with simple assignment */
static void test_else_assign(void) {
	const char *source = "proc main() {\n"
	                      "  let idx := 0;\n"
	                      "  for (;idx < 10;) {\n"
	                      "    let line_pos := 0;\n"
	                      "    for (;line_pos < 5;) {\n"
	                      "      if (1 == 1) { line_pos = 1; }\n"
	                      "      else { line_pos = 2; }\n"
	                      "    }\n"
	                      "  }\n"
	                      "}\n";

	ParseResult result = parse_source(source);
	if (result.error_count != 0) {
		printf("FAIL: else_assign - %ld errors at [Line %d, Col %d]\n",
		       result.error_count, result.errors[0].line, result.errors[0].column);
	} else {
		printf("PASS: else_assign\n");
	}
	parse_result_free(&result);
}

int main(void) {
	printf("=== Parser Else Bug Tests ===\n\n");
	test_else_break();
	test_else_let();
	test_else_assign();
	return 0;
}
