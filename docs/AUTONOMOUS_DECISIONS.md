# Autonomous decisions log — Phase 2 (collectives) + Phase 3 (GPU)

This log records decisions made autonomously while implementing the map-kernel roadmap's Phase 2 and
Phase 3, so they can be reviewed and revisited. Each entry: the decision, the alternatives, and why.
Companion: `docs/DECISIONS_gpu.md` (GPU design detail), `docs/explain/E0046.md`/`E0047.md`.

## Phase 2 — collectives (`reduce` / `scan` / `sort`)

1. **Collectives are builtins, not keywords.** `reduce`/`scan`/`sort` are recognized call names, not new
   syntax. *Alt:* dedicated keywords. *Why:* they read as ordinary calls, add no grammar, and match
   Futhark/APL SOAC conventions. Identifiers `reduce`/`scan`/`sort` are still usable elsewhere (they are
   only special as a call head).

2. **The monoid op is the first argument, spelled `+ * min max`.** `+`/`*` are operator tokens, so the
   parser wraps the first arg of `reduce`/`scan` uniformly as a literal carrying the op text; `min`/`max`
   are plain idents handled the same way. *Alt:* pass a named monoid value or a `func` + identity. *Why:*
   `reduce(+, col)` is the least-surprising surface; an explicit `func`+identity monoid is the planned
   extension for user-defined ops (the identity table already exists internally).

3. **Built-in identities; associativity trusted.** `+`→0, `*`→1, `min`→+∞/INT_MAX, `max`→−∞/INT_MIN; an
   empty pool folds to the identity. Associativity is assumed, not proven. *Why:* this is the Futhark/C++
   contract — it is what licenses reordering the fold (and, later, parallel/GPU folding). `and`/`or`
   deferred (no identity ambiguity, just not wired).

4. **`reduce` is an expression; `scan`/`sort` are statements.** `reduce` returns a scalar (its result
   type = the column element type, so `total := reduce(+, N.v)` types correctly); `scan`/`sort` mutate in
   place and yield nothing. *Why:* matches the value/effect split — a fold is a value, an in-place
   rearrangement is an action.

5. **`sort(Pool.key)` permutes ALL columns by the key.** Sorting one column alone would shear the rows.
   *Alt:* sort a single column / take a comparator / a direction flag. *Why:* row integrity is the only
   safe default for an archetype; ascending-by-key is the common case. A direction/comparator is a future
   additive option, not a v1 requirement.

6. **Collectives rejected inside a `map` (E0047).** A `map` is per-element; a collective is whole-column.
   *Why:* the category split is the whole point — keep kernels element-wise, run collectives from the
   driver.

7. **v1 codegen = sequential scalar loops; insertion sort.** Correct first, fast later. *Why:* the plan
   stages parallel/vectorized reduce+scan and a better sort behind this; a simple correct CPU lowering is
   the honest baseline. (Sort is stable insertion sort — O(n²); fine for the small pools today, flagged
   for upgrade.)

## Phase 3 — GPU (`@gpu` maps)

8. **GLSL compute (→ glslc → SPIR-V) is the GPU target, not hand-written SPIR-V or PTX/CUDA.** *Why:* a
   `map` body maps almost 1:1 onto a GLSL compute `main()`; glslc is a trusted, installed compiler that
   produces validated SPIR-V (checked with spirv-val). Direct SPIR-V emission is more code for no near-term
   benefit; PTX/CUDA locks to NVIDIA. SPIR-V/Vulkan runs on both GPUs present here (NVIDIA + AMD).

9. **`@gpu` is a decorator on the `map`, not a keyword or per-column residency.** *Why:* it mirrors the
   existing `@intrinsic`/`@drop`/`@default` decorator mechanism (one consistent surface), and it is
   transparent — the CPU path is unchanged; `@gpu` only ADDS shader emission. Per-column `@gpu` residency
   (device buffers) is the planned finer-grained form (see DECISIONS_gpu staged list).

10. **v1 emitter scope: float columns + arithmetic + comparisons + `select`; bail otherwise.** A map that
    uses int columns, consts, singletons, or other calls is simply not emitted (with a stderr note); its
    CPU path is untouched. *Why:* guarantees every emitted shader is valid, well-typed GLSL (verified by
    glslc/spirv-val) rather than shipping a half-correct translator. The unsupported cases are listed for
    follow-up.

11. **GPU test targets are OPT-IN (`make test-gpu`, `make test-gpu-run`), NOT part of `make test`.** They
    skip cleanly when glslc/spirv-val/Vulkan are absent. *Why:* the **core suite must stay green on a
    machine with no GPU and no shader toolchain** — adding a hard GPU/CI dependency would be a regression
    in itself. The CPU correctness of `@gpu` maps IS covered by the normal suite (the fixtures run on CPU
    with RUN lines). This is the key call that lets Phase 3 land without risking "no regressions."

12. **Comparisons → `float(...)` in value position, raw bool inside a `select` condition; `select` →
    ternary.** *Why:* reconciles arche's "comparison is 0/1, freely mixable" semantics with GLSL's strict
    typing while keeping the shader well-typed. The ternary lowers to a GPU select (no real divergence for
    a scalar condition).

13. **Bounds via a push-constant `count` + the standard grid guard `if (i >= count) return;`.** *Why:* a
    uniform grid guard is universal compute-shader boilerplate, not a per-element data branch — it does not
    violate the branch-free kernel model.

## Follow-up decisions (from the post-implementation review)

15. **`float` is f32 everywhere** (was f64 on the CPU). Unified to match the GPU shader and Jai's `float`;
    the earlier CPU-f64 / GPU-f32 divergence is gone — GPU == CPU bit-for-bit. Float literals not exactly
    representable in f32 are emitted as the hex bit-pattern; a C shim taking arche `float` now uses C
    `float`. *Alt:* keep f64 + emit f64 GPU shaders (~1/64 GPU throughput — rejected).
16. **GPU dispatch is a call-site decision** — `run map @gpu`, not a decl-site `@gpu map` decorator. The
    same kernel can run on the CPU from one driver and the GPU from another.
17. **`sort` takes an optional `asc`/`desc` direction** — `sort(Pool.key, desc)`; `asc` default.
18. **`reduce` is SIMD-vectorized** — a `<4 x T>` accumulator folds the 4-aligned prefix (lane-wise
    op; min/max via vector compare+select), then a horizontal fold + a scalar tail finish. The four monoid
    identities are idempotent under self-combine, so short columns and empty pools stay correct. **Still
    scalar (staged):** `scan` (a prefix fold is cross-lane — needs a Hillis-Steele / blocked scan) and
    `sort` (O(n²) insertion → a real O(n log n) permuting all columns). Done now because reduce is the
    keystone data-parallel primitive and the lowest-risk to vectorize correctly.

## What is explicitly STAGED (designed, not built) — see `docs/DECISIONS_gpu.md`

- The production GPU runtime wired into `arche run`: device-local buffers, persistent column residency,
  async submit + barriers, async `run`. (`tests/gpu/vk_run.c` is a single-SSBO **test harness** proving
  the path on hardware, not the in-binary dispatcher.)
- Multi-SSBO / per-map descriptor layouts in the dispatcher (the emitter already emits N SSBOs).
- Collectives on the GPU (`reduce`/`scan`/`sort` as GPU tree-fold / segmented scan / sort).
- Instanced rendering (columns → instance buffer → one instanced draw).
- The emitter's unsupported cases: int/mixed columns, consts, singletons, user `func` calls in a kernel.

These are staged because each needs real runtime engineering that cannot be made test-clean and
regression-free in this increment; shipping a verified slice plus a concrete design is preferred over
shipping untested code (per the project's "tested properly, no regressions" bar).
