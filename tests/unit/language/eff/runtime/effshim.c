/* C externs for the Eff (flat-effect value) feature tests. Each is a genuine primitive the language
 * cannot express in itself — exactly what an Eff wraps. Single return value = one out-slot (the C ABI). */

#include <string.h>

int eff_add1(int x) {
	return x + 1;
}

/* takes a SLICE (own []char → i8*) and returns a scalar — exercises non-scalar arg threading through the
 * build-func fusion (the `own`-param move/copy-wrapped arg that must be substituted, not dropped). */
int eff_strlen(const char *s) {
	return (int)strlen(s);
}

int eff_mul(int a, int b) {
	return a * b;
}

/* A STATEFUL primitive: accumulates `x` into a running total and returns the new total. Used to prove
 * `seq(a, b)` runs BOTH effects in order (not just the last): seq(acc 10, acc 20) leaves total 30. */
static int eff_total_state = 0;
int eff_acc(int x) {
	eff_total_state += x;
	return eff_total_state;
}

/* A VOID primitive (no out-slot) + its reader — proves an empty `Eff()` built from a void extern actually
 * RUNS: eff_flag() flips the flag, eff_flagged()(r:) reads it back (r==1 ⇒ the value-less effect ran). */
static int eff_flag_state = 0;
void eff_flag(void) {
	eff_flag_state = 1;
}
int eff_flagged(void) {
	return eff_flag_state;
}

/* An IN-OUT buffer primitive: writes "XYZ" into the caller's buffer in place (through the pointer) and
 * returns the count. The arche decl shadows `buf` in the out-list (`(buf, r)`) — proves a func→Eff binds
 * that in-out out-param to the SAME memory as the in-arg (written in place), not a fresh null. */
int eff_fill(long s, char *buf, int n) {
	(void)s;
	if (n >= 3) {
		buf[0] = 'X';
		buf[1] = 'Y';
		buf[2] = 'Z';
	}
	return 3;
}
