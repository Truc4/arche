#ifndef LEXER_H
#define LEXER_H

#include <stddef.h>

typedef enum TokenKind {
	TOK_EOF,
	TOK_ERROR,

	/* identifiers / literals */
	TOK_IDENT,
	TOK_NUMBER,
	TOK_STRING,
	TOK_COMMENT,

	/* keywords */
	TOK_ARCHETYPE,
	TOK_PROC,
	TOK_SYS,
	TOK_FUNC,
	TOK_META,
	TOK_COL,
	TOK_LET,
	TOK_FOR,
	TOK_IN,
	TOK_FREE,
	TOK_EXTERN,

	/* punctuation */
	TOK_LPAREN,   /* ( */
	TOK_RPAREN,   /* ) */
	TOK_LBRACE,   /* { */
	TOK_RBRACE,   /* } */
	TOK_LBRACKET, /* [ */
	TOK_RBRACKET, /* ] */
	TOK_COMMA,    /* , */
	TOK_DOT,      /* . */
	TOK_COLON,    /* : */
	TOK_SEMI,     /* ; */

	/* assignment */
	TOK_EQ,       /* = */
	TOK_PLUS_EQ,  /* += */
	TOK_MINUS_EQ, /* -= */
	TOK_STAR_EQ,  /* *= */
	TOK_SLASH_EQ, /* /= */

	/* arithmetic */
	TOK_PLUS,  /* + */
	TOK_MINUS, /* - */
	TOK_STAR,  /* * */
	TOK_SLASH, /* / */

	/* comparisons */
	TOK_EQ_EQ,   /* == */
	TOK_BANG_EQ, /* != */
	TOK_LT,      /* < */
	TOK_GT,      /* > */
	TOK_LT_EQ,   /* <= */
	TOK_GT_EQ,   /* >= */

	/* unary */
	TOK_BANG /* ! */
} TokenKind;

typedef struct Token {
	TokenKind kind;
	const char *start;
	size_t length;
	int line;
	int column;
} Token;

typedef struct Lexer {
	const char *src;
	const char *cur;
	int line;
	int column;
} Lexer;

void lexer_init(Lexer *lexer, const char *src);
Token lexer_next_token(Lexer *lexer);
const char *token_kind_name(TokenKind kind);

#endif
