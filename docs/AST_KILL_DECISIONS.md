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

### Decision — `parser_tests.c` disposition

`parser_tests.c` (912 lines, ~116 assertions) parsed a string and asserted on the reconstructed
`AstProgram` (`prog->decls[i]->data.archetype->fields[j]->kind`, …) via `cst_to_program_from_source`
— it tested the very shim being deleted, not the parser→tree step nor the analyzer. It had to go off
the AST one way or another to delete the structs. Two options: **retire** it (its subject disappears;
parser-shape coverage already lives in the formatter round-trip suite + the 478 LIT tests), or
**rewrite** it onto `SyntaxView`.

I first rewrote it (to preserve the explicit per-construct assertions) → 45/45. On review the user
chose **retire**: the rewritten tests re-asserted syntax-tree shape that the round-trip + LIT suites
already cover, so they were redundant maintenance weight. **Final: retired** — `parser_tests.c`
deleted and all its Makefile wiring removed (`PARSER_TEST_BIN`/`_OBJS`, the `all`/`test` deps, the
build rule, the `test-parser` target, `.PHONY`). The `cst_to_program_from_source` /
`semantic_context_program` entry points it depended on were already removed in C4. Remaining compiler
unit coverage: `semantic-test` (31), `codegen-test` (8), `lower-test` (6), `syntax-view-test`, plus
the 478 LIT suite and the formatter round-trip sweep. (Two tree facts surfaced by the short-lived
rewrite, kept here as reference: `parse_source` lexes the caller's `src` directly and `ParseResult`
doesn't retain it; a bind `x := v`'s value is expr-position child **1**, child 0 being the name.)

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
`UnaryOperator`) + `SourceLoc`. Validated at every step: 478 LIT + semantic/codegen/lower unit tests
+ ASan 31/31 + 6/6.

## C5 — DONE

`syntax/cst.{h,c}` (now only `TypeRef` + enums + `SourceLoc`) renamed to `syntax/type_ref.{h,c}`
(guard `CST_H` → `TYPE_REF_H`); all 11 includers + the Makefile object/source lists updated. Stale
`AstProgram`/`cst_to_program` comments scrubbed from lower.c/compile.c/tycheck.h/semantic_tests.c.
Clean `make clean && make`: warning-free; 478 LIT, semantic/codegen/lower units, ASan 31+6, format clean.

**The AST-kill is complete: the syntax tree + SemModel + DeclSummary table are the single source of
truth.** (Phase 3 — migrate `TypeRef` → `TypeId` — is separate, not part of this work.)
