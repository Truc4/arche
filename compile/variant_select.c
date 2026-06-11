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

void variant_map_load_manifest(VariantMap *m, const char *source_dir) {
	char *path = find_manifest(source_dir);
	if (!path)
		return;
	FILE *f = fopen(path, "r");
	free(path);
	if (!f)
		return;
	/* A deliberately small TOML SUBSET: `[section]` headers, `key = value` lines, `#` comments. We
	 * only consume the `[select]` table (device = "variant"); other sections are skipped, so a
	 * future `[build]`/`[editor]` section is forward-compatible without touching this reader. */
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
		if (strcmp(section, "select") == 0 && key[0])
			variant_map_set(m, key, val);
	}
	fclose(f);
}

/* CLI `--select` overrides accumulate here (highest precedence), set before any compile. */
static VariantMap g_cli_overrides;

void variant_select_cli_set(const char *spec) {
	variant_map_parse(&g_cli_overrides, spec);
}

void variant_map_load_resolved(VariantMap *m, const char *source_dir) {
	/* Lowest precedence first; each later source overrides via variant_map_set. */
	variant_map_load_manifest(m, source_dir); /* project manifest */
	variant_map_load_env(m);                  /* ARCHE_SELECT */
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
