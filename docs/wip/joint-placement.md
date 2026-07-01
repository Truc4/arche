# WIP: Joint placement + residency (breaking the phase-ordering trap)

Status: **not implemented — approved implementation plan below.** A working doc: the problem, the researched
direction, and the concrete plan to build it. Reader-facing placement docs: `../design/static-mapper.md`.

## The problem — a circular dependency

Today arche decides placement in two ordered, independent phases:

1. **Placement**, per kernel, greedily: `cg_placement_decide` costs each eligible map on its own, pricing the
   GPU as `launch + PCIe round-trip + compute` — i.e. **assuming the data must be transferred**.
2. **Residency**, downstream: the coherence pass runs *after* placement is frozen and only reacts to it —
   keeping a pool resident across an already-chosen run of GPU maps.

That ordering is a deadlock. A kernel that would win on the GPU **only if its data were resident** is costed
as if it isn't (phase 1 hasn't decided residency yet), so it's placed on the CPU — and then phase 2 has no GPU
run to keep resident. The result is the loop the design can't escape:

> not fast because not resident · not resident because not placed on the GPU · not placed on the GPU because,
> costed non-resident, it isn't fast.

The information needed to break it (that a *cluster* of maps sharing a pool could keep it resident and pay the
transfer once) is exactly the information the greedy per-kernel decision throws away.

## What already works (do not re-solve)

- Residency + coherence *are* derived correctly **given** a placement (the coherence pass: consecutive GPU
  writers → resident, host read → sync). The gap is purely that placement can't *see* residency when it
  decides.
- Whole-program **measurement** exists (`measure.py`) and beats the estimate — but it measures the schedule as
  authored, not alternative placement *configurations*.
- Correctness is never at stake: a wrong placement is only slower. So this can be explored incrementally
  behind the existing frozen-decision model.

## The core idea — optimize the schedule graph as a whole

Stop deciding per kernel. Treat the folded `#run` schedule as a **dataflow graph** — kernels are nodes, a pool
shared by two kernels is an edge — and choose a CPU/GPU assignment for *all* nodes at once to minimize

```
Σ compute(node, device)  +  Σ transfer(edge)   where an edge costs 0 if both endpoints are on the same device
```

This is a **min-cut / graph-partition** framing (as in hardware/software co-design and operator-graph
placement). Its key property is exactly what the greedy version lacks: two GPU kernels sharing a pool have a
**zero-cost edge**, so resident clusters form for free and a transfer is paid only where the partition
actually cuts (a true CPU↔GPU boundary). "Seed + physics resident on the GPU across the loop" becomes a single
*candidate configuration* that gets costed as a unit — and chosen if it wins.

Two ways to evaluate candidates, likely combined:

- **Estimate to prune** — a residency-aware cost model (a per-edge resident-vs-transfer term the current
  estimate lacks) ranks partitions cheaply.
- **Measure to decide** — AutoMap-style, actually build+run the top candidate configurations and freeze the
  winner. This is "measure, don't estimate" applied to the *joint* space, and it's the real break in the
  cycle: the resident configuration is costed *as it actually runs*, not against the current non-resident
  state.

Residency then stops being a downstream pass and becomes a *consequence* of the partition: a pool internal to
a GPU cluster is resident by definition; the coherence pass's job shrinks to inserting the syncs at the cut
edges the partition already identified.

## Decisions / open problems

- **Search space.** A free CPU/GPU choice per kernel is exponential. Lean on structure: the min-cut has
  polynomial exact algorithms for the 2-way (CPU/GPU) case; measurement is reserved for a handful of
  top-ranked partitions, not all of them. Open: how many candidates to measure, and the estimate's accuracy as
  a pruner.
- **The cost model needs a residency term.** `cg_placement_prefer_gpu` folds transfer into a fixed
  per-dispatch overhead; a joint model needs per-edge transfer that goes to ~0 when a pool stays resident
  across a cluster, plus the one-time upload/download at the cut.
- **Loops.** The biggest wins are loop-carried (a pool resident across frames). This needs residency derived
  *through* a back-edge — the coherence pass's documented v1 barrier. Joint placement and loop-residency are
  the same feature from two angles and should land together.
- **Host boundaries are hard cuts.** A CPU consumer that reads a pool is an edge the partition *must* cut,
  every iteration — no assignment removes it. For compute-only pipelines (a chain of maps with no host read
  between) this is fine and the win is real. For a rendered app it is not: the CPU renderer reads the data
  every frame. That boundary is only removable by moving the consumer onto the GPU — see
  [`gpu-graphics-pipeline.md`](gpu-graphics-pipeline.md).
- **Interaction with the frozen model.** The chosen partition is still frozen at build time (no runtime
  scheduler) — this changes *how* the decision is computed, not arche's no-runtime-mapper bet.

## Implementation plan (approved)

For arche's (near-)linear schedules the min-cut degenerates to **residency-aware cluster costing** — the
tractable first cut. Residency stops being a separate downstream decision and becomes a *consequence* of the
partition. Correctness is never at stake (a wrong placement is only slower), so this slots behind the existing
frozen-decision model.

### Part 1 — An honest cost model (the prerequisite the estimate lacks)

The cost model can't amortize transfer because calibrate **folds transfer into `gpu_launch_us`** and zeroes the
size term (`pcie_*_gbps = 1e6`, `cli/cmd_calibrate.c:224`). Split them:

- **`measure_gpu` (cmd_calibrate.c):** add a resident-LIGHT arm (pure launch, no transfer) alongside the
  existing non-resident-light (launch+transfer) and resident-heavy (compute) arms. Derive three separate
  constants: `gpu_launch_us` = pure launch; `pcie_*_gbps` = real bandwidth from `(nonres_light − res_light)`
  over the transferred bytes; `gpu_gflops` = compute (`res_heavy − res_light`, effective fallback). Keep it
  gentle (few dispatches, timeout — never saturate the display GPU).
- **`cg_placement_prefer_gpu` (codegen.c:1443):** formula unchanged, but now fed real launch + real PCIe, so
  the `up + down` transfer term is meaningful — the term joint costing needs. (Also fixes today's inflated
  `launch_us` that skews every decision.)

### Part 2 — The joint placement pass (the graph decision)

A schedule pre-pass `cg_joint_placement(ctx, tree)` run in `codegen_run_decl` **before** `cg_coherence_pass`
(codegen.c:12844), so the coherence pass and the emit site see the same decisions:

- **Build clusters.** Walk the folded `ScheduleTree` in order (mirroring `cg_coh_process_seq`). Eligible maps
  are single-pool by construction (the `cg_map_placed_gpu` gate), so a **cluster** = a maximal run of
  consecutive eligible maps over the *same* pool, bounded by any host access to that pool, a non-eligible step,
  or a control-flow node. Reuse the eligibility gate (`map_query_cols`/`query_match_archs`/`cg_gpu_col_llty`),
  `cg_kernel_flops_per_elem` (codegen.c:1425), `get_arch_static_capacity`, and `cg_kernel_footprint`.
- **Cost each cluster as a unit** (the min-cut for a linear cluster):
  - `cpu = Σ (fpe_i · rows) / cpu_gflops`
  - `gpu = N·launch + Σ (fpe_i · rows)/gpu_gflops + transfer(pool)`  ← **transfer counted ONCE** (one upload
    at entry + one download at the host cut), not per map.
  - Choose GPU for the whole cluster iff `gpu < cpu`.
- **Feed the partition back.** Store per-map decisions in an in-memory table on `CodegenContext`
  (`map-name → gpu|cpu`). `cg_placement_decide` (codegen.c:1481) gains a tier: `@gpu` → `ARCHE_FORCE_PLACE` →
  `ARCHE_FORCE_PLACE_ONLY` → measured file → **joint table** → (fallback) the per-map estimate. A size-1
  cluster reproduces today's single-map estimate, so the table covers every eligible map and the greedy tier
  is only a fallback for maps the pass didn't see.
- **Residency is a consequence.** A pool internal to a GPU cluster is resident by definition; the coherence
  pass already derives exactly that from the same consecutive-GPU structure, so residency + syncs stay
  consistent with no change to the coherence pass. `ARCHE_PLACE_DEBUG` gains a per-cluster line.

**Loops / host boundaries** (already handled by the coherence work, reused): a loop body is a cluster whose
pool stays resident across the back-edge; a host read/write of a pool is a hard cut edge — the transfer there
is mandatory (the download/upload the coherence pass already inserts). A CPU consumer every iteration is an
un-removable cut (→ `gpu-graphics-pipeline.md`).

### Critical files

- `cli/cmd_calibrate.c` — `measure_gpu`: third (resident-light) arm → separate launch / PCIe / compute.
- `codegen/codegen.c` — new `cg_joint_placement` (schedule walk + cluster cost + decision table); a table on
  `CodegenContext`; new tier in `cg_placement_decide` (:1481); call it in `codegen_run_decl` before
  `cg_coherence_pass` (:12844). Reuse the eligibility gate + `cg_kernel_flops_per_elem` + `cg_kernel_footprint`.

### Verification

- **The break, on the real box:** a chain/loop of low-intensity maps over one resident pool that the greedy
  estimate places CPU is now placed **GPU** by a plain `arche build` (cluster transfer amortized) — via
  `ARCHE_PLACE_DEBUG` + a before/after timing (device-gated). `derive_int` / a resident loop stay GPU; a single
  membound map still → CPU (one-map cluster); an interleaved host read still cuts (no false residency).
- **Cost model:** `arche calibrate` prints distinct `launch_us`, `pcie_gbps`, `gpu_gflops` (launch no longer
  absorbs transfer); numbers physically plausible. Gentle probe.
- **Consistency:** joint decision, coherence residency, and emit agree; straight-line `derive.arche` /
  `derive_residency` and all `test-gpu*`, `test-derived-*`, `test-loop-residency`, `test-upload-resident`,
  `test-placement`, lit, C units, `verify-fmt`, `check-corpus` green.

### Non-goals (follow-ons)

- **Measure-to-decide over clusters** — extend `measure.py` to time whole-program cluster configs (a multi-map
  force) and freeze; the estimate prunes, measurement confirms (the "real break"). Next after the analytic pass.
- **General max-flow min-cut** — for branching/DAG dataflow beyond linear single-pool clusters. The cluster
  cost is the exact optimum for the linear case.
- **Multi-pool kernels** in one cluster — kept out by the single-pool eligibility gate for now.

## Prior art

- **Legion** (Bauer et al., SC 2012) + mapper — co-decides task placement *and* region-instance placement, so
  a cluster of tasks keeps a region resident. **AutoMap** (SC 2023) — searches that joint mapping space by
  measurement, precisely because greedy/analytic per-task decisions are suboptimal.
- **CGCM** (Jablin et al., PLDI 2011) / **DyManD** (CGO 2012) — hoist transfers out of loops, keep data
  GPU-resident across kernel launches, transfer back only when the host actually accesses it.
- **Polly-ACC** (Grosser & Hoefler, ICS 2016) / **PPCG** (Verdoolaege et al., TACO 2013) — inter-kernel reuse:
  keep data on-device across kernels when no host access intervenes; minimize host↔device copies.
- **Delite/DMLL**, **Sponge** (Hormati et al., ASPLOS 2011), **Dandelion** (Rossbach et al., SOSP 2013) —
  place operator/stream graphs across CPU+GPU with cross-device communication as edge cost.
- **Hardware/software partitioning** (co-design) — the classic min-cut framing of "which nodes on which
  device, minimizing compute + cut communication."
