#ifndef ARCHE_MODULE_RESOLVE_H
#define ARCHE_MODULE_RESOLVE_H

#include "../semantic/sem_ids.h" /* DeclOrigin */

/* Shared module/device resolution POLICY, used by BOTH the compiler front-end
 * (compile/compile.c) and the editor analyzer (arche_analyzer.c).
 *
 * The two front-ends used to carry independent copies of this logic, which had already
 * drifted (opposite stdlib/source search order; the analyzer bypassing arche_resource_dir).
 * That is exactly what makes "the LSP says fine while the compiler complains" possible. The
 * fix is structural: one resolver, one search order, one place that decides which file backs
 * an `#import`. Each front-end keeps its OWN registration (the compiler also lowers; the
 * analyzer only feeds semantics) by supplying callbacks; this unit owns ONLY the policy —
 * search order (stdlib -> importing source dir -> core), folder-vs-single-file layout,
 * variant-subfolder selection, and dedup. */

typedef struct ModuleResolver {
	void *ctx; /* opaque per-front-end state passed back to every callback */

	/* Dedup: return 1 if `name` is already loaded (caller skips it); otherwise record it as
	 * loaded and return 0. Marking before load also makes transitive `#import` cycle-safe. */
	int (*mark_seen)(void *ctx, const char *name);

	/* Parse + register one `.arche` file into the front-end (the front-end recurses into that
	 * file's own `#import`s, which re-enters this resolver). Returns 1 on success, 0 otherwise. */
	int (*register_file)(void *ctx, const char *mod_name, const char *path, const char *source_dir,
	                     DeclOrigin origin);

	/* Note that `mod_name`'s folder carries a `.ds.arche` datasheet (it is a device). Optional —
	 * may be NULL for front-ends that don't track the device set. */
	void (*mark_device)(void *ctx, const char *mod_name);

	/* The selected variant subfolder for a device (e.g. "x11"), or NULL/"" for none. Optional —
	 * may be NULL. Consulted only for bare-name device imports, never for path imports. */
	const char *(*select_variant)(void *ctx, const char *mod_name);
} ModuleResolver;

/* `#import { name }` — a device/module imported by bare name: searched in stdlib, then the
 * importing file's source dir, then core (stdlib authoritative). Returns 1 if resolved (loaded
 * now or already loaded), 0 if not found anywhere. */
int arche_module_load_by_name(const ModuleResolver *r, const char *name, const char *source_dir);

/* `#import { "./path" }` — a plain module imported by path, resolved relative to the importer's
 * dir. No stdlib/core search and no variant overlay (path imports are plain modules). Returns 1
 * if resolved (loaded now or already loaded), 0 if not found. */
int arche_module_load_by_path(const ModuleResolver *r, const char *path, const char *source_dir);

#endif /* ARCHE_MODULE_RESOLVE_H */
