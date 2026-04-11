#include <ctype.h>
#include <string.h>

#include "lexer.h"

static int is_at_end(Lexer *lexer) { return *lexer->cur == '\0'; }

static char peek(Lexer *lexer) { return *lexer->cur; }

static char peek_next(Lexer *lexer) {
	if (is_at_end(lexer) || lexer->cur[1] == '\0') {
		return '\0';
	}
	return lexer->cur[1];
}

static char advance(Lexer *lexer) {
	char c = *lexer->cur;
	if (c != '\0') {
		lexer->cur++;
		if (c == '\n') {
			lexer->line++;
			lexer->column = 1;
		} else {
			lexer->column++;
		}
	}
	return c;
}

static Token make_token(Lexer *lexer, TokenKind kind, const char *start,
						size_t length, int line, int column) {
	Token tok;
	tok.kind = kind;
	tok.start = start;
	tok.length = length;
	tok.line = line;
	tok.column = column;
	return tok;
}

static Token error_token(Lexer *lexer, const char *message, int line,
						 int column) {
	Token tok;
	tok.kind = TOK_ERROR;
	tok.start = message;
	tok.length = strlen(message);
	tok.line = line;
	tok.column = column;
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
	if (length == 5 && strncmp(start, "arche", 5) == 0) {
		return TOK_ARCHETYPE;
	}
	if (length == 4 && strncmp(start, "proc", 4) == 0) {
		return TOK_PROC;
	}
	if (length == 3 && strncmp(start, "sys", 3) == 0) {
		return TOK_SYS;
	}
	if (length == 4 && strncmp(start, "func", 4) == 0) {
		return TOK_FUNC;
	}
	if (length == 4 && strncmp(start, "meta", 4) == 0) {
		return TOK_META;
	}
	if (length == 3 && strncmp(start, "col", 3) == 0) {
		return TOK_COL;
	}
	if (length == 3 && strncmp(start, "let", 3) == 0) {
		return TOK_LET;
	}
	if (length == 3 && strncmp(start, "for", 3) == 0) {
		return TOK_FOR;
	}
	if (length == 2 && strncmp(start, "in", 2) == 0) {
		return TOK_IN;
	}
	if (length == 4 && strncmp(start, "free", 4) == 0) {
		return TOK_FREE;
	}
	return TOK_IDENT;
}

static Token lex_identifier(Lexer *lexer) {
	const char *start = lexer->cur;
	int line = lexer->line;
	int column = lexer->column;

	while (is_ident_char(peek(lexer))) {
		advance(lexer);
	}

	size_t length = (size_t)(lexer->cur - start);
	TokenKind kind = keyword_kind(start, length);
	return make_token(lexer, kind, start, length, line, column);
}

static Token lex_number(Lexer *lexer) {
	const char *start = lexer->cur;
	int line = lexer->line;
	int column = lexer->column;

	while (isdigit((unsigned char)peek(lexer))) {
		advance(lexer);
	}

	/* fractional part: digits '.' digits */
	if (peek(lexer) == '.' && isdigit((unsigned char)peek_next(lexer))) {
		advance(lexer); /* consume '.' */

		while (isdigit((unsigned char)peek(lexer))) {
			advance(lexer);
		}
	}

	return make_token(lexer, TOK_NUMBER, start, (size_t)(lexer->cur - start),
					  line, column);
}

void lexer_init(Lexer *lexer, const char *src) {
	lexer->src = src;
	lexer->cur = src;
	lexer->line = 1;
	lexer->column = 1;
}

Token lexer_next_token(Lexer *lexer) {
	skip_whitespace(lexer);

	const char *start = lexer->cur;
	int line = lexer->line;
	int column = lexer->column;

	if (is_at_end(lexer)) {
		return make_token(lexer, TOK_EOF, start, 0, line, column);
	}

	char c = advance(lexer);

	if (is_ident_start(c)) {
		lexer->cur--;
		lexer->column--;
		return lex_identifier(lexer);
	}

	if (isdigit((unsigned char)c)) {
		lexer->cur--;
		lexer->column--;
		return lex_number(lexer);
	}

	switch (c) {
	case '(':
		return make_token(lexer, TOK_LPAREN, start, 1, line, column);
	case ')':
		return make_token(lexer, TOK_RPAREN, start, 1, line, column);
	case '{':
		return make_token(lexer, TOK_LBRACE, start, 1, line, column);
	case '}':
		return make_token(lexer, TOK_RBRACE, start, 1, line, column);
	case '[':
		return make_token(lexer, TOK_LBRACKET, start, 1, line, column);
	case ']':
		return make_token(lexer, TOK_RBRACKET, start, 1, line, column);
	case ',':
		return make_token(lexer, TOK_COMMA, start, 1, line, column);
	case '.':
		return make_token(lexer, TOK_DOT, start, 1, line, column);
	case ':':
		return make_token(lexer, TOK_COLON, start, 1, line, column);
	case ';':
		return make_token(lexer, TOK_SEMI, start, 1, line, column);

	case '+':
		if (peek(lexer) == '=') {
			advance(lexer);
			return make_token(lexer, TOK_PLUS_EQ, start, 2, line, column);
		}
		return make_token(lexer, TOK_PLUS, start, 1, line, column);

	case '-':
		if (peek(lexer) == '=') {
			advance(lexer);
			return make_token(lexer, TOK_MINUS_EQ, start, 2, line, column);
		}
		return make_token(lexer, TOK_MINUS, start, 1, line, column);

	case '*':
		if (peek(lexer) == '=') {
			advance(lexer);
			return make_token(lexer, TOK_STAR_EQ, start, 2, line, column);
		}
		return make_token(lexer, TOK_STAR, start, 1, line, column);

	case '/':
		if (peek(lexer) == '=') {
			advance(lexer);
			return make_token(lexer, TOK_SLASH_EQ, start, 2, line, column);
		}
		return make_token(lexer, TOK_SLASH, start, 1, line, column);

	case '=':
		if (peek(lexer) == '=') {
			advance(lexer);
			return make_token(lexer, TOK_EQ_EQ, start, 2, line, column);
		}
		return make_token(lexer, TOK_EQ, start, 1, line, column);

	case '!':
		if (peek(lexer) == '=') {
			advance(lexer);
			return make_token(lexer, TOK_BANG_EQ, start, 2, line, column);
		}
		return make_token(lexer, TOK_BANG, start, 1, line, column);

	case '<':
		if (peek(lexer) == '=') {
			advance(lexer);
			return make_token(lexer, TOK_LT_EQ, start, 2, line, column);
		}
		return make_token(lexer, TOK_LT, start, 1, line, column);

	case '>':
		if (peek(lexer) == '=') {
			advance(lexer);
			return make_token(lexer, TOK_GT_EQ, start, 2, line, column);
		}
		return make_token(lexer, TOK_GT, start, 1, line, column);

	default:
		return error_token(lexer, "unexpected character", line, column);
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
	case TOK_NUMBER:
		return "TOK_NUMBER";

	case TOK_ARCHETYPE:
		return "TOK_ARCHETYPE";
	case TOK_PROC:
		return "TOK_PROC";
	case TOK_SYS:
		return "TOK_SYS";
	case TOK_FUNC:
		return "TOK_FUNC";
	case TOK_META:
		return "TOK_META";
	case TOK_COL:
		return "TOK_COL";
	case TOK_LET:
		return "TOK_LET";
	case TOK_FOR:
		return "TOK_FOR";
	case TOK_IN:
		return "TOK_IN";
	case TOK_FREE:
		return "TOK_FREE";

	case TOK_LPAREN:
		return "TOK_LPAREN";
	case TOK_RPAREN:
		return "TOK_RPAREN";
	case TOK_LBRACE:
		return "TOK_LBRACE";
	case TOK_RBRACE:
		return "TOK_RBRACE";
	case TOK_LBRACKET:
		return "TOK_LBRACKET";
	case TOK_RBRACKET:
		return "TOK_RBRACKET";
	case TOK_COMMA:
		return "TOK_COMMA";
	case TOK_DOT:
		return "TOK_DOT";
	case TOK_COLON:
		return "TOK_COLON";
	case TOK_SEMI:
		return "TOK_SEMI";

	case TOK_EQ:
		return "TOK_EQ";
	case TOK_PLUS_EQ:
		return "TOK_PLUS_EQ";
	case TOK_MINUS_EQ:
		return "TOK_MINUS_EQ";
	case TOK_STAR_EQ:
		return "TOK_STAR_EQ";
	case TOK_SLASH_EQ:
		return "TOK_SLASH_EQ";

	case TOK_PLUS:
		return "TOK_PLUS";
	case TOK_MINUS:
		return "TOK_MINUS";
	case TOK_STAR:
		return "TOK_STAR";
	case TOK_SLASH:
		return "TOK_SLASH";

	case TOK_EQ_EQ:
		return "TOK_EQ_EQ";
	case TOK_BANG_EQ:
		return "TOK_BANG_EQ";
	case TOK_LT:
		return "TOK_LT";
	case TOK_GT:
		return "TOK_GT";
	case TOK_LT_EQ:
		return "TOK_LT_EQ";
	case TOK_GT_EQ:
		return "TOK_GT_EQ";

	case TOK_BANG:
		return "TOK_BANG";

	default:
		return "TOK_UNKNOWN";
	}
}
