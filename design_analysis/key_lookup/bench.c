/* Key-lookup strategy benchmark — settles the design argument with numbers.
 *
 * Four strategies for "given a key, find its value", over the same synthetic
 * route-like string key set, swept across N routes:
 *
 *   1. SCAN   — linear scan + strcmp           (the radix tree's per-node behavior)
 *   2. HASH   — open-addressing string hash     ("keep string keys")
 *   3. INTERN — hash the incoming string -> id, then value[id]   (DOD intern + array,
 *               with a STRING query: shows interning still pays the input hash)
 *   4. DENSE  — value[id], id already known      (ECS sparse-set ceiling, no hash)
 *
 * Build: cc -O2 -o bench bench.c
 * Run:   ./bench
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define KEYLEN 24
#define QUERIES 10000000

static uint64_t fnv1a(const char *s) {
	uint64_t h = 1469598103934665603ULL;
	for (; *s; s++) {
		h ^= (uint8_t)*s;
		h *= 1099511628211ULL;
	}
	return h;
}

static double now_ns(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec * 1e9 + (double)t.tv_nsec;
}

/* deterministic PRNG so runs are comparable */
static uint64_t rng_state = 88172645463325252ULL;
static uint64_t xrand(void) {
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 7;
	rng_state ^= rng_state << 17;
	return rng_state;
}

static void gen_key(char *out, int seed) {
	/* path-segment-ish: "/aXX/bYY/..." length varies a bit */
	static const char alpha[] = "abcdefghijklmnopqrstuvwxyz0123456789";
	int n = 6 + (seed % 14); /* 6..19 chars */
	int j = 0;
	out[j++] = '/';
	uint64_t s = (uint64_t)seed * 2654435761u + 12345u;
	for (; j < n; j++) {
		s = s * 6364136223846793005ULL + 1442695040888963407ULL;
		out[j] = alpha[(s >> 33) % (sizeof(alpha) - 1)];
	}
	out[j] = 0;
}

int main(void) {
	int Ns[] = {16, 100, 1000};
	int nN = sizeof(Ns) / sizeof(Ns[0]);

	printf("ns per lookup  (%d queries each, ~75%% hit / 25%% miss for string strategies)\n", QUERIES);
	printf("%-6s %10s %10s %10s %10s\n", "N", "SCAN", "HASH", "INTERN", "DENSE(id)");

	for (int ni = 0; ni < nN; ni++) {
		int N = Ns[ni];

		/* --- build the key set + values --- */
		char (*keys)[KEYLEN] = malloc((size_t)N * KEYLEN);
		int *vals = malloc((size_t)N * sizeof(int));
		for (int i = 0; i < N; i++) {
			gen_key(keys[i], i);
			vals[i] = i * 7 + 1;
		}

		/* --- hash table (open addressing), used by HASH and as INTERN's index --- */
		int cap = 1;
		while (cap < N * 2)
			cap <<= 1; /* power of two, load factor < 0.5 */
		uint64_t mask = (uint64_t)cap - 1;
		int *slot = malloc((size_t)cap * sizeof(int)); /* slot[b] = key index + 1, 0 = empty */
		memset(slot, 0, (size_t)cap * sizeof(int));
		for (int i = 0; i < N; i++) {
			uint64_t b = fnv1a(keys[i]) & mask;
			while (slot[b])
				b = (b + 1) & mask;
			slot[b] = i + 1;
		}

		/* DENSE: value indexed directly by dense id (== key index here) */
		int *dense = malloc((size_t)N * sizeof(int));
		for (int i = 0; i < N; i++)
			dense[i] = vals[i];

		/* --- build the query workload: 75% real keys (hits), 25% fabricated misses --- */
		char (*queries)[KEYLEN] = malloc((size_t)QUERIES * KEYLEN);
		int *q_expect = malloc((size_t)QUERIES * sizeof(int));
		int *q_id = malloc((size_t)QUERIES * sizeof(int)); /* for DENSE */
		rng_state = 88172645463325252ULL;
		for (int q = 0; q < QUERIES; q++) {
			int idx = (int)(xrand() % N);
			q_id[q] = idx;
			if (xrand() % 4 == 0) { /* 25% miss: mangle into a non-key */
				gen_key(queries[q], idx);
				size_t L = strlen(queries[q]);
				queries[q][L] = '~'; /* '~' not in alpha -> guaranteed miss */
				queries[q][L + 1] = 0;
				q_expect[q] = -1;
			} else {
				memcpy(queries[q], keys[idx], KEYLEN);
				q_expect[q] = vals[idx];
			}
		}

		volatile long sink = 0;
		double t0, t1;

		/* 1. SCAN */
		t0 = now_ns();
		for (int q = 0; q < QUERIES; q++) {
			int found = -1;
			for (int i = 0; i < N; i++) {
				if (strcmp(queries[q], keys[i]) == 0) {
					found = vals[i];
					break;
				}
			}
			sink += found;
		}
		t1 = now_ns();
		double scan = (t1 - t0) / QUERIES;

		/* 2. HASH (string key) */
		t0 = now_ns();
		for (int q = 0; q < QUERIES; q++) {
			uint64_t b = fnv1a(queries[q]) & mask;
			int found = -1;
			while (slot[b]) {
				int ki = slot[b] - 1;
				if (strcmp(keys[ki], queries[q]) == 0) {
					found = vals[ki];
					break;
				}
				b = (b + 1) & mask;
			}
			sink += found;
		}
		t1 = now_ns();
		double hash = (t1 - t0) / QUERIES;

		/* 3. INTERN: hash the incoming string -> id (same probe), then dense[id].
		 *    This is the DOD "intern + array" with a STRING query — the input hash is
		 *    unavoidable, so it should track HASH (plus one array indirection). */
		t0 = now_ns();
		for (int q = 0; q < QUERIES; q++) {
			uint64_t b = fnv1a(queries[q]) & mask;
			int found = -1;
			while (slot[b]) {
				int ki = slot[b] - 1;
				if (strcmp(keys[ki], queries[q]) == 0) {
					found = dense[ki];
					break;
				}
				b = (b + 1) & mask;
			}
			sink += found;
		}
		t1 = now_ns();
		double intern = (t1 - t0) / QUERIES;

		/* 4. DENSE: id already in hand (ECS) — pure array read, no hash, no compare. */
		t0 = now_ns();
		for (int q = 0; q < QUERIES; q++) {
			sink += dense[q_id[q]];
		}
		t1 = now_ns();
		double denseT = (t1 - t0) / QUERIES;

		printf("%-6d %10.2f %10.2f %10.2f %10.2f\n", N, scan, hash, intern, denseT);

		if (sink == 0x7fffffff)
			printf(" "); /* keep sink live */
		free(keys);
		free(vals);
		free(slot);
		free(dense);
		free(queries);
		free(q_expect);
		free(q_id);
	}

	printf("\nDENSE(id) assumes the key is ALREADY a dense int (no string in hand).\n");
	printf("INTERN takes a STRING query, so it pays the same input hash as HASH.\n");
	return 0;
}
