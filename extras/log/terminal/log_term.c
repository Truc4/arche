/* Terminal backend shim for the arche `log` device. Auto-discovered + compiled because it sits in the
 * log/terminal/ variant folder; pure libc, so no `#link`. This STRONG log_be_emit overrides the weak
 * default in runtime/log.c when ARCHE_SELECT=log=terminal (the default selection).
 *
 * Leveled routing (level: 0 debug, 1 info, 2 warn, 3 error): warn/error to stderr (fd 2), info/debug to
 * stdout (fd 1). The message is a (ptr,len) pair — arche `[]char` lowers to a bare pointer, so the length
 * comes as a separate arg (no strlen). Kept deliberately plain: it is the explicit form of what the weak
 * default does, so a program that selects `log=terminal` behaves identically to a bare one. */
#include <unistd.h>

void log_be_emit(int level, const char *p, int n) {
	(void)write(level >= 2 ? 2 : 1, p, n);
}
