#include "../compile/compile.h"
#include "../semantic/semantic.h"
#include "args.h"
#include "cli.h"
#include <stdio.h>
#include <stdlib.h>

enum { C_WNO_PCBF = 1, C_WNO_PNE, C_WERR_PCBF, C_WERR_PNE, C_WERR, C_ALLOW_UNDEFINED, C_EXPORTED_MUTABLE };

static const ArgSpec k_check_specs[] = {
    {C_WNO_PCBF, "-Wno-proc-could-be-func", ARG_FLAG, 0, 0, NULL, "disable the proc-could-be-func lint"},
    {C_WNO_PNE, "-Wno-proc-no-effect", ARG_FLAG, 0, 0, NULL, "disable the proc-no-effect lint"},
    {C_WERR_PCBF, "-Werror=proc-could-be-func", ARG_FLAG, 0, 0, NULL,
     "promote the proc-could-be-func lint to an error"},
    {C_WERR_PNE, "-Werror=proc-no-effect", ARG_FLAG, 0, 0, NULL, "promote the proc-no-effect lint to an error"},
    {C_WERR, "-Werror", ARG_FLAG, 0, 0, NULL, "promote all lints to errors"},
    {C_ALLOW_UNDEFINED, "--allow-undefined", ARG_FLAG, 0, 0, NULL,
     "permit the raw, runtime-unsafe `!undefined` opt-out (forbidden by default)"},
    {C_EXPORTED_MUTABLE, "--exported-mutable", ARG_VALUE, 0, 0, "<level>",
     "exported-mutable-global lint (W0022): error (default) | warn | allow"},
    {0, NULL, ARG_FLAG, 0, 0, NULL, NULL},
};

int check_run(int argc, char **argv, const GlobalOpts *g) {
	(void)g;
	ArgParse p;
	if (args_parse(k_check_specs, argc, argv, &p) != 0) {
		fprintf(stderr, "%s: %s\n", g_prog, p.err);
		args_usage(stderr, g_prog, "check", "[flags] <input.arche>", k_check_specs);
		return ARCHE_USAGE;
	}
	if (p.want_help) {
		args_usage(stdout, g_prog, "check", "[flags] <input.arche>", k_check_specs);
		return ARCHE_OK;
	}

	int pcbf_en = 1, pcbf_we = 0, pne_en = 1, pne_we = 0;
	if (args_has(&p, C_WNO_PCBF))
		pcbf_en = 0;
	if (args_has(&p, C_WNO_PNE))
		pne_en = 0;
	if (args_has(&p, C_WERR_PCBF))
		pcbf_we = 1;
	if (args_has(&p, C_WERR_PNE))
		pne_we = 1;
	if (args_has(&p, C_WERR)) {
		pcbf_we = 1;
		pne_we = 1;
	}
	semantic_set_lint_proc_could_be_func(pcbf_en, pcbf_we);
	semantic_set_lint_proc_no_effect(pne_en, pne_we);
	semantic_set_allow_undefined(args_has(&p, C_ALLOW_UNDEFINED));
	if (cli_apply_exported_mutable(args_value(&p, C_EXPORTED_MUTABLE)) != 0) {
		fprintf(stderr, "%s: --exported-mutable expects error|warn|allow\n", g_prog);
		args_usage(stderr, g_prog, "check", "[flags] <input.arche>", k_check_specs);
		return ARCHE_USAGE;
	}

	if (p.pos_count == 0) {
		fprintf(stderr, "%s: no input file\n", g_prog);
		args_usage(stderr, g_prog, "check", "[flags] <input.arche>", k_check_specs);
		return ARCHE_USAGE;
	}
	char *input = cli_resolve_input(p.pos[p.pos_count - 1]);
	if (!input)
		return ARCHE_USAGE;
	char *src = cli_read_file(input);
	if (!src) {
		free(input);
		return ARCHE_ERR;
	}

	CompileOpts opts = {0};
	int rc = compile_check(src, input, &opts);
	free(src);
	free(input);
	return rc; /* diagnostics already on stderr; silent on success, like `cargo check` */
}

const ArgSpec *check_specs(void) {
	return k_check_specs;
}
