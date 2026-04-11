#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "lexer.h"

static int is_at_end(Lexer *lexer) { return *lexer->cur == '\0'; }

static char peek(Lexer *lexer) { return *lexer->cur; }

static char peek_next(Lexer *lexer) {
	if (is_at_end(lexer)) {
		return '\0';
	}
	return lexer->cur[1];
}

static char advance(Lexer *lexer) {
	char c = *lexer->cur;
	if (c != '\0') {
		lexer->cur++;
	}
	return c;
}

static Token make_token(Lexer *lexer, TokenKind kind, const char *start,
						size_t length) {
	Token tok;
	tok.kind = kind;
	tok.start = start;
	tok.length = length;
	tok.line = lexer->line;
	return tok;
}

static Token error_token(Lexer *lexer, const char *message) {
	Token tok;
	tok.kind = TOK_ERROR;
	tok.start = message;
	tok.length = strlen(message);
	tok.line = lexer->line;
	return tok;
}

static void skip_whitespace(Lexer *lexer) {
	for (;;) {
		char c = peek(lexer);

		switch (c) {
		case ' ':
		case '\r':
		case '\t':
			advance(lexer);
			break;

		case '\n':
			lexer->line++;
			advance(lexer);
			break;

		case '/':
			if (peek_next(lexer) == '/') {
				while (peek(lexer) != '\n' && !is_at_end(lexer)) {
					advance(lexer);
				}
			} else {
				return;
			}
			break;

		default:
			return;
		}
	}
}

static int is_ident_start(char c) {
	return isalpha((unsigned char)c) || c == '_';
}

static int is_ident_char(char c) {
	return isalnum((unsigned char)c) || c == '_';
}

static TokenKind keyword_kind(const char *start, size_t length) {
	if (length == 4 && strncmp(start, "pool", 4) == 0) {
		return TOK_POOL;
	}
	if (length == 4 && strncmp(start, "proc", 4) == 0) {
		return TOK_PROC;
	}
	return TOK_IDENT;
}

static Token lex_identifier(Lexer *lexer) {
	const char *start = lexer->cur;

	while (is_ident_char(peek(lexer))) {
		advance(lexer);
	}

	size_t length = (size_t)(lexer->cur - start);
	TokenKind kind = keyword_kind(start, length);
	return make_token(lexer, kind, start, length);
}

static Token lex_number(Lexer *lexer) {
	const char *start = lexer->cur;

	while (isdigit((unsigned char)peek(lexer))) {
		advance(lexer);
	}

	return make_token(lexer, TOK_INT, start, (size_t)(lexer->cur - start));
}

void lexer_init(Lexer *lexer, const char *src) {
	lexer->src = src;
	lexer->cur = src;
	lexer->line = 1;
}

Token lexer_next_token(Lexer *lexer) {
	skip_whitespace(lexer);

	const char *start = lexer->cur;

	if (is_at_end(lexer)) {
		return make_token(lexer, TOK_EOF, start, 0);
	}

	char c = advance(lexer);

	if (is_ident_start(c)) {
		lexer->cur--;
		return lex_identifier(lexer);
	}

	if (isdigit((unsigned char)c)) {
		lexer->cur--;
		return lex_number(lexer);
	}

	switch (c) {
	case '(':
		return make_token(lexer, TOK_LPAREN, start, 1);
	case ')':
		return make_token(lexer, TOK_RPAREN, start, 1);
	case '{':
		return make_token(lexer, TOK_LBRACE, start, 1);
	case '}':
		return make_token(lexer, TOK_RBRACE, start, 1);
	case ',':
		return make_token(lexer, TOK_COMMA, start, 1);
	case ';':
		return make_token(lexer, TOK_SEMI, start, 1);
	default:
		return error_token(lexer, "unexpected character");
	}
}

const char *token_kind_name(TokenKind kind) {
	switch (kind) {
	case TOK_EOF:
		return "TOK_EOF";
	case TOK_ERROR:
		return "TOK_ERROR";
	case TOK_IDENT:
		return "TOK_IDENT";
	case TOK_INT:
		return "TOK_INT";
	case TOK_POOL:
		return "TOK_POOL";
	case TOK_PROC:
		return "TOK_PROC";
	case TOK_LPAREN:
		return "TOK_LPAREN";
	case TOK_RPAREN:
		return "TOK_RPAREN";
	case TOK_LBRACE:
		return "TOK_LBRACE";
	case TOK_RBRACE:
		return "TOK_RBRACE";
	case TOK_COMMA:
		return "TOK_COMMA";
	case TOK_SEMI:
		return "TOK_SEMI";
	default:
		return "TOK_UNKNOWN";
	}
}
