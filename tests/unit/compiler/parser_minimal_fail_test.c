/* Minimal test to isolate parser bug */

#include "../../../parser/parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Remove array indexing - does it help? */
static void test_no_array_index(void) {
	const char *source = "proc main() {\n"
	                     "  let idx := 0;\n"
	                     "  for (;idx < 10;) {\n"
	                     "    let n := 5;\n"
	                     "    let line_pos := 0;\n"
	                     "    let field_idx := 0;\n"
	                     "    for (;line_pos < n;) {\n"
	                     "      if (field_idx == 1) { line_pos = line_pos + 1; }\n"
	                     "      else { line_pos = line_pos + 1; }\n"
	                     "    }\n"
	                     "    idx = idx + 1;\n"
	                     "  }\n"
	                     "}\n";

	ParseResult result = parse_source(source);
	if (result.error_count != 0) {
		printf("FAIL: no_array_index - %ld errors at [Line %d, Col %d]\n", result.error_count, result.errors[0].line,
		       result.errors[0].column);
	} else {
		printf("PASS: no_array_index\n");
	}
	parse_result_free(&result);
}

/* Simpler: just nested for with assignment in else */
static void test_nested_for_else_assign(void) {
	const char *source = "proc main() {\n"
	                     "  let idx := 0;\n"
	                     "  for (;idx < 10;) {\n"
	                     "    let n := 5;\n"
	                     "    let line_pos := 0;\n"
	                     "    for (;line_pos < n;) {\n"
	                     "      if (1 == 1) { line_pos = line_pos + 1; }\n"
	                     "    }\n"
	                     "    idx = idx + 1;\n"
	                     "  }\n"
	                     "}\n";

	ParseResult result = parse_source(source);
	if (result.error_count != 0) {
		printf("FAIL: nested_for_else_assign - %ld errors at [Line %d, Col %d]\n", result.error_count,
		       result.errors[0].line, result.errors[0].column);
	} else {
		printf("PASS: nested_for_else_assign\n");
	}
	parse_result_free(&result);
}

/* Add else clause */
static void test_nested_for_with_else(void) {
	const char *source = "proc main() {\n"
	                     "  let idx := 0;\n"
	                     "  for (;idx < 10;) {\n"
	                     "    let n := 5;\n"
	                     "    let line_pos := 0;\n"
	                     "    for (;line_pos < n;) {\n"
	                     "      if (1 == 1) { line_pos = line_pos + 1; }\n"
	                     "      else { line_pos = line_pos + 1; }\n"
	                     "    }\n"
	                     "    idx = idx + 1;\n"
	                     "  }\n"
	                     "}\n";

	ParseResult result = parse_source(source);
	if (result.error_count != 0) {
		printf("FAIL: nested_for_with_else - %ld errors at [Line %d, Col %d]\n", result.error_count,
		       result.errors[0].line, result.errors[0].column);
	} else {
		printf("PASS: nested_for_with_else\n");
	}
	parse_result_free(&result);
}

int main(void) {
	printf("=== Parser Minimal Fail Tests ===\n\n");
	test_no_array_index();
	test_nested_for_else_assign();
	test_nested_for_with_else();
	return 0;
}
