/* arche-analyzer — editor analysis service for the arche language server.
 *
 * Runs the compiler's real front-end (prepend core.arche, parse, resolve `use`
 * modules, semantic analysis — exactly as main.c does) and emits an "explicit
 * view" of the program: the implicit information the compiler infers, anchored
 * to source positions, for the editor to render as inlay hints (and, later,
 * diagnostics). The analyzer is the single source of truth — the CST drives
 * syntactic facts via typed views, the node-id-keyed SemModel drives inferred
 * facts — so the output tracks the language as it evolves.
 *
 * Output lines (positions are 1-based, translated to USER-file coordinates):
 *   SYN <line> <col> <padL> <padR> <kind> <text...>   inlay hint
 *   DIAG <line> <col> <severity> <name> <msg...>      diagnostic (error|warning)
 *
 * Modes:
 *   --dump [file]   one-shot: analyze file (or stdin) and print all lines.
 *   --serve         persistent: warm per-document analysis over a line protocol
 *                   (UPDATE / TOKENS / HINTS / CLOSE), driven by the LSP server.
 *
 * core.arche is prepended before analysis, so every emitted position is in
 * combined (core+user) coordinates; we subtract the core line count and drop
 * anything inside the core region so the editor sees its own buffer's lines.
 */
#include "cst/cst_view.h"
#include "cst/syntax_tree.h"
#include "cst/token_category.h"
#include "lexer/lexer.h"
#include "parser/parser.h"
#include "semantic/semantic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifndef ARCHE_CORE_DIR
#define ARCHE_CORE_DIR "core"
#endif

/* C99 doesn't expose strdup; declare it explicitly (as codegen.c / sem_model.c do). */
char *strdup(const char *s);

/* ---- small file helpers (mirrors main.c; analysis-only, no lower/codegen) ---- */

static char *read_stream(FILE *f) {
	size_t cap = 4096, len = 0;
	char *buf = malloc(cap);
	if (!buf)
		return NULL;
	size_t n;
	while ((n = fread(buf + len, 1, cap - len, f)) > 0) {
		len += n;
		if (len == cap) {
			cap *= 2;
			char *grown = realloc(buf, cap);
			if (!grown) {
				free(buf);
				return NULL;
			}
			buf = grown;
		}
	}
	buf[len] = '\0';
	return buf;
}

static char *read_file(const char *path) {
	FILE *f = fopen(path, "r");
	if (!f)
		return NULL;
	char *s = read_stream(f);
	fclose(f);
	return s;
}

static int file_exists(const char *path) {
	struct stat sb;
	return stat(path, &sb) == 0;
}

static char *source_dir_of(const char *path) {
	const char *slash = strrchr(path, '/');
	if (!slash) {
		char *d = malloc(2);
		strcpy(d, ".");
		return d;
	}
	int len = (int)(slash - path);
	char *d = malloc(len + 1);
	memcpy(d, path, len);
	d[len] = '\0';
	return d;
}

static int count_newlines(const char *s) {
	int n = 0;
	for (; *s; s++)
		if (*s == '\n')
			n++;
	return n;
}

/* ---- module loading: register `use` modules with semantic only ---- */

typedef struct {
	SyntaxNode *root;
	char *src;
} ModuleHold;

typedef struct {
	ModuleHold *items;
	int count, cap;
} ModuleHolds;

static void holds_push(ModuleHolds *h, SyntaxNode *root, char *src) {
	if (h->count >= h->cap) {
		h->cap = h->cap ? h->cap * 2 : 4;
		h->items = realloc(h->items, (size_t)h->cap * sizeof(ModuleHold));
	}
	h->items[h->count].root = root;
	h->items[h->count].src = src;
	h->count++;
}

static void holds_free(ModuleHolds *h) {
	for (int i = 0; i < h->count; i++) {
		syntax_node_free(h->items[i].root);
		free(h->items[i].src);
	}
	free(h->items);
}

/* Parse each `use <name>;` module and register its CST with the semantic analyzer
 * (which inlines + name-prefixes it). Analysis-only: unlike main.c's resolve_uses
 * we don't touch the lowerer. The module CST + source are borrowed by the registry,
 * so we keep them alive in `holds` until after analysis. */
static void resolve_uses_sem(const SyntaxNode *root, const char *src, const char *path, ModuleHolds *holds) {
	if (!root)
		return;
	for (int u = 0; u < root->child_count; u++) {
		if (root->children[u].tag != SE_NODE || root->children[u].as.node->kind != SN_USE_DECL)
			continue;
		const SyntaxNode *ud = root->children[u].as.node;

		char mod_name[256];
		mod_name[0] = '\0';
		for (int k = 0; k < ud->child_count; k++)
			if (ud->children[k].tag == SE_TOKEN && ud->children[k].as.token.kind == TOK_IDENT) {
				size_t L = ud->children[k].as.token.length;
				if (L > sizeof(mod_name) - 1)
					L = sizeof(mod_name) - 1;
				memcpy(mod_name, src + ud->children[k].as.token.offset, L);
				mod_name[L] = '\0';
				break;
			}
		if (!mod_name[0])
			continue;

		char *dir = source_dir_of(path);
		char path1[512], path2[512];
		snprintf(path1, sizeof(path1), "%s/%s.arche", dir, mod_name);
		free(dir);
		snprintf(path2, sizeof(path2), "%s/%s.arche", ARCHE_CORE_DIR, mod_name);
		const char *found = file_exists(path1) ? path1 : (file_exists(path2) ? path2 : NULL);
		if (!found)
			continue;

		char *mod_src = read_file(found);
		if (!mod_src)
			continue;
		ParseResult mp = parse_source(mod_src);
		if (mp.error_count > 0 || !mp.cst_root) {
			parse_result_free(&mp);
			free(mod_src);
			continue;
		}
		semantic_add_module(mod_name, mp.cst_root, mod_src);
		holds_push(holds, mp.cst_root, mod_src); /* keep alive; registry borrows */
		mp.cst_root = NULL;
		parse_result_free(&mp);
	}
}

/* ---- explicit-view emission ---- */

static int g_core_lines; /* combined→user line translation: user_line = line - g_core_lines */

/* core.arche is invariant across documents and edits, so read + measure it once.
 * This is the main saving that makes the warm server cheap: every analysis reuses
 * the cached prelude instead of re-reading 130+ lines from disk per keystroke. */
static char *g_core;
static int g_core_loaded;

static void ensure_core(void) {
	if (g_core_loaded)
		return;
	g_core_loaded = 1;
	char core_path[512];
	snprintf(core_path, sizeof(core_path), "%s/core.arche", ARCHE_CORE_DIR);
	g_core = read_file(core_path);
	g_core_lines = (g_core && g_core[0]) ? count_newlines(g_core) : 0;
}

/* Render an internal type name as it would be written in arche source (longhand). */
static const char *display_type(const char *ty) {
	if (strcmp(ty, "char_array") == 0)
		return "char[]";
	return ty;
}

/* Emit a SYN line, translating combined→user coords and dropping the core region.
 * Format: SYN <line> <col> <padL> <padR> <kind> <text>. Position is the column the
 * virtual text anchors at; padL/padR are render-time spaces around the label, so
 * the result reads as the longhand of the surrounding real tokens. */
static void emit_syn(int line, int col, int padL, int padR, const char *kind, const char *text) {
	int uline = line - g_core_lines;
	if (uline <= 0)
		return; /* inside the prepended core region */
	printf("SYN %d %d %d %d %s %s\n", uline, col, padL ? 1 : 0, padR ? 1 : 0, kind, text);
}

/* Inferred-type hint: for a bind with no written type, slot the inferred type
 * INTO the bind operator so the rendered view reads exactly as the longhand —
 *   `r := e`  →  `r : int = e`     (anchor before the `=`, pad both sides)
 *   `x :: e`  →  `x : int : e`     (anchor before the 2nd `:`, pad both sides)
 * Skips type-alias declarations and any bind that already states its type. */
static void emit_bind_hint(CstView bind, SemanticContext *ctx) {
	const SemModel *model = sem_context_model(ctx);
	if (sem_model_bind_alias(model, cv_id(bind)))
		return;
	if (cv_type_count(bind) > 0)
		return;
	CstView target = cv_expr_at(bind, 0);
	CstView value = cv_expr_at(bind, 1);
	if (!cv_present(target) || !cv_present(value) || cv_kind(target) != SN_NAME_EXPR)
		return;
	const char *ty = sem_model_expr_type(model, cv_id(value));
	if (!ty)
		return;
	/* Anchor at the 2nd separator token of the bind operator: the `=` of `:=`, or
	 * (when there is no `=`) the 2nd `:` of `::`. The type then renders between
	 * the two real tokens that the bind operator is made of. */
	CvPos anchor = cv_token_pos(bind, TOK_EQ);
	if (!anchor.line)
		anchor = cv_token_pos_at(bind, TOK_COLON, 1);
	if (!anchor.line)
		return;
	emit_syn(anchor.line, anchor.column, 1, 1, "type", display_type(ty));
}

/* A type position naming an alias shows its backing type, e.g. `count (int)`. */
static void emit_typeref_hint(CstView tr, SemanticContext *ctx) {
	CvText id = cv_token(tr, TOK_IDENT);
	if (!id.ptr)
		return;
	char name[256];
	size_t L = id.len < sizeof(name) - 1 ? id.len : sizeof(name) - 1;
	memcpy(name, id.ptr, L);
	name[L] = '\0';
	const char *backing = semantic_resolve_type_alias(ctx, name);
	if (!backing || strcmp(backing, name) == 0)
		return; /* not an alias (or already its own backing) */
	char text[260];
	snprintf(text, sizeof(text), "(%s)", display_type(backing));
	/* Annotation after the alias name: pad-left only (space between name and `(`). */
	CvPos end = cv_last_token_pos(tr);
	if (!end.line)
		return;
	emit_syn(end.line, end.column + (int)end.length, 1, 0, "alias", text);
}

/* Call-site parameter hint (gopls-style): show the resolved parameter's name
 * before the argument, prefixed `own ` when it takes ownership. Recorded by
 * semantic analysis (needs call resolution), keyed by the argument's node. */
static void emit_param_hint(CstView arg, SemanticContext *ctx) {
	const SemHints *h = sem_context_hints(ctx);
	const char *name = sem_hints_param_name(h, cv_id(arg));
	if (!name)
		return;
	char text[300];
	snprintf(text, sizeof(text), "%s%s:", sem_hints_param_is_own(h, cv_id(arg)) ? "own " : "", name);
	/* Anchor at the argument's start: pad-right only (space between `name:` and arg). */
	CvPos start = cv_first_token_pos(arg);
	if (!start.line)
		return;
	emit_syn(start.line, start.column, 0, 1, "param", text);
}

static void walk(CstView v, SemanticContext *ctx) {
	if (!v.node)
		return;
	emit_param_hint(v, ctx); /* any node may be a resolved call argument */
	if (cv_kind(v) == SN_BIND_STMT)
		emit_bind_hint(v, ctx);
	else if (cv_kind(v) == SN_TYPE_REF)
		emit_typeref_hint(v, ctx);
	for (int i = 0; i < v.node->child_count; i++)
		if (v.node->children[i].tag == SE_NODE) {
			CstView c = {v.node->children[i].as.node, v.src};
			walk(c, ctx);
		}
}

/* ---- analysis unit (shared by --dump and --serve) ---- */

typedef struct {
	char *combined;       /* core + user buffer; owned */
	SyntaxNode *cst_root; /* owned */
	SemanticContext *ctx; /* owned; NULL on failure */
	ModuleHolds holds;    /* owned module CSTs + sources */
} Analysis;

/* Analyze a user buffer (takes ownership of `user`), mirroring main.c's front-end:
 * prepend cached core, parse, resolve `use` modules (semantic-only), analyze. */
static Analysis analyze(char *user, const char *path) {
	Analysis a = {NULL, NULL, NULL, {NULL, 0, 0}};
	ensure_core();
	if (g_core && g_core[0]) {
		size_t cl = strlen(g_core), ul = strlen(user);
		a.combined = malloc(cl + ul + 1);
		memcpy(a.combined, g_core, cl);
		memcpy(a.combined + cl, user, ul + 1);
		free(user);
	} else {
		a.combined = user; /* ownership moved */
	}
	ParseResult pr = parse_source(a.combined);
	a.cst_root = pr.cst_root;
	pr.cst_root = NULL;
	parse_result_free(&pr);
	if (!a.cst_root)
		return a;
	resolve_uses_sem(a.cst_root, a.combined, path && path[0] ? path : ".", &a.holds);
	a.ctx = semantic_analyze_cst(a.cst_root, a.combined);
	return a;
}

static void analysis_free(Analysis *a) {
	if (a->ctx)
		semantic_context_free(a->ctx);
	holds_free(&a->holds);
	syntax_node_free(a->cst_root);
	free(a->combined);
	a->combined = NULL;
	a->cst_root = NULL;
	a->ctx = NULL;
	a->holds = (ModuleHolds){NULL, 0, 0};
}

static void emit_hints(const Analysis *a) {
	if (a->ctx)
		walk(cv_root(a->cst_root, a->combined), a->ctx);
}

/* Emit diagnostics collected during semantic analysis, translated to user coords.
 * Errors with no source position default to user line 1 col 1 so they still surface;
 * anything inside the prepended core region is dropped (belongs to the prelude). */
static void emit_diags(const Analysis *a) {
	if (!a->ctx)
		return;
	int n = sem_diag_count(a->ctx);
	for (int i = 0; i < n; i++) {
		const SemDiag *d = sem_diag_at(a->ctx, i);
		if (!d)
			continue;
		int line = 1, col = 1;
		if (d->has_loc) {
			int uline = d->loc.line - g_core_lines;
			if (uline <= 0)
				continue; /* inside the core prelude */
			line = uline;
			col = d->loc.column;
		}
		printf("DIAG %d %d %s %s %s\n", line, col, d->severity ? "error" : "warning", d->name, d->message);
	}
}

/* Syntax-highlighting tokens, same `offset length line col CATEGORY` format the
 * editor already consumes — but served from the warm parse and translated to user
 * coordinates (core-region tokens dropped). Needs only the CST, not analysis. */
static void walk_tokens(const SyntaxNode *node) {
	for (int i = 0; i < node->child_count; i++) {
		const SyntaxElem *e = &node->children[i];
		if (e->tag == SE_NODE) {
			walk_tokens(e->as.node);
		} else {
			const char *cat = arche_token_category(e->as.token.kind, node->kind);
			if (!cat)
				continue;
			int uline = e->as.token.line - g_core_lines;
			if (uline <= 0)
				continue; /* core region */
			printf("%u %u %d %d %s\n", e->as.token.offset, e->as.token.length, uline, e->as.token.column, cat);
		}
	}
}

static void emit_tokens(const Analysis *a) {
	if (a->cst_root)
		walk_tokens(a->cst_root);
}

/* ---- one-shot dump ---- */

static int run_dump(const char *path) {
	char *user = path ? read_file(path) : read_stream(stdin);
	if (!user) {
		fprintf(stderr, "arche-analyzer: could not read %s\n", path ? path : "<stdin>");
		return 1;
	}
	Analysis a = analyze(user, path);
	if (!a.cst_root) {
		fprintf(stderr, "arche-analyzer: parse produced no CST\n");
		analysis_free(&a);
		return 1;
	}
	emit_hints(&a);
	emit_diags(&a);
	analysis_free(&a);
	return 0;
}

/* ---- persistent server: warm per-document analysis over a line protocol ----
 *
 * Requests on stdin (one per line; the editor's LSP server is the only client):
 *   UPDATE <bytelen> <path>\n<bytelen bytes>   (re)analyze a document, keep it warm
 *   HINTS <path>\n                              emit SYN lines for it, then a blank line
 *   CLOSE <path>\n                              drop the document
 * Responses: HINTS ends with a blank line; UPDATE/CLOSE reply "OK". Core + modules
 * stay parsed across requests, so each edit costs only the user buffer.
 */

typedef struct {
	char *path;
	Analysis a;
} Doc;

typedef struct {
	Doc *items;
	int count, cap;
} Docs;

static Doc *docs_find(Docs *d, const char *path) {
	for (int i = 0; i < d->count; i++)
		if (strcmp(d->items[i].path, path) == 0)
			return &d->items[i];
	return NULL;
}

static Doc *docs_get_or_add(Docs *d, const char *path) {
	Doc *existing = docs_find(d, path);
	if (existing)
		return existing;
	if (d->count >= d->cap) {
		d->cap = d->cap ? d->cap * 2 : 8;
		d->items = realloc(d->items, (size_t)d->cap * sizeof(Doc));
	}
	Doc *doc = &d->items[d->count++];
	doc->path = strdup(path);
	doc->a = (Analysis){NULL, NULL, NULL, {NULL, 0, 0}};
	return doc;
}

static void docs_remove(Docs *d, const char *path) {
	for (int i = 0; i < d->count; i++)
		if (strcmp(d->items[i].path, path) == 0) {
			analysis_free(&d->items[i].a);
			free(d->items[i].path);
			d->items[i] = d->items[--d->count];
			return;
		}
}

static void docs_free(Docs *d) {
	for (int i = 0; i < d->count; i++) {
		analysis_free(&d->items[i].a);
		free(d->items[i].path);
	}
	free(d->items);
}

/* Read one '\n'-terminated line from f (newline stripped). NULL on EOF. */
static char *read_line(FILE *f) {
	size_t cap = 128, len = 0;
	char *buf = malloc(cap);
	int c;
	while ((c = fgetc(f)) != EOF && c != '\n') {
		if (len + 1 >= cap) {
			cap *= 2;
			buf = realloc(buf, cap);
		}
		buf[len++] = (char)c;
	}
	if (c == EOF && len == 0) {
		free(buf);
		return NULL;
	}
	buf[len] = '\0';
	return buf;
}

static int run_serve(void) {
	Docs docs = {NULL, 0, 0};
	char *line;
	while ((line = read_line(stdin)) != NULL) {
		if (strncmp(line, "UPDATE ", 7) == 0) {
			/* UPDATE <bytelen> <path> */
			char *p = line + 7;
			long n = strtol(p, &p, 10);
			while (*p == ' ')
				p++;
			const char *path = p;
			char *src = malloc((size_t)(n > 0 ? n : 0) + 1);
			size_t got = n > 0 ? fread(src, 1, (size_t)n, stdin) : 0;
			src[got] = '\0';
			Doc *doc = docs_get_or_add(&docs, path);
			analysis_free(&doc->a);
			doc->a = analyze(src, doc->path); /* analyze takes ownership of src */
			printf("OK\n\n");                 /* blank line terminates every response */
			fflush(stdout);
		} else if (strncmp(line, "HINTS ", 6) == 0) {
			Doc *doc = docs_find(&docs, line + 6);
			if (doc)
				emit_hints(&doc->a);
			printf("\n"); /* blank line terminates the response */
			fflush(stdout);
		} else if (strncmp(line, "TOKENS ", 7) == 0) {
			Doc *doc = docs_find(&docs, line + 7);
			if (doc)
				emit_tokens(&doc->a);
			printf("\n");
			fflush(stdout);
		} else if (strncmp(line, "DIAG ", 5) == 0) {
			Doc *doc = docs_find(&docs, line + 5);
			if (doc)
				emit_diags(&doc->a);
			printf("\n");
			fflush(stdout);
		} else if (strncmp(line, "CLOSE ", 6) == 0) {
			docs_remove(&docs, line + 6);
			printf("OK\n\n");
			fflush(stdout);
		}
		free(line);
	}
	docs_free(&docs);
	return 0;
}

int main(int argc, char *argv[]) {
	if (argc >= 2 && strcmp(argv[1], "--dump") == 0)
		return run_dump(argc >= 3 ? argv[2] : NULL);
	if (argc >= 2 && strcmp(argv[1], "--serve") == 0)
		return run_serve();
	fprintf(stderr, "usage: %s (--dump [file] | --serve)\n", argv[0]);
	return 2;
}
