#include "sem_hints.h"
#include <stdlib.h>
#include <string.h>

/* C99 doesn't expose strdup; declare it explicitly (as sem_model.c does). */
char *strdup(const char *s);

SemHints *sem_hints_new(void) {
	SemHints *h = malloc(sizeof(SemHints));
	h->param_name = NULL;
	h->param_is_own = NULL;
	h->effect_call = NULL;
	h->cap = 0;
	return h;
}

void sem_hints_free(SemHints *h) {
	if (!h)
		return;
	if (h->param_name) {
		for (int i = 0; i < h->cap; i++)
			free((char *)h->param_name[i]); /* owns its copies */
	}
	free(h->param_name);
	free(h->param_is_own);
	free(h->effect_call);
	free(h);
}

static void ensure(SemHints *h, uint32_t node_id) {
	if ((int)node_id < h->cap)
		return;
	int newcap = h->cap ? h->cap * 2 : 64;
	while (newcap <= (int)node_id)
		newcap *= 2;
	h->param_name = realloc(h->param_name, (size_t)newcap * sizeof(const char *));
	h->param_is_own = realloc(h->param_is_own, (size_t)newcap * sizeof(uint8_t));
	h->effect_call = realloc(h->effect_call, (size_t)newcap * sizeof(uint8_t));
	for (int i = h->cap; i < newcap; i++) {
		h->param_name[i] = NULL;
		h->param_is_own[i] = 0;
		h->effect_call[i] = 0;
	}
	h->cap = newcap;
}

void sem_hints_set_param(SemHints *h, uint32_t node_id, const char *name, int is_own) {
	if (!name)
		return;
	ensure(h, node_id);
	free((char *)h->param_name[node_id]); /* a node may be re-resolved */
	h->param_name[node_id] = strdup(name);
	h->param_is_own[node_id] = is_own ? 1 : 0;
}

const char *sem_hints_param_name(const SemHints *h, uint32_t node_id) {
	if (!h || (int)node_id >= h->cap)
		return NULL;
	return h->param_name[node_id];
}

int sem_hints_param_is_own(const SemHints *h, uint32_t node_id) {
	if (!h || (int)node_id >= h->cap)
		return 0;
	return h->param_is_own[node_id];
}

void sem_hints_set_effect_call(SemHints *h, uint32_t node_id) {
	ensure(h, node_id);
	h->effect_call[node_id] = 1;
}

int sem_hints_is_effect_call(const SemHints *h, uint32_t node_id) {
	if (!h || (int)node_id >= h->cap)
		return 0;
	return h->effect_call[node_id];
}
