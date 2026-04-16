#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* C equivalent for multidim_example.arche */

typedef struct {
	char (*text)[256];
	char (*author)[64];
	int count;
} Message;

typedef struct {
	float (*data)[10][10];
	int count;
} Matrix;

Message *message_alloc(int capacity) {
	Message *m = malloc(sizeof(Message));
	m->text = malloc(capacity * sizeof(char[256]));
	m->author = malloc(capacity * sizeof(char[64]));
	m->count = capacity;
	return m;
}

Matrix *matrix_alloc(int capacity) {
	Matrix *m = malloc(sizeof(Matrix));
	m->data = malloc(capacity * sizeof(float[10][10]));
	m->count = capacity;
	return m;
}

void free_message(Message *m) {
	free(m->text);
	free(m->author);
	free(m);
}

void free_matrix(Matrix *m) {
	free(m->data);
	free(m);
}

int main() {
	Message *messages = message_alloc(100);
	Matrix *matrices = matrix_alloc(50);

	const char msg[] = "allocations done\n";
	write(1, msg, strlen(msg));

	free_message(messages);
	free_matrix(matrices);

	return 0;
}
