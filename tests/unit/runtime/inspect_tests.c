/* Deterministic unit test for the dev state inspector (runtime/inspect.c).
 *
 * It drives the SAME entry points the codegen-emitted host uses (arche_inspect_register / _field) and the
 * request handler (arche_inspect_handle) against a hand-built fixture pool whose C layout matches what
 * codegen emits for a STATIC pool: columns, then count(i64), free_list[cap](i64), free_count(i64),
 * gen_counters[cap](i32). No socket, no arche compiler, no running program — every check is a pure
 * function of the fixture bytes, so the liveness/poke logic is exercised without timing. Run under ASan.
 *
 * The layout + liveness predicate mirror codegen's arche_insert_/arche_delete_ (free slots occupy
 * free_list[1 .. free_count-1]; free_count is 1-based), so this doubles as a regression guard for them. */
#define _GNU_SOURCE
#include "../../../runtime/inspect.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int test_count = 0, test_pass = 0, test_fail = 0;
void test_start(const char *name) {
	test_count++;
	printf("  [%d] %s ", test_count, name);
	fflush(stdout);
}
void test_pass_msg(void) {
	test_pass++;
	printf("\xe2\x9c\x93\n");
}
void test_fail_msg(const char *reason) {
	test_fail++;
	printf("\xe2\x9c\x97 (%s)\n", reason);
}
#define ASSERT_TRUE(cond, msg)                                                                                         \
	if (!(cond)) {                                                                                                     \
		test_fail_msg(msg);                                                                                            \
		return;                                                                                                        \
	}

/* Fixture static pool: Particle { pos:float, vel:float, hp:int }, capacity 8. Field order and the trailing
 * bookkeeping fields exactly match codegen_archetype_decl's static layout. */
#define CAP 8
typedef struct {
	float pos[CAP];         /* field 0 (column) */
	float vel[CAP];         /* field 1 (column) */
	int32_t hp[CAP];        /* field 2 (column) */
	int64_t count;          /* count_idx     = 3 */
	int64_t free_list[CAP]; /* free_list_idx = 4 */
	int64_t free_count;     /* free_count    = 5 */
	int32_t gen[CAP];       /* gen_counters  = 6 */
} Fixture;

static Fixture g_fix;

static unsigned char g_blob[8192];
static char g_hdr[8192];

/* Register the fixture, mirroring the codegen-emitted register/field calls (offsets via offsetof). */
static void register_fixture(void) {
	memset(&g_fix, 0, sizeof(g_fix));
	arche_inspect_register("Particle", &g_fix, /*is_dynamic*/ 0, /*capacity*/ CAP, /*field_count*/ 3,
	                       (long)offsetof(Fixture, count), (long)offsetof(Fixture, free_list),
	                       (long)offsetof(Fixture, free_count), (long)offsetof(Fixture, gen), /*cap_off*/ -1);
	arche_inspect_field("Particle", "pos", AIT_F32, ARCHE_INSPECT_FIELD_COLUMN, (long)offsetof(Fixture, pos), 1, 4);
	arche_inspect_field("Particle", "vel", AIT_F32, ARCHE_INSPECT_FIELD_COLUMN, (long)offsetof(Fixture, vel), 1, 4);
	arche_inspect_field("Particle", "hp", AIT_I32, ARCHE_INSPECT_FIELD_COLUMN, (long)offsetof(Fixture, hp), 1, 4);
}

/* Populate `n` live rows; free_count = 1 (sentinel, no frees). */
static void populate(int n) {
	g_fix.count = n;
	g_fix.free_count = 1;
	for (int i = 0; i < n; i++) {
		g_fix.pos[i] = (float)(i + 1);
		g_fix.vel[i] = (float)(i + 1) / 10.0f;
		g_fix.hp[i] = 100 - i;
		g_fix.gen[i] = 1;
	}
}

static unsigned long handle(const char *req) {
	unsigned long blen = 0;
	arche_inspect_handle(req, g_hdr, sizeof(g_hdr), g_blob, sizeof(g_blob), &blen);
	return blen;
}

static void test_list(void) {
	test_start("LIST reports the pool, static, capacity 8, 3 fields");
	handle("LIST");
	ASSERT_TRUE(strstr(g_hdr, "OK 1\n") == g_hdr, "expected OK 1");
	ASSERT_TRUE(strstr(g_hdr, "Particle 0 8 3") != NULL, "pool line wrong");
	test_pass_msg();
}

static void test_schema(void) {
	test_start("SCHEMA lists pos/vel/hp with tags and sizes");
	handle("SCHEMA Particle");
	ASSERT_TRUE(strstr(g_hdr, "OK 3\n") == g_hdr, "expected OK 3");
	ASSERT_TRUE(strstr(g_hdr, "pos ") != NULL && strstr(g_hdr, "vel ") != NULL && strstr(g_hdr, "hp ") != NULL,
	            "missing field");
	test_pass_msg();
}

static void test_rows_values(void) {
	test_start("ROWS returns 3 live rows with correct cell bytes");
	populate(3);
	unsigned long blen = handle("ROWS Particle 0 -1");
	long nrows = 0, rb = 0;
	sscanf(g_hdr, "OK %ld %ld", &nrows, &rb);
	ASSERT_TRUE(nrows == 3, "expected 3 rows");
	ASSERT_TRUE(rb == 8 + 4 + 4 + 4 + 4, "row stride wrong"); /* slot+gen + pos+vel+hp */
	ASSERT_TRUE(blen == (unsigned long)(nrows * rb), "blob length wrong");
	/* Row 1: slot 1, gen 1, pos 2.0, vel 0.2, hp 99. */
	const unsigned char *p = g_blob + rb; /* second row */
	int64_t slot;
	int32_t gen, hp;
	float pos;
	memcpy(&slot, p, 8);
	memcpy(&gen, p + 8, 4);
	memcpy(&pos, p + 12, 4);
	memcpy(&hp, p + 20, 4);
	ASSERT_TRUE(slot == 1 && gen == 1, "slot/gen wrong");
	ASSERT_TRUE(pos == 2.0f && hp == 99, "cell value wrong");
	test_pass_msg();
}

static void test_rows_liveness(void) {
	test_start("a freed slot is excluded from ROWS");
	populate(3);
	/* Free slot 1: push to free_list[1], free_count -> 2, bump its gen (mirrors arche_delete). */
	g_fix.free_list[1] = 1;
	g_fix.free_count = 2;
	g_fix.gen[1] = 2;
	handle("ROWS Particle 0 -1");
	long nrows = 0, rb = 0;
	sscanf(g_hdr, "OK %ld %ld", &nrows, &rb);
	ASSERT_TRUE(nrows == 2, "freed slot should be excluded");
	/* The two rows must be slots 0 and 2. */
	int64_t s0, s1;
	memcpy(&s0, g_blob, 8);
	memcpy(&s1, g_blob + rb, 8);
	ASSERT_TRUE(s0 == 0 && s1 == 2, "wrong live slots");
	test_pass_msg();
}

static void test_poke_ok(void) {
	test_start("POKE a live cell with matching gen updates memory");
	populate(3);
	handle("POKE Particle 0 1 2 0 e7030000"); /* hp (field 2) = 0x000003e7 = 999 (LE) */
	ASSERT_TRUE(strncmp(g_hdr, "OK", 2) == 0, "poke should succeed");
	ASSERT_TRUE(g_fix.hp[0] == 999, "hp not written");
	test_pass_msg();
}

static void test_poke_stale_gen(void) {
	test_start("POKE with a stale generation is rejected");
	populate(3); /* gen[0] == 1 */
	int32_t before = g_fix.hp[0];
	handle("POKE Particle 0 7 2 0 01000000"); /* wrong gen 7 */
	ASSERT_TRUE(strstr(g_hdr, "stale-gen") != NULL, "expected stale-gen");
	ASSERT_TRUE(g_fix.hp[0] == before, "memory must be untouched");
	test_pass_msg();
}

static void test_poke_dead_slot(void) {
	test_start("POKE a freed slot is rejected");
	populate(3);
	g_fix.free_list[1] = 1;
	g_fix.free_count = 2;
	g_fix.gen[1] = 2;
	int32_t before = g_fix.hp[1];
	handle("POKE Particle 1 2 2 0 01000000");
	ASSERT_TRUE(strstr(g_hdr, "dead-slot") != NULL, "expected dead-slot");
	ASSERT_TRUE(g_fix.hp[1] == before, "memory must be untouched");
	test_pass_msg();
}

static void test_poke_oob_and_width(void) {
	test_start("POKE rejects out-of-range slot, bad field, wrong width");
	populate(3);
	handle("POKE Particle 50 1 2 0 01000000");
	ASSERT_TRUE(strstr(g_hdr, "slot-oob") != NULL, "expected slot-oob");
	handle("POKE Particle 0 1 9 0 01000000");
	ASSERT_TRUE(strstr(g_hdr, "bad-field") != NULL, "expected bad-field");
	handle("POKE Particle 0 1 2 0 0102"); /* 2 bytes for a 4-byte field */
	ASSERT_TRUE(strstr(g_hdr, "width") != NULL, "expected width");
	test_pass_msg();
}

static void test_unknown(void) {
	test_start("unknown pool / verb reported in-band");
	handle("ROWS Nope 0 -1");
	ASSERT_TRUE(strstr(g_hdr, "no-pool") != NULL, "expected no-pool");
	handle("FROBNICATE");
	ASSERT_TRUE(strstr(g_hdr, "unknown-verb") != NULL, "expected unknown-verb");
	test_pass_msg();
}

int main(void) {
	register_fixture();
	if (arche_inspect_pool_count() != 1) {
		fprintf(stderr, "fixture registration failed\n");
		return 1;
	}

	printf("State inspector tests:\n");
	test_list();
	test_schema();
	test_rows_values();
	test_rows_liveness();
	test_poke_ok();
	test_poke_stale_gen();
	test_poke_dead_slot();
	test_poke_oob_and_width();
	test_unknown();

	printf("\nResults: %d/%d passed\n", test_pass, test_count);
	if (test_fail > 0) {
		printf("  %d failed\n", test_fail);
		return 1;
	}
	return 0;
}
