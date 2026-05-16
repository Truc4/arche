/* Task 1 (C, transform-only): revenue = price * quantity, repeated N=216,000 times.
 * Load CSV into in-memory arrays once; the timed loop is pure column compute.
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

#define DEFAULT_CSV "design_analysis/benchmarks/etl/data/data_10k.csv"
#define ROWS 10000
#define N_ITERS 216000
#define OUT_PATH "design_analysis/benchmarks/transform/out/c/task_1.csv"

static double now_sec(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static double price[ROWS];
static int quantity[ROWS];
static double revenue[ROWS];

static int load_csv(const char *path) {
	int fd = open(path, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "open(%s): %s\n", path, strerror(errno));
		return -1;
	}
	struct stat sb;
	if (fstat(fd, &sb) < 0) {
		close(fd);
		return -1;
	}
	size_t size = (size_t)sb.st_size;
	char *base = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
	if (base == MAP_FAILED) {
		close(fd);
		return -1;
	}

	char *p = (char *)memchr(base, '\n', size);
	if (!p) {
		munmap(base, size);
		close(fd);
		return -1;
	}
	p++;
	char *end = base + size;

	int idx = 0;
	while (p < end && idx < ROWS) {
		char *nl = (char *)memchr(p, '\n', (size_t)(end - p));
		if (!nl)
			break;
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
		price[idx] = strtod(c1 + 1, NULL);
		quantity[idx] = (int)strtol(c2 + 1, NULL, 10);
		idx++;
		p = nl + 1;
	}
	munmap(base, size);
	close(fd);
	return idx;
}

static void mkdir_p(const char *path) {
	char tmp[512];
	snprintf(tmp, sizeof(tmp), "%s", path);
	for (char *p = tmp + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			mkdir(tmp, 0755);
			*p = '/';
		}
	}
}

int main(int argc, char **argv) {
	const char *path = argc > 1 ? argv[1] : DEFAULT_CSV;
	int rows = load_csv(path);
	if (rows < 0)
		return 1;

	double t0 = now_sec();
	for (int k = 0; k < N_ITERS; k++) {
		for (int i = 0; i < rows; i++) {
			revenue[i] = price[i] * (double)quantity[i];
		}
		/* Memory clobber: forces the compiler to treat revenue[] as
		 * observed between outer iterations, preventing -O3 from collapsing
		 * the N=216,000 outer loop into one iter. */
		__asm__ __volatile__("" : : "r"(revenue) : "memory");
	}
	double elapsed = now_sec() - t0;

	double sum = 0.0;
	for (int i = 0; i < rows; i++)
		sum += revenue[i];

	mkdir_p(OUT_PATH);
	FILE *out = fopen(OUT_PATH, "w");
	if (out) {
		fprintf(out, "revenue\n");
		for (int i = 0; i < rows; i++)
			fprintf(out, "%.6f\n", revenue[i]);
		fclose(out);
	}

	printf("task1_checksum: %.6f\n", sum);
	printf("task1_time: %.10f\n", elapsed / (double)N_ITERS);
	return 0;
}
