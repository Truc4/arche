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

/* handle + out param: dereferences the handle (would crash on a raw, unwrapped
 * handle int) and also fills an out buffer. Returns the resource value. */
int resource_fill(int *r, char *buf, int n) {
	int v = *r; /* <-- requires the handle to be unwrapped to a real pointer */
	int k = 0;
	if (n > 1) {
		buf[0] = (char)('0' + (v % 10));
		k = 1;
	}
	buf[k] = 0;
	return v;
}
