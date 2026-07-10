/* Weak default for the `log_be_emit` diagnostic seam (declared #foreign in core/core.arche). It backs the
 * abort policies' panic prints and the `log` device (extras/log) when NO log backend is selected: a bare
 * program still aborts cleanly with zero setup. A selected backend (a C shim under extras/log/<variant>/)
 * provides a STRONG log_be_emit that overrides this — the Rust `std`-provides-the-default / newlib
 * weak-`_write` model.
 *
 * Deliberately NOT part of WASM_RT_SRCS: on wasm this weak def must be absent so `-Wl,--allow-undefined`
 * turns log_be_emit into an `env` import the browser host fulfils (same as the shim-less gfx_be_* symbols).
 *
 * level: 0 debug, 1 info, 2 warn, 3 error. warn/error → stderr (fd 2); info/debug → stdout (fd 1). The
 * message is a (ptr,len) pair — arche `[]char` carries its length, so no strlen (the old arche_eputs wart). */
#include <unistd.h>

__attribute__((weak)) void log_be_emit(int level, const char *p, int n) {
	(void)write(level >= 2 ? 2 : 1, p, n);
}
