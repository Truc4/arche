#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* C equivalent for test_array_ops.arche */

typedef struct {
	float *pos;
	float *vel;
	int count;
} Particle;

Particle *particle_alloc(int capacity) {
	Particle *p = malloc(sizeof(Particle));
	p->pos = malloc(capacity * sizeof(float));
	p->vel = malloc(capacity * sizeof(float));
	p->count = capacity;
	return p;
}

void free_particle(Particle *p) {
	free(p->pos);
	free(p->vel);
	free(p);
}

int main() {
	Particle *p = particle_alloc(1000);

	for (int i = 0; i < p->count; i++) {
		p->pos[i] = p->pos[i] + p->vel[i];
	}

	const char msg[] = "array ops done\n";
	write(1, msg, strlen(msg));

	free_particle(p);
	return 0;
}
