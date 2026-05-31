/* `arche run` uses POSIX process + temp-file calls (mkdtemp, fork, execv, waitpid, unlink, rmdir),
 * which glibc hides under -std=c99 without a feature-test macro. */
#define _POSIX_C_SOURCE 200809L
#include "../driver/compile.h"
#include "../semantic/semantic.h"
#include "args.h"
#include "cli.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

enum { R_WNO_PCBF = 1, R_WNO_PNE, R_WERR_PCBF, R_WERR_PNE, R_WERR };

static const ArgSpec k_run_specs[] = {
	{R_WNO_PCBF, "-Wno-proc-could-be-func", ARG_FLAG, 0, 0, NULL, "disable the proc-could-be-func lint"},
	{R_WNO_PNE, "-Wno-proc-no-effect", ARG_FLAG, 0, 0, NULL, "disable the proc-no-effect lint"},
	{R_WERR_PCBF, "-Werror=proc-could-be-func", ARG_FLAG, 0, 0, NULL, "promote the proc-could-be-func lint to an error"},
	{R_WERR_PNE, "-Werror=proc-no-effect", ARG_FLAG, 0, 0, NULL, "promote the proc-no-effect lint to an error"},
	{R_WERR, "-Werror", ARG_FLAG, 0, 0, NULL, "promote all lints to errors"},
	{0, NULL, ARG_FLAG, 0, 0, NULL, NULL},
};

/* `arche run <file> [prog-args...]` / `arche run <file> -- [prog-args...]`: compile to a temp
 * executable, run it with the forwarded arguments, and return the program's exit code. The first
 * positional is the source file; any remaining positionals plus everything after `--` are the
 * program's argv (so both `run f a b` and `run f -- a b` work). */
int run_run(int argc, char **argv, const GlobalOpts *g) {
	(void)g;
	ArgParse p;
	if (args_parse(k_run_specs, argc, argv, &p) != 0) {
		fprintf(stderr, "%s: %s\n", g_prog, p.err);
		args_usage(stderr, g_prog, "run", "[flags] <input.arche> [-- program-args...]", k_run_specs);
		return ARCHE_USAGE;
	}
	if (p.want_help) {
		args_usage(stdout, g_prog, "run", "[flags] <input.arche> [-- program-args...]", k_run_specs);
		return ARCHE_OK;
	}

	int pcbf_en = 1, pcbf_we = 0, pne_en = 1, pne_we = 0;
	if (args_has(&p, R_WNO_PCBF))
		pcbf_en = 0;
	if (args_has(&p, R_WNO_PNE))
		pne_en = 0;
	if (args_has(&p, R_WERR_PCBF))
		pcbf_we = 1;
	if (args_has(&p, R_WERR_PNE))
		pne_we = 1;
	if (args_has(&p, R_WERR)) {
		pcbf_we = 1;
		pne_we = 1;
	}
	semantic_set_lint_proc_could_be_func(pcbf_en, pcbf_we);
	semantic_set_lint_proc_no_effect(pne_en, pne_we);

	if (p.pos_count == 0) {
		fprintf(stderr, "%s: no input file\n", g_prog);
		args_usage(stderr, g_prog, "run", "[flags] <input.arche> [-- program-args...]", k_run_specs);
		return ARCHE_USAGE;
	}
	const char *input = p.pos[0]; /* first positional is the source; the rest are program args */
	char *src = cli_read_file(input);
	if (!src)
		return ARCHE_ERR;

	/* Build into a private temp dir (mkdtemp, mode 0700 — same approach as the driver's workdir). */
	char dir[] = "/tmp/arche_run_XXXXXX";
	if (!mkdtemp(dir)) {
		perror("Failed to create temp dir");
		free(src);
		return ARCHE_ERR;
	}
	char exe[600];
	snprintf(exe, sizeof(exe), "%s/a.out", dir);

	CompileOpts opts = {0};
	opts.quiet = 1; /* `go run`-style: no pipeline chatter, just the program's own output */
	int rc = compile_source(src, input, exe, &opts);
	free(src);
	if (rc != 0) {
		unlink(exe);
		rmdir(dir);
		return rc;
	}

	/* Assemble the child's argv: exe, then remaining positionals, then post-`--` args. */
	char *cargv[ARG_MAX_POS * 2 + 2];
	int n = 0;
	cargv[n++] = exe;
	for (int i = 1; i < p.pos_count; i++)
		cargv[n++] = (char *)p.pos[i];
	for (int i = 0; i < p.fwd_count; i++)
		cargv[n++] = (char *)p.fwd[i];
	cargv[n] = NULL;

	pid_t pid = fork();
	if (pid < 0) {
		perror("fork");
		unlink(exe);
		rmdir(dir);
		return ARCHE_ERR;
	}
	if (pid == 0) {
		execv(exe, cargv);
		perror("exec");
		_exit(127);
	}
	int status = 0;
	while (waitpid(pid, &status, 0) < 0)
		;
	unlink(exe);
	rmdir(dir);
	if (WIFEXITED(status))
		return WEXITSTATUS(status);
	if (WIFSIGNALED(status))
		return 128 + WTERMSIG(status);
	return ARCHE_ERR;
}

const ArgSpec *run_specs(void) { return k_run_specs; }
