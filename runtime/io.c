#define _GNU_SOURCE
#include <fcntl.h>
#include <float.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
/* wasm32-wasi has neither `mmap` nor pthreads — the file map falls back to read-into-buffer and the
 * `cores` reduce backend degrades to a serial fold (see below). Native keeps both. */
#ifndef __wasm__
#include <pthread.h>
#include <sys/mman.h>
#endif

/* Returns `float` (f32), not `double`: arche `float` is f32, and the FFI return type must match the
 * caller's ABI or it reads the wrong register bytes (garbage). For sub-second timing prefer `os.now_ms`
 * (pure-arche `clock_gettime` via syscall) — f32 monotonic seconds is coarse (~0.06s ULP at typical
 * uptimes). (The integer ms clock and the sleep now live in os.arche as pure-arche syscall wrappers.) */
float os_now_sec(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (float)(ts.tv_sec + ts.tv_nsec * 1e-9);
}

/* Write a NUL-terminated string to a raw fd — the panic-path print for core.arche's abort policies.
 * A non-variadic replacement for libc `dprintf`: on wasm32 the strict-signature link rejects calling
 * variadic `dprintf` with a fixed `(fd, fmt)` prototype (it compiles to `(i32,i32,i32)`), so use plain
 * `write` instead. Returns the byte count so it matches the arche `(r: int)` result. */
int arche_eputs(int fd, const char *s) {
	return (int)write(fd, s, strlen(s));
}

/* The file/stdio family (stdin/stdout/stderr, fopen/fread/fwrite/fclose, fread_line,
 * csv_read_chunk) now lives in core.arche as pure-Arche syscall wrappers — a `file` is a raw
 * fd. Only the mmap-based file map, the clock, and argv remain here (they need a language
 * primitive — array-from-address / freestanding entry — before they can move too). */

/* =========================
   Whole-file mmap as a raw byte view
   =========================
   arche_file_map returns a pointer to the mapped file; Arche treats it as a
   char[] (raw i8*) and scans it with i64 offsets, so files larger than 2 GB
   work. The size is stashed for arche_file_size(), which must be called right
   after the map (single-threaded; one map "in flight" per load is enough). */

static long g_arche_map_size;

char *arche_file_map(const char *path) {
	int fd = open(path, O_RDONLY);
	if (fd < 0) {
		g_arche_map_size = 0;
		return 0;
	}
	struct stat st;
	if (fstat(fd, &st) < 0) {
		close(fd);
		g_arche_map_size = 0;
		return 0;
	}
#ifdef __wasm__
	/* No mmap under wasm: read the whole file into a malloc'd buffer (freed by arche_file_unmap). */
	char *data = (st.st_size > 0) ? malloc((size_t)st.st_size) : malloc(1);
	if (!data) {
		close(fd);
		g_arche_map_size = 0;
		return 0;
	}
	long off = 0;
	while (off < (long)st.st_size) {
		long r = (long)read(fd, data + off, (size_t)((long)st.st_size - off));
		if (r <= 0)
			break;
		off += r;
	}
	close(fd);
	g_arche_map_size = off;
	return data;
#else
	char *data = mmap(0, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (data == MAP_FAILED) {
		g_arche_map_size = 0;
		return 0;
	}
	madvise(data, st.st_size, MADV_SEQUENTIAL);
	g_arche_map_size = (long)st.st_size;
	return data;
#endif
}

long arche_file_size(void) {
	return g_arche_map_size;
}

void arche_file_unmap(char *data, long size) {
#ifdef __wasm__
	(void)size;
	free(data);
#else
	if (data)
		munmap(data, (size_t)size);
#endif
}

/* =========================
   Program command-line args
   =========================
   The codegen-emitted main() forwards (argc, argv) here once at startup; Arche
   programs read them back via os_argc()/os_argv(i). argv[i] is returned
   as a raw char* (Arche char[]); length is found with strlen, as with the file
   APIs above. Out-of-range indices return 0. */

static int g_arche_argc;
static char **g_arche_argv;

void arche_set_args(int argc, char **argv) {
	g_arche_argc = argc;
	g_arche_argv = argv;
}

int os_argc(void) {
	return g_arche_argc;
}

char *os_argv(int i) {
	/* Out of range → a valid empty string, NEVER NULL: the Arche side slices the result
	 * (`raw[0:len]`), and a GEP off a NULL base is UB the optimizer can turn into a crash. */
	static char empty[1] = "";
	return (i >= 0 && i < g_arche_argc) ? g_arche_argv[i] : empty;
}

/* FFI-boundary length for argv[i]: a char[] crossing IN from C has no carried length, so the os
 * wrapper materializes one here with strlen — NUL is a C-ABI detail confined to this boundary;
 * arche-side the result is a normal `(ptr, len)` slice. Index-based so no char[] crosses out. */
long os_argv_len(int i) {
	return (i >= 0 && i < g_arche_argc) ? (long)strlen(g_arche_argv[i]) : 0;
}

/* ---- data-parallel column reduce across CPU cores (the `cores` backend's primitive) ----------------
 * arche is otherwise single-threaded; this is STRUCTURED data parallelism only — the column is split
 * into disjoint chunks, each thread folds its chunk with the monoid, and the partials are combined with
 * the same op. Associativity is the license (same as the SIMD reduce). No shared mutable state during
 * the fold → no locks; deterministic modulo float reassociation. No arche-heap allocation (OS thread
 * stacks; a small fixed worker array). Threads are spawned per call (fine for the large-n reductions
 * that select this backend); small n folds serially. op codes MUST match codegen's emitted constants. */
enum { PR_ADD = 0, PR_MUL = 1, PR_MIN = 2, PR_MAX = 3 };
#define PR_MAX_THREADS 16
#define PR_MIN_PER_THREAD 16384

#ifdef __wasm__
/* wasm32 has no threads: the `cores` reduce backend is a serial monoid fold. codegen declares
 * `arche_par_reduce_<T>` unconditionally, so we still define them — just single-threaded. */
#define PR_GEN(NAME, T, ID_MIN, ID_MAX)                                                                                \
	T arche_par_reduce_##NAME(const T *col, long long n, int op) {                                                     \
		T acc = (op == PR_ADD) ? (T)0 : (op == PR_MUL) ? (T)1 : (op == PR_MIN) ? (T)(ID_MIN) : (T)(ID_MAX);            \
		for (long long i = 0; i < n; i++) {                                                                            \
			T v = col[i];                                                                                              \
			if (op == PR_ADD)                                                                                          \
				acc = acc + v;                                                                                         \
			else if (op == PR_MUL)                                                                                     \
				acc = acc * v;                                                                                         \
			else if (op == PR_MIN) {                                                                                   \
				if (v < acc)                                                                                           \
					acc = v;                                                                                           \
			} else if (v > acc)                                                                                        \
				acc = v;                                                                                               \
		}                                                                                                              \
		return acc;                                                                                                    \
	}
#else
static int pr_nthreads(long long n) {
	long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
	if (ncpu < 1)
		ncpu = 1;
	if (ncpu > PR_MAX_THREADS)
		ncpu = PR_MAX_THREADS;
	long long by_work = n / PR_MIN_PER_THREAD;
	if (by_work < 1)
		by_work = 1;
	int k = (by_work < ncpu) ? (int)by_work : (int)ncpu;
	return k < 1 ? 1 : k;
}

#define PR_GEN(NAME, T, ID_MIN, ID_MAX)                                                                                \
	typedef struct {                                                                                                   \
		const T *col;                                                                                                  \
		long long lo, hi;                                                                                              \
		int op;                                                                                                        \
		T out;                                                                                                         \
	} NAME##_arg;                                                                                                      \
	static T NAME##_fold(const T *col, long long lo, long long hi, int op) {                                           \
		T acc = (op == PR_ADD) ? (T)0 : (op == PR_MUL) ? (T)1 : (op == PR_MIN) ? (T)(ID_MIN) : (T)(ID_MAX);            \
		for (long long i = lo; i < hi; i++) {                                                                          \
			T v = col[i];                                                                                              \
			if (op == PR_ADD)                                                                                          \
				acc = acc + v;                                                                                         \
			else if (op == PR_MUL)                                                                                     \
				acc = acc * v;                                                                                         \
			else if (op == PR_MIN) {                                                                                   \
				if (v < acc)                                                                                           \
					acc = v;                                                                                           \
			} else if (v > acc)                                                                                        \
				acc = v;                                                                                               \
		}                                                                                                              \
		return acc;                                                                                                    \
	}                                                                                                                  \
	static void *NAME##_worker(void *p) {                                                                              \
		NAME##_arg *a = (NAME##_arg *)p;                                                                               \
		a->out = NAME##_fold(a->col, a->lo, a->hi, a->op);                                                             \
		return 0;                                                                                                      \
	}                                                                                                                  \
	T arche_par_reduce_##NAME(const T *col, long long n, int op) {                                                     \
		T id = (op == PR_ADD) ? (T)0 : (op == PR_MUL) ? (T)1 : (op == PR_MIN) ? (T)(ID_MIN) : (T)(ID_MAX);             \
		if (n <= 0)                                                                                                    \
			return id;                                                                                                 \
		int k = pr_nthreads(n);                                                                                        \
		if (k <= 1)                                                                                                    \
			return NAME##_fold(col, 0, n, op);                                                                         \
		pthread_t th[PR_MAX_THREADS];                                                                                  \
		NAME##_arg args[PR_MAX_THREADS];                                                                               \
		long long chunk = n / k;                                                                                       \
		int spawned = 0;                                                                                               \
		for (int t = 0; t < k; t++) {                                                                                  \
			args[t].col = col;                                                                                         \
			args[t].op = op;                                                                                           \
			args[t].lo = (long long)t * chunk;                                                                         \
			args[t].hi = (t == k - 1) ? n : (long long)(t + 1) * chunk;                                                \
			if (pthread_create(&th[t], 0, NAME##_worker, &args[t]) == 0)                                               \
				spawned++;                                                                                             \
			else                                                                                                       \
				args[t].out = NAME##_fold(col, args[t].lo, args[t].hi, op);                                            \
		}                                                                                                              \
		for (int t = 0; t < spawned; t++)                                                                              \
			pthread_join(th[t], 0);                                                                                    \
		T acc = args[0].out;                                                                                           \
		for (int t = 1; t < k; t++) {                                                                                  \
			T v = args[t].out;                                                                                         \
			if (op == PR_ADD)                                                                                          \
				acc = acc + v;                                                                                         \
			else if (op == PR_MUL)                                                                                     \
				acc = acc * v;                                                                                         \
			else if (op == PR_MIN) {                                                                                   \
				if (v < acc)                                                                                           \
					acc = v;                                                                                           \
			} else if (v > acc)                                                                                        \
				acc = v;                                                                                               \
		}                                                                                                              \
		return acc;                                                                                                    \
	}
#endif /* __wasm__ */

PR_GEN(f32, float, FLT_MAX, -FLT_MAX)
PR_GEN(i32, int32_t, INT32_MAX, INT32_MIN)
PR_GEN(i64, int64_t, INT64_MAX, INT64_MIN)
