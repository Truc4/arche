#include <stdio.h>
#include <stdlib.h>

#include "lexer.h"

char *read_file(const char *filename) {
	FILE *f = fopen(filename, "r");
	if (!f)
		return NULL;

	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);

	char *buf = malloc(size + 1);
	if (!buf) {
		fclose(f);
		return NULL;
	}

	fread(buf, 1, size, f);
	buf[size] = '\0';
	fclose(f);

	return buf;
}

static void print_token(Token tok) {
	printf("line %-3d %-12s", tok.line, token_kind_name(tok.kind));

	if (tok.kind == TOK_EOF) {
		printf("\n");
		return;
	}

	printf(" '");
	for (size_t i = 0; i < tok.length; i++) {
		putchar(tok.start[i]);
	}
	printf("'\n");
}

int main(int argc, char **argv) {
	if (argc < 2) {
		fprintf(stderr, "usage: %s <file>\n", argv[0]);
		return 1;
	}

	char *src = read_file(argv[1]);
	if (!src) {
		fprintf(stderr, "failed to read file: %s\n", argv[1]);
		return 1;
	}

	Lexer lexer;
	lexer_init(&lexer, src);

	for (;;) {
		Token tok = lexer_next_token(&lexer);
		print_token(tok);

		if (tok.kind == TOK_ERROR || tok.kind == TOK_EOF) {
			break;
		}
	}

	free(src);
	return 0;
}
