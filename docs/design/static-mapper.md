# Placement: where each kernel runs (CPU vs GPU)

arche decides CPU-vs-GPU placement for each kernel *per machine*, instead of making you hand-annotate `@gpu`.
The decision is made **at build time, for all kernels jointly**, from a per-machine cost model — and frozen
into the build. There is no runtime scheduler. A pure `map` (branchless, effect-free, over 32-bit float/int
columns) is GPU-eligible by construction; `@gpu` survives only as a manual force-GPU override.

## Why placement is normally a runtime decision — and why arche's isn't

Systems like Legion and StarPU defer placement to a runtime *mapper*: per task, the runtime asks the mapper
which processor and which memory to use. They do this at runtime by necessity — data sizes are dynamic,
regions are created dynamically, the task graph is built at runtime, and the cost constants (GPU launch
overhead, PCIe bandwidth, CPU/GPU throughput) differ per machine.

arche removes the first three:

| forces runtime placement | arche |
|---|---|
| dynamic data sizes | static pools, fixed compile-time capacity |
| dynamically-created regions | columns are fixed |
| a runtime task graph | `#run` folds to a constant schedule |

The only input that genuinely varies is the **machine's cost constants**. arche resolves that by measuring on
the target machine *once* (`arche calibrate`) and freezing the result — not by carrying a mapper into the
running program.

- [Legion mapper API](https://legion.stanford.edu/mapper/index.html) — the runtime machine model + `map_task`.
- [Legion (SC 2012)](https://cs.stanford.edu/~sjt/pubs/sc12.pdf) — the "why runtime" rationale.

## The cost model

`arche calibrate` measures four per-machine constants and caches them in `ARCHE_CACHE_DIR`:

- **`gpu_launch_us`** — pure per-dispatch launch overhead, measured on a resident pool (no transfer).
- **`gpu_xfer_us`** — the cost of one host↔device round-trip of a pool. It is *staging-dominated* (each copy
  builds and tears down a staging buffer and waits on the queue), so it behaves as a roughly fixed per-copy
  cost rather than a bandwidth × size term.
- **`cpu_gflops`, `gpu_gflops`** — CPU and effective GPU arithmetic throughput.

A kernel's own cost is its per-element flop count × its (static) row count. From these, the GPU time for a run
of maps is `launch·(#dispatches) + transfer·(#device-boundary crossings) + compute`, and the CPU time is just
`compute`. The key term is **transfer**: it is paid *per crossing between the CPU and GPU*, not per map — which
is exactly what makes placement a joint decision rather than a per-kernel one.

## How it works — joint placement

The naive way to place kernels is greedily, one at a time, pricing each GPU map as `launch + a full transfer
round-trip + compute`. That is wrong in a specific, load-bearing way: a map that is worth the GPU **only once
its data is already there** is costed as if it must transfer, so it is placed on the CPU — and then never
becomes resident. [AutoMap (SC 2023)](https://rohany.github.io/publications/sc2023-automap.pdf) makes the
general version of this point — greedy/analytic per-task placement is unreliable because it can't see the
joint effect of co-placement.

arche instead treats the folded `#run` schedule as a dataflow graph — kernels are nodes, a shared pool is an
edge — and places **all nodes together** to minimize `Σ compute + Σ transfer`, where an edge between two
same-device kernels costs **zero**. Two GPU maps over one pool then form a resident cluster and the transfer
is paid *once*, at the true CPU↔GPU boundary.

For arche's near-linear schedules this is exact and cheap:

1. **Eligibility (free).** Under `--gpu`, every pure map over 32-bit columns is a GPU candidate with no
   annotation. Eligible maps are single-pool by construction.
2. **Cluster.** Consecutive eligible maps over the *same* pool form a chain, bounded by any host access to
   that pool, a CPU/ineligible step, or a control-flow node — each of which is a cut edge.
3. **Assign by DP.** Each chain is placed by a dynamic program — the exact min-cut for a linear chain — that
   assigns each map CPU or GPU to minimize `Σ compute + (#device-boundaries)·transfer`. The DP **may split** a
   chain: `membound, heavy` over one pool places `membound` on the CPU and `heavy` on the GPU (the transfer
   for `heavy` is paid regardless, and `membound` is cheaper on the CPU). A single-map chain reduces exactly
   to the greedy per-map estimate.
4. **Edges = residency.** A chain's entry/exit device is the pool's resident device at its boundaries: **CPU**
   for a straight-line chain (bounded by host accesses), **GPU** for a loop body whose pool has *no* host
   access (the pool stays resident across the back-edge — the residency win, with no boundary transfer). A
   host consumer inside the loop — a software renderer reading positions each frame — is a hard cut: the pool
   round-trips every iteration, so that transfer is charged and the DP decides accordingly. This is a cost,
   not a ban: a heavy enough map still wins the GPU across a host cut; a light one (an 8-row step feeding a
   CPU renderer) correctly stays on the CPU.

The chosen target is frozen into the folded schedule. Decision order for an eligible map: explicit `@gpu` →
`ARCHE_FORCE_PLACE` → a measured decision → the joint decision → (fallback) the greedy per-map estimate.

## Measure, don't estimate — the optional override

The joint cost model captures the one structural effect the naive estimate missed (transfer amortized across a
resident cluster), but it is still a model. When exactness matters, measurement overrides it:
`design_analysis/benchmarks/placement/measure.py` builds the program once per candidate placement, **runs each
executable and times the real end-to-end run** at its actual static size, and writes the winner per map to
`<ARCHE_CACHE_DIR>/placement.decisions` (`name gpu|cpu`). `arche build` reads that file and bakes in the
measured choice, ahead of the model. This is profile-guided optimization for placement: profile → freeze →
consume, with no runtime search left in the shipped binary. As a concrete case, a compute-heavy kernel run
repeatedly over a GPU-resident pool runs **~2.7× faster on the GPU** (≈700 ms vs ≈1900 ms, 16M rows, GTX
1650), which measurement confirms directly.

## Residency and coherence, derived

Placement says *where* a kernel runs; **residency** says which columns stay in VRAM across dispatches.
Residency is a **consequence of the joint partition**: a pool internal to a GPU cluster is resident by
definition (uploaded once, not downloaded after every map). That is what turns a compute-heavy loop from a
wash into a win — the one transfer amortizes across the run.

Residency carries a coherence obligation: once the GPU holds the authoritative copy, the host reads stale
data until it is synced back — and symmetrically, once the host writes a resident pool, the GPU reads stale
VRAM until it is refreshed. Both are **derived**, not annotated. A single forward pass over the folded `#run`
schedule tracks, per pool, which device holds the authoritative copy (from each kernel's read/write footprint
and its placement) and inserts the minimal transfers:

- a **download (`gpu.sync`) before any host read** of a pool the GPU last wrote — the mandatory copy that
  keeps a host observation correct;
- an **upload before any GPU read** of a pool the host wrote after it went resident — the mirror, so a kernel
  never reads stale VRAM;
- residency is carried **through loop back-edges**: a pool the GPU touches every iteration stays in VRAM
  across frames, with the download derived *inside* the loop before a host read rather than flushed at the
  boundary.

This is footprint-based coherence in the tradition of
[PPCG](https://dl.acm.org/doi/10.1145/2400682.2400713) (minimal host↔device copies from array footprints),
[Polly-ACC](https://dl.acm.org/doi/10.1145/2925426.2926286) (data resident across kernel invocations when no
host access intervenes), and [CGCM](https://dl.acm.org/doi/10.1145/1993498.1993516) (elides redundant CPU↔GPU
transfers) — made straightforward by arche's fully static schedule. The posture is conservative: over-syncing
is only slower, but a missing sync is *wrong*, so the pass transfers before every host/device observation it
can't prove redundant. `@resident`/`gpu.sync` remain as manual overrides (a hand-written sync is honored, and
never duplicated), exactly as `@gpu` overrides derived placement.

## Not yet built

- **Measurement over clusters.** `measure.py` times one map on the GPU at a time against an all-CPU baseline;
  it does not yet measure a whole *cluster* co-resident, which is the configuration the joint model reasons
  about. Extending it to measure and freeze cluster configurations would let measurement confirm the model's
  residency decisions, not just its per-map ones.
- **General graph min-cut.** The DP is exact for a linear single-pool chain, which covers arche's typical
  schedules. Branching/DAG dataflow across multiple pools, and clusters spanning kernels that touch more than
  one pool, fall back to per-chain placement rather than a full graph partition.
- **The rendered-app boundary.** A CPU consumer that reads a pool every frame (a software renderer) is a hard
  cut the partition cannot remove — no placement keeps that pool resident. Compute-only pipelines have no such
  cut and see the full residency win; removing the boundary for a renderer means moving the consumer onto the
  GPU, which needs a graphics pipeline arche does not have.
- **Portable binaries — known limitation.** Placement and residency are frozen for the **build host's**
  hardware. A binary run on a *different* machine is still **correct** — an absent or slower GPU falls back to
  the CPU, and derived syncs are no-ops on pools that aren't actually resident — but its frozen choices are
  **not re-tuned**, so it can be far from optimal. There is no fat/adaptive binary and no launch-time
  re-selection yet. Ship a binary only to hardware matching the build host, or rebuild on the target. The
  design for a `--adaptive` binary that re-picks placement at launch from a per-machine bench cache is tracked
  in [`../wip/portable-mapped-binaries.md`](../wip/portable-mapped-binaries.md).
