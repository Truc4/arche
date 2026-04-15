#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NUM_STRINGS 10000
#define MAX_STRING_LEN 100
#define ITERATIONS 1000

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

// Generate test data
char **generate_string_array(int count) {
	char **strings = malloc(count * sizeof(char *));
	for (int i = 0; i < count; i++) {
		strings[i] = malloc(MAX_STRING_LEN);
		snprintf(strings[i], MAX_STRING_LEN, "test_string_%d_with_some_data_content", i);
	}
	return strings;
}

void free_string_array(char **strings, int count) {
	for (int i = 0; i < count; i++)
		free(strings[i]);
	free(strings);
}

// Operation: mixed modifications (grow, shrink, or stay same per string)
int random_op(int *seed) {
	*seed = (*seed * 1103515245 + 12345) & 0x7fffffff;
	return *seed % 3; // 0: shrink, 1: grow, 2: stay same
}

void mixed_modify_string(char *str, int *seed) {
	int op = random_op(seed);
	int len = strlen(str);

	if (op == 0 && len > 5) {
		// Shrink: delete every other character
		int write = 0;
		for (int read = 0; read < len; read += 2) {
			str[write++] = str[read];
		}
		str[write] = '\0';
	} else if (op == 1 && len < MAX_STRING_LEN - 10) {
		// Grow: append suffix
		strcat(str, "_modified");
	}
	// else: stay same
}

// Method 1: Array of strings (traditional)
void method1_modify(char **strings, int count, int *seed) {
	for (int i = 0; i < count; i++) {
		mixed_modify_string(strings[i], seed);
	}
}

// Method 2: Contiguous (computed offsets) Contiguous buffer with lengths (needs repack)
void method2_modify(char *buffer, int *lengths, int count, int *seed) {
	// For mixed changes: need to reallocate or handle gaps
	// Realistic: copy to new buffer, compacting on the fly
	char *new_buffer = malloc(count * MAX_STRING_LEN);
	int write_offset = 0;

	for (int i = 0; i < count; i++) {
		int read_offset = 0;
		for (int j = 0; j < i; j++) {
			read_offset += lengths[j] + 1;
		}

		int old_len = lengths[i];
		int op = random_op(seed);
		int new_len = old_len;

		// Simulate modifications
		if (op == 0 && old_len > 5) {
			new_len = (old_len + 1) / 2; // shrink
		} else if (op == 1 && old_len < MAX_STRING_LEN - 10) {
			new_len = old_len + 9; // "_modified" = 9 chars
		}

		// Copy to new buffer
		if (new_len > 0) {
			memcpy(new_buffer + write_offset, buffer + read_offset, new_len < old_len ? new_len : old_len);
		}
		new_buffer[write_offset + new_len] = '\0';
		lengths[i] = new_len;
		write_offset += new_len + 1;
	}

	memcpy(buffer, new_buffer, write_offset);
	free(new_buffer);
}

// Method 3: Contiguous (cached offsets) Struct of arrays with offsets
typedef struct {
	char **data;
	int *lengths;
	int *offsets;
	char *buffer;
	int count;
} StringArray;

StringArray create_string_soa(char **strings, int count) {
	StringArray soa = {0};
	soa.count = count;
	soa.offsets = malloc(count * sizeof(int));
	soa.lengths = malloc(count * sizeof(int));

	int total_size = 0;
	for (int i = 0; i < count; i++) {
		soa.lengths[i] = strlen(strings[i]);
		soa.offsets[i] = total_size;
		total_size += soa.lengths[i] + 1;
	}

	soa.buffer = malloc(total_size);
	int pos = 0;
	for (int i = 0; i < count; i++) {
		memcpy(soa.buffer + pos, strings[i], soa.lengths[i]);
		soa.buffer[pos + soa.lengths[i]] = '\0';
		pos += soa.lengths[i] + 1;
	}

	return soa;
}

void free_string_soa(StringArray soa) {
	free(soa.buffer);
	free(soa.lengths);
	free(soa.offsets);
}

void method3_modify(StringArray soa, int *seed) {
	char *new_buffer = malloc(soa.count * MAX_STRING_LEN);
	int write_offset = 0;

	for (int i = 0; i < soa.count; i++) {
		int old_len = soa.lengths[i];
		int op = random_op(seed);
		int new_len = old_len;

		if (op == 0 && old_len > 5) {
			new_len = (old_len + 1) / 2;
		} else if (op == 1 && old_len < MAX_STRING_LEN - 10) {
			new_len = old_len + 9;
		}

		memcpy(new_buffer + write_offset, soa.buffer + soa.offsets[i], new_len < old_len ? new_len : old_len);
		new_buffer[write_offset + new_len] = '\0';
		soa.offsets[i] = write_offset;
		soa.lengths[i] = new_len;
		write_offset += new_len + 1;
	}

	memcpy(soa.buffer, new_buffer, write_offset);
	free(new_buffer);
}

// Method 4: 2D matrix (fixed-size rows, just update lengths)
typedef struct {
	char (*matrix)[MAX_STRING_LEN];
	int *lengths;
	int count;
} StringMatrix;

StringMatrix create_string_matrix(char **strings, int count) {
	StringMatrix sm;
	sm.matrix = malloc(count * MAX_STRING_LEN);
	sm.lengths = malloc(count * sizeof(int));
	sm.count = count;

	for (int i = 0; i < count; i++) {
		sm.lengths[i] = strlen(strings[i]);
		memcpy(sm.matrix[i], strings[i], sm.lengths[i] + 1);
	}
	return sm;
}

void free_string_matrix(StringMatrix sm) {
	free(sm.matrix);
	free(sm.lengths);
}

void method4_modify(StringMatrix sm, int *seed) {
	for (int i = 0; i < sm.count; i++) {
		int old_len = sm.lengths[i];
		int op = random_op(seed);

		if (op == 0 && old_len > 5) {
			// Shrink: delete every other character
			int write = 0;
			for (int read = 0; read < old_len; read += 2) {
				sm.matrix[i][write++] = sm.matrix[i][read];
			}
			sm.matrix[i][write] = '\0';
			sm.lengths[i] = write;
		} else if (op == 1 && old_len < MAX_STRING_LEN - 10) {
			// Grow: append suffix
			strcat(sm.matrix[i], "_modified");
			sm.lengths[i] = strlen(sm.matrix[i]);
		}
		// else: stay same
	}
}

int main() {
	printf("String Benchmark (MIXED MODIFICATIONS) - %d strings, %d iterations\n", NUM_STRINGS, ITERATIONS);
	printf("Operation: Random per-string (33%% shrink, 33%% grow, 33%% stay same)\n\n");

	char **original = generate_string_array(NUM_STRINGS);

	// Method 1
	printf("Method 1: Array of pointers (char**)\n");
	char **strings1 = malloc(NUM_STRINGS * sizeof(char *));
	for (int i = 0; i < NUM_STRINGS; i++) {
		strings1[i] = malloc(MAX_STRING_LEN);
		strcpy(strings1[i], original[i]);
	}

	int seed = 42;
	Timer t1 = timer_start();
	for (int iter = 0; iter < ITERATIONS; iter++) {
		for (int i = 0; i < NUM_STRINGS; i++)
			strcpy(strings1[i], original[i]);
		method1_modify(strings1, NUM_STRINGS, &seed);
	}
	long long time1 = timer_end(t1);
	printf("  Time: %.2f ms (%.0f ns/op)\n\n", time1 / 1e6, (double)time1 / ITERATIONS);
	free_string_array(strings1, NUM_STRINGS);

	// Method 2
	printf("Method 2: Contiguous (computed offsets) Contiguous buffer + length array\n");
	int *lengths = malloc(NUM_STRINGS * sizeof(int));
	int total_size = 0;
	for (int i = 0; i < NUM_STRINGS; i++) {
		lengths[i] = strlen(original[i]);
		total_size += lengths[i] + 1;
	}
	char *buffer_orig = malloc(total_size);
	int pos = 0;
	for (int i = 0; i < NUM_STRINGS; i++) {
		memcpy(buffer_orig + pos, original[i], lengths[i]);
		buffer_orig[pos + lengths[i]] = '\0';
		pos += lengths[i] + 1;
	}

	char *buffer = malloc(total_size);
	seed = 42;
	Timer t2 = timer_start();
	for (int iter = 0; iter < ITERATIONS; iter++) {
		memcpy(buffer, buffer_orig, total_size);
		int *lens = malloc(NUM_STRINGS * sizeof(int));
		for (int i = 0; i < NUM_STRINGS; i++)
			lens[i] = lengths[i];
		method2_modify(buffer, lens, NUM_STRINGS, &seed);
		free(lens);
	}
	long long time2 = timer_end(t2);
	printf("  Time: %.2f ms (%.0f ns/op)\n\n", time2 / 1e6, (double)time2 / ITERATIONS);
	free(buffer);
	free(buffer_orig);
	free(lengths);

	// Method 3
	printf("Method 3: Contiguous (cached offsets) Struct of Arrays (offsets + buffer)\n");
	StringArray soa_orig = create_string_soa(original, NUM_STRINGS);
	int soa_buffer_size = 0;
	for (int i = 0; i < NUM_STRINGS; i++) {
		soa_buffer_size += strlen(original[i]) + 1;
	}

	StringArray soa = {.buffer = malloc(soa_buffer_size),
	                   .lengths = malloc(NUM_STRINGS * sizeof(int)),
	                   .offsets = malloc(NUM_STRINGS * sizeof(int)),
	                   .count = NUM_STRINGS};

	seed = 42;
	Timer t3 = timer_start();
	for (int iter = 0; iter < ITERATIONS; iter++) {
		int pos = 0;
		for (int i = 0; i < NUM_STRINGS; i++) {
			int len = strlen(original[i]);
			soa.lengths[i] = len;
			soa.offsets[i] = pos;
			memcpy(soa.buffer + pos, original[i], len + 1);
			pos += len + 1;
		}
		method3_modify(soa, &seed);
	}
	long long time3 = timer_end(t3);
	printf("  Time: %.2f ms (%.0f ns/op)\n\n", time3 / 1e6, (double)time3 / ITERATIONS);
	free(soa.buffer);
	free(soa.lengths);
	free(soa.offsets);
	free_string_soa(soa_orig);

	// Method 4
	printf("Method 4: 2D matrix (fixed MAX_STRING_LEN rows)\n");
	StringMatrix matrix = create_string_matrix(original, NUM_STRINGS);

	seed = 42;
	Timer t4 = timer_start();
	for (int iter = 0; iter < ITERATIONS; iter++) {
		for (int i = 0; i < NUM_STRINGS; i++) {
			int len = strlen(original[i]);
			memcpy(matrix.matrix[i], original[i], len + 1);
			matrix.lengths[i] = len;
		}
		method4_modify(matrix, &seed);
	}
	long long time4 = timer_end(t4);
	printf("  Time: %.2f ms (%.0f ns/op)\n\n", time4 / 1e6, (double)time4 / ITERATIONS);
	free_string_matrix(matrix);

	// Output JSON results
	FILE *out = fopen("results/mixed_modifications_results.json", "w");
	fprintf(out, "{\n");
	fprintf(out, "  \"benchmark\": \"mixed_modifications\",\n");
	fprintf(out, "  \"num_strings\": %d,\n", NUM_STRINGS);
	fprintf(out, "  \"iterations\": %d,\n", ITERATIONS);
	fprintf(out, "  \"operation\": \"random per-string (33%% shrink, 33%% grow, 33%% stay same)\",\n");
	fprintf(out, "  \"methods\": [\n");
	fprintf(out, "    {\"name\": \"array_pointers\", \"ns_per_op\": %.0f},\n", (double)time1 / ITERATIONS);
	fprintf(out, "    {\"name\": \"contiguous_buffer\", \"ns_per_op\": %.0f},\n", (double)time2 / ITERATIONS);
	fprintf(out, "    {\"name\": \"soa_offsets\", \"ns_per_op\": %.0f},\n", (double)time3 / ITERATIONS);
	fprintf(out, "    {\"name\": \"matrix_fixed\", \"ns_per_op\": %.0f}\n", (double)time4 / ITERATIONS);
	fprintf(out, "  ]\n");
	fprintf(out, "}\n");
	fclose(out);

	printf("Results saved to results/mixed_modifications_results.json\n");

	free_string_array(original, NUM_STRINGS);
	return 0;
}
