#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define INITIAL 200
#define MAX_LEN 100
#define ITERS 5

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
		int op = (*seed = (*seed * 1103515245 + 12345) & 0x7fffffff) % 3;
		int len = strlen(m.strs[i]);
		if (op == 0 && len > 5) {
			int newlen = len / 2;
			m.strs[i][newlen] = '\0';
		} else if (op == 1 && len < MAX_LEN - 5) {
			int newlen = len + 5;
			m.strs[i] = realloc(m.strs[i], newlen + 1);
			m.strs[i][len] = 'x';
			m.strs[i][newlen] = '\0';
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
		} else if (op == 1 && len < MAX_LEN - 5) {
			m.len[i] = len + 5;
			m.mx[i][m.len[i]] = '\0';
		}
	}
}
void m4_free(M4 m) {
	free(m.mx);
	free(m.len);
}

int main() {
	FILE *csv = fopen("results/exhaustive_2d.csv", "w");
	fprintf(csv, "lifecycle,modify,m1_ns,m4_ns,winner\n");

	printf("2D exhaustive sweep: every 10%% combination...\n");

	for (int lc = 0; lc <= 100; lc += 10) {
		for (int mo = 0; mo <= 100; mo += 10) {
			int seed = 42;
			long long t1, t4;

			// M1
			{
				M1 m = m1_create(INITIAL);
				Timer tm = timer_start();
				for (int it = 0; it < ITERS; it++) {
					int lc_ops = (50 * lc) / 100;
					int mo_ops = (50 * mo) / 100;

					for (int i = 0; i < lc_ops / 2; i++) {
						m1_del(&m, random_int(&seed, m.cnt));
						m1_add(&m, 9000 + i);
					}

					for (int i = 0; i < mo_ops; i++)
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
					int lc_ops = (50 * lc) / 100;
					int mo_ops = (50 * mo) / 100;

					for (int i = 0; i < lc_ops / 2; i++) {
						m4_del(&m, random_int(&seed, m.cnt));
						m4_add(&m, 9000 + i);
					}

					for (int i = 0; i < mo_ops; i++)
						m4_modify(m, &seed);
				}
				t4 = timer_end(tm);
				m4_free(m);
			}

			int w = (t1 < t4) ? 1 : 4;
			double ratio = (t1 > 0) ? (double)t1 / (double)t4 : 0;

			printf("lc:%3d mo:%3d | M1:%7lld M4:%7lld | W:%d | %.2fx\n", lc, mo, t1 / ITERS, t4 / ITERS, w, ratio);
			fprintf(csv, "%d,%d,%lld,%lld,%d\n", lc, mo, t1 / ITERS, t4 / ITERS, w);
		}
	}

	fclose(csv);
	printf("\nSaved to results/exhaustive_2d.csv\n");

	// Count wins
	FILE *in = fopen("results/exhaustive_2d.csv", "r");
	int m1_wins = 0, m4_wins = 0;
	char line[100];
	fgets(line, 100, in); // skip header
	while (fgets(line, 100, in)) {
		int w = line[strlen(line) - 2] - '0';
		if (w == 1)
			m1_wins++;
		else if (w == 4)
			m4_wins++;
	}
	fclose(in);

	printf("\nWins: M1=%d (%.1f%%), M4=%d (%.1f%%)\n", m1_wins, 100.0 * m1_wins / (m1_wins + m4_wins), m4_wins,
	       100.0 * m4_wins / (m1_wins + m4_wins));

	return 0;
}
