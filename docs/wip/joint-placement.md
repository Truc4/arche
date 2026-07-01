# WIP: Joint placement + residency (breaking the phase-ordering trap)

Status: **design only, not implemented.** A working doc — the problem, the direction, and the open questions
for deciding CPU/GPU placement and data residency *together* instead of one-then-the-other. Reader-facing
placement docs: `../design/static-mapper.md`.

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
