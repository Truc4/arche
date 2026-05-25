# Lossless CST + syntax tooling — design & decisions

Status: implemented & verified (2026-05-24). **Nothing is committed** — all changes
live in the working tree on the current branch for review.

## Verification (as of this session)

- `make` builds clean; compiler test suites unchanged: parser 47/47, semantic 37/37,
  codegen 8/8, lower 7/7 — the CST instrumentation is purely additive and the AST path
  is invariant.
- Lossless round-trip across the whole corpus: **271/271** `.arche` files reconstruct
  byte-for-byte from token spans + inter-token gaps.
- `arche-cst-tokens` classifies identifiers by node context (type / property / function /
  parameter / variable) — confirmed on real examples.
- Editor end-to-end in Neovim 0.12: `.arche` buffer → LSP attach → `semanticTokens/full`
  → 25 correctly-classified tokens.

What is NOT done (deliberate follow-ons): the rust-analyzer end-state where the AST is a
typed *view* over the CST (here the AST is still built directly, alongside the CST); and
the semantic-token layer pulled from the type checker (archetype-vs-builtin, aliases,
mutability, unused).

## Goal

Drive editor syntax highlighting (and, later, rename/diagnostics/other tooling) from
arche's own front-end, so it tracks the language automatically instead of being a
separately-maintained grammar. The editor side is an LSP server emitting semantic
tokens; this document covers the compiler-side foundation it needs.

## Why a lossless CST

The module under `cst/` is named CST but was historically an **AST**: abstract. It
dropped punctuation, stored only a start `line:column` per node (no length/range), and
didn't separately locate sub-identifiers (e.g. the name in `a.field`). You cannot drive
faithful highlighting from that — you'd be reduced to re-lexing and fuzzy-matching
`line:col`, which is exactly the hack modern compilers avoid.

Decision: make the syntax layer a real **lossless concrete syntax tree** — every token
present, every node mapping to an exact source range. This is the rust-analyzer (rowan)
/ Roslyn model.

### What "lossless" means here (and a deliberate boundary)

- Every **syntactic token** (keywords, identifiers, literals, operators, punctuation)
  **and every comment** is a leaf in the tree, each with an exact byte `offset` + `length`.
- **Whitespace is not stored as tokens.** It is *derivable*: the gap between two adjacent
  leaves is `src[prev.offset+prev.length .. next.offset]`. Given the source buffer the tree
  is information-lossless and the source is reconstructable. rust-analyzer keeps whitespace
  as trivia tokens; we keep the source buffer instead. This is a deliberate, documented
  deviation — highlighting and the existing trivia/formatter machinery don't need
  whitespace-as-nodes, and not storing it keeps the builder trivial.

## Architecture

rust-analyzer separates *parsing* (emits events) from *tree construction* (a
`GreenNodeBuilder` consumes events). We port that **safely** into arche's existing
hand-written recursive-descent parser:

- The parser keeps building the existing AST exactly as before — **its logic is
  untouched**. We only *add* builder calls. Consequence: a bug in CST construction can
  never change compiler behaviour. The AST path (and therefore `make test`) is invariant
  by construction; CST correctness is validated separately by a source round-trip.
- A **checkpoint/wrap** green-tree builder (`cst/syntax_tree.{h,c}`), modelled on
  rust-analyzer's `GreenNodeBuilder`:
  - `advance()` (the parser's single token-consumption chokepoint) appends each consumed
    token — and each skipped comment — as a leaf.
  - `checkpoint()` records a position; `wrap(checkpoint, kind)` retroactively groups the
    leaves added since then into a node. Missing/forgotten wraps simply leave a flatter
    tree — **still lossless and correct**, just coarser. This forgiveness is why
    instrumenting a 2600-line parser is low-risk.
- The AST stays the primary input to semantic analysis / lowering / codegen / formatter,
  unchanged. (The rust-analyzer end-state — AST as a typed *view* over the CST, with the
  CST as sole source — is a larger follow-on; not done here.)

## Identifier-role classification (semantic highlighting)

Non-identifier tokens are classified by token kind alone (keyword / string / number /
comment / operator / punctuation). Identifiers get a role from their **CST node context**,
not token adjacency:

| Role        | Source of truth in the CST                                  | LSP token type |
|-------------|-------------------------------------------------------------|----------------|
| `type`      | identifier inside a `TYPE_REF` node, or an `ALLOC_TYPE` name | `type`         |
| `type` (def)| archetype name node (`TYPE_DEF_NAME`)                       | `type`         |
| `function`  | call callee node (`CALLEE_NAME`), proc/sys/func def name    | `function`     |
| `property`  | field-decl name and field-access name (`FIELD_NAME`)        | `property`     |
| `parameter` | parameter name node (`PARAM_NAME`)                          | `parameter`    |
| `variable`  | any other identifier (`NAME_REF`)                           | `variable`     |

Semantic refinements that need name resolution / types (archetype-vs-builtin, type
aliases, mutability, unused) are a later layer pulled from the type checker — independent
of this CST work, matching how rust-analyzer/gopls add semantic on top of syntactic.

## Editor side (separate repo)

- `arche-cst-tokens`: CLI in this repo that parses (stdin or file), walks the CST, and
  prints `offset length line col CATEGORY` per token. The single source of truth for
  highlighting; it tracks the language because it reuses the front-end.
- LSP server (TypeScript, `vscode-languageserver`) shells out to `arche-cst-tokens` and
  re-encodes as LSP semantic tokens. Legend: keyword, string, number, comment, operator,
  punctuation, variable, type, property, function, parameter. Generic category→type map,
  so new categories degrade gracefully without server edits.
- Neovim consumes the semantic tokens natively (`@lsp.type.*` → highlight groups).
- The old tree-sitter grammar + stale arche submodule in the plugin repo are removed; the
  plugin depends on the installed `arche-cst-tokens` binary.

## Implementation decisions (the re-architecture, in progress)

The work follows a 9-stage gated migration (see the approved plan). Decisions so far:

- **Two permanent gates** guard every stage: `make verify-cst` (the CST reconstructs
  every corpus file byte-for-byte — `arche_cst_roundtrip.c`) and `make verify-codegen`
  (emitted LLVM IR for 11 representative programs diffed against checked-in goldens in
  `tests/codegen_golden/`). The codegen-golden gate is the real guard — unit tests don't
  assert whole-program IR, so type/lowering changes can shift codegen silently.
- **CST built by instrumenting the existing parser, additively.** The parser keeps building
  the `Program` AST unchanged; we only *add* `cst_cp()`/`cst_wrap()` calls. A CST bug cannot
  change compiler output (the AST path is untouched) — proven by the IR goldens staying
  identical through all CST work.
- **Wrap at the cheapest correct site, keyed by the AST node already built:**
  - Declarations: one wrap at the `parse_program` call site, keyed by `decl->kind`.
  - Statements: one wrap at `parse_statement`'s single `cleanup` exit, keyed by `stmt->type`.
  - Primary expressions: one wrap at the `parse_unary_expr` call site, keyed by `expr->type`.
  This avoids editing the many early-return paths inside each parse function.
- **Binary precedence nesting** uses the rust-analyzer checkpoint trick: thread the
  left-operand's checkpoint through `parse_binary_rhs` and `cst_wrap` each fold from there;
  left-assoc nesting falls out because each fold collapses to one node at that checkpoint.
- **Parenthesised expressions** wrap themselves as `SN_PAREN_EXPR` inside `parse_primary_expr`;
  the `parse_unary_expr` call site detects "already collapsed to a single node"
  (`cst_single_node`) and skips re-wrapping, avoiding a redundant layer.
- **Grouping nodes `SN_PARAM`/`SN_BLOCK` are deferred**: the CST already nests params
  (`PARAM_NAME` + `TYPE_REF`) and body statements as direct children, which views can scan;
  add explicit grouping only if the view layer needs it.
- **Node ids assigned at wrap time** (a builder counter), not in a post-order pass at finish.
  This lets the parser stash a node's id onto the AST `Expression` it built *as a value*
  (`Expression.cst_id`, stored +1 so 0 = unlinked) — surviving the CST being freed, which the
  pointer form did not (test harnesses free the parse result before semantic, dangling a
  pointer and making the side model over-allocate on a garbage id). Ids stay dense + unique.
- **Typed views** (`cst/cst_view.{h,c}`) are `(node, src)` lenses, zero-copy: text via borrowed
  source slices, optional children via a null view (`cv_present`), `cv_has_error` for recovery.
  Generic accessors (`cv_child`, `cv_count`, `cv_node_at`, `cv_token`) are the substrate;
  construct-specific accessors are thin wrappers added as consumers need them.
- **Semantic side model** (`semantic/sem_model.{h,c}`): resolved types keyed by CST node id,
  populated alongside `Expression.resolved_type` (the tree-mutation channel that S8 deletes with
  `Program`). NOTE the lossless CST is *already* immutable — semantic only mutates `Program`
  (the abstract AST), never the CST — so the side model's real purpose is to give semantic a
  home for types once it walks the CST directly (4d), not to "stop mutating the CST".
- **Lowering reads the side model (4b) is gated off until S6.** The read path
  (`lower_set_model` + `lower_expr_type`) is built, but activating it revealed — via the
  codegen-golden gate — that `use`-module inlining merges several independently-parsed CSTs
  whose `cst_id`s restart at 0 and therefore **collide** in the flat side model (wrong type for
  inlined expressions: `i64` vs `i32`). Globally-unique ids across modules is S6's job
  (`resolve_uses`), so 4b activation waits for S6. Until then lowering keeps using
  `Expression.resolved_type` (correct for every program, modules included).

## Guiding references

Decisions follow Rust/Go practice: rowan/Roslyn green-red trees (lossless, span-carrying),
two-layer highlighting (syntactic from the tree, semantic from analysis), and LSP semantic
tokens as the editor protocol (rust-analyzer, gopls).
