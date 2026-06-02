#include "token_category.h"

/* Role for an identifier, from the node that directly contains it. */
static const char *ident_role(SyntaxNodeKind parent) {
	switch (parent) {
	case SN_TYPE_REF:
	case SN_TYPE_ARRAY:
	case SN_TYPE_SHAPED_ARRAY:
	case SN_TYPE_TUPLE:
	case SN_TYPE_HANDLE:
	case SN_ALLOC_TYPE:
	case SN_TYPE_DEF_NAME:
		return "type";
	case SN_FUNC_DEF_NAME:
	case SN_CALLEE_NAME:
		return "function";
	case SN_FIELD_NAME:
		return "property";
	case SN_PARAM_NAME:
		return "parameter";
	default:
		return "variable";
	}
}

const char *arche_token_category(TokenKind kind, SyntaxNodeKind parent) {
	switch (kind) {
	case TOK_ARCHETYPE:
	case TOK_PROC:
	case TOK_SYS:
	case TOK_FUNC:
	case TOK_LET:
	case TOK_FOR:
	case TOK_IF:
	case TOK_ELSE:
	case TOK_IN:
	case TOK_BREAK:
	case TOK_MOVE:
	case TOK_OWN:
	case TOK_COPY:
	case TOK_RETURN:
	case TOK_USE:
	case TOK_EACH_FIELD:
	case TOK_HASH_MODULE:
	case TOK_HASH_FILE:
	case TOK_HASH_FOREIGN:
	case TOK_RUN:
	case TOK_ENUM:
	case TOK_MATCH:
		return "keyword";

	case TOK_NUMBER:
		return "number";
	case TOK_STRING:
	case TOK_CHAR_LIT:
		return "string";
	case TOK_COMMENT:
		return "comment";

	case TOK_IDENT:
		return ident_role(parent);

	case TOK_EQ:
	case TOK_PLUS_EQ:
	case TOK_MINUS_EQ:
	case TOK_STAR_EQ:
	case TOK_SLASH_EQ:
	case TOK_PLUS:
	case TOK_MINUS:
	case TOK_STAR:
	case TOK_SLASH:
	case TOK_EQ_EQ:
	case TOK_BANG_EQ:
	case TOK_LT:
	case TOK_GT:
	case TOK_LT_EQ:
	case TOK_GT_EQ:
	case TOK_ARROW:
	case TOK_BANG:
	case TOK_AT:
	case TOK_AMP_AMP:
	case TOK_PIPE_PIPE:
		return "operator";

	case TOK_LPAREN:
	case TOK_RPAREN:
	case TOK_LBRACE:
	case TOK_RBRACE:
	case TOK_LBRACKET:
	case TOK_RBRACKET:
	case TOK_COMMA:
	case TOK_DOT:
	case TOK_DOTDOTDOT:
	case TOK_COLON:
	case TOK_SEMI:
		return "punctuation";

	/* Genuinely uncategorized — not highlighted. Listed explicitly (NOT a `default`) so this switch
	 * is EXHAUSTIVE over TokenKind. The build is `-Werror=switch` project-wide, so a no-`default`
	 * switch that misses an enum value is a compile error — adding a new token without categorizing
	 * it here cannot build (and tests/unit/tooling/keyword_highlight.arche backstops it). Make wrong
	 * harder than right: omit the `default`, list every case. */
	case TOK_EOF:
	case TOK_ERROR:
	case TOK_HASH: /* bare/unknown `#` — a parse error, not a highlightable token */
		return NULL;
	}
	return NULL; /* unreachable: the switch above is exhaustive */
}
