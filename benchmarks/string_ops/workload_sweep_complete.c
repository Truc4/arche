#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define INITIAL_STRINGS 500
#define MAX_STRING_LEN 100
#define ITERATIONS 20

typedef struct {
	long long ns;
} Timer;

Timer timer_start() {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (Timer){ts.tv_sec * 1000000000LL + ts.tv_nsec};
}

long long timer_end(Timer start) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	long long end = ts.tv_sec * 1000000000LL + ts.tv_nsec;
	return end - start.ns;
}

int random_int(int *seed, int max) {
	if (max <= 0)
		return 0;
	*seed = (*seed * 1103515245 + 12345) & 0x7fffffff;
	return *seed % max;
}

// === Method 2: Contiguous (computed offsets) ===
typedef struct {
	char *buffer;
	int *lengths;
	int count, buffer_size, buffer_capacity;
} Method2;

Method2 method2_create(int n) {
	Method2 m = {0};
	m.count = n;
	m.buffer_capacity = n * MAX_STRING_LEN;
	m.buffer = malloc(m.buffer_capacity);
	m.lengths = malloc(n * sizeof(int));
	int pos = 0;
	for (int i = 0; i < n; i++) {
		char temp[MAX_STRING_LEN];
		snprintf(temp, MAX_STRING_LEN, "s_%d", i);
		int len = strlen(temp);
		m.lengths[i] = len;
		memcpy(m.buffer + pos, temp, len + 1);
		pos += len + 1;
		m.buffer_size = pos;
	}
	return m;
}

void method2_add(Method2 *m, int id) {
	if (m->count >= 2000)
		return;
	char temp[MAX_STRING_LEN];
	snprintf(temp, MAX_STRING_LEN, "s_%d", id);
	int len = strlen(temp);
	if (m->buffer_size + len + 1 >= m->buffer_capacity) {
		m->buffer_capacity *= 2;
		m->buffer = realloc(m->buffer, m->buffer_capacity);
	}
	m->lengths = realloc(m->lengths, (m->count + 1) * sizeof(int));
	m->lengths[m->count] = len;
	memcpy(m->buffer + m->buffer_size, temp, len + 1);
	m->buffer_size += len + 1;
	m->count++;
}

void method2_remove(Method2 *m, int idx) {
	if (idx < 0 || idx >= m->count || m->count == 0)
		return;
	int offset = 0;
	for (int i = 0; i < idx; i++)
		offset += m->lengths[i] + 1;
	int old_len = m->lengths[idx];
	int shift_size = m->buffer_size - (offset + old_len + 1);
	if (shift_size > 0)
		memmove(m->buffer + offset, m->buffer + offset + old_len + 1, shift_size);
	m->buffer_size -= (old_len + 1);
	for (int i = idx; i < m->count - 1; i++)
		m->lengths[i] = m->lengths[i + 1];
	m->count--;
}

void method2_free(Method2 m) {
	free(m.buffer);
	free(m.lengths);
}

// === Method 3: Contiguous (cached offsets) ===
typedef struct {
	char *buffer;
	int *offsets;
	int *lengths;
	int count, buffer_size, buffer_capacity;
} Method3;

Method3 method3_create(int n) {
	Method3 m = {0};
	m.count = n;
	m.buffer_capacity = n * MAX_STRING_LEN;
	m.buffer = malloc(m.buffer_capacity);
	m.offsets = malloc(n * sizeof(int));
	m.lengths = malloc(n * sizeof(int));
	int pos = 0;
	for (int i = 0; i < n; i++) {
		char temp[MAX_STRING_LEN];
		snprintf(temp, MAX_STRING_LEN, "s_%d", i);
		int len = strlen(temp);
		m.offsets[i] = pos;
		m.lengths[i] = len;
		memcpy(m.buffer + pos, temp, len + 1);
		pos += len + 1;
		m.buffer_size = pos;
	}
	return m;
}

void method3_add(Method3 *m, int id) {
	if (m->count >= 2000)
		return;
	char temp[MAX_STRING_LEN];
	snprintf(temp, MAX_STRING_LEN, "s_%d", id);
	int len = strlen(temp);
	if (m->buffer_size + len + 1 >= m->buffer_capacity) {
		m->buffer_capacity *= 2;
		m->buffer = realloc(m->buffer, m->buffer_capacity);
	}
	m->offsets = realloc(m->offsets, (m->count + 1) * sizeof(int));
	m->lengths = realloc(m->lengths, (m->count + 1) * sizeof(int));
	m->offsets[m->count] = m->buffer_size;
	m->lengths[m->count] = len;
	memcpy(m->buffer + m->buffer_size, temp, len + 1);
	m->buffer_size += len + 1;
	m->count++;
}

void method3_remove(Method3 *m, int idx) {
	if (idx < 0 || idx >= m->count || m->count == 0)
		return;
	int offset = m->offsets[idx];
	int old_len = m->lengths[idx];
	int shift_size = m->buffer_size - (offset + old_len + 1);
	if (shift_size > 0)
		memmove(m->buffer + offset, m->buffer + offset + old_len + 1, shift_size);
	m->buffer_size -= (old_len + 1);
	for (int i = idx; i < m->count - 1; i++) {
		m->offsets[i] = m->offsets[i + 1] - (old_len + 1);
		m->lengths[i] = m->lengths[i + 1];
	}
	m->count--;
}

void method3_free(Method3 m) {
	free(m.buffer);
	free(m.offsets);
	free(m.lengths);
}

// === Method 4: Matrix ===
typedef struct {
	char (*matrix)[MAX_STRING_LEN];
	int *lengths;
	int count, capacity;
} Method4;

Method4 method4_create(int n) {
	Method4 m;
	m.capacity = n * 2;
	m.matrix = malloc(m.capacity * MAX_STRING_LEN);
	m.lengths = malloc(m.capacity * sizeof(int));
	m.count = n;
	for (int i = 0; i < n; i++) {
		char temp[MAX_STRING_LEN];
		snprintf(temp, MAX_STRING_LEN, "s_%d", i);
		int len = strlen(temp);
		m.lengths[i] = len;
		memcpy(m.matrix[i], temp, len + 1);
	}
	return m;
}

void method4_add(Method4 *m, int id) {
	if (m->count >= m->capacity) {
		m->capacity *= 2;
		m->matrix = realloc(m->matrix, m->capacity * MAX_STRING_LEN);
		m->lengths = realloc(m->lengths, m->capacity * sizeof(int));
	}
	char temp[MAX_STRING_LEN];
	snprintf(temp, MAX_STRING_LEN, "s_%d", id);
	int len = strlen(temp);
	m->lengths[m->count] = len;
	memcpy(m->matrix[m->count], temp, len + 1);
	m->count++;
}

void method4_remove(Method4 *m, int idx) {
	if (idx < 0 || idx >= m->count || m->count == 0)
		return;
	if (idx < m->count - 1) {
		m->lengths[idx] = m->lengths[m->count - 1];
		memcpy(m->matrix[idx], m->matrix[m->count - 1], MAX_STRING_LEN);
	}
	m->count--;
}

void method4_free(Method4 m) {
	free(m.matrix);
	free(m.lengths);
}

void run_workload(int lifecycle_pct, int fixed_pct, int *seed, long long *m2_time, long long *m3_time,
                  long long *m4_time) {
	int ops = 50;

	// Method 2
	{
		Method2 m = method2_create(INITIAL_STRINGS);
		Timer t = timer_start();
		for (int iter = 0; iter < ITERATIONS; iter++) {
			int lc = (ops * lifecycle_pct) / 100;
			for (int i = 0; i < lc / 2; i++) {
				method2_remove(&m, random_int(seed, m.count));
				method2_add(&m, 10000 + i);
			}
		}
		*m2_time = timer_end(t);
		method2_free(m);
	}

	// Method 3
	{
		Method3 m = method3_create(INITIAL_STRINGS);
		Timer t = timer_start();
		for (int iter = 0; iter < ITERATIONS; iter++) {
			int lc = (ops * lifecycle_pct) / 100;
			for (int i = 0; i < lc / 2; i++) {
				method3_remove(&m, random_int(seed, m.count));
				method3_add(&m, 10000 + i);
			}
		}
		*m3_time = timer_end(t);
		method3_free(m);
	}

	// Method 4
	{
		Method4 m = method4_create(INITIAL_STRINGS);
		Timer t = timer_start();
		for (int iter = 0; iter < ITERATIONS; iter++) {
			int lc = (ops * lifecycle_pct) / 100;
			for (int i = 0; i < lc / 2; i++) {
				method4_remove(&m, random_int(seed, m.count));
				method4_add(&m, 10000 + i);
			}
		}
		*m4_time = timer_end(t);
		method4_free(m);
	}
}

int main() {
	FILE *csv = fopen("results/workload_sweep_complete.csv", "w");
	fprintf(csv, "lifecycle_pct,method2_ns,method3_ns,method4_ns,winner\n");

	int seed = 42;
	printf("Sweeping with lifecycle operations (0-100%%)...\n");

	for (int lifecycle = 0; lifecycle <= 100; lifecycle += 10) {
		long long m2, m3, m4;
		run_workload(lifecycle, 0, &seed, &m2, &m3, &m4);

		long long min = m2;
		int winner = 2;
		if (m3 < min) {
			min = m3;
			winner = 3;
		}
		if (m4 < min) {
			min = m4;
			winner = 4;
		}

		printf("Lifecycle: %3d%% | M2: %8lld | M3: %8lld | M4: %8lld | Winner: M%d\n", lifecycle, m2 / ITERATIONS,
		       m3 / ITERATIONS, m4 / ITERATIONS, winner);

		fprintf(csv, "%d,%lld,%lld,%lld,%d\n", lifecycle, m2 / ITERATIONS, m3 / ITERATIONS, m4 / ITERATIONS, winner);
	}

	fclose(csv);
	printf("\nResults saved to results/workload_sweep_complete.csv\n");
	return 0;
}
