/* `arche run` uses POSIX process + temp-file calls (mkdtemp, fork, execv, waitpid, unlink, rmdir),
 * which glibc hides under -std=c99 without a feature-test macro. */
#define _POSIX_C_SOURCE 200809L
#include "../codegen/codegen.h"
#include "../compile/compile.h"
#include "../compile/variant_select.h"
#include "../semantic/semantic.h"
#include "args.h"
#include "cli.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Copy `src` (a freshly-linked executable) to `dst` and mark it executable. The dev-loop watcher relinks
 * the host exe in place on each edit; the live child runs this copy so a rebuild can't hit ETXTBSY. */
static int copy_exec(const char *src, const char *dst) {
	int in = open(src, O_RDONLY);
	if (in < 0)
		return -1;
	int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0700);
	if (out < 0) {
		close(in);
		return -1;
	}
	char buf[1 << 16];
	ssize_t r;
	int err = 0;
	while ((r = read(in, buf, sizeof(buf))) > 0) {
		ssize_t off = 0;
		while (off < r) {
			ssize_t w = write(out, buf + off, (size_t)(r - off));
			if (w < 0) {
				err = 1;
				break;
			}
			off += w;
		}
		if (err)
			break;
	}
	if (r < 0)
		err = 1;
	close(in);
	close(out);
	return err ? -1 : 0;
}

/* Newest modification time of any `.arche` file under `root` (recursive), in NANOSECONDS. The dev-loop
 * watcher samples this and rebuilds when it bumps. Nanosecond precision is load-bearing: a save within the
 * same wall-clock second as the previous build must still be detected (seconds-granularity `st_mtime` would
 * silently miss a fast edit). Cheap, dependency-free edit detection across every device in the project. */
static long long latest_arche_mtime(const char *root) {
	DIR *d = opendir(root);
	if (!d)
		return 0;
	long long newest = 0;
	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		if (e->d_name[0] == '.') /* skip ., .., and dotdirs (build/.arche-* etc.) */
			continue;
		char path[2048];
		snprintf(path, sizeof(path), "%s/%s", root, e->d_name);
		struct stat st;
		if (stat(path, &st) != 0)
			continue;
		if (S_ISDIR(st.st_mode)) {
			long long sub = latest_arche_mtime(path);
			if (sub > newest)
				newest = sub;
		} else {
			size_t len = strlen(e->d_name);
			if (len >= 6 && strcmp(e->d_name + len - 6, ".arche") == 0) {
				long long m = (long long)st.st_mtim.tv_sec * 1000000000LL + (long long)st.st_mtim.tv_nsec;
				if (m > newest)
					newest = m;
			}
		}
	}
	closedir(d);
	return newest;
}

enum {
	R_WNO_PCBF = 1,
	R_WNO_PNE,
	R_WERR_PCBF,
	R_WERR_PNE,
	R_WERR,
	R_ALLOW_UNDEFINED,
	R_WHOLE_PROGRAM,
	R_EXPORTED_MUTABLE,
	R_PROC_LEAF,
	R_SYS_FOREIGN_WRITE,
	R_POOL_INDEX,
	R_PROC_NOT_PRIMITIVE,
	R_DISCARDED_OK,
	R_WNO_LSA,
	R_WERR_LSA
};

static const ArgSpec k_run_specs[] = {
    {R_WNO_PCBF, "-Wno-proc-could-be-func", ARG_FLAG, 0, 0, NULL, "disable the proc-could-be-func lint"},
    {R_WNO_PNE, "-Wno-proc-no-effect", ARG_FLAG, 0, 0, NULL, "disable the proc-no-effect lint"},
    {R_WERR_PCBF, "-Werror=proc-could-be-func", ARG_FLAG, 0, 0, NULL,
     "promote the proc-could-be-func lint to an error"},
    {R_WERR_PNE, "-Werror=proc-no-effect", ARG_FLAG, 0, 0, NULL, "promote the proc-no-effect lint to an error"},
    {R_WERR, "-Werror", ARG_FLAG, 0, 0, NULL, "promote all lints to errors"},
    {R_WNO_LSA, "-Wno-large-stack-array", ARG_FLAG, 0, 0, NULL, "disable the large-stack-array lint (W0026)"},
    {R_WERR_LSA, "-Werror=large-stack-array", ARG_FLAG, 0, 0, NULL,
     "promote the large-stack-array lint (W0026) to an error"},
    {R_ALLOW_UNDEFINED, "--allow-undefined", ARG_FLAG, 0, 0, NULL,
     "permit the raw, runtime-unsafe `!undefined` opt-out (forbidden by default)"},
    {R_WHOLE_PROGRAM, "--whole-program", ARG_FLAG, 0, 0, NULL,
     "force a whole-program build (run defaults to incremental: per-device object cache, fast rebuilds)"},
    {R_EXPORTED_MUTABLE, "--exported-mutable", ARG_VALUE, 0, 0, "<level>",
     "exported-mutable-global lint (W0022): error (default) | warn | allow"},
    {R_PROC_LEAF, "--proc-leaf", ARG_VALUE, 0, 0, "<level>",
     "proc-calls-proc lint (W0028): warn (default) | error | allow"},
    {R_SYS_FOREIGN_WRITE, "--map-foreign-write", ARG_VALUE, 0, 0, "<level>",
     "map-writes-foreign-pool lint (W0024): error (default) | warn | allow"},
    {R_POOL_INDEX, "--pool-index", ARG_VALUE, 0, 0, "<level>",
     "pool-index-outside-query lint (W0029): warn (default) | error | allow"},
    {R_PROC_NOT_PRIMITIVE, "--proc-not-primitive", ARG_VALUE, 0, 0, "<level>",
     "proc-not-primitive lint (W0030): error (default) | warn | allow"},
    {R_DISCARDED_OK, "--discarded-ok", ARG_VALUE, 0, 0, "<level>",
     "discarded-ok lint (W0016): error (default) | warn | allow"},
    {0, NULL, ARG_FLAG, 0, 0, NULL, NULL},
};

/* `arche run <file> [prog-args...]` / `arche run <file> -- [prog-args...]`: compile to a temp
 * executable, run it with the forwarded arguments, and return the program's exit code. The first
 * positional is the source file; any remaining positionals plus everything after `--` are the
 * program's argv (so both `run f a b` and `run f -- a b` work). */
int run_run(int argc, char **argv, const GlobalOpts *g) {
	(void)g;
	ArgParse p;
	if (args_parse(k_run_specs, argc, argv, &p) != 0) {
		fprintf(stderr, "%s: %s\n", g_prog, p.err);
		args_usage(stderr, g_prog, "run", "[flags] <input.arche> [-- program-args...]", k_run_specs);
		return ARCHE_USAGE;
	}
	if (p.want_help) {
		args_usage(stdout, g_prog, "run", "[flags] <input.arche> [-- program-args...]", k_run_specs);
		return ARCHE_OK;
	}

	int pcbf_en = 1, pcbf_we = 0, pne_en = 1, pne_we = 0, lsa_en = 1, lsa_we = 0;
	if (args_has(&p, R_WNO_PCBF))
		pcbf_en = 0;
	if (args_has(&p, R_WNO_PNE))
		pne_en = 0;
	if (args_has(&p, R_WNO_LSA))
		lsa_en = 0;
	if (args_has(&p, R_WERR_PCBF))
		pcbf_we = 1;
	if (args_has(&p, R_WERR_PNE))
		pne_we = 1;
	if (args_has(&p, R_WERR_LSA))
		lsa_we = 1;
	if (args_has(&p, R_WERR)) {
		pcbf_we = 1;
		pne_we = 1;
		lsa_we = 1;
	}
	semantic_set_lint_proc_could_be_func(pcbf_en, pcbf_we);
	semantic_set_lint_proc_no_effect(pne_en, pne_we);
	semantic_set_lint_large_stack_array(lsa_en, lsa_we);
	semantic_set_allow_undefined(args_has(&p, R_ALLOW_UNDEFINED));
	if (cli_apply_exported_mutable(args_value(&p, R_EXPORTED_MUTABLE)) != 0) {
		fprintf(stderr, "%s: --exported-mutable expects error|warn|allow\n", g_prog);
		args_usage(stderr, g_prog, "run", "[flags] <input.arche> [-- program-args...]", k_run_specs);
		return ARCHE_USAGE;
	}
	if (cli_apply_proc_leaf(args_value(&p, R_PROC_LEAF)) != 0) {
		fprintf(stderr, "%s: --proc-leaf expects error|warn|allow\n", g_prog);
		args_usage(stderr, g_prog, "run", "[flags] <input.arche> [-- program-args...]", k_run_specs);
		return ARCHE_USAGE;
	}
	if (cli_apply_map_foreign_write(args_value(&p, R_SYS_FOREIGN_WRITE)) != 0) {
		fprintf(stderr, "%s: --map-foreign-write expects error|warn|allow\n", g_prog);
		args_usage(stderr, g_prog, "run", "[flags] <input.arche> [-- program-args...]", k_run_specs);
		return ARCHE_USAGE;
	}
	if (cli_apply_proc_not_primitive(args_value(&p, R_PROC_NOT_PRIMITIVE)) != 0) {
		fprintf(stderr, "%s: --proc-not-primitive expects error|warn|allow\n", g_prog);
		args_usage(stderr, g_prog, "run", "[flags] <input.arche> [-- program-args...]", k_run_specs);
		return ARCHE_USAGE;
	}
	if (cli_apply_discarded_ok(args_value(&p, R_DISCARDED_OK)) != 0) {
		fprintf(stderr, "%s: --discarded-ok expects error|warn|allow\n", g_prog);
		args_usage(stderr, g_prog, "run", "[flags] <input.arche> [-- program-args...]", k_run_specs);
		return ARCHE_USAGE;
	}
	if (cli_apply_pool_index(args_value(&p, R_POOL_INDEX)) != 0) {
		fprintf(stderr, "%s: --pool-index expects error|warn|allow\n", g_prog);
		args_usage(stderr, g_prog, "run", "[flags] <input.arche> [-- program-args...]", k_run_specs);
		return ARCHE_USAGE;
	}

	if (p.pos_count == 0) {
		fprintf(stderr, "%s: no input file\n", g_prog);
		args_usage(stderr, g_prog, "run", "[flags] <input.arche> [-- program-args...]", k_run_specs);
		return ARCHE_USAGE;
	}
	/* First positional is the source (a file, or a directory to resolve); the rest are program args. */
	char *input = cli_resolve_input(p.pos[0]);
	if (!input)
		return ARCHE_USAGE;
	char *src = cli_read_file(input);
	if (!src) {
		free(input);
		return ARCHE_ERR;
	}

	/* Build into a private temp dir (mkdtemp, mode 0700 — same approach as the driver's workdir). */
	char dir[] = "/tmp/arche_run_XXXXXX";
	if (!mkdtemp(dir)) {
		perror("Failed to create temp dir");
		free(src);
		return ARCHE_ERR;
	}
	char exe[600];
	snprintf(exe, sizeof(exe), "%s/a.out", dir);

	/* `arche run` is the dev-iteration path → default to device-granular incremental codegen (per-unit +
	 * object cache) so editing one device only recompiles that device. `arche build` stays whole-program
	 * (full cross-device inlining) for release. `--whole-program` opts run out. */
	if (args_has(&p, R_WHOLE_PROGRAM))
		codegen_force_whole_program(); /* hard override, beats the ARCHE_PER_UNIT env too */
	else
		codegen_set_per_unit(1);
	variant_select_set_warnings(1); /* warn on a typo'd / undefined target (compiler only) */

	/* Project root = the `arche.toml` dir, else the source dir. Anchors the (persistent) object cache AND
	 * the hot-reload `.so` dir; it is also the tree the dev-loop watcher polls for edits. */
	char src_dir[1024];
	snprintf(src_dir, sizeof(src_dir), "%s", input);
	char *sd_slash = strrchr(src_dir, '/');
	if (sd_slash)
		*sd_slash = '\0';
	else
		snprintf(src_dir, sizeof(src_dir), ".");
	char proj[1024];
	if (!variant_manifest_dir(src_dir, proj, sizeof(proj)))
		snprintf(proj, sizeof(proj), "%s", src_dir);

	/* The run output is a throwaway temp, so anchor the object cache to the project root — otherwise the
	 * cache would vanish each run. Don't clobber an explicit ARCHE_CACHE_DIR. */
	if (!getenv("ARCHE_CACHE_DIR")) {
		char cache[1200];
		snprintf(cache, sizeof(cache), "%s/build/.arche-cache", proj); /* under the build/ dir convention */
		setenv("ARCHE_CACHE_DIR", cache, 0);
	}

	/* DEV = HOT (no flag): `arche run` IS the hot-reload loop. Each imported device builds to its own `.so`
	 * under ARCHE_HOT_DIR; the host's cross-device calls route through the reload runtime (codegen `ctx->hot`,
	 * enabled in compile.c when ARCHE_HOT_DIR is set). While the host lives we watch the project tree and
	 * rebuild the changed device's `.so` in place — the host picks it up on its next call. A one-shot program
	 * (no loop) just exits, so the watcher returns immediately. `--whole-program` is the release-style codegen
	 * path (static, direct calls), so it opts OUT of hot — and `arche build` never sets ARCHE_HOT_DIR at all.
	 * Don't clobber an explicit ARCHE_HOT_DIR (the lit/test harness sets its own). */
	int hot = !args_has(&p, R_WHOLE_PROGRAM);
	char hotdir[1200];
	if (hot && !getenv("ARCHE_HOT_DIR")) {
		snprintf(hotdir, sizeof(hotdir), "%s/build/.arche-hot", proj);
		setenv("ARCHE_HOT_DIR", hotdir, 1);
	}

	/* Dev state inspector: the host (built with inspect.o in hot mode) serves its live pools on this Unix
	 * socket, under the same dir the watcher manages. `arche inspect` connects here. One socket per project
	 * (running two sessions of the same project simultaneously collides — last one wins). */
	if (hot && !getenv("ARCHE_INSPECT_SOCK")) {
		const char *hd = getenv("ARCHE_HOT_DIR");
		char sock[1300];
		snprintf(sock, sizeof(sock), "%s/inspect.sock", hd ? hd : proj);
		setenv("ARCHE_INSPECT_SOCK", sock, 1);
	}

	CompileOpts opts = {0};
	opts.quiet = 1; /* `go run`-style: no pipeline chatter, just the program's own output */
	int rc = compile_source(src, input, exe, &opts);
	free(src);
	if (rc != 0) {
		unlink(exe);
		rmdir(dir);
		return rc;
	}

	/* The watcher rebuilds the host exe in place on each edit; the running child must therefore exec a COPY
	 * (Linux refuses to relink a file that is a running program's text image — ETXTBSY). */
	char exe_run[640];
	snprintf(exe_run, sizeof(exe_run), "%s/a.run", dir);
	if (copy_exec(exe, exe_run) != 0) {
		fprintf(stderr, "%s: could not stage run image\n", g_prog);
		unlink(exe);
		rmdir(dir);
		return ARCHE_ERR;
	}

	/* Assemble the child's argv: exe, then remaining positionals, then post-`--` args. */
	char *cargv[ARG_MAX_POS * 2 + 2];
	int n = 0;
	cargv[n++] = exe_run;
	for (int i = 1; i < p.pos_count; i++)
		cargv[n++] = (char *)p.pos[i];
	for (int i = 0; i < p.fwd_count; i++)
		cargv[n++] = (char *)p.fwd[i];
	cargv[n] = NULL;

	pid_t pid = fork();
	if (pid < 0) {
		perror("fork");
		unlink(exe);
		unlink(exe_run);
		rmdir(dir);
		return ARCHE_ERR;
	}
	if (pid == 0) {
		execv(exe_run, cargv);
		perror("exec");
		_exit(127);
	}

	/* Dev-loop watcher (hot only): poll the project tree's newest .arche mtime; while the host is alive, a
	 * bump means a device was edited → recompile (the object cache reuses unchanged units; only the touched
	 * device's `.so` is relinked into ARCHE_HOT_DIR, where the running host reloads it). The host exe is
	 * rebuilt too but never re-exec'd — the live child keeps running on its staged copy. `--whole-program`
	 * (not hot) just blocks on the child like a plain `go run`.
	 *
	 * TODO(hot-reload, deferred — see docs/hot_reload.md "Deferred rebuild work" for the why/why-not):
	 *  - BENCHMARK then maybe trim the rebuild: an edit re-runs the WHOLE front-end (parse+analyze every
	 *    unit) + relinks the changed device `.so`. Fine for small projects (sub-second); measure before
	 *    adding front-end incrementality, which is real complexity for a dev-only path.
	 *  - DEBOUNCE: a burst of saves triggers a rebuild per 200ms tick. Partial writes are already retried
	 *    (advance watch_mtime only on success), so this is cosmetic; add coalescing only if it churns.
	 *  - PER-CALL dispatch cost: arche_hot_resolve does stat+dlsym per cross-device call. Device calls are
	 *    coarse (~tens/frame; a system call processes all rows at once), so it's ~0.2% at 60fps — measure
	 *    before caching the resolved pointer + a reload generation counter.
	 *  - CLEANUP: ARCHE_HOT_DIR accumulates unit_N.so + the runtime's versioned .hot.<gen> copies across a
	 *    session; prune stale ones (keep the live generation) on startup/exit.
	 *  - FLAKY-HARNESS NOTE: a Python subprocess launching this with stdout=DEVNULL + stderr=file +
	 *    start_new_session can wedge the host on some boxes (shell `>/dev/null` and a piped stdout are
	 *    fine); the integration tests route stdout to a file + retry once. Root-cause the fd interaction. */
	long long watch_mtime = hot ? latest_arche_mtime(proj) : 0;
	int status = 0;
	if (!hot) {
		while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
			;
	} else
		for (;;) {
			pid_t w = waitpid(pid, &status, WNOHANG);
			if (w == pid)
				break;
			if (w < 0) {
				if (errno == EINTR)
					continue;
				break;
			}
			struct timespec ts = {0, 200 * 1000 * 1000}; /* 200ms poll */
			nanosleep(&ts, NULL);
			long long m = latest_arche_mtime(proj);
			if (getenv("ARCHE_HOT_DEBUG"))
				fprintf(stderr, "[hot] proj=%s base=%lld now=%lld %s\n", proj, watch_mtime, m,
				        m > watch_mtime ? "REBUILD" : "-");
			if (m > watch_mtime) {
				char *nsrc = cli_read_file(input);
				if (nsrc) {
					CompileOpts ropts = {0};
					ropts.quiet = 1;
					int brc = compile_source(nsrc, input, exe, &ropts);
					free(nsrc);
					if (brc == 0)
						watch_mtime = m; /* advance only on success, so a transient bad edit is retried */
					else
						fprintf(stderr, "%s: rebuild failed — host keeps running on last-good code\n", g_prog);
				} else {
					watch_mtime = m; /* couldn't read the entry file; don't spin on it */
				}
			}
		}

	unlink(exe);
	unlink(exe_run);
	rmdir(dir);
	if (WIFEXITED(status))
		return WEXITSTATUS(status);
	if (WIFSIGNALED(status))
		return 128 + WTERMSIG(status);
	return ARCHE_ERR;
}

const ArgSpec *run_specs(void) {
	return k_run_specs;
}
