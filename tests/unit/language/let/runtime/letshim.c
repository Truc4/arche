/* C externs for the multi-value let-binding tests. In the proc-free model, binding several results
 * `f(...)(a:, b:, …)` is running an Eff and binding its out-slots; out-slots come from primitives
 * (externs), composed with `zip`. These are the genuine primitives the let-binding grammar destructures.
 * Single C return value = one out-slot. */

int let_sum(int a, int b) {
	return a + b;
}

int let_diff(int a, int b) {
	return a - b;
}

/* An IN-OUT buffer primitive: writes the two bytes (c0, c1) + a NUL into the caller's buffer in place.
 * Void (one in-out buffer out-slot, no scalar return), so `zip`ing several of them yields exactly one
 * out-slot each. The arche decl shadows the buffer in the out-list, proving a func→Eff binds that in-out
 * out-param to the SAME memory as the in-arg. */
void let_fill2(long s, char *buf, int c0, int c1, int n) {
	(void)s;
	if (n >= 3) {
		buf[0] = (char)c0;
		buf[1] = (char)c1;
		buf[2] = 0;
	}
}
