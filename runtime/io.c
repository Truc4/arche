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

#define ARCHE_MAX_FILES 64
static FILE *arche_files[ARCHE_MAX_FILES];

static void __attribute__((constructor)) init_stdio(void) {
	arche_files[0] = stdin;
	arche_files[1] = stdout;
	arche_files[2] = stderr;
}

int arche_fopen_write(const char *path) {
	for (int i = 0; i < ARCHE_MAX_FILES; i++) {
		if (!arche_files[i]) {
			arche_files[i] = fopen(path, "w");
			return arche_files[i] ? i + 1 : 0;
		}
	}
	return 0;
}

void arche_fwrite(int fd, const char *buf, int n) {
	if (fd > 0 && fd <= ARCHE_MAX_FILES && arche_files[fd - 1])
		fwrite(buf, 1, n, arche_files[fd - 1]);
}

void arche_fclose(int fd) {
	if (fd > 0 && fd <= ARCHE_MAX_FILES && arche_files[fd - 1]) {
		fclose(arche_files[fd - 1]);
		arche_files[fd - 1] = NULL;
	}
}

int arche_fopen_read(const char *path) {
	for (int i = 0; i < ARCHE_MAX_FILES; i++) {
		if (!arche_files[i]) {
			arche_files[i] = fopen(path, "r");
			return arche_files[i] ? i + 1 : 0;
		}
	}
	return 0;
}

int arche_fread(int fd, char *buf, int n) {
	if (fd > 0 && fd <= ARCHE_MAX_FILES && arche_files[fd - 1])
		return fread(buf, 1, n, arche_files[fd - 1]);
	return 0;
}

int arche_fread_line(int fd, char *buf, int n) {
	if (fd <= 0 || fd > ARCHE_MAX_FILES || !arche_files[fd - 1])
		return -1;
	if (fgets(buf, n, arche_files[fd - 1]) == NULL)
		return 0;
	int len = strlen(buf);
	if (len > 0 && buf[len - 1] == '\n') {
		buf[len - 1] = '\0';
		len--;
	}
	return len;
}

int arche_csv_read_chunk(int fd, char *buf, int max_bytes) {
	if (fd <= 0 || fd > ARCHE_MAX_FILES || !arche_files[fd - 1])
		return -1;
	return (int)fread(buf, 1, max_bytes, arche_files[fd - 1]);
}

/* =========================
   Memory-mapped file access
   ========================= */

#define ARCHE_MMAP_MAX 16
typedef struct {
	const char *data;
	long size;
} MmapEntry;
static MmapEntry arche_mmaps[ARCHE_MMAP_MAX];

int arche_mmap_open(const char *path) {
	int fd = open(path, O_RDONLY);
	if (fd < 0)
		return 0;
	struct stat st;
	if (fstat(fd, &st) < 0) {
		close(fd);
		return 0;
	}
	const char *data = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (data == MAP_FAILED)
		return 0;
	madvise((void *)data, st.st_size, MADV_SEQUENTIAL);
	for (int i = 0; i < ARCHE_MMAP_MAX; i++) {
		if (!arche_mmaps[i].data) {
			arche_mmaps[i].data = data;
			arche_mmaps[i].size = st.st_size;
			return i + 1;
		}
	}
	munmap((void *)data, st.st_size);
	return 0;
}

/* Positions are treated as uint32_t to support files up to 4GB.
 * Arche's int wraps identically to uint32, so signed-int Arche code
 * interoperates correctly: e.g. a 3.4 GB file returns (int)3485384676
 * = -809582620 as signed, but C reinterprets it as unsigned 3485384676. */
int arche_mmap_size(int handle) {
	if (handle < 1 || handle > ARCHE_MMAP_MAX || !arche_mmaps[handle - 1].data)
		return 0;
	return (int)(uint32_t)arche_mmaps[handle - 1].size;
}

void arche_mmap_close(int handle) {
	if (handle < 1 || handle > ARCHE_MMAP_MAX || !arche_mmaps[handle - 1].data)
		return;
	munmap((void *)arche_mmaps[handle - 1].data, arche_mmaps[handle - 1].size);
	arche_mmaps[handle - 1].data = NULL;
	arche_mmaps[handle - 1].size = 0;
}

/* Returns first position of ch in [start, end), or end if not found.
 * start/end are treated as uint32_t to support files up to 4GB. */
int arche_mmap_find(int handle, int start_i, int end_i, int ch) {
	if (handle < 1 || handle > ARCHE_MMAP_MAX || !arche_mmaps[handle - 1].data)
		return end_i;
	uint32_t start = (uint32_t)start_i;
	uint32_t end = (uint32_t)end_i;
	uint64_t size = (uint64_t)arche_mmaps[handle - 1].size;
	if (start >= size || end > size || start >= end)
		return end_i;
	const char *base = arche_mmaps[handle - 1].data;
	const char *found = memchr(base + start, (unsigned char)ch, end - start);
	return found ? (int)(uint32_t)(found - base) : end_i;
}

/* Parse float at pos directly from mapped memory — no temp buffer. */
double arche_mmap_parse_float(int handle, int pos_i) {
	if (handle < 1 || handle > ARCHE_MMAP_MAX || !arche_mmaps[handle - 1].data)
		return 0.0;
	uint32_t pos = (uint32_t)pos_i;
	if ((uint64_t)pos >= (uint64_t)arche_mmaps[handle - 1].size)
		return 0.0;
	char *end;
	return strtod(arche_mmaps[handle - 1].data + pos, &end);
}

/* Parse int at pos directly from mapped memory — no temp buffer. */
int arche_mmap_parse_int(int handle, int pos_i) {
	if (handle < 1 || handle > ARCHE_MMAP_MAX || !arche_mmaps[handle - 1].data)
		return 0;
	uint32_t pos = (uint32_t)pos_i;
	if ((uint64_t)pos >= (uint64_t)arche_mmaps[handle - 1].size)
		return 0;
	char *end;
	return (int)strtol(arche_mmaps[handle - 1].data + pos, &end, 10);
}

/* Walk the header line (up to first '\n'), find the column whose name matches
 * `name`, and return its zero-based index. -1 if not found. */
int arche_csv_column_index(int handle, const char *name) {
	if (handle < 1 || handle > ARCHE_MMAP_MAX || !arche_mmaps[handle - 1].data || !name)
		return -1;
	const char *base = arche_mmaps[handle - 1].data;
	uint64_t size = (uint64_t)arche_mmaps[handle - 1].size;
	size_t namelen = strlen(name);
	uint32_t pos = 0;
	int col = 0;
	while (pos < size && base[pos] != '\n') {
		const char *comma = memchr(base + pos, ',', size - pos);
		const char *newline = memchr(base + pos, '\n', size - pos);
		const char *end = comma;
		if (!end || (newline && newline < end))
			end = newline;
		size_t field_len = end ? (size_t)(end - (base + pos)) : (size - pos);
		if (field_len == namelen && memcmp(base + pos, name, namelen) == 0) {
			return col;
		}
		if (!comma || (newline && newline < comma))
			break;
		pos = (uint32_t)((comma - base) + 1);
		col++;
	}
	return -1;
}

/* Given a row starting at `row_start`, return the byte offset of the
 * `col_idx`'th comma-separated field within that row. */
int arche_csv_field_pos(int handle, int row_start_i, int col_idx) {
	if (handle < 1 || handle > ARCHE_MMAP_MAX || !arche_mmaps[handle - 1].data)
		return 0;
	const char *base = arche_mmaps[handle - 1].data;
	uint64_t size = (uint64_t)arche_mmaps[handle - 1].size;
	uint32_t pos = (uint32_t)row_start_i;
	int remaining = col_idx;
	while (remaining > 0 && pos < size && base[pos] != '\n') {
		const char *comma = memchr(base + pos, ',', size - pos);
		const char *newline = memchr(base + pos, '\n', size - pos);
		if (!comma || (newline && newline < comma)) {
			/* Row ended before reaching col_idx; return position-just-past
			 * so the parser sees an empty field and returns 0. */
			return newline ? (int)(uint32_t)(newline - base) : (int)(uint32_t)size;
		}
		pos = (uint32_t)((comma - base) + 1);
		remaining--;
	}
	return (int)pos;
}
