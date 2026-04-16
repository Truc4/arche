#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* C equivalent for test_archetype.arche */

typedef struct {
	float *pos_x;
	float *pos_y;
	float *vel_vx;
	float *vel_vy;
	int count;
	int capacity;
	int64_t *free_list;
	int free_count;
} Particle;

typedef struct {
	float *pos_x;
	float *pos_y;
	float *vel_vx;
	float *vel_vy;
	int count;
	int capacity;
	int64_t *free_list;
	int free_count;
} Body;

Particle *particle_alloc(int capacity) {
	/* Single allocation: struct + all column data + free_list */
	size_t struct_sz = sizeof(Particle);
	size_t data_sz = capacity * (4 * sizeof(float) + sizeof(int64_t));
	void *block = malloc(struct_sz + data_sz);

	Particle *p = (Particle *)block;
	p->pos_x = (float *)(block + struct_sz);
	p->pos_y = p->pos_x + capacity;
	p->vel_vx = p->pos_y + capacity;
	p->vel_vy = p->vel_vx + capacity;
	p->free_list = (int64_t *)(p->vel_vy + capacity);
	p->count = 0;
	p->capacity = capacity;
	p->free_count = 0;
	return p;
}

Body *body_alloc(int capacity) {
	/* Single allocation: struct + all column data + free_list */
	size_t struct_sz = sizeof(Body);
	size_t data_sz = capacity * (4 * sizeof(float) + sizeof(int64_t));
	void *block = malloc(struct_sz + data_sz);

	Body *b = (Body *)block;
	b->pos_x = (float *)(block + struct_sz);
	b->pos_y = b->pos_x + capacity;
	b->vel_vx = b->pos_y + capacity;
	b->vel_vy = b->vel_vx + capacity;
	b->free_list = (int64_t *)(b->vel_vy + capacity);
	b->count = 0;
	b->capacity = capacity;
	b->free_count = 0;
	return b;
}

void particle_insert(Particle *p, float pos_vx, float pos_vy, float vel_vx, float vel_vy) {
	int slot;
	if (p->free_count > 0) {
		slot = p->free_list[--p->free_count];
	} else {
		slot = p->count++;
	}
	p->pos_x[slot] = pos_vx;
	p->pos_y[slot] = pos_vy;
	p->vel_vx[slot] = vel_vx;
	p->vel_vy[slot] = vel_vy;
}

void particle_delete(Particle *p, int idx) {
	p->free_list[p->free_count++] = idx;
}

void body_insert(Body *b, float pos_x, float pos_y, float vel_vx, float vel_vy) {
	int slot;
	if (b->free_count > 0) {
		slot = b->free_list[--b->free_count];
	} else {
		slot = b->count++;
	}
	b->pos_x[slot] = pos_x;
	b->pos_y[slot] = pos_y;
	b->vel_vx[slot] = vel_vx;
	b->vel_vy[slot] = vel_vy;
}

void body_delete(Body *b, int idx) {
	b->free_list[b->free_count++] = idx;
}

void move(Particle *p, Body *b) {
	for (int i = 0; i < p->count; i++) {
		p->pos_x[i] += p->vel_vx[i];
		p->pos_y[i] += p->vel_vy[i];
	}
	for (int i = 0; i < b->count; i++) {
		b->pos_x[i] += b->vel_vx[i];
		b->pos_y[i] += b->vel_vy[i];
	}
}

void dampen(Particle *p, Body *b) {
	for (int i = 0; i < p->count; i++) {
		p->vel_vx[i] *= 0.99f;
		p->vel_vy[i] *= 0.99f;
	}
	for (int i = 0; i < b->count; i++) {
		b->vel_vx[i] *= 0.99f;
		b->vel_vy[i] *= 0.99f;
	}
}

void free_particle(Particle *p) {
	/* Single allocation: one free for everything */
	free(p);
}

void free_body(Body *b) {
	/* Single allocation: one free for everything */
	free(b);
}

int main() {
	Particle *particles = particle_alloc(100);
	Body *bodies = body_alloc(50);

	/* Insert live entries using pooling */
	particle_insert(particles, 1.0f, 2.0f, 0.1f, 0.2f);
	particle_insert(particles, 3.0f, 4.0f, 0.3f, 0.4f);
	particle_insert(particles, 5.0f, 6.0f, 0.5f, 0.6f);

	body_insert(bodies, 10.0f, 20.0f, 1.0f, 2.0f);
	body_insert(bodies, 30.0f, 40.0f, 3.0f, 4.0f);

	/* Systems execute on live entries only */
	move(particles, bodies);
	dampen(particles, bodies);

	/* Demonstrate reuse from free list */
	particle_delete(particles, 1);
	particle_insert(particles, 7.0f, 8.0f, 0.7f, 0.8f);

	const char msg[] = "systems executed\n";
	write(1, msg, strlen(msg));

	free_particle(particles);
	free_body(bodies);

	return 0;
}
