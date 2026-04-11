#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lexer.h"

char *read_file(const char *filename);

int main(int argc, char *argv[]) {
	if (argc < 2) {
		fprintf(stderr, "Usage: %s <file>\n", argv[0]);
		return 1;
	}

	char *source = read_file(argv[1]);
	if (!source) {
		fprintf(stderr, "Failed to read file: %s\n", argv[1]);
		return 1;
	}

	// TODO: Tokenize source and print tokens
	// For now, just print file contents as proof of concept
	printf("Lexing: %s\n", argv[1]);
	printf("Source length: %ld bytes\n", strlen(source));

	free(source);
	return 0;
}
