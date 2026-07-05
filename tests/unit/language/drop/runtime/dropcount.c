/* Toy handle library for the "opaque stored into a pool column" drop test. Tracks how many handles
 * are currently live, so a premature @drop (close) is observable as a drop in the live count. */
#include <stdlib.h>

static int g_live = 0;

void *obj_open(void) {
	g_live++;
	return malloc(8);
}

void obj_close(void *h) {
	g_live--;
	free(h);
}

int obj_live(void) {
	return g_live;
}
