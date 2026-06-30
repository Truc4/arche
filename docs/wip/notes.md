# arche Research Propositions: Prior Art

Each proposition was checked against existing languages and literature. For each, the idea, a short
example, the closest existing work (with links), and the part not directly covered by that work.

None of these is a new mechanism. Every technique already exists somewhere. What can be claimed is a
specific combination that no single existing system holds.

---

## 1. Pure kernel, per-element fan, effectful composer (`map` / `each` / `system`)

Three kinds run over the columns a query hands them, differing in what they may do. A `map` is a pure,
branch-free, per-element kernel (no control flow, `select(c,a,b)` only), compiled to CPU and GPU,
bit-exact f32. An `each` is the per-element fan: query columns become this element's scalars and the body
may use control flow and effects (a `map` that is not branch-free). A `system` is handed whole columns and
runs effects over them, column-level work rather than per-element iteration; a query-less `system { }` is
the composer that schedules the others. Purity is the seam: `map` is the pure, branch-free one; `each` and
`system` carry the effects.

```arche
bump :: map (query { v, w }) {          // pure kernel: per-element, branch-free
    v = select(v > 0, v + 1, v - 1);    // no `if`, select only
    w = select(w > 0.0, w * 2.0, 0.0);
}

show :: each (query { n }) {            // per-element fan: scalar columns, control flow + effects OK
    if (n > 15) { fmt.printf("n=%d\n", n); }
}

advance :: system (query { n }) {       // whole-column ops + effects, not per-element
    n = n + 5;
}
```

Closest existing work:
- [Dex (Paszke et al., 2021)](https://arxiv.org/abs/2104.05372): purity/effects already gate iteration
  parallelism (one `for`, parallel under `Accum`, sequential under `State`).
- [Halide](https://halide-lang.org/) and [Futhark](https://futhark-lang.org/): pure data-parallel kernel
  to GPU, and they allow pure conditionals, so arche's branch-free rule is stricter, not more capable.
- [Unity DOTS + Burst](https://unity.com/dots): effectful system-over-query, in production.
- [Taichi `kernel`/`func`](https://docs.taichi-lang.org/docs/kernel_function): a two-function split, but
  entry-point vs helper, not pure vs effectful.
- [ISPC](https://ispc.github.io/ispc.html): allows branches in kernels and masks them rather than
  rejecting control flow.

Not directly covered: using the branch-free contract itself as the determinism mechanism. No control flow
plus fixed op order plus f32 lets the language guarantee bit-exact CPU=GPU output.
[NVIDIA CCCL](https://developer.nvidia.com/blog/controlling-floating-point-determinism-in-nvidia-cccl/)
ships determinism levels `not_guaranteed` / `run_to_run` / `gpu_to_gpu`, with no `cpu_to_gpu` level; the
CPU-versus-GPU case is treated as not generally guaranteeable. This is the strongest, most testable claim:
bit-exactness is pass/fail.

---

## 2. Flat compile-time-erased effects (`Eff`)

Effects are values never stored at runtime. Build = an under-applied extern captured in a function. Run =
applying it with an out-param, fused so the builder is erased (zero runtime rep, zero alloc). Composition
is applicative/selective (`|>` fmap, `seq`), flat: no monadic bind, no handlers, no resumption.

```arche
bump :: func(x: int) -> Eff(int) {
    return eff_add1(x);             // build: under-applied extern, inert value
}

bumpneg :: func(x: int) -> Eff(int) {
    return eff_add1(x) |> neg;      // fmap composes a pure func into the effect
}

bump(41)(r:);                       // run: supply the out-slot, runs inline (no `run` keyword)
```

Closest existing work:
- [Compiling Effect Handlers in Capability-Passing Style (Schuster et al., ICFP 2020)](https://dl.acm.org/doi/10.1145/3408975)
  ([PDF](https://ps.informatik.uni-tuebingen.de/publications/schuster20capability.pdf)): "zero-cost"
  handlers via full elimination of the handler abstraction for a program subset.
- [Destination-Passing Style (Shaikhha et al., FHPC 2017)](https://simon.peytonjones.org/assets/pdfs/destination-passing-style.pdf):
  build-deferred, realize-into-out-param, builder fused. arche's run mechanism, applied to arrays.
- [Free Applicative Functors (Capriotti & Kaposi, 2014)](https://arxiv.org/abs/1403.0749) and
  [Selective Applicative Functors (Mokhov et al., ICFP 2019)](https://dl.acm.org/doi/abs/10.1145/3341694):
  the flat-not-monadic static effect set with dynamic selection. arche's own design doc cites this.
- [Koka](https://koka-lang.github.io/koka/doc/book.html) evidence-passing,
  [Effects Without Monads (Kiselyov, 2019)](https://arxiv.org/abs/1905.06544), and
  [Compiling to Categories (Elliott, 2017)](http://conal.net/papers/compiling-to-categories/): each
  erases another axis of the same idea.

Not directly covered: the full bundle together (selective-not-monadic plus destination-passing result plus
full compile-time erasure plus a no-GC systems target). Each property appears pairwise in the works above,
never all four in one system. This is a systems-integration point, not a new abstraction.

---

## 3. ECS as a general-purpose compiled language

Archetypes are first-class relation types, components are columns (SoA), queries are source-agnostic
(named by component, not archetype), systems compile to data-parallel kernels.

```arche
Body :: arche { px :: float, py :: float, mass :: float }   // archetype = columnar relation type
[4]Body(4) { px: 0.0, py: 0.0, mass: 0.0 }                  // pool: capacity 4, 4 live rows

Drift :: map (query { px, py }) {                           // query names components, not the archetype
    px = px + 1.0;
    py = py + 1.0;
}
```

Closest existing work:
- [SpacetimeDB: Databases are the endgame for data-oriented design](https://spacetimedb.com/blog/databases-and-data-oriented-design):
  states that the ECS data model is a strict subset of the relational model and that systems run inner
  joins on the entity id. The whole framing, published.
- [Exploring Concurrency in the ECS Pattern (OOPSLA 2025)](https://arxiv.org/abs/2508.15264)
  ([ACM](https://dl.acm.org/doi/10.1145/3763050)): a formal model where ECS query conjunction behaves like
  a relational join.
- [flecs queries](https://www.flecs.dev/flecs/md_docs_2Queries.html) and
  [Games as databases (Mertens)](https://ajmmertens.medium.com/why-it-is-time-to-start-thinking-of-games-as-databases-e7971da33ac3):
  a real Datalog-style join engine over columnar archetypes, at runtime in a string DSL.
- [Bevy `Query`](https://docs.rs/bevy/latest/bevy/ecs/system/struct.Query.html): tuple queries
  `Query<(&A, &B)>` plus a `.join()` it describes as an inner join.
- [Madrona](https://madrona-engine.github.io/) (a fully-GPU ECS) and [DOTS/Burst](https://unity.com/dots):
  systems compiled to SoA/GPU kernels.
- [Cell](https://www.cell-lang.net/relational.html): a compiled language with first-class relations plus
  Entity/Component plus SoA. Closest single artifact.
- [q/kdb+](https://code.kx.com/q/learn/tour/tables/) (columnar relations as the native value) and
  [Odin `#soa`](https://odin-lang.org/docs/overview/) (SoA as a type).

Not directly covered: the four together (SoA as the storage semantics of a relation type, a query algebra
in the type system, AOT-native general-purpose compilation, and systems lowered to GPU kernels). flecs is
a runtime interpreter, Madrona/DOTS are libraries, GPU-Datalog is a DB engine, Cell is single-threaded.
The relational insight itself is owned by the work above and should be conceded.

---

## 4. Programmable failure policies (OOB / OOM / divide-by-zero)

A first-class `policy` construct declares per-category failure behavior (abort/clamp/wrap/zero/saturate/
evict), unified across the three, that a prover reasons about. Defaults can key on context: pure `func`
gives `x/0 = 0`, effectful `proc` gives `x/0 = abort`.

```arche
c := P.v[oob] !clamp;             // per-op policy: out-of-range index clamps into range
P.v[ix] !clamp = 99;              // write through a clamped index

@default(proc, bounds, clamp)     // scope default, order is (kind, category, policy)
```

Closest existing work:
- [IEEE-754 trap and mask (glibc FP exceptions)](https://www.gnu.org/software/libc/manual/html_node/FP-Exceptions.html):
  a selectable, per-category, value-substituting failure policy since the 1980s (div0 to inf, invalid to
  NaN, per-exception trap enable, optional handlers).
- [Common Lisp conditions and restarts](https://gigamonkeys.com/book/beyond-exception-handling-conditions-and-restarts.html):
  programmable recovery beyond on/off, selected by context.
- [Ada `pragma Suppress`](https://ada-lang.io/docs/arm/AA-11/AA-11.5/) and
  [SPARK overflow modes](https://docs.adacore.com/spark2014-docs/html/ug/source/overflow_modes.html):
  per-category configurable checks plus a prover that elides the ones it proves. Closest single competitor.
- [Rust `Wrapping<T>` / `checked_*` / `saturating_*`](https://doc.rust-lang.org/std/num/struct.Wrapping.html),
  [Zig overflow operators](https://ziglang.org/documentation/master/#Integer-Overflow), and
  [Redis eviction policies](https://redis.io/docs/latest/develop/reference/eviction/): the
  wrap/saturate/clamp/evict menu.
- The purity-conditioned default is standard totality theory:
  [Total Functional Programming (Turner)](http://sblp2004.ic.uff.br/papers/turner.pdf) says to make
  division total (`x/0 = 0`), and
  [the same convention in proof assistants](https://xenaproject.wordpress.com/2020/07/05/division-by-zero-in-type-theory-a-faq/)
  defines `1/0 = 0` because the logic is pure; Koka tracks partiality as a `div` effect.

Not directly covered: the mechanization, one construct unifying OOB+OOM+div0 with a "clamped is safe"
prover rule, plus auto-selecting the default from func/proc class. This is packaging over known
principles.

---

## 5. A data-oriented language as a reliable LLM target

A branch-free / columnar / no-alias / loopless language should be a good LLM codegen target: pure
aliasing-free kernels are easy to verify, the restricted form shrinks the space of wrong outputs.

Closest existing work:
- [Toward Programming Languages for Reasoning (Marron, 2024)](https://arxiv.org/abs/2407.06356) and
  [Bosque](https://bosquelanguage.github.io/): Bosque eliminates loops and aliasing so reasoning reduces
  to decidable logic, and the 2024 paper re-aims that substrate at AI agents. This is the same thesis.
- [Anka: A DSL for Reliable LLM Code Generation (2025)](https://arxiv.org/abs/2512.23214): a data-pipeline
  DSL whose explicit point is shrinking the space of wrong outputs. Notably, with zero training exposure
  to Anka, a model reached 95.8% task accuracy from in-context examples alone, beating Python on
  multi-step tasks. This directly addresses the "no model is trained on it" objection: a novel DSL can be
  learned in-context.
- [KernelBenchX (2026)](https://arxiv.org/abs/2605.04956): published evidence that structurally simpler
  kernels are generated far more reliably (task category explains about 3x more correctness variance than
  the generation method).
- [Type-Constrained Code Generation (PLDI 2025)](https://arxiv.org/abs/2504.09246): reaches verifiable LLM
  output with no data-oriented language at all, so a restricted language is not required for this goal.

Caveat: no LLM is trained on arche, so an "arche vs C" comparison risks measuring training familiarity
rather than structure. Anka suggests in-context learning can offset this, but the cleanest study isolates
structure from familiarity:
1. 2x2 design: cross familiarity (high: Python/numpy, low: a novel DSL) with structure (imperative vs
   branch-free), so familiarity is a measured factor, not a confound.
2. Within-familiar-language: imperative Python vs vectorized no-loop no-alias Python.
3. Verifiability-as-detection: the LLM writes a familiar language, you lower the pure-kernel subset to a
   no-alias form and mechanically catch a defined class of wrongness (OOB, div0, effect violations are
   decidable there), then equivalence-check the rest against a reference. The compiler proves the
   decidable classes; a trained detector handles the residue. The research question is where that boundary
   sits.

Not directly covered: only the controlled experiment, which would run in familiar languages with arche as
motivation rather than apparatus.

---

## 6. Static residency/placement inference from access permissions

The permission signature a system already declares — read set (query), write set (mutables), `eff`/branch
flags — is enough to *derive* a data-residency level on the memory hierarchy at compile time, and to
*constrain* it. The levels form a lattice: CPU-only (host RAM) → GPU host-visible (PCIe) → GPU device-local
(VRAM) → CPU+GPU coherent. Two directions: (a) **infer** the level from permissions (read-only ⇒ replicate;
written only by GPU kernels ⇒ VRAM-resident, no host sync; read by both CPU and GPU ⇒ coherent, with a sync
edge inserted); (b) **enforce/forbid** a level (`@resident`/`@vram`/`@coherent` as a *bound* on the lattice;
a compile error when the derived access pattern violates it).

```arche
@resident [N]Mover(N) { ... }                       // explicit flag now; placer-derived later
step :: map (query { pos }) (vel) { ... }            // R pos, W vel  ->  derive: VRAM-resident, no sync
#run seq({ ..., step, gpu.sync(Mover), report })     // a sync edge is needed only before a CPU read
```

Closest existing work:
- [Legion (Bauer et al., SC 2012)](https://legion.stanford.edu/publications/) /
  [Regent (Slaughter et al., SC 2015)](http://regent-lang.org/images/regent2015.pdf): region privileges
  (read/write/reduce) + coherence are checked **statically**, but physical placement into the memory
  hierarchy is left **entirely to the runtime mapper** — the static-region leader does not place statically.
- GPU columnar DBMS — [Yogatama et al., VLDB 2022](https://www.vldb.org/pvldb/vol15/p2491-yogatama.pdf),
  [Li et al., VLDB 2025](https://www.vldb.org/pvldb/vol18/p4518-li.pdf), CoGaDB: which columns live on the
  GPU — but chosen by **runtime cost models / cache-aware replication**, not static access facts.
- Polyhedral host↔device placement (PPCG, TACO 2013; Polly-ACC, ICS 2016): static, but **affine arrays only**.
- [MLIR memref memory-space + bufferization](https://mlir.llvm.org/docs/Dialects/BufferizationOps/) / XLA
  buffer assignment: static memory-*space* assignment, but the space is **annotated or target-lowered** over
  a tensor graph — not inferred from access permissions. (A good lowering target, not the inference.)
- Memory-hierarchy place lattices — Sequoia (SC 2006), Hierarchical Place Trees (LCPC 2009): the *vocabulary*
  of residency levels, but programmer-mapped.
- Static array-region analysis (Creusillet & Irigoin, IJPP 1996) + region inference (Tofte & Talpin, IC
  1997): the static analyses that recover the accessed region / infer placement — arche gets the region for
  free from the permission signature. StarPU access modes (Euro-Par 2009) and typestate (DeLine & Fähndrich,
  ECOOP 2004) cover the privilege→coherence and enforce-the-sync-state halves.

Not directly covered — and it splits cleanly on the same legality/profitability line as the placement work
(`migration-derived-placement.md` §5):
- **Residency-*class*** (which tiers are *legal* from the permissions: replicate read-only, VRAM for
  GPU-exclusive, coherent for shared) is a clean integration of Legion's coherence rules lifted to
  compile-time over columns — buildable now, not research.
- **Residency-*profitability*** (which tier is *worth* it, statically, with no runtime mapper) is the genuine
  open angle. Every system above that actually decides placement does it at **runtime** (Legion mapper, DB
  cost models, LLM tiering) precisely because static profitability over real hardware is hard. arche's
  no-runtime-scheduler constraint forces the static version nobody has cracked — and the
  `map_vs_each_step` benchmark is direct evidence it's hard: an "ideal-parallel" branchless kernel still lost
  on the GPU, because profitability hinges on arithmetic intensity + residency tier + hardware, not on
  parallelism. **Static placement over non-affine columnar storage, no runtime scheduler, driven by the same
  permission signature that already drives scheduling — that intersection is unoccupied.**

---

## Untested candidates (mined from the implementation, no prior-art check yet)

- Static compile-time-folded schedule: `#run` CTFE-folds to a static tree, no runtime scheduler, no
  function pointers (Bevy/flecs/async all use runtime schedulers).
- Hot-reload as a compile-time mode switch: same source emits trampoline IR in dev, direct-call IR in
  release; state lives in host pools so a device reloads statelessly. The coupling (pools-in-host implies
  stateless reload) may be the novel part.
- One provability verdict drives both editor hints and codegen elision; neither re-derives it.
- Query-enforced data locality (W0029): `Pool.col[i]` is a compile error outside a query, making DOD a
  compiler invariant.

These have not been checked against prior art and deserve the same sweep before being relied on.
