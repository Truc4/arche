#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NUM_ENTITIES 10000
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

/* ============ AoS ============ */

typedef struct {
	float x, y, z;
} Vec3;

typedef struct {
	Vec3 pos;
	Vec3 vel;
	int alive;
} EntityAoS;

typedef struct {
	EntityAoS *entities;
	int count;
	int capacity;
} ArrayAoS;

ArrayAoS aos_create(int capacity) {
	ArrayAoS arr = {0};
	arr.capacity = capacity;
	arr.entities = malloc(capacity * sizeof(EntityAoS));
	for (int i = 0; i < capacity; i++) {
		arr.entities[i] = (EntityAoS){
		    {(float)rand() / RAND_MAX, (float)rand() / RAND_MAX, (float)rand() / RAND_MAX},
		    {(float)rand() / RAND_MAX * 0.01f, (float)rand() / RAND_MAX * 0.01f, (float)rand() / RAND_MAX * 0.01f},
		    1};
	}
	arr.count = capacity;
	return arr;
}

void aos_update(ArrayAoS *arr) {
	for (int i = 0; i < arr->count; i++) {
		if (arr->entities[i].alive) {
			arr->entities[i].pos.x += arr->entities[i].vel.x;
			arr->entities[i].pos.y += arr->entities[i].vel.y;
			arr->entities[i].pos.z += arr->entities[i].vel.z;
		}
	}
}

void aos_delete_random(ArrayAoS *arr, int count) {
	for (int i = 0; i < count; i++) {
		int idx = rand() % arr->count;
		arr->entities[idx].alive = 0;
	}
}

void aos_add_random(ArrayAoS *arr, int count) {
	if (arr->count + count > arr->capacity) {
		arr->capacity = arr->count + count;
		arr->entities = realloc(arr->entities, arr->capacity * sizeof(EntityAoS));
	}
	for (int i = 0; i < count; i++) {
		arr->entities[arr->count++] = (EntityAoS){
		    {(float)rand() / RAND_MAX, (float)rand() / RAND_MAX, (float)rand() / RAND_MAX},
		    {(float)rand() / RAND_MAX * 0.01f, (float)rand() / RAND_MAX * 0.01f, (float)rand() / RAND_MAX * 0.01f},
		    1};
	}
}

void aos_compact(ArrayAoS *arr) {
	int write_idx = 0;
	for (int i = 0; i < arr->count; i++) {
		if (arr->entities[i].alive) {
			arr->entities[write_idx++] = arr->entities[i];
		}
	}
	arr->count = write_idx;
}

void aos_free(ArrayAoS arr) {
	free(arr.entities);
}

/* ============ SoA ============ */

typedef struct {
	float *pos_x, *pos_y, *pos_z;
	float *vel_x, *vel_y, *vel_z;
	int *alive;
	int count;
	int capacity;
} ArraySoA;

ArraySoA soa_create(int capacity) {
	ArraySoA arr = {0};
	arr.capacity = capacity;
	arr.pos_x = malloc(capacity * sizeof(float));
	arr.pos_y = malloc(capacity * sizeof(float));
	arr.pos_z = malloc(capacity * sizeof(float));
	arr.vel_x = malloc(capacity * sizeof(float));
	arr.vel_y = malloc(capacity * sizeof(float));
	arr.vel_z = malloc(capacity * sizeof(float));
	arr.alive = malloc(capacity * sizeof(int));

	for (int i = 0; i < capacity; i++) {
		arr.pos_x[i] = (float)rand() / RAND_MAX;
		arr.pos_y[i] = (float)rand() / RAND_MAX;
		arr.pos_z[i] = (float)rand() / RAND_MAX;
		arr.vel_x[i] = (float)rand() / RAND_MAX * 0.01f;
		arr.vel_y[i] = (float)rand() / RAND_MAX * 0.01f;
		arr.vel_z[i] = (float)rand() / RAND_MAX * 0.01f;
		arr.alive[i] = 1;
	}
	arr.count = capacity;
	return arr;
}

void soa_update(ArraySoA *arr) {
	for (int i = 0; i < arr->count; i++) {
		if (arr->alive[i]) {
			arr->pos_x[i] += arr->vel_x[i];
			arr->pos_y[i] += arr->vel_y[i];
			arr->pos_z[i] += arr->vel_z[i];
		}
	}
}

void soa_delete_random(ArraySoA *arr, int count) {
	for (int i = 0; i < count; i++) {
		int idx = rand() % arr->count;
		arr->alive[idx] = 0;
	}
}

void soa_add_random(ArraySoA *arr, int count) {
	if (arr->count + count > arr->capacity) {
		arr->capacity = arr->count + count;
		arr->pos_x = realloc(arr->pos_x, arr->capacity * sizeof(float));
		arr->pos_y = realloc(arr->pos_y, arr->capacity * sizeof(float));
		arr->pos_z = realloc(arr->pos_z, arr->capacity * sizeof(float));
		arr->vel_x = realloc(arr->vel_x, arr->capacity * sizeof(float));
		arr->vel_y = realloc(arr->vel_y, arr->capacity * sizeof(float));
		arr->vel_z = realloc(arr->vel_z, arr->capacity * sizeof(float));
		arr->alive = realloc(arr->alive, arr->capacity * sizeof(int));
	}
	for (int i = 0; i < count; i++) {
		arr->pos_x[arr->count] = (float)rand() / RAND_MAX;
		arr->pos_y[arr->count] = (float)rand() / RAND_MAX;
		arr->pos_z[arr->count] = (float)rand() / RAND_MAX;
		arr->vel_x[arr->count] = (float)rand() / RAND_MAX * 0.01f;
		arr->vel_y[arr->count] = (float)rand() / RAND_MAX * 0.01f;
		arr->vel_z[arr->count] = (float)rand() / RAND_MAX * 0.01f;
		arr->alive[arr->count] = 1;
		arr->count++;
	}
}

void soa_compact(ArraySoA *arr) {
	int write_idx = 0;
	for (int i = 0; i < arr->count; i++) {
		if (arr->alive[i]) {
			arr->pos_x[write_idx] = arr->pos_x[i];
			arr->pos_y[write_idx] = arr->pos_y[i];
			arr->pos_z[write_idx] = arr->pos_z[i];
			arr->vel_x[write_idx] = arr->vel_x[i];
			arr->vel_y[write_idx] = arr->vel_y[i];
			arr->vel_z[write_idx] = arr->vel_z[i];
			write_idx++;
		}
	}
	arr->count = write_idx;
}

void soa_free(ArraySoA arr) {
	free(arr.pos_x);
	free(arr.pos_y);
	free(arr.pos_z);
	free(arr.vel_x);
	free(arr.vel_y);
	free(arr.vel_z);
	free(arr.alive);
}

/* ============ MIXED WORKLOAD ============ */

int main() {
	printf("Mixed Workload: 95%% array ops + 5%% lifecycle\n");
	printf("=========================================\n\n");

	/* AoS mixed */
	printf("AoS: 1000 iterations, %d entities\n", NUM_ENTITIES);
	ArrayAoS aos = aos_create(NUM_ENTITIES);

	Timer t1 = timer_start();
	for (int iter = 0; iter < ITERATIONS; iter++) {
		aos_update(&aos);

		if (iter % 20 == 0) {
			aos_add_random(&aos, 10);
			aos_delete_random(&aos, 5);
		}

		if (iter % 50 == 0) {
			aos_compact(&aos);
		}
	}
	long long time_aos = timer_end(t1);

	printf("  Time: %.2f ms (%.0f ns/op)\n", time_aos / 1e6, (double)time_aos / ITERATIONS);
	printf("  Final entities: %d / %d\n", aos.count, aos.capacity);
	printf("  Memory: %.2f KB\n\n", aos.capacity * sizeof(EntityAoS) / 1024.0);

	aos_free(aos);

	/* SoA mixed */
	printf("SoA: 1000 iterations, %d entities\n", NUM_ENTITIES);
	ArraySoA soa = soa_create(NUM_ENTITIES);

	Timer t2 = timer_start();
	for (int iter = 0; iter < ITERATIONS; iter++) {
		soa_update(&soa);

		if (iter % 20 == 0) {
			soa_add_random(&soa, 10);
			soa_delete_random(&soa, 5);
		}

		if (iter % 50 == 0) {
			soa_compact(&soa);
		}
	}
	long long time_soa = timer_end(t2);

	printf("  Time: %.2f ms (%.0f ns/op)\n", time_soa / 1e6, (double)time_soa / ITERATIONS);
	printf("  Final entities: %d / %d\n", soa.count, soa.capacity);
	printf("  Memory: %.2f KB\n\n", (soa.capacity * 6 * sizeof(float) + soa.capacity * sizeof(int)) / 1024.0);

	soa_free(soa);

	printf("Speedup: SoA is %.1f%% %s\n", (double)llabs(time_aos - time_soa) / time_aos * 100,
	       time_soa < time_aos ? "faster" : "slower");

	return 0;
}
