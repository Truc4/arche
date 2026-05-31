#ifndef ARCHE_CLI_ARGS_H
#define ARCHE_CLI_ARGS_H

/* Tiny, dependency-free, table-driven argument parser shared by every `arche` subcommand.
 *
 * Each subcommand declares a static array of ArgSpec (terminated by a zeroed entry). That one
 * table is the single source of truth: it drives parsing here AND generates `--help` (args_usage).
 *
 * A spec lists one or more accepted spellings in `forms` (space-separated) so the legacy gcc-style
 * single-dash long flags (`-emit-llvm`, `-Wno-proc-no-effect`, `-Werror=...`) sit beside ordinary
 * `--long` / `-short` flags and aliases — e.g. forms "--emit -emit-llvm" makes the old spelling a
 * hidden alias of the new one.
 *
 *   ARG_FLAG   — boolean, matched by exact token (`-emit-llvm`, `-Werror`, `-Werror=proc-no-effect`).
 *   ARG_VALUE  — takes a value, accepted as `<form> <value>` or `<form>=<value>` (`-o out`, `--emit=asm`).
 */

#include <stdio.h>

typedef enum { ARG_FLAG, ARG_VALUE } ArgKind;

typedef struct {
	int id;              /* caller's enum, returned on match (use values >= 1; 0 ends the table)   */
	const char *forms;   /* space-separated accepted spellings, canonical first ("--emit -emit-llvm") */
	ArgKind kind;        /* flag or value-taking                                                    */
	int repeatable;      /* may appear more than once (e.g. --link); else last occurrence wins      */
	int hidden;          /* omit from generated help (aliases, dev flags)                           */
	const char *metavar; /* value placeholder for help ("<path>", "<kind>"); NULL for flags         */
	const char *help;    /* one-line description for help                                           */
} ArgSpec;

#define ARG_MAX_HITS 128
#define ARG_MAX_POS 256

typedef struct {
	int id;
	const char *value; /* NULL for flags */
} ArgHit;

typedef struct {
	ArgHit hits[ARG_MAX_HITS];
	int hit_count;
	const char *pos[ARG_MAX_POS]; /* positional (non-flag) arguments, in order */
	int pos_count;
	const char *fwd[ARG_MAX_POS]; /* arguments after a literal `--`, forwarded verbatim */
	int fwd_count;
	int saw_dashdash; /* a `--` separator was present */
	int want_help;    /* `-h` / `--help` was seen (recognized for every command) */
	char err[256];    /* parse-error message; empty if parse succeeded */
} ArgParse;

/* Parse argv[1 .. argc-1] against `specs` (argv[0] is the command/prog name, ignored). Returns 0 on
 * success; non-zero on a usage error, with a message in out->err. Unknown flags and missing values
 * are usage errors. Everything after a literal `--` lands in out->fwd untouched. */
int args_parse(const ArgSpec *specs, int argc, char **argv, ArgParse *out);

/* True if a flag/value with this id was seen. */
int args_has(const ArgParse *p, int id);

/* Value of the LAST occurrence of `id` (last-wins, matching the legacy `-o` behavior), or NULL. */
const char *args_value(const ArgParse *p, int id);

/* Print a generated usage block for one subcommand: synopsis line + a flag list built from `specs`
 * (hidden specs omitted). `prog` is "arche", `cmd` the subcommand name, `synopsis` the arg summary. */
void args_usage(FILE *f, const char *prog, const char *cmd, const char *synopsis, const ArgSpec *specs);

#endif /* ARCHE_CLI_ARGS_H */
