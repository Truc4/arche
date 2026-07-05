# Systems and permissions — the kernel model

A kernel in arche is defined by two orthogonal choices plus an explicit permission signature:

```
<kind> <selector> (<writes>) [eff]
```

- **kind** — `map` or `system`: are you handed each *individual* element, or the *whole column*?
- **selector** — a query or an archetype: *which* data you operate on.
- **(writes)** — the selector-bound columns the body assigns.
- **eff** — present iff the kernel may run effects (insert/delete, extern/proc calls, I/O, running an `Eff`).

Everything else (branchlessness, GPU-eligibility, read set) is *derived* from the body and the signature, not
declared.

## The two axes

**Kind — individual vs column.** This is the one thing you can't read off a statement. `pos = pos + vel` is
byte-identical in a `map` and a `system`:

- **`map`** hands you each **individual**: `pos` is *this* entity's scalar, and the body runs once per row
  (fanned over the selector).
- **`system`** hands you the **whole column**: `pos` is the entire column, and the body runs once (a bulk
  vector op, a reduction, the composer).

**Selector — query vs archetype.** A **query** (a set of components) is source-agnostic: it matches every
archetype that has those columns. An **archetype** names one pool. Either way the selector **binds its columns
by bare name** (`pos`, `vel`) in the body. Ambient state is the exception: a singleton is pool-qualified
(`Config.gravity[0]`) because it isn't part of the selector. A `system` with *no* selector is the degenerate
composer — handed only ambient state and effects.

All four combinations are valid: `map (query{…})`, `map (Archetype)`, `system (query{…})`, `system
(Archetype)`.

## The permission signature

A kernel is **pure by default**. The signature declares exactly what it may touch:

- **Reads** — the selector's columns (derived, not written out).
- **Writes** — `(pos, vel)`: the selector-bound columns the body assigns. A body that writes an undeclared
  column, or omits `(writes)` while writing one, is a hard error; over-declaring is allowed (keeps the check
  sound). Whole-column bulk initialization in a run-once `system` (`R.v = { … }`) is not a selector-bound
  write and isn't declared. Hand-indexing the kernel's own selector pool to write it (`R.v[i] = …`) inside a
  selector kernel is a hard error — use the bound bare column.
- **`eff`** — the kernel may run effects. Without it, effects are a hard error; column and singleton writes
  are data work, not effects.
- **branch** — data-dependent control flow is *derived* from the body, never declared. A kernel with none is
  branchless, which is what makes a pure `map` GPU-eligible.

## `each` is `map + eff`

There is no `each` keyword. The effectful per-entity fan is spelled **`map (Q) eff`** in every position:

- **Top-level**: `name :: map (Q) eff { … }`.
- **Inline** in a `system`: `map (Q) eff { … };` — a per-entity fan that captures the system's context (an
  `fd`, singletons).
- **Row-binder**: `map (query {…} as w) eff { … }` binds the matched row's handle to `w` for `delete(w)` or
  relationship filters. `as` without `eff` is an error — the binder only makes sense for the effectful fan.

`map (Q) eff` lifts the pure-map restriction to branchless transforms, so it may branch and run effects (it
runs on the CPU). `each` is now an ordinary identifier.

## Composition: `system { map }` only

- A `system` (the composer) may contain an inline `map` fan that captures its context and may carry `eff`.
- Never `map { system }` — a kernel can't open the composer (kernels are flat; a system can't call a system).
- A pure standalone kernel is a top-level `map` (scheduled by name, placement-derived, GPU-able). Effectful
  per-row work that needs context is a `map + eff` nested in a `system`.

```arche
serve :: system eff {                 // composer: ambient + effects + a captured fd
  open_conn(8080)(fd:, ok:);
  if ok {
    map (query { row, ok }) eff {      // per-entity fan; captures fd
      write(fd, fmt(row))(n:, err:);
      ok = (err == 0);
    };
  }
}
```

## Why `map` is the default kernel

- **Fused by construction** — intermediates are registers; one pass per entity. Whole-column `system` ops are
  unfused by default (each statement is a separate column sweep — measured ~2.4× on the physics step).
- **1:1 to the GPU** — the body *is* the shader invocation.
- **Control flow is natural** per entity, instead of whole-column mask gymnastics.
- **The signature is the body's interface** — reads are the selector columns, writes are the mutables.

A whole-column `system` op, if you want it fused, lowers to a per-entity `map` fan — so `map` is the
primitive and the column `system` op is the bulk form.

## Placement

A pure `map`'s branchless, effect-free signature is exactly what makes it GPU-eligible — no `@gpu` needed. The
CPU-vs-GPU choice is then derived per machine by measurement and frozen into the build, and GPU **residency**
(which pools stay in VRAM across dispatches) and the coherence **syncs** back to the host are derived from the
same read/write footprint — so `@gpu`/`@resident`/`gpu.sync` are all optional overrides. See
[placement](static-mapper.md).
