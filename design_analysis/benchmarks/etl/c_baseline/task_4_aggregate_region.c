/* Task 4 (C baseline): sum(price * quantity).
 * Same compute as Task 1; named "aggregate_region" for symmetry with arche/pandas/etc.
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

	double revenue_sum = 0.0;
	while (p < end) {
		char *nl = (char *)memchr(p, '\n', (size_t)(end - p));
		if (!nl) break;
		char *c1 = (char *)memchr(p, ',', (size_t)(nl - p));
		if (!c1) {
			p = nl + 1;
			continue;
		}
		char *price_start = c1 + 1;
		char *c2 = (char *)memchr(price_start, ',', (size_t)(nl - price_start));
		if (!c2) {
			p = nl + 1;
			continue;
		}

		double price = strtod(price_start, NULL);
		long quantity = strtol(c2 + 1, NULL, 10);
		revenue_sum += price * (double)quantity;

		p = nl + 1;
	}

	double elapsed = now_sec() - t0;

	munmap(base, size);
	close(fd);

	printf("task4_checksum: %.6f\n", revenue_sum);
	printf("task4_time: %.6f\n", elapsed);
	return 0;
}
