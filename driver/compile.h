#ifndef ARCHE_DRIVER_COMPILE_H
#define ARCHE_DRIVER_COMPILE_H

/* Maximum number of --link paths accepted on one command line */
#define ARCHE_MAX_LINK_PATHS 32

/* How far the pipeline runs and what `out_path` receives. EMIT_LINK (0) is the default — a full
 * executable — so a zero-initialized CompileOpts behaves as before. */
typedef enum {
	EMIT_LINK = 0, /* opt → llc → cc: a native executable      */
	EMIT_LLVM_IR,  /* codegen only: LLVM IR (.ll)              */
	EMIT_ASM,      /* opt → llc: x86-64 assembly (.s)          */
	EMIT_OBJ,      /* opt → llc → cc -c: an object file (.o)   */
} EmitKind;

/* Options for one compile. Mirrors the CLI flags the historical inline pipeline
 * in main() consumed; kept here so every frontend (the `build` path, the
 * doctest runner, future tools) drives compilation through one entry point. */
typedef struct {
	EmitKind emit;                                /* pipeline stop point / output kind (default EMIT_LINK) */
	int quiet;                                    /* suppress progress chatter on stdout (doctest runner) */
	const char *link_paths[ARCHE_MAX_LINK_PATHS]; /* extra .c/.o files for cc at link time */
	int link_count;
} CompileOpts;

/* Compile `user_source` (raw user text — the core prelude is NOT yet prepended;
 * compile_source prepends it) to `out_path`. `source_path` is the on-disk path
 * of the user file, used for module (`use`) resolution. Returns 0 on success,
 * non-zero on any failure (diagnostics printed to stderr). Behavior-preserving
 * extraction of the pipeline that previously lived inline in main(). */
int compile_source(const char *user_source, const char *source_path, const char *out_path, const CompileOpts *opts);

/* Run only the front-end (prepend core → parse → resolve modules → semantic analysis) and report
 * success/failure, without lowering or codegen — backs the `check` subcommand. Returns 0 if the
 * program is well-formed, 1 otherwise (diagnostics printed to stderr). */
int compile_check(const char *user_source, const char *source_path, const CompileOpts *opts);

#endif /* ARCHE_DRIVER_COMPILE_H */
