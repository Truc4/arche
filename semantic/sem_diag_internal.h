#ifndef SEM_DIAG_INTERNAL_H
#define SEM_DIAG_INTERNAL_H

/* Internal plumbing shared between semantic.c and sem_diagnostics.c.
 * NOT part of the public API — anything here is implementation detail.
 *
 * `diag_push` is the single point that appends to SemanticContext.diags and
 * bumps error_count when severity is 1. Both legacy paths (error_at / lint_emit)
 * and the new typed wrappers in sem_diagnostics.c route through here, so
 * SemDiag ownership rules live in one place. */

#include "semantic.h"

SemDiag *diag_push(SemanticContext *ctx, int severity, int has_loc, SourceLoc loc, const char *code, const char *name,
                   const char *msg);

/* True if the currently-analyzed decl has `@allow(<slug>)` suppressing this slug.
 * Lints consult this; errors never do (errors are never suppressible). */
int sem_diag_slug_suppressed(SemanticContext *ctx, const char *slug);

#endif /* SEM_DIAG_INTERNAL_H */
