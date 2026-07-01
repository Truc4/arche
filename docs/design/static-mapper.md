# Placement: where each kernel runs (CPU vs GPU)

arche decides CPU-vs-GPU placement for each kernel *per machine*, instead of making you hand-annotate `@gpu`.
The decision is made by **measuring the real program and freezing the winner into the build** — there is no
runtime scheduler. A pure `map` (branchless, effect-free, over float columns) is GPU-eligible by
construction; `@gpu` survives only as a manual force-GPU override.

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
the target machine *once* and freezing the result — not by carrying a mapper into the running program.

- [Legion mapper API](https://legion.stanford.edu/mapper/index.html) — the runtime machine model + `map_task`.
- [Legion (SC 2012)](https://cs.stanford.edu/~sjt/pubs/sc12.pdf) — the "why runtime" rationale.

## Measure, don't estimate

[AutoMap (SC 2023)](https://rohany.github.io/publications/sc2023-automap.pdf) showed that placement
performance is noisy and *not* an analytic function of the inputs: you get it right by running the real thing
and timing it, not from a closed-form cost model. It rejects static estimates explicitly — *"the actual costs
of executing tasks and copying data, rather than relying on static estimates."*

arche follows that. A closed-form cost model (`launch + bytes/bandwidth + flops/throughput`) is kept only as
a cheap pre-filter; the decision itself comes from measurement. The model alone is not enough — for example,
a compute-heavy kernel run repeatedly over a GPU-resident pool runs **~2.7× faster on the GPU** (≈700 ms vs
≈1900 ms, 16M rows, GTX 1650), yet the static estimate predicts CPU: a formula can't know the kernel's
*effective* throughput or that the one-time transfer amortizes across the loop. Measurement gets it right.

## How it works

1. **Eligibility (free).** A pure `map` is branchless and effect-free, so under `--gpu` every pure map over
   float columns is a GPU candidate with no annotation.
2. **Measurement.** `design_analysis/benchmarks/placement/measure.py` builds the program once per candidate
   placement, **runs each executable and times the real end-to-end run** at its actual (static) size, and
   writes the winner per map to `<ARCHE_CACHE_DIR>/placement.decisions` (`name gpu|cpu`). This is
   profile-guided optimization (PGO) for placement: profile → freeze → consume.
3. **Build.** `arche build` reads `placement.decisions` and bakes the chosen target into the folded schedule.
   It does no measuring of its own; absent a decisions file it falls back to the static estimate. Decision
   order: explicit `@gpu` → the measured decision → the estimate.
4. **Calibration.** `arche calibrate` measures this machine's CPU/GPU throughput (cached in
   `ARCHE_CACHE_DIR`) to feed the estimate pre-filter.

The advantage over a runtime mapper is **freeze vs live search**: both execute the program to measure, but
because arche's workload is fully static, one ahead-of-time pass can freeze a single answer — no runtime
search, no per-run adaptation, nothing left in the shipped binary.

## Residency and coherence, derived

Placement says *where* a kernel runs; **residency** says which columns stay in VRAM across dispatches. Keeping
a pool GPU-resident — uploaded once, not downloaded after every map — is what turns a compute-heavy loop from
a wash into a win: a map run 40× over a resident 16M-row pool is **~2.7× faster on the GPU** (≈700 ms vs
≈1900 ms, GTX 1650), because the one transfer amortizes across the loop. But residency has a coherence
obligation: once the GPU holds the authoritative copy, the host reads stale data until it is synced back.

Both are **derived** from the schedule, not annotated. A single forward pass over the folded `#run` schedule
tracks, per pool, which device last wrote it, using each kernel's read/write footprint and its placement:

- a pool written by **two or more consecutive GPU-placed maps** with no host read in between is kept
  **resident**, eliding the per-dispatch downloads;
- a **`gpu.sync` is inserted before any host read** of a pool the GPU last wrote — the mandatory download that
  keeps the result correct.

This is footprint-based coherence in the tradition of [PPCG](https://dl.acm.org/doi/10.1145/2400682.2400713)
(generates minimal host↔device copies from array footprints),
[Polly-ACC](https://dl.acm.org/doi/10.1145/2925426.2926286) (keeps data resident across kernel invocations
when no host access intervenes), and [CGCM](https://dl.acm.org/doi/10.1145/1993498.1993516) (elides redundant
CPU↔GPU transfers) — made straightforward by arche's fully static schedule. The posture is conservative:
over-syncing is only slower, but a missing sync is *wrong*, so the pass syncs before every host observation it
can't prove redundant. `@resident`/`gpu.sync` remain as manual overrides (a hand-written sync is honored, and
never duplicated), exactly as `@gpu` overrides derived placement.

## Not yet built

- The measurement is a **separate manual step** (`measure.py`), not folded into `arche build`, and it times
  the *whole program* rather than micro-benchmarking each kernel. Folding a per-kernel micro-benchmark into
  the build is the path to "the compiler measures it itself."
- Non-terminating (`forever`) programs — whole-program timing needs a terminating run; a per-dispatch timer
  would lift that.
- Residency is derived across a **straight-line schedule**; a `loop`/`when`/`par` node is a conservative
  barrier (syncs at the boundary, no residency carried across it). Carrying residency *through* a loop
  back-edge is the next step. Its *profitability* (is residency worth it here) is a structural heuristic today
  — consecutive GPU steps — not yet measured the way placement is.

- **Portable binaries are not supported — known limitation.** Placement and residency are frozen into the
  build for the **build host's** hardware. A binary run on a *different* machine is still **correct** — an
  absent or slower GPU falls back to the CPU, and derived syncs are no-ops on pools that aren't actually
  resident — but its frozen CPU/GPU/residency choices are **not re-tuned** for that machine, so it can be far
  from optimal (a GPU left idle, or a GPU chosen where it now loses). There is no fat/adaptive binary and no
  launch-time re-selection yet. Ship a binary only to hardware matching the build host, or rebuild on the
  target. The design for a `--adaptive` binary that re-picks placement at launch from a per-machine bench
  cache is tracked in [`../wip/portable-mapped-binaries.md`](../wip/portable-mapped-binaries.md).
