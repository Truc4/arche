#include "cli.h"
#include <stdio.h>

#ifndef ARCHE_VERSION
#define ARCHE_VERSION "0.0.0-dev"
#endif

const char *arche_version_string(void) {
	return ARCHE_VERSION;
}

int version_run(int argc, char **argv, const GlobalOpts *g) {
	(void)argc;
	(void)argv;
	(void)g;
	printf("arche %s\n", ARCHE_VERSION);
	return ARCHE_OK;
}
