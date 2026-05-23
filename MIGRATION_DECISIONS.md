# Archetype-as-type migration — autonomous decisions log

Running log of design decisions made WITHOUT user input during the big-bang migration
(user is on a break and asked me to decide and track for later discussion). Each entry:
the decision, why, and how to revisit. Plan: `~/.claude/plans/alright-new-huge-change-ethereal-papert.md`.

## Locked surface syntax

1. **Type alias** — `name :: <type>`. A `::` decl with a *name* RHS (`count :: int`,
   `file :: opaque`) or a *type-form* RHS is a nominal type alias; a *literal* RHS
   (`MAX :: 100`) stays a value const. Consts are literal-only, so name/type RHS is
   unambiguously a type. No `type` keyword.

2. **Tuple alias** — `pos :: (x: int, y: int)` mints flat name-prefixed nominal types
   `pos_x`, `pos_y`. *Decision:* used the **per-field** tuple form `(x: int, y: int)`
   (each field typed), not the grouped `(x, y: int)` sugar — grouped can be added later.

3. **Archetype components** — `arche Foo { a, b }`: bare type names, no accessors. The
   component's type name IS its access path (`field name = type name`). Multi-same-type
   handled by minting distinct types / tuples (per user).

4. **Capacity decl** — `static pool<Foo>(N);`. *Decision:* kept the `static` keyword and
   trailing `;`, just renamed `table`→`pool`. Reuses the existing table allocation codegen
   wholesale. (Plan left exact spelling open; this was lowest-risk.)

5. **insert** — `insert(Foo, v1, v2, …)`: the bare archetype name names the type and its
   (inferred) pool; values positional; returns a handle. *Decision:* kept positional values
   (vs an entity-literal) and reused codegen's existing bare-archetype-name path.

6. **Column access + whole-column ops RETAINED** — `Foo.comp[i]` and
   `Foo.pos = Foo.pos + Foo.vel` (vectorized) still work. *Decision:* the plan leaned on
   handle/entity access + `run` queries, but the existing SoA column model is load-bearing
   in many tests and codegen; keeping it avoids a full codegen rewrite. Revisit if we want
   to forbid archetype-level column access in favor of handle/query-only.

## Pending / to-decide as I reach them
- **`run { components } { body }` query semantics** — per-column vectorized (matches the
  existing SoA model + tuple `+=`) vs per-entity scalar. Leaning per-column. (systems tests)
- **`delete(h)` / handle representation** (P4).
- **handle/entity as components** — likely via alias `name :: handle<Foo>` (needs alias
  backing to accept handle types).
- **ownership** `move`/`consume` linear must-consume (P5).
- **`opaque<archetype>` phantom-tag revert** (coupled with extern_type test migration).

## Implementation notes (where things live)
- Aliases: `semantic.c` pass 0 of `semantic_analyze`; `resolve_type_alias`/`is_type_alias`;
  erased to backing in pass-3 `erase_type_aliases` (CST rewrite, opaque→`TYPE_OPAQUE`).
- Field-access type resolution resolves aliases (`resolve_expression_type` EXPR_FIELD).
- Distinctness: additive `VariableInfo.nominal_type` + `nominal_type_of_expr` + check in
  the extern call-arg loop.
- Tuple-alias: `ConstDecl.type_value` (TypeRef); parser `::` path; semantic pass-0 expansion;
  formatter emits `format_type` for `type_value`.

## Migrated tests (green under new model) — 207/207 lit green
- `types/alias_*` + `tuple_alias` (aliases, chains, distinctness, negative).
- `systems/*` (bare components + `static pool` + `insert(Foo,…)` + `sys`/`run` + column access).
- `tuples/*` (8) — FLAT model: `pos :: (x,y:float)` → `pos_x`/`pos_y`, flat access, per-component ops.
- ~58 files storage-migrated by script (`static table`→`static pool`, `insert(table<X>,…)`→`insert(X,…)`).
- `extern_type/*` (8) — nominal `resource :: opaque`, `consume`, distinctness; pooled via `arche {cell}`+`static pool`.
- `opaque/*` — migrated to nominal; 2 obsolete `opaque<X>`-rule tests DELETED (behavior removed).
- `formatter/opaque_domain` — rewritten to nominal.

## Additional decisions
7. **`is_consumable_ref` resolves aliases** — `consume r: resource` (resource::opaque) is allowed.
8. **redefinition-must-agree** — `file :: opaque` twice (same backing) is fine; different backing errors
   (`"redefined with a different backing"`). Lets core + a test both declare `file :: opaque`.
9. **`sys`/`run name` RETAINED** (not yet `run { comps } { body }` query) — query-driven run deferred.
10. **`delete(table<X>, h)` RETAINED** as interim (works via the `table<X>` value-ref) — `delete(h)` deferred.
11. **`out` params RETAINED** (handle_out_param) — multi-return desugaring deferred.
12. Removed 2 obsolete `opaque<X>` phantom-tag rule tests (no end-product equivalent).

## RESOLVED — opaque-i64 codegen + stdlib migration DONE (207/207 green)
The opaque i32/i64 inconsistency is fixed: opaque is now consistently **i64** across var-load
(`codegen.c` ~`:1152`), binary-op width + `emit_int_convert` (opaque/handle → i64), the call-arg
type (EXPR_CALL + multi-bind/out paths), and the if-condition (opaque `if (fd)` → `icmp ne i64 …, 0`,
a non-null cell check). **`core.arche` + `core/csv.arche` are now nominal `file :: opaque` /
`socket :: opaque`** (no `extern File(64)`/`handle<File>`); the C runtime was unchanged (FILE* is the
i64 cell). The extern-type pool is no longer used by the stdlib. Below is the original (now-resolved)
blocker writeup for history.

## (HISTORY) BLOCKER — stdlib (core.arche) nominal migration reverted
Migrating `core/core.arche` + `core/csv.arche` from `extern File(64)`/`handle<File>` to nominal
`file :: opaque` hit a **pervasive opaque i32/i64 codegen inconsistency**: an opaque value is alloca'd
as i64 but *loaded/compared/passed as i32* (default path in codegen.c — the var-load ~`:1147`, the
call-arg type ~`:2510`, plus comparisons like `r == 0` and `out`/return paths). For opaque *locals*
LLVM tolerates the i64-alloca/i32-load pun, so by-value tests pass; but an opaque **function param**
(e.g. csv's `close(fd: file)` → `arche_fclose(fd)`) emits `call @arche_fclose(i32 %arg0)` against an
`i64`-declared fn → opt error. Fixing the var-load + call-arg to i64 cascades into the compare/return/
out paths (broke `null_return`, `handle_out_param`). **Proper fix = make opaque consistently i64 across
load / store / compare / call-arg / return** (a focused codegen audit). Reverted core/csv to the working
extern-File model to keep 207 green; this is the main remaining end-product gap.

## RESOLVED — linear must-consume (P5) DONE (210/210 green)
An opaque LOCAL must be consumed before scope end (`pop_scope` check via `var_is_opaque`).
- Only `let`-bound opaque locals are checked; params are borrowed (exempt via `is_param`).
- Consumption = `move` into any call, a `consume`-param call (extern OR non-extern), `return r`,
  or `insert(Foo, …, r, …)`. (return/insert marking is opaque-only — data/handles copy.)
- **Decision:** the consume-marking now runs for non-extern callees too (moved it before the
  `if (!is_extern) break;` distinctness gate) — the canonical close-fn is a plain
  `proc close(consume r: file)`. Relaxed the old "`consume` only on extern-type params" rule to
  "`consume` only on opaque-backed params" for non-extern proc/func.
- **Decision:** stdlib `csv.close` is now `proc close(consume fd: file)` (terminal sink). The
  `null_return`/`invalid_path` tests close even null cells — both `arche_fclose` and
  `resource_close` are null-safe in C, so retiring a null opaque is a no-op.
- Tests: `types/must_consume_leak` (negative), `types/must_consume_return` (return = consume),
  `types/move_basic`/`move_use_after` (move). ~13 leaking tests gained explicit closes.

## RESOLVED — set-based archetype identity + dup-component (P1 finish) DONE (212/212 green)
- `compute_shape_signature` now **sorts** its per-field parts → order-independent: `{hp,mp}` and
  `{mp,hp}` share one shape signature. Codegen is keyed by archetype *name* + per-decl column
  order, so the sort only affects shape dedup, not memory layout.
- **Dup-component error**: a component type appearing twice in one archetype (`arche Foo {hp,hp}`)
  is rejected (`analyze_archetype_decl`). Test: `types/dup_component` (negative).
- Set-identity is observable via the existing "Shape already allocated" guard: two reordered
  same-set archetypes can't both declare a pool. Test: `types/set_identity` (negative — proves dedup).
- **KNOWN GAP (deferred, documented):** inserting/accessing a shape via a *different* same-set
  archetype name (e.g. `insert(B,…)`/`b.hp` where B shares A's shape and only A has a pool) is NOT
  wired in codegen (codegen is name-based; B has no `@B$pool`, emits raw token). Niche; the main
  set-identity consumer (`run {comps}` queries) is CANCELLED, so nothing needs cross-name sharing.

## CANCELLED (per user) — `run {comps}` query
User: "leave run alone, run sys; that's all it will ever be until I say otherwise." The
query-driven `run { pos, vel } { … }` form is OFF. `sys`/`run name` stays as-is, permanently.

## DEFERRED (deliberate, documented) — delete the dead extern-type code path
The extern-type path (`parse_extern_type_decl`, `DECL_EXTERN_TYPE`/`ExternTypeDecl`, extern-table
codegen, `runtime/handles.c __arche_slot_*`, `semantic_has_extern_type`, the `opaque<archetype>`
phantom-tag + `validate_opaque_tag`) is **dead for all `.arche`** (stdlib + every test migrated to
nominal opaque). But it is **69 references across 14 files**, and 4 of those are the C unit-test
files (`tests/unit/compiler/{parser,semantic,codegen,handle_runtime}_tests.c`) which actively
exercise `extern Window(8)` and are GREEN. **Decision (autonomous):** leave it. Ripping out 69
refs + rewriting 4 green C-test files is a large mechanical change with ZERO behavioral benefit and
real risk to the all-green suite — against the "done = everything green" bar. The product goal (one
type system for all real code) is already met. Best done later as its own focused pass with explicit
greenlight. NOT a blocker for the migration being functionally complete.

## DONE (this session, all green)
- Linear must-consume (P5) — see RESOLVED block above.
- Set-based archetype identity + dup-component (P1 finish) — see RESOLVED block above.
- README rewritten to the nominal-types model: new "Types and declarations (`::`)" section;
  Archetypes (set-of-nominal-types, set identity, dup error); Memory Model (`static pool<Foo>(N)`,
  no `alloc`); flat tuple access (`pos_x`); `sys step` examples; ownership/`move`/must-consume
  subsection in foreign-resources.

## Still TODO (end-product gaps remaining — all DOCUMENTED; suite is 212/212 green)
DONE: P1 aliases/distinctness/redef-agree; P2 tuples flat; P3 pool/insert/`delete(h)` (handle→archetype
inference, propagated through `let alias := h`)/access; P4 nominal opaque foreign + stdlib + opaque-i64;
**P5 `move` keyword** (contextual `UNARY_MOVE`: transparent value, marks operand consumed → use-after-move
errors; renamed `sys move`→`sys step` to free the word) + `consume`; README foreign-types section.
Remaining (each distinct; suite green so do one at a time):
- **Linear must-consume** (the strict half of P5): an opaque LOCAL must be consumed before scope end.
  NUANCED — must (a) apply only to `let`-bound opaque locals, NOT params (a plain opaque param is borrowed,
  e.g. `resource_get(r)` reads without consuming), (b) count `return`/`insert`/`move`/`consume`-call as
  consumption. A naive pop_scope check would wrongly flag params + returned values. Will (intentionally)
  break non-consuming tests (`null_return` opens r1–r9 without closing) → needs paired test rewrites.
- **`run { comps } { body }` query** (replace `sys`/`run name`) — real feature: match ALL pools whose
  component set ⊇ {comps} (multi-pool iteration), reusing the sys/column codegen per matching archetype.
- **`delete(h)`** (replace interim `delete(table<X>, h)`) — needs handle→archetype inference; codegen
  `ValueInfo.handle_archetype` already tracks it, so `delete(h)` can emit `arche_delete_<X>`.
- **Identity set/dedup** — order-independent `compute_shape_signature` (`semantic.c:~106`) + dup-component
  -name error. RISK: collapsing two reordered archetypes to one ArchetypeInfo while codegen lays columns
  by per-decl order → mismatch. Needs care (key identity by sorted set but keep per-decl column order).
- **Delete the extern-type PATH** — `parse_extern_type_decl`, `ExternTypeDecl`/`DECL_EXTERN_TYPE`, the
  extern-table codegen, `runtime/handles.c __arche_slot_*`, `semantic_has_extern_type`, and the
  consume-slot-marshal in codegen. Now DEAD for all `.arche` (stdlib migrated), but still exercised by C
  unit tests (`tests/unit/compiler/{codegen,parser}_tests.c` use `extern Window(8)`), which must be removed
  with it. Also revert the `opaque<archetype>` phantom-tag code (parser `:302`, `validate_opaque_tag`,
  type_ref_equal, cst `format_type` `opaque<%s>`) — likewise dead.
- **README**: foreign-types section rewritten ✓; still stale — Archetypes section (old `field: type` framing),
  Memory Model (`table`→`pool`), `handle(Name)` paren syntax mentions, Out Parameters section.
