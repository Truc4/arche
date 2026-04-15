#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NUM_ENTITIES 10000
#define ITERATIONS 100
#define ADD_OPS_PER_ITER 500    /* entities added per iteration */
#define DELETE_OPS_PER_ITER 400 /* entities deleted per iteration */
#define MOD_OPS_PER_ITER 200    /* row modifications per iteration */

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

/* ============ AoS LIFECYCLE ============ */

typedef struct {
	float x, y, z;
} Vec3;

typedef struct {
	Vec3 pos;
	Vec3 vel;
	int alive; /* flag for "deleted" */
} EntityAoS;

typedef struct {
	EntityAoS *entities;
	int count;
	int capacity;
} ArrayAoS;

ArrayAoS aos_create(int initial_capacity) {
	ArrayAoS arr = {0};
	arr.capacity = initial_capacity;
	arr.entities = malloc(initial_capacity * sizeof(EntityAoS));
	arr.count = 0;
	return arr;
}

void aos_add(ArrayAoS *arr, Vec3 pos, Vec3 vel) {
	if (arr->count >= arr->capacity) {
		arr->capacity *= 2;
		arr->entities = realloc(arr->entities, arr->capacity * sizeof(EntityAoS));
	}
	arr->entities[arr->count] = (EntityAoS){pos, vel, 1};
	arr->count++;
}

void aos_delete(ArrayAoS *arr, int idx) {
	if (idx < arr->count) {
		arr->entities[idx].alive = 0;
	}
}

void aos_modify(ArrayAoS *arr, int idx, Vec3 new_pos) {
	if (idx < arr->count && arr->entities[idx].alive) {
		arr->entities[idx].pos = new_pos;
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

/* ============ SoA LIFECYCLE ============ */

typedef struct {
	float *pos_x, *pos_y, *pos_z;
	float *vel_x, *vel_y, *vel_z;
	int *alive;
	int count;
	int capacity;
} ArraySoA;

ArraySoA soa_create(int initial_capacity) {
	ArraySoA arr = {0};
	arr.capacity = initial_capacity;
	arr.pos_x = malloc(initial_capacity * sizeof(float));
	arr.pos_y = malloc(initial_capacity * sizeof(float));
	arr.pos_z = malloc(initial_capacity * sizeof(float));
	arr.vel_x = malloc(initial_capacity * sizeof(float));
	arr.vel_y = malloc(initial_capacity * sizeof(float));
	arr.vel_z = malloc(initial_capacity * sizeof(float));
	arr.alive = malloc(initial_capacity * sizeof(int));
	arr.count = 0;
	return arr;
}

void soa_add(ArraySoA *arr, Vec3 pos, Vec3 vel) {
	if (arr->count >= arr->capacity) {
		arr->capacity *= 2;
		arr->pos_x = realloc(arr->pos_x, arr->capacity * sizeof(float));
		arr->pos_y = realloc(arr->pos_y, arr->capacity * sizeof(float));
		arr->pos_z = realloc(arr->pos_z, arr->capacity * sizeof(float));
		arr->vel_x = realloc(arr->vel_x, arr->capacity * sizeof(float));
		arr->vel_y = realloc(arr->vel_y, arr->capacity * sizeof(float));
		arr->vel_z = realloc(arr->vel_z, arr->capacity * sizeof(float));
		arr->alive = realloc(arr->alive, arr->capacity * sizeof(int));
	}
	arr->pos_x[arr->count] = pos.x;
	arr->pos_y[arr->count] = pos.y;
	arr->pos_z[arr->count] = pos.z;
	arr->vel_x[arr->count] = vel.x;
	arr->vel_y[arr->count] = vel.y;
	arr->vel_z[arr->count] = vel.z;
	arr->alive[arr->count] = 1;
	arr->count++;
}

void soa_delete(ArraySoA *arr, int idx) {
	if (idx < arr->count) {
		arr->alive[idx] = 0;
	}
}

void soa_modify(ArraySoA *arr, int idx, Vec3 new_pos) {
	if (idx < arr->count && arr->alive[idx]) {
		arr->pos_x[idx] = new_pos.x;
		arr->pos_y[idx] = new_pos.y;
		arr->pos_z[idx] = new_pos.z;
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

/* ============ BENCHMARKS ============ */

void bench_aos_lifecycle() {
	ArrayAoS arr = aos_create(NUM_ENTITIES);

	/* Initialize with some entities */
	for (int i = 0; i < NUM_ENTITIES / 2; i++) {
		aos_add(&arr, (Vec3){1.0f, 2.0f, 3.0f}, (Vec3){0.01f, 0.01f, 0.01f});
	}

	Timer t = timer_start();
	for (int iter = 0; iter < ITERATIONS; iter++) {
		/* Add entities */
		for (int i = 0; i < ADD_OPS_PER_ITER; i++) {
			aos_add(&arr, (Vec3){(float)rand() / RAND_MAX, (float)rand() / RAND_MAX, (float)rand() / RAND_MAX},
			        (Vec3){0.01f, 0.01f, 0.01f});
		}

		/* Delete some (sparse deletes) */
		for (int i = 0; i < DELETE_OPS_PER_ITER; i++) {
			int idx = rand() % arr.count;
			aos_delete(&arr, idx);
		}

		/* Modify some */
		for (int i = 0; i < MOD_OPS_PER_ITER; i++) {
			int idx = rand() % arr.count;
			aos_modify(&arr, idx, (Vec3){(float)rand() / RAND_MAX, (float)rand() / RAND_MAX, (float)rand() / RAND_MAX});
		}

		/* Periodic compact */
		if (iter % 10 == 0) {
			aos_compact(&arr);
		}
	}
	long long time_aos = timer_end(t);

	printf("AoS Lifecycle:\n");
	printf("  Time: %.2f ms (%.2f μs/op)\n", time_aos / 1e6, (double)time_aos / (ITERATIONS * 1000.0));
	printf("  Final entities: %d (capacity: %d)\n", arr.count, arr.capacity);
	printf("  Memory: %.2f KB\n\n", arr.capacity * sizeof(EntityAoS) / 1024.0);

	aos_free(arr);
}

void bench_soa_lifecycle() {
	ArraySoA arr = soa_create(NUM_ENTITIES);

	/* Initialize */
	for (int i = 0; i < NUM_ENTITIES / 2; i++) {
		soa_add(&arr, (Vec3){1.0f, 2.0f, 3.0f}, (Vec3){0.01f, 0.01f, 0.01f});
	}

	Timer t = timer_start();
	for (int iter = 0; iter < ITERATIONS; iter++) {
		/* Add entities */
		for (int i = 0; i < ADD_OPS_PER_ITER; i++) {
			soa_add(&arr, (Vec3){(float)rand() / RAND_MAX, (float)rand() / RAND_MAX, (float)rand() / RAND_MAX},
			        (Vec3){0.01f, 0.01f, 0.01f});
		}

		/* Delete some */
		for (int i = 0; i < DELETE_OPS_PER_ITER; i++) {
			int idx = rand() % arr.count;
			soa_delete(&arr, idx);
		}

		/* Modify some */
		for (int i = 0; i < MOD_OPS_PER_ITER; i++) {
			int idx = rand() % arr.count;
			soa_modify(&arr, idx, (Vec3){(float)rand() / RAND_MAX, (float)rand() / RAND_MAX, (float)rand() / RAND_MAX});
		}

		/* Periodic compact */
		if (iter % 10 == 0) {
			soa_compact(&arr);
		}
	}
	long long time_soa = timer_end(t);

	printf("SoA Lifecycle:\n");
	printf("  Time: %.2f ms (%.2f μs/op)\n", time_soa / 1e6, (double)time_soa / (ITERATIONS * 1000.0));
	printf("  Final entities: %d (capacity: %d)\n", arr.count, arr.capacity);
	printf("  Memory per entity: %.2f bytes\n",
	       (arr.capacity * 7 * sizeof(float) + arr.capacity * sizeof(int)) / (float)arr.capacity);
	printf("  Total memory: %.2f KB\n\n", (arr.capacity * 6 * sizeof(float) + arr.capacity * sizeof(int)) / 1024.0);

	soa_free(arr);
}

/* Stress: maximize deletions to test fragmentation */
void bench_aos_heavy_delete() {
	ArrayAoS arr = aos_create(NUM_ENTITIES);

	for (int i = 0; i < NUM_ENTITIES / 2; i++) {
		aos_add(&arr, (Vec3){1.0f, 2.0f, 3.0f}, (Vec3){0.01f, 0.01f, 0.01f});
	}

	Timer t = timer_start();
	for (int iter = 0; iter < ITERATIONS; iter++) {
		/* Heavy deletes (80% of ops) */
		for (int i = 0; i < DELETE_OPS_PER_ITER * 2; i++) {
			if (arr.count > 100) {
				int idx = rand() % arr.count;
				aos_delete(&arr, idx);
			}
		}

		/* Fewer adds to maintain steady state */
		for (int i = 0; i < ADD_OPS_PER_ITER / 2; i++) {
			aos_add(&arr, (Vec3){(float)rand() / RAND_MAX, (float)rand() / RAND_MAX, (float)rand() / RAND_MAX},
			        (Vec3){0.01f, 0.01f, 0.01f});
		}

		if (iter % 10 == 0) {
			aos_compact(&arr);
		}
	}
	long long time_aos = timer_end(t);

	printf("AoS Heavy Delete (fragmentation stress):\n");
	printf("  Time: %.2f ms (%.2f μs/op)\n", time_aos / 1e6, (double)time_aos / (ITERATIONS * 1000.0));
	printf("  Final entities: %d\n\n", arr.count);

	aos_free(arr);
}

void bench_soa_heavy_delete() {
	ArraySoA arr = soa_create(NUM_ENTITIES);

	for (int i = 0; i < NUM_ENTITIES / 2; i++) {
		soa_add(&arr, (Vec3){1.0f, 2.0f, 3.0f}, (Vec3){0.01f, 0.01f, 0.01f});
	}

	Timer t = timer_start();
	for (int iter = 0; iter < ITERATIONS; iter++) {
		/* Heavy deletes */
		for (int i = 0; i < DELETE_OPS_PER_ITER * 2; i++) {
			if (arr.count > 100) {
				int idx = rand() % arr.count;
				soa_delete(&arr, idx);
			}
		}

		/* Fewer adds */
		for (int i = 0; i < ADD_OPS_PER_ITER / 2; i++) {
			soa_add(&arr, (Vec3){(float)rand() / RAND_MAX, (float)rand() / RAND_MAX, (float)rand() / RAND_MAX},
			        (Vec3){0.01f, 0.01f, 0.01f});
		}

		if (iter % 10 == 0) {
			soa_compact(&arr);
		}
	}
	long long time_soa = timer_end(t);

	printf("SoA Heavy Delete (fragmentation stress):\n");
	printf("  Time: %.2f ms (%.2f μs/op)\n", time_soa / 1e6, (double)time_soa / (ITERATIONS * 1000.0));
	printf("  Final entities: %d\n\n", arr.count);

	soa_free(arr);
}

int main() {
	printf("Lifecycle Operations Benchmark\n");
	printf("==============================\n\n");

	printf("Baseline Lifecycle (500 adds, 400 deletes, 200 modifies per iteration, %d iterations)\n\n", ITERATIONS);

	bench_aos_lifecycle();
	bench_soa_lifecycle();

	printf("\nFragmentation Stress (2x deletes, sparse marking)\n\n");

	bench_aos_heavy_delete();
	bench_soa_heavy_delete();

	return 0;
}
