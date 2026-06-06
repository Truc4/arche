#include "../parser/parser.h"
#include "../syntax/format_syntax.h"
#include "args.h"
#include "cli.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { F_CHECK = 1, F_WRITE };

static const ArgSpec k_fmt_specs[] = {
    {F_CHECK, "--check", ARG_FLAG, 0, 0, NULL, "exit non-zero if any file is not formatted; write nothing"},
    {F_WRITE, "-w --write", ARG_FLAG, 0, 0, NULL, "rewrite each file in place"},
    {0, NULL, ARG_FLAG, 0, 0, NULL, NULL},
};

/* Format `src` through the lossless syntax tree into a freshly malloc'd buffer (caller frees), via a
 * tmpfile() so we stay within C99 (no open_memstream). Returns NULL on parse error, after printing
 * the first few diagnostics for `path`. */
static char *format_to_buffer(const char *path, const char *src, long *out_len) {
	ParseResult pr = parse_source(src);
	if (pr.error_count > 0) {
		fprintf(stderr, "%s:\n", path);
		fprintf(stderr, "  [Line %d, Col %d] Error: %s\n", pr.errors[0].line, pr.errors[0].column,
		        pr.errors[0].message);
		parse_result_free(&pr);
		return NULL;
	}
	FILE *tmp = tmpfile();
	if (!tmp) {
		perror("tmpfile");
		parse_result_free(&pr);
		return NULL;
	}
	format_syntax(tmp, pr.syntax_root, src);
	parse_result_free(&pr);
	fflush(tmp);
	long n = ftell(tmp);
	rewind(tmp);
	char *buf = malloc((size_t)n + 1);
	if (fread(buf, 1, (size_t)n, tmp) != (size_t)n) {
		perror("read formatted output");
		free(buf);
		fclose(tmp);
		return NULL;
	}
	buf[n] = '\0';
	fclose(tmp);
	*out_len = n;
	return buf;
}

int fmt_run(int argc, char **argv, const GlobalOpts *g) {
	(void)g;
	ArgParse p;
	if (args_parse(k_fmt_specs, argc, argv, &p) != 0) {
		fprintf(stderr, "%s: %s\n", g_prog, p.err);
		args_usage(stderr, g_prog, "fmt", "[--check | --write] <files...>", k_fmt_specs);
		return ARCHE_USAGE;
	}
	if (p.want_help) {
		args_usage(stdout, g_prog, "fmt", "[--check | --write] <files...>", k_fmt_specs);
		return ARCHE_OK;
	}
	if (p.pos_count == 0) {
		fprintf(stderr, "%s: no files to format\n", g_prog);
		args_usage(stderr, g_prog, "fmt", "[--check | --write] <files...>", k_fmt_specs);
		return ARCHE_USAGE;
	}

	int check = args_has(&p, F_CHECK);
	int write = args_has(&p, F_WRITE);
	int rc = ARCHE_OK;

	/* Expand each argument: a file is taken as-is; a directory or `./...` recurses over its
	 * `.arche` files (gofmt / `arche test ./...` style). */
	CliPathList files = {0};
	for (int i = 0; i < p.pos_count; i++)
		cli_collect_arche(p.pos[i], &files);
	if (files.count == 0) {
		fprintf(stderr, "%s: no .arche files matched\n", g_prog);
		cli_pathlist_free(&files);
		return ARCHE_USAGE;
	}

	for (int i = 0; i < files.count; i++) {
		const char *path = files.items[i];
		char *src = cli_read_file(path);
		if (!src) {
			rc = ARCHE_ERR;
			continue;
		}
		long n = 0;
		char *out = format_to_buffer(path, src, &n);
		if (!out) {
			rc = ARCHE_ERR;
			free(src);
			continue;
		}
		int changed = (strcmp(out, src) != 0);
		if (check) {
			if (changed) {
				fprintf(stderr, "%s\n", path); /* report unformatted files, like `gofmt -l` */
				rc = ARCHE_ERR;
			}
		} else if (write) {
			if (changed) {
				FILE *f = fopen(path, "w");
				if (!f) {
					perror(path);
					rc = ARCHE_ERR;
				} else {
					fwrite(out, 1, (size_t)n, f);
					fclose(f);
				}
			}
		} else {
			fwrite(out, 1, (size_t)n, stdout);
		}
		free(out);
		free(src);
	}
	cli_pathlist_free(&files);
	return rc;
}

const ArgSpec *fmt_specs(void) {
	return k_fmt_specs;
}
