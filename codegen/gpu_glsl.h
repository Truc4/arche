#ifndef ARCHE_GPU_GLSL_H
#define ARCHE_GPU_GLSL_H

#include "../hir/hir.h"

/* Emit a GLSL compute shader (`.comp`, one SSBO per column) for every `@gpu` map in the program, one
 * shader per (map, matching archetype). This is the GPU lowering of a `map`: the same loopless,
 * branch-free kernel that vectorizes on the CPU becomes one compute invocation per element.
 *
 * Writes `<out_dir>/<map>__<arch>.comp`. Returns the number of shaders written (>= 0), or -1 on a hard
 * I/O error. A map that uses constructs the v1 emitter doesn't support yet (non-float columns, consts,
 * singletons, calls other than `select`) is SKIPPED with a note on stderr — its CPU path is unaffected.
 * `*out_count` (if non-NULL) receives the number of `@gpu` maps seen. */
int arche_gpu_emit(HirProgram *prog, const char *out_dir, int *out_count);

#endif
