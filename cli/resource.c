/* readlink (exe-relative discovery) is POSIX; expose it under -std=c99. */
#define _POSIX_C_SOURCE 200809L
#include "resource.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef ARCHE_CORE_DIR
#define ARCHE_CORE_DIR "core"
#endif
#ifndef ARCHE_STDLIB_DIR
#define ARCHE_STDLIB_DIR "stdlib"
#endif
#ifndef ARCHE_RUNTIME_DIR
#define ARCHE_RUNTIME_DIR "build/runtime"
#endif
#ifndef ARCHE_EXPLAIN_DIR
#define ARCHE_EXPLAIN_DIR "docs/explain"
#endif

/* Per-resource: the install-layout subdir, the env-override variable, and the compiled-in default. */
static const char *res_subdir(ArcheResource w) {
	switch (w) {
	case ARCHE_RES_CORE:
		return "core";
	case ARCHE_RES_STDLIB:
		return "stdlib";
	case ARCHE_RES_RUNTIME:
		return "runtime";
	case ARCHE_RES_EXPLAIN:
		return "explain";
	}
	return "";
}
static const char *res_env(ArcheResource w) {
	switch (w) {
	case ARCHE_RES_CORE:
		return "ARCHE_CORE_DIR";
	case ARCHE_RES_STDLIB:
		return "ARCHE_STDLIB_DIR";
	case ARCHE_RES_RUNTIME:
		return "ARCHE_RUNTIME_DIR";
	case ARCHE_RES_EXPLAIN:
		return "ARCHE_EXPLAIN_DIR";
	}
	return "";
}
static const char *res_default(ArcheResource w) {
	switch (w) {
	case ARCHE_RES_CORE:
		return ARCHE_CORE_DIR;
	case ARCHE_RES_STDLIB:
		return ARCHE_STDLIB_DIR;
	case ARCHE_RES_RUNTIME:
		return ARCHE_RUNTIME_DIR;
	case ARCHE_RES_EXPLAIN:
		return ARCHE_EXPLAIN_DIR;
	}
	return "";
}

static int dir_exists(const char *p) {
	struct stat st;
	return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

const char *arche_resource_dir(ArcheResource which) {
	static char cache[4][1024];
	static int cached[4];
	int i = (int)which;
	if (cached[i])
		return cache[i];

	/* 1. per-resource env override */
	const char *ov = getenv(res_env(which));
	if (ov && *ov) {
		snprintf(cache[i], sizeof(cache[i]), "%s", ov);
		cached[i] = 1;
		return cache[i];
	}

	/* 2. explicit sysroot */
	const char *sysroot = getenv("ARCHE_SYSROOT");
	if (sysroot && *sysroot) {
		snprintf(cache[i], sizeof(cache[i]), "%s/lib/arche/%s", sysroot, res_subdir(which));
		cached[i] = 1;
		return cache[i];
	}

	/* 3. exe-relative install layout: <bindir>/../lib/arche/<subdir>, if present */
	char exe[1024];
	ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
	if (n > 0) {
		exe[n] = '\0';
		char *slash = strrchr(exe, '/');
		if (slash) {
			*slash = '\0'; /* exe now = bindir */
			char cand[1024];
			snprintf(cand, sizeof(cand), "%s/../lib/arche/%s", exe, res_subdir(which));
			if (dir_exists(cand)) {
				snprintf(cache[i], sizeof(cache[i]), "%s", cand);
				cached[i] = 1;
				return cache[i];
			}
		}
	}

	/* 4. compiled-in default (in-tree build paths) */
	snprintf(cache[i], sizeof(cache[i]), "%s", res_default(which));
	cached[i] = 1;
	return cache[i];
}
