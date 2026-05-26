/* arche-cst-tokens — emit classified syntax tokens for editor highlighting.
 *
 * Parses the input, walks the lossless CST, and prints one line per token leaf:
 *
 *     <offset> <length> <line> <col> <CATEGORY>
 *
 * Non-identifier tokens are classified by token kind. Identifiers are classified
 * by their enclosing CST node (type / property / function / parameter / variable),
 * which is the whole point of building a real CST. This is the single source of
 * truth for syntax highlighting and tracks the language as the front-end evolves.
 *
 * Reads a file argument, or stdin when none is given (the LSP pipes the current,
 * possibly-unsaved buffer this way).
 */
#include "cst/syntax_tree.h"
#include "lexer/lexer.h"
#include "parser/parser.h"
#include <stdio.h>
#include <stdlib.h>

/* Role for an identifier, from the node that directly contains it. */
static const char *ident_role(SyntaxNodeKind parent) {
	switch (parent) {
	case SN_TYPE_REF:
	case SN_TYPE_ARRAY:
	case SN_TYPE_SHAPED_ARRAY:
	case SN_TYPE_TUPLE:
	case SN_TYPE_HANDLE:
	case SN_ALLOC_TYPE:
	case SN_TYPE_DEF_NAME:
		return "type";
	case SN_FUNC_DEF_NAME:
	case SN_CALLEE_NAME:
		return "function";
	case SN_FIELD_NAME:
		return "property";
	case SN_PARAM_NAME:
		return "parameter";
	default:
		return "variable";
	}
}

/* Category for a token leaf, given the kind of the node that contains it. */
static const char *token_category(TokenKind kind, SyntaxNodeKind parent) {
	switch (kind) {
	case TOK_ARCHETYPE:
	case TOK_PROC:
	case TOK_SYS:
	case TOK_FUNC:
	case TOK_LET:
	case TOK_FOR:
	case TOK_IF:
	case TOK_ELSE:
	case TOK_IN:
	case TOK_FREE:
	case TOK_BREAK:
	case TOK_EXTERN:
	case TOK_UNSAFE:
	case TOK_MOVE:
	case TOK_OWN:
	case TOK_COPY:
	case TOK_RETURN:
	case TOK_USE:
	case TOK_EACH_FIELD:
		return "keyword";

	case TOK_NUMBER:
		return "number";
	case TOK_STRING:
	case TOK_CHAR_LIT:
		return "string";
	case TOK_COMMENT:
		return "comment";

	case TOK_IDENT:
		return ident_role(parent);

	case TOK_EQ:
	case TOK_PLUS_EQ:
	case TOK_MINUS_EQ:
	case TOK_STAR_EQ:
	case TOK_SLASH_EQ:
	case TOK_PLUS:
	case TOK_MINUS:
	case TOK_STAR:
	case TOK_SLASH:
	case TOK_EQ_EQ:
	case TOK_BANG_EQ:
	case TOK_LT:
	case TOK_GT:
	case TOK_LT_EQ:
	case TOK_GT_EQ:
	case TOK_ARROW:
	case TOK_BANG:
	case TOK_AT:
		return "operator";

	case TOK_LPAREN:
	case TOK_RPAREN:
	case TOK_LBRACE:
	case TOK_RBRACE:
	case TOK_LBRACKET:
	case TOK_RBRACKET:
	case TOK_COMMA:
	case TOK_DOT:
	case TOK_COLON:
	case TOK_SEMI:
		return "punctuation";

	default:
		return NULL; /* EOF / ERROR / anything unmapped: skip */
	}
}

static void walk(const SyntaxNode *node) {
	for (int i = 0; i < node->child_count; i++) {
		const SyntaxElem *e = &node->children[i];
		if (e->tag == SE_NODE) {
			walk(e->as.node);
		} else {
			const char *cat = token_category(e->as.token.kind, node->kind);
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
			fprintf(stderr, "arche-cst-tokens: could not open '%s'\n", argv[1]);
			return 1;
		}
		src = read_all(file);
		fclose(file);
	} else {
		src = read_all(stdin);
	}
	if (!src) {
		fprintf(stderr, "arche-cst-tokens: out of memory reading input\n");
		return 1;
	}

	ParseResult result = parse_source(src);
	if (result.cst_root) {
		walk(result.cst_root);
	}

	AstProgram *prog = result.ast;
	parse_result_free(&result);
	ast_program_free(prog);
	free(src);
	return 0;
}
