/* Public API of the dev hot-reload dispatch runtime (runtime/hotreload.c). The codegen-emitted host calls
 * arche_hot_register/arche_hot_resolve; the trampolines reference arche_hot_resolve by name. Declared here
 * so the unit test (tests/unit/runtime/hotreload_tests.c) can drive the same entry points the generated
 * code uses, against fixture `.so`s. */
#ifndef ARCHE_HOTRELOAD_H
#define ARCHE_HOTRELOAD_H

/* Record a device unit's reloadable `.so`. `name` is resolved under $ARCHE_HOT_DIR if set, else used as a
 * path verbatim. Codegen emits one call per device unit at host startup. */
void arche_hot_register(int unit, const char *name);

/* Resolve `sym` in `unit`'s current `.so`, transparently reloading first if the file's (nanosecond) mtime
 * changed. Returns the function pointer for the indirect call, or NULL if unavailable (host no-ops). */
void *arche_hot_resolve(int unit, const char *sym);

/* Reload generation for `unit`: 0 before the first successful load, incremented on every successful
 * (re)load. Lets a caller/test tell "reloaded" from "served the cached handle" without timing. */
unsigned arche_hot_gen(int unit);

#endif /* ARCHE_HOTRELOAD_H */
