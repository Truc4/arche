#ifndef SEM_DIAGNOSTICS_H
#define SEM_DIAGNOSTICS_H

/* Diagnostic registry: the single source of truth for every error and lint the
 * semantic analyzer emits. The goal is that adding a diagnostic is mechanical
 * and obvious to a new contributor:
 *
 *   1. Add an entry to `SemDiagKind` and a row to `sem_diag_table` (sem_diagnostics.c).
 *   2. Hand-write one wrapper function `sem_emit_<slug>(ctx, loc, args...)` that
 *      calls `sem_emit_v(ctx, kind, loc, "...format...", args...)`. The format
 *      literal lives in the wrapper body so `gcc -Wformat` catches arg/type/order
 *      bugs at every callsite — that's why the wrapper exists.
 *   3. Call the wrapper from the detection site.
 *   4. Add a lit fixture under tests/unit/language/.
 *
 * Severity is configuration, not identity (clang's model): hard errors fire
 * unconditionally; lints can be disabled (`-Wno-<slug>`) or promoted to errors
 * (`-Werror=<slug>`) via `semantic_set_diag`.
 *
 * Wrappers return `SemDiag *` so callers can attach related-location notes via
 * `sem_diag_note(d, loc, "fmt", ...)`. The pointer stays valid for the lifetime
 * of the SemanticContext — `ctx->diags` is a pointer array, not a flat array, so
 * subsequent emissions don't invalidate earlier returns. `NULL` is returned when
 * a lint is disabled, and `sem_diag_note(NULL, ...)` is a safe no-op so callers
 * don't need to branch on it. */

#include "semantic.h"
#include <stdint.h>

typedef enum {
	/* === Errors (hard, always fire) === */
	/* Name resolution */
	SEM_DIAG_undefined_symbol,
	SEM_DIAG_use_after_consume,
	SEM_DIAG_undefined_field_base,
	SEM_DIAG_undefined_archetype_alloc,
	SEM_DIAG_undefined_archetype_for,
	SEM_DIAG_undefined_archetype_bind,

	/* Type system */
	SEM_DIAG_type_alias_redefined,
	SEM_DIAG_type_alias_unknown_backing,
	SEM_DIAG_local_alias_invalid_backing,
	SEM_DIAG_alias_backing_invalid,
	SEM_DIAG_extern_type_mismatch,
	SEM_DIAG_no_implicit_conversion,
	SEM_DIAG_tuple_field_not_simple,
	SEM_DIAG_const_type_mismatch,
	SEM_DIAG_meta_type_invalid_position,
	SEM_DIAG_binop_type_mismatch,

	/* Ownership */
	SEM_DIAG_opaque_not_consumed,
	SEM_DIAG_cannot_copy_opaque,
	SEM_DIAG_cannot_move_borrowed,
	SEM_DIAG_copy_unsupported,
	SEM_DIAG_assign_after_move,
	SEM_DIAG_own_requires_move_or_copy,
	SEM_DIAG_cannot_mutate_borrowed,
	SEM_DIAG_move_outside_arg,
	/* E0113/E0114 retired: arche has zero runtime allocation, no `free` statement.
	 * Burn-on-delete (codes never reused). */

	/* Field / component access */
	SEM_DIAG_no_field,
	SEM_DIAG_cannot_read_through_handle,
	SEM_DIAG_not_archetype_instance,
	SEM_DIAG_field_on_non_archetype,

	/* Declaration shape */
	SEM_DIAG_archetype_only_as_param,
	SEM_DIAG_archetype_not_return_type,
	SEM_DIAG_archetype_funcs_only,
	SEM_DIAG_multiple_archetype_params,
	SEM_DIAG_handle_in_sys_param,
	SEM_DIAG_each_field_filter_type_not_name,
	SEM_DIAG_each_field_filter_type_not_primitive,
	SEM_DIAG_each_field_invalid_rhs,

	/* Allocation / archetype */
	SEM_DIAG_alloc_not_at_top,
	SEM_DIAG_alloc_count_not_literal,
	SEM_DIAG_shape_already_allocated,
	SEM_DIAG_duplicate_component,

	/* Function calls / groups */
	SEM_DIAG_action_in_expression,
	SEM_DIAG_no_group_match,
	SEM_DIAG_ambiguous_group_call,
	SEM_DIAG_unknown_group_member,
	SEM_DIAG_group_member_extern,
	SEM_DIAG_group_member_not_declared,
	SEM_DIAG_duplicate_group_signatures,
	SEM_DIAG_group_name_collision,
	SEM_DIAG_empty_group,

	/* Extern / type signature */
	SEM_DIAG_extern_proc_bad_type,
	SEM_DIAG_extern_func_bad_type,
	SEM_DIAG_extern_func_bad_return,
	SEM_DIAG_extern_proc_bad_return,

	/* Unsafe / builtins */
	SEM_DIAG_syscall_in_safe,

	/* Constants / meta */
	SEM_DIAG_constant_redefined,
	SEM_DIAG_const_value_is_type,
	SEM_DIAG_unknown_const_value,
	SEM_DIAG_const_rhs_invalid,

	/* Purity */
	SEM_DIAG_func_not_pure,

	/* Assignment targets */
	SEM_DIAG_assign_to_const,
	SEM_DIAG_assign_to_undeclared,

	/* Front-end (parse + module loading) — wired in P1f */
	SEM_DIAG_parse_error,
	SEM_DIAG_module_not_found,
	SEM_DIAG_module_parse_failed,

	/* === Lints (promotable warnings) === */
	SEM_LINT_proc_could_be_func,
	SEM_LINT_proc_no_effect,
	SEM_LINT_func_impure,
	SEM_LINT_unused_local,
	SEM_LINT_unused_use,

	SEM_DIAG_KIND_COUNT
} SemDiagKind;

/* Stable identity for a diagnostic kind. Codes are immutable once shipped (burn on
 * delete — a code never returns to the pool); messages are NOT part of the API. */
const char *sem_diag_code(SemDiagKind kind); /* "E0001" / "W0001" */
const char *sem_diag_slug(SemDiagKind kind); /* "undefined_symbol" / "proc_no_effect" */

/* Runtime configuration. Hard errors aren't configurable; calling `semantic_set_diag`
 * on a non-lint kind is silently ignored (logged via assert in debug builds). */
void semantic_set_diag(SemDiagKind kind, int enabled, int werror);
int semantic_diag_enabled(SemDiagKind kind);
int semantic_diag_werror(SemDiagKind kind);

/* Escape-hatch builder for diagnostics that need rich multi-span structure
 * (two-span type-mismatch, did-you-mean suggestions). Reserved for the ~20% of
 * future diagnostics that don't fit `printf` + notes; not used by Phase 1.
 *   d = sem_diag_open(ctx, KIND, primary_loc);
 *   sem_diag_label(d, "expected %s", expected);
 *   sem_diag_span(d, other_loc, "got %s here", got);
 *   sem_diag_emit(d);
 */
SemDiag *sem_diag_open(SemanticContext *ctx, SemDiagKind kind, SourceLoc primary);
void sem_diag_label(SemDiag *d, const char *fmt, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 2, 3)))
#endif
    ;
void sem_diag_span(SemDiag *d, SourceLoc loc, const char *fmt, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 3, 4)))
#endif
    ;
void sem_diag_emit(SemDiag *d);

/* === Per-kind typed wrappers ===
 *
 * Each wrapper holds the format string as a literal in its body, so `-Wformat`
 * checks every callsite (the wrapper's signature also documents the args).
 *
 * Returns the new SemDiag (so the caller can attach notes via sem_diag_note),
 * or NULL when the kind is a disabled lint. NULL-tolerant note attachment is
 * the explicit contract — callers don't have to branch. */

SemDiag *sem_emit_undefined_symbol(SemanticContext *ctx, SourceLoc loc, const char *name);
SemDiag *sem_emit_use_after_consume(SemanticContext *ctx, SourceLoc loc, const char *name);
SemDiag *sem_emit_undefined_field_base(SemanticContext *ctx, SourceLoc loc, const char *name);
SemDiag *sem_emit_undefined_archetype_alloc(SemanticContext *ctx, SourceLoc loc, const char *name);
SemDiag *sem_emit_undefined_archetype_for(SemanticContext *ctx, SourceLoc loc, const char *name);
SemDiag *sem_emit_undefined_archetype_bind(SemanticContext *ctx, SourceLoc loc, const char *name);

SemDiag *sem_emit_type_alias_redefined(SemanticContext *ctx, SourceLoc loc, const char *name);
SemDiag *sem_emit_type_alias_unknown_backing(SemanticContext *ctx, SourceLoc loc, const char *name,
                                             const char *backing);
SemDiag *sem_emit_local_alias_invalid_backing(SemanticContext *ctx, SourceLoc loc);
SemDiag *sem_emit_alias_backing_invalid(SemanticContext *ctx, SourceLoc loc);
SemDiag *sem_emit_extern_type_mismatch(SemanticContext *ctx, SourceLoc loc, const char *func_name,
                                       const char *param_name, const char *expected, const char *got);
SemDiag *sem_emit_no_implicit_conversion(SemanticContext *ctx, SourceLoc loc, const char *got, const char *name,
                                         const char *want);
SemDiag *sem_emit_tuple_field_not_simple(SemanticContext *ctx, SourceLoc loc);
SemDiag *sem_emit_const_type_mismatch(SemanticContext *ctx, SourceLoc loc, const char *name, const char *want,
                                      const char *got);
SemDiag *sem_emit_meta_type_invalid_position(SemanticContext *ctx, SourceLoc loc, const char *where);

SemDiag *sem_emit_opaque_not_consumed(SemanticContext *ctx, SourceLoc loc, const char *name);
SemDiag *sem_emit_cannot_copy_opaque(SemanticContext *ctx, SourceLoc loc, const char *name);
SemDiag *sem_emit_cannot_move_borrowed(SemanticContext *ctx, SourceLoc loc, const char *name);
SemDiag *sem_emit_copy_unsupported(SemanticContext *ctx, SourceLoc loc, const char *name);
SemDiag *sem_emit_assign_after_move(SemanticContext *ctx, SourceLoc loc, const char *name);
SemDiag *sem_emit_own_requires_move_or_copy(SemanticContext *ctx, SourceLoc loc, const char *arg_name,
                                            const char *param_name, const char *func_name);
SemDiag *sem_emit_cannot_mutate_borrowed(SemanticContext *ctx, SourceLoc loc, const char *name);

SemDiag *sem_emit_no_field(SemanticContext *ctx, SourceLoc loc, const char *arch_name, const char *field_name);
SemDiag *sem_emit_cannot_read_through_handle(SemanticContext *ctx, SourceLoc loc, const char *field_name,
                                             const char *base_name, const char *arch_name);
SemDiag *sem_emit_not_archetype_instance(SemanticContext *ctx, SourceLoc loc, const char *name);

SemDiag *sem_emit_archetype_only_as_param(SemanticContext *ctx, SourceLoc loc);
SemDiag *sem_emit_archetype_not_return_type(SemanticContext *ctx, SourceLoc loc, const char *func_name);
SemDiag *sem_emit_archetype_funcs_only(SemanticContext *ctx, SourceLoc loc, const char *func_name);
SemDiag *sem_emit_multiple_archetype_params(SemanticContext *ctx, SourceLoc loc, const char *proc_name);
SemDiag *sem_emit_handle_in_sys_param(SemanticContext *ctx, SourceLoc loc, const char *name);
SemDiag *sem_emit_each_field_filter_type_not_name(SemanticContext *ctx, SourceLoc loc);
SemDiag *sem_emit_each_field_filter_type_not_primitive(SemanticContext *ctx, SourceLoc loc);
SemDiag *sem_emit_each_field_invalid_rhs(SemanticContext *ctx, SourceLoc loc, const char *name);

SemDiag *sem_emit_alloc_not_at_top(SemanticContext *ctx, SourceLoc loc);
SemDiag *sem_emit_alloc_count_not_literal(SemanticContext *ctx, SourceLoc loc);
SemDiag *sem_emit_shape_already_allocated(SemanticContext *ctx, SourceLoc loc, const char *name);
SemDiag *sem_emit_duplicate_component(SemanticContext *ctx, SourceLoc loc, const char *name);

SemDiag *sem_emit_action_in_expression(SemanticContext *ctx, SourceLoc loc, const char *kind, const char *name);
SemDiag *sem_emit_no_group_match(SemanticContext *ctx, SourceLoc loc, const char *name);
SemDiag *sem_emit_ambiguous_group_call(SemanticContext *ctx, SourceLoc loc, const char *name);
SemDiag *sem_emit_unknown_group_member(SemanticContext *ctx, SourceLoc loc, const char *member, const char *group);
SemDiag *sem_emit_group_member_extern(SemanticContext *ctx, SourceLoc loc, const char *member, const char *group);
SemDiag *sem_emit_group_member_not_declared(SemanticContext *ctx, SourceLoc loc, const char *member, const char *group);
SemDiag *sem_emit_duplicate_group_signatures(SemanticContext *ctx, SourceLoc loc, const char *m1, const char *m2,
                                             const char *group);
SemDiag *sem_emit_group_name_collision(SemanticContext *ctx, SourceLoc loc, const char *name);
SemDiag *sem_emit_empty_group(SemanticContext *ctx, SourceLoc loc);

SemDiag *sem_emit_extern_proc_bad_type(SemanticContext *ctx, SourceLoc loc, const char *type, const char *proc_name);
SemDiag *sem_emit_extern_func_bad_type(SemanticContext *ctx, SourceLoc loc, const char *type, const char *func_name);
SemDiag *sem_emit_extern_func_bad_return(SemanticContext *ctx, SourceLoc loc, const char *type, const char *func_name);

SemDiag *sem_emit_syscall_in_safe(SemanticContext *ctx, SourceLoc loc);

SemDiag *sem_emit_constant_redefined(SemanticContext *ctx, SourceLoc loc, const char *name);
SemDiag *sem_emit_const_value_is_type(SemanticContext *ctx, SourceLoc loc, const char *name, const char *rhs);
SemDiag *sem_emit_unknown_const_value(SemanticContext *ctx, SourceLoc loc, const char *rhs, const char *name);
SemDiag *sem_emit_const_rhs_invalid(SemanticContext *ctx, SourceLoc loc);

SemDiag *sem_emit_func_not_pure(SemanticContext *ctx, SourceLoc loc, const char *name, const char *reason);

SemDiag *sem_emit_assign_to_const(SemanticContext *ctx, SourceLoc loc, const char *name);
SemDiag *sem_emit_assign_to_undeclared(SemanticContext *ctx, SourceLoc loc, const char *name);

SemDiag *sem_emit_parse_error(SemanticContext *ctx, SourceLoc loc, const char *msg);
SemDiag *sem_emit_module_not_found(SemanticContext *ctx, SourceLoc loc, const char *name);
SemDiag *sem_emit_module_parse_failed(SemanticContext *ctx, SourceLoc loc, const char *name);

/* Tier 1 (Phase 2) — see docs/DIAGNOSTICS.md. Codes assigned append-only as E0110+ /
 * W0004+; enum positions are grouped by category for readability. */
SemDiag *sem_emit_binop_type_mismatch(SemanticContext *ctx, SourceLoc loc, const char *op, const char *lhs,
                                      const char *rhs);
SemDiag *sem_emit_field_on_non_archetype(SemanticContext *ctx, SourceLoc loc, const char *base_type, const char *field);
SemDiag *sem_emit_move_outside_arg(SemanticContext *ctx, SourceLoc loc, const char *keyword);
SemDiag *sem_emit_extern_proc_bad_return(SemanticContext *ctx, SourceLoc loc, const char *type, const char *proc_name);

/* Lints */
SemDiag *sem_emit_lint_proc_could_be_func(SemanticContext *ctx, SourceLoc loc, const char *name);
SemDiag *sem_emit_lint_proc_no_effect(SemanticContext *ctx, SourceLoc loc, const char *name);
SemDiag *sem_emit_lint_func_impure(SemanticContext *ctx, SourceLoc loc, const char *name, const char *reason);
SemDiag *sem_emit_lint_unused_local(SemanticContext *ctx, SourceLoc loc, const char *name);
SemDiag *sem_emit_lint_unused_use(SemanticContext *ctx, SourceLoc loc, const char *name);

#endif /* SEM_DIAGNOSTICS_H */
