#ifndef LEXER_H
#define LEXER_H

typedef enum {
	TOK_EOF,
	TOK_IDENT,
	TOK_POOL,
	TOK_PROC,
	TOK_LPAREN,
	TOK_RPAREN,
	TOK_LBRACE,
	TOK_RBRACE,
	TOK_ERROR
} TokenKind;

typedef struct {
	TokenKind kind;
	const char *start;
	int length;
	int line;
} Token;

/* File I/O */
char *read_file(const char *filename);

#endif
