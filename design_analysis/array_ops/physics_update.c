#define _POSIX_C_SOURCE 200809L
#include <immintrin.h>
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

// Method 1: AoS (Array of Structs) - traditional
typedef struct {
	float x, y, z;
} Vec3;

typedef struct {
	Vec3 pos;
	Vec3 vel;
} EntityAoS;

EntityAoS *create_aos(int count) {
	EntityAoS *entities = malloc(count * sizeof(EntityAoS));
	for (int i = 0; i < count; i++) {
		entities[i].pos = (Vec3){(float)rand() / RAND_MAX, (float)rand() / RAND_MAX, (float)rand() / RAND_MAX};
		entities[i].vel = (Vec3){(float)rand() / RAND_MAX * 0.01f, (float)rand() / RAND_MAX * 0.01f,
		                         (float)rand() / RAND_MAX * 0.01f};
	}
	return entities;
}

void update_aos(EntityAoS *entities, int count) {
	for (int i = 0; i < count; i++) {
		entities[i].pos.x += entities[i].vel.x;
		entities[i].pos.y += entities[i].vel.y;
		entities[i].pos.z += entities[i].vel.z;
	}
}

// Method 2: SoA - flat arrays
typedef struct {
	float *pos_x, *pos_y, *pos_z;
	float *vel_x, *vel_y, *vel_z;
	int count;
} EntitySoAFlat;

EntitySoAFlat create_soa_flat(int count) {
	EntitySoAFlat soa = {0};
	soa.count = count;
	soa.pos_x = malloc(count * sizeof(float));
	soa.pos_y = malloc(count * sizeof(float));
	soa.pos_z = malloc(count * sizeof(float));
	soa.vel_x = malloc(count * sizeof(float));
	soa.vel_y = malloc(count * sizeof(float));
	soa.vel_z = malloc(count * sizeof(float));

	for (int i = 0; i < count; i++) {
		soa.pos_x[i] = (float)rand() / RAND_MAX;
		soa.pos_y[i] = (float)rand() / RAND_MAX;
		soa.pos_z[i] = (float)rand() / RAND_MAX;
		soa.vel_x[i] = (float)rand() / RAND_MAX * 0.01f;
		soa.vel_y[i] = (float)rand() / RAND_MAX * 0.01f;
		soa.vel_z[i] = (float)rand() / RAND_MAX * 0.01f;
	}
	return soa;
}

void free_soa_flat(EntitySoAFlat soa) {
	free(soa.pos_x);
	free(soa.pos_y);
	free(soa.pos_z);
	free(soa.vel_x);
	free(soa.vel_y);
	free(soa.vel_z);
}

void update_soa_flat(EntitySoAFlat soa) {
	for (int i = 0; i < soa.count; i++) {
		soa.pos_x[i] += soa.vel_x[i];
		soa.pos_y[i] += soa.vel_y[i];
		soa.pos_z[i] += soa.vel_z[i];
	}
}

// Method 2b: SoA flat with explicit AVX2
#pragma GCC target("avx2")
void update_soa_flat_avx2(EntitySoAFlat soa) {
	int i = 0;
	int count_vec = (soa.count / 8) * 8;

	for (; i < count_vec; i += 8) {
		__m256 px = _mm256_loadu_ps(&soa.pos_x[i]);
		__m256 vx = _mm256_loadu_ps(&soa.vel_x[i]);
		__m256 py = _mm256_loadu_ps(&soa.pos_y[i]);
		__m256 vy = _mm256_loadu_ps(&soa.vel_y[i]);
		__m256 pz = _mm256_loadu_ps(&soa.pos_z[i]);
		__m256 vz = _mm256_loadu_ps(&soa.vel_z[i]);

		_mm256_storeu_ps(&soa.pos_x[i], _mm256_add_ps(px, vx));
		_mm256_storeu_ps(&soa.pos_y[i], _mm256_add_ps(py, vy));
		_mm256_storeu_ps(&soa.pos_z[i], _mm256_add_ps(pz, vz));
	}

	for (; i < soa.count; i++) {
		soa.pos_x[i] += soa.vel_x[i];
		soa.pos_y[i] += soa.vel_y[i];
		soa.pos_z[i] += soa.vel_z[i];
	}
}
#pragma GCC reset_options

// Method 3: SoA with nested arrays
typedef struct {
	float *pos[3]; // pos[0]=x, pos[1]=y, pos[2]=z
	float *vel[3];
	int count;
} EntitySoANested;

EntitySoANested create_soa_nested(int count) {
	EntitySoANested soa = {0};
	soa.count = count;
	for (int axis = 0; axis < 3; axis++) {
		soa.pos[axis] = malloc(count * sizeof(float));
		soa.vel[axis] = malloc(count * sizeof(float));
	}

	for (int i = 0; i < count; i++) {
		soa.pos[0][i] = (float)rand() / RAND_MAX;
		soa.pos[1][i] = (float)rand() / RAND_MAX;
		soa.pos[2][i] = (float)rand() / RAND_MAX;
		soa.vel[0][i] = (float)rand() / RAND_MAX * 0.01f;
		soa.vel[1][i] = (float)rand() / RAND_MAX * 0.01f;
		soa.vel[2][i] = (float)rand() / RAND_MAX * 0.01f;
	}
	return soa;
}

void free_soa_nested(EntitySoANested soa) {
	for (int axis = 0; axis < 3; axis++) {
		free(soa.pos[axis]);
		free(soa.vel[axis]);
	}
}

void update_soa_nested(EntitySoANested soa) {
	for (int axis = 0; axis < 3; axis++) {
		for (int i = 0; i < soa.count; i++) {
			soa.pos[axis][i] += soa.vel[axis][i];
		}
	}
}

// Method 4: Interleaved (x y z x y z...)
typedef struct {
	float *pos; // [x0 y0 z0 x1 y1 z1 ...]
	float *vel;
	int count;
} EntityInterleaved;

EntityInterleaved create_interleaved(int count) {
	EntityInterleaved ent = {0};
	ent.count = count;
	ent.pos = malloc(count * 3 * sizeof(float));
	ent.vel = malloc(count * 3 * sizeof(float));

	for (int i = 0; i < count; i++) {
		for (int axis = 0; axis < 3; axis++) {
			ent.pos[i * 3 + axis] = (float)rand() / RAND_MAX;
			ent.vel[i * 3 + axis] = (float)rand() / RAND_MAX * 0.01f;
		}
	}
	return ent;
}

void free_interleaved(EntityInterleaved ent) {
	free(ent.pos);
	free(ent.vel);
}

void update_interleaved(EntityInterleaved ent) {
	for (int i = 0; i < ent.count; i++) {
		ent.pos[i * 3 + 0] += ent.vel[i * 3 + 0];
		ent.pos[i * 3 + 1] += ent.vel[i * 3 + 1];
		ent.pos[i * 3 + 2] += ent.vel[i * 3 + 2];
	}
}

// Method 5: AoSoA (batch-64 entities per struct)
#define BATCH_SIZE 64
typedef struct {
	Vec3 pos[BATCH_SIZE];
	Vec3 vel[BATCH_SIZE];
} EntityBatch;

typedef struct {
	EntityBatch *batches;
	int count;
	int num_batches;
} EntityAoSoA;

EntityAoSoA create_aosoa(int count) {
	EntityAoSoA soa = {0};
	soa.count = count;
	soa.num_batches = (count + BATCH_SIZE - 1) / BATCH_SIZE;
	soa.batches = malloc(soa.num_batches * sizeof(EntityBatch));

	for (int i = 0; i < count; i++) {
		int batch_idx = i / BATCH_SIZE;
		int local_idx = i % BATCH_SIZE;
		soa.batches[batch_idx].pos[local_idx] =
		    (Vec3){(float)rand() / RAND_MAX, (float)rand() / RAND_MAX, (float)rand() / RAND_MAX};
		soa.batches[batch_idx].vel[local_idx] = (Vec3){
		    (float)rand() / RAND_MAX * 0.01f, (float)rand() / RAND_MAX * 0.01f, (float)rand() / RAND_MAX * 0.01f};
	}
	return soa;
}

void free_aosoa(EntityAoSoA soa) {
	free(soa.batches);
}

void update_aosoa(EntityAoSoA soa) {
	for (int b = 0; b < soa.num_batches; b++) {
		for (int i = 0; i < BATCH_SIZE; i++) {
			soa.batches[b].pos[i].x += soa.batches[b].vel[i].x;
			soa.batches[b].pos[i].y += soa.batches[b].vel[i].y;
			soa.batches[b].pos[i].z += soa.batches[b].vel[i].z;
		}
	}
}

// Method 6: Packed floats (raw arrays, manual indexing)
typedef struct {
	float *data; // [pos_x0 pos_y0 pos_z0 vel_x0 vel_y0 vel_z0 pos_x1 ...]
	int count;
} EntityPacked;

EntityPacked create_packed(int count) {
	EntityPacked ent = {0};
	ent.count = count;
	ent.data = malloc(count * 6 * sizeof(float));

	for (int i = 0; i < count; i++) {
		int base = i * 6;
		ent.data[base + 0] = (float)rand() / RAND_MAX;
		ent.data[base + 1] = (float)rand() / RAND_MAX;
		ent.data[base + 2] = (float)rand() / RAND_MAX;
		ent.data[base + 3] = (float)rand() / RAND_MAX * 0.01f;
		ent.data[base + 4] = (float)rand() / RAND_MAX * 0.01f;
		ent.data[base + 5] = (float)rand() / RAND_MAX * 0.01f;
	}
	return ent;
}

void free_packed(EntityPacked ent) {
	free(ent.data);
}

void update_packed(EntityPacked ent) {
	for (int i = 0; i < ent.count; i++) {
		int base = i * 6;
		ent.data[base + 0] += ent.data[base + 3];
		ent.data[base + 1] += ent.data[base + 4];
		ent.data[base + 2] += ent.data[base + 5];
	}
}

int main() {
	printf("Physics Update Benchmark - %d entities, %d iterations\n\n", NUM_ENTITIES, ITERATIONS);

	// Method 1: AoS
	printf("Method 1: AoS (Array of Structs)\n");
	EntityAoS *aos = create_aos(NUM_ENTITIES);
	Timer t1 = timer_start();
	for (int iter = 0; iter < ITERATIONS; iter++) {
		update_aos(aos, NUM_ENTITIES);
	}
	long long time1 = timer_end(t1);
	printf("  Time: %.2f ms (%.0f ns/op)\n", time1 / 1e6, (double)time1 / ITERATIONS);
	printf("  Memory: %.2f KB\n\n", NUM_ENTITIES * sizeof(EntityAoS) / 1024.0);
	free(aos);

	// Method 2: SoA Flat
	printf("Method 2: SoA (flat arrays: pos_x, pos_y, pos_z, vel_x, vel_y, vel_z)\n");
	EntitySoAFlat soa_flat = create_soa_flat(NUM_ENTITIES);
	Timer t2 = timer_start();
	for (int iter = 0; iter < ITERATIONS; iter++) {
		update_soa_flat(soa_flat);
	}
	long long time2 = timer_end(t2);
	printf("  Time: %.2f ms (%.0f ns/op)\n", time2 / 1e6, (double)time2 / ITERATIONS);
	printf("  Memory: %.2f KB\n\n", NUM_ENTITIES * 6 * sizeof(float) / 1024.0);
	free_soa_flat(soa_flat);

	// Method 2b: SoA Flat with AVX2
	printf("Method 2b: SoA (flat) with explicit AVX2 SIMD\n");
	EntitySoAFlat soa_flat_avx2 = create_soa_flat(NUM_ENTITIES);
	Timer t2b = timer_start();
	for (int iter = 0; iter < ITERATIONS; iter++) {
		update_soa_flat_avx2(soa_flat_avx2);
	}
	long long time2b = timer_end(t2b);
	printf("  Time: %.2f ms (%.0f ns/op)\n", time2b / 1e6, (double)time2b / ITERATIONS);
	printf("  Memory: %.2f KB\n\n", NUM_ENTITIES * 6 * sizeof(float) / 1024.0);
	free_soa_flat(soa_flat_avx2);

	// Method 3: SoA Nested
	printf("Method 3: SoA (nested: pos[3], vel[3])\n");
	EntitySoANested soa_nested = create_soa_nested(NUM_ENTITIES);
	Timer t3 = timer_start();
	for (int iter = 0; iter < ITERATIONS; iter++) {
		update_soa_nested(soa_nested);
	}
	long long time3 = timer_end(t3);
	printf("  Time: %.2f ms (%.0f ns/op)\n", time3 / 1e6, (double)time3 / ITERATIONS);
	printf("  Memory: %.2f KB\n\n", NUM_ENTITIES * 6 * sizeof(float) / 1024.0);
	free_soa_nested(soa_nested);

	// Method 4: Interleaved
	printf("Method 4: Interleaved (x y z x y z...)\n");
	EntityInterleaved interleaved = create_interleaved(NUM_ENTITIES);
	Timer t4 = timer_start();
	for (int iter = 0; iter < ITERATIONS; iter++) {
		update_interleaved(interleaved);
	}
	long long time4 = timer_end(t4);
	printf("  Time: %.2f ms (%.0f ns/op)\n", time4 / 1e6, (double)time4 / ITERATIONS);
	printf("  Memory: %.2f KB\n\n", NUM_ENTITIES * 6 * sizeof(float) / 1024.0);
	free_interleaved(interleaved);

	// Method 5: AoSoA
	printf("Method 5: AoSoA (batch size %d)\n", BATCH_SIZE);
	EntityAoSoA aosoa = create_aosoa(NUM_ENTITIES);
	Timer t5 = timer_start();
	for (int iter = 0; iter < ITERATIONS; iter++) {
		update_aosoa(aosoa);
	}
	long long time5 = timer_end(t5);
	printf("  Time: %.2f ms (%.0f ns/op)\n", time5 / 1e6, (double)time5 / ITERATIONS);
	printf("  Memory: %.2f KB\n\n", aosoa.num_batches * sizeof(EntityBatch) / 1024.0);
	free_aosoa(aosoa);

	// Method 6: Packed
	printf("Method 6: Packed (manual indexing: [px py pz vx vy vz ...])\n");
	EntityPacked packed = create_packed(NUM_ENTITIES);
	Timer t6 = timer_start();
	for (int iter = 0; iter < ITERATIONS; iter++) {
		update_packed(packed);
	}
	long long time6 = timer_end(t6);
	printf("  Time: %.2f ms (%.0f ns/op)\n", time6 / 1e6, (double)time6 / ITERATIONS);
	printf("  Memory: %.2f KB\n\n", NUM_ENTITIES * 6 * sizeof(float) / 1024.0);
	free_packed(packed);

	// Summary
	printf("Summary (lower is better):\n");
	printf("  Method 1 (AoS):           %.0f ns/op (baseline)\n", (double)time1 / ITERATIONS);
	printf("  Method 2 (SoA flat):      %.0f ns/op (%.1f%% %s)\n", (double)time2 / ITERATIONS,
	       (double)llabs(time1 - time2) / time1 * 100, time2 < time1 ? "faster" : "slower");
	printf("  Method 2b (SoA+AVX2):     %.0f ns/op (%.1f%% %s)\n", (double)time2b / ITERATIONS,
	       (double)llabs(time1 - time2b) / time1 * 100, time2b < time1 ? "faster" : "slower");
	printf("  Method 3 (SoA nested):    %.0f ns/op (%.1f%% %s)\n", (double)time3 / ITERATIONS,
	       (double)llabs(time1 - time3) / time1 * 100, time3 < time1 ? "faster" : "slower");
	printf("  Method 4 (Interleaved):   %.0f ns/op (%.1f%% %s)\n", (double)time4 / ITERATIONS,
	       (double)llabs(time1 - time4) / time1 * 100, time4 < time1 ? "faster" : "slower");
	printf("  Method 5 (AoSoA batch):   %.0f ns/op (%.1f%% %s)\n", (double)time5 / ITERATIONS,
	       (double)llabs(time1 - time5) / time1 * 100, time5 < time1 ? "faster" : "slower");
	printf("  Method 6 (Packed):        %.0f ns/op (%.1f%% %s)\n", (double)time6 / ITERATIONS,
	       (double)llabs(time1 - time6) / time1 * 100, time6 < time1 ? "faster" : "slower");

	// JSON output
	FILE *out = fopen("design_analysis/array_ops/results/physics_update_results.json", "w");
	fprintf(out, "{\n");
	fprintf(out, "  \"benchmark\": \"physics_update\",\n");
	fprintf(out, "  \"num_entities\": %d,\n", NUM_ENTITIES);
	fprintf(out, "  \"iterations\": %d,\n", ITERATIONS);
	fprintf(out, "  \"operation\": \"pos += vel\",\n");
	fprintf(out, "  \"methods\": [\n");
	fprintf(out, "    {\"name\": \"aos\", \"ns_per_op\": %.0f},\n", (double)time1 / ITERATIONS);
	fprintf(out, "    {\"name\": \"soa_flat\", \"ns_per_op\": %.0f},\n", (double)time2 / ITERATIONS);
	fprintf(out, "    {\"name\": \"soa_flat_avx2\", \"ns_per_op\": %.0f},\n", (double)time2b / ITERATIONS);
	fprintf(out, "    {\"name\": \"soa_nested\", \"ns_per_op\": %.0f},\n", (double)time3 / ITERATIONS);
	fprintf(out, "    {\"name\": \"interleaved\", \"ns_per_op\": %.0f},\n", (double)time4 / ITERATIONS);
	fprintf(out, "    {\"name\": \"aosoa_batch64\", \"ns_per_op\": %.0f},\n", (double)time5 / ITERATIONS);
	fprintf(out, "    {\"name\": \"packed\", \"ns_per_op\": %.0f}\n", (double)time6 / ITERATIONS);
	fprintf(out, "  ]\n");
	fprintf(out, "}\n");
	fclose(out);

	printf("\nResults saved to design_analysis/array_ops/results/physics_update_results.json\n");

	return 0;
}
