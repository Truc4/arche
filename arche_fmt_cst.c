/* arche-fmt-cst — format a .arche file by walking the lossless CST.
 *
 * The CST-driven counterpart to arche-fmt (which re-synthesizes from the
 * abstract Program tree). Built as a separate tool so the existing formatter and
 * the corpus are untouched until this is validated; eventually it replaces
 * format_program. Reads a file argument or stdin. */
#include "cst/format_cst.h"
#include "cst/syntax_tree.h"
#include "parser/parser.h"
#include <stdio.h>
#include <stdlib.h>

static char *read_all(FILE *f) {
	size_t cap = 4096, len = 0;
	char *buf = malloc(cap);
	size_t n;
	while ((n = fread(buf + len, 1, cap - len, f)) > 0) {
		len += n;
		if (len == cap) {
			cap *= 2;
			buf = realloc(buf, cap);
		}
	}
	buf[len] = '\0';
	return buf;
}

int main(int argc, char *argv[]) {
	char *src;
	if (argc >= 2) {
		FILE *f = fopen(argv[1], "r");
		if (!f) {
			fprintf(stderr, "arche-fmt-cst: could not open '%s'\n", argv[1]);
			return 1;
		}
		src = read_all(f);
		fclose(f);
	} else {
		src = read_all(stdin);
	}

	ParseResult r = parse_source(src);
	if (r.cst_root)
		format_cst(stdout, r.cst_root, src);

	Program *prog = r.ast;
	parse_result_free(&r);
	program_free(prog);
	free(src);
	return 0;
}
