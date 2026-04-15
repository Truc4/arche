#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define INITIAL 100
#define MAX_LEN 100
#define ITERS 5

int random_int(int *seed, int max) {
	if (max <= 0)
		return 0;
	*seed = (*seed * 1103515245 + 12345) & 0x7fffffff;
	return *seed % max;
}

int main() {
	printf("Test Method 2 add/remove\n");

	for (int iter = 0; iter < ITERS; iter++) {
		printf("Iteration %d\n", iter);

		char *buffer = malloc(INITIAL * MAX_LEN);
		int *lengths = malloc(INITIAL * sizeof(int));
		int *offsets = malloc(INITIAL * sizeof(int));
		int count = INITIAL;
		int buffer_size = 0, buffer_capacity = INITIAL * MAX_LEN;

		// Initialize
		for (int i = 0; i < count; i++) {
			char temp[MAX_LEN];
			snprintf(temp, MAX_LEN, "test_%d", i);
			int len = strlen(temp);
			lengths[i] = len;
			offsets[i] = buffer_size;
			memcpy(buffer + buffer_size, temp, len + 1);
			buffer_size += len + 1;
		}

		printf("  Initial: %d strings, buffer_size=%d\n", count, buffer_size);

		int seed = 42 + iter;

		// Delete 10
		for (int d = 0; d < 10; d++) {
			if (count <= 0)
				break;
			int idx = random_int(&seed, count);
			printf("    Delete index %d (count=%d)\n", idx, count);

			int old_len = lengths[idx];
			int old_offset = offsets[idx];

			int shift_size = buffer_size - (old_offset + old_len + 1);
			if (shift_size > 0) {
				memmove(buffer + old_offset, buffer + old_offset + old_len + 1, shift_size);
			}
			buffer_size -= (old_len + 1);

			for (int i = idx; i < count - 1; i++) {
				lengths[i] = lengths[i + 1];
				offsets[i] = offsets[i + 1] - (old_len + 1);
			}
			count--;
		}

		printf("  After deletes: %d strings\n", count);

		// Add 10
		for (int a = 0; a < 10; a++) {
			char temp[MAX_LEN];
			snprintf(temp, MAX_LEN, "new_%d", a);
			int len = strlen(temp);

			if (buffer_size + len + 1 >= buffer_capacity) {
				buffer_capacity *= 2;
				buffer = realloc(buffer, buffer_capacity);
			}

			lengths = realloc(lengths, (count + 1) * sizeof(int));
			offsets = realloc(offsets, (count + 1) * sizeof(int));

			lengths[count] = len;
			offsets[count] = buffer_size;
			memcpy(buffer + buffer_size, temp, len + 1);
			buffer_size += len + 1;
			count++;
		}

		printf("  After adds: %d strings\n", count);

		free(buffer);
		free(lengths);
		free(offsets);
	}

	printf("Done\n");
	return 0;
}
