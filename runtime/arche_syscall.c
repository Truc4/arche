/* Portable syscall shim for the wasm32-wasi backend.
 *
 * wasm has no `syscall` instruction, so arche's `@syscall(N)` / `@intrinsic syscall` lower (in
 * codegen's emit_syscall_asm, under --arch=wasm32) to a `call @arche_syscall(...)` here instead of
 * the x86-64 inline `syscall`. We translate the Linux/x86-64 syscall NUMBERS that arche's stdlib
 * emits (stdlib/os/os.arche: read=0 write=1 open=2 close=3 lseek=8 nanosleep=35 clock_gettime=228
 * exit_group=231) into the equivalent wasi-libc calls. Buffers arrive as i64 (pre-coerced via
 * ptrtoint), so we truncate back through uintptr_t (32-bit on wasm32).
 *
 * The return follows the Linux syscall ABI (>= 0 on success, a NEGATIVE value on error) — arche's
 * stdlib checks the sign — so a libc `-1`/errno failure is returned as `-errno`. An unhandled number
 * returns -ENOSYS: a stray syscall is a visible failure, never silent corruption. This file is
 * compiled ONLY for wasm builds (linked by the clang invocation in compile.c); the native path uses
 * the inline `syscall` asm and never references it. */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static int64_t ret(long r) {
	return r < 0 ? -(int64_t)errno : (int64_t)r;
}

int64_t arche_syscall(int64_t n, int64_t a0, int64_t a1, int64_t a2, int64_t a3, int64_t a4, int64_t a5) {
	(void)a3;
	(void)a4;
	(void)a5;
	switch (n) {
	case 0: /* read(fd, buf, count) */
		return ret(read((int)a0, (void *)(uintptr_t)a1, (size_t)a2));
	case 1: /* write(fd, buf, count) */
		return ret(write((int)a0, (const void *)(uintptr_t)a1, (size_t)a2));
	case 2: /* open(path, flags, mode) */
		return ret(open((const char *)(uintptr_t)a0, (int)a1, (int)a2));
	case 3: /* close(fd) */
		return ret(close((int)a0));
	case 8: /* lseek(fd, offset, whence) */
		return ret((long)lseek((int)a0, (off_t)a1, (int)a2));
	case 35: /* nanosleep(req, rem) */
		return ret(nanosleep((const struct timespec *)(uintptr_t)a0, (struct timespec *)(uintptr_t)a1));
	case 228: /* clock_gettime(clk_id, tp) */
		return ret(clock_gettime((clockid_t)a0, (struct timespec *)(uintptr_t)a1));
	case 231: /* exit_group(status) */
		_exit((int)a0);
		return 0;
	default:
		return -ENOSYS;
	}
}
