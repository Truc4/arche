# Queries, maps, systems, effects — the data model

The canonical model for how arche programs touch data and run effects. When the code disagrees with this
doc, one of them is wrong — reconcile, don't guess. (Supersedes the earlier "system is the effectful sibling
of map / system(query) is a per-entity fan" description, which was wrong on two counts noted below.)

## The cast

Everything below that touches pool data is **handed columns** by its query — not per-entity scalars, not a
hidden row cursor. What differs is what each one is *allowed to do* with those columns:

- **`func`** — a pure value. Reads no pool/global state. **Computes** (returns values) and **builds `Eff`
  values**. Never runs an effect. Its **return must be consumed** — calling a func and discarding the result
  is a no-op the compiler deletes (there were no side effects to keep). To thread state you rebind the
  return: `px := paint(move px, …)`.
- **`map`** — handed columns; a **pure** per-element kernel. Branch-free, effect-free (E0046), GPU-portable.
- **`each`** — handed columns; the **per-element fan** when the work has control flow / effects (a `map` that
  isn't branch-free). If each element is **independent**, it parallelizes like a `map`. If it threads an
  **accumulator** (each step consumes the previous result — folding shapes onto one framebuffer), it is a
  **sequential fold/scan**, not parallel. *(Not yet built.)*
- **`system`** — handed columns; **runs effects** over them (builds `Eff`s via funcs, runs them with `()`). A
  system **never iterates**: the instant it feeds a column into a per-element scalar call it has secretly
  become an `each`. It only composes/runs the **boundary effects**.
- **`#run`** — the orchestrator: the compile-time-folded schedule. The only place dispatch is composed.

Removed / retiring: **`proc`** (gone — split into `func` that builds and `system`/effects that run);
**`run` the statement** (retiring — redundant now that `#run` schedules and an `Eff` is run with `()`; it was
also the only way to write the "super-system" smell of one system imperatively dispatching another).

> Why the old `system(query)` was wrong: it handed per-row **scalars** instead of **columns**, and ran a
> per-entity **body** instead of **composing an `Eff`**. Both are corrected above.

## Queries

A query is **source-agnostic**: it names **components**, never an archetype/pool. `query { pos, vel }`
matches any entity that has those components. **A query gives you the columns** — joined across archetypes if
it spans several. `map`/`each`/`system` are *handed* those columns.

- **Selector forms are interchangeable**: inline `query { … }`, a named `Q :: query { … }`, or an archetype
  name all mean the same thing — the columns.
- **A join is a tuple of queries**: `(A, B)` / `(query{…}, query{…})`. First cut: one side is the driver,
  every other side is a `[1]` **singleton broadcast** (every driver row sees the singleton). A true N×M join
  needs a relationship/key (future).
- **No hand-indexing.** `Pool.col[i]` (incl. the singleton `[0]`) outside a query is banned (W0029). You get
  values by querying, never by indexing.

## Effects as values (`Eff`)

An effect is a value. A **func builds** an `Eff` (composes effects); a **system runs** it.

- An `Eff` is **run by applying its out-params with `()`**: `build(args)(out:)`. Not with `run`.
- **`|>`** maps a **pure** func over an `Eff` (`fmap`) — left **must be an `Eff`**, right a pure func. It is
  **not** a generic value-pipe: `px |> f` where `px` is plain data is ill-typed, and a terminal effect on the
  right is not `fmap`. So `|>` only appears where an effect *yields a value* (e.g. `poll()`), not on a path
  whose only effect is a terminal commit. **`seq`** sequences independent `Eff`s (applicative).

### The applicative invariant (and why storage lives in pools)

`Eff` is a **free applicative**: effects are composed **independently**. A later effect **cannot depend on an
earlier effect's runtime output** — that dependency is monadic (`bind`), which the applicative layer does not
provide.

This is the real reason **all long-lived storage lives in pools** — it is not a style rule, it is what keeps
the effect layer applicative:

- If an effect *produced* long-lived data that a later effect *consumed* — e.g. a `frame` effect returning
  the framebuffer that a `present` effect commits — that is exactly the forbidden monadic dependency.
- Put the data in a **pool** instead (shared state, handed to the system as columns), and the effects stop
  threading outputs to each other. The pure transforms in between read/write pool state; the only real
  effects are the **boundary commits**. Independent effects → free applicative holds.

**FFI**: avoid it; when a C pointer is unavoidable it must be **opaque** (never a raw pointer handed to
arche — see slices).

## A procedure is composed, not a kind

There is no `proc` — and more deeply, **a "procedure" (a multi-step effectful action) is not a declared kind
at all.** It is *composed*: pure `func`s build `Eff` values, `|>` threads a pure transform through an effect,
and a `system` runs the result with `()`. The effectfulness lives in the **composition**, never in a keyword.

So "draw one circle as a procedure" means *compose* it from a pure func + a boundary effect —

```
painted := circle(move px, w, h, cx, cy, r, color);   // pure: paint the disc, get the canvas back
blit(handle, move painted)()                            // run the one effect
```

— not a monolithic `proc { … }` block. What used to be one opaque effectful kind is now three separable
things: **pure** (`func`) + **effect at the boundary** (the `Eff`, run with `()`) + **orchestration**
(`system` / `#run`). Nothing *names* "the procedure" — it is the shape you get by composing those three, and
that is the whole reason `proc` is gone.

## Slices

A slice is a **view**: `{ ptr, len }`. **Its pointer is fixed at creation.** You may write *through* it
(`s[i] = x` — mutates the viewed data) but you may **never repoint it** (`s = otherSlice`). Repointing is how
a raw pointer gets laundered into a slice value and a hidden effect smuggled past the type system (the old
gfx `frame` did `px = raw[0:w*h]`, turning a device pointer into "plain data"). Tests:
`errors/slice_repoint_rejected.arche`, `arrays/slice_write_through_ok.arche`.

## Worked example — gfx circles

The **canvas lives in the `Window` pool** (`px` + dims + the **opaque** handle). A `Circle` is just a
**shape** (`cx, cy, r, color`) — it does not own pixels. Three pieces: a pure unit, a fold that batches it,
and the one boundary effect.

```
// canvas is Window state; a Circle is just a shape
Window :: arche { handle :: window  px :: []int  w :: int  h :: int }
Circle :: arche { cx :: int  cy :: int  r :: int  color :: int }

// pure UNIT: paint one disc onto a framebuffer, and RETURN the framebuffer (the return is the whole point)
circle :: func(own px: []int, w: int, h: int, cx: int, cy: int, r: int, color: int) -> []int { … return px }

// BATCH: a sequential fold over the shapes — move the canvas through each circle and rebind the result
circles :: each (Circle, Window) {
  px := circle(move px, w, h, cx, cy, r, color);
}

// COMMIT once: the only device effect (blit px to the device via the opaque handle), run with ()
present :: system (Window) {
  blit(handle, px)();
}
```

- **`px` is Window state, not Circle's.** `circle` is really `paint_circle(canvas, shape) -> canvas`: it
  takes the canvas (from `Window`, broadcast `[1]`) and a shape (from the `Circle` column) and hands the
  canvas back. The shapes are folded onto the one canvas.
- **The return must be used.** `circle(move px, …);` alone would be a dead no-op. `px := circle(move px, …)`
  moves the canvas in (consuming the old binding — `own`/`move` kills it), then rebinds the returned canvas
  with `:=`. Linear, zero-copy, fused in-place; nothing copied, nothing aliased, no slice repointed.
- **`circles` is a fold, not a parallel `each`** — each shape consumes the previous canvas, so order matters
  (overlap) and it can't run out of order.
- **No `|>`, no `frame`.** There is no value-bearing `Eff` on the draw path to `fmap` over — the pixels are
  pool state painted by the fold, and the **only** effect is `present`/`blit` at the boundary. That keeps it
  applicative (the fold's writes and the blit don't thread outputs through each other).
- **One-off** (draw + show a single circle): `painted := circle(move px, …); blit(handle, move painted)()`.

## Bans (status)

- **No mutable globals**, even `#file`-private — W0022 (widened). Error-default.
- **No `Pool.col[i]` outside a query** — W0029. Warn-default, app build sets it to error.
- **No slice repoint** — test landed RED; rule to build.
- **Joins on `map` are rejected** — joins are systems-only.

## Open / not yet built

- `each` (the effectful/control-flow per-element fan) and its **fold/scan** form (threading an accumulator).
- gfx as data: framebuffer-as-`Window`-pool-`px`, `circle` the pure unit, `circles` the fold, `present`/
  `blit` the one boundary effect via the opaque handle. `frame` removed.
- `run` (the statement) retirement + `map` dispatchable directly from `#run`.
- A pure func's discarded-return → no-op (lint?), and the move/`:=` rebind threading for `own` returns.
- N×M joins (relationships/keys).
- Slice-repoint enforcement (test landed RED).
- `system` must hand **columns** and compose effects — the shipped `system(query)` per-row fan is wrong and
  needs reworking (it currently hands per-row scalars and runs a per-entity body).
