#ifndef TOKEN_CATEGORY_H
#define TOKEN_CATEGORY_H

#include "../lexer/lexer.h"
#include "syntax_tree.h"

/* Editor token classification: the category string for a token leaf given the
 * kind of the syntax tree node that directly contains it, or NULL to skip (EOF / ERROR /
 * anything unmapped). Identifiers are classified by their enclosing node
 * (type / function / property / parameter / variable). This is the single source
 * of truth for syntax highlighting, shared by `arche-syntax-tokens` and the
 * analyzer's warm TOKENS query. */
const char *arche_token_category(TokenKind kind, SyntaxNodeKind parent);

#endif /* TOKEN_CATEGORY_H */
