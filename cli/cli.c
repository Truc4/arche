/* opendir/readdir/stat (directory entry-point resolution) are POSIX; expose under -std=c99. */
#define _POSIX_C_SOURCE 200809L
#include "cli.h"
#include "../semantic/semantic.h"
#include "resource.h"
#include <dirent.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

const char *g_prog = "arche";

int cli_apply_exported_mutable(const char *value) {
	if (!value)
		return 0; /* flag absent — keep the error-by-default (set in ensure_init) */
	if (strcmp(value, "error") == 0)
		semantic_set_lint_exported_mutable_global(1, 1);
	else if (strcmp(value, "warn") == 0)
		semantic_set_lint_exported_mutable_global(1, 0);
	else if (strcmp(value, "allow") == 0)
		semantic_set_lint_exported_mutable_global(0, 0);
	else
		return -1; /* unknown level */
	return 0;
}

int cli_apply_proc_leaf(const char *value) {
	if (!value)
		return 0; /* flag absent — keep the warn-by-default (set in ensure_init) */
	if (strcmp(value, "error") == 0)
		semantic_set_lint_proc_calls_proc(1, 1);
	else if (strcmp(value, "warn") == 0)
		semantic_set_lint_proc_calls_proc(1, 0);
	else if (strcmp(value, "allow") == 0)
		semantic_set_lint_proc_calls_proc(0, 0);
	else
		return -1; /* unknown level */
	return 0;
}

int cli_apply_map_foreign_write(const char *value) {
	if (!value)
		return 0; /* flag absent — keep the error-by-default (set in ensure_init) */
	if (strcmp(value, "error") == 0)
		semantic_set_lint_map_writes_foreign_pool(1, 1);
	else if (strcmp(value, "warn") == 0)
		semantic_set_lint_map_writes_foreign_pool(1, 0);
	else if (strcmp(value, "allow") == 0)
		semantic_set_lint_map_writes_foreign_pool(0, 0);
	else
		return -1; /* unknown level */
	return 0;
}

int cli_apply_pool_index(const char *value) {
	if (!value)
		return 0; /* flag absent — keep the warn-by-default */
	if (strcmp(value, "error") == 0)
		semantic_set_lint_pool_index_outside_query(1, 1);
	else if (strcmp(value, "warn") == 0)
		semantic_set_lint_pool_index_outside_query(1, 0);
	else if (strcmp(value, "allow") == 0)
		semantic_set_lint_pool_index_outside_query(0, 0);
	else
		return -1; /* unknown level */
	return 0;
}

int cli_apply_proc_not_primitive(const char *value) {
	if (!value)
		return 0; /* flag absent — keep the error-by-default (set in ensure_init) */
	if (strcmp(value, "error") == 0)
		semantic_set_lint_proc_not_primitive(1, 1);
	else if (strcmp(value, "warn") == 0)
		semantic_set_lint_proc_not_primitive(1, 0);
	else if (strcmp(value, "allow") == 0)
		semantic_set_lint_proc_not_primitive(0, 0);
	else
		return -1; /* unknown level */
	return 0;
}

int cli_apply_discarded_ok(const char *value) {
	if (!value)
		return 0; /* flag absent — keep the error-by-default (set in ensure_init) */
	if (strcmp(value, "error") == 0)
		semantic_set_lint_discarded_ok(1, 1);
	else if (strcmp(value, "warn") == 0)
		semantic_set_lint_discarded_ok(1, 0);
	else if (strcmp(value, "allow") == 0)
		semantic_set_lint_discarded_ok(0, 0);
	else
		return -1; /* unknown level */
	return 0;
}

static int cli_has_arche_ext(const char *name) {
	size_t n = strlen(name);
	return n > 6 && strcmp(name + n - 6, ".arche") == 0;
}

/* Directories never descended into during a recursive walk: dotdirs, build artifacts, deps. */
static int cli_skip_dir(const char *name) {
	return name[0] == '.' || strcmp(name, "build") == 0 || strcmp(name, "node_modules") == 0 ||
	       strcmp(name, "site-packages") == 0;
}

static void cli_pl_push(CliPathList *pl, const char *path) {
	if (pl->count >= pl->cap) {
		pl->cap = pl->cap ? pl->cap * 2 : 32;
		pl->items = realloc(pl->items, (size_t)pl->cap * sizeof(char *));
	}
	pl->items[pl->count++] = strdup(path);
}

static void cli_recurse_arche(const char *dir, CliPathList *pl) {
	DIR *d = opendir(dir);
	if (!d)
		return;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
			continue;
		char full[1024];
		snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);
		struct stat sb;
		if (stat(full, &sb) != 0)
			continue;
		if (S_ISDIR(sb.st_mode)) {
			if (!cli_skip_dir(e->d_name))
				cli_recurse_arche(full, pl);
		} else if (S_ISREG(sb.st_mode) && cli_has_arche_ext(e->d_name)) {
			cli_pl_push(pl, full);
		}
	}
	closedir(d);
}

static int cli_cmp_str(const void *a, const void *b) {
	return strcmp(*(const char *const *)a, *(const char *const *)b);
}

void cli_collect_arche(const char *spec, CliPathList *pl) {
	size_t n = strlen(spec);
	int start = pl->count;
	if (n >= 3 && strcmp(spec + n - 3, "...") == 0) {
		/* `<dir>/...` (or bare `...`) → recurse from <dir>, defaulting to "." */
		char dir[1024];
		size_t dn = n - 3;
		while (dn > 0 && spec[dn - 1] == '/') /* drop the slash before `...` */
			dn--;
		if (dn == 0) {
			strcpy(dir, ".");
		} else {
			if (dn >= sizeof(dir))
				dn = sizeof(dir) - 1;
			memcpy(dir, spec, dn);
			dir[dn] = '\0';
		}
		cli_recurse_arche(dir, pl);
	} else {
		struct stat sb;
		if (stat(spec, &sb) == 0 && S_ISDIR(sb.st_mode))
			cli_recurse_arche(spec, pl);
		else
			cli_pl_push(pl, spec); /* literal path, as given */
	}
	/* sort just the entries this call added, for deterministic output */
	if (pl->count - start > 1)
		qsort(pl->items + start, (size_t)(pl->count - start), sizeof(char *), cli_cmp_str);
}

void cli_pathlist_free(CliPathList *pl) {
	for (int i = 0; i < pl->count; i++)
		free(pl->items[i]);
	free(pl->items);
	pl->items = NULL;
	pl->count = pl->cap = 0;
}

char *cli_resolve_input(const char *path) {
	struct stat st;
	if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode))
		return strdup(path); /* not a directory (or doesn't exist) — use verbatim; fopen reports later */

	/* Directory: prefer `main.arche`, else the single `.arche` file in it (go-run-`.` semantics). */
	char cand[1024];
	snprintf(cand, sizeof(cand), "%s/main.arche", path);
	if (stat(cand, &st) == 0 && S_ISREG(st.st_mode))
		return strdup(cand);

	DIR *d = opendir(path);
	if (!d) {
		fprintf(stderr, "%s: cannot open directory '%s'\n", g_prog, path);
		return NULL;
	}
	char *found = NULL;
	int count = 0;
	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		size_t L = strlen(e->d_name);
		if (L > 6 && strcmp(e->d_name + L - 6, ".arche") == 0) {
			count++;
			if (!found) {
				snprintf(cand, sizeof(cand), "%s/%s", path, e->d_name);
				found = strdup(cand);
			}
		}
	}
	closedir(d);
	if (count == 0) {
		fprintf(stderr, "%s: no .arche file in '%s'\n", g_prog, path);
		free(found);
		return NULL;
	}
	if (count > 1) {
		fprintf(stderr, "%s: multiple .arche files in '%s' — name one, or add a main.arche\n", g_prog, path);
		free(found);
		return NULL;
	}
	return found;
}

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
    {"init", "scaffold a device or driver (arche init <device|driver> <name>)", 0, init_run, NULL},
    {"fill", "size a driver's pools from its imported devices' datasheets", 0, fill_run, NULL},
    {"inspect", "view/edit a running `arche run` session's pools", 0, inspect_run, inspect_specs},
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
	/* Behave like a well-mannered Unix tool: when a downstream consumer closes the pipe early
	 * (e.g. `arche completion bash | grep -q _arche`), writes should fail with EPIPE and let us
	 * exit cleanly rather than dying with signal 13 / exit 141. */
	signal(SIGPIPE, SIG_IGN);

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

	/* Unknown command. */
	fprintf(stderr, "%s: unknown command '%s'\n\n", g_prog, a1);
	print_top_help(stderr);
	return ARCHE_USAGE;
}
