/* Test parser with structure matching task_1 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../../parser/parser.h"

static void test_task1_structure(void) {
	const char *source = "arche Transaction {\n"
	                      "  price: float,\n"
	                      "  quantity: int,\n"
	                      "  revenue: float,\n"
	                      "}\n"
	                      "\n"
	                      "static Transaction(1000, 1000) {\n"
	                      "  price: 0.0,\n"
	                      "  quantity: 0,\n"
	                      "  revenue: 0.0,\n"
	                      "};\n"
	                      "\n"
	                      "proc main() {\n"
	                      "  let fd := 1;\n"
	                      "  let line: char[128];\n"
	                      "  let price_str: char[32];\n"
	                      "  let qty_str: char[32];\n"
	                      "\n"
	                      "  let idx := 0;\n"
	                      "  for (;idx < 1000;) {\n"
	                      "    let n := 10;\n"
	                      "    if (n <= 0) { break; }\n"
	                      "\n"
	                      "    let line_pos := 0;\n"
	                      "    let field_idx := 0;\n"
	                      "    let c1 := 0;\n"
	                      "    let c2 := 0;\n"
	                      "\n"
	                      "    for (;line_pos < n;) {\n"
	                      "      if (line[line_pos] == ',') {\n"
	                      "        field_idx = field_idx + 1;\n"
	                      "        if (field_idx == 1) { c1 = line_pos; }\n"
	                      "        line_pos = line_pos + 1;\n"
	                      "      } else {\n"
	                      "        line_pos = line_pos + 1;\n"
	                      "      }\n"
	                      "    }\n"
	                      "\n"
	                      "    let i := 0;\n"
	                      "    let j := 0;\n"
	                      "    for (;i < c2 - c1 - 1;) {\n"
	                      "      price_str[j] = line[c1 + 1 + i];\n"
	                      "      j = j + 1;\n"
	                      "      i = i + 1;\n"
	                      "    }\n"
	                      "    price_str[j] = 0;\n"
	                      "\n"
	                      "    idx = idx + 1;\n"
	                      "  }\n"
	                      "\n"
	                      "  let sum := 0.0;\n"
	                      "  let m := 0;\n"
	                      "  for (;m < 1000;) {\n"
	                      "    sum = sum + 1.0;\n"
	                      "    m = m + 1;\n"
	                      "  }\n"
	                      "\n"
	                      "  printf(\"result: %g\\n\", sum);\n"
	                      "}\n";

	ParseResult result = parse_source(source);

	if (result.error_count != 0) {
		printf("FAIL: task1_structure - parser reported %ld errors\n", result.error_count);
		for (size_t i = 0; i < result.error_count && i < 10; i++) {
			printf("  [Line %d, Col %d] %s\n", result.errors[i].line, result.errors[i].column, result.errors[i].message);
		}
		parse_result_free(&result);
		return;
	}

	printf("PASS: task1_structure\n");
	parse_result_free(&result);
}

int main(void) {
	printf("=== Parser Task1 Structure Test ===\n\n");
	test_task1_structure();
	return 0;
}
