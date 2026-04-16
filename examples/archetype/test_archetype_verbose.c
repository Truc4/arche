#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* C equivalent for test_archetype_verbose.arche */

typedef struct {
	float *pos_x;
	float *pos_y;
	float *vel_vx;
	float *vel_vy;
	float *mass;
	int count;
} Particle;

typedef struct {
	float *pos_x;
	float *pos_y;
	float *vel_vx;
	float *vel_vy;
	float *density;
	int count;
} Body;

Particle *particle_alloc(int capacity) {
	Particle *p = malloc(sizeof(Particle));
	p->pos_x = malloc(capacity * sizeof(float));
	p->pos_y = malloc(capacity * sizeof(float));
	p->vel_vx = malloc(capacity * sizeof(float));
	p->vel_vy = malloc(capacity * sizeof(float));
	p->mass = malloc(capacity * sizeof(float));
	p->count = capacity;
	return p;
}

Body *body_alloc(int capacity) {
	Body *b = malloc(sizeof(Body));
	b->pos_x = malloc(capacity * sizeof(float));
	b->pos_y = malloc(capacity * sizeof(float));
	b->vel_vx = malloc(capacity * sizeof(float));
	b->vel_vy = malloc(capacity * sizeof(float));
	b->density = malloc(capacity * sizeof(float));
	b->count = capacity;
	return b;
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
	free(p->pos_x);
	free(p->pos_y);
	free(p->vel_vx);
	free(p->vel_vy);
	free(p->mass);
	free(p);
}

void free_body(Body *b) {
	free(b->pos_x);
	free(b->pos_y);
	free(b->vel_vx);
	free(b->vel_vy);
	free(b->density);
	free(b);
}

int main() {
	Particle *particles = particle_alloc(1000);
	Body *bodies = body_alloc(100);

	move(particles, bodies);
	dampen(particles, bodies);

	const char msg[] = "simulation completed\n";
	write(1, msg, strlen(msg));

	free_particle(particles);
	free_body(bodies);

	return 0;
}
