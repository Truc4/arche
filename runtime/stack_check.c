#define _POSIX_C_SOURCE 200112L
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef SIGSTKSZ
#define SIGSTKSZ 8192
#endif

#ifndef SA_ONSTACK
#define SA_ONSTACK 0x08000000
#endif

typedef struct {
	void *ss_sp;
	int ss_flags;
	size_t ss_size;
} stack_t;

int sigaltstack(const stack_t *ss, stack_t *old_ss);

static const char msg[] = "stack overflow\n";
static char alt_stack[8192];

static void segv_handler(int sig, siginfo_t *info, void *context) {
	(void)sig;
	(void)info;
	(void)context;
	write(2, msg, sizeof(msg) - 1);
	_exit(0);
}

__attribute__((constructor)) void init_runtime(void) {
	stack_t ss;
	memset(&ss, 0, sizeof(ss));
	ss.ss_sp = alt_stack;
	ss.ss_size = SIGSTKSZ;
	sigaltstack(&ss, NULL);

	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = segv_handler;
	sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
	sigaction(SIGSEGV, &sa, NULL);
}
