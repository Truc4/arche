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
11. ~~`out` params RETAINED~~ — **SUPERSEDED: `out` deleted, replaced by multi-return** (see DONE block below).
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
- **RESOLVED — aliases share one pool (shape-keyed codegen).** Corrected model (per user): one
  shape = one pool; archetype *names* are aliases for the shape, and every alias resolves to that
  one pool/struct. Codegen now canonicalizes: `find_archetype_decl` returns the first-declared decl
  of the same shape; `codegen_archetype_decl` emits the struct + `@arche_insert/_delete/_dealloc` +
  global ONCE per shape (skips non-canonical aliases); `get_arch_static_capacity/count`, the
  archetype-name→pointer path, alloc-init, and insert/delete/dealloc call sites all canonicalize the
  name. So `insert(B,…)` lands in `pool<A>`'s storage; positional insert uses the shape's canonical
  (first-declared) component order. Tests: `types/alias_shared_pool` (sharing), `types/set_identity`
  (one-shape-one-pool double-alloc guard). The "shape already allocated" guard is CORRECT and stays.

## CANCELLED (per user) — `run {comps}` query
User: "leave run alone, run sys; that's all it will ever be until I say otherwise." The
query-driven `run { pos, vel } { … }` form is OFF. `sys`/`run name` stays as-is, permanently.

## DONE (user greenlit "delete it all then make it green") — extern-type path removed
The entire extern-type path is **gone**: `parse_extern_type_decl`, `DECL_EXTERN_TYPE`/`ExternTypeDecl`
(cst.h/cst.c), the slot-marshal + slot-pool codegen (`codegen_emit_extern_types`, the arg/return
`__arche_slot_*` blocks), `runtime/handles.{c,h}` (deleted), `semantic_has_extern_type` +
`semantic_extern_type_*` + `ExternTypeEntry` + `validate_opaque_tag`, the `opaque<archetype>`
phantom-tag (`TypeRef.data.opaque`), and the `extern_handle_target[_ast]` shims. Makefile + main.c
link line scrubbed of `handles.o`. The C unit tests that exercised `extern Window(8)`
(`parser/semantic/codegen/handle_runtime_tests.c`) were rewritten to the nominal `:: opaque` model or
removed; `handle_runtime_tests.c` deleted. Suite: 213/213 lit + all C tests green.

## DONE (user request) — archetype grammar: components are TYPES, single-colon `:` removed
"foo : bar means nothing." The archetype body now accepts only **type references** (`arche Foo { a, b }`)
and **inline definitions** `name :: type` (≡ a top-level `name :: type` + include — mints the nominal
type globally, registered in semantic pass-0, redefinition-must-agree). The old accessor `name: type`
is a **parse error** ("archetype components are types …"). Tuple/array components stay column-only (no
nominal alias). Migrated ~86 `.arche` files + 4 C-test files (`:` → `::` inside archetype bodies only).
Formatter emits bare for refs, `name :: type` for inline defs (round-trips + recompiles). Negative test:
`types/arche_colon_rejected`.

**Crash/leak fixed (the user's "it leaked again"):** a malformed archetype body made the global
`synchronize` skip past the closing `}` to the next decl keyword and park there without advancing, so
`parse_archetype_decl`'s field loop **spun forever allocating leading-trivia each pass → RAM blowup →
OOM core dump**. Rejecting single-colon newly triggered it. Fix: the field loop now **breaks** on a
malformed component (the missing `}` is reported after) instead of `synchronize`+`continue`. Bad input
no longer hangs/leaks.

## DONE (user request) — `out` keyword DELETED, replaced by multi-return signatures
The `out` parameter keyword and the `is_out` field are **gone** (cst/ast Parameter, parser,
lower, semantic name_is_out_param, codegen). A buffer-filling function now takes the buffer as an
ordinary param and **returns it**: `func read_chunk(fd, buf, size) -> (char[], int) { … return buf, n; }`,
caller `let buf, n := read_chunk(fd, buf, 65536)`. Surface = real multi-return:
- Parser: `-> (T1, …, Tn)` return-type list; `return e1, …, en` value list; `out` is now a parse error
  (helpful message). FuncDecl gains `return_types[]`/`return_type_count` (return_type aliases the
  final/scalar); ReturnStmt gains `values[]`/`value_count` (value aliases the last) — kept so the
  **formatter round-trips `return buf, n` faithfully** (verified) rather than dropping tokens.
- Codegen UNCHANGED in spirit: the function still physically returns only the scalar; the leading
  array returns are caller-passed buffers filled in place. The old `is_out` per-param flag is replaced
  by `param_is_fill_buffer(func, i)` derived from the signature (first n−1 array params are buffers).
- Migrated: core `read`/`net_recv`, csv `read_chunk`, and the out tests (out_param_basic, multibind_*,
  multi_bind*, let_multi_syntax, handle_out_param). New parser test: `out` keyword rejected.
- **Decision/limit:** only the (buffers…, scalar) shape is wired (every real use). A genuine
  multi-*scalar* return (e.g. `-> (int, int)` via struct return) is a clean future extension; nothing
  needs it yet. This supersedes the earlier "out RETAINED" decision (#11).

## DONE (user request) — `move` REQUIRED for opaque→consume transfers
A named opaque binding handed to a `consume` parameter must be written `move x`; a bare name is a
compile error (`opaque value 'x' must be moved into consuming parameter …`). Opaque can't copy, so
the transfer is always visible. Plain (non-consume) params borrow — no `move` needed. The old
auto-consume-on-bare-name (the "hack" the user flagged) is removed; consumption now flows only
through `move` (UNARY_MOVE marks the binding consumed), `return`, or `insert`. `nominal_type_of_expr`
sees through `move` so distinctness still checks the moved value. Cascade: ~33 `.arche` files
(stdlib `csv.close`→`arche_fclose(move fd)`, every test close site) updated to `move`. Negative test:
`types/consume_requires_move`.

## DONE (this session, all green)
- Linear must-consume (P5) — see RESOLVED block above.
- Set-based archetype identity + dup-component (P1 finish) — see RESOLVED block above.
- README rewritten to the nominal-types model: new "Types and declarations (`::`)" section;
  Archetypes (set-of-nominal-types, set identity, dup error); Memory Model (`static pool<Foo>(N)`,
  no `alloc`); flat tuple access (`pos_x`); `sys step` examples; ownership/`move`/must-consume
  subsection in foreign-resources.

## Status — migration COMPLETE, suite 213/213 lit + all C unit tests green
ALL planned work is done or explicitly cancelled:
- P1 aliases/distinctness/redef-agree + **set-based identity** (sorted `compute_shape_signature`) +
  **dup-component error** — DONE.
- P2 tuples flat — DONE.
- P3 `pool`/`insert`/`delete(h)`/access — DONE. (`run`-query CANCELLED by user; `sys`/`run name` stays.)
- P4 nominal opaque foreign + stdlib + opaque-i64 + **extern-type PATH fully deleted** — DONE.
- P5 `move`/`consume` + **linear must-consume** + **`move` REQUIRED for opaque→consume** — DONE.
- P6 stdlib migrated + README rewritten to the nominal model — DONE.

### Remaining KNOWN GAPS (deliberate, documented; not blockers)
- **`run { comps }` query** — CANCELLED by user ("run sys; that's all it will ever be").
- **Handle-field *read* (`h.comp`) is unimplemented** — reading a component through a handle value
  (e.g. `let a := insert(A,…); printf(a.hp)`) emits the field name as a raw token → invalid IR.
  PRE-EXISTING (affects even non-aliased archetypes; uncovered while testing A4), NOT caused by the
  migration. No test/real code reads `h.comp` today (column access `A.hp[i]` is the supported path).
  Wire it (handle → pool slot → column load) when handle-centric access is wanted.
- **Formatter** emits inline components as `name: type` (round-trips, same signature) rather than bare
  `name`; `static pool` and `move`/`consume`/`opaque` all round-trip correctly. (Repo-wide `make format`
  still has the separate RAM-explosion/semicolon bug — do not run it blanket.)
