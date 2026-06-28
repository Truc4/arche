#include "../codegen/codegen.h"
#include "../compile/compile.h"
#include "../compile/variant_select.h"
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
	B_NO_ABORT,
	B_NO_IMPLICIT_ABORT,
	B_ALLOW_UNDEFINED,
	B_FORBID_ALLOW,
	B_UNCHECKED,
	B_SELECT,
	B_TARGET,
	B_INCREMENTAL,
	B_WHOLE_PROGRAM,
	B_EXPORTED_MUTABLE,
	B_PROC_LEAF,
	B_SYS_FOREIGN_WRITE,
	B_POOL_INDEX,
	B_PROC_NOT_PRIMITIVE,
	B_DISCARDED_OK,
	B_EMIT_GPU,
	B_GPU,
	B_WNO_LSA,
	B_WERR_LSA,
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
    {B_WERR, "-Werror", ARG_FLAG, 0, 0, NULL, "promote all (enabled) lints to errors"},
    {B_FORBID_ALLOW, "--forbid-allow", ARG_FLAG, 0, 0, NULL, "reject any `@allow(...)` lint escape hatch in your code"},
    {B_UNCHECKED, "--unchecked", ARG_FLAG, 0, 0, NULL,
     "trusted/embedded build: strip implicit bounds checks (unannotated fallible ops become !undefined)"},
    {B_NO_ABORT, "--no-abort", ARG_FLAG, 0, 0, NULL,
     "crash-free build: reject any op resolving to `!abort` (implicit or explicit)"},
    {B_NO_IMPLICIT_ABORT, "--no-implicit-abort", ARG_FLAG, 0, 0, NULL,
     "reject only the implicit/default `!abort` — every fallible op must be annotated"},
    {B_ALLOW_UNDEFINED, "--allow-undefined", ARG_FLAG, 0, 0, NULL,
     "permit the raw, runtime-unsafe `!undefined` opt-out (forbidden by default)"},
    {B_SELECT, "--select", ARG_VALUE, 1, 0, "<dev>=<variant>",
     "select a device's variant subfolder (repeatable; overrides arche.toml and ARCHE_SELECT)"},
    {B_TARGET, "--target", ARG_VALUE, 0, 0, "<name>",
     "select the active target/platform profile (overrides arche.toml `target` and ARCHE_TARGET)"},
    {B_INCREMENTAL, "--incremental", ARG_FLAG, 0, 0, NULL,
     "device-granular incremental build: cache each device's object, reuse unchanged ones (no cross-unit inlining)"},
    {B_WHOLE_PROGRAM, "--whole-program", ARG_FLAG, 0, 0, NULL,
     "force a whole-program build (the default for build; full cross-device inlining)"},
    {B_EXPORTED_MUTABLE, "--exported-mutable", ARG_VALUE, 0, 0, "<level>",
     "exported-mutable-global lint (W0022): error (default) | warn | allow"},
    {B_PROC_LEAF, "--proc-leaf", ARG_VALUE, 0, 0, "<level>",
     "proc-calls-proc lint (W0028): warn (default) | error | allow"},
    {B_SYS_FOREIGN_WRITE, "--map-foreign-write", ARG_VALUE, 0, 0, "<level>",
     "map-writes-foreign-pool lint (W0024): error (default) | warn | allow"},
    {B_POOL_INDEX, "--pool-index", ARG_VALUE, 0, 0, "<level>",
     "pool-index-outside-query lint (W0029): warn (default) | error | allow"},
    {B_PROC_NOT_PRIMITIVE, "--proc-not-primitive", ARG_VALUE, 0, 0, "<level>",
     "proc-not-primitive lint (W0030): error (default) | warn | allow"},
    {B_DISCARDED_OK, "--discarded-ok", ARG_VALUE, 0, 0, "<level>",
     "discarded-ok lint (W0016): error (default) | warn | allow"},
    {B_EMIT_GPU, "--emit-gpu", ARG_VALUE, 0, 0, "<dir>",
     "also emit a GLSL compute shader per `@gpu` map into <dir> (side artifact; CPU build unchanged)"},
    {B_GPU, "--gpu", ARG_FLAG, 0, 0, NULL,
     "dispatch `run map @gpu` on the GPU at runtime (embeds SPIR-V; CPU fallback if no device/glslc)"},
    {B_WNO_LSA, "-Wno-large-stack-array", ARG_FLAG, 0, 0, NULL, "disable the large-stack-array lint (W0026)"},
    {B_WERR_LSA, "-Werror=large-stack-array", ARG_FLAG, 0, 0, NULL,
     "promote the large-stack-array lint (W0026) to an error"},
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
	int pcbf_en = 1, pcbf_we = 0, pne_en = 1, pne_we = 0, lsa_en = 1, lsa_we = 0;
	if (args_has(&p, B_WNO_PCBF))
		pcbf_en = 0;
	if (args_has(&p, B_WNO_PNE))
		pne_en = 0;
	if (args_has(&p, B_WNO_LSA))
		lsa_en = 0;
	if (args_has(&p, B_WERR_PCBF))
		pcbf_we = 1;
	if (args_has(&p, B_WERR_PNE))
		pne_we = 1;
	if (args_has(&p, B_WERR_LSA))
		lsa_we = 1;
	if (args_has(&p, B_WERR)) {
		pcbf_we = 1;
		pne_we = 1;
		lsa_we = 1;
	}
	semantic_set_lint_proc_could_be_func(pcbf_en, pcbf_we);
	semantic_set_lint_proc_no_effect(pne_en, pne_we);
	semantic_set_lint_large_stack_array(lsa_en, lsa_we);
	/* Bare `-Werror`: promote EVERY enabled lint to an error (not just the two named above). */
	if (args_has(&p, B_WERR))
		semantic_set_all_lints_werror(1);
	/* W0022 tri-state — applied after `-Werror` so an explicit `--exported-mutable=warn|allow` can
	 * still demote it (W0022 is error-by-default; this is the dedicated opt-out). */
	if (cli_apply_exported_mutable(args_value(&p, B_EXPORTED_MUTABLE)) != 0) {
		fprintf(stderr, "%s: --exported-mutable expects error|warn|allow\n", g_prog);
		args_usage(stderr, g_prog, "build", "[flags] <input.arche>", k_build_specs);
		return ARCHE_USAGE;
	}
	if (cli_apply_proc_leaf(args_value(&p, B_PROC_LEAF)) != 0) {
		fprintf(stderr, "%s: --proc-leaf expects error|warn|allow\n", g_prog);
		args_usage(stderr, g_prog, "build", "[flags] <input.arche>", k_build_specs);
		return ARCHE_USAGE;
	}
	/* W0024 tri-state — applied after `-Werror` so an explicit `--map-foreign-write=warn|allow` can
	 * still demote it (W0024 is error-by-default; this is the dedicated opt-out). */
	if (cli_apply_map_foreign_write(args_value(&p, B_SYS_FOREIGN_WRITE)) != 0) {
		fprintf(stderr, "%s: --map-foreign-write expects error|warn|allow\n", g_prog);
		args_usage(stderr, g_prog, "build", "[flags] <input.arche>", k_build_specs);
		return ARCHE_USAGE;
	}
	if (cli_apply_proc_not_primitive(args_value(&p, B_PROC_NOT_PRIMITIVE)) != 0) {
		fprintf(stderr, "%s: --proc-not-primitive expects error|warn|allow\n", g_prog);
		args_usage(stderr, g_prog, "build", "[flags] <input.arche>", k_build_specs);
		return ARCHE_USAGE;
	}
	if (cli_apply_discarded_ok(args_value(&p, B_DISCARDED_OK)) != 0) {
		fprintf(stderr, "%s: --discarded-ok expects error|warn|allow\n", g_prog);
		args_usage(stderr, g_prog, "build", "[flags] <input.arche>", k_build_specs);
		return ARCHE_USAGE;
	}
	if (cli_apply_pool_index(args_value(&p, B_POOL_INDEX)) != 0) {
		fprintf(stderr, "%s: --pool-index expects error|warn|allow\n", g_prog);
		args_usage(stderr, g_prog, "build", "[flags] <input.arche>", k_build_specs);
		return ARCHE_USAGE;
	}

	/* Crash-free enforcement (failure policies) + escape-hatch ban. */
	semantic_set_no_abort(args_has(&p, B_NO_ABORT));
	semantic_set_no_implicit_abort(args_has(&p, B_NO_IMPLICIT_ABORT));
	/* `!undefined` is forbidden by default; --allow-undefined opts in, and --unchecked (which strips
	 * implicit ops to undefined anyway) implies it. */
	semantic_set_allow_undefined(args_has(&p, B_ALLOW_UNDEFINED) || args_has(&p, B_UNCHECKED));
	semantic_set_forbid_allow(args_has(&p, B_FORBID_ALLOW));
	codegen_set_unchecked(args_has(&p, B_UNCHECKED));

	/* `--incremental`: device-granular separate compilation with a per-device object cache. `build`
	 * defaults to whole-program (release: full cross-device inlining); `--whole-program` is a hard
	 * override (beats `--incremental` and the env). (`arche run` defaults to incremental.) */
	if (args_has(&p, B_WHOLE_PROGRAM))
		codegen_force_whole_program();
	else
		codegen_set_per_unit(args_has(&p, B_INCREMENTAL));

	/* `--select dev=var` (repeatable): the highest-precedence backend selection, layered over
	 * arche.toml + ARCHE_SELECT when the front-end resolves variants. */
	for (int i = 0; i < p.hit_count; i++)
		if (p.hits[i].id == B_SELECT)
			variant_select_cli_set(p.hits[i].value);

	/* `--target <name>`: the active platform profile; one switch retargets every device. */
	if (args_has(&p, B_TARGET))
		variant_select_cli_target(args_value(&p, B_TARGET));
	variant_select_set_warnings(1); /* warn on a typo'd / undefined target (compiler only) */

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
	opts.emit_gpu_dir = args_value(&p, B_EMIT_GPU); /* NULL if not passed */
	opts.gpu = args_has(&p, B_GPU);

	int rc = compile_source(source, input_file, output_file, &opts);
	free(source);
	return rc;
}

const ArgSpec *build_specs(void) {
	return k_build_specs;
}
