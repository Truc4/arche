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

// Modify: randomly grow, shrink, or keep same
void modify_string(char *str, int *seed) {
	int op = (*seed = (*seed * 1103515245 + 12345) & 0x7fffffff) % 3;
	int len = strlen(str);

	if (op == 0 && len > 5) {
		// Shrink
		int newlen = len / 2;
		str[newlen] = '\0';
	} else if (op == 1 && len < MAX_LEN - 10) {
		// Grow
		strcat(str, "_mod");
	}
	// else: stay same
}

// === M1 ===
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
void m1_modify(M1 m, int *seed) {
	for (int i = 0; i < m.cnt; i++) {
		// On grow: realloc
		int oldlen = strlen(m.strs[i]);
		modify_string(m.strs[i], seed);
		int newlen = strlen(m.strs[i]);
		if (newlen > oldlen) {
			m.strs[i] = realloc(m.strs[i], newlen + MAX_LEN / 2);
		}
	}
}
void m1_free(M1 m) {
	for (int i = 0; i < m.cnt; i++)
		free(m.strs[i]);
	free(m.strs);
}

// === M4 ===
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
void m4_modify(M4 m, int *seed) {
	for (int i = 0; i < m.cnt; i++) {
		int op = (*seed = (*seed * 1103515245 + 12345) & 0x7fffffff) % 3;
		int len = m.len[i];

		if (op == 0 && len > 5) {
			m.len[i] = len / 2;
			m.mx[i][m.len[i]] = '\0';
		} else if (op == 1 && len < MAX_LEN - 10) {
			memcpy(m.mx[i] + len, "_mod", 4);
			m.len[i] = len + 4;
			m.mx[i][m.len[i]] = '\0';
		}
	}
}
void m4_free(M4 m) {
	free(m.mx);
	free(m.len);
}

int main() {
	FILE *csv = fopen("results/realistic_sweep.csv", "w");
	fprintf(csv, "lifecycle,modify,m1_ns,m4_ns,winner\n");

	int seed = 42;
	printf("Realistic sweep: lifecycle vs modify operations...\n");

	for (int lc = 0; lc <= 100; lc += 10) {
		int mo = 100 - lc;

		long long t1, t4;

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
				if (mo > 0)
					m1_modify(m, &seed);
			}
			t1 = timer_end(tm);
			m1_free(m);
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
				if (mo > 0)
					m4_modify(m, &seed);
			}
			t4 = timer_end(tm);
			m4_free(m);
		}

		int w = (t1 < t4) ? 1 : 4;

		printf("lc:%3d mo:%3d | M1:%8lld M4:%8lld | Winner:%d | Speedup:%.1fx\n", lc, mo, t1 / ITERS, t4 / ITERS, w,
		       (double)t1 / (double)t4);
		fprintf(csv, "%d,%d,%lld,%lld,%d\n", lc, mo, t1 / ITERS, t4 / ITERS, w);
	}

	fclose(csv);
	printf("Saved to results/realistic_sweep.csv\n");
	return 0;
}
