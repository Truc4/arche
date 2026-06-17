#ifndef ARCHE_GPU_EMBED_H
#define ARCHE_GPU_EMBED_H

#include "../hir/hir.h"

/* Generate the shader-registry C source for a `--gpu` build. For every `@gpu` map it builds the GLSL
 * (gpu_glsl_build_src), compiles it to SPIR-V with `glslc`, and writes `out_c_path`: the SPIR-V byte
 * arrays plus `arche_gpu_lookup` (the contract in runtime/arche_gpu.h, re-declared self-contained so
 * the file compiles with no include path). compile.c then cc's it to an object and links it next to
 * runtime/gpu_runtime.o.
 *
 * Returns the number of shaders embedded (>= 0), or -1 on a hard error. If `glslc` is not on PATH, an
 * EMPTY registry is written and 0 returned — every dispatch then falls back to CPU, so the build still
 * succeeds and runs correctly (just without GPU acceleration). */
int arche_gpu_embed(HirProgram *prog, const char *out_c_path, int quiet);

#endif
