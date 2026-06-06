#include "sem_model.h"
#include <stdlib.h>
#include <string.h>

/* C99 doesn't expose strdup; declare it explicitly (as codegen.c does). */
char *strdup(const char *s);

SemModel *sem_model_new(void) {
	SemModel *m = malloc(sizeof(SemModel));
	m->expr_type = NULL;
	m->expr_nominal = NULL;
	m->expr_type_id = NULL;
	m->bind_alias = NULL;
	m->implicit_move = NULL;
	m->callee_name = NULL;
	m->ref_name = NULL;
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
	if (m->expr_nominal) {
		for (int i = 0; i < m->cap; i++)
			free((char *)m->expr_nominal[i]);
	}
	if (m->callee_name) {
		for (int i = 0; i < m->cap; i++)
			free((char *)m->callee_name[i]);
	}
	if (m->ref_name) {
		for (int i = 0; i < m->cap; i++)
			free((char *)m->ref_name[i]);
	}
	free(m->expr_type);
	free(m->expr_nominal);
	free(m->expr_type_id);
	free(m->bind_alias);
	free(m->implicit_move);
	free(m->callee_name);
	free(m->ref_name);
	free(m);
}

static void ensure(SemModel *m, uint32_t node_id) {
	if ((int)node_id < m->cap)
		return;
	int newcap = m->cap ? m->cap * 2 : 64;
	while (newcap <= (int)node_id)
		newcap *= 2;
	m->expr_type = realloc(m->expr_type, (size_t)newcap * sizeof(const char *));
	m->expr_nominal = realloc(m->expr_nominal, (size_t)newcap * sizeof(const char *));
	m->expr_type_id = realloc(m->expr_type_id, (size_t)newcap * sizeof(TypeId));
	m->bind_alias = realloc(m->bind_alias, (size_t)newcap * sizeof(uint8_t));
	m->implicit_move = realloc(m->implicit_move, (size_t)newcap * sizeof(uint8_t));
	m->callee_name = realloc(m->callee_name, (size_t)newcap * sizeof(const char *));
	m->ref_name = realloc(m->ref_name, (size_t)newcap * sizeof(const char *));
	for (int i = m->cap; i < newcap; i++) {
		m->expr_type[i] = NULL;
		m->expr_nominal[i] = NULL;
		m->expr_type_id[i] = TYID_UNKNOWN;
		m->bind_alias[i] = 0;
		m->implicit_move[i] = 0;
		m->callee_name[i] = NULL;
		m->ref_name[i] = NULL;
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

void sem_model_set_expr_type_id(SemModel *m, uint32_t node_id, TypeId t) {
	ensure(m, node_id);
	m->expr_type_id[node_id] = t;
}

TypeId sem_model_expr_type_id(const SemModel *m, uint32_t node_id) {
	if (!m || (int)node_id >= m->cap)
		return TYID_UNKNOWN;
	return m->expr_type_id[node_id];
}

int sem_model_cap(const SemModel *m) {
	return m ? m->cap : 0;
}

void sem_model_set_expr_nominal(SemModel *m, uint32_t node_id, const char *name) {
	ensure(m, node_id);
	free((char *)m->expr_nominal[node_id]);
	m->expr_nominal[node_id] = name ? strdup(name) : NULL;
}

const char *sem_model_expr_nominal(const SemModel *m, uint32_t node_id) {
	if (!m || (int)node_id >= m->cap)
		return NULL;
	return m->expr_nominal[node_id];
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

void sem_model_set_implicit_move(SemModel *m, uint32_t node_id) {
	ensure(m, node_id);
	m->implicit_move[node_id] = 1;
}

int sem_model_implicit_move(const SemModel *m, uint32_t node_id) {
	if (!m || (int)node_id >= m->cap)
		return 0;
	return m->implicit_move[node_id];
}

void sem_model_set_callee_name(SemModel *m, uint32_t node_id, const char *name) {
	ensure(m, node_id);
	free((char *)m->callee_name[node_id]);
	m->callee_name[node_id] = name ? strdup(name) : NULL;
}

const char *sem_model_callee_name(const SemModel *m, uint32_t node_id) {
	if (!m || (int)node_id >= m->cap)
		return NULL;
	return m->callee_name[node_id];
}

void sem_model_set_ref_name(SemModel *m, uint32_t node_id, const char *name) {
	ensure(m, node_id);
	free((char *)m->ref_name[node_id]);
	m->ref_name[node_id] = name ? strdup(name) : NULL;
}

const char *sem_model_ref_name(const SemModel *m, uint32_t node_id) {
	if (!m || (int)node_id >= m->cap)
		return NULL;
	return m->ref_name[node_id];
}
