#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lexer.h"

char *read_file(const char *filename) {
	FILE *f = fopen(filename, "r");
	if (!f)
		return NULL;

	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);

	char *buf = malloc(size + 1);
	fread(buf, 1, size, f);
	buf[size] = '\0';
	fclose(f);

	return buf;
}
