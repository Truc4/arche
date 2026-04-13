#ifndef PARSER_H
#define PARSER_H

#include "../ast/ast.h"
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
	Program *ast;
	ParseError *errors;
	size_t error_count;
	Token *comments;
	size_t comment_count;
} ParseResult;

/* Opaque parser state */
typedef struct Parser Parser;

Parser *parser_create(Lexer *lexer);
void parser_free(Parser *parser);
ParseResult parse_program(Parser *parser);
ParseResult parse_source(const char *src);
void parse_result_free(ParseResult *result);

#endif /* PARSER_H */
