#include "lexer/lexer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main() {
	const char *src = "arche Test { data: char[256] }";
	fprintf(stderr, "Creating lexer...\n");
	Lexer lexer;
	lexer_init(&lexer, src);
	fprintf(stderr, "Lexing tokens...\n");
	int count = 0;
	while (count < 100) {
		Token tok = lexer_next_token(&lexer);
		fprintf(stderr, "Token %d: kind=%d\n", count, tok.kind);
		if (tok.kind == TOK_EOF)
			break;
		count++;
	}
	fprintf(stderr, "Lex complete: %d tokens\n", count);
	lexer_free(&lexer);
	return 0;
}
