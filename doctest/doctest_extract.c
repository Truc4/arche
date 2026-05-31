#include "doctest_extract.h"
#include "../cst/cst_view.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* C99 doesn't expose strdup; declare it explicitly (as sem_model.c / sem_hints.c do). */
char *strdup(const char *s);

#define MAX_DOC_LINES 512

/* Trim leading/trailing ASCII spaces and tabs; returns the trimmed [out_ptr,out_len). */
static void trim(const char *p, size_t len, const char **out_ptr, size_t *out_len) {
	size_t a = 0, b = len;
	while (a < b && (p[a] == ' ' || p[a] == '\t'))
		a++;
	while (b > a && (p[b - 1] == ' ' || p[b - 1] == '\t' || p[b - 1] == '\r'))
		b--;
	*out_ptr = p + a;
	*out_len = b - a;
}

static int slice_eq(const char *p, size_t len, const char *s) {
	size_t n = strlen(s);
	return len == n && memcmp(p, s, n) == 0;
}

/* A fence opener is a line trimming to ```arche optionally followed by a
 * comma-separated flag list (```arche,no_run,should_panic). Returns 1 and sets
 * *flags if it is an arche fence; 0 otherwise. A closer trims to ```. */
static int parse_fence_open(const char *p, size_t len, int *flags) {
	const char *t;
	size_t tl;
	trim(p, len, &t, &tl);
	if (tl < 3 || t[0] != '`' || t[1] != '`' || t[2] != '`')
		return 0;

	/* Info string after the backticks, trimmed. */
	const char *info;
	size_t il;
	trim(t + 3, tl - 3, &info, &il);

	/* First comma-separated token must be exactly "arche". */
	size_t a = 0;
	while (a < il && info[a] != ',')
		a++;
	const char *ft;
	size_t ftl;
	trim(info, a, &ft, &ftl);
	if (!slice_eq(ft, ftl, "arche"))
		return 0;

	int f = 0;
	size_t i = a;
	while (i < il) {
		if (info[i] == ',')
			i++;
		size_t s = i;
		while (i < il && info[i] != ',')
			i++;
		const char *tok;
		size_t tkl;
		trim(info + s, i - s, &tok, &tkl);
		if (slice_eq(tok, tkl, "no_run"))
			f |= DOCTEST_NO_RUN;
		else if (slice_eq(tok, tkl, "compile_fail"))
			f |= DOCTEST_COMPILE_FAIL;
		else if (slice_eq(tok, tkl, "should_panic"))
			f |= DOCTEST_SHOULD_PANIC;
		else if (slice_eq(tok, tkl, "ignore"))
			f |= DOCTEST_IGNORE;
		/* unknown flags are ignored, like rustdoc */
	}
	*flags = f;
	return 1;
}
static int is_fence_close(const char *p, size_t len) {
	const char *t;
	size_t tl;
	trim(p, len, &t, &tl);
	return slice_eq(t, tl, "```");
}

static void examples_push(DoctestExamples *ex, char *code, const char *name, int src_line, int has_main, int flags) {
	DoctestExample *grown = realloc(ex->items, (size_t)(ex->count + 1) * sizeof(DoctestExample));
	if (!grown) {
		free(code);
		return;
	}
	ex->items = grown;
	ex->items[ex->count].code = code;
	ex->items[ex->count].decl_name = name ? strdup(name) : strdup("item");
	ex->items[ex->count].src_line = src_line;
	ex->items[ex->count].has_main = has_main;
	ex->items[ex->count].flags = flags;
	ex->count++;
}

/* Does the example body declare `main`? Lexical substring check at a line start is enough — the
 * runner only needs to know whether to wrap in a main. Unified grammar: `main :: proc(`. */
static int code_has_main(const char *code) {
	const char *p = code;
	while (*p) {
		const char *q = p;
		while (*q == ' ' || *q == '\t')
			q++;
		if (strncmp(q, "main :: proc", 12) == 0 || strncmp(q, "main::proc", 10) == 0)
			return 1;
		/* advance to next line */
		while (*p && *p != '\n')
			p++;
		if (*p == '\n')
			p++;
	}
	return 0;
}

/* Scan a sequence of doc-text lines for ```arche fences, pushing each fenced
 * block as an example. Lines are borrowed slices (NOT NUL-terminated); linenos[i]
 * is the 1-based source line of lines[i], used to report the exact fence line. */
static void scan_fences(const CvText *lines, const int *linenos, int nlines, const char *name, DoctestExamples *out) {
	int i = 0;
	while (i < nlines) {
		int flags = 0;
		if (!parse_fence_open(lines[i].ptr, lines[i].len, &flags)) {
			i++;
			continue;
		}
		int fence_line = linenos[i];
		/* Found an opener; gather body until a closer. */
		int j = i + 1;
		size_t total = 0;
		int closed = 0;
		for (; j < nlines; j++) {
			if (is_fence_close(lines[j].ptr, lines[j].len)) {
				closed = 1;
				break;
			}
			total += lines[j].len + 1; /* + newline */
		}
		if (!closed) {
			/* Unterminated fence: skip the rest of this doc block. */
			return;
		}
		/* Assemble the body into an owned, NUL-terminated buffer. */
		char *code = malloc(total + 1);
		if (code) {
			size_t pos = 0;
			for (int k = i + 1; k < j; k++) {
				memcpy(code + pos, lines[k].ptr, lines[k].len);
				pos += lines[k].len;
				code[pos++] = '\n';
			}
			code[pos] = '\0';
			examples_push(out, code, name, fence_line, code_has_main(code), flags);
		}
		i = j + 1; /* continue past the closer — multiple fences per doc allowed */
	}
}

static void decl_name_of(CstView decl, char *buf, size_t bufsz) {
	CstView n = cv_child(decl, SN_FUNC_DEF_NAME);
	if (!cv_present(n))
		n = cv_child(decl, SN_TYPE_DEF_NAME);
	CvText t = {NULL, 0};
	if (cv_present(n))
		t = cv_text(n);
	if (!t.ptr)
		t = cv_token(decl, TOK_IDENT);
	size_t k = 0;
	if (t.ptr) {
		k = t.len < bufsz - 1 ? t.len : bufsz - 1;
		memcpy(buf, t.ptr, k);
	}
	buf[k] = '\0';
	if (k == 0)
		snprintf(buf, bufsz, "item");
}

DoctestExamples doctest_extract(const SyntaxNode *root, const char *src) {
	DoctestExamples ex = {NULL, 0};
	if (!root)
		return ex;
	CstView rv = cv_root(root, src);

	int nn = cv_node_count(rv);
	for (int i = 0; i < nn; i++) {
		CstView decl = cv_node_at(rv, i);
		SyntaxNodeKind k = cv_kind(decl);
		if (k < SN_WORLD_DECL || k > SN_USE_DECL)
			continue;

		CvText lines[MAX_DOC_LINES];
		int linenos[MAX_DOC_LINES];
		int n = cv_decl_doc_lines(rv, decl, lines, linenos, MAX_DOC_LINES);
		if (n <= 0)
			continue;

		char name[256];
		decl_name_of(decl, name, sizeof(name));
		scan_fences(lines, linenos, n, name, &ex);
	}
	return ex;
}

void doctest_examples_free(DoctestExamples *ex) {
	if (!ex)
		return;
	for (int i = 0; i < ex->count; i++) {
		free(ex->items[i].code);
		free(ex->items[i].decl_name);
	}
	free(ex->items);
	ex->items = NULL;
	ex->count = 0;
}
