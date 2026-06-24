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
	SEM_DIAG_extern_array_param_needs_own,
	SEM_DIAG_proc_return_has_value,
	SEM_DIAG_map_no_return,
	SEM_DIAG_map_not_a_transform,
	SEM_DIAG_collective_in_map,
	SEM_DIAG_invalid_monoid_op,

	SEM_DIAG_move_outside_arg,
	/* E0113/E0114 retired: arche has zero runtime allocation, no `free` statement.
	 * Burn-on-delete (codes never reused). */
	SEM_DIAG_underscore_not_inout,
	SEM_DIAG_local_shadows_callable,

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
	SEM_DIAG_handle_in_map_param,
	SEM_DIAG_each_field_filter_type_not_name,
	SEM_DIAG_each_field_filter_type_not_primitive,
	SEM_DIAG_each_field_invalid_rhs,

	/* Allocation / archetype */
	SEM_DIAG_alloc_not_at_top,
	SEM_DIAG_alloc_count_not_literal,
	SEM_DIAG_shape_already_allocated,
	SEM_DIAG_duplicate_component,
	SEM_DIAG_component_redefined,

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
	SEM_DIAG_extern_multi_out,

	/* Constants / meta */
	SEM_DIAG_constant_redefined,
	SEM_DIAG_const_value_is_type,
	SEM_DIAG_unknown_const_value,
	SEM_DIAG_const_rhs_invalid,

	/* Purity */
	SEM_DIAG_func_not_pure,

	/* Totality */
	SEM_DIAG_insert_delete_outlist,

	/* Failure policies (`expr !policy`) — E0097/E0098/E0099/E0124 */
	SEM_DIAG_policy_provable_oob,        /* a constant index/slice provably outside [0,N) — even with a policy */
	SEM_DIAG_policy_func_aborts,         /* explicit `!abort` inside a func/policy (must be total) */
	SEM_DIAG_policy_unknown,             /* `!name` is neither an intrinsic nor a visible `policy` decl */
	SEM_DIAG_policy_wrong_category,      /* `!name` resolves to a policy of the wrong @policy(category) */
	SEM_DIAG_policy_wrong_sigil,         /* `?` (handler) used on a panic op, or `!` (panic) used on an insert */
	SEM_DIAG_policy_abort_forbidden,     /* an op resolves to `!abort` under --no-abort / --no-implicit-abort */
	SEM_DIAG_policy_undefined_forbidden, /* an `!undefined` site under --no-undefined */
	SEM_DIAG_policy_uses_undefined,      /* `!undefined` (raw/UB op) inside a `policy` body — must stay total */
	SEM_DIAG_cyclic_policy,              /* a `policy` applies itself (directly/transitively) — infinite inlining */
	SEM_DIAG_allow_forbidden,            /* an `@allow(...)` decorator under --forbid-allow */
	SEM_DIAG_duplicate_default,          /* a second `@default(kind, category, ...)` for the same cell */
	SEM_DIAG_default_invalid,            /* `@default` shape error: func+abort, func+pool, bad category */

	/* Assignment targets */
	SEM_DIAG_assign_to_const,
	SEM_DIAG_assign_to_undeclared,

	/* Front-end (parse + module loading) — wired in P1f */
	SEM_DIAG_parse_error,
	SEM_DIAG_module_not_found,
	SEM_DIAG_module_parse_failed,

	/* Control-flow + declaration uniqueness (P3 Phase C — E0030+) */
	SEM_DIAG_break_outside_loop,
	SEM_DIAG_continue_outside_loop,
	SEM_DIAG_module_no_member,
	SEM_DIAG_duplicate_decl,

	/* Opaque destructors / RAII (`@drop`) — E0118+ */
	SEM_DIAG_drop_invalid,     /* a `@drop` proc with a bad signature (not `proc(own T)()`) */
	SEM_DIAG_drop_redefined,   /* a second `@drop` for an already-registered opaque type */
	SEM_DIAG_drop_conditional, /* a handle consumed on some-but-not-all branch paths */

	/* Region uniqueness — E0121 */
	SEM_DIAG_duplicate_region, /* a second region of the same kind (`#module`/`#file`/`#foreign`/`#import`) in one file
	                            */

	/* Opaque sealing — E0122/E0123 */
	SEM_DIAG_opaque_overwrite, /* `=` over an existing opaque binding (create-once: bind with `:=`, never overwrite) */
	SEM_DIAG_opaque_construct, /* `T(x)` minting an opaque in arche (opaque handles originate only at the FFI boundary)
	                            */

	/* Tycheck (P3 type-check pass — E0200+) */
	SEM_DIAG_type_mismatch,
	SEM_DIAG_not_indexable,
	SEM_DIAG_wrong_arity,
	SEM_DIAG_non_exhaustive_match,
	SEM_DIAG_callable_in_archetype,
	SEM_DIAG_wildcard_in_enum_match,

	/* Query (`map(Name)` / `run`) — E0215+ */
	SEM_DIAG_unknown_query,     /* a map names a query that is not declared */
	SEM_DIAG_run_targets_query, /* `run X` where X is a query, not a map */

	/* Entities (`insert(Name{…})`) — E0217+ */
	SEM_DIAG_entity_missing_column, /* an entity literal omits a required column */
	SEM_DIAG_entity_unknown_column, /* an entity literal names a field that is not a column */
	SEM_DIAG_entity_unknown_type,   /* `Name{…}` where Name is neither an archetype nor a query */
	SEM_DIAG_positional_insert,     /* legacy positional `insert(Pool, v0, …)` — use an entity literal */
	SEM_DIAG_proc_under_applied,    /* under-applying a (non-extern) proc to "build an Eff" — only an extern
	                                   is inert under-applied; a proc minus its out-slots is a suspended
	                                   computation, not a value. E0221. */
	SEM_DIAG_eff_extern_not_static, /* an Eff at a run site whose extern is not statically one extern (a
	                                   runtime `?:`/`match` selecting different externs) — no fn-pointer. E0222. */

	/* === Lints (promotable warnings) === */
	SEM_LINT_proc_could_be_func,
	SEM_LINT_proc_no_effect,
	SEM_LINT_func_impure,
	SEM_LINT_unused_local,
	SEM_LINT_unused_use,
	SEM_LINT_inout_redundant_arg,
	SEM_LINT_inout_param_shadow,
	SEM_LINT_unused_function,
	SEM_LINT_unused_static_const,
	SEM_LINT_unused_enum,
	SEM_LINT_unused_query, /* a `query {…}` decl that no map references — W0025 */
	SEM_LINT_discarded_ok,
	SEM_LINT_raw_pool_index,
	SEM_LINT_policy_on_safe_op,       /* an explicit `!policy` on an op the prover already proved safe (dead policy) */
	SEM_LINT_handler_foreign_arch,    /* a pool `?handler` body references a DIFFERENT archetype's columns */
	SEM_LINT_redundant_guard,         /* a leading guard-exit re-tests the enclosing loop's own condition */
	SEM_LINT_func_could_be_const,     /* a zero-param func whose body is a single `return <literal/const>;` */
	SEM_LINT_exported_mutable_global, /* a top-level mutable global on the exported surface — shared mutable
	                                     state must be a pool or be narrowed to #module/#file. Lint-class so
	                                     it's tunable, but default-promoted to error (see ensure_init). */
	SEM_LINT_outarg_shadows_outparam, /* a call out-arg `(name:)` whose COLON declares a fresh local that
	                                     shadows the enclosing proc's out-param of the same name — the
	                                     call's result fills the shadow, the out-param is left unwritten
	                                     (silent lost writeback). Use `(name)` (no colon) to write it. */
	SEM_LINT_map_writes_foreign_pool, /* a system writes a pool it does not iterate — a foreign-pool write in
	                                     a per-entity system runs once, not per row (the driver WRITES shared
	                                     singletons, a system READS them). Lint-class so it's tunable, but
	                                     default-promoted to error (see ensure_init). */
	SEM_LINT_large_stack_array,       /* a local fixed-size array `[N]T` whose storage exceeds the frame
	                                     threshold (1 KB) — a big stack value. Prefer a single-type archetype
	                                     pool `[N]T` (static, columnar) or a #module-private global. W0026. */
	SEM_LINT_pointless_move,          /* `move` of a value with no ownership to transfer — currently a pool
	                                     column (slice): it is shared, fixed storage, so `move` does nothing.
	                                     Pass it as a plain borrow. W0027. */
	SEM_LINT_proc_calls_proc,         /* a proc body calls another (non-extern) proc — the flat-effect
	                                     proc→proc ban. Reuse lives in funcs (building Eff values), so a proc
	                                     never needs another proc; permitted callees are extern/func/map.
	                                     Default WARN (the stdlib/apps still nest procs until the Eff
	                                     convenience layer lands; flip to error later). W0028. */
	SEM_LINT_pool_index_outside_query, /* an explicit index on a pool column `Pool.col[i]` (incl. the
	                                      singleton `[0]`) outside a query/map/system fan body — pool values
	                                      must come from a query, not hand-indexing. Default WARN (tunable to
	                                      error per build, e.g. for an app + its libs). W0029. */

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

/* CLI sets this after prepending core.arche so user-typed diagnostic lines show
 * the user's file line, not the combined-source line. 0 = no translation. */
void semantic_set_print_line_offset(int offset);
int semantic_print_line_offset(void);

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
SemDiag *sem_emit_non_exhaustive_match(SemanticContext *ctx, SourceLoc loc, const char *missing);
SemDiag *sem_emit_wildcard_in_enum_match(SemanticContext *ctx, SourceLoc loc);
SemDiag *sem_emit_callable_in_archetype(SemanticContext *ctx, SourceLoc loc, const char *name);

SemDiag *sem_emit_opaque_not_consumed(SemanticContext *ctx, SourceLoc loc, const char *name);
SemDiag *sem_emit_cannot_copy_opaque(SemanticContext *ctx, SourceLoc loc, const char *name);
SemDiag *sem_emit_cannot_move_borrowed(SemanticContext *ctx, SourceLoc loc, const char *name);
SemDiag *sem_emit_copy_unsupported(SemanticContext *ctx, SourceLoc loc, const char *name);
SemDiag *sem_emit_assign_after_move(SemanticContext *ctx, SourceLoc loc, const char *name);
SemDiag *sem_emit_own_requires_move_or_copy(SemanticContext *ctx, SourceLoc loc, const char *arg_name,
                                            const char *param_name, const char *func_name);
SemDiag *sem_emit_cannot_mutate_borrowed(SemanticContext *ctx, SourceLoc loc, const char *name);
SemDiag *sem_emit_extern_array_param_needs_own(SemanticContext *ctx, SourceLoc loc, const char *param_name,
                                               const char *proc_name);
SemDiag *sem_emit_proc_return_has_value(SemanticContext *ctx, SourceLoc loc);
SemDiag *sem_emit_map_no_return(SemanticContext *ctx, SourceLoc loc);
SemDiag *sem_emit_map_not_a_transform(SemanticContext *ctx, SourceLoc loc, const char *kind);
SemDiag *sem_emit_collective_in_map(SemanticContext *ctx, SourceLoc loc, const char *name);
SemDiag *sem_emit_invalid_monoid_op(SemanticContext *ctx, SourceLoc loc, const char *fn, const char *op);

SemDiag *sem_emit_no_field(SemanticContext *ctx, SourceLoc loc, const char *arch_name, const char *field_name);
SemDiag *sem_emit_cannot_read_through_handle(SemanticContext *ctx, SourceLoc loc, const char *field_name,
                                             const char *base_name, const char *arch_name);
SemDiag *sem_emit_not_archetype_instance(SemanticContext *ctx, SourceLoc loc, const char *name);

SemDiag *sem_emit_archetype_only_as_param(SemanticContext *ctx, SourceLoc loc);
SemDiag *sem_emit_archetype_not_return_type(SemanticContext *ctx, SourceLoc loc, const char *func_name);
SemDiag *sem_emit_archetype_funcs_only(SemanticContext *ctx, SourceLoc loc, const char *func_name);
SemDiag *sem_emit_multiple_archetype_params(SemanticContext *ctx, SourceLoc loc, const char *proc_name);
SemDiag *sem_emit_handle_in_map_param(SemanticContext *ctx, SourceLoc loc, const char *name);
SemDiag *sem_emit_unknown_query(SemanticContext *ctx, SourceLoc loc, const char *map_name, const char *query_name);
SemDiag *sem_emit_run_targets_query(SemanticContext *ctx, SourceLoc loc, const char *name);
SemDiag *sem_emit_entity_missing_column(SemanticContext *ctx, SourceLoc loc, const char *type_name, const char *col);
SemDiag *sem_emit_entity_unknown_column(SemanticContext *ctx, SourceLoc loc, const char *type_name, const char *col);
SemDiag *sem_emit_entity_unknown_type(SemanticContext *ctx, SourceLoc loc, const char *name);
SemDiag *sem_emit_positional_insert(SemanticContext *ctx, SourceLoc loc, const char *pool);
SemDiag *sem_emit_proc_under_applied(SemanticContext *ctx, SourceLoc loc, const char *name);
SemDiag *sem_emit_eff_extern_not_static(SemanticContext *ctx, SourceLoc loc);
SemDiag *sem_emit_lint_unused_query(SemanticContext *ctx, SourceLoc loc, const char *name, const char *module_path);
SemDiag *sem_emit_lint_map_writes_foreign_pool(SemanticContext *ctx, SourceLoc loc, const char *name);
SemDiag *sem_emit_each_field_filter_type_not_name(SemanticContext *ctx, SourceLoc loc);
SemDiag *sem_emit_each_field_filter_type_not_primitive(SemanticContext *ctx, SourceLoc loc);
SemDiag *sem_emit_each_field_invalid_rhs(SemanticContext *ctx, SourceLoc loc, const char *name);
SemDiag *sem_emit_underscore_not_inout(SemanticContext *ctx, SourceLoc loc);
SemDiag *sem_emit_local_shadows_callable(SemanticContext *ctx, SourceLoc loc, const char *name);

SemDiag *sem_emit_alloc_not_at_top(SemanticContext *ctx, SourceLoc loc);
SemDiag *sem_emit_alloc_count_not_literal(SemanticContext *ctx, SourceLoc loc);
SemDiag *sem_emit_shape_already_allocated(SemanticContext *ctx, SourceLoc loc, const char *name);
SemDiag *sem_emit_duplicate_component(SemanticContext *ctx, SourceLoc loc, const char *name);
SemDiag *sem_emit_component_redefined(SemanticContext *ctx, SourceLoc loc, const char *name);

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

SemDiag *sem_emit_constant_redefined(SemanticContext *ctx, SourceLoc loc, const char *name);
SemDiag *sem_emit_const_value_is_type(SemanticContext *ctx, SourceLoc loc, const char *name, const char *rhs);
SemDiag *sem_emit_unknown_const_value(SemanticContext *ctx, SourceLoc loc, const char *rhs, const char *name);
SemDiag *sem_emit_const_rhs_invalid(SemanticContext *ctx, SourceLoc loc);

SemDiag *sem_emit_func_not_pure(SemanticContext *ctx, SourceLoc loc, const char *name, const char *reason);
SemDiag *sem_emit_insert_delete_outlist(SemanticContext *ctx, SourceLoc loc, const char *name, const char *form);

SemDiag *sem_emit_policy_provable_oob(SemanticContext *ctx, SourceLoc loc, const char *base, int idx, int len);
SemDiag *sem_emit_policy_func_aborts(SemanticContext *ctx, SourceLoc loc, const char *func);
SemDiag *sem_emit_duplicate_default(SemanticContext *ctx, SourceLoc loc, const char *kind, const char *category);
SemDiag *sem_emit_default_invalid(SemanticContext *ctx, SourceLoc loc, const char *msg);
SemDiag *sem_emit_policy_unknown(SemanticContext *ctx, SourceLoc loc, const char *name);
SemDiag *sem_emit_policy_wrong_sigil(SemanticContext *ctx, SourceLoc loc, const char *name, int want_handler);
SemDiag *sem_emit_policy_wrong_category(SemanticContext *ctx, SourceLoc loc, const char *name, const char *want,
                                        const char *got);
SemDiag *sem_emit_policy_abort_forbidden(SemanticContext *ctx, SourceLoc loc, const char *which, const char *flag);
SemDiag *sem_emit_policy_undefined_forbidden(SemanticContext *ctx, SourceLoc loc);
SemDiag *sem_emit_policy_uses_undefined(SemanticContext *ctx, SourceLoc loc, const char *policy);
SemDiag *sem_emit_cyclic_policy(SemanticContext *ctx, SourceLoc loc, const char *policy);
SemDiag *sem_emit_allow_forbidden(SemanticContext *ctx, SourceLoc loc, const char *slug);

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
SemDiag *sem_emit_extern_multi_out(SemanticContext *ctx, SourceLoc loc, const char *proc_name, int n_out_only);

/* Tycheck (P3 type-check pass). E0200 is the general type_mismatch — every
 * typing-rule failure routes here. The `where` string describes the constraint
 * source (e.g. "if condition", "argument 2 of 'foo'", "return value"). */
SemDiag *sem_emit_type_mismatch(SemanticContext *ctx, SourceLoc loc, const char *where, const char *expected,
                                const char *got);
SemDiag *sem_emit_not_indexable(SemanticContext *ctx, SourceLoc loc, const char *base_type);
SemDiag *sem_emit_wrong_arity(SemanticContext *ctx, SourceLoc loc, const char *name, int expected, int got);
SemDiag *sem_emit_wrong_return_arity(SemanticContext *ctx, SourceLoc loc, const char *fn_name, int expected, int got);
SemDiag *sem_emit_break_outside_loop(SemanticContext *ctx, SourceLoc loc);
SemDiag *sem_emit_continue_outside_loop(SemanticContext *ctx, SourceLoc loc);
SemDiag *sem_emit_module_no_member(SemanticContext *ctx, SourceLoc loc, const char *module, const char *member);
SemDiag *sem_emit_duplicate_decl(SemanticContext *ctx, SourceLoc loc, const char *kind, const char *name);
SemDiag *sem_emit_drop_invalid(SemanticContext *ctx, SourceLoc loc, const char *msg);
SemDiag *sem_emit_drop_redefined(SemanticContext *ctx, SourceLoc loc, const char *type_name);
SemDiag *sem_emit_drop_conditional(SemanticContext *ctx, SourceLoc loc, const char *name);
SemDiag *sem_emit_duplicate_region(SemanticContext *ctx, SourceLoc loc, const char *region);
SemDiag *sem_emit_opaque_overwrite(SemanticContext *ctx, SourceLoc loc, const char *opaque_type);
SemDiag *sem_emit_opaque_construct(SemanticContext *ctx, SourceLoc loc, const char *opaque_type);

/* Lints */
SemDiag *sem_emit_lint_proc_could_be_func(SemanticContext *ctx, SourceLoc loc, const char *name);
SemDiag *sem_emit_lint_proc_no_effect(SemanticContext *ctx, SourceLoc loc, const char *name);
SemDiag *sem_emit_lint_func_impure(SemanticContext *ctx, SourceLoc loc, const char *name, const char *reason);
SemDiag *sem_emit_lint_unused_local(SemanticContext *ctx, SourceLoc loc, const char *name);
SemDiag *sem_emit_lint_unused_use(SemanticContext *ctx, SourceLoc loc, const char *name);
SemDiag *sem_emit_lint_inout_redundant_arg(SemanticContext *ctx, SourceLoc loc, const char *name);
SemDiag *sem_emit_lint_inout_param_shadow(SemanticContext *ctx, SourceLoc loc, const char *name);
SemDiag *sem_emit_lint_inout_param_shadow_cabi(SemanticContext *ctx, SourceLoc loc, const char *name);
SemDiag *sem_emit_lint_unused_function(SemanticContext *ctx, SourceLoc loc, const char *name, const char *module_path);
SemDiag *sem_emit_lint_unused_static_const(SemanticContext *ctx, SourceLoc loc, const char *kind, const char *name,
                                           const char *module_path);
SemDiag *sem_emit_lint_unused_enum(SemanticContext *ctx, SourceLoc loc, const char *name, const char *module_path);
SemDiag *sem_emit_lint_discarded_ok(SemanticContext *ctx, SourceLoc loc, const char *name);
SemDiag *sem_emit_lint_raw_pool_index(SemanticContext *ctx, SourceLoc loc, const char *arch);
SemDiag *sem_emit_lint_policy_on_safe_op(SemanticContext *ctx, SourceLoc loc, const char *name, const char *base);
SemDiag *sem_emit_lint_redundant_guard(SemanticContext *ctx, SourceLoc loc, const char *var);
SemDiag *sem_emit_lint_handler_foreign_arch(SemanticContext *ctx, SourceLoc loc, const char *handler,
                                            const char *foreign, const char *target);
SemDiag *sem_emit_lint_func_could_be_const(SemanticContext *ctx, SourceLoc loc, const char *name);
SemDiag *sem_emit_lint_exported_mutable_global(SemanticContext *ctx, SourceLoc loc, const char *name);
SemDiag *sem_emit_lint_outarg_shadows_outparam(SemanticContext *ctx, SourceLoc loc, const char *name);
SemDiag *sem_emit_lint_large_stack_array(SemanticContext *ctx, SourceLoc loc, const char *name, int size_bytes);
SemDiag *sem_emit_lint_pointless_move(SemanticContext *ctx, SourceLoc loc);
SemDiag *sem_emit_lint_proc_calls_proc(SemanticContext *ctx, SourceLoc loc, const char *callee);
SemDiag *sem_emit_lint_pool_index_outside_query(SemanticContext *ctx, SourceLoc loc, const char *pool);

#endif /* SEM_DIAGNOSTICS_H */
