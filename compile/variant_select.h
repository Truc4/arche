#ifndef ARCHE_VARIANT_SELECT_H
#define ARCHE_VARIANT_SELECT_H

#include <stddef.h> /* size_t */

/* Per-device variant selection — "which backend subfolder backs this device this build".
 *
 * A device folder may carry variant subfolders (e.g. `gfx/x11/`, `gfx/wayland/`); the active
 * selection picks which one's files merge on top of the always-merged top-level files (see
 * compile/module_resolve.c). The SAME selection must drive the compiler and the editor analyzer,
 * or the LSP would green-light code the build rejects. The shared source of truth is therefore
 * loaded into one VariantMap that both front-ends consult.
 *
 * Selection precedence (highest first): CLI `--select dev=var` > env `ARCHE_SELECT` >
 * active `[target.<name>]` profile > `[select]` base table > a device's declared default. The active
 * target itself is `--target` (CLI) > `ARCHE_TARGET` (env) > the manifest's top-level `target = "..."`,
 * so flipping one `target` line retargets every device. Later sources are added first and
 * higher-precedence sources OVERRIDE, so `variant_map_set` replaces any prior entry for a device. */

typedef struct {
	char *device;  /* owned */
	char *variant; /* owned */
} VariantSel;

typedef struct {
	VariantSel *items;
	int count, cap;
} VariantMap;

/* Set `device`'s selected variant (replacing any prior entry). An empty/NULL variant clears it. */
void variant_map_set(VariantMap *m, const char *device, const char *variant);

/* Parse a comma/space-separated `dev=var` spec (e.g. "gfx=x11, net=tcp") into the map, each entry
 * overriding any prior one. Tolerant of empty fields. */
void variant_map_parse(VariantMap *m, const char *spec);

/* Convenience: parse the `ARCHE_SELECT` environment variable into the map (no-op if unset). */
void variant_map_load_env(VariantMap *m);

/* Parse the `[select]` table of the nearest `arche.toml` (searched upward from `source_dir` to the
 * filesystem root, like Cargo.toml / go.mod) into the map. No-op if no manifest is found. */
void variant_map_load_manifest(VariantMap *m, const char *source_dir);

/* Record a CLI `--select dev=var` override (accumulates; highest precedence). The compiler's CLI
 * sets these; the analyzer never does (it has no command line), which is why the CLI layer can't
 * desync the editor — the persistent layers (manifest, env) are what both front-ends share. */
void variant_select_cli_set(const char *spec);

/* Record the CLI `--target <name>` override (compiler only; transient, highest precedence for choosing
 * the active platform profile). The analyzer never sets this — the persistent target is the manifest's
 * `target` key / `ARCHE_TARGET`, which both front-ends read, keeping the editor and build in agreement. */
void variant_select_cli_target(const char *name);

/* Enable a stderr warning when the active target names no `[target.<name>]` table (typo guard). The
 * compiler's CLI turns this on; the analyzer leaves it off to avoid per-analysis log spam. */
void variant_select_set_warnings(int on);

/* Build the effective selection for a build/analysis rooted at `source_dir`, applying every source
 * in precedence order (lowest first, each overriding): manifest -> env -> CLI overrides. Both the
 * compiler and the analyzer call THIS, so they resolve backends identically. */
void variant_map_load_resolved(VariantMap *m, const char *source_dir);

/* Copy the directory of the nearest `arche.toml` (walking up from `source_dir`) into `out`. Returns 1
 * if a manifest was found (out = its dir), 0 otherwise (out untouched). Used to anchor a project-stable
 * object-cache dir for `arche run`, whose output is a throwaway temp. */
int variant_manifest_dir(const char *source_dir, char *out, size_t cap);

/* The selected variant subfolder name for `device`, or NULL if none is selected. */
const char *variant_map_lookup(const VariantMap *m, const char *device);

void variant_map_free(VariantMap *m);

#endif /* ARCHE_VARIANT_SELECT_H */
