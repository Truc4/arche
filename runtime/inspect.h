/* Public API of the dev runtime state inspector (runtime/inspect.c). In arche ALL program state lives
 * in the driver-owned global pools (devices are behavior-only); so a host built with `arche run` can
 * expose its entire live state for viewing/editing. The codegen-emitted host calls
 * arche_inspect_register/arche_inspect_field once per pool at startup (in hot mode only); the inspect
 * server is then serviced cooperatively from the hot-reload quiesce point (arche_inspect_poll, called by
 * runtime/hotreload.c's arche_hot_resolve) so reads/writes never race the program mid-mutation.
 *
 * This file is C, not arche: the socket/poll machinery is a dev-loop implementation detail, confined here
 * exactly like the dlopen/dlsym hot-reload machinery in runtime/hotreload.c. In a release `arche build`
 * this runtime is NOT linked and codegen emits none of the register calls.
 *
 * The `arche inspect` client (cli/cmd_inspect.c) connects to $ARCHE_INSPECT_SOCK and speaks the wire
 * protocol documented in inspect.c. The unit test (tests/unit/runtime/inspect_tests.c) drives these same
 * entry points against a hand-built fixture pool. */
#ifndef ARCHE_INSPECT_H
#define ARCHE_INSPECT_H

#include <stdint.h>

#ifndef ARCHE_INSPECT_MAX_POOLS
#define ARCHE_INSPECT_MAX_POOLS 256
#endif
#ifndef ARCHE_INSPECT_MAX_FIELDS
#define ARCHE_INSPECT_MAX_FIELDS 64
#endif

/* Element type tags — stable wire values, independent of LLVM spellings. Carry width+signedness so the
 * client can format cells correctly. Arche has no `bool` column type (int widths, float=f32, char=i8,
 * handle/opaque=i64), so there is no AIT_BOOL. */
enum ArcheInspectType {
	AIT_I8 = 1,
	AIT_I16,
	AIT_I32,
	AIT_I64,
	AIT_I128,
	AIT_U8,
	AIT_U16,
	AIT_U32,
	AIT_U64,
	AIT_U128,
	AIT_F32,
	AIT_CHAR,
	AIT_HANDLE,
	AIT_OPAQUE
};

/* Field kinds mirror syntax/type_ref.h FieldKind: FIELD_META = 0 (one value for the whole pool),
 * FIELD_COLUMN = 1 (one value per row). */
#define ARCHE_INSPECT_FIELD_META 0
#define ARCHE_INSPECT_FIELD_COLUMN 1

typedef struct {
	const char *name; /* field name, NUL-terminated (points at a codegen-emitted constant) */
	int type_tag;     /* enum ArcheInspectType of the element */
	int kind;         /* ARCHE_INSPECT_FIELD_{META,COLUMN} */
	long byte_offset; /* byte offset within %struct.<Pool> of element 0 (column) / the scalar (meta).
	                   * For a DYNAMIC pool a column field's storage is a T* at this offset (dereffed). */
	long elem_count;  /* per-row element count: 1 for a scalar, N for T[N]/char[N] */
	long elem_size;   /* sizeof one element in bytes */
} ArcheInspectField;

typedef struct {
	const char *name; /* pool / archetype name */
	void *base;       /* &@<Name> (static: the struct) OR &@archetype_<Name> (dynamic: a struct**) */
	int is_dynamic;   /* 0 = static (base IS the struct), 1 = dynamic (deref base for the struct) */
	long capacity;    /* static capacity; for dynamic pools read from cap_off at runtime instead */
	int field_count;
	ArcheInspectField fields[ARCHE_INSPECT_MAX_FIELDS];
	/* Byte offsets within %struct.<Pool> of the bookkeeping fields (LLVM-computed, never recomputed in C):
	 * count(i64), free_list(static:[cap x i64] / dynamic:i64*), free_count(i64),
	 * gen_counters(static:[cap x i32] / dynamic:i32*), capacity(dynamic-only i64; -1 for static). */
	long count_off;
	long free_list_off;
	long free_count_off;
	long gen_off;
	long cap_off;
} ArcheInspectPool;

/* Begin registering a pool. Codegen emits one call per pool in the host's main() (hot mode only), passing
 * the runtime address of the pool global (so the design is PIE-agnostic). Subsequent arche_inspect_field
 * calls append columns to the most-recently registered pool. Also lazily creates the listening socket. */
void arche_inspect_register(const char *name, void *base, int is_dynamic, long capacity, int field_count,
                            long count_off, long free_list_off, long free_count_off, long gen_off, long cap_off);

/* Append one field/column descriptor to the most-recently registered pool. */
void arche_inspect_field(const char *pool_name, const char *field_name, int type_tag, int kind, long byte_offset,
                         long elem_count, long elem_size);

/* Service any pending inspect-socket traffic and return immediately (non-blocking). Called from the
 * hot-reload quiesce point (arche_hot_resolve) so it runs on the main thread between pool mutations —
 * reads are consistent and writes land at a safe point, with no locks. No-op if no socket / no client. */
void arche_inspect_poll(void);

/* Number of registered pools (for the unit test). */
int arche_inspect_pool_count(void);

/* Internal: process one request line (the wire protocol in inspect.c), writing an ASCII response header
 * into `hdr` and any binary payload into `blob` (blob_len set to its length). Returns 0 always (errors are
 * reported in-band as `ERR ...`). Exposed so the unit test can drive requests without a real socket; the
 * poll loop calls it too. */
int arche_inspect_handle(const char *line, char *hdr, unsigned long hdrcap, unsigned char *blob, unsigned long blobcap,
                         unsigned long *blob_len);

#endif /* ARCHE_INSPECT_H */
