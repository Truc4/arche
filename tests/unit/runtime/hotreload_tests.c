/* Deterministic unit test for the dev hot-reload runtime (runtime/hotreload.c).
 *
 * This replaces three fragile, timing-based Python integration tests. It drives the SAME entry points the
 * codegen-emitted host uses (arche_hot_register / arche_hot_resolve / arche_hot_gen) against tiny fixture
 * `.so`s we build here with `cc -shared`. No arche compiler, no window, NO sleeps: file changes are made
 * visible by setting mtime EXPLICITLY with utimensat, so every "did it reload?" check is a deterministic
 * state transition rather than a race. The edge cases below (same-second mtime, truncated `.so`, dlopen
 * realpath caching, broken rebuild, keep-last-good) are exactly the failure modes a reload runtime has —
 * and the same ones fixed in hotreload.c, so this doubles as their regression guard. Run under ASan to
 * catch dlopen handle leaks / use-after-free across reloads. */
#define _GNU_SOURCE
#include "../../../runtime/hotreload.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Test harness (same minimal pattern as tests/unit/compiler/semantic_tests.c). */
int test_count = 0;
int test_pass = 0;
int test_fail = 0;

void test_start(const char *name) {
	test_count++;
	printf("  [%d] %s ", test_count, name);
	fflush(stdout);
}
void test_pass_msg(void) {
	test_pass++;
	printf("\xe2\x9c\x93\n");
}
void test_fail_msg(const char *reason) {
	test_fail++;
	printf("\xe2\x9c\x97 (%s)\n", reason);
}
#define ASSERT_TRUE(cond, msg)                                                                                         \
	if (!(cond)) {                                                                                                     \
		test_fail_msg(msg);                                                                                            \
		return;                                                                                                        \
	}
#define ASSERT_EQ(a, b, msg)                                                                                           \
	if ((a) != (b)) {                                                                                                  \
		test_fail_msg(msg);                                                                                            \
		return;                                                                                                        \
	}

/* Shared fixture state (set in main). */
static char g_dir[256];     /* temp dir = $ARCHE_HOT_DIR */
static char g_so[300];      /* the device .so path the runtime watches: <dir>/u.so */
static long g_clock = 1000; /* monotonically-increasing fake seconds for explicit mtimes */

static void write_text(const char *path, const char *text) {
	FILE *f = fopen(path, "wb");
	if (f) {
		fwrite(text, 1, strlen(text), f);
		fclose(f);
	}
}

/* Build a fixture `.so` (overwriting g_so) whose `tick()` returns `val`, or — if body is given — from
 * arbitrary C. Returns 0 on success. */
static int build_so(const char *body) {
	char cpath[400];
	snprintf(cpath, sizeof(cpath), "%s/_fix.c", g_dir);
	write_text(cpath, body);
	char cmd[1024];
	snprintf(cmd, sizeof(cmd), "cc -shared -fPIC -o %s %s 2>/dev/null", g_so, cpath);
	return system(cmd);
}
static int build_tick(int val) {
	char body[128];
	snprintf(body, sizeof(body), "int tick(void){return %d;}\n", val);
	return build_so(body);
}

/* Set g_so's mtime to {sec, nsec} (both atime+mtime). The runtime compares NANOSECOND mtime. */
static void set_mtime(long sec, long nsec) {
	struct timespec ts[2];
	ts[0].tv_sec = sec;
	ts[0].tv_nsec = nsec;
	ts[1] = ts[0];
	utimensat(AT_FDCWD, g_so, ts, 0);
}
/* Replace g_so with `tick`→val and stamp a strictly-newer second (the common "rebuilt" case). */
static int rebuild(int val) {
	int rc = build_tick(val);
	set_mtime(g_clock++, 0);
	return rc;
}

/* Call the unit's current `tick`, or return -1 if the symbol is unresolved (NULL). memcpy avoids the
 * void*→fn-ptr cast warning under -Werror. */
static int call_tick(void) {
	void *p = arche_hot_resolve(0, "tick");
	if (!p)
		return -1;
	int (*fn)(void);
	memcpy(&fn, &p, sizeof fn);
	return fn();
}

/* --- Cases --- */

static void test_first_load(void) {
	test_start("register + first resolve loads the .so");
	ASSERT_EQ(rebuild(105), 0, "fixture build failed");
	ASSERT_EQ(call_tick(), 105, "first load did not return v1");
	ASSERT_EQ(arche_hot_gen(0), 1u, "gen should be 1 after first load");
	test_pass_msg();
}

static void test_reload_new_code(void) {
	/* Overwriting the file + a newer mtime must hand back NEW code — this is the core reload AND the
	 * proof we defeat dlopen's realpath cache (same path, must not return the stale image). */
	test_start("rebuild + newer mtime -> reload returns new code");
	ASSERT_EQ(rebuild(205), 0, "fixture rebuild failed");
	ASSERT_EQ(call_tick(), 205, "reload did not pick up v2 (stale dlopen cache?)");
	ASSERT_EQ(arche_hot_gen(0), 2u, "gen should be 2 after one reload");
	test_pass_msg();
}

static void test_same_second_nsec(void) {
	/* A save within the SAME wall-clock second as the last load must still reload — seconds-granularity
	 * mtime would silently miss it (the bug fixed in hotreload.c). Stamp same sec, larger nsec. */
	test_start("same-second, larger-nsec mtime still reloads");
	long sec = g_clock; /* same second as the previous rebuild used (g_clock-1)+? — force equality below */
	build_tick(105);
	set_mtime(sec, 0);          /* baseline at this exact second */
	(void)call_tick();          /* load it (records mtime sec,0) */
	build_tick(305);            /* change content... */
	set_mtime(sec, 500000000L); /* ...same second, +0.5s in nanoseconds */
	ASSERT_EQ(call_tick(), 305, "same-second edit was missed (seconds-only mtime?)");
	g_clock = sec + 1; /* keep the monotonic clock ahead for later cases */
	test_pass_msg();
}

static void test_no_change_no_reload(void) {
	test_start("no file change -> no reload (cached handle, gen steady)");
	unsigned before = arche_hot_gen(0);
	int v = call_tick();
	int v2 = call_tick(); /* nothing touched the file */
	ASSERT_EQ(v, v2, "value changed without an edit");
	ASSERT_EQ(arche_hot_gen(0), before, "gen advanced without an edit (needless reload)");
	test_pass_msg();
}

static void test_truncated_keeps_last_good(void) {
	/* A truncated / mid-write .so (non-ELF) must FAIL to load and the runtime must keep serving the
	 * last-good handle — the reader surviving a non-atomic writer. */
	test_start("truncated .so -> keep last-good, no crash");
	ASSERT_EQ(rebuild(205), 0, "good rebuild failed");
	ASSERT_EQ(call_tick(), 205, "precondition: good handle loaded");
	unsigned good_gen = arche_hot_gen(0);
	write_text(g_so, "not a real shared object\n"); /* garbage, newer mtime */
	set_mtime(g_clock++, 0);
	ASSERT_EQ(call_tick(), 205, "truncated load should keep the last-good symbol");
	ASSERT_EQ(arche_hot_gen(0), good_gen, "a failed load must not advance gen");
	test_pass_msg();
}

static void test_broken_missing_symbol(void) {
	/* A VALID rebuild that no longer exports the symbol: it loads, but the symbol resolves to NULL — the
	 * host no-ops rather than crashing. */
	test_start("valid rebuild missing the symbol -> resolve NULL, no crash");
	ASSERT_EQ(build_so("int nottick(void){return 0;}\n"), 0, "broken fixture build failed");
	set_mtime(g_clock++, 0);
	ASSERT_EQ(call_tick(), -1, "missing symbol should resolve to NULL");
	test_pass_msg();
}

static void test_repeated_reload_no_leak(void) {
	/* Many reloads: each must dlclose the superseded handle (ASan/LSan flags a leak otherwise). */
	test_start("50 reloads alternate cleanly (no handle leak under ASan)");
	for (int k = 0; k < 50; k++) {
		int want = (k & 1) ? 205 : 105;
		ASSERT_EQ(rebuild(want), 0, "rebuild failed mid-loop");
		ASSERT_EQ(call_tick(), want, "reload mismatch mid-loop");
	}
	test_pass_msg();
}

int main(void) {
	snprintf(g_dir, sizeof(g_dir), "/tmp/arche_hrtest_XXXXXX");
	if (!mkdtemp(g_dir)) {
		fprintf(stderr, "could not create temp dir\n");
		return 1;
	}
	setenv("ARCHE_HOT_DIR", g_dir, 1);
	snprintf(g_so, sizeof(g_so), "%s/u.so", g_dir);
	arche_hot_register(0, "u.so");

	printf("Hot-reload runtime tests:\n");
	test_first_load();
	test_reload_new_code();
	test_same_second_nsec();
	test_no_change_no_reload();
	test_truncated_keeps_last_good();
	test_broken_missing_symbol();
	test_repeated_reload_no_leak();

	char rm[400];
	snprintf(rm, sizeof(rm), "rm -rf %s", g_dir);
	if (system(rm) != 0)
		fprintf(stderr, "warning: temp cleanup failed: %s\n", g_dir);

	printf("\nResults: %d/%d passed\n", test_pass, test_count);
	if (test_fail > 0) {
		printf("  %d failed\n", test_fail);
		return 1;
	}
	return 0;
}
