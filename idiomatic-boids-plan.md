# Make the idiomatic boids compile — fix the five language gaps (G1–G8)

## Context

The boids demo written the honest, idiomatic way — `pos(x, y)`/`vel(x, y)` 2-vectors, and the self-join as a
`system` querying whole COLUMNS wrapping a `map` querying per-element SCALARS with a `reduce` over the
system's column — does not compile. The five gap-clusters are logged in `docs/wip/idiomatic-boids-gaps.md`
and each has a RED test (no XFAIL) that flips green when fixed. This plan implements all of them, in
dependency order, so the demo becomes both correct and idiomatic. Every stage: fix → its red test goes green →
full suite stays green → any new bug found gets its own regression test first (test-captures-every-bug).

Red tests (the acceptance checks):
`tuples/tuple_float_subcol_write.arche` (G4/G5), `each/map_self_binder.arche` (G6),
`systems/system_nested_map_reduce.arche` (G7/G8), `tuples/tuple_value_literal.arche` (G1),
`tuples/tuple_value_func.arche` (G2/G3). Green already: `tuples/tuple_nary.arche` (N-ary column tuples).

## Stage 1 — G4/G5: float tuple sub-column indexed read/write (codegen)

The indexed store/load type defaults to `i32` for a nested `field.field` base (`codegen/codegen.c:~9732`,
`scalar_type = "i32"`; the type fallback at `:9745-9764` only resolves a plain-NAME base, so `Pool.tup.sub`
misses). Correct for `int`, wrong for `float`.

- **Fix**: in the indexed-store (`:9683+`) and matching indexed-load paths, resolve the scalar type from the
  tuple sub-column when the base is `Pool.tuple.sub` — walk `HIR_EXPR_FIELD` whose base is itself a
  `HIR_EXPR_FIELD` (the tuple group) to the archetype's flattened `tuple_sub` column type (`float`/`i32`),
  instead of defaulting to i32. Reuse the archetype field-type lookup already used at `:9734-9788`.
- **Verify**: `tuple_float_subcol_write.arche` → green (`x=0 1 2`); the int case (`sys_tuple_subcol_*`) stays
  green.

## Stage 2 — G6: `as me` per-element column binder, decoupled from `eff` (parser + sema + codegen)

Today `parser.c:1635-1639` rejects `as` on a pure `map`, and `as w` binds a **delete-handle** (`row_var`,
`hir/hir.h:186`; codegen `codegen_each_fan` `:12273`), NOT a column accessor — there is no `me.field` column
lvalue/rvalue path. The self-join needs `me.pos` = this element's column value (to disambiguate self from the
enclosing system's same-named column).

- **Parser**: drop the `map_has_bind` requires-eff error (`parser.c:1636-1638`); a pure `map (query {…} as me)`
  parses to `SN_MAP_EXPR` carrying the bind.
- **Semantic/codegen**: introduce a **row binder** distinct from the handle — `as me` on a per-element kernel
  binds `me` such that `me.<col>` resolves to the kernel's bound column at the current row (a per-row scalar
  read AND an assignable lvalue for a declared write). In codegen this is the same per-row column access the
  bare bound name already lowers to (the `each_fan`/map per-row column at `%row`); `me.col` just routes the
  field access to that binding. Keep the effectful `as w`/`delete(w)` handle form intact (it's the `eff`
  path); the new binder is the non-eff column form.
- **Verify**: `map_self_binder.arche` → green (`v=2 4 6`). (This is what lets the flock nested map write
  `me.nvel` and read `me.pos` against the system's bare `pos` column.)

## Stage 3 — G7: a pure `map` statement is an inline fan (sema + lower + codegen)

The parser already shares `parse_block_body` for both `system` forms; the `;` error is generic
(`parser.c:2619`) — a nested `map`'s `}` needs a trailing `;` (fix in the test). The real gap: only
`SN_EACH_EXPR` (`map … eff`) is routed to the inline-fan path; a pure `map` (`SN_MAP_EXPR`) statement falls to
a discarded expression.

- **Semantic** (`semantic.c:3448-3454`) and **lower** (`lower.c:1159-1183`): route `SN_MAP_EXPR` (not only
  `SN_EACH_EXPR`) through the inline-each path (`analyze_inline_each` / `HIR_STMT_EACH` via
  `lower_each_payload`), so a nested pure `map` becomes a `HIR_STMT_EACH` fan.
- **Codegen**: `HIR_STMT_EACH` already fans via `codegen_each_fan` (`codegen.c:10446`). Confirm it nests
  correctly inside a columnar `system(query)` body (the outer `in_columnar_system` binds whole columns
  `:12043-12112`; the inner fan drives per-row over the same/other pool). The inner fan reads the outer
  system's bound columns (type-4) as whole columns — which Stage 4 folds.
- **Verify**: the nested-`map`-in-`system` structure parses + runs (drives the `system_nested_map_reduce.arche`
  test toward green; completed by Stage 4).

## Stage 4 — G8: `reduce` folds an enclosing system's bound column (codegen)

`emit_fold_expr`/`find_fold_pool_field`/`is_pool_col_field` (`codegen.c:3073-3160`) discover the fold domain
ONLY from a `Pool.col` FIELD ref. Inside the nested map, the domain is a **bare name** `v` resolving to the
enclosing columnar system's **type-4 column pointer** (`codegen.c:12093`, `ValueInfo.type==4` with
`arch_name`/`field_type`).

- **Fix**: extend the fold-domain discovery to also accept a bare `HIR_EXPR_NAME` whose scope binding is a
  type-4 column pointer; derive the fold base + count from that binding via the `in_columnar_system`
  pool-global path (`emit_query_pool_ptr`, `codegen.c:9624-9629 / 12077`) instead of `emit_collective_column`
  on a FIELD. Key `ctx->fold_pool`/`fold_index` (`:3148`) off the bound column's arch so its per-row index is
  the fold counter, while the enclosing map's self (`me.*`) stays at the outer row.
- **Verify**: `system_nested_map_reduce.arche` → green (`o=6 6 6`), with the `;` fixed and (optionally) `as me`
  once Stage 2 lands.

## Stage 5 — G1–G3: tuple VALUE types (parser + sema + codegen) — the big one

arche tuples are column-group macros (`lower.c` `CstTupleGroup`, flattened to `pos_x`/`pos_y`); the real
`TYK_TUPLE` type-with-fields exists (`sem_types.c:236` + `tyid_tuple_field_name/type`) but is never a codegen
value. Reuse the **multi-return aggregate** machinery (LLVM `{T,…}` by value: `llvm_return_list_type`
`codegen.c:765`, `insertvalue` pack `:10567`, `extractvalue` unpack `:9090`).

- **Parser**: promote the primary paren rule (`parser.c:1919`) to emit `SN_TUPLE_LIT` when it holds >1 element
  (the entity-field path at `:1811` already does this) → a tuple **value** literal `(a, b)`; and accept a
  tuple value on a `NAME(x, y) :: (a, b)` const RHS (the named-vector const `CENTER(X, Y) :: (320.0, 240.0)`).
- **Semantic**: resolve a bare tuple-group name in **type position** (func param/return, local) to its stored
  `const_type_value_id` `TYK_TUPLE` (`semantic.c:9997-10031`; hook `semantic_resolve_type_alias` `:10967`)
  instead of a bare `TYK_NOMINAL`. `.x`/`.y` field access already works for `TYK_TUPLE` (the checker exempts
  it, `:1580`; index via the accessors). Type tuple arithmetic (`a - b`, `v * s`, `v / s`) as element-wise
  over the tuple's fields; type a `SN_TUPLE_LIT` as `TYK_TUPLE`.
- **Codegen**: lower `TYK_TUPLE` as an LLVM aggregate `{T × N}` passed by value. Tuple literal → `insertvalue`
  chain; `.x` → `extractvalue`; arithmetic → per-lane extract/op/insert; extend `emit_func_params`
  (`codegen.c:11503`) to pass a tuple aggregate by value; returns reuse the multi-return aggregate return.
  Keep N-ary (mirror the column model — 2 is just a case).
- **Verify**: `tuple_value_literal.arche` (`c=320 240`) + `tuple_value_func.arche` (`s=4 6`) → green;
  `tuple_nary.arche` + all existing tuple-column tests stay green.

## Stage 6 — idiomatic boids compiles, runs, flocks

With G1–G8 fixed, `arche-rpg/game/game.arche` + `app/app.arche` (already written in the idiomatic form)
build. Fix any residual (e.g. tuple whole-column ops in the `apply` system, tuple sub-column seed writes now
that G4 is fixed). Headless-verify flocking (in-bounds stable, clustering, motion; a GIF), CPU placement
(a `reduce` body → not GPU-eligible → CPU, display-safe).

## Critical files

- `codegen/codegen.c` — G4 store/load type (`:9683`,`:9732-9788`); G7 fan (`:10446`); G8 fold-over-bound-column
  (`emit_fold_expr` `:3073-3160`, `in_columnar_system` `:9624`,`:12043-12112`); G1–G3 tuple aggregate (reuse
  `:765`,`:10567`,`:9090`; `emit_func_params` `:11503`).
- `semantic/semantic.c` — G6 `me.field` resolution; G7 inline-`SN_MAP_EXPR` (`:3448`); G1–G3 tuple type in
  type position (`:9997`,`:10967`), field access (`:1580`), arithmetic.
- `parser/parser.c` — G6 drop the `as`-needs-`eff` gate (`:1635`); G1 tuple literal (`:1919`,`:1811`) + const RHS.
- `lower/lower.c` — G7 route `SN_MAP_EXPR` to `HIR_STMT_EACH` (`:1159`); tuple-group interplay (`:100`,`:1622`,`:2310`).
- `arche-rpg/game/game.arche`, `app/app.arche` — the demo (already idiomatic; compiles after the fixes).
- Reuse: multi-return aggregate (`{T,…}` insert/extract) for value tuples; `codegen_each_fan` for the nested
  fan; the existing `TYK_TUPLE` accessors (`sem_types.c:236+`).

## Verification

- **Per stage**: the stage's RED test flips green; `lit tests/ extras/` shows the remaining gap tests still red
  (expected) and NOTHING else newly red; `make test-codegen-unit test-semantic test-lower` + `verify-fmt`
  green. Any bug uncovered mid-implementation → a new regression test first.
- **End state**: all 5 gap tests green (0 red), full suite 100%, `tuple_nary` + existing tuple tests green,
  and the idiomatic boids builds headless and flocks (GIF).

## Non-goals / follow-ons

- Kernel/reduce **fusion** (the flock does 7 unfused O(N) scans + `near` ×5 — a ~5–7× redundancy) — a perf
  optimization, separate.
- GPU backend for the pool `reduce` (scales boids past CPU O(N²)) — separate.
- Vector builtins (`length`, `min`/`max`, `dot`) beyond `sqrt`+`select` — additive once value tuples exist.
