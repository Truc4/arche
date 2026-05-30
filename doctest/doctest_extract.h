#ifndef ARCHE_DOCTEST_EXTRACT_H
#define ARCHE_DOCTEST_EXTRACT_H

#include "../cst/syntax_tree.h"

/* Fence info-string flags (```arche,<flag>,...), Rust-style. */
typedef enum {
	DOCTEST_NORMAL = 0,
	DOCTEST_NO_RUN = 1 << 0,       /* compile only, do not run */
	DOCTEST_COMPILE_FAIL = 1 << 1, /* expected to FAIL compilation */
	DOCTEST_SHOULD_PANIC = 1 << 2, /* expected to exit non-zero when run */
	DOCTEST_IGNORE = 1 << 3,       /* shown in docs but neither compiled nor run */
} DoctestFlags;

/* One runnable example pulled from a ```arche fenced block inside a doc comment. */
typedef struct {
	char *code;      /* owned, NUL-terminated: the fenced block body (arche source) */
	char *decl_name; /* owned: name of the documented declaration ("module" for //!) */
	int src_line;    /* 1-based source line of the opening ```arche fence */
	int has_main;    /* 1 if `code` already declares `proc main` (runner won't wrap it) */
	int flags;       /* bitwise-OR of DoctestFlags */
} DoctestExample;

typedef struct {
	DoctestExample *items;
	int count;
} DoctestExamples;

/* Walk every declaration in `root`, collect its doc comments (via the cst_view
 * doc query), and extract each ```arche fenced block as one example. An
 * unterminated fence is skipped (its decl yields no example) rather than
 * aborting. Caller frees with doctest_examples_free. */
DoctestExamples doctest_extract(const SyntaxNode *root, const char *src);
void doctest_examples_free(DoctestExamples *ex);

#endif /* ARCHE_DOCTEST_EXTRACT_H */
