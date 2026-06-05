#include "../cst/syntax_tree.h"
#include "../lexer/lexer.h"
#include "../parser/parser.h"
#include "cli.h"
#include "resource.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* `arche fill <driver>` — read the device datasheets the driver imports and write a pool decl into
 * the driver for each required shape it does not already size, at the datasheet's minimum. The driver
 * owns all storage; this is the "256 for free" convenience: the size lands in YOUR source, editable.
 * Idempotent — a shape already sized in the driver is left untouched (your edits are never changed).
 *
 * Shape sizing is keyed off the shape's own (last-segment) name: a datasheet requirement `Node[4]`
 * matches a driver pool `Node[...]` or `dev.Node[...]`. Shared requirements compose by `max`. */

#define FILL_MAX 256

typedef struct {
	char name[128]; /* shape's own (last-segment) name */
	int min;        /* composed minimum (max across datasheets) */
} ReqEntry;

/* Copy a token's source text into `out` (truncating to cap-1). */
static void tok_text(const char *src, const SyntaxElem *e, char *out, size_t cap) {
	size_t L = e->as.token.length;
	if (L > cap - 1)
		L = cap - 1;
	memcpy(out, src + e->as.token.offset, L);
	out[L] = '\0';
}

/* Find the first NUMBER token anywhere in `node`'s subtree (DFS), or -1. The capacity in `Name[C]`
 * is parsed as a nested expression, so it isn't a direct token child. */
static int first_number(const SyntaxNode *node, const char *src) {
	for (int i = 0; i < node->child_count; i++) {
		const SyntaxElem *ch = &node->children[i];
		if (ch->tag == SE_TOKEN && ch->as.token.kind == TOK_NUMBER) {
			char buf[32];
			tok_text(src, ch, buf, sizeof(buf));
			return atoi(buf);
		}
		if (ch->tag == SE_NODE) {
			int n = first_number(ch->as.node, src);
			if (n >= 0)
				return n;
		}
	}
	return -1;
}

/* For a static/pool decl node `Name[N]` (or `dev.Name[N]`), write the shape's OWN name (the last
 * dotted IDENT before `[`) into `name`, and return the capacity literal (the first number), or -1. */
static int static_decl_shape(const SyntaxNode *node, const char *src, char *name, size_t cap) {
	int found_name = 0;
	name[0] = '\0';
	for (int i = 0; i < node->child_count; i++) {
		const SyntaxElem *ch = &node->children[i];
		if (ch->tag != SE_TOKEN)
			continue;
		if (ch->as.token.kind == TOK_LBRACKET)
			break; /* name is the dotted IDENT run before `[`; capacity follows (nested) */
		if (ch->as.token.kind == TOK_IDENT) {
			tok_text(src, ch, name, cap); /* keep the LAST IDENT before `[` = the shape's own name */
			found_name = 1;
		}
	}
	return found_name ? first_number(node, src) : -1;
}

/* Locate device `dev`'s datasheet (`<dir>/<dev>/<dev>.ds.arche`), searching the driver's source dir
 * then the stdlib. Writes the path into `out`; returns 1 if found. */
static int find_datasheet(const char *dev, const char *source_dir, char *out, size_t cap) {
	const char *dirs[2] = {source_dir, arche_resource_dir(ARCHE_RES_STDLIB)};
	for (int i = 0; i < 2; i++) {
		if (!dirs[i])
			continue;
		snprintf(out, cap, "%s/%s/%s.ds.arche", dirs[i], dev, dev);
		FILE *f = fopen(out, "r");
		if (f) {
			fclose(f);
			return 1;
		}
	}
	return 0;
}

/* Record a requirement (shape, min) into `reqs`, composing by `max` if the shape is already present. */
static void req_add(ReqEntry *reqs, int *n, const char *name, int min) {
	for (int i = 0; i < *n; i++) {
		if (strcmp(reqs[i].name, name) == 0) {
			if (min > reqs[i].min)
				reqs[i].min = min;
			return;
		}
	}
	if (*n < FILL_MAX) {
		snprintf(reqs[*n].name, sizeof(reqs[*n].name), "%s", name);
		reqs[*n].min = min;
		(*n)++;
	}
}

/* Collect the storage requirements (pool decls) from one datasheet file into `reqs`. */
static void collect_datasheet_reqs(const char *path, ReqEntry *reqs, int *n) {
	char *src = cli_read_file(path);
	if (!src)
		return;
	ParseResult pr = parse_source(src);
	if (pr.cst_root) {
		for (int i = 0; i < pr.cst_root->child_count; i++) {
			if (pr.cst_root->children[i].tag != SE_NODE)
				continue;
			const SyntaxNode *cn = pr.cst_root->children[i].as.node;
			if (cn->kind != SN_STATIC_DECL)
				continue;
			char shape[128];
			int capn = static_decl_shape(cn, src, shape, sizeof(shape));
			if (shape[0] && capn > 0)
				req_add(reqs, n, shape, capn);
		}
	}
	parse_result_free(&pr);
	free(src);
}

/* dirname of `path` into `out` (".", if no slash). */
static void path_dir(const char *path, char *out, size_t cap) {
	const char *slash = strrchr(path, '/');
	if (!slash) {
		snprintf(out, cap, ".");
		return;
	}
	size_t L = (size_t)(slash - path);
	if (L > cap - 1)
		L = cap - 1;
	memcpy(out, path, L);
	out[L] = '\0';
}

/* Fill `driver_path`: append a pool decl for every imported device's required shape not already sized
 * in the driver. Returns the number of pools written (0 = nothing to do), or -1 on error. */
int arche_fill_driver(const char *driver_path) {
	char *src = cli_read_file(driver_path);
	if (!src)
		return -1;
	ParseResult pr = parse_source(src);
	if (!pr.cst_root) {
		fprintf(stderr, "arche fill: cannot parse '%s'\n", driver_path);
		parse_result_free(&pr);
		free(src);
		return -1;
	}

	char source_dir[600];
	path_dir(driver_path, source_dir, sizeof(source_dir));

	/* Shapes the driver already sizes — never touch these. */
	char have[FILL_MAX][128];
	int have_n = 0;
	/* Devices the driver imports. */
	char devs[FILL_MAX][128];
	int dev_n = 0;
	for (int i = 0; i < pr.cst_root->child_count; i++) {
		if (pr.cst_root->children[i].tag != SE_NODE)
			continue;
		const SyntaxNode *cn = pr.cst_root->children[i].as.node;
		if (cn->kind == SN_STATIC_DECL) {
			char shape[128];
			static_decl_shape(cn, src, shape, sizeof(shape));
			if (shape[0] && have_n < FILL_MAX)
				snprintf(have[have_n++], 128, "%s", shape);
		} else if (cn->kind == SN_USE_DECL) {
			for (int k = 0; k < cn->child_count; k++) {
				if (cn->children[k].tag == SE_TOKEN && cn->children[k].as.token.kind == TOK_IDENT && dev_n < FILL_MAX)
					tok_text(src, &cn->children[k], devs[dev_n++], 128);
			}
		}
	}

	/* Gather requirements from every imported device's datasheet (composed by max). */
	ReqEntry reqs[FILL_MAX];
	int req_n = 0;
	for (int d = 0; d < dev_n; d++) {
		char ds[800];
		if (find_datasheet(devs[d], source_dir, ds, sizeof(ds)))
			collect_datasheet_reqs(ds, reqs, &req_n);
	}

	parse_result_free(&pr);
	free(src);

	/* Append a pool for each required shape the driver does not already size. */
	int written = 0;
	FILE *out = NULL;
	for (int r = 0; r < req_n; r++) {
		int already = 0;
		for (int h = 0; h < have_n; h++)
			if (strcmp(have[h], reqs[r].name) == 0) {
				already = 1;
				break;
			}
		if (already)
			continue;
		if (!out) {
			out = fopen(driver_path, "a");
			if (!out) {
				fprintf(stderr, "arche fill: cannot write '%s'\n", driver_path);
				return -1;
			}
			fputs("\n// storage filled by `arche fill` from device datasheets (edit sizes as needed)\n", out);
		}
		fprintf(out, "%s[%d]\n", reqs[r].name, reqs[r].min);
		printf("filled %s[%d]\n", reqs[r].name, reqs[r].min);
		written++;
	}
	if (out)
		fclose(out);
	return written;
}

int fill_run(int argc, char **argv, const GlobalOpts *g) {
	(void)g;
	if (argc < 2) {
		fprintf(stderr, "usage: arche fill <driver.arche | dir>\n");
		return ARCHE_USAGE;
	}
	char *input = cli_resolve_input(argv[1]);
	if (!input)
		return ARCHE_ERR;
	int n = arche_fill_driver(input);
	free(input);
	if (n < 0)
		return ARCHE_ERR;
	if (n == 0)
		printf("arche fill: nothing to do (all required pools already sized)\n");
	return ARCHE_OK;
}
