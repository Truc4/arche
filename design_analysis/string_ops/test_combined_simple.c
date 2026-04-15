#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INITIAL 1000
#define MAX_LEN 100

int main() {
	printf("Test: simple add/remove\n");

	char **strings = malloc(INITIAL * 2 * sizeof(char *));
	int count = INITIAL;
	int capacity = INITIAL * 2;

	for (int i = 0; i < count; i++) {
		strings[i] = malloc(MAX_LEN);
		snprintf(strings[i], MAX_LEN, "test_%d", i);
	}

	printf("Initial: %d strings\n", count);

	// Delete first 100
	for (int i = 0; i < 100; i++) {
		if (count <= 0)
			break;
		free(strings[0]);
		for (int j = 0; j < count - 1; j++) {
			strings[j] = strings[j + 1];
		}
		count--;
	}

	printf("After deletes: %d strings\n", count);

	// Add 100
	for (int i = 0; i < 100; i++) {
		if (count >= capacity) {
			capacity *= 2;
			strings = realloc(strings, capacity * sizeof(char *));
		}
		strings[count] = malloc(MAX_LEN);
		snprintf(strings[count], MAX_LEN, "new_%d", i);
		count++;
	}

	printf("After adds: %d strings\n", count);

	for (int i = 0; i < count; i++)
		free(strings[i]);
	free(strings);

	printf("Done\n");
	return 0;
}
