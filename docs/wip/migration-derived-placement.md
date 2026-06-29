# Migrating arche-rpg to Derived-Access Placement and Scheduling

A worked example of the proposition in `notes.md` #6: the compiler **derives** per-column
read/write facts from how `map` / `each` / `system` touch a pool, and uses those facts to drive (1) the
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
| `game.step` (map over `Movers`) | `pos`, `vel` | `pos`, `vel` | branch-free, so a GPU kernel candidate |
| `gfx.circle` (reads Player to draw) | `pos`, `color`, `r` | none on the pool (writes the framebuffer) | read-only consumer of the pool |

Two conclusions fall straight out:
- `pos` and `vel` are the **hot read-write set** (written every frame by `step`).
- `color` and `r` are **read-only for the whole frame loop** (only ever read, by `circle`).

Nothing above is written by the arche-rpg author. It is recovered from the existing `map` and the existing
`gfx.circle` access pattern.

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

The author keeps writing ordinary `map` / `each` / `system` kernels over the pool. The columns' access
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
