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

double arche_now_sec(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* File handles flow through the extern-type table (`extern File(N)`).
 * The C ABI takes/returns `FILE*` directly; the codegen-emitted marshal layer
 * allocates a slot, stores the pointer, and hands Arche an opaque handle.
 *
 * Each arche_stdin/stdout/stderr call allocates a new slot, so Arche programs
 * should bind it once at startup and reuse the handle. */

FILE *arche_stdin(void) {
	return stdin;
}
FILE *arche_stdout(void) {
	return stdout;
}
FILE *arche_stderr(void) {
	return stderr;
}

FILE *arche_fopen_write(const char *path) {
	return fopen(path, "w");
}

void arche_fwrite(FILE *f, const char *buf, int n) {
	if (f)
		fwrite(buf, 1, n, f);
}

void arche_fclose(FILE *f) {
	if (f)
		fclose(f);
}

FILE *arche_fopen_read(const char *path) {
	return fopen(path, "r");
}

int arche_fread(FILE *f, char *buf, int n) {
	if (!f)
		return 0;
	return (int)fread(buf, 1, n, f);
}

int arche_fread_line(FILE *f, char *buf, int n) {
	if (!f)
		return -1;
	if (fgets(buf, n, f) == NULL)
		return 0;
	int len = strlen(buf);
	if (len > 0 && buf[len - 1] == '\n') {
		buf[len - 1] = '\0';
		len--;
	}
	return len;
}

int arche_csv_read_chunk(FILE *f, char *buf, int max_bytes) {
	if (!f)
		return -1;
	return (int)fread(buf, 1, max_bytes, f);
}

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
