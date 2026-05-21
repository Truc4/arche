/* Toy "OS resource" library for extern_type end-to-end tests.
 * Each "resource" is just an int counter in a fixed pool. */
#include <stdio.h>
#include <stdlib.h>

#define POOL_CAP 8
static int pool[POOL_CAP];
static int in_use[POOL_CAP];

int *resource_open(int initial) {
	for (int i = 0; i < POOL_CAP; i++) {
		if (!in_use[i]) {
			in_use[i] = 1;
			pool[i] = initial;
			return &pool[i];
		}
	}
	return NULL; /* triggers null-handle path */
}

int resource_get(int *r) {
	return *r;
}

void resource_set(int *r, int v) {
	*r = v;
}

void resource_close(int *r) {
	for (int i = 0; i < POOL_CAP; i++) {
		if (&pool[i] == r) {
			in_use[i] = 0;
			return;
		}
	}
}
