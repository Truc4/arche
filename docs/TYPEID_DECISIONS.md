# Phase 3 — TypeId migration decision log

Living record of non-obvious decisions while migrating the type representation onto the interned
`TypeId` arena (`semantic/sem_types.{h,c}`) and retiring `TypeRef` + middle-of-compiler type strings.
Plan: `~/.claude/plans/i-need-an-honest-humming-kahn.md`. Mirrors `docs/AST_KILL_DECISIONS.md`.

## Status: Stages 0–4 DONE + green (478 LIT, semantic 33/33, codegen 8/8, lower 6/6, ASan clean). Stage 5 (deletions) remaining.

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
