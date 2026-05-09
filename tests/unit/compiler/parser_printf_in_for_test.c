/* Test for parser issue with string literals in complex nested contexts */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../../parser/parser.h"

/* Test 1: printf with string in simple proc */
static void test_printf_simple(void) {
	const char *source = "proc main() {\n"
	                      "  let sum := 0.0;\n"
	                      "  printf(\"test: %g\\n\", sum);\n"
	                      "}\n";

	ParseResult result = parse_source(source);

	if (result.error_count != 0) {
		printf("FAIL: printf_simple - parser reported %ld errors\n", result.error_count);
		for (size_t i = 0; i < result.error_count && i < 5; i++) {
			printf("  [Line %d, Col %d] %s\n", result.errors[i].line, result.errors[i].column, result.errors[i].message);
		}
		parse_result_free(&result);
		return;
	}

	printf("PASS: printf_simple\n");
	parse_result_free(&result);
}

/* Test 2: printf with string after nested for loops */
static void test_printf_after_for(void) {
	const char *source = "proc main() {\n"
	                      "  let sum := 0.0;\n"
	                      "  let m := 0;\n"
	                      "  for (;m < 10;) {\n"
	                      "    sum = sum + 1.0;\n"
	                      "    m = m + 1;\n"
	                      "  }\n"
	                      "  printf(\"result: %g\\n\", sum);\n"
	                      "}\n";

	ParseResult result = parse_source(source);

	if (result.error_count != 0) {
		printf("FAIL: printf_after_for - parser reported %ld errors\n", result.error_count);
		for (size_t i = 0; i < result.error_count && i < 5; i++) {
			printf("  [Line %d, Col %d] %s\n", result.errors[i].line, result.errors[i].column, result.errors[i].message);
		}
		parse_result_free(&result);
		return;
	}

	printf("PASS: printf_after_for\n");
	parse_result_free(&result);
}

/* Test 3: printf after multiple nested for loops with let statements */
static void test_printf_after_complex_nesting(void) {
	const char *source = "proc main() {\n"
	                      "  let sum := 0.0;\n"
	                      "  for (;0 < 1;) {\n"
	                      "    let idx := 0;\n"
	                      "    for (;idx < 5;) {\n"
	                      "      let x := 1.0;\n"
	                      "      sum = sum + x;\n"
	                      "      idx = idx + 1;\n"
	                      "    }\n"
	                      "    break;\n"
	                      "  }\n"
	                      "  printf(\"sum: %g\\n\", sum);\n"
	                      "}\n";

	ParseResult result = parse_source(source);

	if (result.error_count != 0) {
		printf("FAIL: printf_after_complex_nesting - parser reported %ld errors\n", result.error_count);
		for (size_t i = 0; i < result.error_count && i < 5; i++) {
			printf("  [Line %d, Col %d] %s\n", result.errors[i].line, result.errors[i].column, result.errors[i].message);
		}
		parse_result_free(&result);
		return;
	}

	printf("PASS: printf_after_complex_nesting\n");
	parse_result_free(&result);
}

int main(void) {
	printf("=== Parser Printf in Context Tests ===\n\n");

	test_printf_simple();
	test_printf_after_for();
	test_printf_after_complex_nesting();

	printf("\n=== Summary ===\n");
	printf("If any test fails, the parser has issues with string literals in certain contexts.\n");

	return 0;
}
