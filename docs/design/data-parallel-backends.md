# Data-parallel ops: unified IR + pluggable backends

How arche compiles its whole-column data-parallel operations (`map`, `reduce`, `scan`, `sort`), why they
share one internal IR, and which parallel-backend directions were evaluated and rejected.

User-facing behavior of these ops lives in `docs/language.md` (§Maps, §Collectives, §GPU maps); this doc
is the internal architecture + the design rationale.

## The model: one `ParOp` IR, one `Backend` interface

`map`/`reduce`/`scan`/`sort` are one family — whole-column data-parallel ops — and the execution backend
(scalar / SIMD / multicore / GPU) is an *orthogonal* axis. They lower to a single IR node in
`codegen/codegen.c`:

```
ParOp = PAR_MAP | PAR_REDUCE | PAR_SCAN | PAR_PERMUTE   (+ ParMonoid {op, associative, commutative})
Backend = { emit_reduce, emit_scan, emit_permute, emit_map }   // a vtable, one per backend
```

Codegen builds a `ParOp` and emits it via `select_backend(target)->emit_X(parop)` — there is **no per-op
`strcmp`→emitter dispatch**. The CPU backend's primitives are `emit_fold` (reduce/scan, scalar + 4-lane
SIMD), `emit_sort` (index-permutation sort), and `emit_whole_column_loop` (the map auto-loop).

**Why:** adding a backend (or an op) is now "implement the interface once," **O(primitives)**, not
O(ops×backends). Before this, each builtin hand-wrote codegen straight to LLVM/GLSL strings (~24:1
bespoke-to-shared, and the GPU map lowered a second time in `gpu_glsl.c`); adding multicore + GPU per op
would have meant ~54 bespoke cells. This unification is the load-bearing win and stands on its own.

Inspiration: **Futhark**'s SOAC IR (one representation, backends diverge only at the final emitter). What
arche gets *for free* that Futhark works to acquire: SoA columns already; static allocation (no GPU/heap
allocator); flat columns (no flattening pass); monomorphic scalars (no boxing). So arche's version is
just: a small combinator IR + a backend interface.

## Monoid

`ParMonoid` carries the op + associative/commutative flags (the four built-ins `+`/`*`/`min`/`max` are
all both — the property that licenses SIMD/parallel reordering). User-defined `func`+identity monoids are
deferred (no demand; deep intersection with the func system).

## Multicore (`cores`) backend — prototyped; not a win for pure folds

A `cores` backend is ~40 LOC of codegen (`cores_emit_reduce` + `BACKEND_CORES`) plus one runtime function
(`arche_par_reduce_{f32,i32,i64}` in `runtime/io.c`: chunk the column → per-thread fold → combine partials
with the monoid). No build/link changes (pthread is in glibc ≥2.34; the code lives in the always-linked
`io.o`). Reachable experimentally via `ARCHE_REDUCE_CORES=1 arche build` (no per-op grammar surface yet).

**Measured (100M f32 `reduce(+)`, 12 cores): no speedup.** Single-thread SIMD ~26–32 ms vs multicore
~30 ms. A pure column sum reads 400 MB doing ~1 add/element → it is **memory-bandwidth-bound**, and the
single-thread SIMD reduce already saturates bandwidth, so more threads add nothing (thread-spawn overhead
even nudges it slower). Multicore wins only on **compute-bound** kernels. (Cores is incidentally *more
accurate* — exact `1e8` vs SIMD's saturated `6.71e7`, from per-chunk f32 accumulators.)

Takeaway: a pure column sum — which is exactly the ETL's aggregation task (task 4 `aggregate_region` =
`reduce(+, revenue)`, where summing the column *is the whole task*) — is **inherently bandwidth-bound**.
The 4-lane SIMD reduce already reaches the memory-bandwidth ceiling (that's why it beats the
latency-bound scalar loop ~3×); multicore can't exceed that ceiling, so it adds nothing. So
"monoids → parallel → faster" is false for a standalone fold: **SIMD is the win, more threads aren't.**
Where extra parallelism *does* pay is the compute-bound **map** (real arithmetic per row), which the
unified backend already parallelizes. Benchmark detail: `design_analysis/benchmarks/etl/results_fold_reduce.md`.

## Fusion — evaluated and rejected (not arche's lever)

Fusion (combining adjacent ops to avoid materializing the intermediate) is *central* to **Futhark** because
Futhark is **pure-functional**: idiomatic `reduce(map(map(...)))` produces piles of anonymous intermediate
arrays, and without fusion each is allocated and round-tripped (catastrophic on GPU). Its whole optimizer
is built around it.

**That rationale does not transfer to arche.** arche uses **mutable, materialized, persistent columns** —
maps mutate *named, declared* columns in place; there is no anonymous-intermediate explosion to clean up.
And derived columns are typically **deliverables** (written out), so they must be materialized — the
"eliminate the intermediate" fusion is outright illegal there (it would delete a live output). Parallel
speedup also does not depend on fusion: the map is already the compute-bound op the backend parallelizes.

The ETL *does* have a standalone aggregation task — task 4, `reduce(+, revenue)` — but it sums an
**already-materialized** column (`revenue` is a deliverable produced by task 1), so there is no
intermediate to fuse; it's simply a (bandwidth-bound) fold. The only thing fusion would add is the niche
**fuse-into-producer** case: if a single pipeline both wrote a column *and* wanted its aggregate, compute
them in one sweep instead of write-then-reread (column preserved; saves one pass). The "eliminate the
intermediate" form — summing an expression like `price*qty` you never keep — arche can't express today,
and it doesn't arise in this workload precisely *because* the workload deliberately keeps `revenue`.

**Decision: no fusion pass.** Reconsider only the fuse-into-producer case if a real workload shows the
redundant read-back mattering.

## Deferred

- Per-op `@cores`/`@gpu` **target annotation** (would replace the env toggle and fix the grammar asymmetry
  — collectives currently have no backend surface; `map` has `@gpu` on `run`). Needs syntax-tree → HIR →
  lower attribute plumbing. Do it only if a compute-bound multicore case justifies shipping `cores`.
- GPU `reduce` (tree-fold). Low priority — transfer-bound without GPU-resident columns; a one-shot GPU
  reduce loses to the host↔device copy. Worthwhile only behind column residency + a chained on-device
  pipeline.
- User-defined monoids (`func` + identity).
