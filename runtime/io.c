#define _GNU_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

double os_now_sec(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
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
	char *data = mmap(0, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (data == MAP_FAILED) {
		g_arche_map_size = 0;
		return 0;
	}
	madvise(data, st.st_size, MADV_SEQUENTIAL);
	g_arche_map_size = (long)st.st_size;
	return data;
}

long arche_file_size(void) {
	return g_arche_map_size;
}

void arche_file_unmap(char *data, long size) {
	if (data)
		munmap(data, (size_t)size);
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
