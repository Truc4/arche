#include "../driver/compile.h"
#include "../semantic/semantic.h"
#include "args.h"
#include "cli.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>

enum {
	B_OUT = 1,
	B_LINK,
	B_EMIT,
	B_EMIT_LLVM,
	B_WNO_PCBF,
	B_WNO_PNE,
	B_WERR_PCBF,
	B_WERR_PNE,
	B_WERR,
};

/* Flag table = parsing + `--help`, one source of truth. The `-Wno-*` / `-Werror[=...]` spellings are
 * gcc-style single-dash longs matched as exact ARG_FLAG tokens; `-emit-llvm` is preserved here (it
 * becomes a hidden alias of `--emit=llvm-ir` in a later phase). */
static const ArgSpec k_build_specs[] = {
    {B_OUT, "-o", ARG_VALUE, 0, 0, "<path>", "output path (executable, or the emitted artifact)"},
    {B_LINK, "--link", ARG_VALUE, 1, 0, "<path>", "pass an extra .c/.o file to cc at link time (repeatable)"},
    {B_EMIT, "--emit", ARG_VALUE, 0, 0, "<kind>", "what to produce: link (default), llvm-ir, asm, obj"},
    {B_EMIT_LLVM, "-emit-llvm", ARG_FLAG, 0, 1, NULL, "alias for --emit=llvm-ir"},
    {B_WNO_PCBF, "-Wno-proc-could-be-func", ARG_FLAG, 0, 0, NULL, "disable the proc-could-be-func lint"},
    {B_WNO_PNE, "-Wno-proc-no-effect", ARG_FLAG, 0, 0, NULL, "disable the proc-no-effect lint"},
    {B_WERR_PCBF, "-Werror=proc-could-be-func", ARG_FLAG, 0, 0, NULL,
     "promote the proc-could-be-func lint to an error"},
    {B_WERR_PNE, "-Werror=proc-no-effect", ARG_FLAG, 0, 0, NULL, "promote the proc-no-effect lint to an error"},
    {B_WERR, "-Werror", ARG_FLAG, 0, 0, NULL, "promote all lints to errors"},
    {0, NULL, ARG_FLAG, 0, 0, NULL, NULL},
};

/* Derive the default output path "build/<basename-without-extension>" (heap; leaked at exit). */
static char *default_output(const char *input_file) {
	const char *base = strrchr(input_file, '/');
	base = base ? base + 1 : input_file;
	size_t len = strlen(base) + 20;
	char *out = malloc(len);
	strcpy(out, "build/");
	strcat(out, base);
	char *dot = strrchr(out, '.');
	if (dot)
		*dot = '\0';
	return out;
}

int build_run(int argc, char **argv, const GlobalOpts *g) {
	(void)g;
	ArgParse p;
	if (args_parse(k_build_specs, argc, argv, &p) != 0) {
		fprintf(stderr, "%s: %s\n", g_prog, p.err);
		args_usage(stderr, g_prog, "build", "[flags] <input.arche>", k_build_specs);
		return ARCHE_USAGE;
	}
	if (p.want_help) {
		args_usage(stdout, g_prog, "build", "[flags] <input.arche>", k_build_specs);
		return ARCHE_OK;
	}

	/* Lint config — both on by default; flags disable or promote to errors. */
	int pcbf_en = 1, pcbf_we = 0, pne_en = 1, pne_we = 0;
	if (args_has(&p, B_WNO_PCBF))
		pcbf_en = 0;
	if (args_has(&p, B_WNO_PNE))
		pne_en = 0;
	if (args_has(&p, B_WERR_PCBF))
		pcbf_we = 1;
	if (args_has(&p, B_WERR_PNE))
		pne_we = 1;
	if (args_has(&p, B_WERR)) {
		pcbf_we = 1;
		pne_we = 1;
	}
	semantic_set_lint_proc_could_be_func(pcbf_en, pcbf_we);
	semantic_set_lint_proc_no_effect(pne_en, pne_we);

	if (p.pos_count == 0) {
		fprintf(stderr, "%s: no input file\n", g_prog);
		args_usage(stderr, g_prog, "build", "[flags] <input.arche>", k_build_specs);
		return ARCHE_USAGE;
	}
	/* Last positional wins, matching the historical scan that overwrote input_file each time.
	 * A directory resolves to its entry file (main.arche / the sole .arche), like `arche run .`. */
	char *input_file = cli_resolve_input(p.pos[p.pos_count - 1]);
	if (!input_file)
		return ARCHE_USAGE;
	const char *output_file = args_value(&p, B_OUT); /* last-wins, or NULL */

	/* Output kind: `--emit=<kind>` (canonical) or the `-emit-llvm` alias; default is a full link. */
	EmitKind emit = EMIT_LINK;
	if (args_has(&p, B_EMIT_LLVM))
		emit = EMIT_LLVM_IR;
	const char *emit_val = args_value(&p, B_EMIT);
	if (emit_val) {
		if (strcmp(emit_val, "link") == 0)
			emit = EMIT_LINK;
		else if (strcmp(emit_val, "llvm-ir") == 0)
			emit = EMIT_LLVM_IR;
		else if (strcmp(emit_val, "asm") == 0)
			emit = EMIT_ASM;
		else if (strcmp(emit_val, "obj") == 0)
			emit = EMIT_OBJ;
		else {
			fprintf(stderr, "%s: unknown --emit kind '%s' (want: link, llvm-ir, asm, obj)\n", g_prog, emit_val);
			return ARCHE_USAGE;
		}
	}

	/* Limit memory to 512MB to prevent runaway compilation. */
	struct rlimit mem_limit;
	mem_limit.rlim_cur = 512 * 1024 * 1024;
	mem_limit.rlim_max = 512 * 1024 * 1024;
	if (setrlimit(RLIMIT_AS, &mem_limit) != 0) {
		perror("Error: Could not set memory limit");
		return ARCHE_ERR;
	}

	if (!output_file)
		output_file = default_output(input_file);

	/* Read the input file. */
	FILE *input = fopen(input_file, "r");
	if (!input) {
		perror("Failed to open input file");
		return ARCHE_ERR;
	}
	fseek(input, 0, SEEK_END);
	long file_size = ftell(input);
	fseek(input, 0, SEEK_SET);
	char *source = malloc((size_t)file_size + 1);
	if (fread(source, 1, (size_t)file_size, input) != (size_t)file_size) {
		perror("Failed to read input file");
		free(source);
		fclose(input);
		return ARCHE_ERR;
	}
	source[file_size] = '\0';
	fclose(input);

	CompileOpts opts = {0};
	opts.emit = emit;
	for (int i = 0; i < p.hit_count; i++) {
		if (p.hits[i].id != B_LINK)
			continue;
		if (opts.link_count >= ARCHE_MAX_LINK_PATHS) {
			fprintf(stderr, "Error: too many --link arguments (max %d)\n", ARCHE_MAX_LINK_PATHS);
			free(source);
			return ARCHE_ERR;
		}
		opts.link_paths[opts.link_count++] = p.hits[i].value;
	}

	int rc = compile_source(source, input_file, output_file, &opts);
	free(source);
	return rc;
}

const ArgSpec *build_specs(void) {
	return k_build_specs;
}
