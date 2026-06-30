# Systems and permissions — the unified kernel model

> **Status: in progress — Slices 1 (`eff`) + 2 (`(writes)`) landed; `each` keyword removed.** The model
> unifies the kinds to **`map` + `system`** under an **explicit permission signature**
> `<kind> <selector> (<writes>) [eff]`. The effectful per-entity fan is spelled **`map (Q) eff`** (the
> `each` keyword is gone — now an ordinary identifier). The `eff` permission and the explicit `(writes)`
> write-set are implemented and enforced; the remaining slices (decl collapse, placement derivation) are
> below.

## Slice plan (build order)

The full model is sliced for tractability; each slice is fully correct on its own:

1. **`eff` permission — DONE.** Uniform on both kinds, **pure-by-default**, enforced as a hard error
   (`E0226 effect_without_eff`), propagating up through nested kernels. A `map`/`system` with no `eff`
   may not run effects (insert/delete, extern/proc calls, running an `Eff`, I/O); column/singleton writes
   are data work, not effects. `map (Q) eff` is the effectful per-entity fan and lifts the pure-map E0046
   transform-only restriction.
2. **Explicit `(writes)` signature — DONE.** A selector kernel declares the bound columns it assigns —
   `map (Movers) (pos, vel) { … }` — checked by a body write-walk (`kernel_write_walk`): writing an
   undeclared bound column, or omitting `(writes)` entirely, is a hard error (`E0227 write_set_mismatch`);
   over-declaring is permitted (keeps the check sound). A `(writes)` list covers **selector-bound columns
   only** (bare names); a run-once `system`'s whole-column bulk init `R.v = { … }` stays legal and is not
   declared. Hand-indexing the kernel's own selector pool to write it (`R.v[i] = …`) inside a selector
   kernel is a hard error (`E0228 indexed_write_in_selector`) — use the bound bare column; loaders / run-once
   systems may still hand-index (the W0029 lint). The write-walk is the per-column write fact Slice 4's
   placer reads off the signature.
2b. **`each` keyword removed — DONE.** `map (Q) eff` parses directly to the per-entity fan (`SN_EACH_EXPR`
   machinery) — top-level *and* inline statement position — and supports the `as w` row-binder
   (`map (Q as w) eff`). The `each` keyword is removed (now an ordinary identifier); all sites migrated.
3. **Decl collapse** — fold `HirMapDecl`/`HirEachDecl`/`HirSystemDecl` into one decl + permission flags.
   (`HirEachDecl` is the internal fan representation `map (Q) eff` lowers to; the surface keyword is gone.)
4. **Placement / residency derivation** — the placer reads the signature (see `static-mapper.md`).

### Resolved design decisions (locked)

- **Signature grammar** `<kind> <selector> (<writes>) [eff]` — the `eff` permission is a bare contextual
  keyword trailing the selector (`system (Q) eff { … }`). (Slice 1 parses/enforces `eff`; `(writes)` is
  Slice 2.) *Resolved: yes.*
- **Writes explicit** — the mutable set is declared (selector-bound columns), checked against the body.
  *Resolved: yes — implemented in Slice 2 (`E0227`/`E0228`).*
- **`branch` derived, not declared** — branchlessness (GPU-eligibility) is derived from the body; there is
  no `branch` keyword. *Resolved: yes.*

### `each` status — removed

The `each` keyword is **gone**. The per-entity effectful fan is `map (Q) eff` in every position:

- **Top-level**: `name :: map (Q) eff { … }`.
- **Inline** (nested in a `system`): `map (Q) eff { … };` — a per-entity fan capturing the system's
  context (this is what an inline `each` was).
- **Row-binder**: `map (query {…} as w) eff { … }` binds the matched row's handle to `w` for `delete(w)` /
  relationship filters. `as` without `eff` is an error (the binder only makes sense for the effectful fan).

Internally `map (Q) eff` parses to `SN_EACH_EXPR` and reuses the existing fan machinery (HIR `HirEachDecl`,
codegen row loop) unchanged — only the surface keyword was removed. `each` is now an ordinary identifier.

## Two orthogonal axes

A kernel is defined by two independent choices:

1. **Selector — query vs archetype — *what* data you operate on.**
   - A **query** (a set of components) is source-agnostic: it matches every archetype that has those columns.
   - An **archetype** names one specific pool.
   - Either way, the selector **binds the columns by bare name** (`pos`, `vel`) in the body.

2. **Kind — `map` vs `system` — *how* you're handed it: individual or column.**
   - **`map`**: handed each **individual** — `pos` is *this entity's* scalar value; the body runs per row
     (fanned).
   - **`system`**: handed the **whole column** — `pos` is the entire column; the body runs once (bulk).

They're orthogonal: `map (query{…})`, `map (Archetype)`, `system (query{…})`, `system (Archetype)` are all
valid combinations.

## The disambiguation (the column-vs-individual question)

`pos = pos + vel` is byte-identical in a `map` and a `system`. **The kind decides** whether `pos` is an
individual scalar (`map` → per-entity add, fanned over rows) or the whole column (`system` → one bulk vector
add). That is *why* `map` and `system` are separate kinds: the kind is the **individual-vs-column** marker,
and it cannot be read off the statement. The selector (query/archetype) is the *other* axis — it only says
*which* columns are in scope, never the granularity.

Ambient state is the exception to bare-name binding: a singleton read inside any kernel is pool-qualified
(`Config.gravity[0]`) because it isn't part of the selector — selector columns are bare, ambient/singletons
are qualified. A `system` with **no selector** is the degenerate composer: handed only ambient state and
effects.

## Permissions = the signature (kind/preset implied by it)

A kernel declares its access: **read set** (selector columns it reads) + **write set** (`(mutables)`) +
**`eff`** (may run effects) + **branch** (data-dependent control flow; absent ⇒ branchless ⇒ GPU-eligible).
Presets:

- **`map`** = individual, **no `eff`** — the pure per-entity kernel (branchless unless `branch`); fused,
  GPU-eligible, placement-derived.
- **`each`** is **not a keyword** — it's **`map + eff`** (a per-entity fan that runs effects; CPU).
- **`system`** = column (or, with no selector, ambient) — bulk ops, reductions, effects, the composer.

## Composition: `system { map }` only

- A `system` (composer) may contain an inline `map` fan that **captures the system's context** (an `fd`,
  singletons) and may carry `eff` — this is exactly what `each` was.
- Never `map { system }` — a kernel can't open the composer (the system-can't-call-system flatness ban).
- Pure standalone kernel → top-level `map` (scheduled by name; placement-derived; GPU-able). Effectful
  per-row work needing context → `map + eff` nested in a `system`.

```arche
serve :: system eff {                 // composer: ambient + effects + captured fd
  open_conn(8080)(fd:, ok:);
  if ok {
    map (query { row, ok }) eff {      // individual fan; captures fd; the effectful per-entity fan
      write(fd, fmt(row))(n:, err:);
      ok = (err == 0);
    };
  }
}
```

## Why individual (`map`) is the default kernel

- **Fused by construction** — intermediates are registers, one pass per entity. Whole-column `system` ops
  are unfused-by-default (each statement a separate column sweep; measured ~2.4× on the physics step).
- **1:1 to GPU/SIMT** — the body is the shader invocation.
- **Control flow is natural** per entity (vs whole-column mask gymnastics).
- **The permission signature is the body's interface** — reads = selector columns, writes = mutables.
- A whole-column `system` op, if you want it fused, lowers to a per-entity `map` fan — so `map` is the
  primitive; column `system` ops are the bulk form (or sugar that desugars to a fan).

## Placement hook (forward reference)

The permission signature is exactly what the placer reads to derive scheduling, CPU/GPU, and residency level
(`../migration-derived-placement.md` §5–§9, `static-mapper.md` — same folder). Legality + residency-class
derive cleanly from the signature; profitability is the open part. `map`'s branchless + no-`eff` signature is
what makes it GPU-eligible by construction.

## What changes vs today

| | today | proposed |
|---|---|---|
| kinds | `map`, `each`, `system` | `map`, `system` (`each` = `map + eff`) |
| individual vs column | `map`/`each` = individual, columnar `system` = column | the **kind** marks it (map = individual, system = column) |
| selector | query (map/each) or pool-qualified `Pool.col` (system) | **query or archetype**, binding **bare names**, on either kind |
| read/write, eff, branch | inferred / implied by kind | the explicit permission signature |

## Open questions (remaining after Slice 1)

- ~~Surface syntax for the signature~~ — **resolved**: `<kind> <selector> (<writes>) [eff]`.
- ~~`branch` as an explicit permission vs derived~~ — **resolved**: derived (no keyword).
- Whether column `system` ops stay a distinct bulk form or become sugar that desugars to a per-entity fan.
  *(Open — Slice 3/4.)*
- ~~Migration: existing `each` → `map + eff`~~ — **done** (keyword removed; top-level + inline + `as`
  binder all migrated). Remaining: existing `Pool.col` columnar systems → `system (selector)` with bare
  names *(Slice 2)*.
- ~~Inline `map (Q) eff` as a statement has no lowering~~ — **done**: `map (Q) eff` parses to the fan node
  in statement position too, so inline fans work.
