#include <ctype.h>
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
		snprintf(strings[i], MAX_STRING_LEN, "test_string_%d_with_some_data", i);
	}
	return strings;
}

void free_string_array(char **strings, int count) {
	for (int i = 0; i < count; i++)
		free(strings[i]);
	free(strings);
}

// Method 1: Array of strings (traditional)
void method1_uppercase(char **strings, int count) {
	for (int i = 0; i < count; i++) {
		for (char *p = strings[i]; *p; p++) {
			*p = toupper((unsigned char)*p);
		}
	}
}

// Method 2: Contiguous (computed offsets) Contiguous buffer with lengths (data-oriented)
void method2_uppercase(char *buffer, int *lengths, int count) {
	int offset = 0;
	for (int i = 0; i < count; i++) {
		for (int j = 0; j < lengths[i]; j++) {
			buffer[offset + j] = toupper((unsigned char)buffer[offset + j]);
		}
		offset += lengths[i] + 1; // +1 for null terminator
	}
}

// Method 3: Contiguous (cached offsets) Struct of arrays (SOA)
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

	// Calculate total size
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

void method3_uppercase(StringArray soa) {
	for (int i = 0; i < soa.count; i++) {
		int offset = soa.offsets[i];
		for (int j = 0; j < soa.lengths[i]; j++) {
			soa.buffer[offset + j] = toupper((unsigned char)soa.buffer[offset + j]);
		}
	}
}

// Method 4: 2D matrix (fixed-size rows)
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

void method4_uppercase(StringMatrix sm) {
	for (int i = 0; i < sm.count; i++) {
		for (int j = 0; j < sm.lengths[i]; j++) {
			sm.matrix[i][j] = toupper((unsigned char)sm.matrix[i][j]);
		}
	}
}

int main() {
	printf("String Benchmark - %d strings, %d iterations\n\n", NUM_STRINGS, ITERATIONS);

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
		method1_uppercase(strings1, NUM_STRINGS);
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
	char *buffer = malloc(total_size);
	int pos = 0;
	for (int i = 0; i < NUM_STRINGS; i++) {
		memcpy(buffer + pos, original[i], lengths[i]);
		buffer[pos + lengths[i]] = '\0';
		pos += lengths[i] + 1;
	}

	Timer t2 = timer_start();
	for (int iter = 0; iter < ITERATIONS; iter++) {
		method2_uppercase(buffer, lengths, NUM_STRINGS);
	}
	long long time2 = timer_end(t2);
	printf("  Time: %.2f ms (%.0f ns/op)\n", time2 / 1e6, (double)time2 / ITERATIONS);
	printf("  Memory: %.2f KB (vs %.2f KB method 1)\n\n", (total_size + NUM_STRINGS * sizeof(int)) / 1024.0,
	       NUM_STRINGS * MAX_STRING_LEN / 1024.0);

	free(buffer);
	free(lengths);

	// Method 3: Contiguous (cached offsets) SOA with offsets
	printf("Method 3: Contiguous (cached offsets) Struct of Arrays (offsets + buffer)\n");
	StringArray soa = create_string_soa(original, NUM_STRINGS);

	Timer t3 = timer_start();
	for (int iter = 0; iter < ITERATIONS; iter++) {
		method3_uppercase(soa);
	}
	long long time3 = timer_end(t3);
	printf("  Time: %.2f ms (%.0f ns/op)\n", time3 / 1e6, (double)time3 / ITERATIONS);
	printf("  Memory: %.2f KB\n\n", (soa.count * sizeof(int) * 2 + total_size) / 1024.0);

	free_string_soa(soa);

	// Method 4: Matrix with lengths
	printf("Method 4: 2D matrix (fixed MAX_STRING_LEN rows)\n");
	StringMatrix matrix = create_string_matrix(original, NUM_STRINGS);

	Timer t4 = timer_start();
	for (int iter = 0; iter < ITERATIONS; iter++) {
		method4_uppercase(matrix);
	}
	long long time4 = timer_end(t4);
	printf("  Time: %.2f ms (%.0f ns/op)\n", time4 / 1e6, (double)time4 / ITERATIONS);
	printf("  Memory: %.2f KB (fixed allocation)\n\n",
	       (NUM_STRINGS * MAX_STRING_LEN + NUM_STRINGS * sizeof(int)) / 1024.0);

	free_string_matrix(matrix);

	// Summary
	printf("Summary (lower is better):\n");
	printf("  Method 1 (pointers):        %.0f ns/op (baseline)\n", (double)time1 / ITERATIONS);
	printf("  Method 2 (contiguous):      %.0f ns/op (%.1f%% faster)\n", (double)time2 / ITERATIONS,
	       (double)(time1 - time2) / time1 * 100);
	printf("  Method 3 (SOA+offsets):     %.0f ns/op (%.1f%% faster)\n", (double)time3 / ITERATIONS,
	       (double)(time1 - time3) / time1 * 100);
	printf("  Method 4 (2D matrix):       %.0f ns/op (%.1f%% faster)\n", (double)time4 / ITERATIONS,
	       (double)(time1 - time4) / time1 * 100);

	// Output JSON results
	FILE *out = fopen("benchmarks/string_ops/results/fixed_length_results.json", "w");
	fprintf(out, "{\n");
	fprintf(out, "  \"benchmark\": \"fixed_length\",\n");
	fprintf(out, "  \"num_strings\": %d,\n", NUM_STRINGS);
	fprintf(out, "  \"iterations\": %d,\n", ITERATIONS);
	fprintf(out, "  \"operation\": \"uppercase (in-place)\",\n");
	fprintf(out, "  \"methods\": [\n");
	fprintf(out, "    {\"name\": \"array_pointers\", \"ns_per_op\": %.0f},\n", (double)time1 / ITERATIONS);
	fprintf(out, "    {\"name\": \"contiguous_buffer\", \"ns_per_op\": %.0f},\n", (double)time2 / ITERATIONS);
	fprintf(out, "    {\"name\": \"soa_offsets\", \"ns_per_op\": %.0f},\n", (double)time3 / ITERATIONS);
	fprintf(out, "    {\"name\": \"matrix_fixed\", \"ns_per_op\": %.0f}\n", (double)time4 / ITERATIONS);
	fprintf(out, "  ]\n");
	fprintf(out, "}\n");
	fclose(out);

	printf("\nResults saved to design_analysis/string_ops/results/fixed_length_results.json\n");

	free_string_array(original, NUM_STRINGS);

	return 0;
}
