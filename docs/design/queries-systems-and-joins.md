# Queries, maps, systems, effects — the data model

The canonical model for how arche programs touch data and run effects. When the code disagrees with this
doc, one of them is wrong — reconcile, don't guess. (Supersedes the earlier "system is the effectful sibling
of map / system(query) is a per-entity fan" description, which was wrong on two counts noted below.)

## The cast

Everything below that touches pool data is **handed columns** by its query — not per-entity scalars, not a
hidden row cursor. What differs is what each one is *allowed to do* with those columns:

- **`func`** — a pure value. Reads no pool/global state. **Computes** (produces results — either a single
  `-> T` return or an **out-param list** `func(in)(out, …)`, the form `proc` used to own) and **builds `Eff`
  values**. Never runs an effect. Its **return must be consumed** — calling a func and discarding the result
  is a no-op the compiler deletes (there were no side effects to keep). To thread state you rebind the
  return: `px := paint(move px, …)`.
- **`map`** — handed columns; a **pure** per-element kernel. Branch-free, effect-free (E0046), GPU-portable.
- **`map (Q) eff`** — the **per-element fan**: a current element exists, so a query column is a **scalar** (`pos.x`
  is *this* element's x), and the body **may have control flow / effects** (a `map` that isn't branch-free).
  It runs over **one** query (no joins); for cross-pool work, **nest** a `map (Q) eff` fan inside another (see *Cross-pool
  work is nesting*). An anonymous `map(Q) eff { … }` may appear in **statement position** — that is the inline
  fan, emitted in place so it captures the enclosing scope. *(Built.)* The **independent fold/scan** form
  (threading an `own` accumulator) is still open.
- **`system`** — handed whole **columns**; **runs effects** over them (builds `Eff`s via funcs, runs them
  with `()`), and does column-level work. A system **never iterates** per element (`pos.x` is the x
  sub-**column**); per-element scalar work is a `map (Q) eff` fan. A run-once `system { }` (no query) is the composer.
- **`#run`** — the orchestrator: the compile-time-folded schedule. The only place dispatch is composed. A
  **leaf is a bare map/system name** — `#run seq({ boot, forever(frame) })`; there is no `run`
  constructor or `run` statement (both retired — `map` are kinds of systems, scheduled by name).

Removed: **`run`** — both the `run <map>` **statement** and the `run(...)` Schedule **constructor** are gone.
A map/system is dispatched only by **naming it in `#run`**; a system body never dispatches. (`run` is no
longer a keyword.) Removed: **`proc`** as a declared kind — a "procedure" is composed (a `func` that builds
an `Eff`, or fills out-params, + a `system`/`map (Q) eff` that runs it). A non-foreign `proc` is **W0030
`proc_not_primitive`** (error-by-default); `proc` survives **only** as the foreign/primitive boundary form —
`#foreign` externs, `@syscall`, `@intrinsic`, and `@drop` destructor hooks. There is no `main` either (a decl
named `main` is **E0225**); the entry is a `#run` schedule.

> `system(Q)` is now genuinely **columnar** (whole-column ops + boundary effects, no per-element row loop).
> The per-element fan that used to be smuggled into `system(query)` is now a `map` with the `eff` permission — **`map (Q) eff`**.

## Queries

A query is **source-agnostic**: it names **components**, never an archetype/pool. `query { pos, vel }`
matches any entity that has those components. **A query gives you the columns.** `map`/`system` are
*handed* those columns.

- **Selector forms are interchangeable**: inline `query { … }`, a named `Q :: query { … }`, or an archetype
  name all mean the same thing — the columns.
- **One query, no joins.** a `map` (pure or `eff`) runs over **exactly one** query; there is no tuple-of-queries, no
  driver/singleton broadcast, no N×M join. A multi-query `map(Q1, Q2) eff` is a **parse error**. (A singleton is
  not a real thing — see *Cross-pool work is nesting*.)
- **No hand-indexing.** `Pool.col[i]` (incl. the singleton `[0]`) outside a query is banned (W0029; the rule
  is error for program code, with `@allow(pool_index_outside_query)` for test peeks — see *Bans* for rollout
  status). You get values by querying or by per-element fan, never by indexing a pool.

### Cross-pool work is nesting, not joins

To combine two pools you **nest** one fan inside another — you do not join. the fan stays single-query; the
*cross* comes from **explicit nesting**, which is just an ordinary loop inside a loop:

```
render :: system {
  map (query { handle, bg }) eff {          // OUTER: per window — establishes the framebuffer context
    gfx_be_frame(handle)(px:);           // this window's canvas (a local)
    map (query { pos, color, r }) eff {     // INNER: per ball — captures px, paints into it
      px[…] = color;
    };
  };
}
```

Why this and not a join:

- **It's written, not guessed.** windows × balls is exactly the nesting you typed — not a hidden default the
  engine picked. (Cartesian as an *implicit* join default is wrong; as the meaning of explicit nesting,
  "draw every ball in every window" is just correct.)
- **No singleton, no `[1]` special case.** `[2]Window` just works: the outer loop runs twice. There is no
  broadcast and nothing is "the one" window.
- **No closures, no new kind.** An anonymous `map(Q) eff { … }` is the `map (Q) eff` value-form used in statement
  position (Odin/Jai: anonymous = named minus the name). It is emitted **in place**, so the body captures the
  enclosing scope lexically — there is no closure object. It is a loop, not a definition, so it does not open
  nested funcs/systems.
- **No hand-indexing of pools.** The inner fan hands you scalar `pos.x`/`color`; `px[…]` is a backend buffer
  (a plain slice), not a pool column.

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

There is no non-foreign `proc` — and more deeply, **a "procedure" (a multi-step effectful action) is not a
declared kind at all.** It is *composed*: pure `func`s build `Eff` values (or fill out-params), `|>` threads a
pure transform through an effect, and a `system`/`map (Q) eff` runs the result with `()`. The effectfulness lives in
the **composition**, never in a keyword. (`proc` itself now only spells the foreign/primitive boundary —
`#foreign`/`@syscall`/`@intrinsic`/`@drop`; see the kind table above.)

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
circles :: map (Circle, Window) eff {
  px := circle(move px, w, h, cx, cy, r, color);
}

// COMMIT once: the only device effect (blit px to the device via the opaque handle), run with ()
present :: system (Window) {
  blit(handle, px)();
}
```

- **`px` is Window state, not Circle's.** `circle` is really `paint_circle(canvas, shape) -> canvas`: it
  takes the canvas (from the `Window` fan it is nested inside) and a shape (from the `Circle` column) and
  hands the canvas back. The shapes are painted onto the one canvas.
- **The return must be used.** `circle(move px, …);` alone would be a dead no-op. `px := circle(move px, …)`
  moves the canvas in (consuming the old binding — `own`/`move` kills it), then rebinds the returned canvas
  with `:=`. Linear, zero-copy, fused in-place; nothing copied, nothing aliased, no slice repointed.
- **`circles` is a fold, not a parallel `map (Q) eff`** — each shape consumes the previous canvas, so order matters
  (overlap) and it can't run out of order.
- **No `|>`, no `frame`.** There is no value-bearing `Eff` on the draw path to `fmap` over — the pixels are
  pool state painted by the fold, and the **only** effect is `present`/`blit` at the boundary. That keeps it
  applicative (the fold's writes and the blit don't thread outputs through each other).
- **One-off** (draw + show a single circle): `painted := circle(move px, …); blit(handle, move painted)()`.

## Bans (status)

- **No mutable globals**, even `#file`-private — W0022 (widened). Error-default.
- **No `Pool.col[i]` outside a query** — W0029. **The rule is error for program code**, with test
  ground-truth peeks opting in via `@allow(pool_index_outside_query)`; you read pool data by querying or by a
  per-element fan, never by indexing a pool. *Status:* the compiler already supports error-by-default
  (`g_werror`), but the flip is **pending a peek migration** — ~126 existing tests + doctests legitimately
  peek and need `@allow` (or a harness-level demotion) first, so it ships **warn-default** until that lands
  (`--pool-index=error` opts in today).
- **No slice repoint** — test landed RED; rule to build.
- **No joins on `map`** — both run over exactly one query; a multi-query `map(Q1, Q2) eff` / `map` join
  is a parse error. Cross-pool work is **nesting** (a `map (Q) eff` fan inside another), not a join. (`system(Q1, Q2)`
  multi-query is a remaining holdover — it should fold into nesting too; tracked as a TODO.)

## Built (this model)

- **`map (Q) eff`** — the per-element fan (scalars + control flow + effects), single-query; cross-pool by nesting
  (incl. anonymous inline `map (Q) eff` in statement position). Dispatched by name from `#run`.
- **`system(Q)` is columnar** — whole-column ops + boundary effects, no per-element row loop.
- **`run` removed** — bare map/system names are schedule leaves; `@gpu` is a decorator on the map decl.

## Open / not yet built

- the fan's **independent fold/scan** form (threading an `own` accumulator through the elements).
- gfx as data: framebuffer-as-`Window`-pool-`px`, `circle` the pure unit, `circles` the fold, `present`/
  `blit` the one boundary effect via the opaque handle. `frame` removed.
- A pure func's discarded-return → no-op (lint?), and the move/`:=` rebind threading for `own` returns.
- **Relationships (keyed cross-pool pairing) — TODO.** Nesting (above) is how the `map (Q) eff` fan does cross-pool work
  *today*: the inner fan sees **every** element of its pool (a full cartesian with the outer). That is right
  when the pairing is shared context (all balls into the same window) or when "everything × everything" is
  what you mean. It is **not** enough when each entity should pair with a *specific* other entity — "this ball
  belongs to *that* window." That is a **relationship**: a keyed reference stored per-entity, resolved by a
  **query variable** (flecs-style `$w`). The shape it will take is a **filter on the inner fan**, not a new
  join construct:

  ```
  map (query { handle } as $w) eff {        // bind the matched window to $w
    map (query { pos, color, r } where window == $w) eff {   // only THIS window's drawables
      …
    };
  };
  ```

  So relationships *narrow* an existing nesting from cartesian to the matched pairs — they are a refinement of
  nesting, not a different mechanism, and nothing written for nesting is thrown away. Needs: a relationship
  component type (an entity reference), query variables (`$w`), and the join solver that binds them. Until
  then, cross-pool pairing that isn't shared-context or full-cartesian has no first-class form (model it with
  an explicit key column + an `if` in the inner body).
- Slice-repoint enforcement (test landed RED).
