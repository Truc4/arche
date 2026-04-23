#include "ast/ast.h"
#include "lexer/lexer.h"
#include "parser/parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main() {
	const char *src = "arche Test {\n  data: char[256]\n}\n\nproc main() {}\n";
	fprintf(stderr, "Starting parse...\n");
	fflush(stderr);
	ParseResult result = parse_source(src);
	fprintf(stderr, "Parse complete, errors: %d\n", result.error_count);
	fflush(stderr);
	fprintf(stderr, "Starting format...\n");
	fflush(stderr);
	format_program(stdout, result.ast);
	fprintf(stderr, "Format complete\n");
	fflush(stderr);
	return 0;
}
