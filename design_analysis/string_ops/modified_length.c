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

// Operation: delete every other character, shortening strings
void shrink_string(char *str) {
	int write = 0;
	for (int read = 0; str[read]; read += 2) {
		str[write++] = str[read];
	}
	str[write] = '\0';
}

// Method 1: Array of strings (traditional)
void method1_shrink(char **strings, int count) {
	for (int i = 0; i < count; i++) {
		shrink_string(strings[i]);
	}
}

// Method 2: Contiguous (computed offsets) Contiguous buffer with lengths (needs repack)
void method2_shrink(char *buffer, int *lengths, int count) {
	// This is tricky: we'd need to move data around
	// Realistic approach: compact in-place
	int write_offset = 0;
	for (int i = 0; i < count; i++) {
		int new_len = 0;
		for (int j = 0; j < lengths[i]; j += 2) {
			buffer[write_offset + new_len++] = buffer[write_offset + j];
		}
		buffer[write_offset + new_len] = '\0';
		lengths[i] = new_len;
		write_offset += new_len + 1;
	}
}

// Method 3: Contiguous (cached offsets) Struct of arrays with offsets (needs repack)
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

void method3_shrink(StringArray soa) {
	int write_offset = 0;
	for (int i = 0; i < soa.count; i++) {
		int read_offset = soa.offsets[i];
		int new_len = 0;
		for (int j = 0; j < soa.lengths[i]; j += 2) {
			soa.buffer[write_offset + new_len++] = soa.buffer[read_offset + j];
		}
		soa.buffer[write_offset + new_len] = '\0';
		soa.offsets[i] = write_offset;
		soa.lengths[i] = new_len;
		write_offset += new_len + 1;
	}
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

void method4_shrink(StringMatrix sm) {
	for (int i = 0; i < sm.count; i++) {
		int write = 0;
		for (int read = 0; read < sm.lengths[i]; read += 2) {
			sm.matrix[i][write++] = sm.matrix[i][read];
		}
		sm.matrix[i][write] = '\0';
		sm.lengths[i] = write;
	}
}

int main() {
	printf("String Benchmark (MODIFIED LENGTH) - %d strings, %d iterations\n", NUM_STRINGS, ITERATIONS);
	printf("Operation: Delete every other character (shrink strings by ~50%%)\n\n");

	// Generate test data once
	char **original = generate_string_array(NUM_STRINGS);

	// Method 1: Array of pointers
	printf("Method 1: Array of pointers (char**)\n");
	char **strings1 = malloc(NUM_STRINGS * sizeof(char *));
	for (int i = 0; i < NUM_STRINGS; i++) {
		strings1[i] = malloc(MAX_STRING_LEN);
		strcpy(strings1[i], original[i]);
	}

	Timer t1 = timer_start();
	for (int iter = 0; iter < ITERATIONS; iter++) {
		// Reset strings before each iteration
		for (int i = 0; i < NUM_STRINGS; i++)
			strcpy(strings1[i], original[i]);
		method1_shrink(strings1, NUM_STRINGS);
	}
	long long time1 = timer_end(t1);
	printf("  Time: %.2f ms (%.0f ns/op)\n\n", time1 / 1e6, (double)time1 / ITERATIONS);

	free_string_array(strings1, NUM_STRINGS);

	// Method 2: Contiguous (computed offsets) Contiguous buffer with lengths
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
	Timer t2 = timer_start();
	for (int iter = 0; iter < ITERATIONS; iter++) {
		// Reset
		memcpy(buffer, buffer_orig, total_size);
		int *lens = malloc(NUM_STRINGS * sizeof(int));
		for (int i = 0; i < NUM_STRINGS; i++)
			lens[i] = lengths[i];
		method2_shrink(buffer, lens, NUM_STRINGS);
		free(lens);
	}
	long long time2 = timer_end(t2);
	printf("  Time: %.2f ms (%.0f ns/op)\n\n", time2 / 1e6, (double)time2 / ITERATIONS);

	free(buffer);
	free(buffer_orig);
	free(lengths);

	// Method 3: Contiguous (cached offsets) SOA with offsets
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

	Timer t3 = timer_start();
	for (int iter = 0; iter < ITERATIONS; iter++) {
		// Reset SOA before each iteration
		int pos = 0;
		for (int i = 0; i < NUM_STRINGS; i++) {
			int len = strlen(original[i]);
			soa.lengths[i] = len;
			soa.offsets[i] = pos;
			memcpy(soa.buffer + pos, original[i], len + 1);
			pos += len + 1;
		}
		method3_shrink(soa);
	}
	long long time3 = timer_end(t3);
	printf("  Time: %.2f ms (%.0f ns/op)\n\n", time3 / 1e6, (double)time3 / ITERATIONS);

	free(soa.buffer);
	free(soa.lengths);
	free(soa.offsets);
	free_string_soa(soa_orig);

	// Method 4: Matrix with lengths
	printf("Method 4: 2D matrix (fixed MAX_STRING_LEN rows)\n");
	StringMatrix matrix = create_string_matrix(original, NUM_STRINGS);

	Timer t4 = timer_start();
	for (int iter = 0; iter < ITERATIONS; iter++) {
		// Reset matrix before each iteration
		for (int i = 0; i < NUM_STRINGS; i++) {
			int len = strlen(original[i]);
			memcpy(matrix.matrix[i], original[i], len + 1);
			matrix.lengths[i] = len;
		}
		method4_shrink(matrix);
	}
	long long time4 = timer_end(t4);
	printf("  Time: %.2f ms (%.0f ns/op)\n\n", time4 / 1e6, (double)time4 / ITERATIONS);

	free_string_matrix(matrix);

	// Summary
	printf("Summary (lower is better):\n");
	printf("  Method 1 (pointers):        %.0f ns/op (baseline)\n", (double)time1 / ITERATIONS);
	printf("  Method 2 (contiguous):      %.0f ns/op (%.1f%% %s)\n", (double)time2 / ITERATIONS,
	       fabs((double)(time1 - time2) / time1 * 100), time2 < time1 ? "faster" : "slower");
	printf("  Method 3 (SOA+offsets):     %.0f ns/op (%.1f%% %s)\n", (double)time3 / ITERATIONS,
	       fabs((double)(time1 - time3) / time1 * 100), time3 < time1 ? "faster" : "slower");
	printf("  Method 4 (2D matrix):       %.0f ns/op (%.1f%% %s)\n", (double)time4 / ITERATIONS,
	       fabs((double)(time1 - time4) / time1 * 100), time4 < time1 ? "faster" : "slower");

	// Sanity check: verify method 4 actually works
	printf("\nSanity check (Method 4 sample results):\n");
	StringMatrix check = create_string_matrix(original, NUM_STRINGS);
	printf("Before: '%s' (len=%d)\n", check.matrix[0], check.lengths[0]);
	method4_shrink(check);
	printf("After:  '%s' (len=%d)\n", check.matrix[0], check.lengths[0]);
	free_string_matrix(check);

	free_string_array(original, NUM_STRINGS);

	return 0;
}
