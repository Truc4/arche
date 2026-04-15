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
	if (idx < m->count - 1) {
		for (int i = idx; i < m->count - 1; i++) {
			m->strings[i] = m->strings[i + 1];
		}
	}
	m->count--;
}

void method1_modify(Method1 m, int *seed) {
	for (int i = 0; i < m.count; i++) {
		int op = (*seed = (*seed * 1103515245 + 12345) & 0x7fffffff) % 3;
		int len = strlen(m.strings[i]);

		if (op == 0 && len > 5) {
			// Shrink
			int write = 0;
			for (int read = 0; read < len; read += 2) {
				m.strings[i][write++] = m.strings[i][read];
			}
			m.strings[i][write] = '\0';
		} else if (op == 1 && len < MAX_STRING_LEN - 10) {
			// Grow
			strcat(m.strings[i], "_mod");
		}
		// else: stay same
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

void method2_modify(Method2 m, int *seed) {
	for (int i = 0; i < m.count; i++) {
		int op = (*seed = (*seed * 1103515245 + 12345) & 0x7fffffff) % 3;
		int offset = 0;
		for (int j = 0; j < i; j++) {
			offset += m.lengths[j] + 1;
		}
		int old_len = m.lengths[i];

		if (op == 0 && old_len > 5) {
			// Shrink: in-place
			int write = 0;
			for (int read = 0; read < old_len; read += 2) {
				m.buffer[offset + write++] = m.buffer[offset + read];
			}
			m.buffer[offset + write] = '\0';
			m.lengths[i] = write;
		} else if (op == 1 && old_len < MAX_STRING_LEN - 10) {
			// Grow: append
			if (old_len + 5 <= MAX_STRING_LEN) {
				strcat(m.buffer + offset, "_mod");
				m.lengths[i] = strlen(m.buffer + offset);
			}
		}
		// else: stay same
	}
}

void method2_free(Method2 m) {
	free(m.buffer);
	free(m.lengths);
}

// === Method 3: Contiguous (cached offsets) ===
typedef struct {
	int *offsets;
	int *lengths;
	char *buffer;
	int count;
	int buffer_size;
	int buffer_capacity;
} Method3;

Method3 method3_create(int initial_count) {
	Method3 m = {0};
	m.count = initial_count;
	m.offsets = malloc(initial_count * sizeof(int));
	m.lengths = malloc(initial_count * sizeof(int));
	m.buffer_capacity = initial_count * MAX_STRING_LEN;
	m.buffer = malloc(m.buffer_capacity);

	int pos = 0;
	for (int i = 0; i < initial_count; i++) {
		char temp[MAX_STRING_LEN];
		snprintf(temp, MAX_STRING_LEN, "str_%d", i);
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
	snprintf(temp, MAX_STRING_LEN, "str_%d", id);
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
	if (shift_size > 0) {
		memmove(m->buffer + offset, m->buffer + offset + old_len + 1, shift_size);
	}
	m->buffer_size -= (old_len + 1);

	for (int i = idx; i < m->count - 1; i++) {
		m->offsets[i] = m->offsets[i + 1];
		m->lengths[i] = m->lengths[i + 1];
		if (i + 1 < m->count) {
			m->offsets[i + 1] -= (old_len + 1);
		}
	}
	m->count--;
}

void method3_modify(Method3 m, int *seed) {
	for (int i = 0; i < m.count; i++) {
		int op = (*seed = (*seed * 1103515245 + 12345) & 0x7fffffff) % 3;
		int offset = m.offsets[i];
		int old_len = m.lengths[i];

		if (op == 0 && old_len > 5) {
			// Shrink: in-place
			int write = 0;
			for (int read = 0; read < old_len; read += 2) {
				m.buffer[offset + write++] = m.buffer[offset + read];
			}
			m.buffer[offset + write] = '\0';
			m.lengths[i] = write;
		} else if (op == 1 && old_len < MAX_STRING_LEN - 10) {
			// Grow: append
			if (old_len + 5 <= MAX_STRING_LEN) {
				strcat(m.buffer + offset, "_mod");
				m.lengths[i] = strlen(m.buffer + offset);
			}
		}
		// else: stay same
	}
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

void method4_modify(Method4 m, int *seed) {
	for (int i = 0; i < m.count; i++) {
		int op = (*seed = (*seed * 1103515245 + 12345) & 0x7fffffff) % 3;
		int old_len = m.lengths[i];

		if (op == 0 && old_len > 5) {
			// Shrink
			int write = 0;
			for (int read = 0; read < old_len; read += 2) {
				m.matrix[i][write++] = m.matrix[i][read];
			}
			m.matrix[i][write] = '\0';
			m.lengths[i] = write;
		} else if (op == 1 && old_len < MAX_STRING_LEN - 10) {
			// Grow
			strcat(m.matrix[i], "_mod");
			m.lengths[i] = strlen(m.matrix[i]);
		}
		// else: stay same
	}
}

void method4_free(Method4 m) {
	free(m.matrix);
	free(m.lengths);
}

// === Workload executor ===
void run_workload(int lifecycle_pct, int modify_pct, int *seed, long long *m1_time, long long *m2_time,
                  long long *m3_time, long long *m4_time) {
	// Method 1
	{
		Method1 m = method1_create(INITIAL_STRINGS);
		Timer t = timer_start();

		int ops = 100;
		for (int iter = 0; iter < ITERATIONS; iter++) {
			int lifecycle_ops = (ops * lifecycle_pct) / 100;
			int modify_ops = (ops * modify_pct) / 100;

			for (int i = 0; i < lifecycle_ops / 2; i++) {
				if (m.count > 0)
					method1_remove(&m, random_int(seed, m.count));
				method1_add(&m, 10000 + i);
			}
			if (modify_ops > 0)
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
			int modify_ops = (ops * modify_pct) / 100;

			for (int i = 0; i < lifecycle_ops / 2; i++) {
				if (m.count > 0)
					method2_remove(&m, random_int(seed, m.count));
				method2_add(&m, 10000 + i);
			}
			if (modify_ops > 0)
				method2_modify(m, seed);
		}

		*m2_time = timer_end(t);
		method2_free(m);
	}

	// Method 3
	{
		Method3 m = method3_create(INITIAL_STRINGS);
		Timer t = timer_start();

		int ops = 100;
		for (int iter = 0; iter < ITERATIONS; iter++) {
			int lifecycle_ops = (ops * lifecycle_pct) / 100;
			int modify_ops = (ops * modify_pct) / 100;

			for (int i = 0; i < lifecycle_ops / 2; i++) {
				if (m.count > 0)
					method3_remove(&m, random_int(seed, m.count));
				method3_add(&m, 10000 + i);
			}
			if (modify_ops > 0)
				method3_modify(m, seed);
		}

		*m3_time = timer_end(t);
		method3_free(m);
	}

	// Method 4
	{
		Method4 m = method4_create(INITIAL_STRINGS);
		Timer t = timer_start();

		int ops = 100;
		for (int iter = 0; iter < ITERATIONS; iter++) {
			int lifecycle_ops = (ops * lifecycle_pct) / 100;
			int modify_ops = (ops * modify_pct) / 100;

			for (int i = 0; i < lifecycle_ops / 2; i++) {
				if (m.count > 0)
					method4_remove(&m, random_int(seed, m.count));
				method4_add(&m, 10000 + i);
			}
			if (modify_ops > 0)
				method4_modify(m, seed);
		}

		*m4_time = timer_end(t);
		method4_free(m);
	}
}

int main() {
	FILE *csv = fopen("results/exhaustive_all_methods.csv", "w");
	fprintf(csv, "lifecycle_pct,modify_pct,m1_ns,m2_ns,m3_ns,m4_ns,winner\n");

	int seed = 42;

	printf("Exhaustive 2D sweep: all 4 methods, 0-100%% lifecycle × 0-100%% modify\n");
	printf("=================================================================\n\n");

	int m1_wins = 0, m2_wins = 0, m3_wins = 0, m4_wins = 0;

	for (int lifecycle = 0; lifecycle <= 100; lifecycle += 10) {
		for (int modify = 0; modify <= 100; modify += 10) {
			long long m1, m2, m3, m4;
			run_workload(lifecycle, modify, &seed, &m1, &m2, &m3, &m4);

			long long min = m1;
			int winner = 1;
			if (m2 < min) {
				min = m2;
				winner = 2;
			}
			if (m3 < min) {
				min = m3;
				winner = 3;
			}
			if (m4 < min) {
				min = m4;
				winner = 4;
			}

			if (winner == 1)
				m1_wins++;
			else if (winner == 2)
				m2_wins++;
			else if (winner == 3)
				m3_wins++;
			else
				m4_wins++;

			printf("lc:%3d%% mo:%3d%% | M1:%8lld M2:%8lld M3:%8lld M4:%8lld | W:M%d\n", lifecycle, modify,
			       m1 / ITERATIONS, m2 / ITERATIONS, m3 / ITERATIONS, m4 / ITERATIONS, winner);

			fprintf(csv, "%d,%d,%lld,%lld,%lld,%lld,%d\n", lifecycle, modify, m1 / ITERATIONS, m2 / ITERATIONS,
			        m3 / ITERATIONS, m4 / ITERATIONS, winner);
		}
	}

	fclose(csv);

	printf("\n=================================================================\n");
	printf("Final wins: M1=%d (%.1f%%), M2=%d (%.1f%%), M3=%d (%.1f%%), M4=%d (%.1f%%)\n", m1_wins,
	       (m1_wins / 121.0) * 100, m2_wins, (m2_wins / 121.0) * 100, m3_wins, (m3_wins / 121.0) * 100, m4_wins,
	       (m4_wins / 121.0) * 100);
	printf("\nResults saved to results/exhaustive_all_methods.csv\n");

	return 0;
}
