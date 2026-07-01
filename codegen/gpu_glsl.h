#ifndef ARCHE_GPU_GLSL_H
#define ARCHE_GPU_GLSL_H

#include "../hir/hir.h"

/* Emit a GLSL compute shader (`.comp`, one SSBO per column) for every `@gpu` map in the program, one
 * shader per (map, matching archetype). This is the GPU lowering of a `map`: the same loopless,
 * branch-free kernel that vectorizes on the CPU becomes one compute invocation per element.
 *
 * Writes `<out_dir>/<map>__<arch>.comp`. Returns the number of shaders written (>= 0), or -1 on a hard
 * I/O error. A map that uses constructs the emitter doesn't support yet (mixed or non-32-bit columns,
 * singletons, calls other than `select`) is SKIPPED with a note on stderr — its CPU path is unaffected.
 * `*out_count` (if non-NULL) receives the number of `@gpu` maps seen. */
int arche_gpu_emit(HirProgram *prog, const char *out_dir, int *out_count);

/* Propagate `run <map> @gpu` dispatch markers from proc bodies onto the target map decls (sets
 * `HirKernelDecl.is_gpu` (kind==MAP)). Both the file emitter above and the in-binary embed path (gpu_embed.c) call
 * this first so they agree on which maps are GPU. Idempotent. */
void gpu_glsl_mark_runs(HirProgram *prog);

/* Build the full GLSL compute-shader text for one (map, arch). Returns a malloc'd string (caller
 * frees), or NULL if the map body isn't GPU-emittable. `prog` supplies compile-time constants. */
char *gpu_glsl_build_src(HirProgram *prog, HirKernelDecl *map, HirArchetypeDecl *arch);

/* The first archetype in `prog` whose columns cover all of `map`'s params with the same 32-bit scalar
 * type (float/int/uint — the GPU emit constraint). NULL if none — the map has no GPU form, stays CPU. */
HirArchetypeDecl *gpu_glsl_first_emittable_arch(HirProgram *prog, HirKernelDecl *map);

#endif
