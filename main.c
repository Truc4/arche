#include "doctest/doctest_run.h"
#include "driver/compile.h"
#include "semantic/semantic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

#ifndef ARCHE_EXPLAIN_DIR
#define ARCHE_EXPLAIN_DIR "docs/explain"
#endif

static void usage(const char *prog) {
	fprintf(stderr, "Usage: %s [-o executable] [--link <path>] input.arche\n", prog);
	fprintf(stderr, "       %s [-emit-llvm -o output.ll] input.arche\n", prog);
	fprintf(stderr, "       --link <path>  Pass additional .c or .o file to cc at link time\n");
	exit(1);
}

/* Maximum number of --link paths accepted on one command line */
#define MAX_LINK_PATHS 32

int main(int argc, char *argv[]) {
	/* Subcommand dispatch. `arche test <file.arche>` runs the file's doctests;
	 * the bare `arche [flags] <file>` form compiles, as it always has. */
	if (argc >= 2 && strcmp(argv[1], "test") == 0) {
		int verbose = 0;
		for (int i = 2; i < argc; i++)
			if (strcmp(argv[i], "-v") == 0)
				verbose = 1;
		int nspec = 0, rc = 0;
		for (int i = 2; i < argc; i++) {
			if (argv[i][0] == '-')
				continue;
			rc |= doctest_run_path(argv[i], verbose); /* run every spec; fail if any fails */
			nspec++;
		}
		if (nspec == 0) {
			fprintf(stderr, "usage: %s test [-v] <file.arche | dir | ./...> ...\n", argv[0]);
			return 1;
		}
		return rc ? 1 : 0;
	}

	const char *input_file = NULL;
	const char *output_file = NULL;
	int emit_llvm = 0;

	/* Extra files to pass to cc at link time (--link <path>) */
	const char *link_paths[MAX_LINK_PATHS];
	int link_count = 0;

	/* Lint config — both on by default; CLI can disable or promote to errors. */
	int lint_pcbf_enabled = 1, lint_pcbf_werror = 0;
	int lint_pne_enabled = 1, lint_pne_werror = 0;

	/* `--explain <code>` prints long-form help for a diagnostic code (e.g.
	 * `--explain E0001`) from docs/explain/<code>.md, then exits. Codes are
	 * stable forever (see docs/DIAGNOSTICS.md); the markdown files are added
	 * incrementally — a missing file prints a short fallback rather than failing. */
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--explain") == 0) {
			if (i + 1 >= argc) {
				fprintf(stderr, "usage: %s --explain <code> (e.g. E0001)\n", argv[0]);
				return 1;
			}
			const char *code = argv[i + 1];
			char path[512];
			snprintf(path, sizeof(path), "%s/%s.md", ARCHE_EXPLAIN_DIR, code);
			FILE *ef = fopen(path, "r");
			if (!ef) {
				fprintf(stderr, "%s: no long-form explanation yet.\n", code);
				fprintf(stderr, "(would live at %s — contributions welcome; see docs/DIAGNOSTICS.md)\n", path);
				return 1;
			}
			char buf[4096];
			size_t n;
			while ((n = fread(buf, 1, sizeof(buf), ef)) > 0)
				fwrite(buf, 1, n, stdout);
			fclose(ef);
			return 0;
		}
	}

	/* Parse command-line arguments */
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-o") == 0) {
			if (i + 1 >= argc) {
				usage(argv[0]);
			}
			output_file = argv[++i];
		} else if (strcmp(argv[i], "--link") == 0) {
			if (i + 1 >= argc) {
				usage(argv[0]);
			}
			if (link_count < MAX_LINK_PATHS) {
				link_paths[link_count++] = argv[++i];
			} else {
				fprintf(stderr, "Error: too many --link arguments (max %d)\n", MAX_LINK_PATHS);
				return 1;
			}
		} else if (strcmp(argv[i], "-emit-llvm") == 0) {
			emit_llvm = 1;
		} else if (strcmp(argv[i], "-Wno-proc-could-be-func") == 0) {
			lint_pcbf_enabled = 0;
		} else if (strcmp(argv[i], "-Wno-proc-no-effect") == 0) {
			lint_pne_enabled = 0;
		} else if (strcmp(argv[i], "-Werror=proc-could-be-func") == 0) {
			lint_pcbf_werror = 1;
		} else if (strcmp(argv[i], "-Werror=proc-no-effect") == 0) {
			lint_pne_werror = 1;
		} else if (strcmp(argv[i], "-Werror") == 0) {
			lint_pcbf_werror = 1;
			lint_pne_werror = 1;
		} else if (argv[i][0] != '-') {
			input_file = argv[i];
		}
	}

	semantic_set_lint_proc_could_be_func(lint_pcbf_enabled, lint_pcbf_werror);
	semantic_set_lint_proc_no_effect(lint_pne_enabled, lint_pne_werror);

	if (!input_file) {
		usage(argv[0]);
	}

	/* Limit memory to 512MB to prevent runaway compilation */
	struct rlimit mem_limit;
	mem_limit.rlim_cur = 512 * 1024 * 1024;
	mem_limit.rlim_max = 512 * 1024 * 1024;
	int limit_result = setrlimit(RLIMIT_AS, &mem_limit);
	if (limit_result != 0) {
		perror("Error: Could not set memory limit");
		return 1;
	}

	if (!output_file) {
		/* Default output: build/basename without extension */
		const char *base = strrchr(input_file, '/');
		if (!base)
			base = input_file;
		else
			base++;

		int len = strlen(base) + 20;
		output_file = malloc(len);
		strcpy((char *)output_file, "build/");
		strcat((char *)output_file, base);
		char *dot = strrchr((char *)output_file, '.');
		if (dot) {
			strcpy(dot, "");
		}
	}

	/* Read input file */
	FILE *input = fopen(input_file, "r");
	if (!input) {
		perror("Failed to open input file");
		return 1;
	}

	fseek(input, 0, SEEK_END);
	long file_size = ftell(input);
	fseek(input, 0, SEEK_SET);

	char *source = malloc(file_size + 1);
	if (fread(source, 1, file_size, input) != (size_t)file_size) {
		perror("Failed to read input file");
		free(source);
		fclose(input);
		return 1;
	}
	source[file_size] = '\0';
	fclose(input);

	/* Drive the whole pipeline through the shared compile entry point (core
	 * prepend → parse → resolve uses → semantic → lower → codegen → opt/llc/cc).
	 * The doctest runner and any future tool call the same function. */
	CompileOpts opts = {0};
	opts.emit_llvm = emit_llvm;
	opts.link_count = link_count;
	for (int li = 0; li < link_count; li++)
		opts.link_paths[li] = link_paths[li];

	int rc = compile_source(source, input_file, output_file, &opts);
	free(source);
	return rc;
}
