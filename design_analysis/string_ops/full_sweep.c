#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define INITIAL 300
#define MAX_LEN 100
#define ITERS 10

typedef struct {
	long long ns;
} Timer;

Timer timer_start() {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (Timer){ts.tv_sec * 1000000000LL + ts.tv_nsec};
}

long long timer_end(Timer start) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	long long end = ts.tv_sec * 1000000000LL + ts.tv_nsec;
	return end - start.ns;
}

int random_int(int *seed, int max) {
	if (max <= 0)
		return 0;
	*seed = (*seed * 1103515245 + 12345) & 0x7fffffff;
	return *seed % max;
}

// === Method 1: Pointers ===
typedef struct {
	char **strs;
	int cnt, cap;
} M1;
M1 m1_create(int n) {
	M1 m = {malloc(n * 2 * sizeof(char *)), n, n * 2};
	for (int i = 0; i < n; i++) {
		m.strs[i] = malloc(MAX_LEN);
		snprintf(m.strs[i], MAX_LEN, "s%d", i);
	}
	return m;
}
void m1_add(M1 *m, int id) {
	if (m->cnt >= m->cap) {
		m->cap *= 2;
		m->strs = realloc(m->strs, m->cap * sizeof(char *));
	}
	m->strs[m->cnt] = malloc(MAX_LEN);
	snprintf(m->strs[m->cnt], MAX_LEN, "s%d", id);
	m->cnt++;
}
void m1_del(M1 *m, int i) {
	if (i < 0 || i >= m->cnt || m->cnt == 0)
		return;
	free(m->strs[i]);
	for (int j = i; j < m->cnt - 1; j++)
		m->strs[j] = m->strs[j + 1];
	m->cnt--;
}
void m1_upper(M1 m) {
	for (int i = 0; i < m.cnt; i++)
		for (char *p = m.strs[i]; *p; p++)
			*p = toupper(*p);
}
void m1_free(M1 m) {
	for (int i = 0; i < m.cnt; i++)
		free(m.strs[i]);
	free(m.strs);
}

// === Method 2: Contiguous (computed) ===
typedef struct {
	char *buf;
	int *len;
	int cnt, sz, cap;
} M2;
M2 m2_create(int n) {
	M2 m = {malloc(n * MAX_LEN), malloc(n * sizeof(int)), n, 0, n * MAX_LEN};
	int p = 0;
	for (int i = 0; i < n; i++) {
		char t[MAX_LEN];
		snprintf(t, MAX_LEN, "s%d", i);
		int l = strlen(t);
		m.len[i] = l;
		memcpy(m.buf + p, t, l + 1);
		p += l + 1;
		m.sz = p;
	}
	return m;
}
void m2_add(M2 *m, int id) {
	if (m->cnt >= 1500)
		return;
	char t[MAX_LEN];
	snprintf(t, MAX_LEN, "s%d", id);
	int l = strlen(t);
	if (m->sz + l + 1 >= m->cap) {
		m->cap *= 2;
		m->buf = realloc(m->buf, m->cap);
	}
	m->len = realloc(m->len, (m->cnt + 1) * sizeof(int));
	m->len[m->cnt] = l;
	memcpy(m->buf + m->sz, t, l + 1);
	m->sz += l + 1;
	m->cnt++;
}
void m2_del(M2 *m, int i) {
	if (i < 0 || i >= m->cnt || m->cnt == 0)
		return;
	int o = 0;
	for (int j = 0; j < i; j++)
		o += m->len[j] + 1;
	int ol = m->len[i], sh = m->sz - (o + ol + 1);
	if (sh > 0)
		memmove(m->buf + o, m->buf + o + ol + 1, sh);
	m->sz -= ol + 1;
	for (int j = i; j < m->cnt - 1; j++)
		m->len[j] = m->len[j + 1];
	m->cnt--;
}
void m2_upper(M2 m) {
	int o = 0;
	for (int i = 0; i < m.cnt; i++) {
		for (int j = 0; j < m.len[i]; j++)
			m.buf[o + j] = toupper(m.buf[o + j]);
		o += m.len[i] + 1;
	}
}
void m2_free(M2 m) {
	free(m.buf);
	free(m.len);
}

// === Method 3: Contiguous (cached) ===
typedef struct {
	char *buf;
	int *off, *len;
	int cnt, sz, cap;
} M3;
M3 m3_create(int n) {
	M3 m = {malloc(n * MAX_LEN), malloc(n * sizeof(int)), malloc(n * sizeof(int)), n, 0, n * MAX_LEN};
	int p = 0;
	for (int i = 0; i < n; i++) {
		char t[MAX_LEN];
		snprintf(t, MAX_LEN, "s%d", i);
		int l = strlen(t);
		m.off[i] = p;
		m.len[i] = l;
		memcpy(m.buf + p, t, l + 1);
		p += l + 1;
		m.sz = p;
	}
	return m;
}
void m3_add(M3 *m, int id) {
	if (m->cnt >= 1500)
		return;
	char t[MAX_LEN];
	snprintf(t, MAX_LEN, "s%d", id);
	int l = strlen(t);
	if (m->sz + l + 1 >= m->cap) {
		m->cap *= 2;
		m->buf = realloc(m->buf, m->cap);
	}
	m->off = realloc(m->off, (m->cnt + 1) * sizeof(int));
	m->len = realloc(m->len, (m->cnt + 1) * sizeof(int));
	m->off[m->cnt] = m->sz;
	m->len[m->cnt] = l;
	memcpy(m->buf + m->sz, t, l + 1);
	m->sz += l + 1;
	m->cnt++;
}
void m3_del(M3 *m, int i) {
	if (i < 0 || i >= m->cnt || m->cnt == 0)
		return;
	int o = m->off[i], ol = m->len[i], sh = m->sz - (o + ol + 1);
	if (sh > 0)
		memmove(m->buf + o, m->buf + o + ol + 1, sh);
	m->sz -= ol + 1;
	for (int j = i; j < m->cnt - 1; j++) {
		m->off[j] = m->off[j + 1] - (ol + 1);
		m->len[j] = m->len[j + 1];
	}
	m->cnt--;
}
void m3_upper(M3 m) {
	for (int i = 0; i < m.cnt; i++)
		for (int j = 0; j < m.len[i]; j++)
			m.buf[m.off[i] + j] = toupper(m.buf[m.off[i] + j]);
}
void m3_free(M3 m) {
	free(m.buf);
	free(m.off);
	free(m.len);
}

// === Method 4: Matrix ===
typedef struct {
	char (*mx)[MAX_LEN];
	int *len;
	int cnt, cap;
} M4;
M4 m4_create(int n) {
	M4 m = {malloc(n * 2 * MAX_LEN), malloc(n * 2 * sizeof(int)), n, n * 2};
	for (int i = 0; i < n; i++) {
		char t[MAX_LEN];
		snprintf(t, MAX_LEN, "s%d", i);
		int l = strlen(t);
		m.len[i] = l;
		memcpy(m.mx[i], t, l + 1);
	}
	return m;
}
void m4_add(M4 *m, int id) {
	if (m->cnt >= m->cap) {
		m->cap *= 2;
		m->mx = realloc(m->mx, m->cap * MAX_LEN);
		m->len = realloc(m->len, m->cap * sizeof(int));
	}
	char t[MAX_LEN];
	snprintf(t, MAX_LEN, "s%d", id);
	int l = strlen(t);
	m->len[m->cnt] = l;
	memcpy(m->mx[m->cnt], t, l + 1);
	m->cnt++;
}
void m4_del(M4 *m, int i) {
	if (i < 0 || i >= m->cnt || m->cnt == 0)
		return;
	if (i < m->cnt - 1) {
		m->len[i] = m->len[m->cnt - 1];
		memcpy(m->mx[i], m->mx[m->cnt - 1], MAX_LEN);
	}
	m->cnt--;
}
void m4_upper(M4 m) {
	for (int i = 0; i < m.cnt; i++)
		for (int j = 0; j < m.len[i]; j++)
			m.mx[i][j] = toupper(m.mx[i][j]);
}
void m4_free(M4 m) {
	free(m.mx);
	free(m.len);
}

int main() {
	FILE *csv = fopen("results/full_sweep.csv", "w");
	fprintf(csv, "lifecycle,fixed,grow,m1_ns,m2_ns,m3_ns,m4_ns,winner\n");

	int seed = 42;
	printf("Full workload sweep (lifecycle x fixed x grow)...\n");

	for (int lc = 0; lc <= 100; lc += 20) {
		for (int fx = 0; fx <= 100 - lc; fx += 20) {
			int gr = 100 - lc - fx;

			long long t1, t2, t3, t4;

			// M1
			{
				M1 m = m1_create(INITIAL);
				Timer tm = timer_start();
				for (int it = 0; it < ITERS; it++) {
					int ln = (50 * lc) / 100;
					for (int i = 0; i < ln / 2; i++) {
						m1_del(&m, random_int(&seed, m.cnt));
						m1_add(&m, 9000 + i);
					}
					if (fx > 0)
						m1_upper(m);
				}
				t1 = timer_end(tm);
				m1_free(m);
			}

			// M2
			{
				M2 m = m2_create(INITIAL);
				Timer tm = timer_start();
				for (int it = 0; it < ITERS; it++) {
					int ln = (50 * lc) / 100;
					for (int i = 0; i < ln / 2; i++) {
						m2_del(&m, random_int(&seed, m.cnt));
						m2_add(&m, 9000 + i);
					}
					if (fx > 0)
						m2_upper(m);
				}
				t2 = timer_end(tm);
				m2_free(m);
			}

			// M3
			{
				M3 m = m3_create(INITIAL);
				Timer tm = timer_start();
				for (int it = 0; it < ITERS; it++) {
					int ln = (50 * lc) / 100;
					for (int i = 0; i < ln / 2; i++) {
						m3_del(&m, random_int(&seed, m.cnt));
						m3_add(&m, 9000 + i);
					}
					if (fx > 0)
						m3_upper(m);
				}
				t3 = timer_end(tm);
				m3_free(m);
			}

			// M4
			{
				M4 m = m4_create(INITIAL);
				Timer tm = timer_start();
				for (int it = 0; it < ITERS; it++) {
					int ln = (50 * lc) / 100;
					for (int i = 0; i < ln / 2; i++) {
						m4_del(&m, random_int(&seed, m.cnt));
						m4_add(&m, 9000 + i);
					}
					if (fx > 0)
						m4_upper(m);
				}
				t4 = timer_end(tm);
				m4_free(m);
			}

			long long mn = t1;
			int w = 1;
			if (t2 < mn) {
				mn = t2;
				w = 2;
			}
			if (t3 < mn) {
				mn = t3;
				w = 3;
			}
			if (t4 < mn) {
				mn = t4;
				w = 4;
			}

			printf("lc:%3d fx:%3d gr:%3d | M1:%8lld M2:%8lld M3:%8lld M4:%8lld | W:%d\n", lc, fx, gr, t1 / ITERS,
			       t2 / ITERS, t3 / ITERS, t4 / ITERS, w);
			fprintf(csv, "%d,%d,%d,%lld,%lld,%lld,%lld,%d\n", lc, fx, gr, t1 / ITERS, t2 / ITERS, t3 / ITERS,
			        t4 / ITERS, w);
		}
	}

	fclose(csv);
	printf("Saved to results/full_sweep.csv\n");
	return 0;
}
