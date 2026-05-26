#ifndef PARSER_H
#define PARSER_H

#include "../cst/cst.h"
#include "../cst/syntax_tree.h"
#include "../lexer/lexer.h"
#include <stddef.h>

/* A single parse error */
typedef struct {
	char *message;
	int line;
	int column;
} ParseError;

/* Result of parsing */
typedef struct {
	AstProgram *ast;
	ParseError *errors;
	size_t error_count;
	Token *comments;
	size_t comment_count;
	/* Lossless concrete syntax tree covering the whole source. Owned by the
	 * result; freed by parse_result_free. NULL only if parsing wasn't run. */
	SyntaxNode *cst_root;
} ParseResult;

/* Opaque parser state */
typedef struct Parser Parser;

Parser *parser_create(Lexer *lexer);
void parser_free(Parser *parser);
ParseResult parse_program(Parser *parser);
ParseResult parse_source(const char *src);
void parse_result_free(ParseResult *result);

#endif /* PARSER_H */
