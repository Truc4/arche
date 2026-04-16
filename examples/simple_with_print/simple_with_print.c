#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* C equivalent for simple_with_print.arche */

typedef struct {
	int *value;
	int count;
} Counter;

Counter *counter_alloc(int capacity) {
	Counter *c = malloc(sizeof(Counter));
	c->value = malloc(capacity * sizeof(int));
	c->count = capacity;
	return c;
}

void free_counter(Counter *c) {
	free(c->value);
	free(c);
}

int main() {
	Counter *counter = counter_alloc(1);
	int x = 10;
	int y = 20;
	const char msg[] = "counter allocated\n";
	write(1, msg, strlen(msg));

	free_counter(counter);
	return 0;
}
