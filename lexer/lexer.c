#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lexer.h"

static int is_at_end(Lexer *lexer) {
	return *lexer->cur == '\0';
}

static char peek(Lexer *lexer) {
	return *lexer->cur;
}

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

static Token make_token(Lexer *lexer, TokenKind kind, const char *start, size_t length, int line, int column) {
	Token tok;
	tok.kind = kind;
	tok.start = start;
	tok.length = length;
	tok.line = line;
	tok.column = column;
	return tok;
}

static Token error_token(Lexer *lexer, const char *message, int line, int column) {
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
		case '\n':
			advance(lexer);
			break;

		default:
			return;
		}
	}
}

static Token lex_comment(Lexer *lexer) {
	const char *start = lexer->cur;
	int line = lexer->line;
	int column = lexer->column;

	advance(lexer); /* consume first '/' */
	advance(lexer); /* consume second '/' */

	while (peek(lexer) != '\n' && !is_at_end(lexer)) {
		advance(lexer);
	}

	return make_token(lexer, TOK_COMMENT, start, (size_t)(lexer->cur - start), line, column);
}

static Token lex_block_comment(Lexer *lexer) {
	const char *start = lexer->cur;
	int line = lexer->line;
	int column = lexer->column;

	advance(lexer); /* consume '/' */
	advance(lexer); /* consume '*' */

	while (!is_at_end(lexer)) {
		if (peek(lexer) == '*' && peek_next(lexer) == '/') {
			advance(lexer); /* consume '*' */
			advance(lexer); /* consume '/' */
			break;
		}
		advance(lexer);
	}

	return make_token(lexer, TOK_COMMENT, start, (size_t)(lexer->cur - start), line, column);
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
	if (length == 3 && strncmp(start, "let", 3) == 0) {
		return TOK_LET;
	}
	if (length == 3 && strncmp(start, "for", 3) == 0) {
		return TOK_FOR;
	}
	if (length == 2 && strncmp(start, "if", 2) == 0) {
		return TOK_IF;
	}
	if (length == 2 && strncmp(start, "in", 2) == 0) {
		return TOK_IN;
	}
	if (length == 4 && strncmp(start, "free", 4) == 0) {
		return TOK_FREE;
	}
	if (length == 5 && strncmp(start, "break", 5) == 0) {
		return TOK_BREAK;
	}
	if (length == 6 && strncmp(start, "extern", 6) == 0) {
		return TOK_EXTERN;
	}
	if (length == 3 && strncmp(start, "out", 3) == 0) {
		return TOK_OUT;
	}
	if (length == 6 && strncmp(start, "return", 6) == 0) {
		return TOK_RETURN;
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

	return make_token(lexer, TOK_NUMBER, start, (size_t)(lexer->cur - start), line, column);
}

static Token lex_char_lit(Lexer *lexer) {
	const char *start = lexer->cur;
	int line = lexer->line;
	int column = lexer->column;

	advance(lexer); /* consume opening '\'' */

	int char_value = 0;
	if (peek(lexer) == '\\') {
		advance(lexer); /* consume backslash */
		if (!is_at_end(lexer)) {
			char escaped = peek(lexer);
			switch (escaped) {
			case 'n':
				char_value = '\n';
				break;
			case 't':
				char_value = '\t';
				break;
			case 'r':
				char_value = '\r';
				break;
			case '\\':
				char_value = '\\';
				break;
			case '\'':
				char_value = '\'';
				break;
			default:
				char_value = escaped;
				break;
			}
			advance(lexer);
		}
	} else if (peek(lexer) != '\'' && !is_at_end(lexer)) {
		char_value = peek(lexer);
		advance(lexer);
	} else {
		return error_token(lexer, "Empty character literal", line, column);
	}

	if (peek(lexer) != '\'') {
		return error_token(lexer, "Unterminated character literal", line, column);
	}

	advance(lexer); /* consume closing '\'' */

	Token token = make_token(lexer, TOK_CHAR_LIT, start, lexer->cur - start, line, column);
	token.int_val = char_value;
	return token;
}

static Token lex_string(Lexer *lexer) {
	const char *start = lexer->cur;
	int line = lexer->line;
	int column = lexer->column;

	advance(lexer); /* consume opening '"' */

	/* Build character array for the string */
	char chars[2048];
	int char_count = 0;

	while (peek(lexer) != '"' && !is_at_end(lexer)) {
		if (peek(lexer) == '\\') {
			advance(lexer); /* consume backslash */
			if (!is_at_end(lexer)) {
				char escaped = peek(lexer);
				switch (escaped) {
				case 'n':
					chars[char_count++] = '\n';
					break;
				case 't':
					chars[char_count++] = '\t';
					break;
				case 'r':
					chars[char_count++] = '\r';
					break;
				case '\\':
					chars[char_count++] = '\\';
					break;
				case '"':
					chars[char_count++] = '"';
					break;
				default:
					chars[char_count++] = escaped;
					break;
				}
				advance(lexer);
			}
		} else {
			chars[char_count++] = peek(lexer);
			advance(lexer);
		}
	}

	if (is_at_end(lexer)) {
		return error_token(lexer, "Unterminated string", line, column);
	}

	advance(lexer); /* consume closing '"' */

	/* Generate array literal tokens: {ascii1, ascii2, ...} */
	int token_count = 1 + char_count * 2; /* LBRACE + (NUM + COMMA)*n + RBRACE - 1 comma */
	if (char_count > 0) {
		token_count--; /* no trailing comma */
	}
	token_count++; /* RBRACE */

	Token *tokens = malloc(token_count * sizeof(Token));
	int tok_idx = 0;

	/* LBRACE */
	tokens[tok_idx++] = make_token(lexer, TOK_LBRACE, "{", 1, line, column);

	/* Each character as ASCII number */
	for (int i = 0; i < char_count; i++) {
		char ascii_buf[16];
		snprintf(ascii_buf, sizeof(ascii_buf), "%d", (unsigned char)chars[i]);
		size_t ascii_len = strlen(ascii_buf);

		/* Ensure string buffer has space */
		if (lexer->string_buf_pos + ascii_len + 1 >= lexer->string_buf_size) {
			lexer->string_buf_size *= 2;
			lexer->string_buf = realloc(lexer->string_buf, lexer->string_buf_size);
		}

		/* Copy ASCII string to buffer */
		const char *num_start = lexer->string_buf + lexer->string_buf_pos;
		strcpy((char *)num_start, ascii_buf);
		lexer->string_buf_pos += ascii_len + 1;

		/* Number token */
		tokens[tok_idx].kind = TOK_NUMBER;
		tokens[tok_idx].start = num_start;
		tokens[tok_idx].length = ascii_len;
		tokens[tok_idx].line = line;
		tokens[tok_idx].column = column;
		tok_idx++;

		/* Comma (except after last element) */
		if (i < char_count - 1) {
			tokens[tok_idx++] = make_token(lexer, TOK_COMMA, ",", 1, line, column);
		}
	}

	/* RBRACE */
	tokens[tok_idx++] = make_token(lexer, TOK_RBRACE, "}", 1, line, column);

	/* Store in lexer pending queue, skip first token which we're returning */
	lexer->pending_tokens = tokens;
	lexer->pending_count = token_count;
	lexer->pending_pos = 1; /* Start at index 1 since we're returning index 0 */

	/* Return first token (LBRACE) */
	return tokens[0];
}

void lexer_init(Lexer *lexer, const char *src) {
	lexer->src = src;
	lexer->cur = src;
	lexer->line = 1;
	lexer->column = 1;
	lexer->pending_tokens = NULL;
	lexer->pending_count = 0;
	lexer->pending_pos = 0;
	lexer->string_buf_size = 4096;
	lexer->string_buf = malloc(lexer->string_buf_size);
	lexer->string_buf_pos = 0;
}

void lexer_free(Lexer *lexer) {
	if (lexer) {
		free(lexer->pending_tokens);
		free(lexer->string_buf);
		lexer->pending_tokens = NULL;
		lexer->string_buf = NULL;
	}
}

Token lexer_next_token(Lexer *lexer) {
	/* Check if we have pending tokens from string expansion */
	if (lexer->pending_tokens && lexer->pending_pos < lexer->pending_count) {
		Token tok = lexer->pending_tokens[lexer->pending_pos++];
		/* Clean up pending tokens array once we've returned them all */
		if (lexer->pending_pos >= lexer->pending_count) {
			free(lexer->pending_tokens);
			lexer->pending_tokens = NULL;
			lexer->pending_count = 0;
			lexer->pending_pos = 0;
		}
		return tok;
	}

	skip_whitespace(lexer);

	const char *start = lexer->cur;
	int line = lexer->line;
	int column = lexer->column;

	if (is_at_end(lexer)) {
		return make_token(lexer, TOK_EOF, start, 0, line, column);
	}

	/* check for comments */
	if (peek(lexer) == '/' && peek_next(lexer) == '*') {
		return lex_block_comment(lexer);
	}
	if (peek(lexer) == '/' && peek_next(lexer) == '/') {
		return lex_comment(lexer);
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

	if (c == '"') {
		lexer->cur--;
		lexer->column--;
		return lex_string(lexer);
	}

	if (c == '\'') {
		lexer->cur--;
		lexer->column--;
		return lex_char_lit(lexer);
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
		if (peek(lexer) == '>') {
			advance(lexer);
			return make_token(lexer, TOK_ARROW, start, 2, line, column);
		}
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
	case TOK_STRING:
		return "TOK_STRING";
	case TOK_COMMENT:
		return "TOK_COMMENT";

	case TOK_ARCHETYPE:
		return "TOK_ARCHETYPE";
	case TOK_PROC:
		return "TOK_PROC";
	case TOK_SYS:
		return "TOK_SYS";
	case TOK_FUNC:
		return "TOK_FUNC";
	case TOK_LET:
		return "TOK_LET";
	case TOK_FOR:
		return "TOK_FOR";
	case TOK_IF:
		return "TOK_IF";
	case TOK_IN:
		return "TOK_IN";
	case TOK_FREE:
		return "TOK_FREE";
	case TOK_BREAK:
		return "TOK_BREAK";
	case TOK_EXTERN:
		return "TOK_EXTERN";
	case TOK_OUT:
		return "TOK_OUT";
	case TOK_RETURN:
		return "TOK_RETURN";
	case TOK_CHAR_LIT:
		return "TOK_CHAR_LIT";

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

	case TOK_ARROW:
		return "TOK_ARROW";

	case TOK_BANG:
		return "TOK_BANG";

	default:
		return "TOK_UNKNOWN";
	}
}

TokenBuffer lexer_tokenize(const char *src) {
	Lexer lexer;
	lexer_init(&lexer, src);

	Token *tokens = NULL;
	size_t count = 0;
	size_t capacity = 0;

	for (;;) {
		Token tok = lexer_next_token(&lexer);

		/* grow array if needed */
		if (count >= capacity) {
			capacity = (capacity == 0) ? 32 : capacity * 2;
			tokens = realloc(tokens, capacity * sizeof(Token));
		}

		tokens[count++] = tok;

		if (tok.kind == TOK_EOF)
			break;
	}

	return (TokenBuffer){tokens, count};
}

void token_buffer_free(TokenBuffer *buf) {
	if (buf) {
		free(buf->tokens);
		buf->tokens = NULL;
		buf->count = 0;
	}
}
