/* Test if else clause parsing works at top level */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../../parser/parser.h"

/* If-else at top level of proc */
static void test_toplevel_else(void) {
	const char *source = "proc main() {\n"
	                      "  if (1 == 1) { let x := 1; }\n"
	                      "  else { break; }\n"
	                      "}\n";

	ParseResult result = parse_source(source);
	if (result.error_count != 0) {
		printf("FAIL: toplevel_else - %ld errors\n", result.error_count);
	} else {
		printf("PASS: toplevel_else\n");
	}
	parse_result_free(&result);
}

/* If-else inside for */
static void test_for_else(void) {
	const char *source = "proc main() {\n"
	                      "  let idx := 0;\n"
	                      "  for (;idx < 10;) {\n"
	                      "    if (1 == 1) { idx = 1; }\n"
	                      "    else { break; }\n"
	                      "  }\n"
	                      "}\n";

	ParseResult result = parse_source(source);
	if (result.error_count != 0) {
		printf("FAIL: for_else - %ld errors\n", result.error_count);
	} else {
		printf("PASS: for_else\n");
	}
	parse_result_free(&result);
}

/* If without else inside for (should pass) */
static void test_for_if_noelse(void) {
	const char *source = "proc main() {\n"
	                      "  let idx := 0;\n"
	                      "  for (;idx < 10;) {\n"
	                      "    if (1 == 1) { idx = 1; }\n"
	                      "  }\n"
	                      "}\n";

	ParseResult result = parse_source(source);
	if (result.error_count != 0) {
		printf("FAIL: for_if_noelse - %ld errors\n", result.error_count);
	} else {
		printf("PASS: for_if_noelse\n");
	}
	parse_result_free(&result);
}

int main(void) {
	printf("=== Parser Statement Else Tests ===\n\n");
	test_toplevel_else();
	test_for_else();
	test_for_if_noelse();
	return 0;
}
