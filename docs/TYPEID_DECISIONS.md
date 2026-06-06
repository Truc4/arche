# Phase 3 — TypeId migration decision log

Living record of non-obvious decisions while migrating the type representation onto the interned
`TypeId` arena (`semantic/sem_types.{h,c}`) and retiring `TypeRef` + middle-of-compiler type strings.
Plan: `~/.claude/plans/i-need-an-honest-humming-kahn.md`. Mirrors `docs/AST_KILL_DECISIONS.md`.

## Status: COMPLETE — Stages 0–5 DONE + green (478 LIT, semantic 33/33, codegen 8/8, lower 6/6, ASan 0 leaks). `TypeRef` struct DELETED; TypeId is the sole type identity in the middle of the compiler.

## Stage 0 — alias-tier encoding in the arena (DONE)
Extended `TYK_NOMINAL` payload to `{name, backing}`. A **transparent** tier-1 alias never makes a
nominal node — the *caller* interns it as its backing's id (so `int`==`i32` for free, no read-time
flag). A **distinct** tier-2 subtype interns by its own name with a `backing` TypeId
(`tyid_of_nominal_sub`). New: `tyid_of_nominal_sub`/`tyid_backing`/`tyid_usable_as` (one-way:
`meters` usable as `float`, not vice-versa). `tyid_equal` stays `a==b`. Unit tests in semantic_tests.c.

## Stage 1 — arena lifetime: TyCtx → SemanticContext (DONE)
`ctx->ty_arena` created in `make_context`, freed LAST in `semantic_context_free` (HirType borrows its
interned name strings through codegen). `sem_context_arena(ctx)` accessor. tycheck uses it (no longer
creates/frees its own). Exactly one `ty_arena_free` in the tree. ASan-verified (no leak/double-free).

## Stage 2 — DeclSummary carries TypeId (DONE)
Added `type_id`/`return_type_ids`/`static_type_id`/`const_*_id` beside the TypeRef fields. Moved
tycheck's `tyid_from_name`/`tyid_from_typeref` into semantic.c as the **shared** `sem_tyid_of_name`/
`sem_tyid_of_typeref` (+`sem_intern_view`) so a type interns to the SAME id everywhere. tycheck reads
the stored ids; its local copies deleted.
- **KEY decision — fill ids AFTER analysis, not in build_decl_table.** Type aliases register *during*
  analysis (`register_type_alias_tiered`), so filling in build_decl_table (pre-analysis) mis-resolves
  aliases → 30 LIT fails. Fixed: `sem_fill_decl_type_ids` runs right before `tycheck_run`, when the
  alias registry is final — matching the state tycheck saw before.
- **KEY decision — match the OLD tyid_from_typeref coverage (NAME + PROC/FUNC only).** The richer
  array/handle/tuple kinds are deliberately left `TYID_UNKNOWN` (fail-open) for byte-identical tycheck;
  interning them adds NEW checks → a separate follow-up with golden updates, not a migration side-effect.

## Stage 3 — SemModel TypeId channel (DONE)
Added `expr_type_id` channel + `sem_model_cap`. Filled in a **post-analysis** pass from the resolved
string channels (nominal-preferring, exactly the old `model_type_of` precedence) — so the id matches
what `tyid_from_name(model_type_of)` produced, for both tycheck and lowering. tycheck's INDEX/SLICE/
NAME/FIELD synth reads the channel; `model_type_of` deleted.
- **Deferred to Stage 5:** the group-overload-match + cast-`T(x)` string compares → `tyid_equal`.
  They run *mid-analysis* where the post-filled ids aren't available; cleaner to convert when the
  resolvers go TypeId-native.

## Stage 4 — lowering reads TypeId (DONE)
Switched `sem_tyid_of_name`'s tier-2 branch to carry the backing (`tyid_of_nominal_sub` — invisible to
tycheck, which is name-based, so still byte-identical). Added `map_type_id(arena, TypeId)` in lower.c
(the back-end twin of `map_type_str`): a tier-2 nominal lowers through its **backing**; a nominal's
spelling (width-int / char_array / handle / opaque / archetype) is parsed via `map_type_str` on the
interned name; prims map directly (`PRIM_STR`→CHAR_ARRAY, `PRIM_BOOL`→NAMED("bool"), to match the old
string path). `syntax_expr_type` now reads `expr_type_id`. Codegen goldens **byte-identical**.
- **Round-trip wrinkle (resolved):** the arena's `PRIM_INT` is width-less and `sem_tyid_of_name`
  nominalizes width-ints + collapses `str`/`char_array`→`PRIM_STR`. `map_type_id` re-derives width
  from nominal names and maps `PRIM_STR`→CHAR_ARRAY. Added `tyid_prim`/`tyid_nominal_name` accessors.
- **No `tyid_display`→string bridge** (plan confirmed): `map_type_id` decodes straight to HirType.

## `int`/`i32` (user directive)
`int` is NOT compiler sugar — it's `int :: alias i32` in core.arche. Zero `int`-special-casing was
added; it flows through the generic transparent-alias path (collapses to `i32`'s prim id). The
existing `int`-hardcodes (`map_type_str` "int"→INT32, literal default) remain at the string boundary
for now; Stage 5 removes the ones in the middle.

## Stage 5 — deletions (REMAINING)
resolve_name_type/field/base_chain/func_return → TypeId (deepest cut; `tyid_display` shim at genuine
string consumers); delete expr_type/expr_nominal channels + the fill + nominal_type_of_expr;
cst_build_type/type_ref_equal/owned_types/sem_view_type/analysis_own_type; DeclSummary TypeRef fields
(decl builder → `sem_intern_view`); group-match/cast → tyid_equal; shrink `syntax/type_ref.{h,c}` to
the six enums + SourceLoc, rename to `syntax/decl_enums.h`, update includes + Makefile.

## Stage 5 — IN PROGRESS (478 green at each checkpoint)

Done so far:
- **SemModel string channels DELETED.** `expr_type`/`expr_nominal` gone; `expr_type_id` computed at
  the analyze_expression writer (mid-analysis timing verified fine — top-level aliases register in
  pass 0 before bodies). LSP hover renders via `tyid_display` (+ `display_type` maps `str`→`char[]`).
- **Rich `sem_tyid_of_typeref` coverage** (array/shaped/handle/tuple/archetype) re-enabled; tycheck
  stays fail-open on those structural kinds (a `check()` guard) so it's byte-identical — encoding
  array/handle arg checks is a deliberate later follow-up. `char_array` kept a distinct nominal (not
  collapsed to PRIM_STR) for a clean round-trip.
- **`VariableInfo.type` + `FieldInfo.type` → TypeId.** ~36 structural-inspection sites converted via
  new accessors `tyid_handle_name`/`tyid_elem`/`tyid_tuple_*`/`tyid_prim`/`tyid_nominal_name` + the
  `sem_tyid_name(ctx,id)` stable-resolved-name helper. `type_is_byref_aggregate` takes `(arena,id)`.
- **KEY TRAP (cost ~100 test fails + a segfault):** DeclSummary `*_id` fields (params/out_params/
  return_type_ids/static_type_id/fields) are filled POST-analysis (for tycheck), so reading them
  DURING analysis gives UNKNOWN / NULL-deref. EVERY analysis-time type read must convert ON THE FLY
  from the still-present TypeRef: `sem_tyid_of_typeref(ctx, <the TypeRef>)`. Fixed in
  nominal_type_of_expr, analyze_drop_decl, the proc/func/sys param-add, static-register, handle
  validation, FieldInfo build, extern byref check.
- semantic-test golden: field-type API now returns canonical `float` (was raw `Float`).

REMAINING: DeclSummary TypeRef fields are still the analysis-time type SOURCE (via on-the-fly
sem_tyid_of_typeref). To delete them: store each param/field's TYPE NODE (SyntaxView) on the summary,
intern on the fly via sem_intern_view from the node, then delete the TypeRef fields +
cst_build_type/type_ref_equal/sem_view_type/owned_types; group-match/cast → tyid_equal; int-hardcodes;
shrink+rename type_ref.{h,c} → decl_enums.h. NOTE the alias-timing rule above for any new conversion.

## Stage 5 — further progress (478 green + ASan throughout)
- `type_ref_equal` DELETED (its one caller → `tyid_equal` on stored param ids).
- Moved `sem_fill_decl_type_ids` to run after pass-0 alias registration (before the archetype/body
  passes) — decl signatures only reference top-level aliases (registered in pass 0), so the ids are
  final there. This lets the archetype + body passes read the STORED decl `type_id`s directly.
- Converted the body/archetype-pass DeclSummary readers (proc/func/sys params, out-params, statics,
  FieldInfo build, handle validation, dup-param check, nominal return) from on-the-fly
  `sem_tyid_of_typeref(ctx, X.type)` to the STORED `X.type_id`. Analysis no longer reads DeclSummary
  TypeRefs except in `sem_fill_decl_type_ids` (which produces the ids).

### REMAINING obstacle (genuine, newly surfaced)
The DeclSummary TypeRefs now serve ONE purpose: they carry the MODULE-RENAMED type names
(`socket`→`net.socket`) applied by `sem_rename_decl_summary` (it calls `sem_rename_typeref` on the
param/return/field TypeRefs). `sem_fill_decl_type_ids` interns those renamed TypeRefs. The bare syntax
type-nodes do NOT have the module prefix, so interning straight from the node would lose it. So fully
deleting the TypeRef fields requires RE-HOMING the module-type rename onto the interning path (intern
bare → apply the recorded module prefix → intern the qualified name), OR storing the renamed type as a
string/TypeId on the summary at rename time. That is a real additional layer beyond "delete the
fields". `cst_build_type` (used by `sem_intern_view`) and the `type_ref.{h,c}`→`decl_enums.h` rename
follow once the fields are gone.

**State: TypeId is the type representation everywhere it matters (identity, comparison, tycheck,
lowering, VariableInfo/FieldInfo, SemModel). TypeRef survives ONLY as the per-decl build-time carrier
of module-renamed type names (decl_summary_from_node → sem_rename_decl_summary → sem_fill → TypeId) —
a transient builder, exactly as the AST was before its own final deletion.**

## Stage 5 — further deletions + the genuine wall (478 green throughout)
Deleted: the module type-rename (`sem_rename_typeref` + its calls — empirically proven unnecessary:
type identity is the interned bare name, both decl signature and use intern it). Converted most const
readers (check_const_literal, the alias-registry tuple/scalar branches, the deferred-value flag, the
tuple-group check) to the interned const `*_id`.

### GENUINE BLOCKER (root-cause needed before the const/callable subsystem can go TypeId-native)
Converting the callable-type-alias registry (`register_callable_type_alias`/`callable_type_alias_ref`)
from `TypeRef*` to `TypeId` broke 2 callback tests with `expected 'func(1)->(1)', got 'func(1)->(1)'`
— STRUCTURALLY IDENTICAL func types that `tyid_equal` reports as different ids. The same callable
interned via the alias path (interned at pass-0 registration) vs the direct-func path
(`tyid_of_callee`, built post-fill) produce different interned ids despite identical
`tyid_of_func([i32],[i32],is_proc=0)` inputs. Hash-consing should make them equal; it doesn't, which
points to a subtle arena/timing interaction I could not root-cause without leaving tests red. REVERTED
to keep `TypeRef` for the callable registry (478 green). This is the blocker: until that interning
divergence is understood, `const_type_value`/`const_decl_type` (read at the callable-alias branch +
the tuple-group func-return expansion `sem_type_deep_copy`) must stay `TypeRef`, so the struct stands.

### Net state
`TypeRef` survives as: (1) the decl-type build representation that `sem_fill_decl_type_ids` interns
into the per-decl `type_id`s (params/out_params/fields/returns/static — analysis reads the ids, only
`sem_fill` reads the TypeRefs; deletable once `sem_fill` is sourced from stored type NODES), and (2)
the const/callable-alias subsystem (blocked above). TypeId is the type identity everywhere it matters;
478 LIT + semantic 33 + codegen 8 + lower 6 + ASan all green.

## Stage 5 — `TypeRef` deleted (DONE)
All `DeclSummary`/`ParamSummary`/`FieldSummary` type fields are interned `TypeId`s
(`return_type_ids`, `static_type_id`, `const_type_value_id`, `const_decl_type_id`, `params[].type_id`,
`fields[].type_id`); the syntax `type_node` views are interned post-pass-0 by `sem_fill_decl_type_ids`.

- **`sem_intern_view` is now the sole type-node→TypeId builder** — it walks the `SN_TYPE_*` shapes
  directly (REF/ARRAY/SHAPED/HANDLE/TUPLE/PROC/FUNC), routing names through `sem_tyid_of_name`'s alias
  tiering. The transient `cst_build_type`→`sem_tyid_of_typeref` two-step is gone.
- **Analysis-time structural reads converted to `tyid_*` accessors**: the `BIND_STMT`
  inferred/nominal-type derivation, the multi-bind `MbTarget` (`type_id` not `TypeRef*`), the
  `#each_field` filter check (resolved-name vs `is_width_int_name`/`float`/`char` — `int` now resolves
  to `i32`), and the static array/scalar element typing all read TypeIds.
- **Deleted**: `cst_build_type`, `sem_tyid_of_typeref`, `analysis_own_type`, `sem_view_type`,
  `type_backing_name`, `sem_type_deep_copy`/`sem_pool_type_copy`, the `ctx->owned_types` pool,
  `semantic_type_from_view`; the `TypeRef` struct + `TypeKind` enum + `type_*_create`/`type_ref_free`
  from `syntax/type_ref.h`/`.c` (header keeps only the shared enums + `SourceLoc`; the `.c` is now an
  intentionally empty TU).
- **New owner for registry-borrowed strings**: the old `owned_types` TypeRef pool transitively owned
  the alias name/backing strings handed by pointer to the type-alias registry. Deleting it orphaned
  them (ASan leak). Replaced by a focused `ctx->owned_strs` string pool (`sem_own_str`) freed at
  teardown — the alias names/backings built on the fly during analysis route through it.
