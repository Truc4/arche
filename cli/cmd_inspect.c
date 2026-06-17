/* `arche inspect` — view/edit the live pool state of a running `arche run` session. The host (built in
 * hot mode) serves its pools over a Unix socket (runtime/inspect.c); this is the client. In arche all
 * state lives in the driver-owned pools, so this shows the program's entire live state.
 *
 *   arche inspect [--sock <path>] [list]                 list pools
 *   arche inspect [--sock <path>] schema <pool>          show a pool's fields
 *   arche inspect [--sock <path>] rows <pool> [start n]  print live rows as a table
 *   arche inspect [--sock <path>] poke <pool> <slot> <field> <value>   edit one live cell
 *
 * Wire protocol + safety model are documented in runtime/inspect.c. The default socket path is
 * $ARCHE_INSPECT_SOCK, else build/.arche-hot/inspect.sock under the current directory. */
#include "../runtime/inspect.h"
#include "args.h"
#include "cli.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
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
		           "[--sock <path>] <list | schema <pool> | rows <pool> [start n] | poke <pool> <slot> <field> "
		           "<value>>",
		           k_inspect_specs);
		return ARCHE_OK;
	}

	const char *verb = p.pos_count > 0 ? p.pos[0] : "list";
	const char *path = resolve_sock(&p);

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
