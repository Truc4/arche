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
- **In-binary GPU dispatch** (`arche build --gpu`): the executable itself runs `@gpu` maps on the GPU.
  - At build time, each `@gpu` map's GLSL is compiled to SPIR-V (`glslc`) and embedded in a generated
    registry object (`codegen/gpu_embed.c`); the build links it plus the in-binary Vulkan dispatcher
    (`runtime/gpu_runtime.c`, `-lvulkan`).
  - At runtime, a `run map @gpu` calls `arche_gpu_dispatch(name, cols, count)` — lazy device init, a
    per-shader compute-pipeline cache, host-coherent SSBOs uploaded per dispatch, a synchronous submit,
    and readback into the same columns.
  - **CPU fallback is structural**: codegen emits the GPU call followed by a branch — any nonzero return
    (no device, no driver, no embedded shader, dispatch error) runs the existing CPU map call instead. So
    a `--gpu` binary is *always* correct; the GPU is a best-effort accelerator. With `float`=f32 the GPU
    result equals the CPU result bit-for-bit.
  - **Default `arche build` is unchanged** — no `--gpu` means no Vulkan dependency, no dispatch calls, a
    byte-identical CPU binary. The core suite never passes `--gpu`, so it stays GPU-free.
  - **Execution gate** `make test-gpu-exe`: builds `--gpu` executables, runs them, asserts the program's
    own output is correct AND (via `ARCHE_GPU_DEBUG`) that the GPU path actually ran when a device is
    present; no device → CPU fallback, still correct. Verified live on an RTX 3060.
  - v1 scope of dispatch: a single matching **static** pool whose columns are all `float` (mirrors the
    emitter). Multi-pool / dynamic / non-float maps stay CPU-only — no incorrectness, just no GPU.

## Precision: CPU and GPU are the same numeric machine

arche `float` is **f32 on the CPU** (lowered to LLVM `float`) **and f32 on the GPU** (a `float` SSBO), so
the two backends compute at the same precision — the GPU result matches the CPU bit-for-bit, not just on
exactly-representable data. (This resolved the earlier f64-CPU / f32-GPU divergence: `float` was unified to
f32 everywhere, matching Jai and the GPU.) One consequence: a C shim taking an arche `float` must use C
`float` (not `double`) — the FFI maps `float` ↔ `float`.

## Known divergences (accepted for v1, must be tracked)

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

1. **Runtime performance: residency + async.** The shipped dispatcher is correct but not yet fast: it uses
   host-coherent buffers created+uploaded+freed *per dispatch* and a synchronous `vkQueueWaitIdle`. The
   performance path needs device-local buffers, persistent column residency across `run`s (upload once),
   async submit + pipeline barriers, and a non-blocking `run`. Correctness-first was the deliberate v1 call;
   these are pure optimizations over a working, hardware-verified base.
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
