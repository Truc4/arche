#include "cst/cst.h"
#include "cst/format_cst.h"
#include "lexer/lexer.h"
#include "parser/parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>

int main(int argc, char *argv[]) {
	/* Limit memory to 512MB to prevent runaway parsing */
	struct rlimit mem_limit;
	mem_limit.rlim_cur = 512 * 1024 * 1024;
	mem_limit.rlim_max = 512 * 1024 * 1024;
	int limit_result = setrlimit(RLIMIT_AS, &mem_limit);
	if (limit_result != 0) {
		perror("Error: Could not set memory limit");
		return 1;
	}
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

	/* Lex + parse. The formatter is driven entirely by the lossless CST, so it preserves
	 * comments and needs no abstract-AST reconstruction. */
	ParseResult parse_result = parse_source(src);

	if (parse_result.error_count > 0) {
		fprintf(stderr, "%s:\n", filename);
		int unique_errors = 1;
		fprintf(stderr, "  [Line %d, Col %d] Error: %s\n", parse_result.errors[0].line, parse_result.errors[0].column,
		        parse_result.errors[0].message);
		for (size_t i = 1; i < parse_result.error_count && unique_errors < 5; i++) {
			if (parse_result.errors[i].line != parse_result.errors[i - 1].line ||
			    parse_result.errors[i].column != parse_result.errors[i - 1].column) {
				fprintf(stderr, "  [Line %d, Col %d] Error: %s\n", parse_result.errors[i].line,
				        parse_result.errors[i].column, parse_result.errors[i].message);
				unique_errors++;
			}
		}
		if (parse_result.error_count > unique_errors) {
			fprintf(stderr, "  ... and %zu more errors\n", parse_result.error_count - unique_errors);
		}
		parse_result_free(&parse_result);
		free(src);
		return 1;
	}
	/* Format directly from the lossless CST (comment- and structure-preserving). */
	format_cst(stdout, parse_result.cst_root, src);

	parse_result_free(&parse_result);
	free(src);

	return 0;
}
