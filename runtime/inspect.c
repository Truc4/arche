/* Runtime state inspector — linked into a host built with `arche run` (the dev path), never in a release
 * `arche build`. In arche all state lives in the driver-owned global pools and devices are behavior-only,
 * so the running host can expose its entire live state. Codegen emits, per pool, arche_inspect_register +
 * arche_inspect_field calls in main() (hot mode only) describing the pool's address and layout; this file
 * keeps that registry, opens a Unix-domain socket at $ARCHE_INSPECT_SOCK, and serves a small request
 * protocol. It is serviced COOPERATIVELY: arche_inspect_poll() is called from arche_hot_resolve (the
 * hot-reload quiesce point), so it always runs on the main thread between pool mutations — reads are
 * consistent and writes land safely, with no locks/atomics (which arche deliberately has none of).
 *
 * Wire protocol (Unix stream socket). On connect the server sends "ARCHE-INSPECT 1\n". Each request is one
 * newline-terminated ASCII line; the response is an ASCII header line, optionally followed by a binary blob
 * (ROWS only). LIST/SCHEMA are intentionally human-readable (drivable by `nc -U`/`socat`).
 *
 *   LIST                                  -> "OK <n>\n" then n lines "<name> <is_dyn> <capacity> <fcount>\n"
 *   SCHEMA <pool>                         -> "OK <fc>\n" then per field "<name> <tag> <kind> <ecount> <esize>\n"
 *   ROWS <pool> <start> <count>           -> "OK <nrows> <row_bytes>\n" then a binary blob: per LIVE row in
 *                                            the [start,start+count) window of the live sequence,
 *                                            { i64 slot, i32 gen, then each COLUMN's raw bytes in order }.
 *   POKE <pool> <slot> <gen> <field> <sub> <hex>  -> "OK\n" or "ERR <reason>\n"
 *                                            Writes one cell. Validated: live slot, matching generation,
 *                                            in-range field/sub, exact width. Handle/opaque cells and all
 *                                            bookkeeping fields are unreachable/rejected (no heap corruption).
 *
 * Liveness mirrors the allocator (codegen arche_insert_/arche_delete_): a slot in [0,count) is live unless
 * it currently sits in the free_list. free_count is 1-based (sentinel at index 0), so the freed slots are
 * free_list[1 .. free_count-1]; gen_counters[slot] is the slot's current generation.
 *
 * This file is C, not arche: sockets/polling are a dev-loop implementation detail, confined here like the
 * dlopen machinery in runtime/hotreload.c. */
#define _GNU_SOURCE
#include "inspect.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static ArcheInspectPool g_pools[ARCHE_INSPECT_MAX_POOLS];
static int g_pool_count = 0;

static int g_listen_fd = -1; /* listening socket, or -1 (not set up / disabled) */
static int g_client_fd = -1; /* current connected client, or -1 */
static int g_setup_done = 0; /* tried to create the socket already? */

/* Per-client line accumulator (requests are single ASCII lines). */
#define REQ_MAX 4096
static char g_req[REQ_MAX];
static unsigned long g_req_len = 0;

/* Reusable response scratch. */
#define HDR_MAX 65536
#define BLOB_MAX (4 * 1024 * 1024)
static char g_hdr[HDR_MAX];
static unsigned char g_blob[BLOB_MAX];

/* ---- registry --------------------------------------------------------------------------------------- */

void arche_inspect_register(const char *name, void *base, int is_dynamic, long capacity, int field_count,
                            long count_off, long free_list_off, long free_count_off, long gen_off, long cap_off) {
	if (g_pool_count >= ARCHE_INSPECT_MAX_POOLS || !name)
		return;
	ArcheInspectPool *p = &g_pools[g_pool_count++];
	p->name = name;
	p->base = base;
	p->is_dynamic = is_dynamic;
	p->capacity = capacity;
	p->field_count = 0; /* filled by arche_inspect_field; field_count arg is advisory */
	(void)field_count;
	p->count_off = count_off;
	p->free_list_off = free_list_off;
	p->free_count_off = free_count_off;
	p->gen_off = gen_off;
	p->cap_off = cap_off;
}

void arche_inspect_field(const char *pool_name, const char *field_name, int type_tag, int kind, long byte_offset,
                         long elem_count, long elem_size) {
	(void)pool_name; /* fields append to the most-recently registered pool (codegen emits them in order) */
	if (g_pool_count == 0)
		return;
	ArcheInspectPool *p = &g_pools[g_pool_count - 1];
	if (p->field_count >= ARCHE_INSPECT_MAX_FIELDS)
		return;
	ArcheInspectField *f = &p->fields[p->field_count++];
	f->name = field_name;
	f->type_tag = type_tag;
	f->kind = kind;
	f->byte_offset = byte_offset;
	f->elem_count = elem_count;
	f->elem_size = elem_size;
}

int arche_inspect_pool_count(void) {
	return g_pool_count;
}

/* ---- pool memory access ----------------------------------------------------------------------------- */

static ArcheInspectPool *find_pool(const char *name) {
	for (int i = 0; i < g_pool_count; i++)
		if (strcmp(g_pools[i].name, name) == 0)
			return &g_pools[i];
	return NULL;
}

/* Resolve the struct base of a pool: for dynamic pools `base` is a struct**, deref once. Returns NULL if a
 * dynamic pool has not been allocated yet. */
static char *pool_struct_base(const ArcheInspectPool *p) {
	if (!p->is_dynamic)
		return (char *)p->base;
	char *slot = (char *)p->base;
	void *s = NULL;
	memcpy(&s, slot, sizeof(void *));
	return (char *)s;
}

static long read_i64(const char *addr) {
	long long v = 0;
	memcpy(&v, addr, 8);
	return (long)v;
}

static long pool_count_val(const ArcheInspectPool *p, char *sb) {
	return read_i64(sb + p->count_off);
}
static long pool_free_count(const ArcheInspectPool *p, char *sb) {
	return read_i64(sb + p->free_count_off);
}
static long pool_capacity(const ArcheInspectPool *p, char *sb) {
	return p->is_dynamic ? read_i64(sb + p->cap_off) : p->capacity;
}

/* free_list base (array of i64): inline for static, dereffed pointer for dynamic. */
static const long long *pool_free_list(const ArcheInspectPool *p, char *sb) {
	if (!p->is_dynamic)
		return (const long long *)(sb + p->free_list_off);
	void *fl = NULL;
	memcpy(&fl, sb + p->free_list_off, sizeof(void *));
	return (const long long *)fl;
}

/* gen_counters base (array of i32): inline for static, dereffed pointer for dynamic. */
static const int *pool_gen(const ArcheInspectPool *p, char *sb) {
	if (!p->is_dynamic)
		return (const int *)(sb + p->gen_off);
	void *g = NULL;
	memcpy(&g, sb + p->gen_off, sizeof(void *));
	return (const int *)g;
}

/* Is `slot` currently in the free_list (i.e. dead)? Freed slots occupy free_list[1 .. free_count-1]. */
static int slot_is_free(const ArcheInspectPool *p, char *sb, long slot) {
	long fc = pool_free_count(p, sb);
	const long long *fl = pool_free_list(p, sb);
	if (!fl)
		return 0;
	for (long i = 1; i < fc; i++)
		if ((long)fl[i] == slot)
			return 1;
	return 0;
}

/* Byte address of a COLUMN cell (field f, row slot, sub-element e). */
static char *cell_addr(const ArcheInspectPool *p, char *sb, const ArcheInspectField *f, long slot, long sub) {
	long flat = slot * f->elem_count + sub;
	if (p->is_dynamic) {
		void *col = NULL;
		memcpy(&col, sb + f->byte_offset, sizeof(void *));
		if (!col)
			return NULL;
		return (char *)col + flat * f->elem_size;
	}
	return sb + f->byte_offset + flat * f->elem_size;
}

/* ---- request handling ------------------------------------------------------------------------------- */

static int hex_nibble(char c) {
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

int arche_inspect_handle(const char *line, char *hdr, unsigned long hdrcap, unsigned char *blob, unsigned long blobcap,
                         unsigned long *blob_len) {
	*blob_len = 0;
	char verb[32] = {0};
	char a1[256] = {0};
	long a2 = 0, a3 = 0, a4 = 0, a5 = 0;
	char hex[2048] = {0};

	if (sscanf(line, "%31s", verb) != 1) {
		snprintf(hdr, hdrcap, "ERR empty\n");
		return 0;
	}

	if (strcmp(verb, "LIST") == 0) {
		unsigned long off = (unsigned long)snprintf(hdr, hdrcap, "OK %d\n", g_pool_count);
		for (int i = 0; i < g_pool_count && off < hdrcap; i++) {
			ArcheInspectPool *p = &g_pools[i];
			char *sb = pool_struct_base(p);
			long cap = sb ? pool_capacity(p, sb) : p->capacity;
			off += (unsigned long)snprintf(hdr + off, hdrcap - off, "%s %d %ld %d\n", p->name, p->is_dynamic, cap,
			                               p->field_count);
		}
		return 0;
	}

	if (strcmp(verb, "SCHEMA") == 0) {
		if (sscanf(line, "%31s %255s", verb, a1) != 2) {
			snprintf(hdr, hdrcap, "ERR usage\n");
			return 0;
		}
		ArcheInspectPool *p = find_pool(a1);
		if (!p) {
			snprintf(hdr, hdrcap, "ERR no-pool\n");
			return 0;
		}
		unsigned long off = (unsigned long)snprintf(hdr, hdrcap, "OK %d\n", p->field_count);
		for (int i = 0; i < p->field_count && off < hdrcap; i++) {
			ArcheInspectField *f = &p->fields[i];
			off += (unsigned long)snprintf(hdr + off, hdrcap - off, "%s %d %d %ld %ld\n", f->name, f->type_tag, f->kind,
			                               f->elem_count, f->elem_size);
		}
		return 0;
	}

	if (strcmp(verb, "ROWS") == 0) {
		if (sscanf(line, "%31s %255s %ld %ld", verb, a1, &a2, &a3) != 4) {
			snprintf(hdr, hdrcap, "ERR usage\n");
			return 0;
		}
		long start = a2, want = a3;
		ArcheInspectPool *p = find_pool(a1);
		if (!p) {
			snprintf(hdr, hdrcap, "ERR no-pool\n");
			return 0;
		}
		char *sb = pool_struct_base(p);
		if (!sb) {
			snprintf(hdr, hdrcap, "ERR not-allocated\n");
			return 0;
		}
		/* Fixed per-row stride: i64 slot + i32 gen + each COLUMN's (elem_count*elem_size) bytes. */
		unsigned long row_bytes = 8 + 4;
		for (int i = 0; i < p->field_count; i++)
			if (p->fields[i].kind == ARCHE_INSPECT_FIELD_COLUMN)
				row_bytes += (unsigned long)(p->fields[i].elem_count * p->fields[i].elem_size);

		long count = pool_count_val(p, sb);
		const int *gen = pool_gen(p, sb);
		long live_index = 0; /* position within the live sequence */
		long nrows = 0;
		unsigned long boff = 0;
		for (long slot = 0; slot < count; slot++) {
			if (slot_is_free(p, sb, slot))
				continue;
			long idx = live_index++;
			if (idx < start)
				continue;
			if (want >= 0 && nrows >= want)
				break;
			if (boff + row_bytes > blobcap)
				break;
			long long sslot = slot;
			memcpy(blob + boff, &sslot, 8);
			boff += 8;
			int g = gen ? gen[slot] : 0;
			memcpy(blob + boff, &g, 4);
			boff += 4;
			for (int i = 0; i < p->field_count; i++) {
				ArcheInspectField *f = &p->fields[i];
				if (f->kind != ARCHE_INSPECT_FIELD_COLUMN)
					continue;
				unsigned long n = (unsigned long)(f->elem_count * f->elem_size);
				char *src = cell_addr(p, sb, f, slot, 0);
				if (src)
					memcpy(blob + boff, src, n);
				else
					memset(blob + boff, 0, n);
				boff += n;
			}
			nrows++;
		}
		*blob_len = boff;
		snprintf(hdr, hdrcap, "OK %ld %lu\n", nrows, row_bytes);
		return 0;
	}

	if (strcmp(verb, "POKE") == 0) {
		/* POKE <pool> <slot> <gen> <field> <sub> <hex> */
		if (sscanf(line, "%31s %255s %ld %ld %ld %ld %2047s", verb, a1, &a2, &a3, &a4, &a5, hex) != 7) {
			snprintf(hdr, hdrcap, "ERR usage\n");
			return 0;
		}
		long slot = a2, want_gen = a3, fidx = a4, sub = a5;
		ArcheInspectPool *p = find_pool(a1);
		if (!p) {
			snprintf(hdr, hdrcap, "ERR no-pool\n");
			return 0;
		}
		char *sb = pool_struct_base(p);
		if (!sb) {
			snprintf(hdr, hdrcap, "ERR not-allocated\n");
			return 0;
		}
		if (fidx < 0 || fidx >= p->field_count) {
			snprintf(hdr, hdrcap, "ERR bad-field\n");
			return 0;
		}
		ArcheInspectField *f = &p->fields[fidx];
		/* Refuse cells that would let an edit create a logic landmine or touch C-owned memory. */
		if (f->type_tag == AIT_HANDLE || f->type_tag == AIT_OPAQUE) {
			snprintf(hdr, hdrcap, "ERR readonly-field\n");
			return 0;
		}
		if (sub < 0 || sub >= f->elem_count) {
			snprintf(hdr, hdrcap, "ERR sub-oob\n");
			return 0;
		}
		/* Decode hex payload; its width must match the element size exactly. */
		long hlen = (long)strlen(hex);
		if (hlen % 2 != 0 || hlen / 2 != f->elem_size) {
			snprintf(hdr, hdrcap, "ERR width\n");
			return 0;
		}
		unsigned char bytes[16];
		if (f->elem_size > (long)sizeof(bytes)) {
			snprintf(hdr, hdrcap, "ERR width\n");
			return 0;
		}
		for (long i = 0; i < f->elem_size; i++) {
			int hi = hex_nibble(hex[2 * i]), lo = hex_nibble(hex[2 * i + 1]);
			if (hi < 0 || lo < 0) {
				snprintf(hdr, hdrcap, "ERR hex\n");
				return 0;
			}
			bytes[i] = (unsigned char)((hi << 4) | lo);
		}

		char *dst;
		if (f->kind == ARCHE_INSPECT_FIELD_COLUMN) {
			long cap = pool_capacity(p, sb);
			long count = pool_count_val(p, sb);
			if (slot < 0 || slot >= cap || slot >= count) {
				snprintf(hdr, hdrcap, "ERR slot-oob\n");
				return 0;
			}
			if (slot_is_free(p, sb, slot)) {
				snprintf(hdr, hdrcap, "ERR dead-slot\n");
				return 0;
			}
			const int *gen = pool_gen(p, sb);
			if (gen && (long)gen[slot] != want_gen) {
				snprintf(hdr, hdrcap, "ERR stale-gen\n");
				return 0;
			}
			dst = cell_addr(p, sb, f, slot, sub);
		} else {
			/* FIELD_META: one value for the whole pool; no slot/gen. */
			dst = sb + f->byte_offset + sub * f->elem_size;
		}
		if (!dst) {
			snprintf(hdr, hdrcap, "ERR no-cell\n");
			return 0;
		}
		memcpy(dst, bytes, (size_t)f->elem_size);
		snprintf(hdr, hdrcap, "OK\n");
		return 0;
	}

	snprintf(hdr, hdrcap, "ERR unknown-verb\n");
	return 0;
}

/* ---- socket plumbing -------------------------------------------------------------------------------- */

static void set_nonblock(int fd) {
	int fl = fcntl(fd, F_GETFL, 0);
	if (fl >= 0)
		fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

/* Create the listening socket lazily on first poll. Disabled (no-op) when $ARCHE_INSPECT_SOCK is unset. */
static void ensure_listen(void) {
	if (g_setup_done)
		return;
	g_setup_done = 1;
	const char *path = getenv("ARCHE_INSPECT_SOCK");
	if (!path || !path[0])
		return;
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return;
	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
	unlink(path); /* stale socket from a previous run */
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(fd, 1) != 0) {
		close(fd);
		return;
	}
	set_nonblock(fd);
	g_listen_fd = fd;
}

/* Write the whole buffer, tolerating short writes; closes the client on hard error. */
static int write_all(int fd, const void *buf, unsigned long n) {
	const char *p = (const char *)buf;
	unsigned long off = 0;
	while (off < n) {
		ssize_t w = write(fd, p + off, n - off);
		if (w > 0) {
			off += (unsigned long)w;
			continue;
		}
		return -1; /* EAGAIN on a non-blocking socket also bails — the client is the inspector, can reconnect */
	}
	return 0;
}

void arche_inspect_poll(void) {
	ensure_listen();
	if (g_listen_fd < 0)
		return;

	if (g_client_fd < 0) {
		int c = accept(g_listen_fd, NULL, NULL);
		if (c < 0)
			return; /* nothing pending */
		set_nonblock(c);
		g_client_fd = c;
		g_req_len = 0;
		const char *hello = "ARCHE-INSPECT 1\n";
		if (write_all(c, hello, (unsigned long)strlen(hello)) != 0) {
			close(c);
			g_client_fd = -1;
			return;
		}
	}

	/* Drain whatever request bytes are available; process each complete line. */
	for (;;) {
		char buf[1024];
		ssize_t r = read(g_client_fd, buf, sizeof(buf));
		if (r == 0) { /* client closed */
			close(g_client_fd);
			g_client_fd = -1;
			return;
		}
		if (r < 0)
			return; /* EAGAIN: no more data this tick */
		for (ssize_t i = 0; i < r; i++) {
			char ch = buf[i];
			if (ch == '\n') {
				g_req[g_req_len] = '\0';
				unsigned long blob_len = 0;
				arche_inspect_handle(g_req, g_hdr, HDR_MAX, g_blob, BLOB_MAX, &blob_len);
				g_req_len = 0;
				if (write_all(g_client_fd, g_hdr, (unsigned long)strlen(g_hdr)) != 0 ||
				    (blob_len && write_all(g_client_fd, g_blob, blob_len) != 0)) {
					close(g_client_fd);
					g_client_fd = -1;
					return;
				}
			} else if (g_req_len + 1 < REQ_MAX) {
				g_req[g_req_len++] = ch;
			} else {
				g_req_len = 0; /* overlong line — drop it */
			}
		}
	}
}
