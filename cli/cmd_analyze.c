#include "../arche_analyzer.h"
#include "args.h"
#include "cli.h"
#include <stdio.h>

enum { A_SERVE = 1 };

static const ArgSpec k_analyze_specs[] = {
	{A_SERVE, "--serve", ARG_FLAG, 0, 0, NULL, "run the persistent LSP line-protocol server"},
	{0, NULL, ARG_FLAG, 0, 0, NULL, NULL},
};

/* `arche analyze --serve` runs the language-server loop; `arche analyze [file]` does a one-shot dump
 * (file or stdin). Both reuse the analyzer's existing entry point (analyze_main) by reconstructing
 * the `--serve` / `--dump [file]` argv it expects. */
int analyze_run(int argc, char **argv, const GlobalOpts *g) {
	(void)g;
	ArgParse p;
	if (args_parse(k_analyze_specs, argc, argv, &p) != 0) {
		fprintf(stderr, "%s: %s\n", g_prog, p.err);
		args_usage(stderr, g_prog, "analyze", "[--serve] [<file.arche>]", k_analyze_specs);
		return ARCHE_USAGE;
	}
	if (p.want_help) {
		args_usage(stdout, g_prog, "analyze", "[--serve] [<file.arche>]", k_analyze_specs);
		return ARCHE_OK;
	}

	if (args_has(&p, A_SERVE)) {
		char *av[] = {(char *)"analyze", (char *)"--serve"};
		return analyze_main(2, av);
	}
	if (p.pos_count > 0) {
		char *av[] = {(char *)"analyze", (char *)"--dump", (char *)p.pos[0]};
		return analyze_main(3, av);
	}
	char *av[] = {(char *)"analyze", (char *)"--dump"}; /* dump from stdin */
	return analyze_main(2, av);
}

const ArgSpec *analyze_specs(void) { return k_analyze_specs; }
