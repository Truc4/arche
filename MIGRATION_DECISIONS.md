# Archetype-as-type migration — autonomous decisions log

Running log of design decisions made WITHOUT user input during the big-bang migration
(user is on a break and asked me to decide and track for later discussion). Each entry:
the decision, why, and how to revisit. Plan: `~/.claude/plans/alright-new-huge-change-ethereal-papert.md`.

## Locked surface syntax

1. **Type alias** — `name :: <type>`. A `::` decl with a *name* RHS (`count :: int`,
   `file :: opaque`) or a *type-form* RHS is a nominal type alias; a *literal* RHS
   (`MAX :: 100`) stays a value const. Consts are literal-only, so name/type RHS is
   unambiguously a type. No `type` keyword.

2. **Tuple group** — `pos (x, y) :: float` mints flat name-prefixed nominal types `pos_x`,
   `pos_y` of the **shared** type. *Decision (per user, supersedes the old per-field form):* the
   suffixes `(x, y)` are part of the *name*; the type follows `::`. Homogeneous only (one shared
   type) — heterogeneous fields are just separate component mints. The old
   `pos :: (x: int, y: int)` per-field syntax was **removed** (parser + formatter) and all decls
   migrated. Parsed by `parse_tuple_name_group`; internal rep stays `TYPE_TUPLE` (field_types =
   the shared type cloned per field), so the existing semantic expansion is unchanged.

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

## DONE (user request) — `let` keyword REMOVED; bindings are bare; for-loop de-hacked
There is no `let`. A binding is recognized by shape: `x := e`, `x: T [= e]`, `a, b := e`, and the
paren multi-bind `(x, y:, n: T) = e` (a trailing `:` marks a newly-declared target). A leftover
`let` is a clear parse error. Implementation:
- New `parse_simple_statement` parses binding/assignment/expression CONTENT (no terminator); the
  statement fallback consumes `;`, and the `for` header consumes `;`/`)`. `parse_binding_tail`
  no longer eats the terminator.
- **The for-loop's hand-rolled init/incr parsers (which hard-coded `let`) are GONE** — `for` now
  uses `parse_simple_statement` for both init and increment. One code path, no duplication.
- AST/CST node renamed `STMT_LET`/`LetStmt`/`let_stmt` → `STMT_BIND`/`BindStmt`/`bind_stmt`
  (`AST_STMT_LET`→`AST_STMT_BIND`) — `let` is not a concept anymore.
- Formatter emits bare bindings (`x := e`, `x: T = e`, `for (i := 0; …)`); round-trips + recompiles.
- Migrated ~177 `.arche` + the C-test string literals; the old `let x = e` (single-`=` decl) and
  `for (let i = 0; …)` forms became `x := e` / `for (i := 0; …)`. Suite: 215/215 lit + all C tests.
- `alloc_in_proc_error` now greps the generic parse error (the message is no longer "expression").

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

## DONE (user request) — REAL multi-value return (struct ABI); `out` keyword DELETED
The `out` parameter keyword and the `is_out` field are **gone** (cst/ast Parameter, parser,
lower, semantic name_is_out_param, codegen). Multi-return is now a **genuine multi-value return**
— no buffer-fill disguise:
- `func sum_diff(a, b) -> (int, int) { return a+b, a-b; }`, caller `s, d := sum_diff(10, 3)` →
  s=13 d=7. Verified end to end (`tests/unit/language/let/multi_return.arche`).
- **Codegen — aggregate ABI:** a function with `return_type_count > 1` returns an LLVM literal
  struct `{ T1, …, Tn }`. `return e1, …, en` packs the values with an `insertvalue` chain;
  the caller `%res = call {…} @f(args)` then `extractvalue`s each member into its bind target.
  The fallback safety `ret` for multi-return funcs uses `zeroinitializer` (0 isn't an aggregate).
- **Uniform list representation (Go-style):** there is **no single-return special case**.
  FuncDecl is `return_types[]`/`return_type_count` (single = count 1, no scalar field); ReturnStmt
  is `values[]`/`count` (single = count 1). Formatter: `count==1 → T` else `(T1, …, Tn)`; round-trips
  `return a, b` and `-> (int, int)` faithfully (verified).
- **Buffer-fill is a separate thing, NOT a return.** An array is reached by reference, so a
  buffer-filling function takes the buffer as an ordinary param and returns just the scalar count:
  `func read(fd, buf, size) -> int { … }`, caller `n := read(fd, buf, 256)` (buf filled in place).
  No `param_is_fill_buffer`, no leading-array-return trick — all removed.
- Migrated: core `read`/`net_recv` → `-> int`; csv `read_chunk` → `-> int`; out/buffer tests
  (out_param_basic, multibind_*, multi_bind*, let_multi_syntax, multi_bind_mixed, handle_out_param,
  csv_load_buffer) → single-return + in-place. multivalue_let_simple → real `pair() -> (int,int)`.
  New: multi_return.arche (genuine multi-scalar). Parser test: `out` keyword rejected.
- This supersedes the earlier "out RETAINED" (#11) AND the intermediate buffer-fill-multi-return cut.

## DONE (user request) — `move` REQUIRED for opaque→consume transfers
A named opaque binding handed to a `consume` parameter must be written `move x`; a bare name is a
compile error (`opaque value 'x' must be moved into consuming parameter …`). Opaque can't copy, so
the transfer is always visible. Plain (non-consume) params borrow — no `move` needed. The old
auto-consume-on-bare-name (the "hack" the user flagged) is removed; consumption now flows only
through `move` (UNARY_MOVE marks the binding consumed), `return`, or `insert`. `nominal_type_of_expr`
sees through `move` so distinctness still checks the moved value. Cascade: ~33 `.arche` files
(stdlib `csv.close`→`arche_fclose(move fd)`, every test close site) updated to `move`. Negative test:
`types/consume_requires_move`.

## DONE (user request) — by-value/no-side-effect model; `consume` RENAMED to `move`
The ownership model the user demanded: **a function has no side effects unless you `move`.**
- **By-value default:** a plain (non-`move`) array argument is COPIED at the call site
  (`llvm.memcpy` using the arg's known size) so the callee's mutations never leak. Externs (C
  ABI) and `move` args stay by-reference. Codegen: `codegen.c` EXPR_CALL, `arg_is_move[]` +
  the type-7/shaped copy. Verified: `ownership/copy_no_side_effect`.
- **`move` (call site)** = by-ref + the binding dies (use-after-move → error, for ANY type, via
  the existing `is_consumed` tracking). A moved value comes back only through a return value;
  reassignment/multi-bind revives the binding (`semantic.c` STMT_ASSIGN / STMT_MULTI_BIND clear
  `is_consumed`; multi-bind now analyzes the RHS *before* binding targets so `move x` refers to
  the existing buffer). Tests: `ownership/move_data_use_after`, `let/multi_bind`.
- **`consume` → `move` (parameter modifier).** `consume` was opaque-special and only ever
  *forced the caller to move* (no terminality, no codegen) — so it was renamed to `move` (one
  keyword, reserved `TOK_MOVE`; call-site `move` is no longer contextual). A `move` param forces
  the caller to write `move` (no silent copy, by-ref) for ANY type, and **may be returned** (the
  fill-buffer contract). Parses on `func` params now (was proc-only — a latent bug). Field
  `is_consume`→`is_move`; formatter emits `move `. Reserving `move` collided with `sys move` →
  renamed example/test systems to `integrate`. Tests: `ownership/move_param_fill`,
  `move_param_requires_move`; C: `test_move_param_modifier`, `test_lex_move_keyword`.
- **Codegen fix — indexing a returned/rebound buffer.** A `char[]` member of a multi-return was
  bound as a scalar int, so `b[i]` after `b, n := f(move b)` mis-typed (`i32` vs `ptr`). Now
  bound as type-6 (`i8*` byte view, value-is-pointer, no alloca) like a func returning `char[]`.
  Test: `ownership/index_rebound_buffer`. (Still open: indexing a *shaped* `char[N]` param inside
  a callee, and `move` on a *static* buffer — both pre-existing; the lib uses unbounded `char[]`
  + local buffers, which avoid both.)
- **Lib/FFI enforce good practice.** Raw libc/runtime externs (`read`, `net_recv`,
  `arche_csv_read_chunk`) stay plain (the FFI escape hatch — C ABI can't return the buffer).
  Safe move-enforcing arche wrappers added: `csv.read_chunk`, `read_into`, `recv_into` — each
  takes `move buf` and returns `(char[], int)`. (`recv_into`, not `recv`, to avoid clobbering
  libc `recv`.) Callers migrated to **local** buffers + `move` (csv tests: `static buf` →
  local; `socket_loopback`, `csv_load_buffer`).

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
- **Handle-field read `h.comp` — DELIBERATE NON-GOAL (per user).** Reading a component through a
  handle value (`h.hp`) is intentionally not in the language. Column access `Foo.comp[i]` is THE way
  to touch data; a handle is a lifetime/capacity token (produced by `insert`, stored, passed,
  null-checked, `delete`d with generation/stale-handle abort), not a read handle. The codegen path
  that would deref `h.comp` does not exist and there are **no plans to add it** — not adding a feature
  just because other languages have it. (A bare `h.comp` currently emits an invalid token rather than
  a diagnostic; if it ever needs guarding, reject it at semantic time, but it's not on the roadmap.)
- **Generation-checked handles** — handle is `i64` = slot (low 32) | generation (high 32); a slot's
  i32 gen bumps on `delete`, validated on use (mismatch → abort: stale/use-after-free/ABA, tested by
  `stale_handle_abort` / `handle_use_after_free` / `handle_use_after_reinsert`). At gen `0xFFFFFFFF` a
  delete aborts loudly instead of wrapping — `codegen.c` `gen_exhausted:`. Reaching it needs 2^32
  deletes (untestable live), so `codegen_tests.c: test_codegen_gen_exhaustion_abort` asserts the branch
  is emitted. DONE.
- **Multi-*scalar* return** (`-> (int, int)` via LLVM struct ABI) — **DONE** (insertvalue/extractvalue,
  uniform return-value lists, no single-return special case). See the multi-return DONE block above.
- **Repo-wide `make format`** still has the pre-existing RAM-explosion/semicolon bug — do not run it
  blanket. (Per-file `arche-fmt` is fine and round-trips all current constructs, verified.)
