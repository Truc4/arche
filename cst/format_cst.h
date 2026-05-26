#ifndef FORMAT_CST_H
#define FORMAT_CST_H

/* A formatter driven by the lossless CST instead of the abstract AstProgram tree.
 * It walks the syntax tree and emits tokens with structure-aware spacing,
 * indentation, and blank-line preservation. Because every token + comment is in
 * the CST, this needs no per-node re-synthesis — unlike the AstProgram-based
 * format_program it is meant to eventually replace. Built first as a separate
 * tool so the existing formatter and corpus are untouched until it's validated.
 */

#include "syntax_tree.h"
#include <stdio.h>

void format_cst(FILE *out, const SyntaxNode *root, const char *src);

#endif /* FORMAT_CST_H */
