#ifndef SEMANTIC_H
#define SEMANTIC_H

#include "../syntax/syntax_view.h"
#include "../syntax/type_ref.h"
#include "sem_hints.h"
#include "sem_model.h"

/* Semantic analysis context */
typedef struct SemanticContext SemanticContext;

/* Create and analyze a program. Collects the resolved DeclSummary table directly from the lossless
 * syntax tree (+ registered module syntax trees) via sem_collect_decls, analyzes it, and keys the
 * side model by syntax tree node id (read by lowering). This is the only analysis entry. */
SemanticContext *semantic_analyze_cst(const SyntaxNode *root, const char *src);

/* Whole-program datasheet-storage validation (a device's `[N]Shape` requirement vs the driver's pool).
 * Run by the COMPILER frontend after `semantic_analyze_cst`, never by the per-file analyzer — it needs the
 * whole assembled program. Returns the number of errors emitted (also added to the context's error count). */
int semantic_check_storage_requirements(SemanticContext *ctx);

/* Register a `use`-module's syntax tree so semantic_analyze_cst can inline it (parallel to
 * lower_add_module). Call once per module before semantic_analyze_cst. */
void semantic_add_module(const char *name, const SyntaxNode *root, const char *src, const char *filename,
                         DeclOrigin origin);
/* Clear registered modules (static registry; reset at the start of each compilation). */
void semantic_reset_modules(void);

/* Collect the deduped system-library names from every `#link { "lib" ... }` region across `root`
 * (the driver) plus all registered modules (= only the selected variant's files, since the resolver
 * overlays just the active variant). Names are written into out[][64]; returns the count, or -1 if a
 * name contains a char outside [A-Za-z0-9._-] (a hard fail — these flow into the cc `-l` link line).
 * The compiler calls this after the front-end to append `-l<name>` to its link command. */
int semantic_collect_link_libs(const SyntaxNode *root, const char *root_src, char out[][64], int cap);

/* Editor-only: also inline module `name` into the root namespace even when the root has no `#import`
 * for it (NULL clears). Set by the analyzer when the open document is a member of a device folder, so
 * its sibling datasheet's global type vocabulary resolves; the compiler never sets it. Call after
 * registering the module's files, before semantic_analyze_cst. Reset by semantic_reset_modules. */
void semantic_set_extra_inline_module(const char *name);
/* 1 if a module of this name is currently registered (used to detect import-not-found). */
int semantic_has_module(const char *name);

void semantic_context_free(SemanticContext *ctx);

/* The resolved-type side model (keyed by syntax tree node id); read by lowering. */
SemModel *sem_context_model(SemanticContext *ctx);

/* The interned TypeId arena (Phase 3); read by lowering's map_type_id. */
TypeArena *sem_context_arena(SemanticContext *ctx);

/* Editor-facing inferred facts (keyed by syntax tree node id); read by the analyzer. */
SemHints *sem_context_hints(SemanticContext *ctx);

/* Resolve a (possibly nominal-alias) type name through the alias chain to its
 * backing; returns `name` unchanged if not an alias. */
const char *semantic_resolve_type_alias(SemanticContext *ctx, const char *name);

/* One-step backing of an alias (`handle :: win` → "win"), NULL if not an alias. Walk this to recognise
 * intermediate ancestors — a distinct subtype is usable AS any type in its backing chain, not only the
 * ultimate backing. */
const char *semantic_alias_backing_step(SemanticContext *ctx, const char *name);

/* 1 if `name` is a registered type alias (tier-1 transparent OR tier-2 subtype). */
int semantic_is_type_alias(SemanticContext *ctx, const char *name);

/* 1 if `name` is a TRANSPARENT (tier-1) alias — same type identity as its backing. A tier-2
 * distinct subtype (the default `name :: T`) or a non-alias name returns 0. */
int semantic_alias_is_transparent(SemanticContext *ctx, const char *name);

/* If `name` is a callable alias (`handler :: some_proc`), the ultimate proc/func target name;
 * else NULL. Lowering rewrites call callees through this and drops the alias binding. */
const char *semantic_resolve_callable_alias(SemanticContext *ctx, const char *name);

/* If `name` is a named callable-type alias (`handler :: proc()(w:int)`), its callable TypeId (the
 * structural signature); else TYID_UNKNOWN. tycheck resolves the name through this. */
TypeId semantic_callable_type_alias(SemanticContext *ctx, const char *name);

/* Enum support (used by lowering to resolve `Enum.variant` and bare variant patterns to int values). */
int semantic_is_enum_type(SemanticContext *ctx, const char *name);
int semantic_func_is_ctfe_only(SemanticContext *ctx, const char *name); /* sum-typed func → erased at lowering */
int semantic_enum_variant_value(SemanticContext *ctx, const char *enum_name, const char *variant, long *out);
int semantic_find_enum_variant(SemanticContext *ctx, const char *variant, long *out);

/* LSP goto navigation: turn a resolved decl index (from a DefId, a type name, or a @drop registry)
 * into a source location. `semantic_decl_src_file` gives the decl's file (NULL = entry buffer, caller
 * splits core vs user by line); `semantic_decl_at(ctx, i)->loc` gives line/col in that file. */
const char *semantic_decl_src_file(const SemanticContext *ctx, int index);
const char *semantic_module_file(const char *name);
int semantic_find_type_decl_index(const SemanticContext *ctx, const char *name);
int semantic_drop_proc_decl_index(const SemanticContext *ctx, const char *type_name);

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

/* CTFE: fold `e` to a compile-time integer constant (literal / const / pure-func-of-constants).
 * Returns 1 and writes *out on success, 0 if `e` is not a compile-time-constant integer. */
int semantic_try_const_int(SemanticContext *ctx, SyntaxView e, int *out);

/* Value-CTFE: fold a `#run` Schedule expression to a constant ScheduleTree (the runtime-dispatch IR),
 * or NULL if it doesn't fold. Owns the returned tree (caller frees via schedule_tree_free). */
struct ScheduleTree *semantic_try_const_schedule(SemanticContext *ctx, SyntaxView e);

/* Lint configuration. Both lints are enabled by default. CLI flags
 * (--Wno-proc-could-be-func / --Wno-proc-no-effect) disable them;
 * --Werror=proc-could-be-func / --Werror=proc-no-effect promote each
 * warning to a hard error. */
void semantic_set_lint_proc_could_be_func(int enabled, int werror);
void semantic_set_lint_proc_no_effect(int enabled, int werror);
/* W0022 exported_mutable_global: error by default (see ensure_init). `--exported-mutable=error|warn|allow`
 * maps to (enabled, werror) = (1,1) / (1,0) / (0,0). */
void semantic_set_lint_exported_mutable_global(int enabled, int werror);
/* W0024 map_writes_foreign_pool: error by default (see ensure_init). `--map-foreign-write=error|warn|allow`
 * maps to (enabled, werror) = (1,1) / (1,0) / (0,0). */
void semantic_set_lint_map_writes_foreign_pool(int enabled, int werror);
/* W0028 proc_calls_proc: WARN by default (the flat-effect proc→proc ban). `--proc-leaf=error|warn|allow`
 * maps to (enabled, werror) = (1,1) / (1,0) / (0,0). */
void semantic_set_lint_proc_calls_proc(int enabled, int werror);
/* W0030 proc_not_primitive: the proc-elimination ban (a proc must be `#foreign`/primitive). DISABLED by
 * default during the migration; enable (then werror) to enforce once stdlib is converted. */
void semantic_set_lint_proc_not_primitive(int enabled, int werror);
/* W0029 pool_index_outside_query: WARN by default (pool values must come from a query, not `Pool.col[i]`).
 * `--pool-index=error|warn|allow` maps to (1,1) / (1,0) / (0,0). */
void semantic_set_lint_pool_index_outside_query(int enabled, int werror);
/* W0026 large_stack_array: warn by default. `-Wno-large-stack-array` disables; `-Werror=large-stack-array`
 * (or bare `-Werror`) promotes to a hard error. */
void semantic_set_lint_large_stack_array(int enabled, int werror);

/* Crash-free enforcement (failure policies). Set from the CLI before analysis; consulted by the
 * failure-policy pass. --no-abort: any op resolving to `!abort` (implicit or explicit) is an error;
 * --no-implicit-abort: only the default/implicit `!abort` errors. `!undefined` is rejected by default;
 * --allow-undefined opts back in. */
void semantic_set_no_abort(int on);
void semantic_set_no_implicit_abort(int on);
void semantic_set_allow_undefined(int on);

/* `-Werror` (bare): promote every enabled lint to a hard error. */
void semantic_set_all_lints_werror(int werror);
/* `--forbid-allow`: any `@allow(...)` in user code is a hard error (no lint escape hatches). */
void semantic_set_forbid_allow(int on);

#endif /* SEMANTIC_H */
