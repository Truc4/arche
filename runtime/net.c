#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

/* TCP sockets for the `extern Socket(N)` extern type.
 *
 * Mirrors runtime/io.c: the C ABI takes/returns native pointers, and the
 * codegen-emitted marshal layer wraps the returned pointer into an opaque
 * `handle(Socket)` (slot table + generation counter + stale-handle aborts).
 * A socket fd is an int, not a pointer, so we box it in a malloc'd int* —
 * the same shape as a FILE* handle. Returning NULL routes through Arche's
 * null-handle path. */

static int *wrap(int fd) {
	int *p = malloc(sizeof(int));
	if (p)
		*p = fd;
	return p;
}

/* socket()+bind()+listen() on 0.0.0.0:port. NULL on any failure.
 * Arche can't build a `struct sockaddr`, so it is constructed here from the
 * scalar args — the one residual FFI limitation. */
int *net_listen(int port) {
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return NULL;
	int yes = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
	struct sockaddr_in addr = {0};
	addr.sin_family = AF_INET;
	addr.sin_port = htons((unsigned short)port);
	addr.sin_addr.s_addr = INADDR_ANY;
	if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0 || listen(fd, 1) < 0) {
		close(fd);
		return NULL;
	}
	return wrap(fd);
}

/* Block until a peer connects; returns a NEW handle for the accepted
 * connection. The listening socket handle stays valid for more accepts. */
int *net_accept(int *listener) {
	if (!listener)
		return NULL;
	int fd = accept(*listener, NULL, NULL);
	if (fd < 0)
		return NULL;
	return wrap(fd);
}

/* socket()+connect() to ip:port. NULL on failure. */
int *net_connect(const char *ip, int port) {
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return NULL;
	struct sockaddr_in addr = {0};
	addr.sin_family = AF_INET;
	addr.sin_port = htons((unsigned short)port);
	if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1 ||
	    connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
		close(fd);
		return NULL;
	}
	return wrap(fd);
}

/* Returns bytes sent, or -1 on error. */
int net_send(int *s, const char *buf, int n) {
	if (!s)
		return -1;
	return (int)send(*s, buf, n, 0);
}

/* Returns bytes received, 0 on orderly peer close, or -1 on error. */
int net_recv(int *s, char *buf, int n) {
	if (!s)
		return -1;
	return (int)recv(*s, buf, n, 0);
}

void net_close(int *s) {
	if (s) {
		close(*s);
		free(s);
	}
}
