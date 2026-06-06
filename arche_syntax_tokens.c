/* arche-syntax-tokens — emit classified syntax tokens for editor highlighting.
 *
 * Parses the input, walks the lossless syntax tree, and prints one line per token leaf:
 *
 *     <offset> <length> <line> <col> <CATEGORY>
 *
 * Non-identifier tokens are classified by token kind. Identifiers are classified
 * by their enclosing syntax tree node (type / property / function / parameter / variable),
 * which is the whole point of building a real syntax tree. This is the single source of
 * truth for syntax highlighting and tracks the language as the front-end evolves.
 *
 * Reads a file argument, or stdin when none is given (the LSP pipes the current,
 * possibly-unsaved buffer this way).
 */
#include "lexer/lexer.h"
#include "parser/parser.h"
#include "syntax/syntax_tree.h"
#include "syntax/token_category.h"
#include <stdio.h>
#include <stdlib.h>

static void walk(const SyntaxNode *node) {
	for (int i = 0; i < node->child_count; i++) {
		const SyntaxElem *e = &node->children[i];
		if (e->tag == SE_NODE) {
			walk(e->as.node);
		} else {
			const char *cat = arche_token_category(e->as.token.kind, node->kind);
			if (cat) {
				printf("%u %u %d %d %s\n", e->as.token.offset, e->as.token.length, e->as.token.line, e->as.token.column,
				       cat);
			}
		}
	}
}

static char *read_all(FILE *f) {
	size_t cap = 4096, len = 0;
	char *buf = malloc(cap);
	if (!buf)
		return NULL;
	size_t n;
	while ((n = fread(buf + len, 1, cap - len, f)) > 0) {
		len += n;
		if (len == cap) {
			cap *= 2;
			char *grown = realloc(buf, cap);
			if (!grown) {
				free(buf);
				return NULL;
			}
			buf = grown;
		}
	}
	buf[len] = '\0';
	return buf;
}

int main(int argc, char *argv[]) {
	char *src;
	if (argc >= 2) {
		FILE *file = fopen(argv[1], "r");
		if (!file) {
			fprintf(stderr, "arche-syntax-tokens: could not open '%s'\n", argv[1]);
			return 1;
		}
		src = read_all(file);
		fclose(file);
	} else {
		src = read_all(stdin);
	}
	if (!src) {
		fprintf(stderr, "arche-syntax-tokens: out of memory reading input\n");
		return 1;
	}

	ParseResult result = parse_source(src);
	if (result.syntax_root) {
		walk(result.syntax_root);
	}

	parse_result_free(&result);
	free(src);
	return 0;
}
