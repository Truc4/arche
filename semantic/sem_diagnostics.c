#include "sem_diagnostics.h"
#include "sem_diag_internal.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

/* C99 doesn't declare strdup — but we don't use it here; vsnprintf-into-malloc
 * gives us owned strings directly. */

/* ========== Registry table ==========
 *
 * One row per SemDiagKind, in the same order as the enum. Edit both together.
 * Severity here is the *default*; runtime overrides for lints live in the arrays
 * below (`g_enabled` / `g_werror`). For error-class kinds the default is locked. */

typedef enum {
	CLASS_ERROR, /* stderr: "Semantic error at line X, col Y: ..." */
	CLASS_LINT,  /* stderr: "Lint warning [slug] at line X, col Y: ..." */
} DiagClass;

typedef struct {
	const char *code; /* "E0001" / "W0001" — stable forever (burn-on-delete) */
	const char *slug; /* "undefined_symbol" / "proc_no_effect" */
	DiagClass class;
	int default_enabled; /* lints can be off by default; ignored for errors */
} SemDiagDesc;

/* clang-format off */
static const SemDiagDesc g_table[SEM_DIAG_KIND_COUNT] = {
	/* Name resolution */
	[SEM_DIAG_undefined_symbol]              = { "E0001", "undefined_symbol",              CLASS_ERROR, 1 },
	[SEM_DIAG_use_after_consume]             = { "E0002", "use_after_consume",             CLASS_ERROR, 1 },
	[SEM_DIAG_undefined_field_base]          = { "E0003", "undefined_field_base",          CLASS_ERROR, 1 },
	[SEM_DIAG_undefined_archetype_alloc]     = { "E0004", "undefined_archetype_alloc",     CLASS_ERROR, 1 },
	[SEM_DIAG_undefined_archetype_for]       = { "E0005", "undefined_archetype_for",       CLASS_ERROR, 1 },
	[SEM_DIAG_undefined_archetype_bind]      = { "E0006", "undefined_archetype_bind",      CLASS_ERROR, 1 },

	/* Type system */
	[SEM_DIAG_type_alias_redefined]          = { "E0010", "type_alias_redefined",          CLASS_ERROR, 1 },
	[SEM_DIAG_type_alias_unknown_backing]    = { "E0011", "type_alias_unknown_backing",    CLASS_ERROR, 1 },
	[SEM_DIAG_local_alias_invalid_backing]   = { "E0012", "local_alias_invalid_backing",   CLASS_ERROR, 1 },
	[SEM_DIAG_alias_backing_invalid]         = { "E0013", "alias_backing_invalid",         CLASS_ERROR, 1 },
	[SEM_DIAG_extern_type_mismatch]          = { "E0014", "extern_type_mismatch",          CLASS_ERROR, 1 },
	[SEM_DIAG_no_implicit_conversion]        = { "E0015", "no_implicit_conversion",        CLASS_ERROR, 1 },
	[SEM_DIAG_tuple_field_not_simple]        = { "E0016", "tuple_field_not_simple",        CLASS_ERROR, 1 },
	[SEM_DIAG_const_type_mismatch]           = { "E0017", "const_type_mismatch",           CLASS_ERROR, 1 },
	[SEM_DIAG_meta_type_invalid_position]    = { "E0018", "meta_type_invalid_position",    CLASS_ERROR, 1 },

	/* Ownership */
	[SEM_DIAG_opaque_not_consumed]           = { "E0020", "opaque_not_consumed",           CLASS_ERROR, 1 },
	[SEM_DIAG_cannot_copy_opaque]            = { "E0021", "cannot_copy_opaque",            CLASS_ERROR, 1 },
	[SEM_DIAG_cannot_move_borrowed]          = { "E0022", "cannot_move_borrowed",          CLASS_ERROR, 1 },
	[SEM_DIAG_copy_unsupported]              = { "E0023", "copy_unsupported",              CLASS_ERROR, 1 },
	[SEM_DIAG_assign_after_move]             = { "E0024", "assign_after_move",             CLASS_ERROR, 1 },
	[SEM_DIAG_own_requires_move_or_copy]     = { "E0025", "own_requires_move_or_copy",     CLASS_ERROR, 1 },
	[SEM_DIAG_cannot_mutate_borrowed]        = { "E0026", "cannot_mutate_borrowed",        CLASS_ERROR, 1 },
	[SEM_DIAG_extern_array_param_needs_own]  = { "E0027", "extern_array_param_needs_own",  CLASS_ERROR, 1 },
	[SEM_DIAG_proc_return_has_value]         = { "E0028", "proc_return_has_value",         CLASS_ERROR, 1 },
	[SEM_DIAG_sys_no_return]                 = { "E0029", "sys_no_return",                 CLASS_ERROR, 1 },

	/* Field / component */
	[SEM_DIAG_no_field]                      = { "E0030", "no_field",                      CLASS_ERROR, 1 },
	[SEM_DIAG_cannot_read_through_handle]    = { "E0031", "cannot_read_through_handle",    CLASS_ERROR, 1 },
	[SEM_DIAG_not_archetype_instance]        = { "E0032", "not_archetype_instance",        CLASS_ERROR, 1 },

	/* Declaration shape */
	[SEM_DIAG_archetype_only_as_param]       = { "E0033", "archetype_only_as_param",       CLASS_ERROR, 1 },
	[SEM_DIAG_archetype_not_return_type]     = { "E0034", "archetype_not_return_type",     CLASS_ERROR, 1 },
	[SEM_DIAG_archetype_funcs_only]          = { "E0035", "archetype_funcs_only",          CLASS_ERROR, 1 },
	[SEM_DIAG_multiple_archetype_params]     = { "E0036", "multiple_archetype_params",     CLASS_ERROR, 1 },
	[SEM_DIAG_handle_in_sys_param]           = { "E0037", "handle_in_sys_param",           CLASS_ERROR, 1 },
	[SEM_DIAG_each_field_filter_type_not_name]      = { "E0038", "each_field_filter_not_name",      CLASS_ERROR, 1 },
	[SEM_DIAG_each_field_filter_type_not_primitive] = { "E0039", "each_field_filter_not_primitive", CLASS_ERROR, 1 },
	[SEM_DIAG_each_field_invalid_rhs]        = { "E0040", "each_field_invalid_rhs",        CLASS_ERROR, 1 },

	/* Allocation / archetype */
	[SEM_DIAG_alloc_not_at_top]              = { "E0041", "alloc_not_at_top",              CLASS_ERROR, 1 },
	[SEM_DIAG_alloc_count_not_literal]       = { "E0042", "alloc_count_not_literal",       CLASS_ERROR, 1 },
	[SEM_DIAG_shape_already_allocated]       = { "E0043", "shape_already_allocated",       CLASS_ERROR, 1 },
	[SEM_DIAG_duplicate_component]           = { "E0044", "duplicate_component",           CLASS_ERROR, 1 },
	[SEM_DIAG_component_redefined]           = { "E0045", "component_redefined",           CLASS_ERROR, 1 },

	/* Calls / groups */
	[SEM_DIAG_action_in_expression]          = { "E0050", "action_in_expression",          CLASS_ERROR, 1 },
	[SEM_DIAG_no_group_match]                = { "E0051", "no_group_match",                CLASS_ERROR, 1 },
	[SEM_DIAG_ambiguous_group_call]          = { "E0052", "ambiguous_group_call",          CLASS_ERROR, 1 },
	[SEM_DIAG_unknown_group_member]          = { "E0053", "unknown_group_member",          CLASS_ERROR, 1 },
	[SEM_DIAG_group_member_extern]           = { "E0054", "group_member_extern",           CLASS_ERROR, 1 },
	[SEM_DIAG_group_member_not_declared]     = { "E0055", "group_member_not_declared",     CLASS_ERROR, 1 },
	[SEM_DIAG_duplicate_group_signatures]    = { "E0056", "duplicate_group_signatures",    CLASS_ERROR, 1 },
	[SEM_DIAG_group_name_collision]          = { "E0057", "group_name_collision",          CLASS_ERROR, 1 },
	[SEM_DIAG_empty_group]                   = { "E0058", "empty_group",                   CLASS_ERROR, 1 },

	/* Extern / signature */
	[SEM_DIAG_extern_proc_bad_type]          = { "E0060", "extern_proc_bad_type",          CLASS_ERROR, 1 },
	[SEM_DIAG_extern_func_bad_type]          = { "E0061", "extern_func_bad_type",          CLASS_ERROR, 1 },
	[SEM_DIAG_extern_func_bad_return]        = { "E0062", "extern_func_bad_return",        CLASS_ERROR, 1 },

	/* Constants */
	[SEM_DIAG_constant_redefined]            = { "E0080", "constant_redefined",            CLASS_ERROR, 1 },
	[SEM_DIAG_const_value_is_type]           = { "E0081", "const_value_is_type",           CLASS_ERROR, 1 },
	[SEM_DIAG_unknown_const_value]           = { "E0082", "unknown_const_value",           CLASS_ERROR, 1 },
	[SEM_DIAG_const_rhs_invalid]             = { "E0083", "const_rhs_invalid",             CLASS_ERROR, 1 },

	/* Purity */
	[SEM_DIAG_func_not_pure]                 = { "E0090", "func_not_pure",                 CLASS_ERROR, 1 },

	/* Assignment targets */
	[SEM_DIAG_assign_to_const]               = { "E0091", "assign_to_const",               CLASS_ERROR, 1 },
	[SEM_DIAG_assign_to_undeclared]          = { "E0092", "assign_to_undeclared",          CLASS_ERROR, 1 },

	/* Front-end (parse + module) */
	[SEM_DIAG_parse_error]                   = { "E0100", "parse_error",                   CLASS_ERROR, 1 },
	[SEM_DIAG_module_not_found]              = { "E0101", "module_not_found",              CLASS_ERROR, 1 },
	[SEM_DIAG_module_parse_failed]           = { "E0102", "module_parse_failed",           CLASS_ERROR, 1 },

	/* Phase 2 Tier 1 — append-only codes E0110+ / W0004+. */
	[SEM_DIAG_binop_type_mismatch]           = { "E0110", "binop_type_mismatch",           CLASS_ERROR, 1 },
	[SEM_DIAG_field_on_non_archetype]        = { "E0111", "field_on_non_archetype",        CLASS_ERROR, 1 },
	[SEM_DIAG_move_outside_arg]              = { "E0112", "move_outside_arg",              CLASS_ERROR, 1 },
	[SEM_DIAG_underscore_not_inout]          = { "E0115", "underscore_not_inout",          CLASS_ERROR, 1 },
	/* E0113 free_non_opaque, E0114 double_free — retired (zero runtime alloc, no free stmt). */
	[SEM_DIAG_extern_proc_bad_return]        = { "E0115", "extern_proc_bad_return",        CLASS_ERROR, 1 },
	/* E0116 revived: local_shadows_callable (was out_not_written, retired). */
	[SEM_DIAG_local_shadows_callable]        = { "E0116", "local_shadows_callable",        CLASS_ERROR, 1 },
	[SEM_DIAG_duplicate_decl]                = { "E0117", "duplicate_decl",                CLASS_ERROR, 1 },
	[SEM_DIAG_drop_invalid]                  = { "E0118", "drop_invalid",                  CLASS_ERROR, 1 },
	[SEM_DIAG_drop_redefined]                = { "E0119", "drop_redefined",                CLASS_ERROR, 1 },
	[SEM_DIAG_drop_conditional]              = { "E0120", "drop_conditional",              CLASS_ERROR, 1 },
	[SEM_DIAG_duplicate_region]              = { "E0121", "duplicate_region",              CLASS_ERROR, 1 },

	/* Tycheck (P3 type-check pass — E0200+). All typing-rule violations route through
	 * E0200; sharper kind/arity constraints get their own codes in Phase B. */
	[SEM_DIAG_break_outside_loop]            = { "E0030", "break_outside_loop",            CLASS_ERROR, 1 },
	[SEM_DIAG_continue_outside_loop]         = { "E0093", "continue_outside_loop",         CLASS_ERROR, 1 },
	[SEM_DIAG_module_no_member]              = { "E0094", "module_no_member",              CLASS_ERROR, 1 },
	[SEM_DIAG_type_mismatch]                 = { "E0200", "type_mismatch",                 CLASS_ERROR, 1 },
	[SEM_DIAG_not_indexable]                 = { "E0201", "not_indexable",                 CLASS_ERROR, 1 },
	[SEM_DIAG_wrong_arity]                   = { "E0203", "wrong_arity",                   CLASS_ERROR, 1 },
	[SEM_DIAG_non_exhaustive_match]          = { "E0210", "non_exhaustive_match",          CLASS_ERROR, 1 },
	[SEM_DIAG_callable_in_archetype]         = { "E0211", "callable_in_archetype",         CLASS_ERROR, 1 },
	[SEM_DIAG_wildcard_in_enum_match]        = { "E0212", "wildcard_in_enum_match",        CLASS_ERROR, 1 },

	/* Lints */
	[SEM_LINT_proc_could_be_func]            = { "W0001", "proc_could_be_func",            CLASS_LINT, 1 },
	[SEM_LINT_proc_no_effect]                = { "W0002", "proc_no_effect",                CLASS_LINT, 1 },
	[SEM_LINT_func_impure]                   = { "W0003", "func_impure",                   CLASS_LINT, 1 },
	[SEM_LINT_unused_local]                  = { "W0004", "unused_local",                  CLASS_LINT, 1 },
	[SEM_LINT_unused_use]                    = { "W0010", "unused_use",                    CLASS_LINT, 1 },
	[SEM_LINT_inout_redundant_arg]           = { "W0011", "inout_redundant_arg",           CLASS_LINT, 1 },
	[SEM_LINT_inout_param_shadow]            = { "W0012", "inout_param_shadow",            CLASS_LINT, 1 },
	[SEM_LINT_unused_function]               = { "W0013", "unused_function",               CLASS_LINT, 1 },
	[SEM_LINT_unused_static_const]           = { "W0014", "unused_static_const",           CLASS_LINT, 1 },
	[SEM_LINT_unused_enum]                   = { "W0015", "unused_enum",                   CLASS_LINT, 1 },
};
/* clang-format on */

/* Runtime config: only lints are mutable (errors are locked at default). Lazily
 * initialized from the table on first access so the global state is correct
 * regardless of static-init order. */
static uint8_t g_enabled[SEM_DIAG_KIND_COUNT];
static uint8_t g_werror[SEM_DIAG_KIND_COUNT];
static int g_init_done = 0;

static void ensure_init(void) {
	if (g_init_done)
		return;
	for (int i = 0; i < SEM_DIAG_KIND_COUNT; i++) {
		g_enabled[i] = (uint8_t)g_table[i].default_enabled;
		g_werror[i] = 0;
	}
	g_init_done = 1;
}

const char *sem_diag_code(SemDiagKind kind) {
	if (kind < 0 || kind >= SEM_DIAG_KIND_COUNT)
		return "E????";
	return g_table[kind].code;
}

const char *sem_diag_slug(SemDiagKind kind) {
	if (kind < 0 || kind >= SEM_DIAG_KIND_COUNT)
		return "unknown";
	return g_table[kind].slug;
}

void semantic_set_diag(SemDiagKind kind, int enabled, int werror) {
	ensure_init();
	if (kind < 0 || kind >= SEM_DIAG_KIND_COUNT)
		return;
	if (g_table[kind].class != CLASS_LINT)
		return; /* hard errors aren't lint-configurable; ignore silently */
	g_enabled[kind] = enabled ? 1 : 0;
	g_werror[kind] = werror ? 1 : 0;
}

int semantic_diag_enabled(SemDiagKind kind) {
	ensure_init();
	if (kind < 0 || kind >= SEM_DIAG_KIND_COUNT)
		return 0;
	return g_enabled[kind];
}

/* CLI line-coord offset (subtracted from .line before stderr printing). main.c
 * sets this to core.arche's newline count so user code shows user-line numbers.
 * 0 (the default) means no translation, used by the warm analyzer which does its
 * own coord translation in arche_analyzer.c. */
static int g_print_line_offset = 0;

void semantic_set_print_line_offset(int offset) {
	g_print_line_offset = offset;
}
int semantic_print_line_offset(void) {
	return g_print_line_offset;
}

int semantic_diag_werror(SemDiagKind kind) {
	ensure_init();
	if (kind < 0 || kind >= SEM_DIAG_KIND_COUNT)
		return 0;
	return g_werror[kind];
}

/* ========== Dispatcher ==========
 *
 * `sem_emit_v` takes a va_list (so wrappers can forward varargs cleanly) and
 * does: severity resolution, vsnprintf-into-heap, byte-stable stderr print
 * (matching the legacy error_at / lint_emit formats), and diag_push.
 *
 * `sem_emit_` is the variadic public-internal entry point — its format-printf
 * attribute is what makes `-Wformat` check the format string against the args
 * at every wrapper callsite. Wrappers always pass a literal format. */

static SemDiag *sem_emit_v(SemanticContext *ctx, SemDiagKind kind, SourceLoc loc, const char *fmt, va_list ap) {
	ensure_init();
	if (kind < 0 || kind >= SEM_DIAG_KIND_COUNT)
		return NULL;
	const SemDiagDesc *desc = &g_table[kind];

	/* Lint suppression: disabled globally → silent; `@allow(<slug>)` on the
	 * currently-analyzed decl → silent; `-Werror=<slug>` promotes to error.
	 * Hard errors fire unconditionally — neither `-Wno-` nor `@allow()` can
	 * silence them (the registry encodes that). */
	int severity;
	if (desc->class == CLASS_LINT) {
		if (!g_enabled[kind])
			return NULL;
		if (sem_diag_slug_suppressed(ctx, desc->slug))
			return NULL;
		severity = g_werror[kind] ? 1 : 0;
	} else {
		severity = 1;
	}

	/* vsnprintf into an owned heap buffer: two-pass with NULL size to size it. */
	va_list aq;
	va_copy(aq, ap);
	int needed = vsnprintf(NULL, 0, fmt, aq);
	va_end(aq);
	if (needed < 0)
		return NULL;
	char *msg = malloc((size_t)needed + 1);
	if (!msg)
		return NULL;
	vsnprintf(msg, (size_t)needed + 1, fmt, ap);

	/* Byte-stable stderr — must match the legacy error_at / lint_emit formats so
	 * the Phase 1 golden-file check passes through the migration unchanged.
	 * Line numbers are translated by g_print_line_offset when set (main.c does so
	 * after computing core.arche's newline count). */
	int has_loc = loc.line != 0;
	int print_line = loc.line - g_print_line_offset;
	if (print_line < 1)
		print_line = loc.line; /* error inside the core region — show raw line */
	if (desc->class == CLASS_ERROR) {
		if (has_loc)
			fprintf(stderr, "Semantic error at line %d, col %d: %s\n", print_line, loc.column, msg);
		else
			fprintf(stderr, "Semantic error: %s\n", msg);
	} else {
		const char *kind_word = severity ? "error" : "warning";
		fprintf(stderr, "Lint %s [%s] at line %d, col %d: %s\n", kind_word, desc->slug, print_line, loc.column, msg);
	}
	fflush(stderr);

	SemDiag *d = diag_push(ctx, severity, has_loc, loc, desc->code, desc->slug, msg);
	free(msg);
	return d;
}

#ifdef __GNUC__
__attribute__((format(printf, 4, 5)))
#endif
static SemDiag *sem_emit_(SemanticContext *ctx, SemDiagKind kind, SourceLoc loc, const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	SemDiag *d = sem_emit_v(ctx, kind, loc, fmt, ap);
	va_end(ap);
	return d;
}

/* ========== Escape-hatch builder (reserved for rich-shape diagnostics) ==========
 *
 * Phase 1 ships the API surface but has no caller. First real user will be
 * `E0053_field_on_non_archetype` (P2b) with a did-you-mean suggestion. The
 * builder keeps the primary message as a `label`-set call and adds spans as
 * additional notes; emission goes through diag_push so the resulting SemDiag
 * is indistinguishable from a wrapper-emitted one. */

SemDiag *sem_diag_open(SemanticContext *ctx, SemDiagKind kind, SourceLoc primary) {
	ensure_init();
	if (kind < 0 || kind >= SEM_DIAG_KIND_COUNT)
		return NULL;
	const SemDiagDesc *desc = &g_table[kind];
	int severity;
	if (desc->class == CLASS_LINT) {
		if (!g_enabled[kind])
			return NULL;
		severity = g_werror[kind] ? 1 : 0;
	} else {
		severity = 1;
	}
	return diag_push(ctx, severity, primary.line != 0, primary, desc->code, desc->slug, "");
}

void sem_diag_label(SemDiag *d, const char *fmt, ...) {
	if (!d)
		return;
	va_list ap;
	va_start(ap, fmt);
	va_list aq;
	va_copy(aq, ap);
	int needed = vsnprintf(NULL, 0, fmt, aq);
	va_end(aq);
	if (needed < 0) {
		va_end(ap);
		return;
	}
	free(d->message);
	d->message = malloc((size_t)needed + 1);
	vsnprintf(d->message, (size_t)needed + 1, fmt, ap);
	va_end(ap);
}

void sem_diag_span(SemDiag *d, SourceLoc loc, const char *fmt, ...) {
	if (!d)
		return;
	va_list ap;
	va_start(ap, fmt);
	va_list aq;
	va_copy(aq, ap);
	int needed = vsnprintf(NULL, 0, fmt, aq);
	va_end(aq);
	if (needed < 0) {
		va_end(ap);
		return;
	}
	char *msg = malloc((size_t)needed + 1);
	vsnprintf(msg, (size_t)needed + 1, fmt, ap);
	va_end(ap);
	d->notes = realloc(d->notes, (size_t)(d->note_count + 1) * sizeof(SemDiagNote));
	d->notes[d->note_count].loc = loc;
	d->notes[d->note_count].message = msg;
	d->note_count++;
}

void sem_diag_emit(SemDiag *d) {
	if (!d)
		return;
	/* Builder-emitted diagnostics go through diag_push at open-time; the byte-
	 * stable stderr print happens here, after label/spans have been set. */
	int has_loc = d->has_loc;
	if (has_loc)
		fprintf(stderr, "Semantic error at line %d, col %d: %s\n", d->loc.line, d->loc.column, d->message);
	else
		fprintf(stderr, "Semantic error: %s\n", d->message);
	for (int i = 0; i < d->note_count; i++) {
		const SemDiagNote *n = &d->notes[i];
		fprintf(stderr, "  note: at line %d, col %d: %s\n", n->loc.line, n->loc.column, n->message);
	}
	fflush(stderr);
}

/* ========== Backward-compat lint setters ==========
 *
 * CLI flag parsing in main.c still calls these by name. They now route through
 * the table-driven config so old spellings keep working unchanged. */

void semantic_set_lint_proc_could_be_func(int enabled, int werror) {
	semantic_set_diag(SEM_LINT_proc_could_be_func, enabled, werror);
}
void semantic_set_lint_proc_no_effect(int enabled, int werror) {
	semantic_set_diag(SEM_LINT_proc_no_effect, enabled, werror);
}
void semantic_set_lint_func_impure(int enabled, int werror) {
	semantic_set_diag(SEM_LINT_func_impure, enabled, werror);
}

/* ========== Typed wrappers ==========
 *
 * One per kind. Each holds the format string as a literal so the call to
 * sem_emit_'s printf-attributed signature triggers -Wformat at the callsite.
 * Add a new diagnostic = add an enum + a table row + one wrapper here. */

/* --- Name resolution --- */

SemDiag *sem_emit_undefined_symbol(SemanticContext *ctx, SourceLoc loc, const char *name) {
	return sem_emit_(ctx, SEM_DIAG_undefined_symbol, loc, "Undefined symbol '%s'", name);
}
SemDiag *sem_emit_use_after_consume(SemanticContext *ctx, SourceLoc loc, const char *name) {
	return sem_emit_(ctx, SEM_DIAG_use_after_consume, loc, "use of consumed handle '%s'", name);
}
SemDiag *sem_emit_undefined_field_base(SemanticContext *ctx, SourceLoc loc, const char *name) {
	return sem_emit_(ctx, SEM_DIAG_undefined_field_base, loc, "Undefined variable '%s'", name);
}
SemDiag *sem_emit_undefined_archetype_alloc(SemanticContext *ctx, SourceLoc loc, const char *name) {
	return sem_emit_(ctx, SEM_DIAG_undefined_archetype_alloc, loc, "Undefined archetype '%s'", name);
}
SemDiag *sem_emit_undefined_archetype_for(SemanticContext *ctx, SourceLoc loc, const char *name) {
	return sem_emit_(ctx, SEM_DIAG_undefined_archetype_for, loc, "For loop iterates over undefined archetype '%s'",
	                 name);
}
SemDiag *sem_emit_undefined_archetype_bind(SemanticContext *ctx, SourceLoc loc, const char *name) {
	return sem_emit_(ctx, SEM_DIAG_undefined_archetype_bind, loc, "Archetype '%s' not defined", name);
}

/* --- Type system --- */

SemDiag *sem_emit_type_alias_redefined(SemanticContext *ctx, SourceLoc loc, const char *name) {
	return sem_emit_(ctx, SEM_DIAG_type_alias_redefined, loc, "type '%s' redefined with a different backing", name);
}
SemDiag *sem_emit_type_alias_unknown_backing(SemanticContext *ctx, SourceLoc loc, const char *name,
                                             const char *backing) {
	return sem_emit_(ctx, SEM_DIAG_type_alias_unknown_backing, loc, "type alias '%s' has unknown backing type '%s'",
	                 name, backing);
}
SemDiag *sem_emit_local_alias_invalid_backing(SemanticContext *ctx, SourceLoc loc) {
	return sem_emit_(ctx, SEM_DIAG_local_alias_invalid_backing, loc,
	                 "a local type alias backing must be a type name or `opaque`");
}
SemDiag *sem_emit_alias_backing_invalid(SemanticContext *ctx, SourceLoc loc) {
	return sem_emit_(ctx, SEM_DIAG_alias_backing_invalid, loc,
	                 "a type alias backing must be a type name, `opaque`, or a tuple");
}
SemDiag *sem_emit_extern_type_mismatch(SemanticContext *ctx, SourceLoc loc, const char *func_name,
                                       const char *param_name, const char *expected, const char *got) {
	return sem_emit_(ctx, SEM_DIAG_extern_type_mismatch, loc,
	                 "type mismatch: '%s' parameter '%s' expects '%s' but got '%s'", func_name, param_name, expected,
	                 got);
}
SemDiag *sem_emit_no_implicit_conversion(SemanticContext *ctx, SourceLoc loc, const char *got, const char *name,
                                         const char *want) {
	return sem_emit_(ctx, SEM_DIAG_no_implicit_conversion, loc,
	                 "cannot bind a %s value to '%s' declared `%s` — arche has no implicit numeric conversion", got,
	                 name, want);
}
SemDiag *sem_emit_tuple_field_not_simple(SemanticContext *ctx, SourceLoc loc) {
	return sem_emit_(ctx, SEM_DIAG_tuple_field_not_simple, loc,
	                 "tuple field must be a simple type (nested tuples are not allowed)");
}
SemDiag *sem_emit_const_type_mismatch(SemanticContext *ctx, SourceLoc loc, const char *name, const char *want,
                                      const char *got) {
	return sem_emit_(ctx, SEM_DIAG_const_type_mismatch, loc,
	                 "constant '%s' is declared `%s` but its value is a %s literal", name, want, got);
}
SemDiag *sem_emit_meta_type_invalid_position(SemanticContext *ctx, SourceLoc loc, const char *where) {
	return sem_emit_(ctx, SEM_DIAG_meta_type_invalid_position, loc,
	                 "the meta-type `type` is only valid as a declaration's type (`name : type : T`); "
	                 "type parameters (generics) are not supported yet (%s)",
	                 where);
}

/* --- Ownership --- */

SemDiag *sem_emit_opaque_not_consumed(SemanticContext *ctx, SourceLoc loc, const char *name) {
	return sem_emit_(ctx, SEM_DIAG_opaque_not_consumed, loc,
	                 "opaque value '%s' not consumed before scope end (move/close/return/insert it)", name);
}
SemDiag *sem_emit_cannot_copy_opaque(SemanticContext *ctx, SourceLoc loc, const char *name) {
	return sem_emit_(ctx, SEM_DIAG_cannot_copy_opaque, loc,
	                 "cannot copy opaque value '%s' — it is move-only; use `move`", name);
}
SemDiag *sem_emit_cannot_move_borrowed(SemanticContext *ctx, SourceLoc loc, const char *name) {
	return sem_emit_(ctx, SEM_DIAG_cannot_move_borrowed, loc,
	                 "cannot move read-only parameter '%s' — it is borrowed, not owned; take "
	                 "it `move` to own it, or copy it into a local",
	                 name);
}
SemDiag *sem_emit_copy_unsupported(SemanticContext *ctx, SourceLoc loc, const char *name) {
	return sem_emit_(ctx, SEM_DIAG_copy_unsupported, loc,
	                 "copy of '%s' is not yet supported (only a local `char[N]` buffer can be "
	                 "copied); copy it into a local first, or use `move`",
	                 name);
}
SemDiag *sem_emit_assign_after_move(SemanticContext *ctx, SourceLoc loc, const char *name) {
	return sem_emit_(ctx, SEM_DIAG_assign_after_move, loc,
	                 "cannot assign to '%s' after it was moved — rebind with `:=`", name);
}
SemDiag *sem_emit_own_requires_move_or_copy(SemanticContext *ctx, SourceLoc loc, const char *arg_name,
                                            const char *param_name, const char *func_name) {
	return sem_emit_(ctx, SEM_DIAG_own_requires_move_or_copy, loc,
	                 "value '%s' must be moved or copied into `own` parameter '%s' of '%s' "
	                 "(write `move %s` or `copy %s`)",
	                 arg_name, param_name, func_name, arg_name, arg_name);
}
SemDiag *sem_emit_cannot_mutate_borrowed(SemanticContext *ctx, SourceLoc loc, const char *name) {
	return sem_emit_(ctx, SEM_DIAG_cannot_mutate_borrowed, loc,
	                 "cannot mutate read-only parameter '%s' — an array/slice parameter is a borrowed "
	                 "(read-only) view by default; to write one, take ownership with `own %s: T[]` and "
	                 "`move` the buffer in, use an in-out out-param, or copy it into a local",
	                 name, name);
}
SemDiag *sem_emit_extern_array_param_needs_own(SemanticContext *ctx, SourceLoc loc, const char *param_name,
                                               const char *proc_name) {
	return sem_emit_(ctx, SEM_DIAG_extern_array_param_needs_own, loc,
	                 "array parameter '%s' of extern proc '%s' must be `own`, or be shadowed by "
	                 "an out-param (in-out, the same name in the out-list) — an extern is assumed "
	                 "to mutate its in-params, and a mutated read-only borrow cannot be allowed",
	                 param_name, proc_name);
}
SemDiag *sem_emit_proc_return_has_value(SemanticContext *ctx, SourceLoc loc) {
	return sem_emit_(ctx, SEM_DIAG_proc_return_has_value, loc,
	                 "a `proc` has no return value — write results to out-params; a bare "
	                 "`return;` is an early exit");
}
SemDiag *sem_emit_sys_no_return(SemanticContext *ctx, SourceLoc loc) {
	return sem_emit_(ctx, SEM_DIAG_sys_no_return, loc,
	                 "a `sys` does not support `return` — it runs to completion over all matching "
	                 "archetypes; use an `if` to guard work instead");
}

/* --- Field / component --- */

SemDiag *sem_emit_no_field(SemanticContext *ctx, SourceLoc loc, const char *arch_name, const char *field_name) {
	return sem_emit_(ctx, SEM_DIAG_no_field, loc, "Archetype '%s' has no field '%s'", arch_name, field_name);
}
SemDiag *sem_emit_cannot_read_through_handle(SemanticContext *ctx, SourceLoc loc, const char *field_name,
                                             const char *base_name, const char *arch_name) {
	return sem_emit_(ctx, SEM_DIAG_cannot_read_through_handle, loc,
	                 "cannot read component '%s' through handle '%s': a handle is a lifetime token, "
	                 "not a row view — use column access `%s.%s[i]`",
	                 field_name, base_name, arch_name, field_name);
}
SemDiag *sem_emit_not_archetype_instance(SemanticContext *ctx, SourceLoc loc, const char *name) {
	return sem_emit_(ctx, SEM_DIAG_not_archetype_instance, loc, "Variable '%s' does not refer to an archetype instance",
	                 name);
}

/* --- Declaration shape --- */

SemDiag *sem_emit_archetype_only_as_param(SemanticContext *ctx, SourceLoc loc) {
	return sem_emit_(ctx, SEM_DIAG_archetype_only_as_param, loc, "`archetype` is only valid as a parameter type");
}
SemDiag *sem_emit_archetype_not_return_type(SemanticContext *ctx, SourceLoc loc, const char *func_name) {
	return sem_emit_(ctx, SEM_DIAG_archetype_not_return_type, loc,
	                 "func '%s': `archetype` is only valid as a parameter type, not a return type", func_name);
}
SemDiag *sem_emit_archetype_funcs_only(SemanticContext *ctx, SourceLoc loc, const char *func_name) {
	return sem_emit_(ctx, SEM_DIAG_archetype_funcs_only, loc,
	                 "func '%s': `archetype` parameter type is only allowed on procs, not funcs", func_name);
}
SemDiag *sem_emit_multiple_archetype_params(SemanticContext *ctx, SourceLoc loc, const char *proc_name) {
	return sem_emit_(ctx, SEM_DIAG_multiple_archetype_params, loc,
	                 "proc '%s': only one `archetype` parameter is allowed per proc", proc_name);
}
SemDiag *sem_emit_handle_in_sys_param(SemanticContext *ctx, SourceLoc loc, const char *name) {
	return sem_emit_(ctx, SEM_DIAG_handle_in_sys_param, loc, "handle column '%s' cannot be sys parameter", name);
}
SemDiag *sem_emit_each_field_filter_type_not_name(SemanticContext *ctx, SourceLoc loc) {
	return sem_emit_(ctx, SEM_DIAG_each_field_filter_type_not_name, loc,
	                 "each_field filter type must be a primitive type");
}
SemDiag *sem_emit_each_field_filter_type_not_primitive(SemanticContext *ctx, SourceLoc loc) {
	return sem_emit_(ctx, SEM_DIAG_each_field_filter_type_not_primitive, loc,
	                 "each_field filter type must be a primitive type (int, float, or char)");
}
SemDiag *sem_emit_each_field_invalid_rhs(SemanticContext *ctx, SourceLoc loc, const char *name) {
	return sem_emit_(ctx, SEM_DIAG_each_field_invalid_rhs, loc,
	                 "each_field RHS '%s' must be an `archetype`-typed parameter of the enclosing proc", name);
}

/* --- Allocation / archetype --- */

SemDiag *sem_emit_alloc_not_at_top(SemanticContext *ctx, SourceLoc loc) {
	return sem_emit_(ctx, SEM_DIAG_alloc_not_at_top, loc,
	                 "alloc only allowed at top-level, not inside proc or sys body");
}
SemDiag *sem_emit_alloc_count_not_literal(SemanticContext *ctx, SourceLoc loc) {
	return sem_emit_(ctx, SEM_DIAG_alloc_count_not_literal, loc,
	                 "alloc count must be a literal; dynamic counts not yet supported");
}
SemDiag *sem_emit_shape_already_allocated(SemanticContext *ctx, SourceLoc loc, const char *name) {
	return sem_emit_(ctx, SEM_DIAG_shape_already_allocated, loc,
	                 "Shape already allocated (alias '%s' shares shape with an earlier alloc)", name);
}
SemDiag *sem_emit_duplicate_component(SemanticContext *ctx, SourceLoc loc, const char *name) {
	return sem_emit_(ctx, SEM_DIAG_duplicate_component, loc,
	                 "duplicate component '%s' in archetype (a component type may appear only once)", name);
}

SemDiag *sem_emit_component_redefined(SemanticContext *ctx, SourceLoc loc, const char *name) {
	return sem_emit_(ctx, SEM_DIAG_component_redefined, loc,
	                 "component '%s' is defined more than once — define each component exactly once "
	                 "(inline or top-level) and reference it by bare name elsewhere",
	                 name);
}

SemDiag *sem_emit_non_exhaustive_match(SemanticContext *ctx, SourceLoc loc, const char *missing) {
	return sem_emit_(ctx, SEM_DIAG_non_exhaustive_match, loc,
	                 "non-exhaustive match: missing '%s' (an enum match must cover every variant; an "
	                 "open-key match needs a `_`)",
	                 missing);
}

SemDiag *sem_emit_wildcard_in_enum_match(SemanticContext *ctx, SourceLoc loc) {
	return sem_emit_(ctx, SEM_DIAG_wildcard_in_enum_match, loc,
	                 "an enum `match` may not use `_` — cover every variant explicitly (add a named "
	                 "case like `not_found` instead of a catch-all)");
}

SemDiag *sem_emit_callable_in_archetype(SemanticContext *ctx, SourceLoc loc, const char *name) {
	return sem_emit_(ctx, SEM_DIAG_callable_in_archetype, loc,
	                 "proc/func types cannot be archetype components ('%s') — archetypes are data; "
	                 "dispatch with `match` or a system",
	                 name);
}

/* --- Calls / groups --- */

SemDiag *sem_emit_action_in_expression(SemanticContext *ctx, SourceLoc loc, const char *kind, const char *name) {
	return sem_emit_(ctx, SEM_DIAG_action_in_expression, loc,
	                 "a %s call is an action, not a value — bind it (`x := %s(...)`) or call "
	                 "it as a statement; it can't appear inside an expression",
	                 kind, name);
}
SemDiag *sem_emit_no_group_match(SemanticContext *ctx, SourceLoc loc, const char *name) {
	return sem_emit_(ctx, SEM_DIAG_no_group_match, loc, "no member of group '%s' matches the argument types", name);
}
SemDiag *sem_emit_ambiguous_group_call(SemanticContext *ctx, SourceLoc loc, const char *name) {
	return sem_emit_(ctx, SEM_DIAG_ambiguous_group_call, loc, "call to '%s' is ambiguous among group members", name);
}
SemDiag *sem_emit_unknown_group_member(SemanticContext *ctx, SourceLoc loc, const char *member, const char *group) {
	return sem_emit_(ctx, SEM_DIAG_unknown_group_member, loc, "unknown member '%s' in func group '%s'", member, group);
}
SemDiag *sem_emit_group_member_extern(SemanticContext *ctx, SourceLoc loc, const char *member, const char *group) {
	return sem_emit_(ctx, SEM_DIAG_group_member_extern, loc,
	                 "member '%s' of func group '%s' is extern; extern funcs cannot be group members", member, group);
}
SemDiag *sem_emit_group_member_not_declared(SemanticContext *ctx, SourceLoc loc, const char *member,
                                            const char *group) {
	return sem_emit_(ctx, SEM_DIAG_group_member_not_declared, loc,
	                 "member '%s' of func group '%s' must be declared before the group", member, group);
}
SemDiag *sem_emit_duplicate_group_signatures(SemanticContext *ctx, SourceLoc loc, const char *m1, const char *m2,
                                             const char *group) {
	return sem_emit_(ctx, SEM_DIAG_duplicate_group_signatures, loc,
	                 "members '%s' and '%s' of group '%s' share a parameter signature", m1, m2, group);
}
SemDiag *sem_emit_group_name_collision(SemanticContext *ctx, SourceLoc loc, const char *name) {
	return sem_emit_(ctx, SEM_DIAG_group_name_collision, loc, "name '%s' is already declared", name);
}
SemDiag *sem_emit_empty_group(SemanticContext *ctx, SourceLoc loc) {
	return sem_emit_(ctx, SEM_DIAG_empty_group, loc, "func group must have at least one member");
}

/* --- Extern / signature --- */

SemDiag *sem_emit_extern_proc_bad_type(SemanticContext *ctx, SourceLoc loc, const char *type, const char *proc_name) {
	return sem_emit_(ctx, SEM_DIAG_extern_proc_bad_type, loc, "unknown type '%s' in extern proc '%s' signature", type,
	                 proc_name);
}
SemDiag *sem_emit_extern_func_bad_type(SemanticContext *ctx, SourceLoc loc, const char *type, const char *func_name) {
	return sem_emit_(ctx, SEM_DIAG_extern_func_bad_type, loc, "unknown type '%s' in extern func '%s' signature", type,
	                 func_name);
}
SemDiag *sem_emit_extern_func_bad_return(SemanticContext *ctx, SourceLoc loc, const char *type, const char *func_name) {
	return sem_emit_(ctx, SEM_DIAG_extern_func_bad_return, loc,
	                 "unknown return type '%s' in extern func '%s' signature", type, func_name);
}

/* --- Constants / purity --- */

SemDiag *sem_emit_constant_redefined(SemanticContext *ctx, SourceLoc loc, const char *name) {
	return sem_emit_(ctx, SEM_DIAG_constant_redefined, loc, "constant '%s' already defined", name);
}
SemDiag *sem_emit_const_value_is_type(SemanticContext *ctx, SourceLoc loc, const char *name, const char *rhs) {
	return sem_emit_(ctx, SEM_DIAG_const_value_is_type, loc,
	                 "constant '%s' has a value type but its RHS '%s' names a type", name, rhs);
}
SemDiag *sem_emit_unknown_const_value(SemanticContext *ctx, SourceLoc loc, const char *rhs, const char *name) {
	return sem_emit_(ctx, SEM_DIAG_unknown_const_value, loc, "unknown value '%s' in declaration of '%s'", rhs, name);
}
SemDiag *sem_emit_const_rhs_invalid(SemanticContext *ctx, SourceLoc loc) {
	return sem_emit_(ctx, SEM_DIAG_const_rhs_invalid, loc, "a constant's value must be a literal, a name, or a type");
}
SemDiag *sem_emit_func_not_pure(SemanticContext *ctx, SourceLoc loc, const char *name, const char *reason) {
	return sem_emit_(ctx, SEM_DIAG_func_not_pure, loc, "func '%s' is not pure — %s (effects belong in a proc)", name,
	                 reason);
}

/* --- Assignment targets --- */

SemDiag *sem_emit_assign_to_const(SemanticContext *ctx, SourceLoc loc, const char *name) {
	return sem_emit_(ctx, SEM_DIAG_assign_to_const, loc, "cannot assign to constant '%s' (declared with `::`)", name);
}
SemDiag *sem_emit_assign_to_undeclared(SemanticContext *ctx, SourceLoc loc, const char *name) {
	return sem_emit_(ctx, SEM_DIAG_assign_to_undeclared, loc, "Variable '%s' not declared", name);
}

/* --- Front-end (wired in P1f) --- */

SemDiag *sem_emit_parse_error(SemanticContext *ctx, SourceLoc loc, const char *msg) {
	return sem_emit_(ctx, SEM_DIAG_parse_error, loc, "%s", msg);
}
SemDiag *sem_emit_module_not_found(SemanticContext *ctx, SourceLoc loc, const char *name) {
	return sem_emit_(ctx, SEM_DIAG_module_not_found, loc, "Module not found: %s", name);
}
SemDiag *sem_emit_module_parse_failed(SemanticContext *ctx, SourceLoc loc, const char *name) {
	return sem_emit_(ctx, SEM_DIAG_module_parse_failed, loc, "Failed to parse module %s", name);
}

/* --- Lints --- */

SemDiag *sem_emit_lint_proc_could_be_func(SemanticContext *ctx, SourceLoc loc, const char *name) {
	return sem_emit_(
	    ctx, SEM_LINT_proc_could_be_func, loc,
	    "proc '%s' has a pure body and returns a value — it should be a `func` (suppress with @allow_pure_proc)", name);
}
SemDiag *sem_emit_lint_proc_no_effect(SemanticContext *ctx, SourceLoc loc, const char *name) {
	return sem_emit_(ctx, SEM_LINT_proc_no_effect, loc,
	                 "proc '%s' has an empty or effect-free body; remove it or add the intended logic", name);
}
SemDiag *sem_emit_lint_func_impure(SemanticContext *ctx, SourceLoc loc, const char *name, const char *reason) {
	return sem_emit_(ctx, SEM_LINT_func_impure, loc, "func '%s' is not pure — %s (effects belong in a proc)", name,
	                 reason);
}

/* --- Tier 1 (Phase 2) --- */

SemDiag *sem_emit_binop_type_mismatch(SemanticContext *ctx, SourceLoc loc, const char *op, const char *lhs,
                                      const char *rhs) {
	return sem_emit_(ctx, SEM_DIAG_binop_type_mismatch, loc,
	                 "type mismatch in '%s': cannot mix '%s' and '%s' — arche has no implicit numeric conversion", op,
	                 lhs, rhs);
}
SemDiag *sem_emit_field_on_non_archetype(SemanticContext *ctx, SourceLoc loc, const char *base_type,
                                         const char *field) {
	return sem_emit_(ctx, SEM_DIAG_field_on_non_archetype, loc, "type '%s' has no field '%s'", base_type, field);
}
SemDiag *sem_emit_move_outside_arg(SemanticContext *ctx, SourceLoc loc, const char *keyword) {
	return sem_emit_(ctx, SEM_DIAG_move_outside_arg, loc, "`%s` is only valid in a function-call argument position",
	                 keyword);
}
SemDiag *sem_emit_underscore_not_inout(SemanticContext *ctx, SourceLoc loc) {
	return sem_emit_(ctx, SEM_DIAG_underscore_not_inout, loc,
	                 "`_` is only valid for an in-out parameter (shadowed by an out-param)");
}
SemDiag *sem_emit_local_shadows_callable(SemanticContext *ctx, SourceLoc loc, const char *name) {
	return sem_emit_(ctx, SEM_DIAG_local_shadows_callable, loc,
	                 "local '%s' shadows the proc/func '%s' — a variable and a callable share the value "
	                 "namespace, so the name is ambiguous (reads hit the local, assignment hits the "
	                 "callable); rename the local",
	                 name, name);
}
SemDiag *sem_emit_extern_proc_bad_return(SemanticContext *ctx, SourceLoc loc, const char *type, const char *proc_name) {
	return sem_emit_(ctx, SEM_DIAG_extern_proc_bad_return, loc,
	                 "unknown return type '%s' in extern proc '%s' signature", type, proc_name);
}

SemDiag *sem_emit_type_mismatch(SemanticContext *ctx, SourceLoc loc, const char *where, const char *expected,
                                const char *got) {
	return sem_emit_(ctx, SEM_DIAG_type_mismatch, loc, "%s: expected '%s', got '%s'", where, expected, got);
}

SemDiag *sem_emit_wrong_arity(SemanticContext *ctx, SourceLoc loc, const char *name, int expected, int got) {
	return sem_emit_(ctx, SEM_DIAG_wrong_arity, loc, "'%s' expects %d argument%s, got %d", name, expected,
	                 expected == 1 ? "" : "s", got);
}

SemDiag *sem_emit_not_indexable(SemanticContext *ctx, SourceLoc loc, const char *base_type) {
	return sem_emit_(ctx, SEM_DIAG_not_indexable, loc,
	                 "value of type '%s' is not indexable — only arrays/strings/archetype columns can be indexed",
	                 base_type);
}

SemDiag *sem_emit_wrong_return_arity(SemanticContext *ctx, SourceLoc loc, const char *fn_name, int expected, int got) {
	return sem_emit_(ctx, SEM_DIAG_wrong_arity, loc, "'%s' declares %d return value%s, returned %d", fn_name, expected,
	                 expected == 1 ? "" : "s", got);
}

SemDiag *sem_emit_break_outside_loop(SemanticContext *ctx, SourceLoc loc) {
	return sem_emit_(ctx, SEM_DIAG_break_outside_loop, loc, "`break` can only appear inside a loop body");
}

SemDiag *sem_emit_continue_outside_loop(SemanticContext *ctx, SourceLoc loc) {
	return sem_emit_(ctx, SEM_DIAG_continue_outside_loop, loc, "`continue` can only appear inside a loop body");
}

SemDiag *sem_emit_module_no_member(SemanticContext *ctx, SourceLoc loc, const char *module, const char *member) {
	return sem_emit_(ctx, SEM_DIAG_module_no_member, loc, "module '%s' has no member '%s'", module, member);
}

SemDiag *sem_emit_duplicate_decl(SemanticContext *ctx, SourceLoc loc, const char *kind, const char *name) {
	return sem_emit_(ctx, SEM_DIAG_duplicate_decl, loc, "%s '%s' is already declared at this scope", kind, name);
}

SemDiag *sem_emit_drop_invalid(SemanticContext *ctx, SourceLoc loc, const char *msg) {
	return sem_emit_(ctx, SEM_DIAG_drop_invalid, loc, "%s", msg);
}
SemDiag *sem_emit_drop_redefined(SemanticContext *ctx, SourceLoc loc, const char *type_name) {
	return sem_emit_(ctx, SEM_DIAG_drop_redefined, loc,
	                 "a destructor is already registered for opaque type '%s' — one `@drop` per type", type_name);
}
SemDiag *sem_emit_drop_conditional(SemanticContext *ctx, SourceLoc loc, const char *name) {
	return sem_emit_(ctx, SEM_DIAG_drop_conditional, loc,
	                 "handle '%s' is consumed on some paths but not others — consume it on every path or none", name);
}
SemDiag *sem_emit_duplicate_region(SemanticContext *ctx, SourceLoc loc, const char *region) {
	return sem_emit_(ctx, SEM_DIAG_duplicate_region, loc,
	                 "a file may contain at most one `%s` region — merge it with the earlier one", region);
}

SemDiag *sem_emit_lint_unused_local(SemanticContext *ctx, SourceLoc loc, const char *name) {
	return sem_emit_(ctx, SEM_LINT_unused_local, loc,
	                 "local '%s' is declared but never read (prefix with '_' to silence, or @allow(unused_local))",
	                 name);
}
SemDiag *sem_emit_lint_unused_use(SemanticContext *ctx, SourceLoc loc, const char *name) {
	return sem_emit_(ctx, SEM_LINT_unused_use, loc, "module '%s' is imported but none of its symbols are used", name);
}
SemDiag *sem_emit_lint_inout_redundant_arg(SemanticContext *ctx, SourceLoc loc, const char *name) {
	return sem_emit_(ctx, SEM_LINT_inout_redundant_arg, loc,
	                 "in-out call repeats '%s' in the in-slot; write `_` to mark the shadowed in-position: "
	                 "`f(_)(%s)`",
	                 name, name);
}
SemDiag *sem_emit_lint_inout_param_shadow(SemanticContext *ctx, SourceLoc loc, const char *name) {
	return sem_emit_(ctx, SEM_LINT_inout_param_shadow, loc,
	                 "out-param '%s' shadows the in-param of the same name (in-out): legitimate only for a "
	                 "`#foreign` proc (C-ABI alignment). Return a fresh out-only result instead",
	                 name);
}
SemDiag *sem_emit_lint_unused_function(SemanticContext *ctx, SourceLoc loc, const char *name) {
	return sem_emit_(ctx, SEM_LINT_unused_function, loc,
	                 "function '%s' is declared but never called (prefix with '_' to silence, or "
	                 "@allow(unused_function))",
	                 name);
}
SemDiag *sem_emit_lint_unused_static_const(SemanticContext *ctx, SourceLoc loc, const char *kind, const char *name) {
	return sem_emit_(ctx, SEM_LINT_unused_static_const, loc,
	                 "%s '%s' is declared but never used (prefix with '_' to silence, or "
	                 "@allow(unused_static_const))",
	                 kind, name);
}
SemDiag *sem_emit_lint_unused_enum(SemanticContext *ctx, SourceLoc loc, const char *name) {
	return sem_emit_(ctx, SEM_LINT_unused_enum, loc,
	                 "enum '%s' is declared but never used (prefix with '_' to silence, or "
	                 "@allow(unused_enum))",
	                 name);
}
