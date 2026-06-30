# The Legion mapper, AutoMap, and why arche can do it at compile time

## The Legion mapper — the runtime placement layer

Legion splits **what computes** (logical regions + privileges: read/write/reduce — checked *statically*) from
**where it runs and where data lives** (the **mapper** — at *runtime*). Per task the runtime calls
`map_task`; the mapper picks the target processor and ranks which memory each region instance occupies, over
a machine model = a graph of processors↔memories with *affinities* (relative access speeds). Placement is the
mapper's job, and it is runtime **by design**: dynamic data sizes, dynamically-created regions, a runtime
task graph, and machine portability.

- [Introduction to the Legion Mapper API](https://legion.stanford.edu/mapper/index.html) — machine model + `map_task` callback.
- [Custom Mappers tutorial](https://legion.stanford.edu/tutorial/custom_mappers.html) — `map_task` selecting memories, concretely.
- [Legion, SC 2012](https://cs.stanford.edu/~sjt/pubs/sc12.pdf) — the mapping interface and the "why runtime" rationale.

## AutoMap (SC 2023) — the mapping *policy* is auto-derivable

[AutoMap](https://rohany.github.io/publications/sc2023-automap.pdf) automatically generates Legion mappers
(replacing hundreds of lines of hand-written C++). The default mapper is just fixed heuristics — "use GPUs;
put data in the highest-bandwidth memory when possible." Takeaway: the placement *policy* can be inferred —
but AutoMap still emits a **runtime-applied** mapper, because Legion's model is dynamic.

## How it applies to arche

arche's permission signature **is** Legion's privileges: read set (query), write set (mutables), `eff`/branch
flags, checked statically. The only difference is placement — and the reasons Legion defers placement to a
runtime mapper do not hold in arche:

| Legion needs runtime because… | arche removes it |
|---|---|
| dynamic data sizes | static pools, compile-time capacity |
| dynamically-created region instances | no dynamic regions; columns are fixed |
| a runtime task graph | `#run` is compile-time-folded to a constant schedule |

So the inputs the mapper consults at runtime are all known at compile time — *except one*: the **machine-
specific cost constants** (GPU launch overhead, PCIe bandwidth, CPU/GPU throughput), which are exactly why
StarPU/Legion decide at runtime. arche resolves that not with a runtime scheduler but with a **per-machine
calibration phase whose decision is frozen**: ⇒ **arche folds the mapper into a per-machine `arche build`
phase.** Because arche compiles from source on the host (like `-march=native`), this folds *into* `arche
build` for the common case (build-host == target): a machine **profile** is measured once (`arche
calibrate`), cached (FFTW-wisdom / `~/.starpu` style, in `ARCHE_CACHE_DIR`), and reused; static pools supply
the row counts, so every cost input is in hand and the decision is made at build time and baked into the
binary — no runtime scheduler. (Distribution, build ≠ run host, defers placement to a first-run/install
step on the target; a follow-on.)

It splits on the legality/profitability line (`../migration-derived-placement.md` §5):

- **Residency-class (which tier is *legal*)** — replicate read-only; VRAM for GPU-exclusive columns; coherent
  (with a `gpu.sync` edge) for columns read by both CPU and GPU. This is Legion's coherence rules lifted to
  compile time over columns. Clean — buildable now.
- **Profitability (which tier is *worth* it)** — AutoMap shows the policy is derivable; doing it *statically,
  with no runtime mapper* is the open part. Everyone (Legion mapper, GPU-DB cost models, LLM tiering) applies
  it at runtime, and the `map_vs_each_step` benchmark shows static profitability is genuinely hard (an
  ideal-parallel kernel still lost on the GPU — it hinges on arithmetic intensity + tier + hardware).

### Landed (Slice 4): CPU/GPU placement derived per machine

Implemented as the first cut of the above (placement only; residency-class derivation is a follow-on, so
`@resident`/`gpu.sync` stay annotated):

- **Eligibility is free** post-decl-collapse: a pure `map` is `kind==MAP && eff==0` — branchless (E0046) and
  effect-free — so under `--gpu` *every* pure map over float columns is GPU-eligible with **no `@gpu`
  annotation**. `@gpu` survives only as a force-GPU override.
- **The cost model** (`codegen.c`, `cg_placement_prefer_gpu`) reads the machine profile + a per-map
  arithmetic-intensity estimate + the static row count: `GPU_t = launch + transfer + flops/gpu_gflops`
  vs `CPU_t = flops/cpu_gflops`; the cheaper wins, frozen into the schedule's dispatch flag.
- **`arche calibrate`** measures `cpu_gflops` in-process and probes the GPU by building+running a tiny
  kernel through the normal `--gpu` pipeline (keeping the compiler itself Vulkan-free), writing
  `ARCHE_CACHE_DIR/machine.profile`. `arche build` loads it (absent ⇒ CPU-only default).
- **Honest result on the dev box (GTX 1650 / PCIe):** the probe shows the GPU is *overhead-dominated* —
  per-dispatch launch+transfer swamps compute for realistic kernels — so calibration declines to auto-place
  on it (CPU-only), matching the `map_vs_each_step` finding. The *mechanism* is validated end-to-end under a
  balanced synthetic profile (`make test-placement`: derives membound→CPU, heavy→GPU, no annotations).

See also `../notes.md` #6 (prior-art kill pass) and `../migration-derived-placement.md` §5 (legality vs profitability).
