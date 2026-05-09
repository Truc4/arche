/* Test if it's about nesting depth or if-else */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../../parser/parser.h"

/* Single for loop with if-else-break */
static void test_single_for_else_break(void) {
	const char *source = "proc main() {\n"
	                      "  let idx := 0;\n"
	                      "  for (;idx < 10;) {\n"
	                      "    if (1 == 1) { idx = 1; }\n"
	                      "    else { break; }\n"
	                      "  }\n"
	                      "}\n";

	ParseResult result = parse_source(source);
	if (result.error_count != 0) {
		printf("FAIL: single_for_else_break - %ld errors\n", result.error_count);
	} else {
		printf("PASS: single_for_else_break\n");
	}
	parse_result_free(&result);
}

/* Two nested for loops, if-else at outer level */
static void test_two_for_else_outer(void) {
	const char *source = "proc main() {\n"
	                      "  let idx := 0;\n"
	                      "  for (;idx < 10;) {\n"
	                      "    let pos := 0;\n"
	                      "    for (;pos < 5;) {\n"
	                      "      pos = pos + 1;\n"
	                      "    }\n"
	                      "    if (1 == 1) { idx = 1; }\n"
	                      "    else { break; }\n"
	                      "  }\n"
	                      "}\n";

	ParseResult result = parse_source(source);
	if (result.error_count != 0) {
		printf("FAIL: two_for_else_outer - %ld errors\n", result.error_count);
	} else {
		printf("PASS: two_for_else_outer\n");
	}
	parse_result_free(&result);
}

/* Two nested for loops, if-else at inner level (the failing case) */
static void test_two_for_else_inner(void) {
	const char *source = "proc main() {\n"
	                      "  let idx := 0;\n"
	                      "  for (;idx < 10;) {\n"
	                      "    let pos := 0;\n"
	                      "    for (;pos < 5;) {\n"
	                      "      if (1 == 1) { pos = 1; }\n"
	                      "      else { break; }\n"
	                      "    }\n"
	                      "  }\n"
	                      "}\n";

	ParseResult result = parse_source(source);
	if (result.error_count != 0) {
		printf("FAIL: two_for_else_inner - %ld errors\n", result.error_count);
	} else {
		printf("PASS: two_for_else_inner\n");
	}
	parse_result_free(&result);
}

int main(void) {
	printf("=== Parser Nesting Bug Tests ===\n\n");
	test_single_for_else_break();
	test_two_for_else_outer();
	test_two_for_else_inner();
	return 0;
}
