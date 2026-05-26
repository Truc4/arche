/* arche-cst-roundtrip — prove the CST is lossless.
 *
 * Parses a file, walks the CST in source order, and reconstructs the source by
 * emitting each token leaf's bytes plus the inter-leaf gaps (whitespace) taken
 * from the original buffer. If the reconstruction does not equal the input
 * byte-for-byte, the CST has lost information — exit non-zero and report.
 *
 * This is a permanent gate: every stage of the front-end re-architecture must
 * keep `make verify-cst` green.
 */
#include "cst/syntax_tree.h"
#include "parser/parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Append leaves in source order into `out`, filling gaps from `src`. `pos` is the
 * running source offset already emitted. Returns updated pos, or -1 on disorder. */
static long emit(const SyntaxNode *node, const char *src, char *out, long *outlen, long pos) {
	for (int i = 0; i < node->child_count; i++) {
		const SyntaxElem *e = &node->children[i];
		if (e->tag == SE_NODE) {
			pos = emit(e->as.node, src, out, outlen, pos);
			if (pos < 0)
				return -1;
		} else {
			long off = (long)e->as.token.offset;
			long len = (long)e->as.token.length;
			if (off < pos)
				return -1; /* leaves out of source order — structural bug */
			/* gap (whitespace/anything between leaves) */
			memcpy(out + *outlen, src + pos, (size_t)(off - pos));
			*outlen += off - pos;
			/* the token itself */
			memcpy(out + *outlen, src + off, (size_t)len);
			*outlen += len;
			pos = off + len;
		}
	}
	return pos;
}

static int roundtrip(const char *path, const char *src) {
	size_t srclen = strlen(src);
	ParseResult result = parse_source(src);
	int ok = 0;

	if (result.cst_root) {
		char *out = malloc(srclen + 1);
		long outlen = 0;
		long pos = emit(result.cst_root, src, out, &outlen, 0);
		if (pos >= 0) {
			/* trailing bytes after the last leaf (e.g. final newline) */
			memcpy(out + outlen, src + pos, srclen - (size_t)pos);
			outlen += (long)(srclen - (size_t)pos);
			ok = (outlen == (long)srclen) && memcmp(out, src, srclen) == 0;
		}
		if (!ok) {
			/* find first differing byte for a useful report */
			long i = 0;
			while (i < outlen && i < (long)srclen && out[i] == src[i])
				i++;
			fprintf(stderr, "MISMATCH %s: diverges at byte %ld (recon %ld vs src %zu)\n", path, i, outlen, srclen);
		}
		free(out);
	} else {
		fprintf(stderr, "MISMATCH %s: no CST produced\n", path);
	}

	AstProgram *prog = result.ast;
	parse_result_free(&result);
	ast_program_free(prog);
	return ok;
}

static void print_tree(const SyntaxNode *node, const char *src, int depth) {
	for (int d = 0; d < depth; d++)
		fputs("  ", stdout);
	printf("%s\n", syntax_node_kind_name(node->kind));
	for (int i = 0; i < node->child_count; i++) {
		const SyntaxElem *e = &node->children[i];
		if (e->tag == SE_NODE) {
			print_tree(e->as.node, src, depth + 1);
		} else {
			for (int d = 0; d <= depth; d++)
				fputs("  ", stdout);
			printf("'%.*s'\n", (int)e->as.token.length, src + e->as.token.offset);
		}
	}
}

static char *read_file(const char *path) {
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *buf = malloc((size_t)n + 1);
	size_t got = fread(buf, 1, (size_t)n, f);
	buf[got] = '\0';
	fclose(f);
	return buf;
}

int main(int argc, char *argv[]) {
	if (argc < 2) {
		fprintf(stderr, "usage: %s [--tree] <file.arche> [file.arche ...]\n", argv[0]);
		return 2;
	}
	if (strcmp(argv[1], "--tree") == 0) {
		char *src = read_file(argv[2]);
		if (!src)
			return 2;
		ParseResult r = parse_source(src);
		if (r.cst_root)
			print_tree(r.cst_root, src, 0);
		AstProgram *prog = r.ast;
		parse_result_free(&r);
		ast_program_free(prog);
		free(src);
		return 0;
	}
	int failures = 0;
	for (int i = 1; i < argc; i++) {
		char *src = read_file(argv[i]);
		if (!src) {
			fprintf(stderr, "could not read %s\n", argv[i]);
			failures++;
			continue;
		}
		if (!roundtrip(argv[i], src))
			failures++;
		free(src);
	}
	if (failures)
		fprintf(stderr, "round-trip: %d file(s) FAILED\n", failures);
	return failures ? 1 : 0;
}
