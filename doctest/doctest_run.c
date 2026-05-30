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

/* Build the synthesized program for one example: bring the documented file's
 * API into scope via `use <module>;`, then the example (wrapped in a `proc main`
 * if it does not declare one). Returns an owned, NUL-terminated string. */
static char *synthesize(const char *module, const DoctestExample *ex) {
	size_t need = strlen("use ;\nproc main() {\n}\n") + strlen(module) + strlen(ex->code) + 8;
	char *out = malloc(need);
	if (!out)
		return NULL;
	if (ex->has_main)
		snprintf(out, need, "use %s;\n%s", module, ex->code);
	else
		snprintf(out, need, "use %s;\nproc main() {\n%s}\n", module, ex->code);
	return out;
}

/* Run a freshly built executable with a wall-clock timeout. Returns its exit
 * code (>= 0), DT_CRASHED if it died by signal, or DT_TIMEOUT if it ran past the
 * limit (its whole process group is then SIGKILLed). */
static int run_executable(const char *exe) {
	/* Flush our buffered stdout before forking: otherwise the child inherits a
	 * copy of the buffer and freopen() re-emits it to the real stdout (duplicate
	 * lines when stdout is a pipe, i.e. under the test harness). */
	fflush(stdout);
	pid_t pid = fork();
	if (pid < 0)
		return DT_CRASHED;
	if (pid == 0) {
		setpgid(0, 0); /* own process group, so a timeout can kill any children it spawns */
		/* Silence the child's stdout so doctest output stays clean; a failing
		 * example is reported by exit code, not its chatter. */
		freopen("/dev/null", "w", stdout);
		execl(exe, exe, (char *)NULL);
		_exit(127); /* exec failed */
	}
	setpgid(pid, pid); /* also set from the parent to close the fork/exec race */

	int waited_ms = 0, status = 0;
	for (;;) {
		pid_t r = waitpid(pid, &status, WNOHANG);
		if (r == pid)
			break;
		if (r < 0)
			return DT_CRASHED;
		if (waited_ms >= DOCTEST_TIMEOUT_SECS * 1000) {
			kill(-pid, SIGKILL); /* kill the whole process group */
			waitpid(pid, &status, 0);
			return DT_TIMEOUT;
		}
		struct timespec ts = {0, 10 * 1000 * 1000}; /* 10ms */
		nanosleep(&ts, NULL);
		waited_ms += 10;
	}
	if (WIFEXITED(status))
		return WEXITSTATUS(status);
	return DT_CRASHED;
}

/* Accumulated tallies across one or many files. */
typedef struct {
	int files; /* files that had at least one example */
	int passed;
	int failed;
	int ignored;
} DtTally;

/* Run the doctests in one file. When quiet_empty is set, a file with no examples
 * prints nothing (used by recursive runs). Tallies fold into *t. Returns
 * non-zero if any example in this file failed. */
static int run_one(const char *path, int quiet_empty, DtTally *t) {
	char *source = read_file(path);
	if (!source) {
		fprintf(stderr, "doctest: cannot read %s\n", path);
		return 1;
	}

	/* Parse the documented file just to walk its declarations + doc comments. */
	ParseResult pr = parse_source(source);
	if (pr.error_count > 0 || !pr.cst_root) {
		fprintf(stderr, "doctest: %s has parse errors; cannot extract examples\n", path);
		for (size_t i = 0; i < pr.error_count; i++)
			fprintf(stderr, "  [Line %d] %s\n", pr.errors[i].line, pr.errors[i].message);
		parse_result_free(&pr);
		free(source);
		return 1;
	}

	DoctestExamples ex = doctest_extract(pr.cst_root, source);
	parse_result_free(&pr); /* examples own copies of everything they need */
	free(source);

	if (ex.count == 0) {
		if (!quiet_empty)
			printf("doctest %s: no examples found\n", path);
		doctest_examples_free(&ex);
		return 0;
	}
	t->files++;

	/* Each example program does `use <module>;`. compile_source resolves `use`
	 * relative to the directory of the path we hand it — point that at the
	 * documented file's own directory, so `use <module>;` finds the real file
	 * (and any modules it transitively uses) without copying anything. The synth
	 * path itself is never read from disk; only its directory matters. */
	char module[256], dir[512], synth_path[768];
	module_name_of(path, module, sizeof(module));
	dir_of(path, dir, sizeof(dir));
	snprintf(synth_path, sizeof(synth_path), "%s/__arche_doctest_synth__.arche", dir);

	int passed = 0, failed = 0, ignored = 0;
	printf("doctest %s: running %d example%s\n", path, ex.count, ex.count == 1 ? "" : "s");
	for (int i = 0; i < ex.count; i++) {
		DoctestExample *e = &ex.items[i];
		const char *result = "ok";
		char detail[64] = "";

		if (e->flags & DOCTEST_IGNORE) {
			ignored++;
			printf("  %s (line %d): ignored\n", e->decl_name, e->src_line);
			continue;
		}

		char *synth = synthesize(module, e);
		char exe[256];
		snprintf(exe, sizeof(exe), "/tmp/arche_dt_%d_%d", (int)getpid(), i);
		CompileOpts opts = {0};
		opts.quiet = 1;
		int rc = synth ? compile_source(synth, synth_path, exe, &opts) : 1;
		free(synth);

		int ok;
		if (e->flags & DOCTEST_COMPILE_FAIL) {
			/* Expected to fail compilation; must NOT be run. */
			ok = (rc != 0);
			if (!ok)
				snprintf(detail, sizeof(detail), " (expected compile error, but it compiled)");
		} else if (rc != 0) {
			ok = 0;
			snprintf(detail, sizeof(detail), " (compile error)");
		} else if (e->flags & DOCTEST_NO_RUN) {
			ok = 1; /* compiled; not run by request */
			unlink(exe);
		} else {
			int code = run_executable(exe);
			unlink(exe);
			if (e->flags & DOCTEST_SHOULD_PANIC) {
				ok = (code > 0); /* a clean exit (0) or a timeout is not the expected panic */
				if (!ok)
					snprintf(detail, sizeof(detail),
					         code == DT_TIMEOUT ? " (timed out)" : " (expected panic, exited 0)");
			} else {
				ok = (code == 0);
				if (!ok) {
					if (code == DT_TIMEOUT)
						snprintf(detail, sizeof(detail), " (timed out)");
					else if (code == DT_CRASHED)
						snprintf(detail, sizeof(detail), " (crashed)");
					else
						snprintf(detail, sizeof(detail), " (exit %d)", code);
				}
			}
		}

		if (ok)
			passed++;
		else {
			failed++;
			result = "FAILED";
		}
		printf("  %s (line %d): %s%s\n", e->decl_name, e->src_line, result, detail);
	}

	if (ignored)
		printf("doctest %s: %d passed, %d failed, %d ignored\n", path, passed, failed, ignored);
	else
		printf("doctest %s: %d passed, %d failed\n", path, passed, failed);
	t->passed += passed;
	t->failed += failed;
	t->ignored += ignored;
	doctest_examples_free(&ex);
	return failed > 0 ? 1 : 0;
}

int doctest_run_file(const char *path) {
	DtTally t = {0, 0, 0, 0};
	return run_one(path, 0, &t);
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

static int run_dir(const char *root) {
	PathList pl = {NULL, 0, 0};
	collect_arche(root, &pl);
	qsort(pl.items, (size_t)pl.count, sizeof(char *), cmp_str); /* deterministic order */

	DtTally t = {0, 0, 0, 0};
	int any_failed = 0;
	for (int i = 0; i < pl.count; i++) {
		if (run_one(pl.items[i], 1 /* quiet on empty */, &t))
			any_failed = 1;
		free(pl.items[i]);
	}
	free(pl.items);

	if (t.files == 0) {
		printf("doctest %s: no examples found in any file\n", root);
		return 0;
	}
	printf("doctest %s: %d passed, %d failed, %d ignored across %d file%s\n", root, t.passed, t.failed, t.ignored,
	       t.files, t.files == 1 ? "" : "s");
	return any_failed ? 1 : 0;
}

int doctest_run_path(const char *spec) {
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
		return run_dir(root);
	}

	struct stat sb;
	if (stat(spec, &sb) == 0 && S_ISDIR(sb.st_mode))
		return run_dir(spec);

	return doctest_run_file(spec);
}
