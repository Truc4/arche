# Idiomatic boids — the language gaps it exposes

The boids demo written the way it *should* look — `pos(x, y)`/`vel(x, y)` 2-vectors, and the self-join as a
`system` querying whole COLUMNS wrapping a `map` querying per-element SCALARS — does not compile. This is the
honest target; the gaps below are **real bugs to fix**, not to work around. The demo source
(`arche-rpg/game/game.arche`, `app/app.arche`) is intentionally left in this idiomatic, non-compiling state.

Each gap has a minimal repro (build with `ARCHE_NO_GPU=1 build/arche run <file>`).

## The vector data model (`pos(x, y)` as a real 2-vector)

**G1 — no tuple value literal / named-vector constant.** A named 2-vector constant declared like a component
(`CENTER(X, Y) :: (320.0, 240.0)`, components named like a constant, structured like `pos(x, y)`) can't be
written — neither the `(X, Y)` component names on a constant nor the `(320.0, 240.0)` value literal parse.
`pos(x, y)` today declares a column *shape*, not a value type.
```
CENTER(X, Y) :: (320.0, 240.0);   // Error: Expected ')' after expression
```

**G2 — a tuple type isn't a value type with fields.** A `func` param typed as the tuple parses, but `.x`/`.y`
don't resolve — so you can't pass/return a vector and use its components.
```
pos(x, y) :: float;
f :: func(a: pos) -> float { return a.x + a.y; }   // Semantic: type 'pos' has no field 'x'
```

**G3 — a func returning a tuple mis-lowers.** `-> pos` reaches codegen and dies (`invalid type for function
argument`). Needed for `near`/`push` returning vectors, and vector arithmetic (`a - b`, `v / s`, `v * s`).

**G4 — FLOAT tuple sub-column hand-indexed WRITE mis-lowers to i32.** The store type defaults to `i32` for a
nested `field.field` base (`codegen.c:~9732`; the type fallback only handles a plain-NAME base, so `Pool.tup.sub`
misses and stays `i32`). Correct for an `int` tuple (i32 is the type), WRONG for `float`. The passing tuple
tests (`sys_tuple_subcol_write/read`, `tuple_*`) all use `int` and/or whole-column writes (`Mover.pos.x = {…}`)
— the float + hand-indexed `[i]` case is uncovered. Blocks seeding `Boid.pos.x[i] = …`.
```
pos(x, y) :: float;                                   // int here → works (v=0 1 2); float → the bug
P :: arche { pos }
[3]P(3);
s :: system eff { for (i:=0;i<3;i+=1){ P.pos.x[i] = float(i); P.pos.y[i]=0.0; } }
s
// opt: '%v10' defined with type 'float' but expected 'i32'
```

**G5 — tuple sub-column READ in a map/reduce mis-lowers to i32.** Same root as G4 — `reduce(+, B.pos.x)` /
`pos.x` in a map body hit the i32 default. So even reading a vector component in a kernel is broken.

## The honest self-join (`system` columns ∘ nested `map` scalars)

**G6 — the `as` self-binder is welded to `eff`.** A self-binder is needed ONLY inside the self-join's nested
map, to disambiguate this boid (`me.pos`) from the enclosing system's same-named column (`pos`). (A plain map
like `apply` needs no binder — bare names already ARE its per-element scalars.) But binding self is rejected
unless the map also declares `eff` — and binding self is not an effect. Same category as the (now-fixed)
"branching welded to the kind": a capability tied to the wrong axis.
```
m :: map (query { x } as me) (x) { me.x = me.x * 2.0; }
// Error: the `as` row-binder requires the `eff` permission
```

**G7 — a columnar `system(query)` can't host a nested `map`.** The core of the honest structure ("system
queries the neighbour COLUMNS, nested map queries the self SCALARS") does not parse — a columnar system body
only accepts `col = expr`, not a nested map fan (unlike a run-once `system { }`, which does host maps).
```
sys :: system (query { v }) { map (query { o }) (o) { o = reduce(+, v); } }
// Error: Expected ';' after statement
```

**G8 — `reduce` can't fold a `system`'s bound column.** The honest fold domain is the enclosing system's
queried column (a *declared*, per-permission whole-column view), not a raw `Pool.col` poke. Today the reduce
only recognizes raw `Pool.col` FIELD refs (Stage B's expedient form) — it has no notion of folding over a
column the enclosing system queried for. Blocked from even being expressed by G6+G7.

## Consequence

The idiomatic self-join —
```
flock :: system (query { pos, vel }) {           // COLUMNS: the neighbour domain
  map (query { pos, vel, nvel } as me) (nvel) {   // SCALARS: this boid
    cnt := reduce(+, near(me.pos, pos));          // fold the system's `pos` column, gated by me.pos
    ...
  }
}
```
needs **G6 + G7 + G8** together (the honest structure), on top of **G1–G5** (the vector data model). The
current working demo uses flat `px/py` + a bare map with `reduce(+, Boid.px …)` — which is exactly the
dishonest form (a whole-column op smuggled into a map, reaching a raw column it never queried). Fixing these
makes the demo both correct *and* idiomatic.

## Captured as RED tests (no XFAIL — honest-red until fixed)

Each gap has a failing test asserting the *correct* behaviour; it flips green when the gap is fixed.

| Gap | Test |
|---|---|
| G1 (tuple value literal / named-vector const) | `tests/unit/language/tuples/tuple_value_literal.arche` |
| G2/G3 (tuple as a value type — func param/return, `.x`) | `tests/unit/language/tuples/tuple_value_func.arche` |
| G4/G5 (float tuple sub-column indexed read/write → i32) | `tests/unit/language/tuples/tuple_float_subcol_write.arche` |
| G6 (`as` self-binder without `eff`) | `tests/unit/language/each/map_self_binder.arche` |
| G7/G8 (columnar `system` ∘ nested `map`, reduce folds the system's column) | `tests/unit/language/systems/system_nested_map_reduce.arche` |

The N-ary tuple *column* model (which already works — `(x, y, z)`, `(a…g)`) is covered green by
`tests/unit/language/tuples/tuple_nary.arche`.

## Rough fix order (cheapest / most-isolated first)

1. **G6** (`as` without `eff`) — parser/semantic, small; mirrors the branching-off-the-kind fix.
2. **G4/G5** (tuple sub-column i32 mis-lower) — codegen store/load type resolution for a `field.field` base.
3. **G1/G2/G3** (tuples as value types + literals) — the biggest: a real 2-vector value type with fields,
   literals, arithmetic, and func params/returns.
4. **G7** (nested map in a columnar system) — parser/lowering + codegen composition.
5. **G8** (reduce folds a system's queried column) — the honest fold domain; needs G7 in place.
