#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lexer/lexer.h"
#include "parser/parser.h"
#include "ast/ast.h"

int main(int argc, char *argv[]) {
	if (argc < 2) {
		fprintf(stderr, "Usage: %s <file.arche>\n", argv[0]);
		return 1;
	}

	const char *filename = argv[1];
	FILE *file = fopen(filename, "r");
	if (!file) {
		fprintf(stderr, "Error: could not open file '%s'\n", filename);
		return 1;
	}

	/* read entire file */
	fseek(file, 0, SEEK_END);
	long file_size = ftell(file);
	fseek(file, 0, SEEK_SET);

	char *src = malloc(file_size + 1);
	size_t bytes_read = fread(src, 1, file_size, file);
	src[bytes_read] = '\0';
	fclose(file);

	/* lex and parse */
	Lexer lexer;
	lexer_init(&lexer, src);

	Parser parser;
	parser_init(&parser, &lexer);

	Program *prog = parse_program(&parser);

	if (parser.had_error) {
		fprintf(stderr, "Parse error\n");
		program_free(prog);
		free(src);
		return 1;
	}

	/* format and print */
	format_program(stdout, prog);

	/* cleanup */
	program_free(prog);
	free(src);

	return 0;
}
