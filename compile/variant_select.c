/* realpath() for manifest discovery is POSIX; expose it under -std=c99. */
#define _POSIX_C_SOURCE 200809L
#include "variant_select.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* C99 + -std=c99 hides these POSIX prototypes without the feature macro fully applied; declare them
 * explicitly (as compile.c does for mkdtemp / codegen.c for strdup). */
char *realpath(const char *path, char *resolved_path);

static char *dup_str(const char *s) {
	size_t n = strlen(s) + 1;
	char *p = malloc(n);
	memcpy(p, s, n);
	return p;
}

void variant_map_set(VariantMap *m, const char *device, const char *variant) {
	if (!device || !device[0])
		return;
	/* Override any existing entry for this device (precedence: higher-priority callers set later). */
	for (int i = 0; i < m->count; i++) {
		if (strcmp(m->items[i].device, device) == 0) {
			free(m->items[i].variant);
			m->items[i].variant = (variant && variant[0]) ? dup_str(variant) : NULL;
			return;
		}
	}
	if (!variant || !variant[0])
		return;
	if (m->count >= m->cap) {
		m->cap = m->cap ? m->cap * 2 : 4;
		m->items = realloc(m->items, (size_t)m->cap * sizeof(VariantSel));
	}
	m->items[m->count].device = dup_str(device);
	m->items[m->count].variant = dup_str(variant);
	m->count++;
}

void variant_map_parse(VariantMap *m, const char *spec) {
	if (!spec)
		return;
	const char *p = spec;
	while (*p) {
		/* skip separators */
		while (*p == ',' || *p == ' ' || *p == '\t')
			p++;
		if (!*p)
			break;
		const char *start = p;
		while (*p && *p != ',')
			p++;
		const char *end = p; /* one past the entry */
		/* trim trailing spaces */
		while (end > start && (end[-1] == ' ' || end[-1] == '\t'))
			end--;
		const char *eq = start;
		while (eq < end && *eq != '=')
			eq++;
		if (eq < end) {
			char dev[64], var[64];
			size_t dl = (size_t)(eq - start);
			size_t vl = (size_t)(end - (eq + 1));
			if (dl >= sizeof(dev))
				dl = sizeof(dev) - 1;
			if (vl >= sizeof(var))
				vl = sizeof(var) - 1;
			memcpy(dev, start, dl);
			dev[dl] = '\0';
			memcpy(var, eq + 1, vl);
			var[vl] = '\0';
			variant_map_set(m, dev, var);
		}
	}
}

void variant_map_load_env(VariantMap *m) {
	variant_map_parse(m, getenv("ARCHE_SELECT"));
}

/* Find the nearest `arche.toml` by walking up from `source_dir` to the filesystem root. Returns a
 * malloc'd absolute path, or NULL if none exists. */
static char *find_manifest(const char *source_dir) {
	char abs[PATH_MAX];
	if (!realpath((source_dir && source_dir[0]) ? source_dir : ".", abs))
		return NULL;
	for (;;) {
		char cand[PATH_MAX + 16];
		snprintf(cand, sizeof(cand), "%s/arche.toml", abs);
		struct stat st;
		if (stat(cand, &st) == 0 && S_ISREG(st.st_mode))
			return dup_str(cand);
		char *slash = strrchr(abs, '/');
		if (!slash)
			break;
		if (slash == abs) { /* parent is the root directory "/" */
			if (stat("/arche.toml", &st) == 0 && S_ISREG(st.st_mode))
				return dup_str("/arche.toml");
			break;
		}
		*slash = '\0';
	}
	return NULL;
}

int variant_manifest_dir(const char *source_dir, char *out, size_t cap) {
	char *path = find_manifest(source_dir);
	if (!path)
		return 0;
	char *slash = strrchr(path, '/'); /* strip the trailing "/arche.toml" → the project dir */
	if (slash)
		*slash = '\0';
	snprintf(out, cap, "%s", path[0] ? path : "/");
	free(path);
	return 1;
}

/* Read a value after `=` into `out` (cap bytes): a `"quoted"` string, or a bare token up to the
 * first whitespace or `#` comment. */
static void read_value(const char *v, char *out, size_t cap) {
	while (*v == ' ' || *v == '\t')
		v++;
	size_t n = 0;
	if (*v == '"') {
		v++;
		while (*v && *v != '"' && n < cap - 1)
			out[n++] = *v++;
	} else {
		while (*v && *v != '#' && *v != '\n' && *v != '\r' && *v != ' ' && *v != '\t' && n < cap - 1)
			out[n++] = *v++;
	}
	out[n] = '\0';
}

/* Copy every entry of `src` into `dst` (later sources override; precedence = call order). */
static void variant_map_merge(VariantMap *dst, const VariantMap *src) {
	for (int i = 0; i < src->count; i++)
		variant_map_set(dst, src->items[i].device, src->items[i].variant);
}

/* In-memory model of one `arche.toml`, parsed once then flattened by the active target. The TOML
 * SUBSET: `[section]` headers, `key = value` lines, `#` comments. We consume the top-level `target`
 * key, the `[select]` base table (device = "variant"), and any number of `[target.<name>]` platform
 * tables (each device = "variant"); unknown sections are ignored, so future sections stay
 * forward-compatible without touching this reader. */
#define MANIFEST_MAX_TARGETS 32
typedef struct {
	char name[64];  /* the `<name>` of a `[target.<name>]` table */
	VariantMap map; /* its device -> variant entries */
} TargetTable;
typedef struct {
	char declared_target[64]; /* top-level `target = "..."` ("" if absent) */
	VariantMap select;        /* `[select]` base */
	TargetTable targets[MANIFEST_MAX_TARGETS];
	int target_count;
} Manifest;

/* Find (or create) the per-name map for a `[target.<name>]` table. NULL only if the table cap is hit. */
static VariantMap *manifest_target_map(Manifest *mf, const char *name) {
	for (int i = 0; i < mf->target_count; i++)
		if (strcmp(mf->targets[i].name, name) == 0)
			return &mf->targets[i].map;
	if (mf->target_count >= MANIFEST_MAX_TARGETS)
		return NULL;
	TargetTable *t = &mf->targets[mf->target_count++];
	snprintf(t->name, sizeof(t->name), "%s", name); /* t->map already zeroed by manifest_parse's memset */
	return &t->map;
}

static void manifest_parse(Manifest *mf, const char *source_dir) {
	memset(mf, 0, sizeof(*mf));
	char *path = find_manifest(source_dir);
	if (!path)
		return;
	FILE *f = fopen(path, "r");
	free(path);
	if (!f)
		return;
	char line[1024];
	char section[64] = "";
	while (fgets(line, sizeof(line), f)) {
		const char *p = line;
		while (*p == ' ' || *p == '\t')
			p++;
		if (*p == '#' || *p == '\n' || *p == '\r' || *p == '\0')
			continue;
		if (*p == '[') {
			const char *end = strchr(p, ']');
			if (end && end > p + 1) {
				size_t n = (size_t)(end - (p + 1));
				if (n >= sizeof(section))
					n = sizeof(section) - 1;
				memcpy(section, p + 1, n);
				section[n] = '\0';
			}
			continue;
		}
		const char *eq = strchr(p, '=');
		if (!eq)
			continue;
		const char *ke = eq;
		while (ke > p && (ke[-1] == ' ' || ke[-1] == '\t'))
			ke--;
		char key[64];
		size_t kl = (size_t)(ke - p);
		if (kl >= sizeof(key))
			kl = sizeof(key) - 1;
		memcpy(key, p, kl);
		key[kl] = '\0';
		char val[64];
		read_value(eq + 1, val, sizeof(val));
		if (section[0] == '\0') { /* top-level keys (before any [section]) */
			if (strcmp(key, "target") == 0)
				snprintf(mf->declared_target, sizeof(mf->declared_target), "%s", val);
		} else if (strcmp(section, "select") == 0) {
			if (key[0])
				variant_map_set(&mf->select, key, val);
		} else if (strncmp(section, "target.", 7) == 0) {
			const char *tname = section + 7;
			if (tname[0] && key[0]) {
				VariantMap *tm = manifest_target_map(mf, tname);
				if (tm)
					variant_map_set(tm, key, val);
			}
		}
	}
	fclose(f);
}

static void manifest_free(Manifest *mf) {
	variant_map_free(&mf->select);
	for (int i = 0; i < mf->target_count; i++)
		variant_map_free(&mf->targets[i].map);
}

void variant_map_load_manifest(VariantMap *m, const char *source_dir) {
	/* Back-compat entry: apply only the target-independent `[select]` base. */
	Manifest mf;
	manifest_parse(&mf, source_dir);
	variant_map_merge(m, &mf.select);
	manifest_free(&mf);
}

/* CLI `--select` overrides accumulate here (highest precedence), set before any compile. */
static VariantMap g_cli_overrides;

void variant_select_cli_set(const char *spec) {
	variant_map_parse(&g_cli_overrides, spec);
}

/* CLI `--target` override (compiler only; transient). The analyzer has no command line, so this can
 * never desync the editor — the persistent target lives in the manifest / ARCHE_TARGET, read by both. */
static char g_cli_target[64];

void variant_select_cli_target(const char *name) {
	if (name && name[0])
		snprintf(g_cli_target, sizeof(g_cli_target), "%s", name);
}

/* Whether to warn (stderr) when the active target names no `[target.<name>]` table — catches typos.
 * Opt-in so only the compiler's CLI warns; the analyzer leaves it off (no per-keystroke log spam). */
static int g_warn_targets;

void variant_select_set_warnings(int on) {
	g_warn_targets = on;
}

void variant_map_load_resolved(VariantMap *m, const char *source_dir) {
	Manifest mf;
	manifest_parse(&mf, source_dir);
	/* Active target (highest precedence first): CLI `--target` > `ARCHE_TARGET` env > manifest `target`. */
	const char *active = NULL;
	if (g_cli_target[0])
		active = g_cli_target;
	if (!active) {
		const char *e = getenv("ARCHE_TARGET");
		if (e && e[0])
			active = e;
	}
	if (!active && mf.declared_target[0])
		active = mf.declared_target;
	/* Flatten, lowest precedence first; each later source overrides via variant_map_set:
	 * `[select]` base -> active `[target.<active>]` -> `ARCHE_SELECT` env -> CLI `--select`. */
	variant_map_merge(m, &mf.select);
	if (active) {
		int matched = 0;
		for (int i = 0; i < mf.target_count; i++)
			if (strcmp(mf.targets[i].name, active) == 0) {
				variant_map_merge(m, &mf.targets[i].map);
				matched = 1;
				break;
			}
		if (!matched && g_warn_targets)
			fprintf(stderr, "warning: target '%s' has no [target.%s] table in arche.toml; using [select]/defaults\n",
			        active, active);
	}
	manifest_free(&mf);
	variant_map_load_env(m);                        /* ARCHE_SELECT */
	for (int i = 0; i < g_cli_overrides.count; i++) /* CLI --select (compiler only) */
		variant_map_set(m, g_cli_overrides.items[i].device, g_cli_overrides.items[i].variant);
}

const char *variant_map_lookup(const VariantMap *m, const char *device) {
	for (int i = 0; i < m->count; i++)
		if (strcmp(m->items[i].device, device) == 0)
			return m->items[i].variant;
	return NULL;
}

void variant_map_free(VariantMap *m) {
	for (int i = 0; i < m->count; i++) {
		free(m->items[i].device);
		free(m->items[i].variant);
	}
	free(m->items);
	m->items = NULL;
	m->count = m->cap = 0;
}

/* Append `dir` to the roots list (dedup, cap-guarded; empty ignored). */
static void lib_roots_add(LibRoots *out, const char *dir) {
	if (!dir || !dir[0] || out->count >= ARCHE_LIB_MAX_ROOTS)
		return;
	for (int i = 0; i < out->count; i++)
		if (strcmp(out->dirs[i], dir) == 0)
			return;
	snprintf(out->dirs[out->count++], sizeof(out->dirs[0]), "%s", dir);
}

/* Pull the `[lib] paths = [ "a", "b" ]` array out of the manifest at `manifest_path` (whose dir is
 * `manifest_dir`), resolving relative entries against the project dir. Single- or multi-line arrays are
 * both handled: we scan from `paths =` to the closing `]`, lifting every "quoted" token. */
static void lib_roots_from_manifest(LibRoots *out, const char *manifest_path, const char *manifest_dir) {
	FILE *f = fopen(manifest_path, "r");
	if (!f)
		return;
	char line[1024];
	char section[64] = "";
	int in_array = 0; /* inside an unterminated `paths = [ ... ` spanning lines */
	while (fgets(line, sizeof(line), f)) {
		const char *p = line;
		while (*p == ' ' || *p == '\t')
			p++;
		if (!in_array) {
			if (*p == '#' || *p == '\n' || *p == '\r' || *p == '\0')
				continue;
			if (*p == '[') {
				const char *end = strchr(p, ']');
				if (end && end > p + 1) {
					size_t n = (size_t)(end - (p + 1));
					if (n >= sizeof(section))
						n = sizeof(section) - 1;
					memcpy(section, p + 1, n);
					section[n] = '\0';
				}
				continue;
			}
			if (strcmp(section, "lib") != 0)
				continue;
			const char *eq = strchr(p, '=');
			if (!eq)
				continue;
			char key[64];
			const char *ke = eq;
			while (ke > p && (ke[-1] == ' ' || ke[-1] == '\t'))
				ke--;
			size_t kl = (size_t)(ke - p);
			if (kl >= sizeof(key))
				kl = sizeof(key) - 1;
			memcpy(key, p, kl);
			key[kl] = '\0';
			if (strcmp(key, "paths") != 0)
				continue;
			p = eq + 1; /* fall through into the array body on this same line */
			in_array = 1;
		}
		/* Inside the array body: lift each "quoted" entry; stop at `]`. */
		for (const char *q = p; *q; q++) {
			if (*q == ']') {
				in_array = 0;
				break;
			}
			if (*q != '"')
				continue;
			char entry[1024];
			size_t n = 0;
			q++;
			while (*q && *q != '"' && n < sizeof(entry) - 1)
				entry[n++] = *q++;
			entry[n] = '\0';
			if (!*q)
				break; /* unterminated quote — bail */
			if (entry[0] == '/') {
				lib_roots_add(out, entry); /* absolute */
			} else if (entry[0]) {
				char abs[2304];
				snprintf(abs, sizeof(abs), "%s/%s", manifest_dir, entry); /* relative to project dir */
				lib_roots_add(out, abs);
			}
		}
	}
	fclose(f);
}

void arche_lib_roots(const char *source_dir, LibRoots *out) {
	out->count = 0;
	/* `ARCHE_PATH` (colon-separated) — highest precedence, searched first. */
	const char *env = getenv("ARCHE_PATH");
	if (env && *env) {
		char buf[4096];
		snprintf(buf, sizeof(buf), "%s", env);
		char *save = NULL;
		for (char *tok = strtok_r(buf, ":", &save); tok; tok = strtok_r(NULL, ":", &save))
			lib_roots_add(out, tok);
	}
	/* Then the nearest `arche.toml`'s `[lib] paths`. */
	char *path = find_manifest(source_dir);
	if (path) {
		char dir[1024];
		snprintf(dir, sizeof(dir), "%s", path);
		char *slash = strrchr(dir, '/');
		if (slash)
			*slash = '\0';
		lib_roots_from_manifest(out, path, dir[0] ? dir : "/");
		free(path);
	}
}
