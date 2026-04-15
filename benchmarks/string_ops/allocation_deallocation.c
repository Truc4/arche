#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define INITIAL_STRINGS 5000
#define MAX_STRING_LEN 100
#define ITERATIONS 100

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
	*seed = (*seed * 1103515245 + 12345) & 0x7fffffff;
	return *seed % max;
}

char *generate_string(int id) {
	char *str = malloc(MAX_STRING_LEN);
	snprintf(str, MAX_STRING_LEN, "entity_%d", id);
	return str;
}

// Method 1: Array of pointers
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
		m.strings[i] = generate_string(i);
	}
	return m;
}

void method1_add(Method1 *m, int entity_id) {
	if (m->count >= m->capacity) {
		m->capacity *= 2;
		m->strings = realloc(m->strings, m->capacity * sizeof(char *));
	}
	m->strings[m->count++] = generate_string(entity_id);
}

void method1_remove(Method1 *m, int index) {
	if (index < 0 || index >= m->count)
		return;
	free(m->strings[index]);
	// Shift remaining pointers
	for (int i = index; i < m->count - 1; i++) {
		m->strings[i] = m->strings[i + 1];
	}
	m->count--;
}

void method1_free(Method1 m) {
	for (int i = 0; i < m.count; i++)
		free(m.strings[i]);
	free(m.strings);
}

// Method 2: Contiguous (computed offsets) Contiguous buffer + lengths
typedef struct {
	char *buffer;
	int *lengths;
	int *offsets;
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
	m.offsets = malloc(initial_count * sizeof(int));

	int pos = 0;
	for (int i = 0; i < initial_count; i++) {
		char temp[MAX_STRING_LEN];
		snprintf(temp, MAX_STRING_LEN, "entity_%d", i);
		int len = strlen(temp);
		m.lengths[i] = len;
		m.offsets[i] = pos;
		memcpy(m.buffer + pos, temp, len + 1);
		pos += len + 1;
		m.buffer_size = pos;
	}
	return m;
}

void method2_add(Method2 *m, int entity_id) {
	if (m->count >= 10000)
		return; // Limit for sanity

	char temp[MAX_STRING_LEN];
	snprintf(temp, MAX_STRING_LEN, "entity_%d", entity_id);
	int len = strlen(temp);

	// Ensure capacity
	if (m->buffer_size + len + 1 >= m->buffer_capacity) {
		m->buffer_capacity *= 2;
		m->buffer = realloc(m->buffer, m->buffer_capacity);
	}

	// Reallocate metadata arrays
	m->lengths = realloc(m->lengths, (m->count + 1) * sizeof(int));
	m->offsets = realloc(m->offsets, (m->count + 1) * sizeof(int));

	m->lengths[m->count] = len;
	m->offsets[m->count] = m->buffer_size;
	memcpy(m->buffer + m->buffer_size, temp, len + 1);
	m->buffer_size += len + 1;
	m->count++;
}

void method2_remove(Method2 *m, int index) {
	if (index < 0 || index >= m->count)
		return;

	// Shift all subsequent strings (expensive!)
	int old_len = m->lengths[index];
	int old_offset = m->offsets[index];

	// Move memory
	int shift_size = m->buffer_size - (old_offset + old_len + 1);
	if (shift_size > 0) {
		memmove(m->buffer + old_offset, m->buffer + old_offset + old_len + 1, shift_size);
	}
	m->buffer_size -= (old_len + 1);

	// Shift metadata
	for (int i = index; i < m->count - 1; i++) {
		m->lengths[i] = m->lengths[i + 1];
		m->offsets[i] = m->offsets[i + 1] - (old_len + 1);
	}
	m->count--;
}

void method2_free(Method2 m) {
	free(m.buffer);
	free(m.lengths);
	free(m.offsets);
}

// Method 3: Contiguous (cached offsets) SOA with offsets (similar to Method 2)
// Skipping for brevity, same removal complexity as Method 2

// Method 4: 2D matrix (fixed-size rows)
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
		snprintf(temp, MAX_STRING_LEN, "entity_%d", i);
		int len = strlen(temp);
		m.lengths[i] = len;
		memcpy(m.matrix[i], temp, len + 1);
	}
	return m;
}

void method4_add(Method4 *m, int entity_id) {
	if (m->count >= m->capacity) {
		m->capacity *= 2;
		m->matrix = realloc(m->matrix, m->capacity * MAX_STRING_LEN);
		m->lengths = realloc(m->lengths, m->capacity * sizeof(int));
	}

	char temp[MAX_STRING_LEN];
	snprintf(temp, MAX_STRING_LEN, "entity_%d", entity_id);
	int len = strlen(temp);
	m->lengths[m->count] = len;
	memcpy(m->matrix[m->count], temp, len + 1);
	m->count++;
}

void method4_remove(Method4 *m, int index) {
	if (index < 0 || index >= m->count)
		return;
	// Swap with last and shrink (O(1))
	if (index < m->count - 1) {
		m->lengths[index] = m->lengths[m->count - 1];
		memcpy(m->matrix[index], m->matrix[m->count - 1], MAX_STRING_LEN);
	}
	m->count--;
}

void method4_free(Method4 m) {
	free(m.matrix);
	free(m.lengths);
}

int main() {
	printf("String Benchmark (ALLOCATION/DEALLOCATION) - %d iterations\n", ITERATIONS);
	printf("Operation: Add 20%%, delete 15%% per iteration\n\n");

	// Method 1
	printf("Method 1: Array of pointers (char**)\n");
	int seed = 42;
	Timer t1 = timer_start();
	for (int iter = 0; iter < ITERATIONS; iter++) {
		Method1 m1 = method1_create(INITIAL_STRINGS);

		int to_delete = m1.count / 7;       // ~15%
		int to_add = (INITIAL_STRINGS / 5); // ~20%

		for (int d = 0; d < to_delete; d++) {
			int idx = random_int(&seed, m1.count);
			method1_remove(&m1, idx);
		}
		for (int a = 0; a < to_add; a++) {
			method1_add(&m1, INITIAL_STRINGS + a);
		}

		method1_free(m1);
	}
	long long time1 = timer_end(t1);
	printf("  Time: %.2f ms (%.0f ns/op)\n\n", time1 / 1e6, (double)time1 / ITERATIONS);

	// Method 2
	printf("Method 2: Contiguous (computed offsets) Contiguous buffer + lengths (expensive delete!)\n");
	seed = 42;
	Timer t2 = timer_start();
	for (int iter = 0; iter < ITERATIONS; iter++) {
		Method2 m2 = method2_create(INITIAL_STRINGS);

		int to_delete = m2.count / 7;
		int to_add = (INITIAL_STRINGS / 5);

		for (int d = 0; d < to_delete; d++) {
			int idx = random_int(&seed, m2.count);
			method2_remove(&m2, idx);
		}
		for (int a = 0; a < to_add; a++) {
			method2_add(&m2, INITIAL_STRINGS + a);
		}

		method2_free(m2);
	}
	long long time2 = timer_end(t2);
	printf("  Time: %.2f ms (%.0f ns/op)\n\n", time2 / 1e6, (double)time2 / ITERATIONS);

	// Method 4
	printf("Method 4: 2D matrix (O(1) delete via swap)\n");
	seed = 42;
	Timer t4 = timer_start();
	for (int iter = 0; iter < ITERATIONS; iter++) {
		Method4 m4 = method4_create(INITIAL_STRINGS);

		int to_delete = m4.count / 7;
		int to_add = (INITIAL_STRINGS / 5);

		for (int d = 0; d < to_delete; d++) {
			int idx = random_int(&seed, m4.count);
			method4_remove(&m4, idx);
		}
		for (int a = 0; a < to_add; a++) {
			method4_add(&m4, INITIAL_STRINGS + a);
		}

		method4_free(m4);
	}
	long long time4 = timer_end(t4);
	printf("  Time: %.2f ms (%.0f ns/op)\n\n", time4 / 1e6, (double)time4 / ITERATIONS);

	// Output JSON
	FILE *out = fopen("results/allocation_deallocation_results.json", "w");
	fprintf(out, "{\n");
	fprintf(out, "  \"benchmark\": \"allocation_deallocation\",\n");
	fprintf(out, "  \"initial_strings\": %d,\n", INITIAL_STRINGS);
	fprintf(out, "  \"iterations\": %d,\n", ITERATIONS);
	fprintf(out, "  \"operation\": \"add 20%%, delete 15%% per iteration\",\n");
	fprintf(out, "  \"methods\": [\n");
	fprintf(out, "    {\"name\": \"array_pointers\", \"ns_per_op\": %.0f},\n", (double)time1 / ITERATIONS);
	fprintf(out, "    {\"name\": \"contiguous_buffer\", \"ns_per_op\": %.0f},\n", (double)time2 / ITERATIONS);
	fprintf(out, "    {\"name\": \"matrix_fixed\", \"ns_per_op\": %.0f}\n", (double)time4 / ITERATIONS);
	fprintf(out, "  ]\n");
	fprintf(out, "}\n");
	fclose(out);

	printf("Results saved to results/allocation_deallocation_results.json\n");
	return 0;
}
