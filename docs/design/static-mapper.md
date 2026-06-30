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

## Not yet built

- The measurement is a **separate manual step** (`measure.py`), not folded into `arche build`, and it times
  the *whole program* rather than micro-benchmarking each kernel. Folding a per-kernel micro-benchmark into
  the build is the path to "the compiler measures it itself."
- Non-terminating (`forever`) programs — whole-program timing needs a terminating run; a per-dispatch timer
  would lift that.
- Residency (which columns live in VRAM) is still annotated (`@resident`/`gpu.sync`), not derived.
- Cross-machine distribution (build host ≠ run host).
