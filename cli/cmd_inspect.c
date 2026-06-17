/* `arche inspect` — view/edit the live pool state of a running `arche run` session. The host (built in
 * hot mode) serves its pools over a Unix socket (runtime/inspect.c); this is the client. In arche all
 * state lives in the driver-owned pools, so this shows the program's entire live state.
 *
 * Default (a tty): an interactive TUI — a live-refreshing table you navigate with the arrow keys and edit
 * in place. The one-shot subcommands below are the scripting/pipe path (and the fallback when stdout is
 * not a tty):
 *
 *   arche inspect                                        interactive TUI (or `tui [pool]`)
 *   arche inspect [--sock <path>] list                   list pools
 *   arche inspect [--sock <path>] schema <pool>          show a pool's fields
 *   arche inspect [--sock <path>] rows <pool> [start n]  print live rows as a table
 *   arche inspect [--sock <path>] poke <pool> <slot> <field> <value>   edit one live cell
 *
 * Wire protocol + safety model are documented in runtime/inspect.c. The default socket path is
 * $ARCHE_INSPECT_SOCK, else build/.arche-hot/inspect.sock under the current directory. */
#include "../runtime/inspect.h"
#include "args.h"
#include "cli.h"
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <termios.h>
#include <unistd.h>

enum { OPT_SOCK = 1 };

static const ArgSpec k_inspect_specs[] = {
    {OPT_SOCK, "--sock", ARG_VALUE, 0, 0, "<path>", "inspect socket path (default $ARCHE_INSPECT_SOCK)"},
    {0, NULL, ARG_FLAG, 0, 0, NULL, NULL},
};

const ArgSpec *inspect_specs(void) {
	return k_inspect_specs;
}

/* ---- buffered socket reader ------------------------------------------------------------------------- */

typedef struct {
	int fd;
	unsigned char buf[8192];
	int len, pos;
} Reader;

static int rd_fill(Reader *r) {
	if (r->pos < r->len)
		return 1;
	ssize_t n = read(r->fd, r->buf, sizeof(r->buf));
	if (n <= 0)
		return 0;
	r->len = (int)n;
	r->pos = 0;
	return 1;
}

/* Read one '\n'-terminated line (newline stripped) into `out`. Returns 0 on success, -1 on EOF/timeout. */
static int rd_line(Reader *r, char *out, size_t cap) {
	size_t o = 0;
	for (;;) {
		if (!rd_fill(r))
			return -1;
		unsigned char c = r->buf[r->pos++];
		if (c == '\n') {
			out[o < cap ? o : cap - 1] = '\0';
			return 0;
		}
		if (o + 1 < cap)
			out[o++] = (char)c;
	}
}

/* Read exactly `n` raw bytes into `out`. Returns 0 on success, -1 on short read. */
static int rd_bytes(Reader *r, unsigned char *out, size_t n) {
	size_t got = 0;
	while (got < n) {
		if (!rd_fill(r))
			return -1;
		int avail = r->len - r->pos;
		int take = (int)(n - got) < avail ? (int)(n - got) : avail;
		memcpy(out + got, r->buf + r->pos, (size_t)take);
		r->pos += take;
		got += (size_t)take;
	}
	return 0;
}

/* ---- connection ------------------------------------------------------------------------------------- */

static const char *resolve_sock(const ArgParse *p) {
	const char *s = args_value(p, OPT_SOCK);
	if (s)
		return s;
	s = getenv("ARCHE_INSPECT_SOCK");
	if (s)
		return s;
	return "build/.arche-hot/inspect.sock";
}

/* Connect to the inspect socket and consume the "ARCHE-INSPECT <v>\n" hello. Returns fd or -1. */
static int inspect_connect(const char *path, Reader *r) {
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;
	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		close(fd);
		return -1;
	}
	/* A live host services the socket only at its device-call cadence; bound the wait so we don't hang
	 * forever against a program that has gone quiet. */
	struct timeval tv = {5, 0};
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	r->fd = fd;
	r->len = r->pos = 0;
	char hello[64];
	if (rd_line(r, hello, sizeof(hello)) != 0 || strncmp(hello, "ARCHE-INSPECT", 13) != 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static int send_req(int fd, const char *line) {
	size_t n = strlen(line);
	return (write(fd, line, n) == (ssize_t)n) ? 0 : -1;
}

/* ---- schema cache ----------------------------------------------------------------------------------- */

typedef struct {
	char name[64];
	int tag, kind;
	long ecount, esize;
} FieldDesc;

/* Fetch a pool's schema. Returns field count (>=0), or -1 on error (message printed). */
static int fetch_schema(int fd, Reader *r, const char *pool, FieldDesc *out, int max) {
	char req[256];
	snprintf(req, sizeof(req), "SCHEMA %s\n", pool);
	if (send_req(fd, req) != 0)
		return -1;
	char line[512];
	if (rd_line(r, line, sizeof(line)) != 0)
		return -1;
	int fc = 0;
	if (sscanf(line, "OK %d", &fc) != 1) {
		fprintf(stderr, "arche inspect: %s\n", line);
		return -1;
	}
	if (fc > max)
		fc = max;
	for (int i = 0; i < fc; i++) {
		if (rd_line(r, line, sizeof(line)) != 0)
			return -1;
		FieldDesc *d = &out[i];
		if (sscanf(line, "%63s %d %d %ld %ld", d->name, &d->tag, &d->kind, &d->ecount, &d->esize) != 5)
			return -1;
	}
	return fc;
}

/* ---- value formatting ------------------------------------------------------------------------------- */

static long long read_signed(const unsigned char *p, long sz) {
	long long v = 0;
	memcpy(&v, p, (size_t)(sz < 8 ? sz : 8));
	/* sign-extend from the high bit of the stored width */
	if (sz < 8) {
		long long sign = 1LL << (sz * 8 - 1);
		if (v & sign)
			v |= -(1LL << (sz * 8));
	}
	return v;
}

static unsigned long long read_unsigned(const unsigned char *p, long sz) {
	unsigned long long v = 0;
	memcpy(&v, p, (size_t)(sz < 8 ? sz : 8));
	return v;
}

/* Format one cell (a column's per-row element block) into `out`. */
static void fmt_cell(char *out, size_t cap, const FieldDesc *d, const unsigned char *cell) {
	switch (d->tag) {
	case AIT_F32: {
		float f;
		memcpy(&f, cell, 4);
		snprintf(out, cap, "%g", (double)f);
		return;
	}
	case AIT_CHAR:
		if (d->ecount > 1) { /* char[N] → string */
			size_t o = 0;
			for (long i = 0; i < d->ecount && cell[i] && o + 1 < cap; i++)
				out[o++] = (char)cell[i];
			out[o] = '\0';
			return;
		}
		snprintf(out, cap, "'%c'", cell[0] ? cell[0] : ' ');
		return;
	case AIT_HANDLE: {
		unsigned long long h = read_unsigned(cell, 8);
		snprintf(out, cap, "%u:%u", (unsigned)(h & 0xffffffff), (unsigned)(h >> 32));
		return;
	}
	case AIT_OPAQUE:
		snprintf(out, cap, "0x%llx", read_unsigned(cell, 8));
		return;
	case AIT_U8:
	case AIT_U16:
	case AIT_U32:
	case AIT_U64:
	case AIT_U128:
		snprintf(out, cap, "%llu", read_unsigned(cell, d->esize));
		return;
	default:
		snprintf(out, cap, "%lld", read_signed(cell, d->esize));
		return;
	}
}

/* Encode a textual value into `bytes` (esize wide) per the field's type. Returns 0 on success. */
static int encode_value(const FieldDesc *d, const char *text, unsigned char *bytes) {
	memset(bytes, 0, (size_t)d->esize);
	if (d->ecount != 1)
		return -1; /* array cells not pokeable via this simple path */
	switch (d->tag) {
	case AIT_F32: {
		float f = (float)strtod(text, NULL);
		memcpy(bytes, &f, 4);
		return 0;
	}
	case AIT_CHAR: {
		bytes[0] = (unsigned char)text[0];
		return 0;
	}
	case AIT_U8:
	case AIT_U16:
	case AIT_U32:
	case AIT_U64:
	case AIT_U128: {
		unsigned long long v = strtoull(text, NULL, 0);
		memcpy(bytes, &v, (size_t)d->esize);
		return 0;
	}
	default: {
		long long v = strtoll(text, NULL, 0);
		memcpy(bytes, &v, (size_t)d->esize);
		return 0;
	}
	}
}

/* Round a double to the nearest integer without pulling in libm (the CLI does not link -lm). */
static long long round_ll(double x) {
	return (long long)(x < 0 ? x - 0.5 : x + 0.5);
}

/* Add `delta` to a numeric cell's current value, writing esize bytes into `bytes`. Float deltas apply
 * directly; integer cells round the delta to the nearest whole step. Returns 0 on success, -1 if the
 * field is not an adjustable scalar number (char/handle/opaque/array cells). */
static int delta_value(const FieldDesc *d, const unsigned char *cur, double delta, unsigned char *bytes) {
	if (d->ecount != 1)
		return -1;
	switch (d->tag) {
	case AIT_F32: {
		float f;
		memcpy(&f, cur, 4);
		f = (float)((double)f + delta);
		memcpy(bytes, &f, 4);
		return 0;
	}
	case AIT_U8:
	case AIT_U16:
	case AIT_U32:
	case AIT_U64:
	case AIT_U128: {
		unsigned long long v = read_unsigned(cur, d->esize);
		v = (unsigned long long)((long long)v + round_ll(delta));
		memcpy(bytes, &v, (size_t)d->esize);
		return 0;
	}
	case AIT_I8:
	case AIT_I16:
	case AIT_I32:
	case AIT_I64:
	case AIT_I128: {
		long long v = read_signed(cur, d->esize);
		v += round_ll(delta);
		memcpy(bytes, &v, (size_t)d->esize);
		return 0;
	}
	default:
		return -1; /* char/handle/opaque are not arithmetic */
	}
}

/* ---- subcommands ------------------------------------------------------------------------------------ */

static int do_list(int fd, Reader *r) {
	if (send_req(fd, "LIST\n") != 0)
		return ARCHE_ERR;
	char line[512];
	if (rd_line(r, line, sizeof(line)) != 0)
		return ARCHE_ERR;
	int n = 0;
	if (sscanf(line, "OK %d", &n) != 1) {
		fprintf(stderr, "arche inspect: %s\n", line);
		return ARCHE_ERR;
	}
	printf("%-24s %-8s %-10s %s\n", "POOL", "KIND", "CAPACITY", "FIELDS");
	for (int i = 0; i < n; i++) {
		if (rd_line(r, line, sizeof(line)) != 0)
			return ARCHE_ERR;
		char name[64];
		int dyn, fc;
		long cap;
		if (sscanf(line, "%63s %d %ld %d", name, &dyn, &cap, &fc) != 4)
			continue;
		printf("%-24s %-8s %-10ld %d\n", name, dyn ? "dynamic" : "static", cap, fc);
	}
	return ARCHE_OK;
}

static int do_schema(int fd, Reader *r, const char *pool) {
	FieldDesc f[ARCHE_INSPECT_MAX_FIELDS];
	int fc = fetch_schema(fd, r, pool, f, ARCHE_INSPECT_MAX_FIELDS);
	if (fc < 0)
		return ARCHE_ERR;
	printf("%-20s %-8s %-8s %s\n", "FIELD", "KIND", "ELEMS", "BYTES/ELEM");
	for (int i = 0; i < fc; i++)
		printf("%-20s %-8s %-8ld %ld\n", f[i].name, f[i].kind == ARCHE_INSPECT_FIELD_COLUMN ? "column" : "meta",
		       f[i].ecount, f[i].esize);
	return ARCHE_OK;
}

static int do_rows(int fd, Reader *r, const char *pool, long start, long count) {
	FieldDesc f[ARCHE_INSPECT_MAX_FIELDS];
	int fc = fetch_schema(fd, r, pool, f, ARCHE_INSPECT_MAX_FIELDS);
	if (fc < 0)
		return ARCHE_ERR;

	char req[256];
	snprintf(req, sizeof(req), "ROWS %s %ld %ld\n", pool, start, count);
	if (send_req(fd, req) != 0)
		return ARCHE_ERR;
	char line[512];
	if (rd_line(r, line, sizeof(line)) != 0)
		return ARCHE_ERR;
	long nrows = 0, row_bytes = 0;
	if (sscanf(line, "OK %ld %ld", &nrows, &row_bytes) != 2) {
		fprintf(stderr, "arche inspect: %s\n", line);
		return ARCHE_ERR;
	}

	/* Header: slot, gen, then one column header per COLUMN field. */
	printf("%-8s %-6s", "slot", "gen");
	for (int i = 0; i < fc; i++)
		if (f[i].kind == ARCHE_INSPECT_FIELD_COLUMN)
			printf(" %-12s", f[i].name);
	printf("\n");

	size_t total = (size_t)(nrows * row_bytes);
	unsigned char *blob = (unsigned char *)malloc(total ? total : 1);
	if (!blob)
		return ARCHE_ERR;
	if (nrows > 0 && rd_bytes(r, blob, total) != 0) {
		free(blob);
		fprintf(stderr, "arche inspect: short row payload\n");
		return ARCHE_ERR;
	}
	for (long row = 0; row < nrows; row++) {
		const unsigned char *p = blob + row * row_bytes;
		long long slot;
		int gen;
		memcpy(&slot, p, 8);
		memcpy(&gen, p + 8, 4);
		const unsigned char *cell = p + 12;
		printf("%-8lld %-6d", slot, gen);
		for (int i = 0; i < fc; i++) {
			if (f[i].kind != ARCHE_INSPECT_FIELD_COLUMN)
				continue;
			char buf[128];
			fmt_cell(buf, sizeof(buf), &f[i], cell);
			printf(" %-12s", buf);
			cell += f[i].ecount * f[i].esize;
		}
		printf("\n");
	}
	free(blob);
	printf("(%ld rows)\n", nrows);
	return ARCHE_OK;
}

static int do_poke(int fd, Reader *r, const char *pool, long slot, const char *field, const char *value) {
	FieldDesc f[ARCHE_INSPECT_MAX_FIELDS];
	int fc = fetch_schema(fd, r, pool, f, ARCHE_INSPECT_MAX_FIELDS);
	if (fc < 0)
		return ARCHE_ERR;
	int fidx = -1;
	for (int i = 0; i < fc; i++)
		if (strcmp(f[i].name, field) == 0) {
			fidx = i;
			break;
		}
	if (fidx < 0) {
		fprintf(stderr, "arche inspect: no field '%s' in pool '%s'\n", field, pool);
		return ARCHE_ERR;
	}

	/* Find the slot's current generation by scanning the live rows. */
	char req[256];
	snprintf(req, sizeof(req), "ROWS %s 0 -1\n", pool);
	if (send_req(fd, req) != 0)
		return ARCHE_ERR;
	char line[512];
	if (rd_line(r, line, sizeof(line)) != 0)
		return ARCHE_ERR;
	long nrows = 0, row_bytes = 0;
	if (sscanf(line, "OK %ld %ld", &nrows, &row_bytes) != 2) {
		fprintf(stderr, "arche inspect: %s\n", line);
		return ARCHE_ERR;
	}
	size_t total = (size_t)(nrows * row_bytes);
	unsigned char *blob = (unsigned char *)malloc(total ? total : 1);
	if (!blob)
		return ARCHE_ERR;
	if (nrows > 0 && rd_bytes(r, blob, total) != 0) {
		free(blob);
		return ARCHE_ERR;
	}
	int gen = -1;
	for (long row = 0; row < nrows; row++) {
		long long s;
		memcpy(&s, blob + row * row_bytes, 8);
		if (s == slot) {
			memcpy(&gen, blob + row * row_bytes + 8, 4);
			break;
		}
	}
	free(blob);
	if (gen < 0) {
		fprintf(stderr, "arche inspect: slot %ld is not live in pool '%s'\n", slot, pool);
		return ARCHE_ERR;
	}

	unsigned char bytes[16];
	if (encode_value(&f[fidx], value, bytes) != 0) {
		fprintf(stderr, "arche inspect: cannot encode value for field '%s'\n", field);
		return ARCHE_ERR;
	}
	char hex[40];
	int ho = 0;
	for (long i = 0; i < f[fidx].esize; i++)
		ho += snprintf(hex + ho, sizeof(hex) - ho, "%02x", bytes[i]);

	snprintf(req, sizeof(req), "POKE %s %ld %d %d 0 %s\n", pool, slot, gen, fidx, hex);
	if (send_req(fd, req) != 0)
		return ARCHE_ERR;
	if (rd_line(r, line, sizeof(line)) != 0)
		return ARCHE_ERR;
	if (strncmp(line, "OK", 2) == 0) {
		printf("ok: %s.%s[slot %ld] = %s\n", pool, field, slot, value);
		return ARCHE_OK;
	}
	fprintf(stderr, "arche inspect: %s\n", line);
	return ARCHE_ERR;
}

/* ---- interactive TUI -------------------------------------------------------------------------------- */

/* Raw-mode terminal setup. We restore the original termios + leave the alternate screen on every exit
 * path (normal quit, SIGINT/SIGTERM) so a crash never leaves the user's terminal wedged. */
static struct termios g_tui_orig;
static int g_tui_raw = 0;

static void tui_restore(void) {
	if (!g_tui_raw)
		return;
	tcsetattr(STDIN_FILENO, TCSANOW, &g_tui_orig);
	/* show cursor, leave alternate screen */
	if (write(STDOUT_FILENO, "\x1b[?25h\x1b[?1049l", 14) < 0) { /* nothing we can do on failure */
	}
	g_tui_raw = 0;
}

static void tui_on_signal(int sig) {
	(void)sig;
	tui_restore();
	_exit(1);
}

static int tui_enter(void) {
	if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))
		return -1;
	if (tcgetattr(STDIN_FILENO, &g_tui_orig) != 0)
		return -1;
	struct termios raw = g_tui_orig;
	/* no canonical line buffering, no echo; ISIG off so Ctrl-C arrives as a byte we handle as quit. */
	raw.c_lflag &= ~(unsigned)(ICANON | ECHO | ISIG);
	raw.c_iflag &= ~(unsigned)(IXON | ICRNL);
	raw.c_cc[VMIN] = 0;
	raw.c_cc[VTIME] = 2; /* read() returns after 0.2s with no input → drives the live-refresh tick */
	if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0)
		return -1;
	g_tui_raw = 1;
	atexit(tui_restore);
	signal(SIGINT, tui_on_signal);
	signal(SIGTERM, tui_on_signal);
	/* When the host dies, writing a request to the now-dead socket would raise SIGPIPE and kill us before
	 * we ever see the failed read. Ignore it so send_req just returns -1 and we drop to the reconnect path. */
	signal(SIGPIPE, SIG_IGN);
	/* enter alternate screen, hide cursor */
	if (write(STDOUT_FILENO, "\x1b[?1049h\x1b[?25l", 14) < 0) {
	}
	return 0;
}

static void tui_term_size(int *rows, int *cols) {
	struct winsize ws;
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0) {
		*rows = ws.ws_row;
		*cols = ws.ws_col;
	} else {
		*rows = 24;
		*cols = 80;
	}
}

/* Keys returned by tui_read_key (printable chars pass through as their byte value). */
enum { K_NONE = -1, K_UP = 256, K_DOWN, K_LEFT, K_RIGHT, K_ENTER, K_ESC, K_QUIT };

/* Byte buffer so multiple bytes from one read() (fast typing, key repeat, `5j` counts, CSI sequences)
 * are each delivered as a separate key on successive calls — never dropped. */
static unsigned char g_kbuf[64];
static int g_klen = 0, g_kpos = 0;

static int kb_next(void) {
	if (g_kpos >= g_klen) {
		ssize_t n = read(STDIN_FILENO, g_kbuf, sizeof(g_kbuf));
		if (n <= 0)
			return -1; /* VTIME tick / no input */
		g_klen = (int)n;
		g_kpos = 0;
	}
	return g_kbuf[g_kpos++];
}

static int tui_read_key(void) {
	int c = kb_next();
	if (c < 0)
		return K_NONE;
	if (c == 27) { /* ESC alone, or an arrow CSI sequence (bytes arrive together) */
		int d = kb_next();
		if (d == '[') {
			switch (kb_next()) {
			case 'A':
				return K_UP;
			case 'B':
				return K_DOWN;
			case 'C':
				return K_RIGHT;
			case 'D':
				return K_LEFT;
			}
		}
		return K_ESC;
	}
	if (c == '\r' || c == '\n')
		return K_ENTER;
	if (c == 3 || c == 'q' || c == 'Q') /* Ctrl-C / q */
		return K_QUIT;
	return c;
}

/* Fetch all live rows of `pool` into a freshly malloc'd blob (caller frees). Sets the nrows + row_bytes
 * out-params. */
static int tui_fetch_rows(int fd, Reader *r, const char *pool, unsigned char **out, long *nrows, long *row_bytes) {
	char req[256];
	snprintf(req, sizeof(req), "ROWS %s 0 -1\n", pool);
	if (send_req(fd, req) != 0)
		return -1;
	char line[512];
	if (rd_line(r, line, sizeof(line)) != 0)
		return -1;
	if (sscanf(line, "OK %ld %ld", nrows, row_bytes) != 2)
		return -1;
	size_t total = (size_t)(*nrows * *row_bytes);
	unsigned char *blob = (unsigned char *)malloc(total ? total : 1);
	if (!blob)
		return -1;
	if (*nrows > 0 && rd_bytes(r, blob, total) != 0) {
		free(blob);
		return -1;
	}
	*out = blob;
	return 0;
}

/* Modal line editor on the status row. Returns 0 with the typed text in `out` (committed with Enter), or
 * -1 if cancelled with Esc. Reads char-by-char in a temporarily-blocking mode. */
static int tui_edit_line(int term_rows, const char *prompt, const char *initial, char *out, size_t cap) {
	struct termios t;
	tcgetattr(STDIN_FILENO, &t);
	struct termios blk = t;
	blk.c_cc[VMIN] = 1;
	blk.c_cc[VTIME] = 0;
	tcsetattr(STDIN_FILENO, TCSANOW, &blk);

	size_t len = 0;
	if (initial) {
		snprintf(out, cap, "%s", initial);
		len = strlen(out);
	} else {
		out[0] = '\0';
	}
	int rc = -1;
	for (;;) {
		char buf[256];
		int o = snprintf(buf, sizeof(buf), "\x1b[%d;1H\x1b[2K\x1b[7m %s \x1b[0m %s", term_rows, prompt, out);
		if (write(STDOUT_FILENO, buf, (size_t)o) < 0) {
		}
		unsigned char c;
		ssize_t n = read(STDIN_FILENO, &c, 1);
		if (n <= 0)
			continue;
		if (c == '\r' || c == '\n') {
			rc = 0;
			break;
		}
		if (c == 27) {
			rc = -1;
			break;
		}
		if ((c == 127 || c == 8) && len > 0) {
			out[--len] = '\0';
			continue;
		}
		if (c >= 32 && c < 127 && len + 1 < cap) {
			out[len++] = (char)c;
			out[len] = '\0';
		}
	}
	tcsetattr(STDIN_FILENO, TCSANOW, &t); /* back to VMIN=0/VTIME tick */
	return rc;
}

/* Append-into-buffer helper for building a frame. */
#define APPEND(...)                                                                                                    \
	do {                                                                                                               \
		if (o < (int)sizeof(frame))                                                                                    \
			o += snprintf(frame + o, sizeof(frame) - o, __VA_ARGS__);                                                  \
	} while (0)

/* SGR palette (16-color, so it renders the same in any terminal incl. nvim's :terminal). */
#define C_RST "\x1b[0m"
#define C_TITLE "\x1b[1;97;44m"   /* bold white on blue — title bar */
#define C_TAB_ON "\x1b[1;30;46m"  /* black on cyan — active pool tab */
#define C_TAB_OFF "\x1b[2m"       /* dim — inactive pool tab */
#define C_HEAD "\x1b[1;4m"        /* bold underline — column header */
#define C_HEAD_SEL "\x1b[1;4;36m" /* + cyan — the selected column's header */
#define C_DIM "\x1b[2m"           /* dim — slot/gen cells, zebra, hints */
#define C_SEL "\x1b[1;7m"         /* bold reverse — the selected cell */
#define C_OK "\x1b[1;32m"         /* green — success status */
#define C_ERR "\x1b[1;31m"        /* red — error status */
#define C_EOL "\x1b[K"            /* clear to end of line */

/* Is this column a number (right-aligned) vs text/handle (left-aligned)? */
static int tag_is_numeric(int tag) {
	return tag != AIT_CHAR && tag != AIT_HANDLE && tag != AIT_OPAQUE;
}
static int num_digits(long long v) {
	int d = 1;
	if (v < 0) {
		d++;
		v = -v;
	}
	while (v >= 10) {
		v /= 10;
		d++;
	}
	return d;
}

/* ~1s between reconnect attempts (the read() VTIME tick is 0.2s, so 5 ticks). */
#define TUI_RECONNECT_TICKS 5

/* (Re)connect to the inspect socket and load the pool list into the caller's arrays. Returns the fd on
 * success (Reader initialized), or -1 if no host is reachable / it spoke garbage. A connected host with
 * zero registered pools is a success (npools 0). */
static int tui_open(const char *path, Reader *r, char pool_names[][64], int *pool_dyn, long *pool_cap, int *pool_fc,
                    int *npools_out) {
	int fd = inspect_connect(path, r);
	if (fd < 0)
		return -1;
	if (send_req(fd, "LIST\n") != 0) {
		close(fd);
		return -1;
	}
	char line[512];
	int npools = 0;
	if (rd_line(r, line, sizeof(line)) != 0 || sscanf(line, "OK %d", &npools) != 1 || npools < 0) {
		close(fd);
		return -1;
	}
	if (npools > 64)
		npools = 64;
	for (int i = 0; i < npools; i++) {
		if (rd_line(r, line, sizeof(line)) != 0) {
			close(fd);
			return -1;
		}
		pool_dyn[i] = pool_fc[i] = 0;
		pool_cap[i] = 0;
		sscanf(line, "%63s %d %ld %d", pool_names[i], &pool_dyn[i], &pool_cap[i], &pool_fc[i]);
	}
	*npools_out = npools;
	return fd;
}

/* Draw a minimal centered full-screen message (used while not connected, or connected with no pools).
 * Keeps the title-bar / footer chrome so the screen does not jump when a host appears. */
static void tui_message_screen(const char *title_text, const char *msg) {
	int trows, tcols;
	tui_term_size(&trows, &tcols);
	char frame[1 << 14];
	int o = 0;
	APPEND("\x1b[H");
	APPEND(C_TITLE "%-*.*s" C_RST C_EOL "\r\n", tcols, tcols, title_text);
	for (int row = 2; row < trows; row++) {
		if (row == trows / 2) {
			int pad = (tcols - (int)strlen(msg)) / 2;
			if (pad < 0)
				pad = 0;
			APPEND(C_DIM "%*s%.*s" C_RST C_EOL "\r\n", pad, "", tcols, msg);
		} else {
			APPEND(C_EOL "\r\n");
		}
	}
	APPEND("\x1b[%d;1H" C_EOL C_DIM " q quit" C_RST, trows);
	if (write(STDOUT_FILENO, frame, (size_t)o) < 0) {
	}
}

static int tui_run(const char *path, const char *start_pool) {
	if (tui_enter() != 0) {
		/* not a tty — fall back to the one-shot listing if a host is up */
		Reader r0;
		int fd0 = inspect_connect(path, &r0);
		if (fd0 < 0) {
			fprintf(stderr, "arche inspect: cannot connect to %s (is `arche run` active?)\n", path);
			return ARCHE_ERR;
		}
		int rc = do_list(fd0, &r0);
		close(fd0);
		return rc;
	}

	char pool_names[64][64];
	int pool_dyn[64], pool_fc[64];
	long pool_cap[64];
	int npools = 0;

	Reader r;
	int fd = -1;           /* <0 = not connected; the TUI runs regardless and reconnects on its own */
	int retry = 0;         /* ticks until the next reconnect attempt */
	int start_applied = 0; /* honor start_pool only on the first successful connect */
	int anim = 0;          /* frame counter for the waiting-screen spinner */

	char line[512];
	int pool_idx = 0;
	FieldDesc fields[ARCHE_INSPECT_MAX_FIELDS];
	int fcount = 0;
	int colmap[ARCHE_INSPECT_MAX_FIELDS]; /* display column -> field index (COLUMN fields only) */
	int colw[ARCHE_INSPECT_MAX_FIELDS];   /* display column -> width (chars) */
	int ncols = 0;
	int loaded_pool = -1;

	int sel_row = 0, sel_col = 0, top = 0, left_col = 0;
	double step = 1;                              /* +/- increment applied to the selected cell */
	int count = 0, have_count = 0, pending_g = 0; /* vim numeric prefix + `g` lead-in */
	int status_kind = 0;                          /* 0 none, 1 ok, 2 err */
	int status_ttl = 0;                           /* ticks to keep `status` visible instead of the hints */
	char status[256] = "";

	for (;;) {
		/* Not connected: try to (re)connect on a throttle, otherwise show a waiting screen and keep going. */
		if (fd < 0) {
			if (retry <= 0) {
				fd = tui_open(path, &r, pool_names, pool_dyn, pool_cap, pool_fc, &npools);
				if (fd < 0) {
					retry = TUI_RECONNECT_TICKS;
				} else {
					loaded_pool = -1; /* force a schema (re)load */
					if (!start_applied) {
						start_applied = 1;
						if (start_pool)
							for (int i = 0; i < npools; i++)
								if (strcmp(pool_names[i], start_pool) == 0)
									pool_idx = i;
					}
					if (pool_idx >= npools)
						pool_idx = 0;
				}
			}
			if (fd < 0) {
				/* a tiny spinner + cycling dots so the screen reads as "alive, retrying" not "frozen".
				 * dots is kept a fixed 3 chars wide (space-padded) so the centered text never jitters. */
				static const char spin[] = "|/-\\";
				char dots[4] = "   ";
				for (int i = 0; i < anim % 4; i++)
					dots[i] = '.';
				char msg[300];
				snprintf(msg, sizeof(msg), "%c  waiting for `arche run` on %s %s", spin[anim % 4], path, dots);
				tui_message_screen(" arche inspect — waiting for a host", msg);
				anim++;
				int key = tui_read_key();
				if (key == K_QUIT)
					break;
				if (key == K_NONE && retry > 0)
					retry--;
				continue;
			}
		}

		/* Connected, but the host registered no pools (yet). */
		if (npools <= 0) {
			tui_message_screen(" arche inspect — connected", "connected · host registered no pools");
			int key = tui_read_key();
			if (key == K_QUIT)
				break;
			continue;
		}

		/* (Re)load schema when the pool changes. */
		if (loaded_pool != pool_idx) {
			fcount = fetch_schema(fd, &r, pool_names[pool_idx], fields, ARCHE_INSPECT_MAX_FIELDS);
			if (fcount < 0)
				fcount = 0;
			ncols = 0;
			for (int i = 0; i < fcount; i++)
				if (fields[i].kind == ARCHE_INSPECT_FIELD_COLUMN)
					colmap[ncols++] = i;
			loaded_pool = pool_idx;
			sel_row = sel_col = top = left_col = 0;
		}

		unsigned char *blob = NULL;
		long nrows = 0, row_bytes = 0;
		if (tui_fetch_rows(fd, &r, pool_names[pool_idx], &blob, &nrows, &row_bytes) != 0) {
			/* the host went away — drop back to the waiting/reconnect state instead of exiting */
			free(blob);
			close(fd);
			fd = -1;
			retry = TUI_RECONNECT_TICKS;
			continue;
		}

		int trows, tcols;
		tui_term_size(&trows, &tcols);
		int body_rows = trows - 4; /* title + tabs + header + ... + footer */
		if (body_rows < 1)
			body_rows = 1;

		/* clamp selection */
		if (sel_col >= ncols)
			sel_col = ncols > 0 ? ncols - 1 : 0;
		if (sel_col < 0)
			sel_col = 0;
		if (sel_row >= nrows)
			sel_row = nrows > 0 ? (int)nrows - 1 : 0;
		if (sel_row < 0)
			sel_row = 0;
		/* vertical scroll to keep the selection on screen */
		if (sel_row < top)
			top = sel_row;
		if (sel_row >= top + body_rows)
			top = sel_row - body_rows + 1;

		/* per-column widths from the live data (header vs widest cell), capped. */
		int slotw = 4, genw = 3;
		for (long row = 0; row < nrows; row++) {
			long long slot;
			int gen;
			memcpy(&slot, blob + row * row_bytes, 8);
			memcpy(&gen, blob + row * row_bytes + 8, 4);
			if (num_digits(slot) > slotw)
				slotw = num_digits(slot);
			if (num_digits(gen) > genw)
				genw = num_digits(gen);
		}
		for (int c = 0; c < ncols; c++) {
			int w = (int)strlen(fields[colmap[c]].name);
			for (long row = 0; row < nrows; row++) {
				char cb[64];
				const unsigned char *cell = blob + row * row_bytes + 12;
				for (int k = 0; k < c; k++)
					cell += fields[colmap[k]].ecount * fields[colmap[k]].esize;
				fmt_cell(cb, sizeof(cb), &fields[colmap[c]], cell);
				int l = (int)strlen(cb);
				if (l > w)
					w = l;
			}
			if (w > 20)
				w = 20;
			if (w < 3)
				w = 3;
			colw[c] = w;
		}

		/* horizontal scroll: keep the selected column within the visible window. */
		if (sel_col < left_col)
			left_col = sel_col;
		int prefixw = slotw + 1 + genw + 1;
		int avail = tcols - prefixw;
		if (avail < 8)
			avail = 8;
		for (;;) {
			int used = 0, last = left_col - 1;
			for (int c = left_col; c < ncols; c++) {
				if (used + colw[c] + 1 > avail)
					break;
				used += colw[c] + 1;
				last = c;
			}
			if (last < left_col || sel_col <= last)
				break; /* a single too-wide col, or selection already visible */
			left_col++;
		}
		int vis_end = left_col;
		{
			int used = 0;
			for (int c = left_col; c < ncols; c++) {
				if (used + colw[c] + 1 > avail && c > left_col)
					break;
				used += colw[c] + 1;
				vis_end = c + 1;
			}
		}

		char frame[1 << 16];
		int o = 0;
		APPEND("\x1b[H"); /* home (we clear each line with C_EOL, avoids full-screen flicker) */

		/* title bar (full width) */
		char title[256];
		snprintf(title, sizeof(title), " arche inspect — %s  ·  %s, cap %ld  ·  %ld live", pool_names[pool_idx],
		         pool_dyn[pool_idx] ? "dynamic" : "static", pool_cap[pool_idx], nrows);
		APPEND(C_TITLE "%-*.*s" C_RST "\r\n", tcols, tcols, title);

		/* pool tabs */
		APPEND(C_DIM "pools:" C_RST);
		for (int i = 0; i < npools; i++)
			APPEND(" %s %s " C_RST, i == pool_idx ? C_TAB_ON : C_TAB_OFF, pool_names[i]);
		APPEND(C_EOL "\r\n");

		/* column header */
		APPEND(C_HEAD "%*s %*s" C_RST, slotw, "slot", genw, "gen");
		for (int c = left_col; c < vis_end; c++) {
			const char *hl = (c == sel_col) ? C_HEAD_SEL : C_HEAD;
			int right = tag_is_numeric(fields[colmap[c]].tag);
			APPEND(" %s%*.*s" C_RST, hl, right ? colw[c] : -colw[c], colw[c], fields[colmap[c]].name);
		}
		APPEND(C_EOL "\r\n");

		/* rows */
		for (int row = top; row < nrows && row < top + body_rows; row++) {
			const unsigned char *p = blob + row * row_bytes;
			long long slot;
			int gen;
			memcpy(&slot, p, 8);
			memcpy(&gen, p + 8, 4);
			const char *zebra = (row & 1) ? C_DIM : "";
			APPEND("%s" C_DIM "%*lld %*d" C_RST, zebra, slotw, slot, genw, gen);
			for (int c = left_col; c < vis_end; c++) {
				char cb[64];
				const unsigned char *cell = p + 12;
				for (int k = 0; k < c; k++)
					cell += fields[colmap[k]].ecount * fields[colmap[k]].esize;
				fmt_cell(cb, sizeof(cb), &fields[colmap[c]], cell);
				int right = tag_is_numeric(fields[colmap[c]].tag);
				int selected = (row == sel_row && c == sel_col);
				APPEND(" %s%*.*s" C_RST, selected ? C_SEL : zebra, right ? colw[c] : -colw[c], colw[c], cb);
			}
			APPEND(C_EOL "\r\n");
		}
		/* clear any leftover body lines from a previous taller frame */
		for (int row = (nrows < top + body_rows ? (int)nrows : top + body_rows); row < top + body_rows; row++)
			APPEND(C_EOL "\r\n");

		/* footer: hints or a transient status on the left, position on the right */
		char lbuf[256], rbuf[160];
		const char *lcolor = C_DIM;
		if (status_ttl > 0 && status[0]) {
			snprintf(lbuf, sizeof(lbuf), " %.250s", status);
			lcolor = status_kind == 2 ? C_ERR : status_kind == 1 ? C_OK : C_DIM;
		} else {
			snprintf(lbuf, sizeof(lbuf),
			         " hjkl move · i edit · +/- adjust · s step · gg/G top/bot · ^D/^U page · [ ] pool · q quit");
		}
		snprintf(rbuf, sizeof(rbuf), "%s%d  step %g  row %d/%ld  %s ", have_count ? "(count) " : "",
		         have_count ? count : 0, step, nrows ? sel_row + 1 : 0, nrows,
		         ncols ? fields[colmap[sel_col]].name : "-");
		int lw = (int)strlen(lbuf), rw = (int)strlen(rbuf);
		if (lw > tcols - rw - 1) {
			lw = tcols - rw - 1;
			if (lw < 0)
				lw = 0;
		}
		int pad = tcols - lw - rw;
		if (pad < 1)
			pad = 1;
		APPEND("\x1b[%d;1H" C_EOL "%s%.*s%*s" C_DIM "%s" C_RST, trows, lcolor, lw, lbuf, pad, "", rbuf);
		if (write(STDOUT_FILENO, frame, (size_t)o) < 0) {
		}

		int key = tui_read_key();
		if (key == K_NONE) {
			if (status_ttl > 0)
				status_ttl--;
			free(blob);
			continue; /* refresh tick */
		}
		if (status_ttl > 0)
			status_ttl--;

		/* vim numeric prefix */
		if (key >= '1' && key <= '9') {
			count = count * 10 + (key - '0');
			have_count = 1;
			free(blob);
			continue;
		}
		if (key == '0' && have_count) {
			count = count * 10;
			free(blob);
			continue;
		}
		int n = have_count ? count : 1;

		/* `g`-led motions: gg (top), gt/gT (pool next/prev) */
		if (pending_g) {
			pending_g = 0;
			if (key == 'g')
				sel_row = 0;
			else if (key == 't')
				pool_idx = (pool_idx + 1) % npools;
			else if (key == 'T')
				pool_idx = (pool_idx - 1 + npools) % npools;
			count = have_count = 0;
			free(blob);
			continue;
		}

		if (key == K_QUIT) {
			free(blob);
			break;
		} else if (key == 'g') {
			pending_g = 1;
			free(blob);
			continue; /* keep count for e.g. ... (gg ignores count) */
		} else if (key == K_UP || key == 'k')
			sel_row -= n;
		else if (key == K_DOWN || key == 'j')
			sel_row += n;
		else if (key == K_LEFT || key == 'h')
			sel_col -= n;
		else if (key == K_RIGHT || key == 'l')
			sel_col += n;
		else if (key == 'G')
			sel_row = have_count ? n - 1 : (int)nrows - 1;
		else if (key == '0' || key == '^')
			sel_col = 0;
		else if (key == '$')
			sel_col = ncols - 1;
		else if (key == 4) /* Ctrl-D */
			sel_row += body_rows / 2;
		else if (key == 21) /* Ctrl-U */
			sel_row -= body_rows / 2;
		else if (key == 6) /* Ctrl-F */
			sel_row += body_rows;
		else if (key == 2) /* Ctrl-B */
			sel_row -= body_rows;
		else if (key == '[')
			pool_idx = (pool_idx - 1 + npools) % npools;
		else if (key == ']' || key == '\t')
			pool_idx = (pool_idx + 1) % npools;
		else if ((key == K_ENTER || key == 'i' || key == 'a' || key == 'e' || key == 'c') && nrows > 0 && ncols > 0) {
			/* edit the selected cell */
			const unsigned char *p = blob + sel_row * row_bytes;
			long long slot;
			int gen;
			memcpy(&slot, p, 8);
			memcpy(&gen, p + 8, 4);
			int fidx = colmap[sel_col];
			FieldDesc *fdesc = &fields[fidx];
			if (fdesc->tag == AIT_HANDLE || fdesc->tag == AIT_OPAQUE) {
				snprintf(status, sizeof(status), "%s is read-only (handle/opaque)", fdesc->name);
				status_kind = 2;
				status_ttl = 12;
			} else {
				char prompt[200], val[128] = {0};
				snprintf(prompt, sizeof(prompt), "%s[%lld].%s =", pool_names[pool_idx], slot, fdesc->name);
				if (tui_edit_line(trows, prompt, NULL, val, sizeof(val)) == 0 && val[0]) {
					unsigned char bytes[16];
					if (encode_value(fdesc, val, bytes) != 0) {
						snprintf(status, sizeof(status), "cannot encode '%s' for %s", val, fdesc->name);
						status_kind = 2;
					} else {
						char hex[40];
						int ho = 0;
						for (long i = 0; i < fdesc->esize; i++)
							ho += snprintf(hex + ho, sizeof(hex) - ho, "%02x", bytes[i]);
						char req[256];
						snprintf(req, sizeof(req), "POKE %s %lld %d %d 0 %s\n", pool_names[pool_idx], slot, gen, fidx,
						         hex);
						if (send_req(fd, req) == 0 && rd_line(&r, line, sizeof(line)) == 0) {
							if (strncmp(line, "OK", 2) == 0) {
								snprintf(status, sizeof(status), "set %s = %s", fdesc->name, val);
								status_kind = 1;
							} else {
								snprintf(status, sizeof(status), "poke failed: %.200s", line);
								status_kind = 2;
							}
						}
					}
				} else {
					snprintf(status, sizeof(status), "edit cancelled");
					status_kind = 0;
				}
				status_ttl = 12;
			}
		} else if ((key == '+' || key == '=' || key == '-' || key == '_') && nrows > 0 && ncols > 0) {
			/* adjust the selected cell by ±(step × count) without retyping its value */
			const unsigned char *p = blob + sel_row * row_bytes;
			long long slot;
			int gen;
			memcpy(&slot, p, 8);
			memcpy(&gen, p + 8, 4);
			int fidx = colmap[sel_col];
			FieldDesc *fdesc = &fields[fidx];
			const unsigned char *cell = p + 12;
			for (int k = 0; k < sel_col; k++)
				cell += fields[colmap[k]].ecount * fields[colmap[k]].esize;
			double mag = step * n;
			double delta = (key == '-' || key == '_') ? -mag : mag;
			unsigned char bytes[16];
			if (delta_value(fdesc, cell, delta, bytes) != 0) {
				snprintf(status, sizeof(status), "%s is not an adjustable number", fdesc->name);
				status_kind = 2;
			} else {
				char hex[40];
				int ho = 0;
				for (long i = 0; i < fdesc->esize; i++)
					ho += snprintf(hex + ho, sizeof(hex) - ho, "%02x", bytes[i]);
				char req[256];
				snprintf(req, sizeof(req), "POKE %s %lld %d %d 0 %s\n", pool_names[pool_idx], slot, gen, fidx, hex);
				if (send_req(fd, req) == 0 && rd_line(&r, line, sizeof(line)) == 0) {
					if (strncmp(line, "OK", 2) == 0) {
						char nb[64];
						fmt_cell(nb, sizeof(nb), fdesc, bytes);
						snprintf(status, sizeof(status), "%s %c= %g → %s", fdesc->name, delta < 0 ? '-' : '+', mag, nb);
						status_kind = 1;
					} else {
						snprintf(status, sizeof(status), "poke failed: %.200s", line);
						status_kind = 2;
					}
				}
			}
			status_ttl = 12;
		} else if (key == 's') {
			/* set the +/- step amount */
			char val[64] = {0};
			if (tui_edit_line(trows, "step =", NULL, val, sizeof(val)) == 0 && val[0]) {
				double s = strtod(val, NULL);
				if (s != 0) {
					step = s;
					snprintf(status, sizeof(status), "step = %g", step);
					status_kind = 1;
				} else {
					snprintf(status, sizeof(status), "invalid step '%s'", val);
					status_kind = 2;
				}
				status_ttl = 12;
			}
		}
		count = have_count = 0;
		free(blob);
	}

	if (fd >= 0)
		close(fd);
	tui_restore();
	return ARCHE_OK;
}
#undef APPEND
#undef C_RST
#undef C_TITLE
#undef C_TAB_ON
#undef C_TAB_OFF
#undef C_HEAD
#undef C_HEAD_SEL
#undef C_DIM
#undef C_SEL
#undef C_OK
#undef C_ERR
#undef C_EOL

int inspect_run(int argc, char **argv, const GlobalOpts *g) {
	(void)g;
	ArgParse p;
	if (args_parse(k_inspect_specs, argc, argv, &p) != 0) {
		fprintf(stderr, "%s: %s\n", g_prog, p.err);
		args_usage(stderr, g_prog, "inspect", "[--sock <path>] [list|schema|rows|poke] ...", k_inspect_specs);
		return ARCHE_USAGE;
	}
	if (p.want_help) {
		args_usage(stdout, g_prog, "inspect",
		           "[--sock <path>] [tui [pool] | list | schema <pool> | rows <pool> [start n] | poke <pool> "
		           "<slot> <field> <value>]",
		           k_inspect_specs);
		return ARCHE_OK;
	}

	/* No verb → interactive TUI. The one-shot verbs stay for scripting / non-tty use. */
	const char *verb = p.pos_count > 0 ? p.pos[0] : "tui";
	const char *path = resolve_sock(&p);

	/* The TUI is connection-independent: it launches with no host, shows a waiting screen, and connects
	 * (and reconnects) on its own. It owns its socket lifecycle, so we do NOT pre-connect here. */
	if (strcmp(verb, "tui") == 0)
		return tui_run(path, p.pos_count > 1 ? p.pos[1] : NULL);

	/* One-shot scripting verbs require a live host — fail fast if none is up. */
	Reader r;
	if (inspect_connect(path, &r) < 0) {
		fprintf(stderr, "arche inspect: cannot connect to %s (is `arche run` active?)\n", path);
		return ARCHE_ERR;
	}
	int fd = r.fd;
	int rc;

	if (strcmp(verb, "list") == 0) {
		rc = do_list(fd, &r);
	} else if (strcmp(verb, "schema") == 0) {
		if (p.pos_count < 2) {
			fprintf(stderr, "usage: %s inspect schema <pool>\n", g_prog);
			rc = ARCHE_USAGE;
		} else {
			rc = do_schema(fd, &r, p.pos[1]);
		}
	} else if (strcmp(verb, "rows") == 0) {
		if (p.pos_count < 2) {
			fprintf(stderr, "usage: %s inspect rows <pool> [start count]\n", g_prog);
			rc = ARCHE_USAGE;
		} else {
			long start = p.pos_count > 2 ? strtol(p.pos[2], NULL, 10) : 0;
			long count = p.pos_count > 3 ? strtol(p.pos[3], NULL, 10) : -1;
			rc = do_rows(fd, &r, p.pos[1], start, count);
		}
	} else if (strcmp(verb, "poke") == 0) {
		if (p.pos_count < 5) {
			fprintf(stderr, "usage: %s inspect poke <pool> <slot> <field> <value>\n", g_prog);
			rc = ARCHE_USAGE;
		} else {
			rc = do_poke(fd, &r, p.pos[1], strtol(p.pos[2], NULL, 10), p.pos[3], p.pos[4]);
		}
	} else {
		fprintf(stderr, "arche inspect: unknown verb '%s'\n", verb);
		rc = ARCHE_USAGE;
	}

	close(fd);
	return rc;
}
