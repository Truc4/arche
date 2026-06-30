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

So the inputs the mapper consults at runtime are all known at compile time ⇒ **arche can fold the mapper into
the compiler.** It splits on the legality/profitability line (`../migration-derived-placement.md` §5):

- **Residency-class (which tier is *legal*)** — replicate read-only; VRAM for GPU-exclusive columns; coherent
  (with a `gpu.sync` edge) for columns read by both CPU and GPU. This is Legion's coherence rules lifted to
  compile time over columns. Clean — buildable now.
- **Profitability (which tier is *worth* it)** — AutoMap shows the policy is derivable; doing it *statically,
  with no runtime mapper* is the open part. Everyone (Legion mapper, GPU-DB cost models, LLM tiering) applies
  it at runtime, and the `map_vs_each_step` benchmark shows static profitability is genuinely hard (an
  ideal-parallel kernel still lost on the GPU — it hinges on arithmetic intensity + tier + hardware).

See also `../notes.md` #6 (prior-art kill pass) and `../migration-derived-placement.md` §5 (legality vs profitability).
