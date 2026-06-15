#ifndef ARCHE_CLI_CLI_H
#define ARCHE_CLI_CLI_H

/* The `arche` multitool: subcommand dispatch + shared global options.
 *
 * main() is a thin shim over cli_main(). Each subcommand lives in its own cmd_*.c, exposes a
 * `<cmd>_run(int argc, char **argv, const GlobalOpts *g)` entry (argv[0] is the command name), and
 * is registered in the dispatch table in cli.c. */

#include "args.h"

/* Uniform process exit codes. */
#define ARCHE_OK 0    /* success                                  */
#define ARCHE_ERR 1   /* compile / user error (diagnostics shown) */
#define ARCHE_USAGE 2 /* CLI usage error (bad flags / args)       */

typedef enum { COLOR_AUTO, COLOR_ALWAYS, COLOR_NEVER } ColorMode;

/* Options stripped from anywhere on the command line and threaded into every subcommand. */
typedef struct {
	int verbose;
	int quiet;
	ColorMode color;
} GlobalOpts;

typedef struct {
	const char *name;    /* "build", "test", ...                                  */
	const char *summary; /* one-liner for the top-level `help` listing            */
	int hidden;          /* dev-only subcommands omitted from `help` (e.g. __cst) */
	int (*run)(int argc, char **argv, const GlobalOpts *g);
	const ArgSpec *(*specs)(void); /* this command's flag table (NULL if none); drives completion */
} SubCmd;

/* The dispatch table (for `completion` / introspection). Sets *count. */
const SubCmd *cli_commands(int *count);

/* Program name shown in usage/help (argv[0] basename, or "arche"). */
extern const char *g_prog;

int cli_main(int argc, char **argv);

/* Apply `--exported-mutable=<level>` (error|warn|allow) to the W0022 exported-mutable-global lint.
 * `value` may be NULL (flag absent → leave the error-by-default). Returns 0 on success, non-zero on
 * an unknown level (the caller surfaces a usage error). Shared by build/check/run. */
int cli_apply_exported_mutable(const char *value);

/* Apply `--sys-foreign-write=<level>` (error|warn|allow) to the W0024 sys-writes-foreign-pool lint.
 * `value` may be NULL (flag absent → leave the error-by-default). Returns 0 on success, non-zero on
 * an unknown level (the caller surfaces a usage error). Shared by build/check/run. */
int cli_apply_sys_foreign_write(const char *value);

/* Read a whole file into a freshly malloc'd, NUL-terminated buffer (caller frees). On error prints
 * a perror message and returns NULL. Shared by the build/check/run subcommands. */
char *cli_read_file(const char *path);

/* A growable list of paths (used to expand fmt's arguments into a concrete file set). */
typedef struct {
	char **items;
	int count, cap;
} CliPathList;

/* Expand one command-line path spec into `.arche` files, appended to *pl:
 *   - a `...`-suffixed spec (`./...`, `src/...`) or a directory → recurse, collecting `*.arche`
 *     and skipping dotfiles / build / node_modules / site-packages;
 *   - anything else → taken as a literal path (added as-is, extension notwithstanding).
 * Results from a recursive walk are sorted for deterministic order. */
void cli_collect_arche(const char *spec, CliPathList *pl);
void cli_pathlist_free(CliPathList *pl);

/* Resolve a build/run/check input argument to a source file (freshly malloc'd; caller frees). A
 * regular path is returned as-is; a directory resolves to its `main.arche`, or the single `.arche`
 * file it contains (go-`run .` style). Returns NULL after printing a message if a directory has no
 * `.arche` file or more than one and no `main.arche`. */
char *cli_resolve_input(const char *path);

/* Subcommand entry points (defined in cmd_*.c). */
int build_run(int argc, char **argv, const GlobalOpts *g);
int test_run(int argc, char **argv, const GlobalOpts *g);
int check_run(int argc, char **argv, const GlobalOpts *g);
int run_run(int argc, char **argv, const GlobalOpts *g);
int explain_run(int argc, char **argv, const GlobalOpts *g);
int fmt_run(int argc, char **argv, const GlobalOpts *g);
int analyze_run(int argc, char **argv, const GlobalOpts *g);
int completion_run(int argc, char **argv, const GlobalOpts *g);
int version_run(int argc, char **argv, const GlobalOpts *g);
int init_run(int argc, char **argv, const GlobalOpts *g);
int fill_run(int argc, char **argv, const GlobalOpts *g);

/* Append a pool decl to driver `path` for each imported device's required shape it does not already
 * size, at the datasheet minimum (composed by max). Idempotent. Returns pools written, or -1 on
 * error. Shared by `arche fill` and `arche init driver`. */
int arche_fill_driver(const char *path);

/* Print the long-form explanation for a diagnostic code (from the explain dir) to stdout; returns
 * ARCHE_OK if found, ARCHE_ERR (with a short note) otherwise. Shared by `explain` and `--explain`. */
int explain_print(const char *code);

/* The version string ("major.minor.patch[-tag]"), from -DARCHE_VERSION. */
const char *arche_version_string(void);

/* Per-command flag tables (NULL-id-terminated), exposed for completion + the dispatch table. */
const ArgSpec *build_specs(void);
const ArgSpec *run_specs(void);
const ArgSpec *check_specs(void);
const ArgSpec *test_specs(void);
const ArgSpec *fmt_specs(void);
const ArgSpec *analyze_specs(void);

#endif /* ARCHE_CLI_CLI_H */
