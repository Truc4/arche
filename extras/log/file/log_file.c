/* File backend shim for the arche `log` device. Auto-discovered + compiled from the log/file/ variant
 * folder; pure libc, no `#link`. This STRONG log_be_emit overrides the weak default in runtime/log.c when
 * ARCHE_SELECT=log=file, so a selected program's app logs AND its panics land in the file, not the terminal.
 *
 * The sink path is the LOG_FILE env var, or ./arche.log by default. The fd is opened once (lazily) in
 * append mode and reused. level is ignored (every level goes to the one file). Message is (ptr,len). */
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

static int log_fd = -1;

void log_be_emit(int level, const char *p, int n) {
	(void)level;
	if (log_fd < 0) {
		const char *path = getenv("LOG_FILE");
		if (!path || !*path)
			path = "arche.log";
		log_fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
		if (log_fd < 0)
			return;
	}
	(void)write(log_fd, p, n);
}
