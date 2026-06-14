#include "module_resolve.h"
#include "../cli/resource.h"
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static int path_exists(const char *p) {
	struct stat st;
	return stat(p, &st) == 0;
}

static int has_suffix(const char *name, const char *suf) {
	size_t L = strlen(name), S = strlen(suf);
	return L >= S && strcmp(name + L - S, suf) == 0;
}

/* Register every `.arche` file directly in `folder` (NOT its subdirs) as part of module
 * `mod_name`. A `.ds.arche` among them marks the module a device. Returns files registered. */
static int merge_arche_dir(const ModuleResolver *r, const char *mod_name, const char *folder, const char *source_dir,
                           DeclOrigin origin) {
	DIR *d = opendir(folder);
	if (!d)
		return 0;
	int n = 0;
	struct dirent *ent;
	while ((ent = readdir(d)) != NULL) {
		/* A `.c` shim alongside the device's `.arche` files (or in the selected variant subfolder) is
		 * compiled + linked, not parsed as arche source. Collected here so it rides the same variant
		 * overlay as the `.arche` files: only the selected variant's glue reaches the link. */
		if (r->add_c_shim && has_suffix(ent->d_name, ".c")) {
			char cp[1300];
			snprintf(cp, sizeof(cp), "%s/%s", folder, ent->d_name);
			r->add_c_shim(r->ctx, cp);
			continue;
		}
		if (!has_suffix(ent->d_name, ".arche"))
			continue;
		char fp[1300];
		snprintf(fp, sizeof(fp), "%s/%s", folder, ent->d_name);
		n += r->register_file(r->ctx, mod_name, fp, source_dir, origin);
		if (r->mark_device && has_suffix(ent->d_name, ".ds.arche"))
			r->mark_device(r->ctx, mod_name);
	}
	closedir(d);
	return n;
}

/* Try to load `mod_name` from `dir`: a FOLDER `<dir>/<mod_name>/` of `.arche` files (its
 * top-level files always merge; when `apply_variant` and a variant is selected, that variant
 * subfolder's `.arche` files merge on top), else a single `<dir>/<mod_name>.arche`. Returns
 * files registered. */
static int try_load_dir(const ModuleResolver *r, const char *mod_name, const char *dir, const char *source_dir,
                        DeclOrigin origin, int apply_variant) {
	char folder[1024];
	snprintf(folder, sizeof(folder), "%s/%s", dir, mod_name);
	int n = merge_arche_dir(r, mod_name, folder, source_dir, origin);
	if (n > 0) {
		const char *variant = (apply_variant && r->select_variant) ? r->select_variant(r->ctx, mod_name) : NULL;
		if (variant && variant[0]) {
			char vfolder[1300];
			snprintf(vfolder, sizeof(vfolder), "%s/%s", folder, variant);
			merge_arche_dir(r, mod_name, vfolder, source_dir, origin);
		}
		return n;
	}
	char fp[1300];
	snprintf(fp, sizeof(fp), "%s/%s.arche", dir, mod_name);
	if (path_exists(fp))
		return r->register_file(r->ctx, mod_name, fp, source_dir, origin);
	return 0;
}

int arche_module_load_by_name(const ModuleResolver *r, const char *name, const char *source_dir) {
	if (r->mark_seen(r->ctx, name))
		return 1; /* already loaded */
	/* STDLIB is authoritative: a stdlib module name always resolves to stdlib, even if the source
	 * tree has a same-named subdir (the prelude imports `io`, so every program triggers this).
	 * Then the importing source dir, then core. arche_resource_dir() honors env/sysroot/install
	 * layout, so the compiler and the analyzer search the SAME directories. */
	int loaded = try_load_dir(r, name, arche_resource_dir(ARCHE_RES_STDLIB), source_dir, DECL_ORIGIN_STDLIB, 1);
	if (!loaded)
		loaded = try_load_dir(r, name, source_dir, source_dir, DECL_ORIGIN_USER_MODULE, 1);
	if (!loaded)
		loaded = try_load_dir(r, name, arche_resource_dir(ARCHE_RES_CORE), source_dir, DECL_ORIGIN_CORE, 1);
	return loaded > 0;
}

int arche_module_load_by_path(const ModuleResolver *r, const char *path, const char *source_dir) {
	/* Split `path` into its directory part and basename; module name = basename sans `.arche`. */
	const char *slash = strrchr(path, '/');
	const char *base = slash ? slash + 1 : path;
	char subdir[512];
	if (slash) {
		size_t dl = (size_t)(slash - path);
		if (dl > sizeof(subdir) - 1)
			dl = sizeof(subdir) - 1;
		memcpy(subdir, path, dl);
		subdir[dl] = '\0';
	} else {
		subdir[0] = '\0';
	}
	char mod_name[256];
	snprintf(mod_name, sizeof(mod_name), "%s", base);
	size_t ml = strlen(mod_name);
	if (ml > 6 && strcmp(mod_name + ml - 6, ".arche") == 0)
		mod_name[ml - 6] = '\0';
	if (mod_name[0] == '\0')
		return 0;
	if (r->mark_seen(r->ctx, mod_name))
		return 1; /* already loaded */
	/* Resolve relative to the importing file's dir; transitive imports resolve from that same dir. */
	char dir[800];
	if (subdir[0])
		snprintf(dir, sizeof(dir), "%s/%s", source_dir, subdir);
	else
		snprintf(dir, sizeof(dir), "%s", source_dir);
	/* path import = plain module, no variant overlay */
	return try_load_dir(r, mod_name, dir, dir, DECL_ORIGIN_USER_MODULE, 0) > 0;
}
