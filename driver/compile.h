#ifndef ARCHE_DRIVER_COMPILE_H
#define ARCHE_DRIVER_COMPILE_H

/* Maximum number of --link paths accepted on one command line */
#define ARCHE_MAX_LINK_PATHS 32

/* Options for one compile. Mirrors the CLI flags the historical inline pipeline
 * in main() consumed; kept here so every frontend (the `build` path, the
 * doctest runner, future tools) drives compilation through one entry point. */
typedef struct {
	int emit_llvm;                                /* emit LLVM IR to out_path instead of an executable */
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

#endif /* ARCHE_DRIVER_COMPILE_H */
