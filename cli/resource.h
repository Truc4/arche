#ifndef ARCHE_CLI_RESOURCE_H
#define ARCHE_CLI_RESOURCE_H

/* Resource (core/stdlib/runtime/explain) directory discovery, so an installed `arche` finds its
 * support files without source-tree-baked absolute paths. Every call site that needs one of these
 * directories goes through arche_resource_dir() instead of the raw ARCHE_*_DIR defines.
 *
 * Resolution order (first that applies wins, cached after the first call):
 *   1. per-resource env override (ARCHE_CORE_DIR / ARCHE_STDLIB_DIR / ARCHE_RUNTIME_DIR /
 *      ARCHE_EXPLAIN_DIR) — for power users / tests;
 *   2. $ARCHE_SYSROOT/lib/arche/<core|stdlib|runtime|explain> — an explicit install root;
 *   3. exe-relative: <dir-of-executable>/../lib/arche/<...> — the FHS install layout
 *      (/usr/local/bin/arche + /usr/local/lib/arche/...), if that directory exists;
 *   4. the compiled-in ARCHE_*_DIR default (the in-tree build paths) — so build/arche just works. */

typedef enum {
	ARCHE_RES_CORE = 0,
	ARCHE_RES_STDLIB,
	ARCHE_RES_RUNTIME,
	ARCHE_RES_EXPLAIN,
} ArcheResource;

const char *arche_resource_dir(ArcheResource which);

/* True when `path` is the core prelude itself, so prepending core would double-define every
 * prelude symbol. Matches by (device, inode) against the resolved prelude AND by the canonical
 * `…/core/core.arche` layout — the latter recognizes the prelude *source* in a repo even when a
 * different installed copy is the resolved prelude (same role, different inode). Shared by the
 * compiler front-end and the analyzer so the two never diverge on "is this core?". */
int arche_path_is_core(const char *path);

#endif /* ARCHE_CLI_RESOURCE_H */
