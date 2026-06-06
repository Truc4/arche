# AST-kill (Syntax Tree Phase 2) — autonomous decision log

Living record of non-obvious decisions made while executing the approved plan
`i-need-an-honest-humming-kahn.md` (Part A hardening + Part B step C: delete the parallel AST).
Each entry: what was decided, why, and the validation that backs it.

## C1 — tree-driven channel population (DONE)

The tree-qualify pass in `build_decl_table` (`sem_tree_qualify_node`/`_walk`) now produces the
`callee_name`/`ref_name` SemModel channels **and** the `module has no member` diagnostic directly
from the SyntaxNode tree. The legacy AST qualify pass (`sem_qualify_expr/_stmt/_decl` +
`lvalue_leftmost_name`) is **deleted**.

- **Root-caused the 52-fail coverage gap** (csv/io/alloc i32-vs-i64): not a *missing-node* problem.
  Every gap node *was* visited. The bug was **base selection** in `sem_tree_qualify_node`: a
  *qualified* call `mod.f(...)` (`SN_CALL_EXPR` with `SN_FIELD_NAME` children) keeps its base name in
  the `TOK_IDENT` token, **not** in `SN_CALLEE_NAME` — exactly as `cst_build_expr`'s `SN_CALL_EXPR`
  case reads it (`sv_token(e, TOK_IDENT)` at the qualified branch vs `SN_CALLEE_NAME` at the bare
  branch). The tree-qualify always read `SN_CALLEE_NAME` (empty for qualified calls) → early-returned
  → no channel for `os.syscall`/`os.write`/`os.read`, so their out-param i64 typing was lost.
  **Fix:** `base = (is_call && !has_fields) ? SN_CALLEE_NAME : TOK_IDENT` — mirrors the AST builder.
- **Diagnostic parity:** re-added the precise `module 'm' has no member 'x'` emission to the
  tree-qualify (`sem_base_is_module` + `sem_emit_module_no_member`), replacing the AST pass's copy.
- **Validation:** with the AST qualify pass fully disabled, then deleted, the suite is **478/478**
  and `make test-asan` is **31/31 + 6/6**, warning-clean, format-clean.

## C4 — delete the parallel AST (IN PROGRESS)

**Linchpin verified:** `build_decl_table` reads only `{d->sn, d->sn_src, is_datasheet,
from_device_impl, is_requirement}` from each `Decl`; it builds the `DeclSummary` from the tree node
(`decl_summary_from_node`) + the rnop registry. The full-Decl reader `decl_summary_from(d)` is a
safety net for an un-ported kind. **Instrumented across all 541 corpus files: the safety net fires
0 times.** So every heavy `Decl` (params/body/types/name built by the `cst_build_*` family) exists
solely to be queried for `{node, src, flags}` — it is deletable.

**Provenance flags are loader-context, not tree-derivable** (`is_datasheet` = `.ds.arche` module
file; `from_device_impl` = device module & not datasheet; `is_requirement` = datasheet STATIC). They
must be carried per decl-node, so the replacement is a lightweight `DeclRef {const SyntaxNode* node;
const char* src; flags}` list, populated by the loader where it already computes those flags.

**Decision — staging (do NOT big-bang; plan flags the loader as the highest-risk change):**
- **C4.1** build the `DeclRef` node-list in `cst_to_program`/`sem_add_module_decl`; switch
  `build_decl_table` to iterate it — *while still building the AST* — to prove the node-list is
  sufficient with zero deletions. Validate 478 + ASan.
- **C4.2** gut `cst_build_decl` to emit only `{sn, src, flags}`; delete `cst_build_proc/func/
  archetype/enum/param/body/body_split/expr/stmt/decl_inner`, the `decl_summary_from` safety net, and
  `sem_rename_decl` (Decl name mutation is dead — only `sem_record_rnop` is needed). Validate.
- **C4.3** *(separable blocker, task #12)* rewrite `parser_tests.c` (~116 assertions on
  `Decl`/`Expression`/`Statement` via `cst_to_program_from_source`) onto the `SyntaxView` tree. This
  is the gate for struct deletion — the parser's unit test is the only remaining deep AST consumer.
- **C4.4** delete `AstProgram`/`Decl`/`Statement`/`Expression` + `ast_program_free`/`expression_free`/
  `statement_free` + the `cst_id`/`sn`/`sn_src` bridge fields + `semantic_context_program` (returns
  NULL, unused). **KEEP** `cst_build_type` + `TypeRef` (the type representation; Phase 3 migrates it
  to `TypeId` separately).

### Decision — `parser_tests.c` disposition (C4.3)

`parser_tests.c` (912 lines, ~116 assertions) parses a string and asserts on the reconstructed
`AstProgram` (`prog->decls[i]->data.archetype->fields[j]->kind`, …) via `cst_to_program_from_source`.
Its stated purpose is "validate that `cst_to_program` faithfully rebuilds each construct" — i.e. it
tests the very shim being deleted, not the parser→tree step nor the analyzer.

Two options weighed:
1. **Retire** the file (its subject disappears; parser-shape coverage is already exercised by the
   formatter round-trip suite + the 478 semantic LIT tests). Lowest effort, but *loses* the explicit
   per-construct shape assertions.
2. **Rewrite onto `SyntaxView`** — `parse_source(src).syntax_root` + `sv_root`/`sv_kind`/`sv_child`/
   `sv_count`/`sv_token`/`sv_has_token`, asserting on the lossless syntax tree the parser actually
   produces.

**Chosen: (2) rewrite.** It preserves coverage (the conservative, lower-risk-for-the-user choice),
re-points the tests at the parser's *real* output, and removes the last deep AST consumer — the gate
for deleting the structs. `parse_string` returns the `SyntaxNode*` root; `cst_to_program_from_source`
and `semantic_context_program` are then removable. Validation oracle: the `parser-test` binary must
stay fully green.

**DONE.** Rewritten onto `parse_source` + `sv_*`. The dead `handle(Window)` test (removed syntax,
the prior 45/46 failer) was dropped → **45/45**. Two non-obvious tree facts the rewrite pinned down:
(a) `parse_source` lexes the caller's `src` directly and `ParseResult` doesn't retain it, so the
view must be built with that exact string — a `parse_src` wrapper records it for `root_view`;
(b) a bind `x := v`'s expr-position children are `[target-name, value?]` — the value is child **1**
(child 0 is the bound NAME), matching `sem_node_at_expr(v, 1)`. parser_tests.c now includes only
lexer/parser/syntax_tree/syntax_view headers — **zero** AST type/`cst_to_program_from_source` deps.
Full suite 478, ASan 31+6, parser-test 45/45, warning/format clean.

## C4 — DONE

The loader (`sem_collect_decls`, formerly `cst_to_program`) now builds the resolved `DeclSummary`
table **directly** from the tree (`decl_summary_from_node`) into `ctx->decls`, sets provenance flags,
records rename ops, and snapshots export sets — no `AstProgram` is built. `build_decl_table` became
an in-place finalize pass (apply rnops → tree-qualify → tuple-expand). Linchpin that made the
deletion safe: instrumented across all 541 corpus files, the full-`Decl` safety net `decl_summary_from`
fired **0 times**, so every heavy `Decl` existed only to be read for `{node, src, flags}`.

Deleted (compiler-guided via `-Werror` unused-static): the entire `cst_build_*` family
(decl/decl_inner/proc/func/archetype/enum/param/body/body_split/expr/stmt), the AST→summary
converters (`build_proc_from` … `build_archetype_from`), `decl_summary_from`, `sem_param_summary`,
`sem_rename_decl`/`sem_rename_stmt`/`sem_rename_expr`, `sem_expand_tuple_groups`, `sem_decl_name`,
`sem_assign_op`, `syntax_decode_str`, `cst_to_program_from_source`, `semantic_context_program`, and
the `owned_prog`/`prog` context fields (~2000 lines). Then the struct family
`AstProgram`/`Decl`/`Statement`/`Expression` (+ ~30 sub-structs + `StatementType`) and their
create/free functions were removed from `cst.{h,c}`. **KEPT**: `cst_build_type` + `TypeRef` + the
shared enums (`DeclKind`/`StaticKind`/`TypeKind`/`FieldKind`/`Operator`/`ExpressionType`/
`UnaryOperator`) + `SourceLoc`. Validated at every step: 478 LIT, parser-test 45/45, ASan 31/31 + 6/6.

## C5 — DONE

`syntax/cst.{h,c}` (now only `TypeRef` + enums + `SourceLoc`) renamed to `syntax/type_ref.{h,c}`
(guard `CST_H` → `TYPE_REF_H`); all 11 includers + the Makefile object/source lists updated. Stale
`AstProgram`/`cst_to_program` comments scrubbed from lower.c/compile.c/tycheck.h/semantic_tests.c.
Clean `make clean && make`: warning-free; 478 LIT, parser-test 45/45, ASan 31+6, format clean.

**The AST-kill is complete: the syntax tree + SemModel + DeclSummary table are the single source of
truth.** (Phase 3 — migrate `TypeRef` → `TypeId` — is separate, not part of this work.)
