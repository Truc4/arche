#include "sem_model.h"
#include <stdlib.h>

SemModel *sem_model_new(void) {
	SemModel *m = malloc(sizeof(SemModel));
	m->expr_type = NULL;
	m->cap = 0;
	return m;
}

void sem_model_free(SemModel *m) {
	if (!m)
		return;
	free(m->expr_type); /* strings are borrowed; not freed here */
	free(m);
}

static void ensure(SemModel *m, uint32_t node_id) {
	if ((int)node_id < m->cap)
		return;
	int newcap = m->cap ? m->cap * 2 : 64;
	while (newcap <= (int)node_id)
		newcap *= 2;
	m->expr_type = realloc(m->expr_type, (size_t)newcap * sizeof(const char *));
	for (int i = m->cap; i < newcap; i++)
		m->expr_type[i] = NULL;
	m->cap = newcap;
}

void sem_model_set_expr_type(SemModel *m, uint32_t node_id, const char *type) {
	ensure(m, node_id);
	m->expr_type[node_id] = type;
}

const char *sem_model_expr_type(const SemModel *m, uint32_t node_id) {
	if (!m || (int)node_id >= m->cap)
		return NULL;
	return m->expr_type[node_id];
}
