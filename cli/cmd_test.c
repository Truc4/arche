#include "../doctest/doctest_run.h"
#include "args.h"
#include "cli.h"
#include <stdio.h>

enum { T_VERBOSE = 1 };

static const ArgSpec k_test_specs[] = {
    {T_VERBOSE, "-v --verbose", ARG_FLAG, 0, 0, NULL, "verbose: show each doctest as it runs"},
    {0, NULL, ARG_FLAG, 0, 0, NULL, NULL},
};

int test_run(int argc, char **argv, const GlobalOpts *g) {
	ArgParse p;
	if (args_parse(k_test_specs, argc, argv, &p) != 0) {
		fprintf(stderr, "%s: %s\n", g_prog, p.err);
		args_usage(stderr, g_prog, "test", "[-v] <file.arche | file.md | dir | ./...> ...", k_test_specs);
		return ARCHE_USAGE;
	}
	if (p.want_help) {
		args_usage(stdout, g_prog, "test", "[-v] <file.arche | file.md | dir | ./...> ...", k_test_specs);
		return ARCHE_OK;
	}
	int verbose = args_has(&p, T_VERBOSE) || (g && g->verbose);

	if (p.pos_count == 0) {
		args_usage(stderr, g_prog, "test", "[-v] <file.arche | file.md | dir | ./...> ...", k_test_specs);
		return ARCHE_ERR;
	}

	int rc = 0;
	for (int i = 0; i < p.pos_count; i++)
		rc |= doctest_run_path(p.pos[i], verbose); /* run every spec; fail if any fails */
	return rc ? ARCHE_ERR : ARCHE_OK;
}

const ArgSpec *test_specs(void) {
	return k_test_specs;
}
