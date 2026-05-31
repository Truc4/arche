#include "args.h"
#include "cli.h"
#include <stdio.h>

static const ArgSpec k_explain_specs[] = {
	{0, NULL, ARG_FLAG, 0, 0, NULL, NULL},
};

int explain_run(int argc, char **argv, const GlobalOpts *g) {
	(void)g;
	ArgParse p;
	if (args_parse(k_explain_specs, argc, argv, &p) != 0) {
		fprintf(stderr, "%s: %s\n", g_prog, p.err);
		args_usage(stderr, g_prog, "explain", "<code>", k_explain_specs);
		return ARCHE_USAGE;
	}
	if (p.want_help) {
		args_usage(stdout, g_prog, "explain", "<code>   (e.g. E0001)", k_explain_specs);
		return ARCHE_OK;
	}
	if (p.pos_count == 0) {
		fprintf(stderr, "usage: %s explain <code> (e.g. E0001)\n", g_prog);
		return ARCHE_USAGE;
	}
	return explain_print(p.pos[0]);
}
