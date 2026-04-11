#ifndef PARSER_H
#define PARSER_H

#include "../lexer/lexer.h"
#include "../ast/ast.h"

typedef struct {
	Lexer *lexer;
	Token current;
	Token previous;
	int had_error;
	int panic_mode;
} Parser;

void parser_init(Parser *parser, Lexer *lexer);

Program *parse_program(Parser *parser);

#endif /* PARSER_H */
