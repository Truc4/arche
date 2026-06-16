# GPU compute for `@gpu` maps — design & decisions (Phase 3)

A `map` is a loopless, branch-free, per-element kernel — which is *exactly* a GPU compute shader's
`main()`. Phase 3 makes that concrete: a `@gpu`-annotated map is also lowered to a GLSL compute shader,
compiled to SPIR-V, and (in the opt-in gates) executed on a real GPU and checked against the CPU result.

See `docs/AUTONOMOUS_DECISIONS.md` for the decision log; this file is the design + the honesty notes.

## What is shipped and verified (this increment)

- **`@gpu` decorator on a `map`** — parsed, lowered to `HirMapDecl.is_gpu`, transparent to the CPU path.
- **GLSL compute emitter** (`codegen/gpu_glsl.c`, `arche build --emit-gpu=<dir>`): one `std430` SSBO per
  column (binding *i* = column *i*), a push-constant row count, one invocation per element under the
  standard grid guard `if (i >= count) return;`. `select(cond,a,b)` → GLSL ternary; a comparison used as a
  value → `float(cond)`.
- **Validation gate** `make test-gpu`: every emitted shader is compiled by `glslc` to SPIR-V and checked by
  `spirv-val`. Proves the emitted GLSL is real, valid, well-typed GPU code.
- **Execution gate** `make test-gpu-run`: a tiny N-SSBO Vulkan runner (`tests/gpu/vk_run.c`) dispatches the
  shader on the GPU and the readback is asserted equal to the CPU result — for both a single-column
  arithmetic map (`scale`) and a two-column map with `select` (`physics_step`).
- Both gates are **opt-in** and skip cleanly when the toolchain / Vulkan device is absent — `make test`
  needs no GPU and no shader toolchain. The CPU correctness of `@gpu` maps is covered by the normal suite.

## Known divergences (accepted for v1, must be tracked)

- **CPU `float` is f64; the GPU shader is f32.** arche lowers `float` to LLVM `double` on the CPU, while a
  GLSL `float` SSBO is 32-bit and Vulkan reads/writes 4-byte floats. So "the same map on CPU and GPU" runs
  at *different precision*. The equivalence gates use exactly-representable integer-valued data, so they are
  honest exact checks; for general floating-point data CPU and GPU results may differ in the low bits. A
  future option is to lower arche `float` to f32 on the CPU too (making both backends the same numeric
  machine) or to emit `double` SSBOs (needs the Vulkan `shaderFloat64` feature; ~1/64 throughput on most
  GPUs — usually not worth it). Until then this is a documented limitation, not a silent one.
- **NaN / select semantics.** CPU `select` normalizes its condition via an integer compare of the (i32 0/1)
  comparison result; the GPU emits a scalar-bool ternary. For *ordered* float compares both agree on NaN
  (a NaN compare is false on both sides). `==`/`!=` on NaN, and any future GPU `min`/`max`, can diverge
  (LLVM `fcmp` vs GLSL spec NaN behavior). Avoid relying on NaN semantics across the boundary.

## v1 emitter scope (a map that exceeds it is skipped, CPU path unaffected)

Supported: float columns, arithmetic (`+ - * /`), comparisons, and `select`. NOT yet emitted: int / mixed
columns, `::` consts and singleton reads inside the kernel, and any `func`/`proc` call in the body. When a
`@gpu` map isn't emittable, the build prints a note and emits nothing for it — the executable is identical
either way.

## Staged (designed, not built)

1. **Production runtime wired into `arche run`.** `tests/gpu/vk_run.c` is a *test harness*, not the
   in-binary dispatcher. The real path needs: device-local buffers (not host-visible), persistent column
   residency across `run`s (upload once, not per dispatch), async submit + pipeline barriers, and an async
   `run` so the CPU isn't blocked on `vkQueueWaitIdle`. This is the bulk of the work and is intentionally
   deferred — it cannot be made regression-free without a GPU CI target.
2. **Per-column `@gpu` residency** (vs the per-map decorator) — mark a column as device-resident so a chain
   of GPU maps doesn't round-trip through host memory.
3. **Collectives on the GPU** — `reduce`/`scan` as a GPU tree-fold / segmented scan, `sort` as a GPU sort.
   The monoid abstraction already in place is what makes this lowering well-defined.
4. **Instanced rendering** — the SoA columns ARE per-instance attributes: bind `pos`/`color` columns as an
   instance buffer and issue one instanced draw, instead of the current CPU software raster. This is the
   "draw is an instanced effect" half of the kernel/GPU thesis.
5. **Emitter coverage** — int/mixed columns, consts/singletons (as uniforms / push constants), and
   compile-time-inlined `func` calls in a kernel.

Each is staged because it needs real runtime engineering; shipping a hardware-verified slice plus this
design beats shipping untested code, per the project's "tested properly, no regressions" bar.
