#include "variant_select.h"
#include <stdlib.h>
#include <string.h>

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
