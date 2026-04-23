#include "lexer/lexer.h"
#include "parser/parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main() {
	const char *src = "arche Test { data: char[256] } proc main() {}";
	fprintf(stderr, "Calling parse_source...\n");
	fflush(stderr);
	ParseResult result = parse_source(src);
	fprintf(stderr, "parse_source returned\n");
	fflush(stderr);
	return 0;
}
