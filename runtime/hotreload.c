/* Hot-reload dispatch runtime — linked into a host built with `arche run` (the dev path). The host owns
 * the process + state; each imported DEVICE is compiled to its own `.so` (a per-unit object becomes a
 * shared library). At startup the codegen-emitted host calls `arche_hot_register(unit, path)` for each
 * device; every driver→device call is lowered to `arche_hot_resolve(unit, "sym")` + an indirect call.
 * When a device's `.so` changes on disk (the dev rebuilt it), the next resolve transparently reloads it
 * — the running host never restarts, and state (the driver's pools) is untouched because it lives in
 * the host, not the device. In a release `arche build` this runtime is NOT linked and the calls are
 * ordinary direct calls (the indirection is compiled out).
 *
 * This file is C, not arche, because arche has no runtime function pointers by design: the dlopen/
 * dlsym/indirect-call machinery is confined here, an internal implementation detail of the dev loop. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#ifndef HOT_MAX_UNITS
#define HOT_MAX_UNITS 64
#endif

typedef struct {
	char path[1024]; /* the device's .so on disk (rebuilt by the dev to trigger a reload) */
	void *handle;    /* current dlopen handle, or NULL until first load */
	long mtime;      /* st_mtime of `path` when `handle` was loaded */
	unsigned gen;    /* bumped each reload → unique temp-copy name (dodge dlopen's path/inode cache) */
	int active;      /* registered? */
} HotUnit;

static HotUnit g_units[HOT_MAX_UNITS];

/* Record a device unit's `.so` path. Codegen emits one call per imported device at host startup. */
void arche_hot_register(int unit, const char *path) {
	if (unit < 0 || unit >= HOT_MAX_UNITS || !path)
		return;
	snprintf(g_units[unit].path, sizeof(g_units[unit].path), "%s", path);
	g_units[unit].handle = NULL;
	g_units[unit].mtime = 0;
	g_units[unit].gen = 0;
	g_units[unit].active = 1;
}

/* (Re)load the unit's `.so` if it is unloaded or its mtime changed. Loads a fresh versioned COPY each
 * time: dlopen caches by realpath, so reopening the same path after dlclose can hand back the stale
 * image — copying to `<path>.hot.<gen>` forces a genuinely new load (the standard hot-reload trick). */
static void ensure_loaded(HotUnit *u) {
	struct stat st;
	if (stat(u->path, &st) != 0)
		return; /* not built yet — keep whatever we have (possibly NULL) */
	long m = (long)st.st_mtime;
	if (u->handle && m == u->mtime)
		return; /* current */

	char tmp[1100];
	snprintf(tmp, sizeof(tmp), "%s.hot.%u", u->path, u->gen + 1);
	FILE *in = fopen(u->path, "rb");
	if (!in)
		return;
	FILE *out = fopen(tmp, "wb");
	if (!out) {
		fclose(in);
		return;
	}
	char buf[65536];
	size_t r;
	while ((r = fread(buf, 1, sizeof(buf), in)) > 0)
		fwrite(buf, 1, r, out);
	fclose(in);
	fclose(out);

	void *h = dlopen(tmp, RTLD_NOW | RTLD_LOCAL);
	if (!h) {
		fprintf(stderr, "hot-reload: dlopen %s failed: %s\n", tmp, dlerror());
		return; /* keep the old handle live so the host keeps running on the last-good code */
	}
	if (u->handle)
		dlclose(u->handle);
	u->handle = h;
	u->mtime = m;
	u->gen++;
}

/* Resolve `sym` in `unit`'s current `.so`, reloading first if the file changed. Returns the function
 * pointer for the codegen-emitted indirect call, or NULL if unavailable (the host should no-op). */
void *arche_hot_resolve(int unit, const char *sym) {
	if (unit < 0 || unit >= HOT_MAX_UNITS || !g_units[unit].active)
		return NULL;
	ensure_loaded(&g_units[unit]);
	if (!g_units[unit].handle)
		return NULL;
	return dlsym(g_units[unit].handle, sym);
}
