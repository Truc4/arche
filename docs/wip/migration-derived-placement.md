# Migrating arche-rpg to Derived-Access Placement and Scheduling

A worked example of the proposition in `notes.md` #6: the compiler **derives** per-column
read/write facts from how `map` / `system` touch a pool, and uses those facts to drive (1) the
parallel schedule, (2) data layout, and (3) CPU-vs-GPU residency and transfers, with no annotations and no
hand-written mapper.

Two conventions in this doc:
- **BEFORE** blocks are verbatim current arche-rpg code (compiles today).
- **PROPOSED** blocks are design sketches for a feature that does not exist yet. They do not compile.
  They show intent, not final syntax.

The headline: most of the migration is **not editing code**. The facts are derived from the kernels you
already wrote. The only new surface is a small set of optional overrides.

---

## 0. The facts the compiler derives (no code change)

arche-rpg's hot loop, verbatim:

```arche
// game/game.ds.arche
pos(x, y) :: int;
vel(x, y) :: int;
color     :: int;
r         :: int;
[8]Player;

// game/game.arche
Player :: arche { pos, vel, color, r }
Movers :: query { pos, vel }

step :: map (Movers) {
  vel.x -= (pos.x - CENTER_X) / STIFFNESS;
  vel.y -= (pos.y - CENTER_Y) / STIFFNESS;
  vel *= DAMP_NUM;
  vel /= DAMP_DEN;
  pos += vel;
}
```

```arche
// main.arche  — driver owns the pool, schedule calls the device + gfx
[8]Player(5);
#run {
  seq({
    app.boot,
    forever(seq({ app.pace, game.step, gfx.clear, gfx.circle, gfx.present, gfx.poll, app.done, app.io })),
  })
}
```

From the kernel bodies alone, with no annotation, the compiler can derive the per-column access set:

| Kernel | reads | writes | notes |
|--------|-------|--------|-------|
| `game.step` (map over `Movers`) | `pos`, `vel` | `pos`, `vel` | branch-free + effect-free ⇒ **GPU-eligible** (§6, §7) |
| `gfx.circle` (reads Player to draw) | `pos`, `color`, `r` | none on the pool (writes the framebuffer) | read-only consumer of the pool |

Two conclusions fall straight out:
- `pos` and `vel` are the **hot read-write set** (written every frame by `step`).
- `color` and `r` are **read-only for the whole frame loop** (only ever read, by `circle`).

Nothing above is written by the arche-rpg author. It is recovered from the existing `map` and the existing
`gfx.circle` access pattern.

One refinement on "GPU kernel candidate," because an earlier draft of this doc framed branch-freedom as a
fact *sniffed from the body*. That is the wrong framing: **branch-freedom is not an independent property you
ask for — it *is* GPU-eligibility.** "Branchless" and "GPU-eligible" name the same fact. The placer derives a
single predicate over the unit — `branchless ∧ effect-free ∧ bounded` — and that predicate *is* the CPU/GPU
axis: fail any conjunct and the unit is CPU-only (branches are free on a CPU anyway, §7); pass all three and
it is eligible for the GPU, where a cost model then decides whether it actually goes (§5, legality vs
profitability). So there is no "branch flag," no "explicit ask for branch power," and nothing the author
annotates — only a derived predicate. The author never says `@gpu` and never says "branchless"; both fall
out of the kind they wrote and what its body touches.

---

## 1. Scheduling from derived facts

BEFORE, the frame order is a hand-written linear sequence:

```arche
forever(seq({ app.pace, game.step, gfx.clear, gfx.circle, gfx.present, gfx.poll, app.done, app.io }))
```

The author is manually asserting "step before circle." The compiler already has the fact that forces it:
`step` writes `pos`, `circle` reads `pos`, so there is a true read-after-write dependency. Conversely, any
two kernels whose write-sets are disjoint from each other's read/write-sets can run concurrently.

PROPOSED: you declare intent, the order/parallelism is derived from access conflicts, not hand-sequenced.

```arche
// PROPOSED (design sketch — does not compile today)
#run {
  forever(frame { app.pace, game.step, gfx.clear, gfx.circle, gfx.present, gfx.poll, app.done, app.io })
  // compiler derives: step (W pos,vel) -> circle (R pos) is ordered;
  // app.io / app.done (no Player access) are independent and may run off the critical path.
}
```

The payoff is small in this tiny app (the chain is mostly serial by nature), but the mechanism is the same
one Bevy uses to parallelize systems from `Query<&T>` / `Query<&mut T>` access: you stop hand-ordering and
let the derived conflict graph decide.

---

## 2. CPU/GPU placement and residency from derived facts

`step` is branch-free, so it *can* run on either CPU or GPU. The point of the access rules is that the
author does **not** say which. No `@gpu`, no device name in the source. The placement is decided from the
derived facts plus a cost estimate, exactly like the schedule in section 1.

BEFORE: `pos` / `vel` live in the host (driver-owned) pool; `step` runs on the CPU.

```arche
[8]Player(5);     // host pool; step runs CPU-side
```

PROPOSED: the kernel body is **unchanged** and carries no placement annotation; the compiler derives where
each column lives and what moves.

```arche
// PROPOSED (design sketch — does not compile today)
step :: map (Movers) { ... }   // identical to BEFORE — no @gpu, no device named

// derived placement plan (compiler output, not source):
//   step is branch-free + writes pos,vel every frame -> profitable to run GPU-side
//   so keep pos,vel GPU-RESIDENT across frames (no per-frame upload)
//   gfx.circle reads pos on CPU  -> ship a read-only pos VIEW to the renderer
//   color,r are read-only        -> upload once, never copy back (see section 3)
```

Whether `step` actually lands on the GPU, and whether `pos` stays resident there, is the compiler's call:
because `pos`/`vel` are written every frame and read by one consumer, keeping them GPU-resident and shipping
a read-only `pos` view beats round-tripping the pool. If `gfx.circle` could also run GPU-side, the plan
would keep everything resident and emit no transfer. The author wrote none of this.

**This is the load-bearing decision, and where "solved" forks (see section 5):** *who* makes the placement
call. A runtime cost-model scheduler (StarPU-style) deciding at run time is solved prior art. Deciding it
**statically**, from derived access facts over a non-affine columnar pool, with no runtime scheduler, is
not — and arche's compile-time-folded `#run` (no runtime scheduler by design) forces the static version.

---

## 3. Read-only columns become shared views with no copy-back

From section 0, `color` and `r` are never written. A read-only column needs no write-back and can be
uploaded once and shared.

BEFORE: a transfer-everything model copies the whole Player pool back and forth around a GPU step.

PROPOSED: the derived read-only set is uploaded once and pinned as a shared view.

```arche
// PROPOSED (design sketch — does not compile today)
// derived: { color, r } read-only over the frame loop
//   -> upload once at boot, never transfer again, share as a read-only view to any consumer
//   -> only pos (and vel if a CPU consumer appears) participate in per-frame movement
```

This is the same idea as StarPU's `STARPU_R` data (read-only data replicated, not copied back) and Legion's
read-only privilege (read-only regions shared as multiple instances), except the read-only fact is derived
from the kernels rather than annotated on the data.

---

## 4. What the arche-rpg author actually changes

Almost nothing. The migration is mostly the absence of work:

| Concern | BEFORE (manual) | PROPOSED (derived) |
|--------|------------------|--------------------|
| Frame ordering | hand-written `seq({ step, circle, ... })` | derived from access conflicts |
| GPU offload | none / would be manual data plumbing | `@gpu` on the kernel; placement derived |
| pos/vel residency | host pool, round-tripped | GPU-resident, derived from write-every-frame |
| color/r transfers | copy with everything | upload once, shared read-only view, derived |
| read/write annotations | none needed | none needed (derived from kernel bodies) |

The author keeps writing ordinary `map` / `system` kernels over the pool. The columns' access
modes, the schedule, the layout, and the CPU/GPU placement are recovered from those kernels.

---

## 5. Status and honest caveats

- This feature **is not implemented.** All PROPOSED blocks are sketches.
- The two jobs have different status, and conflating them is a mistake (an earlier draft of this doc did,
  by writing `@gpu`):
  - **Derive access + schedule (sections 0-1): solved, lift it.** Bevy derives R/W from query types over
    columnar storage and auto-schedules from non-interference. No research.
  - **Decide CPU/GPU placement (sections 2-3): depends on who decides.** If a **runtime cost-model
    scheduler** picks placement at run time, that is solved prior art (StarPU auto-selects CPU-vs-GPU per
    task from performance models, inserts transfers, replicates read-only data; Legion privileges +
    mapper; Polly-ACC for affine arrays). If the placement must be a **static, compile-time** decision
    from derived access facts over **non-affine columnar / ECS** storage, with **no runtime scheduler**,
    there is no exact precedent.
- arche's compile-time-folded `#run` (no runtime scheduler by design) pushes it into the unsolved version:
  you cannot fall back to StarPU's runtime scheduler, so the placement decision has to be made statically
  over the non-affine columnar pool. **That constraint, not the placement itself, is what makes this a
  research question rather than an integration.** See `notes.md` #6 for prior-art links.
- arche-rpg's `map (Movers)` over an archetype pool sits in exactly that non-affine columnar zone, which is
  why it is a fair example. It is small and mostly serial, so the *performance* win is modest; its value is
  as the smallest real program exercising the full derive -> schedule -> place pipeline end to end.
- **Legality vs profitability — the split that shrinks the open cell.** The placement decision is two
  questions, not one. *Legality* ("can this go wide?") is the derived predicate `branchless ∧ effect-free ∧
  bounded` (§0, §6) — trivial, no body analysis beyond what the schedule already needs, no research.
  *Profitability* ("should it?") is the static cost model. Only profitability is the genuinely-open
  research cell; legality is settled. So the unsolved part of this doc is narrower than it first reads:
  not "static placement," but "static *cost* over non-affine columnar storage with no runtime scheduler."
- **Resolution of the open cell — a per-machine calibration phase (landed, Slice 4).** The reason cost is
  "runtime" elsewhere is machine-specific constants. arche measures them **once per machine** (`arche
  calibrate` → a cached profile) and makes the placement decision at **build** time, frozen — no runtime
  scheduler. Static pools supply the row counts, so every cost input is in hand. CPU/GPU placement is now
  derived from a pure map's eligibility (`kind==MAP && !eff`, free) + the profile + intensity; see
  `feature-plan/static-mapper.md` "Landed (Slice 4)". Residency-class derivation is the next follow-on.

---

## 6. Dev/release placement — no annotations, ever

The load-bearing fact that keeps placement annotation-free: **placement is a performance decision, not a
correctness one.** A kernel computes the same result on the CPU or the GPU; the CPU is always a legal home
for any kernel. So a placement can be *wrong* (slow) without being *incorrect*. That decouples the two build
modes:

- **Dev = CPU-only.** Everything is CPU-legal, so a dev build needs **zero** placement analysis and **zero**
  declarations — each unit compiles independently for the CPU, trivially separately-compilable. The
  branch-freedom / GPU-eligibility question never even arises, because on the CPU branches are free (§7).
  "Approximate" placement isn't a compromise here: CPU is *exact for results*, just unoptimized.
- **Release = whole-program.** GPU placement (derive `branchless ∧ effect-free ∧ bounded`, then run the cost
  model) happens only in the optimized build, where the whole program is in hand anyway — so it is fully
  **derived**, never declared.

This is why there are no branch/placement annotations *anywhere*: dev sidesteps the question, release derives
it whole-program. An earlier line of thought wanted to surface branch-ness as a *signature color* so GPU
placement could be separately compiled — that idea is **cut**. The only consumer of a per-unit GPU-eligibility
bit is GPU placement, and the only mode that does GPU placement (release) is whole-program, so the bit is
never needed at a module boundary.

Honest flag: dev-on-CPU means GPU placement and any GPU-specific behavior (float precision, divergence cost)
is exercised only in release/perf builds — a normal dev/release tradeoff (iterate logic on the CPU, validate
placement in release), not a hidden one.

## 7. Branchless is a placement technique, not a speed technique

A tempting error is to read this whole document as "branches are bad, make everything branchless." On a CPU
that is simply false. A well-predicted branch is ~0 extra cycles; a misprediction is ~15-20; and the
branches in real kernels — a connection sitting in one state across many reads, a loop bound that rarely
changes — are *highly* predictable. Reifying that control flow into data (`select`, mask-arithmetic
`x * (a >= b)`, transition tables, per-tick state machines) is **pure overhead on a CPU**: you pay rescans,
predicated wasted work, and — for I/O state machines — polling syscalls, all to delete branches that cost
almost nothing.

Branchlessness pays *only* where branches are genuinely expensive:

- **SIMD/GPU lane divergence** — a divergent branch masks whole lanes, 15-30× the cost of a predicted CPU
  branch. Here predication is the only way to keep lanes full.
- **Pathological misprediction at scale** — stream/packet processing of millions of elements in lockstep,
  where mispredicts (not syscalls) dominate.

**Rule: reify control flow into data only for the kernels you will actually place wide.** Everywhere else,
branches belong in funcs/systems and run on the CPU, where they are free. Branchless is the price you pay to
make a kernel GPU-eligible and statically costable — it is not a speedup you collect on the CPU.

A note on one branchless idiom this doc gestured at: the *transition-table-as-value composed by a parallel
scan* (the simdjson / data-parallel-FSM technique, the only way to parallelize an inherently sequential state
machine) is **not expressible in arche today.** `scan` is an unimplemented Phase-2 spec
(`tests/unit/language/collectives/scan_int.arche:2`, "red until scan is implemented"), and even the spec
restricts the scan operator to a fixed monoid set with a known identity (`semantic.c:2496`) — so a
user-defined associative combiner (composing transition functions) would be a real language feature
(custom-operator scan), not a design idiom over existing primitives. Treat the scan-FSM as future work, not
as something the patterns below can lean on.

## 8. Control flow in leaves is banned; the monadic tail decomposes

The endpoint of "everything that touches data in parallel is branchless" is a hard rule, with the same
character as the flat-effect model's *a system cannot call a system*: **data-dependent control flow in a leaf
(`map`/`system`) is unspellable.** Generalize `map`'s existing `E0046` to every leaf — a branch or a
data-dependent loop in a leaf body is a hard error, with **no flag and no `@allow`**. Hitting it is not a
case to opt out of; it is a signal to *redesign*. Control flow goes to exactly three places instead:

- **pure branching/looping → a `func`** (its legitimate home — recursion is free there; it runs on the CPU,
  where branches cost nothing);
- **conditional effects → selective values** (`whenS`/`ifS`) — a value the leaf runs, not an `if` in its body;
- **result-dependent sequencing → decompose** (state-as-data; below).

The objection is "but the monadic tail — read a header, then read *that many* bytes — has to live somewhere,
and the flat-effect model puts it inline in a `system`/`map (Q) eff`." The claim here is that the monadic tail is **not
irreducible**: it decomposes, and the ban is what forces the decomposition.

```arche
// BEFORE — the BANNED inline monadic form (data-dependent sequencing + branches in a leaf):
handle :: map (query { fd, ok }) eff {
  hdr: [1]char;
  read(fd, hdr, 1)(got:, err:);
  if err != 0 || got == 0 { ok = 0; return; }   // branch + early return — banned
  len := int(hdr[0]);
  body: [256]char;
  read(fd, body, len)(got:, err:);                // read #2 SHAPED BY read #1's runtime result
  ok = (err == 0 && got == len);
}
```

```arche
// PROPOSED — state as data, one branchless step per tick. States linear so "phase complete" = +1:
//   NEED_HDR = 0, NEED_BODY = 1, DONE = 2.   `want - have` is the bytes to pull in BOTH phases
//   (want = HDR in phase 0, want = HDR + len in phase 1), so the read needs no state branch.
tick :: map (query { fd, state, want, have, buf } as r) eff {
  going    := (state < DONE);              // 1 while unfinished, else 0   (mask)
  req      := (want - have) * going;       // bytes to pull; 0 once DONE   (mask)
  recv(fd, req)(buf, got:);  have = have + got;
  was_hdr  := (state == NEED_HDR);         // mask, BEFORE advancing
  done_now := (have >= want) * going;      // finished this phase this tick? (mask)
  want   = select(done_now * was_hdr, HDR + body_len(buf), want);  // re-arm `want` on header completion
  state  = state + done_now;               // 0->1 (hdr), 1->2 (body); never past DONE

  whenS(state == DONE, send_all(fd, build_response(buf, want)))(_:, _:);  // reply — selective, not an `if`
  whenS(state == DONE, delete(r))(_:);                                     // evict — `delete` IS an effect
}
```

`select(c, a, b)` and mask-arithmetic are real (`tests/unit/gpu/physics_step.arche:15`; README "Conditional
Behavior", `tests/unit/language/types/int_mask_times_float_column.arche`). `delete` is a structural-mutation
effect, so the selective rung gates it exactly like `send`; it lowers to a guarded emit into the deferred
command buffer, flushed at the stage barrier (which also sidesteps iterator invalidation — you queue the
delete, the body finishes on the present row, removal happens at the seam).

Two facts worth pinning:

- **It collapses to ONE `map (Q) eff`, not separate systems.** Once conditionality is `whenS`/`select` values, there
  is nothing to *call* between the steps, so advance + reply + evict are one body. Separate systems are forced
  only by a real barrier: a deferred `insert`/`delete` must land before the next step reads pool *membership*;
  a cross-row reduction/sort must complete; or the next step queries a different archetype. None hold here.
- **Decomposition spreads work across *ticks*, not *systems*.** The `read#1 then read#2` sequencing became
  `tick` re-run each loop iteration — same system count as the inline version. "ECS decomposition" (a producer
  system writes a column a consumer reads) is only forced when the monadic step must be *shared between*
  systems or hits one of those barriers; a self-contained framed read is just "reify state into columns + loop
  one branchless `map (Q) eff`."

**Honest cost (cross-ref §7): on a CPU this is objectively slower and buys nothing there** — per-tick rescan
of every active connection, polling `recv`s that mostly return EAGAIN, and predicated waste (`body_len` and
`build_response` evaluated every tick). The inline branchy event loop, driven by readiness (epoll), wins on a
CPU. The branchless form is the right shape *only* if this FSM is placed wide (§7).

**Prerequisites / consequences.** This rule is not free to adopt:

1. The **selective layer must be finished** — value-producing `ifS`, not just the currently-wired guard form
   (`whenS`, `ifS` with a `pure()` else) — or legitimate static conditionals have nowhere to land.
2. It **changes the flat-effect model's position.** `the-flat-effect-model.md` §5/§8 currently *permit* the
   monadic tail to live inline in a flat `system`/`map (Q) eff` as an "honest cost." This rule rewrites that to "monadic
   tail = redesign trigger → decompose."
3. The one sanctioned holdout — `csv.load` under `@allow(proc_not_primitive)` — must become the pending
   archetype-targeted load system, or the no-escape-hatch rule has an escape hatch.

## 9. The permission signature (one `system`, kinds as presets)

The kinds stop being primitives. There is one `system`, carrying a **permission signature**: the read set
(the query), the write set (a `(mutables)` list), and the `eff` flag. The write set is a *separate list*, not
Bevy-style `&mut` inside the query, because arche queries are **named and shared** across many systems while
mutability is per-use — so it hoists out of the query to the system that writes. `map` (pure and `eff`) is then
just named flag-presets over this signature (`map` = no `eff`, fanning over a query; etc.), not distinct kinds.

There is deliberately **no branch flag** on this signature, and **no branch color on funcs.** Leaves are
branchless *by ban* (§8), so they are GPU-eligible by construction; funcs may branch freely and run on the
CPU. Branch-ness is not a permission to grant — it is just the CPU/GPU axis (§0), derived whole-program in a
release build (§6) and invisible in dev. The only signature-level capability that gates placement is `eff`
(effects ⇒ CPU); branch-freedom rides along as part of the derived GPU-eligibility predicate, never as a
declared bit.

---

> **Relationship to landed law.** §6-§9 are WIP. Promoting them would amend
> [the-flat-effect-model.md](../design/the-flat-effect-model.md) §5/§8, which today *permits* the inline
> monadic tail. Until then, the flat-effect model is the source of truth and these sections are a proposed
> direction, not current behavior.
