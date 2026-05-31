#ifndef SEMANTIC_H
#define SEMANTIC_H

#include "../cst/cst.h"
#include "sem_hints.h"
#include "sem_model.h"

/* Semantic analysis context */
typedef struct SemanticContext SemanticContext;

/* Create and analyze a program. Reconstructs the abstract `AstProgram` from the lossless CST
 * (+ registered module CSTs) via cst_to_program, analyzes it, and keys the side model by
 * CST node id (read by lowering). This is the only analysis entry. */
SemanticContext *semantic_analyze_cst(const SyntaxNode *root, const char *src);

/* Register a `use`-module's CST so semantic_analyze_cst can inline it (parallel to
 * lower_add_module). Call once per module before semantic_analyze_cst. */
void semantic_add_module(const char *name, const SyntaxNode *root, const char *src);
/* Clear registered modules (static registry; reset at the start of each compilation). */
void semantic_reset_modules(void);
/* 1 if a module of this name is currently registered (used to detect import-not-found). */
int semantic_has_module(const char *name);

/* Test/helper: parse `src` and reconstruct the abstract `AstProgram` from the lossless CST
 * (parse -> cst_to_program). The returned AstProgram is self-contained (owns all its strings)
 * and outlives the CST, so free it with ast_program_free. Returns NULL on parse error. This is
 * how callers obtain a `AstProgram` now that the parser builds only the CST. */
AstProgram *cst_to_program_from_source(const char *src);

void semantic_context_free(SemanticContext *ctx);

/* The resolved-type side model (keyed by CST node id); read by lowering. */
SemModel *sem_context_model(SemanticContext *ctx);

/* The reconstructed AstProgram (read-only); used by passes that need to walk
 * decls. tycheck uses this to traverse function bodies. */
AstProgram *semantic_context_program(SemanticContext *ctx);

/* Editor-facing inferred facts (keyed by CST node id); read by the analyzer. */
SemHints *sem_context_hints(SemanticContext *ctx);

/* Resolve a (possibly nominal-alias) type name through the alias chain to its
 * backing; returns `name` unchanged if not an alias. */
const char *semantic_resolve_type_alias(SemanticContext *ctx, const char *name);

/* If `name` is a callable alias (`handler :: some_proc`), the ultimate proc/func target name;
 * else NULL. Lowering rewrites call callees through this and drops the alias binding. */
const char *semantic_resolve_callable_alias(SemanticContext *ctx, const char *name);

/* Error checking */
int semantic_has_errors(SemanticContext *ctx);
int semantic_error_count(const SemanticContext *ctx);

/* A secondary location attached to a parent diagnostic — like rustc's "note: …"
 * or clang's related-info. `message` is owned. */
typedef struct {
	SourceLoc loc;
	char *message;
} SemDiagNote;

/* Structured diagnostics collected during analysis (mirrors the stderr lint/error
 * prints but kept queryable for editor consumers). `has_loc==0` means the source
 * position is unknown; editors surface it at file top.
 *
 * Stored as individually heap-allocated objects (ctx->diags is `SemDiag **`), so
 * pointers handed out by sem_emit_<slug> wrappers stay valid for the lifetime
 * of the SemanticContext — `sem_diag_note(d, ...)` after intervening emits is safe. */
typedef struct {
	int severity;       /* 0 = warning, 1 = error */
	int has_loc;        /* 0 = no source position known */
	SourceLoc loc;      /* line/col when known */
	const char *code;   /* stable identifier ("E0001"); NULL only for the legacy path */
	const char *name;   /* slug ("undefined-symbol", "proc-no-effect") or "semantic" */
	char *message;      /* owned */
	SemDiagNote *notes; /* NULL when none; appended via sem_diag_note */
	int note_count;
} SemDiag;

int sem_diag_count(const SemanticContext *ctx);
const SemDiag *sem_diag_at(const SemanticContext *ctx, int i);

/* Attach a related-location note to a previously-emitted diagnostic. NULL-tolerant —
 * sem_emit_<slug> wrappers return NULL for suppressed lints, and `sem_diag_note(NULL, ...)`
 * is a safe no-op so callers don't need to branch. */
void sem_diag_note(SemDiag *parent, SourceLoc loc, const char *fmt, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 3, 4)))
#endif
    ;

/* Archetype queries */
int semantic_archetype_exists(SemanticContext *ctx, const char *name);
int semantic_field_exists(SemanticContext *ctx, const char *archetype_name, const char *field_name);
FieldKind semantic_field_kind(SemanticContext *ctx, const char *archetype_name, const char *field_name);
const char *semantic_field_type_name(SemanticContext *ctx, const char *archetype_name, const char *field_name);

/* Constant queries */
const char *semantic_get_const_value(SemanticContext *ctx, const char *const_name);

/* Lint configuration. Both lints are enabled by default. CLI flags
 * (--Wno-proc-could-be-func / --Wno-proc-no-effect) disable them;
 * --Werror=proc-could-be-func / --Werror=proc-no-effect promote each
 * warning to a hard error. */
void semantic_set_lint_proc_could_be_func(int enabled, int werror);
void semantic_set_lint_proc_no_effect(int enabled, int werror);

#endif /* SEMANTIC_H */
