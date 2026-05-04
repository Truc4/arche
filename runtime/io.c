#include <stdio.h>

#define ARCHE_MAX_FILES 64
static FILE *arche_files[ARCHE_MAX_FILES];

static void __attribute__((constructor)) init_stdio(void) {
	arche_files[0] = stdin;
	arche_files[1] = stdout;
	arche_files[2] = stderr;
}

int arche_fopen_write(const char *path) {
	for (int i = 0; i < ARCHE_MAX_FILES; i++) {
		if (!arche_files[i]) {
			arche_files[i] = fopen(path, "w");
			return arche_files[i] ? i + 1 : 0;
		}
	}
	return 0;
}

void arche_fwrite(int fd, const char *buf, int n) {
	if (fd > 0 && fd <= ARCHE_MAX_FILES && arche_files[fd - 1])
		fwrite(buf, 1, n, arche_files[fd - 1]);
}

void arche_fclose(int fd) {
	if (fd > 0 && fd <= ARCHE_MAX_FILES && arche_files[fd - 1]) {
		fclose(arche_files[fd - 1]);
		arche_files[fd - 1] = NULL;
	}
}

int arche_fopen_read(const char *path) {
	for (int i = 0; i < ARCHE_MAX_FILES; i++) {
		if (!arche_files[i]) {
			arche_files[i] = fopen(path, "r");
			return arche_files[i] ? i + 1 : 0;
		}
	}
	return 0;
}

int arche_fread(int fd, char *buf, int n) {
	if (fd > 0 && fd <= ARCHE_MAX_FILES && arche_files[fd - 1])
		return fread(buf, 1, n, arche_files[fd - 1]);
	return 0;
}
