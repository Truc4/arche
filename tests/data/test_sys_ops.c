#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* C equivalent for test_sys_ops.arche */

typedef struct {
	float *position;
	float *velocity;
	int count;
} Body;

Body *body_alloc(int capacity) {
	Body *b = malloc(sizeof(Body));
	b->position = malloc(capacity * sizeof(float));
	b->velocity = malloc(capacity * sizeof(float));
	b->count = capacity;
	return b;
}

void move(Body *b) {
	for (int i = 0; i < b->count; i++) {
		b->position[i] = b->position[i] + b->velocity[i];
	}
}

void free_body(Body *b) {
	free(b->position);
	free(b->velocity);
	free(b);
}

int main() {
	Body *b = body_alloc(500);

	move(b);

	const char msg[] = "system executed\n";
	write(1, msg, strlen(msg));

	free_body(b);
	return 0;
}
