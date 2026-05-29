#ifndef TYCHECK_H
#define TYCHECK_H

/* Bidirectional type checker. Runs after symbol resolution; reads the
 * reconstructed AstProgram + ctx (symbol tables, archetype registry) and emits
 * diagnostics through the existing sem_emit_* registry. The pass is
 * fail-open during build-out: any rule not yet encoded `synth`'s to
 * TYID_UNKNOWN and `check` accepts unknown against any expected type, so
 * unencoded constructs produce zero spurious diagnostics.
 *
 * Phase A encodes ONE rule end-to-end: STMT_RETURN values must match the
 * enclosing func/proc's declared return types. This proves the wiring; Phase B
 * fills the rest of the rulebook (call arity, arg types, indexing, binops,
 * etc.). */

#include "semantic.h"

void tycheck_run(SemanticContext *ctx);

#endif /* TYCHECK_H */
