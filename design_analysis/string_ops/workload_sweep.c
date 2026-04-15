#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define INITIAL_STRINGS 1000
#define MAX_STRING_LEN 100
#define ITERATIONS 50

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

// === Method 1: Array of pointers ===
typedef struct {
	char **strings;
	int count;
	int capacity;
} Method1;

Method1 method1_create(int initial_count) {
	Method1 m;
	m.capacity = initial_count * 2;
	m.strings = malloc(m.capacity * sizeof(char *));
	m.count = initial_count;
	for (int i = 0; i < initial_count; i++) {
		m.strings[i] = malloc(MAX_STRING_LEN);
		snprintf(m.strings[i], MAX_STRING_LEN, "str_%d", i);
	}
	return m;
}

void method1_add(Method1 *m, int id) {
	if (m->count >= m->capacity) {
		m->capacity *= 2;
		m->strings = realloc(m->strings, m->capacity * sizeof(char *));
	}
	m->strings[m->count] = malloc(MAX_STRING_LEN);
	snprintf(m->strings[m->count], MAX_STRING_LEN, "str_%d", id);
	m->count++;
}

void method1_remove(Method1 *m, int idx) {
	if (idx < 0 || idx >= m->count || m->count == 0)
		return;
	free(m->strings[idx]);
	for (int i = idx; i < m->count - 1; i++) {
		m->strings[i] = m->strings[i + 1];
	}
	m->count--;
}

void method1_uppercase(Method1 m) {
	for (int i = 0; i < m.count; i++) {
		for (char *p = m.strings[i]; *p; p++) {
			*p = toupper((unsigned char)*p);
		}
	}
}

void method1_modify(Method1 m, int *seed) {
	for (int i = 0; i < m.count; i++) {
		int op = (*seed = (*seed * 1103515245 + 12345) & 0x7fffffff) % 2;
		if (op == 0 && strlen(m.strings[i]) > 5) {
			int write = 0;
			for (int read = 0; m.strings[i][read]; read += 2) {
				m.strings[i][write++] = m.strings[i][read];
			}
			m.strings[i][write] = '\0';
		} else if (op == 1 && strlen(m.strings[i]) < MAX_STRING_LEN - 5) {
			strcat(m.strings[i], "_m");
		}
	}
}

void method1_free(Method1 m) {
	for (int i = 0; i < m.count; i++)
		free(m.strings[i]);
	free(m.strings);
}

// === Method 2: Contiguous (computed offsets) ===
typedef struct {
	char *buffer;
	int *lengths;
	int count;
	int buffer_size;
	int buffer_capacity;
} Method2;

Method2 method2_create(int initial_count) {
	Method2 m = {0};
	m.count = initial_count;
	m.buffer_capacity = initial_count * MAX_STRING_LEN;
	m.buffer = malloc(m.buffer_capacity);
	m.lengths = malloc(initial_count * sizeof(int));

	int pos = 0;
	for (int i = 0; i < initial_count; i++) {
		char temp[MAX_STRING_LEN];
		snprintf(temp, MAX_STRING_LEN, "str_%d", i);
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
	snprintf(temp, MAX_STRING_LEN, "str_%d", id);
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
	for (int i = 0; i < idx; i++) {
		offset += m->lengths[i] + 1;
	}
	int old_len = m->lengths[idx];

	int shift_size = m->buffer_size - (offset + old_len + 1);
	if (shift_size > 0) {
		memmove(m->buffer + offset, m->buffer + offset + old_len + 1, shift_size);
	}
	m->buffer_size -= (old_len + 1);

	for (int i = idx; i < m->count - 1; i++) {
		m->lengths[i] = m->lengths[i + 1];
	}
	m->count--;
}

void method2_uppercase(Method2 m) {
	int offset = 0;
	for (int i = 0; i < m.count; i++) {
		for (int j = 0; j < m.lengths[i]; j++) {
			m.buffer[offset + j] = toupper((unsigned char)m.buffer[offset + j]);
		}
		offset += m.lengths[i] + 1;
	}
}

void method2_free(Method2 m) {
	free(m.buffer);
	free(m.lengths);
}

// === Method 4: Matrix ===
typedef struct {
	char (*matrix)[MAX_STRING_LEN];
	int *lengths;
	int count;
	int capacity;
} Method4;

Method4 method4_create(int initial_count) {
	Method4 m;
	m.capacity = initial_count * 2;
	m.matrix = malloc(m.capacity * MAX_STRING_LEN);
	m.lengths = malloc(m.capacity * sizeof(int));
	m.count = initial_count;

	for (int i = 0; i < initial_count; i++) {
		char temp[MAX_STRING_LEN];
		snprintf(temp, MAX_STRING_LEN, "str_%d", i);
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
	snprintf(temp, MAX_STRING_LEN, "str_%d", id);
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

void method4_uppercase(Method4 m) {
	for (int i = 0; i < m.count; i++) {
		for (int j = 0; j < m.lengths[i]; j++) {
			m.matrix[i][j] = toupper((unsigned char)m.matrix[i][j]);
		}
	}
}

void method4_free(Method4 m) {
	free(m.matrix);
	free(m.lengths);
}

// === Workload executor ===
void run_workload(int lifecycle_pct, int fixed_pct, int *seed, long long *m1_time, long long *m2_time,
                  long long *m4_time) {
	int grow_pct = 100 - lifecycle_pct - fixed_pct;

	// Method 1
	{
		Method1 m = method1_create(INITIAL_STRINGS);
		Timer t = timer_start();

		int ops = 100; // operations per iteration
		for (int iter = 0; iter < ITERATIONS; iter++) {
			int lifecycle_ops = (ops * lifecycle_pct) / 100;
			int fixed_ops = (ops * fixed_pct) / 100;
			int grow_ops = ops - lifecycle_ops - fixed_ops;

			for (int i = 0; i < lifecycle_ops / 2; i++) {
				method1_remove(&m, random_int(seed, m.count));
				method1_add(&m, 10000 + i);
			}
			if (fixed_ops > 0)
				method1_uppercase(m);
			if (grow_ops > 0)
				method1_modify(m, seed);
		}

		*m1_time = timer_end(t);
		method1_free(m);
	}

	// Method 2
	{
		Method2 m = method2_create(INITIAL_STRINGS);
		Timer t = timer_start();

		int ops = 100;
		for (int iter = 0; iter < ITERATIONS; iter++) {
			int lifecycle_ops = (ops * lifecycle_pct) / 100;
			int fixed_ops = (ops * fixed_pct) / 100;
			int grow_ops = ops - lifecycle_ops - fixed_ops;

			for (int i = 0; i < lifecycle_ops / 2; i++) {
				method2_remove(&m, random_int(seed, m.count));
				method2_add(&m, 10000 + i);
			}
			if (fixed_ops > 0)
				method2_uppercase(m);
		}

		*m2_time = timer_end(t);
		method2_free(m);
	}

	// Method 4
	{
		Method4 m = method4_create(INITIAL_STRINGS);
		Timer t = timer_start();

		int ops = 100;
		for (int iter = 0; iter < ITERATIONS; iter++) {
			int lifecycle_ops = (ops * lifecycle_pct) / 100;
			int fixed_ops = (ops * fixed_pct) / 100;
			int grow_ops = ops - lifecycle_ops - fixed_ops;

			for (int i = 0; i < lifecycle_ops / 2; i++) {
				method4_remove(&m, random_int(seed, m.count));
				method4_add(&m, 10000 + i);
			}
			if (fixed_ops > 0)
				method4_uppercase(m);
		}

		*m4_time = timer_end(t);
		method4_free(m);
	}
}

int main() {
	FILE *csv = fopen("results/workload_sweep.csv", "w");
	fprintf(csv, "lifecycle_pct,fixed_pct,grow_pct,method1_ns,method2_ns,method4_ns,winner\n");

	int seed = 42;

	printf("Sweeping workload space...\n");

	for (int lifecycle = 0; lifecycle <= 100; lifecycle += 10) {
		for (int fixed = 0; fixed <= 100 - lifecycle; fixed += 10) {
			int grow = 100 - lifecycle - fixed;

			long long m1, m2, m4;
			run_workload(lifecycle, fixed, &seed, &m1, &m2, &m4);

			long long min = m1;
			int winner = 1;
			if (m2 < min) {
				min = m2;
				winner = 2;
			}
			if (m4 < min) {
				min = m4;
				winner = 4;
			}

			printf("Lifecycle: %3d%% | Fixed: %3d%% | Grow: %3d%% | M1: %8lld | M2: %8lld | M4: %8lld | Winner: M%d\n",
			       lifecycle, fixed, grow, m1 / ITERATIONS, m2 / ITERATIONS, m4 / ITERATIONS, winner);

			fprintf(csv, "%d,%d,%d,%lld,%lld,%lld,%d\n", lifecycle, fixed, grow, m1 / ITERATIONS, m2 / ITERATIONS,
			        m4 / ITERATIONS, winner);
		}
	}

	fclose(csv);
	printf("\nResults saved to results/workload_sweep.csv\n");

	return 0;
}
