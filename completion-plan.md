# Principled code completion for arche

## Context

arche today has an editor-analysis server (`arche-analyzer --serve`) that does inlay hints,
diagnostics, semantic tokens, and go-to-definition — but **no code completion**. Typing `fmt.`
and getting the list of module members (or `player.` and getting archetype fields) is unsupported.

We want this built the way mature compilers (rust-analyzer, Roslyn, clangd) do it, not as a
regex/token-peeking hack in the editor. Two properties of the current codebase shape the design:

1. **The parser aborts on incomplete input.** `parse_primary_expr` at `parser/parser.c:1419-1423`
   sees `.` with no following identifier, calls `error()` and `return 0` — the expression collapses
   and is wrapped in a statement-level `SN_ERROR`. Completion fundamentally requires `fmt.` to
   survive as a real `SN_FIELD_EXPR` whose member slot is an explicit *missing* node.
2. **The semantic model can look symbols up by name but cannot enumerate them.** `semantic.h`
   exposes `semantic_field_exists(arch, field)` (name-keyed) and `semantic_decl_count/_at` (globals),
   but there is no "list the members of module `fmt`" or "list the fields of archetype `Player`"
   primitive. Module exports live in the internal `UnitInterface.exports` string array.

Decisions taken (with the user):
- **Completion surface (v1): members + globals.** `fmt.` (module members), `x.field`
  (archetype / tuple / handle fields), and global symbols (procs, funcs, types, consts, module
  names). **In-scope locals are deferred** — they require persisting a scope tree (scopes are freed
  at `pop_scope`, `semantic.c:845-895`) and are explicitly out of scope here.
- **Parser resilience: general missing-node facility.** Build a reusable missing-token/error-node
  primitive in the CST builder and apply it across the expression/postfix/type recovery points, so
  every completion context parses into a well-formed tree — even though the v1 engine only consumes
  the member + global contexts.

The architecture is a clean 4-layer stack: (1) resilient parse → (2) queryable semantic model →
(3) completion engine that classifies context and assembles candidates → (4) protocol + client + tests.

## Non-goals (v1)

- In-scope local/param completion (needs scope-span persistence — separate follow-up).
- Incremental / demand-driven (salsa-style) reanalysis. Re-analyze-on-`UPDATE` is fine at this scale.
- Editing the Neovim client. `Truc4/arche.nvim` is an external repo; we document the new protocol
  verb so it can adopt it, but do not modify it here.
- Fuzzy ranking/scoring sophistication. v1 does prefix filtering + stable ordering.

## Phase 1 — Resilient parsing foundation (general missing-node facility)

**Goal:** incomplete input parses into a well-formed CST with explicit *missing* markers, while
still emitting the same diagnostics. The tree, not the error, drives completion.

- **CST builder primitive** (`syntax/syntax_tree.h` + `syntax/syntax_tree.c`):
  add `syntax_builder_missing(CstBuilder *b, SyntaxNodeKind kind, uint32_t offset, int line, int col)`.
  It appends a **zero-width synthetic leaf** at the cursor offset and wraps it as a node of `kind`
  (e.g. `SN_FIELD_NAME`). Representation: introduce an `is_missing` flag on the token-leaf branch of
  `SyntaxElem` (or a dedicated `SE_MISSING` tag) so a missing leaf is distinguishable from EOF (also
  zero-length). Childless-node wrapping currently returns NULL (`syntax_builder_wrap`), so the
  missing leaf is what gives the node a stable span. Add `syntax_node_is_missing(node)` accessor.
- **Parser recovery pattern** (`parser/parser.c`): at each "expected X here" point in the
  **expression / postfix / type** grammar, replace `error(...); return 0;` with
  `error(...); syntax_builder_missing(...); <continue/produce node>`. The diagnostic is preserved;
  the tree gains a recoverable hole. Apply at:
  - member access `.<missing>` — the dot path at `parser.c:1419-1436` (both the leading and the
    `while (match(TOK_DOT))` chained cases). Produce `SN_FIELD_EXPR` / `SN_CALL_EXPR` with a missing
    `SN_FIELD_NAME` instead of returning 0. **This is the dominant completion trigger.**
  - call args `f(<missing>` and index `x[<missing>` recovery points.
  - type position after `::` / in `SN_TYPE_REF` (future type-completion context).
  Keep statement/decl-level `synchronize()` + `SN_ERROR` recovery (`parser.c:181-216`) intact — the
  new facility sits *below* it, recovering inside an expression before panic-mode is needed.
- **Risk control:** this changes error-recovery shape. Mitigation: keep `error()` calls (diagnostics
  unchanged), and gate on the **full existing parser/lit test suite** passing. Add focused parser
  tests asserting `fmt.`, `f(`, `x[`, `x :: ` each produce a tree with a missing node (not a bare
  `SN_ERROR`).

## Phase 2 — Semantic enumeration / query API

**Goal:** a small, principled set of *enumeration* primitives mirroring the existing *lookup* ones,
exposed in `semantic.h`. No string-scraping in the analyzer.

- **Module members** (new; backs `fmt.`): `semantic_module_member_count(ctx, module)` and
  `semantic_module_member_at(ctx, module, i, &visible_name, &kind)`. Implemented by iterating the
  internal `UnitInterface.exports` `"name=identity"` array (`semantic.c` ~5761-5783, `sem_decls.h`
  `UnitInterface`). Parse off the `visible_name` before `=`; map identity to a `DeclKind` via the
  decl table where possible.
- **Archetype fields by index** (new; backs `x.field`): `semantic_archetype_field_count(ctx, arch)`
  and `semantic_archetype_field_at(ctx, arch, i, &name, &type_name, &kind)`. Thin wrappers over the
  internal `ArchetypeInfo.fields[]` (`semantic.c:21-40`, `find_archetype`). Complements the existing
  name-keyed `semantic_field_exists/_kind/_type_name`.
- **Tuple fields**: already available — `tyid_tuple_count/_field_name/_field_type` (`sem_types.c:342-360`).
- **Globals**: already available — iterate `semantic_decl_count/_at` (`DeclSummary`: `name`, `kind`,
  `unit`, `visibility`). The engine filters to `VIS_EXPORTED` / same-unit.
- **Type-of-base resolution** (for `x.field`): read `sem_model_expr_type_id(model, base_node_id)`
  (`sem_model.h:51`), then use `tyid_kind` + handle/tuple accessors to get the archetype name or
  tuple fields. Add a convenience `semantic_type_of_node(ctx, node_id)` wrapper if cleaner.

## Phase 3 — Completion engine + `COMPLETE` verb

**Goal:** classify the completion *context* at the cursor and assemble candidates. Reuses GOTO's
position plumbing wholesale.

- **`emit_complete(a, line, col, path)`** in `arche_analyzer.c`, modeled on `emit_goto`:
  1. `offset_of_linecol` → `descend_to_offset` → ancestor chain (existing helpers, `~943-999`).
  2. **Classify context** from the chain + missing node:
     - Cursor at a (missing or partial) `SN_FIELD_NAME` inside `SN_FIELD_EXPR`/`SN_CALL_EXPR`:
       resolve the base. If base `SN_NAME_EXPR` text is a registered module (`semantic_has_module`)
       → **module-member** context (Phase-2 module enumeration). Else read the base's
       `expr_type_id`; if handle(archetype) → **archetype-field** context; if tuple → **tuple-field**
       context.
     - Otherwise (bare partial name / expression position) → **global-symbol** context:
       globals (`semantic_decl_at`) + module names + type aliases/enums.
  3. **Prefix filter:** the already-typed text in the field/name node (empty when the node is
     missing, e.g. `fmt.|`) is the prefix; case-sensitive prefix match, stable order.
  4. **Emit** one line per candidate, blank-line terminated (matches GOTO/HINTS framing):
     `CANDIDATE <kind> <name> <detail>` where `<kind>` ∈ {module, proc, func, field, type, const}
     and `<detail>` is a type/signature string (`tyid_display` / `semantic_field_type_name`).
- **Server verb** in `run_serve` (`arche_analyzer.c` ~1371, after GOTO):
  `COMPLETE <line> <col> <path>` → `docs_find` → `emit_complete`. Same `sscanf` shape as GOTO.
- **One-shot CLI mode** (for testability, mirrors `--dump`): `arche-analyzer --complete <L> <C> <file>`
  runs a single analyze + `emit_complete` and prints candidates. This is the deterministic surface
  the test harness drives.

## Phase 4 — Protocol docs + tests

- **Docs:** `docs/tooling.md` — add `COMPLETE` to the persistent-server verb list (~line 69) and a
  short section specifying the request (`COMPLETE <line> <col> <path>`) and the `CANDIDATE` response
  line format, so `arche.nvim` (external) can adopt it.
- **Tests:**
  - Parser resilience unit tests: `fmt.`, `f(`, `x[`, `x :: ` each parse to a tree containing a
    missing node (assert via a tree-dump or a small C test next to existing parser tests).
  - Completion tests via the `--complete` one-shot mode: a fixture `.arche` + expected candidate
    set for `fmt.` (module members), `Player-handle.` (archetype fields), tuple `.`, and a
    top-level bare prefix (globals). Follow the existing analyzer test convention (alongside
    `--dump` tests).
  - Run the **full existing suite** to confirm the parser change is behaviour-preserving for
    valid input.

## Key files

- `syntax/syntax_tree.h`, `syntax/syntax_tree.c` — missing-node builder primitive + accessor.
- `parser/parser.c` — apply missing-node recovery at expression/postfix/type points (esp. 1419-1436).
- `semantic/semantic.h`, `semantic/semantic.c` — new module-member + archetype-field-by-index
  enumeration; optional `semantic_type_of_node`.
- `arche_analyzer.c` — `emit_complete`, `COMPLETE` serve verb, `--complete` one-shot mode.
- `docs/tooling.md` — protocol documentation.
- `tests/...` — parser-resilience + completion fixtures.

## Verification

- `make` builds clean (`-Wall -Wextra -Werror`).
- `make` test suite (existing parser/lang/lit tests) passes unchanged — proves the resilient-parser
  change is behaviour-preserving on valid input.
- `arche-analyzer --complete <L> <C> <file>` returns the expected `CANDIDATE` sets for the four
  fixtures (module members, archetype fields, tuple fields, global prefix).
- Manual end-to-end: drive `arche-analyzer --serve` with `UPDATE` then `COMPLETE` on a `fmt.`
  cursor; confirm `printf` et al. come back. (arche.nvim wiring is external, validated separately.)
- New parser tests confirm `fmt.` / `f(` / `x[` produce missing nodes, not aborted `SN_ERROR`.
