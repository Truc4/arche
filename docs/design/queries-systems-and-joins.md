# Queries, systems, maps, and joins — the data model

This is the canonical model for how arche programs touch pool data. It sits under
[the flat effect model](the-flat-effect-model.md) (the cast of effect kinds) and
[scheduling](scheduling.md) (how work is orchestrated). When in doubt, this document wins; if the code
disagrees, the code is wrong or this doc is stale — reconcile, don't guess.

## The cast

- **`func`** — a pure value. Builds things (including `Eff` effect-values). Reads no pool/global state.
- **`map`** — the **pure columnar kernel**. Branch-free, effect-free (E0046 rejects effects/control flow),
  so it is GPU-portable. Operates on columns.
- **`system`** — the **effectful, schedulable** sibling of `map`. Same columnar body shape, but effects and
  control flow are allowed. This is what the scheduler runs.
- **the scheduler** — orchestration. A compile-time-folded `Schedule` tree (`run`/`seq`/`par`/`loop`/`when`
  /`halt`); see [scheduling](scheduling.md).
- **`proc`** — **being removed.** It was the "squeezed middle": an effect leaf invoked from a hand-written
  loop. Everything a proc did is now a `system` (effect) or a `func` (pure value). Do not write new `proc`s
  and do not write tests in terms of them.

A **`system` may call a `map`** — orchestration calling a pure kernel. Good. A **`system` calling another
`system`** is the "super-system" anti-pattern — orchestration smuggled into a leaf — and is rejected. Let
the scheduler compose systems; let systems invoke maps.

## Queries are source-agnostic

> The entire point of a query is to be source-agnostic.

A query names **components**, never archetypes or pools. `query { pos, vel }` matches *any* entity that has
`pos` and `vel`, wherever it lives. You never write the source archetype's name in a query. This is what
lets the engine pick storage, parallelize, and target the GPU.

A query **gives you the columns**. The body is **columnar**: `col = expr` auto-applies across every matched
row (data-parallel); `pos.x` is the `x` **sub-column** of the `pos` column. There is no exposed "current
row" cursor — that AoS, per-entity-cursor reading is *not* what a query is. (If a body looks like it treats
`pos.x` as one scalar handed to an effect, that is the per-entity-iteration shape, which the query model
does not provide — see Joins/relationships and "open questions" below.)

`map(Q)` and `system(Q)` both take a query selector; an **archetype name is shorthand** for "a query over
all of that archetype's columns" (`map(Movers)` ≡ `map(query{ pos, vel })` when `Movers` has exactly those).

## Joins — a tuple of queries

A join is **not** a longer column list. It is a **product of queries**, written as a tuple in the selector:

```
Movers :: query { pos, color }
Win    :: query { handle }

draw :: system (Movers, Win) { ... }                       // named queries
draw :: system (query { pos, color }, query { handle }) { … }  // inline
```

This keeps every query source-agnostic and distinguishes the three genuinely different things:

| need | syntax | meaning |
|---|---|---|
| several components, one entity | `query { pos, vel, color }` | one entity has all of them |
| **join** | `(query { pos }, query { handle })` | **product of two queries** |
| several independent walks | several separate systems | unrelated |

Each query in the tuple matches independently; the body sees the union of their columns (qualify on a name
collision).

### Cardinality

- **N×1 singleton broadcast** (first cut): when all but one query resolve to a `[1]` pool, the singleton is
  broadcast — every row of the N-side sees the single value. This is exactly what `render` needs (every
  ball joined with the one window). Unambiguous.
- **N×M** (future): a true cross product is almost never wanted without a **key or relationship**. Until
  relationships exist, an N×M tuple-join is an error ("needs a relationship/key").

The same tuple is where the rest of the **relational algebra** grows in, as operators on the queries —
`not` (exclude), `or`, and keyed/relationship joins — always over **components and relationships, never over
sources**. (flecs-style: name the relationship component + a query variable, not the other archetype.)

## Data lives in pools — the rules

Pools are the only mutable shared state. Two hard rules follow:

1. **No mutable globals — even private ones.** A scalar/buffer global with no `::` is banned regardless of
   visibility (`#file` does not exempt it). Shared mutable state belongs in a pool. (`::` consts and pools
   are exempt — they are immutable / are the data model.) *(W0022 today only catches **exported** mutable
   globals; extending it to private ones is pending — see the RED tests.)*
2. **No direct pool access outside a query.** Reading a pool column by explicit index — `Pool.col[i]`,
   including the singleton `Pool.col[0]` hack — is banned. You **query** for pool values; you never
   hand-index. A query body uses bare column names (never `[i]`), so the ban only bites the escape hatch.
   It is a tunable lint (`--allow-pool-index` / `@allow(pool_index_outside_query)`) so test/debug
   ground-truth peeks can opt out; program logic may not. *(Pending — see the RED test.)*

Singletons are just `[1]` pools, **queried** (often broadcast-joined), never indexed at `[0]`.

## Opaque handles

An `opaque` (window, file, socket) is a black box minted **only at the FFI boundary**. It is **create-once**:
it flows by `move`/borrow/return and is **never overwritten** (E0122). It **may live in a pool** — created
via `insert` (create-once), destroyed when the row is — because a pool slot's lifecycle (create/destroy) is
exactly an opaque's lifecycle. What is illegal is **writing an opaque slot**: assigning over an opaque
global, pool column, or array element. So a window is **pool data** (a `[1]Window` pool), not a mutable
opaque global — the global form both violates rule 1 and crashes (spurious `@drop`). See
[opaque sealing](the-flat-effect-model.md).

## Worked example — the renderer

```
[1]Window;
Window :: arche { handle :: window }
[8]Ball(N);
Ball :: arche { pos(x, y) :: int  color :: int }

boot :: system {
  gfx.open(W, H, "title")(w:);
  insert(Window { handle: w })(_:, _:);   // window is pool data, create-once
}

// each ball joined with the singleton window; draw it
render :: system (query { pos, color }, query { handle }) {
  gfx.draw(handle, pos.x, pos.y, color)
}
```

No `win` global, no `Window.handle[0]`, no source names in the queries.

## Process notes

- **Tests reflect the real compiler state — even RED.** Green is never the goal; truth is. A RED test that
  asserts the correct contract is the honest signal that a feature is unfinished.
- **When a design assumption changes, update the tests first** to assert the new intent — never leave tests
  locking in behavior that has been rejected.
- **No new `proc`s** anywhere — code or tests.

## Open questions (not yet resolved)

- Observing a single value in a test without `Pool.col[i]` (the verify-peek tension) — handled for now by
  the `@allow` escape on the pool-access lint.
- Whether a `system` body may carry per-row *effects* at all, or only columnar transforms (the per-entity
  draw is really the join `(Ball, Window)` broadcast, not a per-row cursor).
- Scheduler runtime conditions: the fixed-timestep drain lives in a system today (a `for`-loop calling the
  sim map — and `for`-loops are a smell); a scheduler combinator is the eventual home.
