#include "cli.h"
#include "resource.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *g_prog = "arche";

char *cli_read_file(const char *path) {
	FILE *f = fopen(path, "r");
	if (!f) {
		perror("Failed to open input file");
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *buf = malloc((size_t)size + 1);
	if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
		perror("Failed to read input file");
		free(buf);
		fclose(f);
		return NULL;
	}
	buf[size] = '\0';
	fclose(f);
	return buf;
}

/* The dispatch table. New subcommands land here (one row) + their own cmd_*.c. During the migration
 * the bare form `arche <file>` is an implicit `build` (see cli_main); a later phase removes that. */
static const SubCmd k_cmds[] = {
    {"build", "compile a source file to an executable", 0, build_run, build_specs},
    {"run", "compile and immediately run a source file", 0, run_run, run_specs},
    {"check", "parse + type-check without producing output", 0, check_run, check_specs},
    {"test", "run the doctests in source files", 0, test_run, test_specs},
    {"fmt", "format source files", 0, fmt_run, fmt_specs},
    {"explain", "show the long-form help for a diagnostic code", 0, explain_run, NULL},
    {"analyze", "language-server analysis (one-shot or --serve)", 0, analyze_run, analyze_specs},
    {"completion", "print a shell completion script (bash|zsh|fish)", 0, completion_run, NULL},
    {"version", "print the arche version", 0, version_run, NULL},
};
static const int k_cmd_count = (int)(sizeof(k_cmds) / sizeof(k_cmds[0]));

const SubCmd *cli_commands(int *count) {
	if (count)
		*count = k_cmd_count;
	return k_cmds;
}

static const SubCmd *find_cmd(const char *name) {
	for (int i = 0; i < k_cmd_count; i++)
		if (strcmp(k_cmds[i].name, name) == 0)
			return &k_cmds[i];
	return NULL;
}

static void print_top_help(FILE *f) {
	fprintf(f, "arche — the Arche compiler\n\n");
	fprintf(f, "usage: %s <command> [arguments]\n\n", g_prog);
	fprintf(f, "commands:\n");
	for (int i = 0; i < k_cmd_count; i++)
		if (!k_cmds[i].hidden)
			fprintf(f, "  %-10s %s\n", k_cmds[i].name, k_cmds[i].summary);
	fprintf(f, "\nrun `%s <command> --help` for command-specific flags.\n", g_prog);
}

int explain_print(const char *code) {
	char path[512];
	snprintf(path, sizeof(path), "%s/%s.md", arche_resource_dir(ARCHE_RES_EXPLAIN), code);
	FILE *ef = fopen(path, "r");
	if (!ef) {
		fprintf(stderr, "%s: no long-form explanation yet.\n", code);
		fprintf(stderr, "(would live at %s — contributions welcome; see docs/DIAGNOSTICS.md)\n", path);
		return ARCHE_ERR;
	}
	char buf[4096];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), ef)) > 0)
		fwrite(buf, 1, n, stdout);
	fclose(ef);
	return ARCHE_OK;
}

/* Legacy `--explain <code>` form (scanned anywhere on the line); the `explain` subcommand is the
 * preferred spelling. */
static int do_explain(int argc, char **argv, int at) {
	if (at + 1 >= argc) {
		fprintf(stderr, "usage: %s --explain <code> (e.g. E0001)\n", g_prog);
		return ARCHE_ERR;
	}
	return explain_print(argv[at + 1]);
}

int cli_main(int argc, char **argv) {
	if (argc >= 1 && argv[0]) {
		const char *slash = strrchr(argv[0], '/');
		g_prog = slash ? slash + 1 : argv[0];
	}

	GlobalOpts g = {0};
	g.color = COLOR_AUTO;

	/* Legacy `--explain <code>` may appear anywhere on the line. */
	for (int i = 1; i < argc; i++)
		if (strcmp(argv[i], "--explain") == 0)
			return do_explain(argc, argv, i);

	if (argc < 2) {
		print_top_help(stderr);
		return ARCHE_USAGE;
	}

	const char *a1 = argv[1];
	if (strcmp(a1, "--version") == 0) {
		printf("arche %s\n", arche_version_string());
		return ARCHE_OK;
	}
	if (strcmp(a1, "-h") == 0 || strcmp(a1, "--help") == 0 || strcmp(a1, "help") == 0) {
		/* `help <cmd>` defers to the command's own spec-generated usage (inject `--help`). */
		if (argc >= 3) {
			const SubCmd *hc = find_cmd(argv[2]);
			if (hc) {
				char *help_argv[] = {(char *)hc->name, (char *)"--help"};
				return hc->run(2, help_argv, &g);
			}
		}
		print_top_help(stdout);
		return ARCHE_OK;
	}

	const SubCmd *c = find_cmd(a1);
	if (c)
		return c->run(argc - 1, argv + 1, &g);

	/* Unknown command. A bare source path is the most common mistake (the old implicit-build form),
	 * so point at `build` explicitly. */
	size_t len = strlen(a1);
	if (len > 6 && strcmp(a1 + len - 6, ".arche") == 0)
		fprintf(stderr, "%s: '%s' is not a command. Did you mean `%s build %s`?\n", g_prog, a1, g_prog, a1);
	else
		fprintf(stderr, "%s: unknown command '%s'\n\n", g_prog, a1);
	print_top_help(stderr);
	return ARCHE_USAGE;
}
