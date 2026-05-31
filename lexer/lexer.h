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
	TOK_CHAR_LIT,
	TOK_COMMENT,

	/* keywords */
	TOK_ARCHETYPE,
	TOK_PROC,
	TOK_SYS,
	TOK_FUNC,
	TOK_LET,
	TOK_FOR,
	TOK_IF,
	TOK_ELSE,
	TOK_IN,
	TOK_BREAK,
	TOK_EXTERN,
	TOK_MOVE,
	TOK_OWN,
	TOK_COPY,
	TOK_RETURN,
	TOK_USE,
	TOK_EACH_FIELD,
	TOK_STATIC,
	TOK_POOL,
	TOK_RUN,
	TOK_ENUM,
	TOK_MATCH,

	/* punctuation */
	TOK_LPAREN,      /* ( */
	TOK_RPAREN,      /* ) */
	TOK_LBRACE,      /* { */
	TOK_RBRACE,      /* } */
	TOK_LBRACKET,    /* [ */
	TOK_RBRACKET,    /* ] */
	TOK_COMMA,       /* , */
	TOK_DOT,         /* . */
	TOK_DOTDOTDOT,   /* ... — variadic marker in extern signatures */
	TOK_COLON,       /* : */
	TOK_SEMI,        /* ; */
	TOK_AT,          /* @  (decl-site decorators like @allow_pure_proc) */
	TOK_HASH,        /* #  bare/unknown directive (error) */
	TOK_HASH_MODULE, /* #module — narrow visibility to module scope (phase B) */
	TOK_HASH_FILE,   /* #file   — narrow visibility to file scope (phase B) */

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

	/* arrow */
	TOK_ARROW, /* -> */

	/* unary */
	TOK_BANG /* ! */
} TokenKind;

/* Trivia = anything between syntactic tokens that the parser doesn't structurally
 * consume but the formatter needs to preserve: line comments, block comments, and
 * runs of blank lines. Trivia is attached to the syntactic token that immediately
 * follows it (Roslyn-style leading trivia). The formatter, when emitting that
 * token, prints its leading trivia first. Inline (same-line) trailing comments
 * are also represented this way — they land on the next syntactic token's
 * leading trivia, and the formatter detects them by comparing line numbers. */
typedef enum {
	TRIVIA_LINE_COMMENT,  /* line comment (not currently produced by lexer) */
	TRIVIA_BLOCK_COMMENT, /* block comment (not currently produced by lexer) */
	TRIVIA_BLANK_LINES,   /* one or more blank lines between syntactic tokens */
} TriviaKind;

typedef struct {
	TriviaKind kind;
	const char *start; /* meaningful for COMMENT kinds; NULL for BLANK_LINES */
	size_t length;     /* meaningful for COMMENT kinds; 0 for BLANK_LINES */
	int line;          /* source line where this trivia begins */
	int column;
	int blank_count; /* meaningful for BLANK_LINES (1 = one blank line between tokens) */
} Trivia;

typedef struct Token {
	TokenKind kind;
	const char *start;
	size_t length;
	int line;
	int column;
	int int_val;
	/* Leading trivia: heap-allocated, owned by the Token. Set by the lexer
	 * (or token_buffer_attach_trivia for buffer-based parsing). NULL/0 when
	 * the token has no preceding trivia. */
	Trivia *leading_trivia;
	int leading_count;
} Token;

typedef struct Lexer {
	const char *src;
	const char *cur;
	int line;
	int column;
	Token *pending_tokens;
	int pending_count;
	int pending_pos;
	char *string_buf;
	size_t string_buf_size;
	size_t string_buf_pos;
} Lexer;

void lexer_init(Lexer *lexer, const char *src);
void lexer_free(Lexer *lexer);
Token lexer_next_token(Lexer *lexer);
const char *token_kind_name(TokenKind kind);

/* Doc-comment classification. A doc comment is just a TOK_COMMENT whose text
 * begins with the doc marker — there is no separate token kind (so doc comments
 * round-trip through the formatter for free). These are the ONE place that
 * decides "is this a doc comment"; every consumer (cst_view doc query, doctest
 * extractor, hover) must call them rather than re-checking the prefix inline, so
 * the marker can evolve from a single definition.
 *
 * arche_is_doc_comment    → outer doc `///` (attaches to the following decl),
 *                            excluding `////`+ banner rules (Rust convention).
 * arche_is_inner_doc_comment → inner/module doc `//!`.
 * `text`/`len` are the comment leaf's exact source span (leading `//` included). */
int arche_is_doc_comment(const char *text, size_t len);
int arche_is_inner_doc_comment(const char *text, size_t len);

/* Full-buffer tokenization: lex entire source into an array.
 * Returns a TokenBuffer with all tokens (including TOK_COMMENT).
 * Terminates with TOK_EOF as the last entry.
 * Caller must free buf.tokens with token_buffer_free(). */
typedef struct {
	Token *tokens;
	size_t count;
} TokenBuffer;

TokenBuffer lexer_tokenize(const char *src);
void token_buffer_free(TokenBuffer *buf);

#endif
