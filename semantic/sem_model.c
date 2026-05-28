#include "sem_model.h"
#include <stdlib.h>
#include <string.h>

/* C99 doesn't expose strdup; declare it explicitly (as codegen.c does). */
char *strdup(const char *s);

SemModel *sem_model_new(void) {
	SemModel *m = malloc(sizeof(SemModel));
	m->expr_type = NULL;
	m->bind_alias = NULL;
	m->cap = 0;
	return m;
}

void sem_model_free(SemModel *m) {
	if (!m)
		return;
	if (m->expr_type) {
		for (int i = 0; i < m->cap; i++)
			free((char *)m->expr_type[i]); /* model owns its copies */
	}
	free(m->expr_type);
	free(m->bind_alias);
	free(m);
}

static void ensure(SemModel *m, uint32_t node_id) {
	if ((int)node_id < m->cap)
		return;
	int newcap = m->cap ? m->cap * 2 : 64;
	while (newcap <= (int)node_id)
		newcap *= 2;
	m->expr_type = realloc(m->expr_type, (size_t)newcap * sizeof(const char *));
	m->bind_alias = realloc(m->bind_alias, (size_t)newcap * sizeof(uint8_t));
	for (int i = m->cap; i < newcap; i++) {
		m->expr_type[i] = NULL;
		m->bind_alias[i] = 0;
	}
	m->cap = newcap;
}

void sem_model_set_expr_type(SemModel *m, uint32_t node_id, const char *type) {
	ensure(m, node_id);
	/* The model owns its strings: copy in, freeing any prior value (a node may be
	 * re-resolved). The source pointer's lifetime is no longer our concern. */
	free((char *)m->expr_type[node_id]);
	m->expr_type[node_id] = type ? strdup(type) : NULL;
}

const char *sem_model_expr_type(const SemModel *m, uint32_t node_id) {
	if (!m || (int)node_id >= m->cap)
		return NULL;
	return m->expr_type[node_id];
}

void sem_model_set_bind_alias(SemModel *m, uint32_t node_id) {
	ensure(m, node_id);
	m->bind_alias[node_id] = 1;
}

int sem_model_bind_alias(const SemModel *m, uint32_t node_id) {
	if (!m || (int)node_id >= m->cap)
		return 0;
	return m->bind_alias[node_id];
}
