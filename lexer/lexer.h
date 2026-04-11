#ifndef LEXER_H
#define LEXER_H

#include <stddef.h>

typedef enum TokenKind {
	TOK_EOF,
	TOK_ERROR,

	TOK_IDENT,
	TOK_INT,

	TOK_POOL,
	TOK_PROC,

	TOK_LPAREN,
	TOK_RPAREN,
	TOK_LBRACE,
	TOK_RBRACE,
	TOK_COMMA,
	TOK_SEMI
} TokenKind;

typedef struct Token {
	TokenKind kind;
	const char *start;
	size_t length;
	int line;
} Token;

typedef struct Lexer {
	const char *src;
	const char *cur;
	int line;
} Lexer;

void lexer_init(Lexer *lexer, const char *src);
Token lexer_next_token(Lexer *lexer);
const char *token_kind_name(TokenKind kind);

#endif
