/* POSIX APIs (fork, setpgid, kill, nanosleep, waitpid) under -std=c99. */
#define _POSIX_C_SOURCE 200809L

#include "doctest_run.h"
#include "../compile/compile.h"
#include "../parser/parser.h"
#include "doctest_extract.h"
#include <dirent.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* An example that hangs would hang `arche test` forever; cap each run. */
#define DOCTEST_TIMEOUT_SECS 10

/* run_executable result sentinels (>= 0 is the process exit code). */
#define DT_CRASHED -1
#define DT_TIMEOUT -2

static char *read_file(const char *path) {
	FILE *f = fopen(path, "r");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *buf = malloc((size_t)size + 1);
	if (!buf) {
		fclose(f);
		return NULL;
	}
	size_t n = fread(buf, 1, (size_t)size, f);
	fclose(f);
	buf[n] = '\0';
	return buf;
}

/* Module name = the input's basename without its .arche extension. */
static void module_name_of(const char *path, char *buf, size_t bufsz) {
	const char *base = strrchr(path, '/');
	base = base ? base + 1 : path;
	size_t n = 0;
	for (; base[n] && base[n] != '.' && n < bufsz - 1; n++)
		buf[n] = base[n];
	buf[n] = '\0';
	if (n == 0)
		snprintf(buf, bufsz, "doctest_mod");
}

/* Directory part of `path` (without trailing slash), or "." if none. */
static void dir_of(const char *path, char *buf, size_t bufsz) {
	const char *slash = strrchr(path, '/');
	if (!slash) {
		snprintf(buf, bufsz, ".");
		return;
	}
	size_t n = (size_t)(slash - path);
	if (n >= bufsz)
		n = bufsz - 1;
	memcpy(buf, path, n);
	buf[n] = '\0';
}

static int dt_has_suffix(const char *name, const char *suf) {
	size_t L = strlen(name), S = strlen(suf);
	return L >= S && strcmp(name + L - S, suf) == 0;
}

/* Append `s` to a growable buffer. */
static void dt_str_append(char **buf, size_t *len, size_t *cap, const char *s) {
	size_t sl = strlen(s);
	if (*len + sl + 1 > *cap) {
		*cap = (*len + sl + 1) * 2;
		*buf = realloc(*buf, *cap);
	}
	memcpy(*buf + *len, s, sl);
	*len += sl;
	(*buf)[*len] = '\0';
}

/* If `dir` is a DEVICE folder (contains a `*.ds.arche` datasheet), return the concatenation of ALL
 * its `.arche` files — datasheets first, then impl — so a doctest in the device compiles as a
 * generated DRIVER of it: the datasheet's types + storage requirements (which become real allocations
 * in this single combined unit, not module-inlined) + the impl behavior are all in scope. The doctest
 * can then drive the device's shapes with no manual pool sizing. Returns an owned string, or NULL if
 * `dir` is not a device (caller falls back to the single-file source). */
static char *read_device_context(const char *dir) {
	DIR *d = opendir(dir);
	if (!d)
		return NULL;
	int is_device = 0;
	struct dirent *ent;
	while ((ent = readdir(d)) != NULL)
		if (dt_has_suffix(ent->d_name, ".ds.arche")) {
			is_device = 1;
			break;
		}
	if (!is_device) {
		closedir(d);
		return NULL;
	}
	char *buf = NULL;
	size_t len = 0, cap = 0;
	/* Phase 0: datasheets (types + requirements first); phase 1: impl `.arche` files. */
	for (int phase = 0; phase < 2; phase++) {
		rewinddir(d);
		while ((ent = readdir(d)) != NULL) {
			int ds = dt_has_suffix(ent->d_name, ".ds.arche");
			if (!dt_has_suffix(ent->d_name, ".arche") || (phase == 0) != ds)
				continue;
			char fp[1300];
			snprintf(fp, sizeof(fp), "%s/%s", dir, ent->d_name);
			char *fc = read_file(fp);
			if (fc) {
				dt_str_append(&buf, &len, &cap, fc);
				dt_str_append(&buf, &len, &cap, "\n");
				free(fc);
			}
		}
	}
	closedir(d);
	return buf;
}

/* Does region text [start, end) contain `fmt` as a standalone identifier token? */
static int dt_region_has_fmt(const char *start, const char *end) {
	for (const char *p = start; p + 3 <= end; p++) {
		if (strncmp(p, "fmt", 3) != 0)
			continue;
		char before = (p == start) ? ' ' : p[-1];
		char after = p[3];
		int bw = (before >= 'a' && before <= 'z') || (before >= 'A' && before <= 'Z') ||
		         (before >= '0' && before <= '9') || before == '_';
		int aw = (after >= 'a' && after <= 'z') || (after >= 'A' && after <= 'Z') || (after >= '0' && after <= '9') ||
		         after == '_';
		if (!bw && !aw)
			return 1;
	}
	return 0;
}

/* Locate the file's single top-level `#import` region. On success sets *insert_at to the byte
 * offset where ` fmt` should be spliced to add fmt to the existing region (just inside a `{ … }`
 * block, or at end-of-line for a bare banner) and *already to whether fmt is already listed.
 * Returns 1 if an `#import` region was found, 0 otherwise. A line whose first non-blank chars are
 * `//` is skipped, so a `#import` inside a comment is not matched. */
static int dt_find_import(const char *src, size_t *insert_at, int *already) {
	const char *p = src;
	while (*p) {
		const char *q = p;
		while (*q == ' ' || *q == '\t')
			q++;
		if (strncmp(q, "#import", 7) == 0) {
			const char *eol = strchr(q, '\n');
			if (!eol)
				eol = q + strlen(q);
			const char *lbrace = NULL;
			for (const char *r = q + 7; r < eol; r++)
				if (*r == '{') {
					lbrace = r;
					break;
				}
			if (lbrace) {
				const char *rbrace = strchr(lbrace, '}');
				const char *rend = rbrace ? rbrace : eol;
				*already = dt_region_has_fmt(lbrace + 1, rend);
				*insert_at = (size_t)(lbrace + 1 - src);
			} else {
				*already = dt_region_has_fmt(q + 7, eol);
				*insert_at = (size_t)(eol - src);
			}
			return 1;
		}
		const char *nl = strchr(p, '\n');
		if (!nl)
			break;
		p = nl + 1;
	}
	return 0;
}

/* Build the synthesized program for one example. In-file model: the example runs with the
 * documented file's FULL context — every decl in the file, private/file-local included — not
 * just its public API (a doctest is a unit test, which shouldn't be limited to exports). So we
 * prepend the file's entire source, then the example (wrapped in a `proc main` if it declares
 * none). Returns an owned, NUL-terminated string.
 *
 * `core` is special — compile_source already prepends core.arche to every unit, so a doctest in
 * core/core.arche must NOT re-include it (that would redeclare the whole prelude); its decls are
 * in scope implicitly, so `file_prefix` is empty there.
 *
 * `fmt` (the assertion library) is always made available to examples — `fmt.assert` is the doctest
 * assertion idiom, and a module shouldn't have to depend on fmt just to be doctestable. A file
 * carries at most one `#import` region (E0121), so fmt is MERGED into the file's existing `#import`
 * when it has one (or skipped if already listed); only a file with no imports gets a fresh
 * `#import { fmt }` prepended. */
static char *synthesize(const char *file_source, int is_core, const DoctestExample *ex) {
	if (is_core) {
		size_t need = strlen("\nentry :: system eff {\n}\n#run entry\n") + strlen(ex->code) + 8;
		char *out = malloc(need);
		if (!out)
			return NULL;
		if (ex->has_main)
			snprintf(out, need, "\n%s", ex->code);
		else
			/* Loose statements get their own entry: a `system` scheduled by `#run` (the only entry point —
			 * `main` is no longer special). */
			snprintf(out, need, "\nentry :: system eff {\n%s}\n#run entry\n", ex->code);
		return out;
	}

	/* Splice fmt into the file's own `#import` region if present; else prepend a fresh one. */
	char *prefix = NULL;
	const char *fmt_import = "";
	size_t insert_at = 0;
	int already = 0;
	if (dt_find_import(file_source, &insert_at, &already)) {
		size_t slen = strlen(file_source);
		const char *add = already ? "" : " fmt";
		prefix = malloc(slen + strlen(add) + 1);
		if (!prefix)
			return NULL;
		memcpy(prefix, file_source, insert_at);
		memcpy(prefix + insert_at, add, strlen(add));
		memcpy(prefix + insert_at + strlen(add), file_source + insert_at, slen - insert_at + 1);
	} else {
		fmt_import = "#import { fmt }\n";
	}
	const char *body = prefix ? prefix : file_source;

	size_t need =
	    strlen(fmt_import) + strlen(body) + strlen("\nentry :: system eff {\n}\n#run entry\n") + strlen(ex->code) + 8;
	char *out = malloc(need);
	if (!out) {
		free(prefix);
		return NULL;
	}
	if (ex->has_main)
		snprintf(out, need, "%s%s\n%s", fmt_import, body, ex->code);
	else
		snprintf(out, need, "%s%s\nentry :: system eff {\n%s}\n#run entry\n", fmt_import, body, ex->code);
	free(prefix);
	return out;
}

/* ---- markdown shared-context model ----
 *
 * A `.md` page's ```arche blocks are NOT independent programs. Following the model used by
 * Scala mdoc / Python doctest.testfile / Julia Documenter (and matching arche's order-free
 * top-level hoisting), the whole page shares one declaration scope: every block's top-level
 * DECLARATIONS (`name :: <form>` and `#module`/`#file`/`#foreign` regions) and IMPORTS are
 * pooled, while each block's executable STATEMENTS run in their own `main` with that pooled
 * scope visible. So a type declared in one block is usable by a later block with no repeated
 * setup — which is what makes a reference doc testable without boilerplate.
 *
 * Classification is line-based: at brace-depth 0, a line is a DECL if it is an `#…` region or
 * contains the `::` definition operator (arche uses `::` nowhere else); an `#import` line feeds
 * the merged import set instead; anything else is a STATEMENT. Lines inside an open `{ … }`
 * inherit the classification of the construct that opened them. Limitation (documented): single
 * `:`/`:=` global bindings and pool decls are treated as per-block statements, not shared; a name
 * defined in two blocks collides (E0031), as it would in one file. */

typedef struct {
	char *buf;
	size_t len, cap;
} Str;

static void str_add(Str *s, const char *p, size_t n) {
	if (s->len + n + 1 > s->cap) {
		size_t nc = s->cap ? s->cap * 2 : 256;
		while (nc < s->len + n + 1)
			nc *= 2;
		char *g = realloc(s->buf, nc);
		if (!g)
			return;
		s->buf = g;
		s->cap = nc;
	}
	memcpy(s->buf + s->len, p, n);
	s->len += n;
	s->buf[s->len] = '\0';
}
static void str_addz(Str *s, const char *z) {
	str_add(s, z, strlen(z));
}

/* A small deduplicated set of imported module names. */
typedef struct {
	char **names;
	int count, cap;
} NameSet;

static void nameset_add(NameSet *ns, const char *p, size_t n) {
	if (n == 0)
		return;
	for (int i = 0; i < ns->count; i++)
		if (strlen(ns->names[i]) == n && memcmp(ns->names[i], p, n) == 0)
			return;
	if (ns->count == ns->cap) {
		ns->cap = ns->cap ? ns->cap * 2 : 8;
		ns->names = realloc(ns->names, (size_t)ns->cap * sizeof(char *));
	}
	char *z = malloc(n + 1);
	memcpy(z, p, n);
	z[n] = '\0';
	ns->names[ns->count++] = z;
}
static void nameset_free(NameSet *ns) {
	for (int i = 0; i < ns->count; i++)
		free(ns->names[i]);
	free(ns->names);
}

static int is_ident_ch(char c) {
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

/* Collect identifier tokens from an `#import` region line into `imports`, skipping the
 * `#import` keyword and the braces. */
static void collect_imports(const char *line, size_t len, NameSet *imports) {
	size_t i = 0;
	while (i < len) {
		if (line[i] == '{' || line[i] == '}') {
			i++;
			continue;
		}
		if (is_ident_ch(line[i])) {
			size_t s = i;
			while (i < len && is_ident_ch(line[i]))
				i++;
			size_t n = i - s;
			if (!(n == 7 && memcmp(line + s, "#import", 7) == 0) && !(n == 6 && memcmp(line + s, "import", 6) == 0))
				nameset_add(imports, line + s, n);
		} else {
			i++;
		}
	}
}

/* Trim leading/trailing spaces and tabs from [p,len), returning the inner slice. */
static void md_trim(const char *p, size_t len, const char **out, size_t *out_len) {
	size_t a = 0, b = len;
	while (a < b && (p[a] == ' ' || p[a] == '\t' || p[a] == '\n' || p[a] == '\r'))
		a++;
	while (b > a && (p[b - 1] == ' ' || p[b - 1] == '\t' || p[b - 1] == '\n' || p[b - 1] == '\r'))
		b--;
	*out = p + a;
	*out_len = b - a;
}

/* Does [p,len) contain the `::` definition operator outside a `//` comment? */
static int has_colcol(const char *p, size_t len) {
	for (size_t i = 0; i + 1 < len; i++) {
		if (p[i] == '/' && p[i + 1] == '/')
			return 0; /* rest of line is a comment */
		if (p[i] == ':' && p[i + 1] == ':')
			return 1;
	}
	return 0;
}

static int brace_delta(const char *p, size_t len) {
	int d = 0;
	for (size_t i = 0; i < len; i++) {
		if (p[i] == '{')
			d++;
		else if (p[i] == '}')
			d--;
	}
	return d;
}

/* Classify one block's lines: append top-level declaration lines to `decls`, this block's
 * executable statements to `stmts`, and pool any imported module names into `imports`. */
static void classify_block(const char *code, Str *decls, Str *stmts, NameSet *imports) {
	const char *p = code;
	int depth = 0;
	enum { M_DECL, M_STMT, M_IMPORT } mode = M_STMT;
	while (*p) {
		const char *nl = strchr(p, '\n');
		size_t len = nl ? (size_t)(nl - p) : strlen(p);
		if (depth == 0) {
			const char *t;
			size_t tl;
			md_trim(p, len, &t, &tl);
			if (tl >= 7 && memcmp(t, "#import", 7) == 0)
				mode = M_IMPORT;
			else if ((tl >= 1 && t[0] == '#') || has_colcol(t, tl) || (tl >= 1 && t[0] == '['))
				mode = M_DECL; /* `#…` region, `name :: …` definition, or `[N]Foo` pool storage decl */
			else
				mode = M_STMT;
		}
		if (mode == M_IMPORT) {
			collect_imports(p, len, imports);
		} else if (mode == M_DECL) {
			str_add(decls, p, len);
			str_addz(decls, "\n");
		} else {
			str_add(stmts, p, len);
			str_addz(stmts, "\n");
		}
		depth += brace_delta(p, len);
		if (depth < 0)
			depth = 0;
		if (!nl)
			break;
		p = nl + 1;
	}
}

/* Capture a child/compiler fd into `buf` from a temp file, trimming a trailing
 * newline. Rewinds and reads up to cap-1 bytes. */
static void slurp_fd_file(int fd, char *buf, size_t cap) {
	if (!buf || cap == 0)
		return;
	buf[0] = '\0';
	lseek(fd, 0, SEEK_SET);
	ssize_t n = read(fd, buf, cap - 1);
	if (n < 0)
		n = 0;
	while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
		n--;
	buf[n] = '\0';
}

/* Run a freshly built executable with a wall-clock timeout, capturing its
 * stdout+stderr into `out` (shown only if the example fails). Returns its exit
 * code (>= 0), DT_CRASHED on signal, or DT_TIMEOUT (process group SIGKILLed). */
static int run_executable(const char *exe, char *out, size_t outcap) {
	if (out && outcap)
		out[0] = '\0';
	char tmpl[] = "/tmp/arche_dtout_XXXXXX";
	int fd = mkstemp(tmpl);
	if (fd < 0)
		return DT_CRASHED;
	unlink(tmpl); /* anonymous: vanishes when the fd closes */

	fflush(stdout);
	pid_t pid = fork();
	if (pid < 0) {
		close(fd);
		return DT_CRASHED;
	}
	if (pid == 0) {
		setpgid(0, 0); /* own process group, so a timeout can kill any children it spawns */
		dup2(fd, 1);   /* capture stdout */
		dup2(fd, 2);   /* and stderr */
		execl(exe, exe, (char *)NULL);
		_exit(127); /* exec failed */
	}
	setpgid(pid, pid); /* also set from the parent to close the fork/exec race */

	int waited_ms = 0, status = 0, code;
	for (;;) {
		pid_t r = waitpid(pid, &status, WNOHANG);
		if (r == pid) {
			code = WIFEXITED(status) ? WEXITSTATUS(status) : DT_CRASHED;
			break;
		}
		if (r < 0) {
			code = DT_CRASHED;
			break;
		}
		if (waited_ms >= DOCTEST_TIMEOUT_SECS * 1000) {
			kill(-pid, SIGKILL); /* kill the whole process group */
			waitpid(pid, &status, 0);
			code = DT_TIMEOUT;
			break;
		}
		struct timespec ts = {0, 10 * 1000 * 1000}; /* 10ms */
		nanosleep(&ts, NULL);
		waited_ms += 10;
	}
	slurp_fd_file(fd, out, outcap);
	close(fd);
	return code;
}

/* compile_source writes its diagnostics straight to stderr; redirect fd 2 to a
 * temp file across the call so the runner can show those diagnostics only when
 * an example fails unexpectedly (and stay silent otherwise). */
static int compile_capturing(const char *synth, const char *synth_path, const char *exe, char *err, size_t errcap) {
	if (err && errcap)
		err[0] = '\0';
	char tmpl[] = "/tmp/arche_dterr_XXXXXX";
	int fd = mkstemp(tmpl);
	if (fd < 0) {
		CompileOpts o = {0};
		o.quiet = 1;
		return compile_source(synth, synth_path, exe, &o);
	}
	unlink(tmpl);

	fflush(stderr);
	int saved = dup(2);
	dup2(fd, 2);
	CompileOpts opts = {0};
	opts.quiet = 1;
	int rc = compile_source(synth, synth_path, exe, &opts);
	fflush(stderr);
	dup2(saved, 2);
	close(saved);

	slurp_fd_file(fd, err, errcap);
	close(fd);
	return rc;
}

/* Print captured output indented under a failing example, go-test style. */
static void print_indented(const char *text) {
	if (!text || !text[0])
		return;
	const char *p = text;
	while (*p) {
		const char *nl = strchr(p, '\n');
		int len = nl ? (int)(nl - p) : (int)strlen(p);
		printf("        %.*s\n", len, p);
		if (!nl)
			break;
		p = nl + 1;
	}
}

/* Accumulated tallies across one or many files. */
typedef struct {
	int files; /* files that had at least one example */
	int passed;
	int failed;
	int ignored;
} DtTally;

/* Run the doctests in one file, go-test style: silent per-example on success,
 * a single `ok`/`FAIL`/`?` status line per file, and the failing examples'
 * output shown only on failure. `verbose` adds a per-example PASS line.
 * `quiet_empty` suppresses the `?` line (recursive runs). Tallies fold into *t.
 * Returns non-zero if any example in this file failed. */
/* A markdown file? `.md` doctests are STANDALONE (no documenting file, no in-file
 * model) — extracted by line scan, not via the syntax tree. */
static int has_md_ext(const char *name) {
	size_t n = strlen(name);
	return n > 3 && strcmp(name + n - 3, ".md") == 0;
}

/* Does pooled-decl text declare a top-level `main` proc? (Then the section is a complete program
 * with its own entry point — we run it as-is instead of synthesizing a `main` wrapper.) */
static int decls_have_main(const char *s) {
	const char *p = s;
	int depth = 0;
	while (*p) {
		const char *nl = strchr(p, '\n');
		size_t len = nl ? (size_t)(nl - p) : strlen(p);
		if (depth == 0) {
			const char *t;
			size_t tl;
			md_trim(p, len, &t, &tl);
			if ((tl >= 12 && memcmp(t, "main :: proc", 12) == 0) || (tl >= 10 && memcmp(t, "main::proc", 10) == 0))
				return 1;
			/* `#run <Schedule>` is its OWN entry point (no main) — run the section as-is, never wrap it in
			 * a synthesized main (top-level system/map/each decls can't live inside a proc body). */
			if (tl >= 4 && memcmp(t, "#run", 4) == 0)
				return 1;
		}
		depth += brace_delta(p, len);
		if (depth < 0)
			depth = 0;
		if (!nl)
			break;
		p = nl + 1;
	}
	return 0;
}

/* A markdown ATX heading: 1-6 leading `#` then a space or end-of-line. (Region directives like
 * `#import` have no space after the hashes, so they never match.) Checked only outside code fences. */
static int is_md_heading(const char *t, size_t tl) {
	size_t h = 0;
	while (h < tl && t[h] == '#')
		h++;
	if (h == 0 || h > 6)
		return 0;
	return h == tl || t[h] == ' ' || t[h] == '\t';
}

/* Assign each extracted block to a SECTION group: blocks under the same markdown heading share a
 * group (a new heading starts a fresh one). Walks the source tracking fence state so that `#` lines
 * inside ```arche blocks are never mistaken for headings. group[i] is filled for every example. */
static void assign_groups(const char *source, const DoctestExamples *ex, int *group) {
	int gi = 0, in_fence = 0, lineno = 1, next = 0;
	const char *p = source;
	while (*p && next < ex->count) {
		const char *nl = strchr(p, '\n');
		size_t len = nl ? (size_t)(nl - p) : strlen(p);
		const char *t;
		size_t tl;
		md_trim(p, len, &t, &tl);
		int is_fence = (tl >= 3 && t[0] == '`' && t[1] == '`' && t[2] == '`');
		if (!in_fence && is_md_heading(t, tl))
			gi++;
		while (next < ex->count && ex->items[next].src_line == lineno)
			group[next++] = gi;
		if (is_fence)
			in_fence = !in_fence;
		if (!nl)
			break;
		p = nl + 1;
		lineno++;
	}
	while (next < ex->count) /* defensive: any unmatched trailing blocks */
		group[next++] = gi;
}

/* Run one SECTION's blocks as a shared-context group: pool that section's declarations + imports
 * into one scope, run each of its statement blocks in its own `main` with that scope visible, and —
 * if the section has no executable statements — compile-check its declarations once. */
static void run_group(const char *synth_path, DoctestExamples *ex, const int *group, int g, int verbose, int *passed,
                      int *failed) {
	Str decls = {NULL, 0, 0};
	NameSet imports = {NULL, 0, 0};
	nameset_add(&imports, "fmt", 3);
	Str *stmts = calloc((size_t)ex->count, sizeof(Str));
	for (int i = 0; i < ex->count; i++)
		if (group[i] == g && !(ex->items[i].flags & DOCTEST_IGNORE))
			classify_block(ex->items[i].code, &decls, &stmts[i], &imports);

	Str shared = {NULL, 0, 0};
	str_addz(&shared, "#import {");
	for (int i = 0; i < imports.count; i++) {
		str_addz(&shared, " ");
		str_addz(&shared, imports.names[i]);
	}
	str_addz(&shared, " }\n");
	if (decls.buf)
		str_add(&shared, decls.buf, decls.len);

	int sect_line = -1;
	for (int i = 0; i < ex->count; i++)
		if (group[i] == g) {
			sect_line = ex->items[i].src_line;
			break;
		}

	/* Section declares its own `main` — it is a complete program. Compile and run it as-is; do not
	 * synthesize a wrapper main (that would be a duplicate). Any loose statements in the section are
	 * expected to live inside that main already. */
	if (decls_have_main(shared.buf)) {
		char exe[256], captured[4096] = "";
		snprintf(exe, sizeof(exe), "/tmp/arche_dt_%d_m%d", (int)getpid(), g);
		int rc = compile_capturing(shared.buf, synth_path, exe, captured, sizeof(captured));
		int ok = 1;
		const char *why = "";
		if (rc != 0) {
			ok = 0;
			why = "compile error";
		} else {
			int code = run_executable(exe, captured, sizeof(captured));
			unlink(exe);
			ok = (code == 0);
			if (code == DT_TIMEOUT)
				why = "timed out";
			else if (code == DT_CRASHED)
				why = "crashed";
			else if (code != 0)
				why = "exited non-zero";
		}
		if (ok) {
			(*passed)++;
			if (verbose)
				printf("    --- PASS: doc (line %d)\n", sect_line);
		} else {
			(*failed)++;
			printf("    --- FAIL: doc (line %d): %s\n", sect_line, why);
			print_indented(captured);
		}
		free(decls.buf);
		free(shared.buf);
		for (int i = 0; i < ex->count; i++)
			free(stmts[i].buf);
		free(stmts);
		nameset_free(&imports);
		return;
	}

	int ran_any = 0, first_line = -1;
	for (int i = 0; i < ex->count; i++) {
		if (group[i] != g)
			continue;
		/* `arche,ignore`: shown in docs, never compiled or run (matches run_one for .arche). It
		 * contributed nothing to the pooled decls/imports above, so just skip it here too. */
		if (ex->items[i].flags & DOCTEST_IGNORE)
			continue;
		if (first_line < 0)
			first_line = ex->items[i].src_line;
		const char *body = stmts[i].buf ? stmts[i].buf : "";
		const char *tt;
		size_t tl;
		md_trim(body, strlen(body), &tt, &tl);
		if (tl == 0)
			continue; /* declaration-only block: contributes to scope, runs nothing of its own */
		ran_any = 1;

		Str synth = {NULL, 0, 0};
		str_add(&synth, shared.buf, shared.len);
		str_addz(&synth, "\nentry :: system eff {\n"); /* `#run` is the only entry — `main` isn't special */
		str_addz(&synth, body);
		str_addz(&synth, "}\n#run entry\n");

		char exe[256], detail[80] = "", captured[4096] = "";
		snprintf(exe, sizeof(exe), "/tmp/arche_dt_%d_%d", (int)getpid(), i);
		int rc = compile_capturing(synth.buf, synth_path, exe, captured, sizeof(captured));
		free(synth.buf);

		int ok;
		if (rc != 0) {
			ok = 0;
			snprintf(detail, sizeof(detail), "compile error");
		} else {
			int code = run_executable(exe, captured, sizeof(captured));
			unlink(exe);
			ok = (code == 0);
			if (code == DT_TIMEOUT)
				snprintf(detail, sizeof(detail), "timed out");
			else if (code == DT_CRASHED)
				snprintf(detail, sizeof(detail), "crashed");
			else if (code != 0)
				snprintf(detail, sizeof(detail), "exit %d", code);
		}

		if (ok) {
			(*passed)++;
			if (verbose)
				printf("    --- PASS: %s (line %d)\n", ex->items[i].decl_name, ex->items[i].src_line);
		} else {
			(*failed)++;
			printf("    --- FAIL: %s (line %d)%s%s\n", ex->items[i].decl_name, ex->items[i].src_line,
			       detail[0] ? ": " : "", detail);
			print_indented(captured);
		}
	}

	/* Section had no executable statements: still verify its pooled declarations compile. */
	if (!ran_any && first_line >= 0) {
		Str synth = {NULL, 0, 0};
		str_add(&synth, shared.buf, shared.len);
		str_addz(&synth, "\nentry :: system eff {\n}\n#run entry\n");
		char exe[256], captured[4096] = "";
		snprintf(exe, sizeof(exe), "/tmp/arche_dt_%d_ctx%d", (int)getpid(), g);
		int rc = compile_capturing(synth.buf, synth_path, exe, captured, sizeof(captured));
		free(synth.buf);
		unlink(exe);
		if (rc != 0) {
			(*failed)++;
			printf("    --- FAIL: doc (line %d): declarations do not compile\n", first_line);
			print_indented(captured);
		} else {
			(*passed)++;
			if (verbose)
				printf("    --- PASS: declarations compile (line %d)\n", first_line);
		}
	}

	free(decls.buf);
	free(shared.buf);
	for (int i = 0; i < ex->count; i++)
		free(stmts[i].buf);
	free(stmts);
	nameset_free(&imports);
}

/* Run the doctests in one `.md` file. Blocks are partitioned into SECTION groups by markdown
 * heading; each section is its own shared-context scope (decls/imports pooled within a section,
 * isolated across sections — so independent examples never collide). Returns non-zero on any fail. */
static int run_one_markdown(const char *path, char *source, int quiet_empty, int verbose, DtTally *t) {
	DoctestExamples ex = doctest_extract_markdown(source);
	if (ex.count == 0) {
		if (!quiet_empty)
			printf("?    %s  [no examples]\n", path);
		doctest_examples_free(&ex);
		return 0;
	}
	t->files++;

	char dir[512], synth_path[768];
	dir_of(path, dir, sizeof(dir));
	snprintf(synth_path, sizeof(synth_path), "%s/__arche_doctest_synth__.arche", dir);

	int *group = calloc((size_t)ex.count, sizeof(int));
	assign_groups(source, &ex, group);
	int max_group = 0;
	for (int i = 0; i < ex.count; i++)
		if (group[i] > max_group)
			max_group = group[i];

	int passed = 0, failed = 0;
	for (int g = 0; g <= max_group; g++) {
		int has = 0;
		for (int i = 0; i < ex.count; i++)
			if (group[i] == g) {
				has = 1;
				break;
			}
		if (has)
			run_group(synth_path, &ex, group, g, verbose, &passed, &failed);
	}

	if (failed > 0)
		printf("FAIL %s\n", path);
	else if (verbose)
		printf("ok   %s  (%d passed)\n", path, passed);
	else
		printf("ok   %s\n", path);

	t->passed += passed;
	t->failed += failed;

	free(group);
	doctest_examples_free(&ex);
	return failed > 0 ? 1 : 0;
}

static int run_one(const char *path, int quiet_empty, int verbose, DtTally *t) {
	char *source = read_file(path);
	if (!source) {
		printf("FAIL %s  (cannot read)\n", path);
		return 1;
	}

	if (has_md_ext(path)) {
		int r = run_one_markdown(path, source, quiet_empty, verbose, t);
		free(source);
		return r;
	}

	/* Parse the file and extract doctests from the (error-recovering) syntax tree. A file with no
	 * extractable examples is NOT a doctest target — skip it silently, even with parse errors (e.g.
	 * intentional negative-test fixtures). We only surface failures for files that carry doctests. */
	ParseResult pr = parse_source(source);
	DoctestExamples ex = {NULL, 0};
	if (pr.syntax_root)
		ex = doctest_extract(pr.syntax_root, source);
	parse_result_free(&pr); /* examples own copies of everything they need */

	if (ex.count == 0) {
		if (!quiet_empty)
			printf("?    %s  [no examples]\n", path);
		doctest_examples_free(&ex);
		free(source);
		return 0;
	}
	t->files++;

	/* The synthesized program embeds the file's own source (in-file model), and its `#import`s
	 * resolve relative to the path we hand compile_source — point that at the documented file's
	 * directory so the file's transitive imports find their modules. The synth path itself is
	 * never read from disk; only its directory matters. */
	char module[256], dir[512], synth_path[768];
	module_name_of(path, module, sizeof(module));
	dir_of(path, dir, sizeof(dir));
	snprintf(synth_path, sizeof(synth_path), "%s/__arche_doctest_synth__.arche", dir);
	int is_core = (strcmp(module, "core") == 0);

	/* If this file lives in a DEVICE folder, compile the example as a generated driver over the whole
	 * device (datasheet + all impl files), so it can drive the device's datasheet-declared shapes. */
	char *device_ctx = is_core ? NULL : read_device_context(dir);
	const char *ctx_source = device_ctx ? device_ctx : source;

	int passed = 0, failed = 0, ignored = 0;
	for (int i = 0; i < ex.count; i++) {
		DoctestExample *e = &ex.items[i];
		char detail[80] = "", captured[4096] = "";

		if (e->flags & DOCTEST_IGNORE) {
			ignored++;
			if (verbose)
				printf("    --- ignore: %s (line %d)\n", e->decl_name, e->src_line);
			continue;
		}

		char *synth = synthesize(ctx_source, is_core, e);
		char exe[256];
		snprintf(exe, sizeof(exe), "/tmp/arche_dt_%d_%d", (int)getpid(), i);
		int rc = synth ? compile_capturing(synth, synth_path, exe, captured, sizeof(captured)) : 1;
		free(synth);

		int ok;
		if (e->flags & DOCTEST_COMPILE_FAIL) {
			ok = (rc != 0); /* expected to fail compilation; not run */
			if (!ok)
				snprintf(detail, sizeof(detail), "expected compile error, but it compiled");
			captured[0] = '\0'; /* an expected compile error is not interesting output */
		} else if (rc != 0) {
			ok = 0;
			snprintf(detail, sizeof(detail), "compile error");
		} else if (e->flags & DOCTEST_NO_RUN) {
			ok = 1; /* compiled; not run by request */
			unlink(exe);
		} else {
			int code = run_executable(exe, captured, sizeof(captured));
			unlink(exe);
			if (e->flags & DOCTEST_SHOULD_PANIC) {
				ok = (code > 0); /* clean exit / timeout is not the expected panic */
				if (!ok)
					snprintf(detail, sizeof(detail), code == DT_TIMEOUT ? "timed out" : "expected panic, exited 0");
			} else {
				ok = (code == 0);
				if (code == DT_TIMEOUT)
					snprintf(detail, sizeof(detail), "timed out");
				else if (code == DT_CRASHED)
					snprintf(detail, sizeof(detail), "crashed");
				else if (code != 0)
					snprintf(detail, sizeof(detail), "exit %d", code);
			}
		}

		if (ok) {
			passed++;
			if (verbose)
				printf("    --- PASS: %s (line %d)\n", e->decl_name, e->src_line);
		} else {
			failed++;
			printf("    --- FAIL: %s (line %d)%s%s\n", e->decl_name, e->src_line, detail[0] ? ": " : "", detail);
			print_indented(captured);
		}
	}

	/* One go-test-style status line per file. */
	if (failed > 0)
		printf("FAIL %s\n", path);
	else if (verbose)
		printf("ok   %s  (%d passed%s)\n", path, passed, ignored ? ", some ignored" : "");
	else
		printf("ok   %s\n", path);

	t->passed += passed;
	t->failed += failed;
	t->ignored += ignored;
	doctest_examples_free(&ex);
	free(source); /* kept alive through synthesis (in-file model embeds it) */
	free(device_ctx);
	return failed > 0 ? 1 : 0;
}

int doctest_run_file(const char *path) {
	DtTally t = {0, 0, 0, 0};
	return run_one(path, 0, 0, &t);
}

/* ---- recursive directory walking (Go-style `arche test ./...` / a dir) ---- */

static int has_arche_ext(const char *name) {
	size_t n = strlen(name);
	return n > 6 && strcmp(name + n - 6, ".arche") == 0;
}

/* Directory names we never descend into: build artifacts, VCS, venvs. */
static int skip_dir(const char *name) {
	return name[0] == '.' || strcmp(name, "build") == 0 || strcmp(name, "node_modules") == 0 ||
	       strcmp(name, "site-packages") == 0;
}

typedef struct {
	char **items;
	int count, cap;
} PathList;

static void pathlist_push(PathList *pl, const char *path) {
	if (pl->count >= pl->cap) {
		pl->cap = pl->cap ? pl->cap * 2 : 32;
		pl->items = realloc(pl->items, (size_t)pl->cap * sizeof(char *));
	}
	pl->items[pl->count++] = strdup(path);
}

static void collect_arche(const char *dir, PathList *pl) {
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
			if (!skip_dir(e->d_name))
				collect_arche(full, pl);
		} else if (S_ISREG(sb.st_mode) && (has_arche_ext(e->d_name) || has_md_ext(e->d_name))) {
			pathlist_push(pl, full);
		}
	}
	closedir(d);
}

static int cmp_str(const void *a, const void *b) {
	return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static int run_dir(const char *root, int verbose) {
	PathList pl = {NULL, 0, 0};
	collect_arche(root, &pl);
	qsort(pl.items, (size_t)pl.count, sizeof(char *), cmp_str); /* deterministic order */

	DtTally t = {0, 0, 0, 0};
	int any_failed = 0;
	for (int i = 0; i < pl.count; i++) {
		/* quiet_empty: in a tree sweep, don't print a line for every file that
		 * has no examples — only the ones with doctests get an ok/FAIL line. */
		if (run_one(pl.items[i], 1, verbose, &t))
			any_failed = 1;
		free(pl.items[i]);
	}
	free(pl.items);

	if (t.files == 0)
		printf("?    %s  [no examples in any file]\n", root);
	return any_failed ? 1 : 0;
}

int doctest_run_path(const char *spec, int verbose) {
	/* Go-style `<dir>/...` or bare `...` → recurse from <dir> (or cwd). */
	size_t n = strlen(spec);
	if (n >= 3 && strcmp(spec + n - 3, "...") == 0) {
		char root[1024];
		size_t rl = n - 3;
		/* drop a trailing slash before the `...` (e.g. `./...` → `.`) */
		while (rl > 1 && spec[rl - 1] == '/')
			rl--;
		if (rl == 0) {
			strcpy(root, ".");
		} else {
			if (rl >= sizeof(root))
				rl = sizeof(root) - 1;
			memcpy(root, spec, rl);
			root[rl] = '\0';
		}
		return run_dir(root, verbose);
	}

	struct stat sb;
	if (stat(spec, &sb) == 0 && S_ISDIR(sb.st_mode))
		return run_dir(spec, verbose);

	DtTally t = {0, 0, 0, 0};
	return run_one(spec, 0, verbose, &t);
}
