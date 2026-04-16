#include "ast/ast.h"
#include "lexer/lexer.h"
#include "parser/parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char *argv[]) {
	if (argc < 2) {
		fprintf(stderr, "Usage: %s <file.arche>\n", argv[0]);
		return 1;
	}

	const char *filename = argv[1];

	/* Check file extension */
	size_t len = strlen(filename);
	if (len < 6 || strcmp(filename + len - 6, ".arche") != 0) {
		fprintf(stderr, "Error: only .arche files supported, got '%s'\n", filename);
		return 1;
	}

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
	ParseResult parse_result = parse_source(src);

	if (parse_result.error_count > 0) {
		for (size_t i = 0; i < parse_result.error_count; i++) {
			fprintf(stderr, "[Line %d, Col %d] Error: %s\n", parse_result.errors[i].line, parse_result.errors[i].column,
			        parse_result.errors[i].message);
		}
		Program *prog = parse_result.ast;
		parse_result_free(&parse_result);
		program_free(prog);
		free(src);
		return 1;
	}

	Program *prog = parse_result.ast;
	parse_result_free(&parse_result);

	/* format and print */
	format_program(stdout, prog);

	/* cleanup */
	program_free(prog);
	free(src);

	return 0;
}
