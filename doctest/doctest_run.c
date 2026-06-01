/* POSIX APIs (fork, setpgid, kill, nanosleep, waitpid) under -std=c99. */
#define _POSIX_C_SOURCE 200809L

#include "doctest_run.h"
#include "../driver/compile.h"
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

/* Build the synthesized program for one example. In-file model: the example runs with the
 * documented file's FULL context — every decl in the file, private/file-local included — not
 * just its public API (a doctest is a unit test, which shouldn't be limited to exports). So we
 * prepend the file's entire source, then the example (wrapped in a `proc main` if it declares
 * none). Returns an owned, NUL-terminated string.
 *
 * `core` is special — compile_source already prepends core.arche to every unit, so a doctest in
 * core/core.arche must NOT re-include it (that would redeclare the whole prelude); its decls are
 * in scope implicitly, so `file_prefix` is empty there. */
static char *synthesize(const char *file_source, int is_core, const DoctestExample *ex) {
	const char *prefix = is_core ? "" : file_source;
	size_t need = strlen(prefix) + strlen("\nmain :: proc() {\n}\n") + strlen(ex->code) + 8;
	char *out = malloc(need);
	if (!out)
		return NULL;
	if (ex->has_main)
		snprintf(out, need, "%s\n%s", prefix, ex->code);
	else
		snprintf(out, need, "%s\nmain :: proc() {\n%s}\n", prefix, ex->code);
	return out;
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
static int run_one(const char *path, int quiet_empty, int verbose, DtTally *t) {
	char *source = read_file(path);
	if (!source) {
		printf("FAIL %s  (cannot read)\n", path);
		return 1;
	}

	/* Parse the file and extract doctests from the (error-recovering) CST. A file
	 * with no extractable examples is NOT a doctest target — skip it silently,
	 * even if it has parse errors (e.g. intentional negative-test fixtures). We
	 * only surface failures for files that actually carry doctests. */
	ParseResult pr = parse_source(source);
	DoctestExamples ex = {NULL, 0};
	if (pr.cst_root)
		ex = doctest_extract(pr.cst_root, source);
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

		char *synth = synthesize(source, is_core, e);
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
		} else if (S_ISREG(sb.st_mode) && has_arche_ext(e->d_name)) {
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
