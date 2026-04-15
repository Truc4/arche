#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INITIAL_STRINGS 10000
#define MAX_STRING_LEN 100

// === Method 1: Array of pointers ===
typedef struct {
	char **strings;
	int count;
	int capacity;
} Method1;

void method1_create_and_measure(int count, long long *heap_bytes) {
	Method1 m;
	m.capacity = count * 2;

	// Allocate pointer array
	long long ptrs = m.capacity * sizeof(char *);
	m.strings = malloc(ptrs);

	// Allocate individual strings
	long long strings_total = 0;
	for (int i = 0; i < count; i++) {
		m.strings[i] = malloc(MAX_STRING_LEN);
		strings_total += MAX_STRING_LEN;
		snprintf(m.strings[i], MAX_STRING_LEN, "str_%d", i);
	}

	*heap_bytes = ptrs + strings_total;

	// Free
	for (int i = 0; i < count; i++)
		free(m.strings[i]);
	free(m.strings);
}

// === Method 2: Contiguous (computed offsets) ===
typedef struct {
	char *buffer;
	int *lengths;
	int count;
	int buffer_capacity;
} Method2;

void method2_create_and_measure(int count, long long *heap_bytes) {
	Method2 m = {0};
	m.count = count;
	m.buffer_capacity = count * MAX_STRING_LEN;
	m.buffer = malloc(m.buffer_capacity);
	m.lengths = malloc(count * sizeof(int));

	long long buffer_size = m.buffer_capacity;
	long long lengths_size = count * sizeof(int);

	*heap_bytes = buffer_size + lengths_size;

	// Free
	free(m.buffer);
	free(m.lengths);
}

// === Method 3: Contiguous (cached offsets) ===
typedef struct {
	int *offsets;
	int *lengths;
	char *buffer;
	int count;
	int buffer_capacity;
} Method3;

void method3_create_and_measure(int count, long long *heap_bytes) {
	Method3 m = {0};
	m.count = count;
	m.buffer_capacity = count * MAX_STRING_LEN;
	m.buffer = malloc(m.buffer_capacity);
	m.offsets = malloc(count * sizeof(int));
	m.lengths = malloc(count * sizeof(int));

	long long buffer_size = m.buffer_capacity;
	long long offsets_size = count * sizeof(int);
	long long lengths_size = count * sizeof(int);

	*heap_bytes = buffer_size + offsets_size + lengths_size;

	// Free
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

void method4_create_and_measure(int count, long long *heap_bytes) {
	Method4 m;
	m.capacity = count * 2;
	m.matrix = malloc(m.capacity * MAX_STRING_LEN);
	m.lengths = malloc(m.capacity * sizeof(int));

	long long matrix_size = m.capacity * MAX_STRING_LEN;
	long long lengths_size = m.capacity * sizeof(int);

	*heap_bytes = matrix_size + lengths_size;

	// Free
	free(m.matrix);
	free(m.lengths);
}

int main() {
	printf("Memory Footprint Analysis\n");
	printf("====================================\n\n");

	printf("Configuration: %d strings, MAX_STRING_LEN=%d\n", INITIAL_STRINGS, MAX_STRING_LEN);
	printf("Note: Using 2x capacity for dynamic methods (doubled at creation)\n\n");

	long long m1, m2, m3, m4;

	method1_create_and_measure(INITIAL_STRINGS, &m1);
	method2_create_and_measure(INITIAL_STRINGS, &m2);
	method3_create_and_measure(INITIAL_STRINGS, &m3);
	method4_create_and_measure(INITIAL_STRINGS, &m4);

	printf("Memory Usage (bytes):\n");
	printf("  M1 (Array Pointers):        %11lld bytes  (%6.2f KB)\n", m1, m1 / 1024.0);
	printf("  M2 (Contiguous Computed):   %11lld bytes  (%6.2f KB)\n", m2, m2 / 1024.0);
	printf("  M3 (Contiguous Cached):     %11lld bytes  (%6.2f KB)\n", m3, m3 / 1024.0);
	printf("  M4 (Matrix Fixed):          %11lld bytes  (%6.2f KB)\n", m4, m4 / 1024.0);
	printf("\n");

	printf("Per-String Memory:\n");
	printf("  M1: %.1f bytes/string\n", (double)m1 / INITIAL_STRINGS);
	printf("  M2: %.1f bytes/string\n", (double)m2 / INITIAL_STRINGS);
	printf("  M3: %.1f bytes/string\n", (double)m3 / INITIAL_STRINGS);
	printf("  M4: %.1f bytes/string\n", (double)m4 / INITIAL_STRINGS);
	printf("\n");

	printf("Memory vs M1:\n");
	printf("  M1: baseline\n");
	printf("  M2: +%.1f%% (%.0f KB more)\n", (double)(m2 - m1) / m1 * 100, (m2 - m1) / 1024.0);
	printf("  M3: +%.1f%% (%.0f KB more)\n", (double)(m3 - m1) / m1 * 100, (m3 - m1) / 1024.0);
	printf("  M4: +%.1f%% (%.0f KB more)\n", (double)(m4 - m1) / m1 * 100, (m4 - m1) / 1024.0);
	printf("\n");

	// Breakdown for M4
	printf("M4 Breakdown:\n");
	printf("  Matrix (2000 strings × 100 bytes):  %lld KB\n", (long long)2000 * 100 / 1024);
	printf("  Lengths (2000 ints):                %lld KB\n", (long long)2000 * 4 / 1024);
	printf("  Total (at creation):                %lld KB\n", (m4) / 1024);
	printf("\n");

	// Breakdown for M1
	printf("M1 Breakdown:\n");
	printf("  Pointer array (2000 ptrs):          %lld KB\n", (long long)2000 * 8 / 1024);
	printf("  Strings (if avg 50%% utilization): ~%lld KB\n", (long long)INITIAL_STRINGS * 50 / 1024);
	printf("  Total (at creation):                %lld KB\n", (m1) / 1024);
	printf("\n");

	return 0;
}
