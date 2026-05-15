/* Task 2 (C baseline): count(quantity > 0).
 * Single-pass mmap, parse only the quantity column.
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_CSV "design_analysis/benchmarks/etl/data/data_100m.csv"

static double now_sec(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

int main(int argc, char **argv) {
	const char *path = argc > 1 ? argv[1] : DEFAULT_CSV;

	int fd = open(path, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "open(%s): %s\n", path, strerror(errno));
		return 1;
	}
	struct stat sb;
	if (fstat(fd, &sb) < 0) {
		close(fd);
		return 1;
	}
	size_t size = (size_t)sb.st_size;
	char *base = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
	if (base == MAP_FAILED) {
		close(fd);
		return 1;
	}

	double t0 = now_sec();

	char *p = (char *)memchr(base, '\n', size);
	if (!p) {
		munmap(base, size);
		close(fd);
		return 1;
	}
	p++;
	char *end = base + size;

	long long count = 0;
	while (p < end) {
		char *nl = (char *)memchr(p, '\n', (size_t)(end - p));
		if (!nl) break;
		char *c1 = (char *)memchr(p, ',', (size_t)(nl - p));
		if (!c1) {
			p = nl + 1;
			continue;
		}
		char *c2 = (char *)memchr(c1 + 1, ',', (size_t)(nl - c1 - 1));
		if (!c2) {
			p = nl + 1;
			continue;
		}
		long quantity = strtol(c2 + 1, NULL, 10);
		if (quantity > 0) count++;

		p = nl + 1;
	}

	double elapsed = now_sec() - t0;

	munmap(base, size);
	close(fd);

	printf("task2_checksum: %lld\n", count);
	printf("task2_time: %.6f\n", elapsed);
	return 0;
}
