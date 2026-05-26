# Lossless CST + syntax tooling — design & decisions

Status: implemented & verified (2026-05-24). **Nothing is committed** — all changes
live in the working tree on the current branch for review.

## Verification (as of this session)

- `make` builds clean; unit suites: parser 47/47, semantic 37/37, codegen 8/8, lower 7/7,
  cst-view 13/13.
- **The CST is the compiler's single syntax tree.** The parser now builds ONLY the lossless
  CST — every `parse_*` returns `int` (success) and drives `cst_cp`/`cst_wrap`; it constructs
  **no** AST nodes (0 AST constructors in `parser/parser.c`; `parse_source().ast == NULL`).
  The abstract `AstProgram` is built solely by `cst_to_program` (from the CST), consumed by
  semantic (`semantic_analyze_cst` → `analyze_program_core`); lowering reads the CST
  (`lower_to_hir`). Other AST consumers obtain it the same way: `cst_to_program_from_source`
  (parser unit-tests, `arche-fmt`).
- **Naming (S9 done) — three clearly-distinguished trees:** the lossless **CST** is
  `SyntaxNode` (`cst/syntax_tree`, `cst/cst_view`); the abstract **AST** is `AstProgram` +
  idiomatic `Decl`/`Statement`/`Expression`/`TypeRef` (`cst/cst.{c,h}`, built by
  `cst_to_program`); the **HIR** is `HirProgram`/`HirDecl`/`HIR_*` (`hir/hir.{c,h}`, built by
  `lower_to_hir`, consumed by codegen).
- The legacy Program-tree backends were deleted (lower) / made CST-fed (semantic); the
  `resolve_uses` pre-pass is CST-driven (registers module CSTs only) and the
  `expand_archetype_tuple_groups` pre-pass + module-prefix machinery are gone.
- Validated: lit corpus **253/253**, **11/11 codegen goldens byte-identical**, `verify-cst`
  **292/292** lossless round-trip, unit suites parser 47 / semantic 37 / codegen 8 / lower 7 /
  cst-view 13. The CST round-trip (292/292) is what proves the parser's wrap re-keying — done
  while removing AST construction — kept the CST byte-identical.
- Design (rust/go-aligned): the compiler builds an abstract AST from the parse and analyzes
  *that* — rustc/Go model — except the AST is built from the lossless CST (`cst_to_program`)
  rather than directly by the parser, so the CST is the one syntax tree feeding analysis,
  lowering, and tooling. (rust-*analyzer*'s "analysis as typed views over the CST" is the IDE
  tool's model; not adopted for this batch compiler.)
- **Formatter is CST-native (S7 done):** `arche-fmt` now formats straight from the lossless
  CST via `format_cst` (`cst/format_cst.{c,h}`) — comment-preserving (comments are CST leaves)
  and idempotent over all 357 parseable corpus files (`make format`). The old 874-line
  AST-based `format_program` + helpers were deleted from `cst/cst.c`, and the redundant
  `arche-fmt-cst` PoC was removed (its job is now `arche-fmt` itself).
- `arche-cst-tokens` classifies identifiers by node context (type / property / function /
  parameter / variable) — confirmed on real examples.
- Editor end-to-end in Neovim 0.12: `.arche` buffer → LSP attach → `semanticTokens/full`
  → 25 correctly-classified tokens.

**All 9 stages (S1–S9) are complete.** The front-end is a single lossless CST: the parser
builds only the CST; the abstract `AstProgram` is derived from it (`cst_to_program`) for the
analyzer; the `HirProgram` is derived from it (`lower_to_hir`) for codegen; and all tooling
(highlighting, formatting, round-trip) reads the CST. The `AstProgram` struct remains by design
as the abstract-AST IR (built from the CST, not a duplicate parser tree).

Recovery note: a delegated attempt at the parser conversion crashed mid-edit leaving
`parser.c` half-converted (forward decls + decl subtree to `int`, expr/stmt subtree still
building AST, non-compiling — and a stale `parser.o` initially masked it). It was finished by
hand: expr + statement subtrees converted to `int`/CST-only, guarded by `verify-cst` staying
292/292. Lesson: after a large/aborted edit, force-rebuild the touched `.o` before trusting gates.

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
- **Lowering reads the side model (4b): ACTIVE and verified.** The codegen-golden gate first
  caught that `use`-module inlining merges several independently-parsed CSTs whose `cst_id`s
  restarted at 0 and **collided** in the flat side model (`i64` vs `i32` for inlined exprs).
  Fixed by making node ids a **single monotonic id space for the whole compiler process**
  (`g_node_id` in `syntax_tree.c`) spanning the main file and all inlined modules — like a
  compiler-session id space. With unique ids, lowering reads types from the model
  (`lower_set_model`/`lower_expr_type`) and the IR goldens match, modules included. The model
  is now genuinely load-bearing: semantic populates it, lowering consumes it.
- **Still on `Expression.resolved_type` (deferred): 4c (stop mutating)** and **4d (semantic
  walks the CST instead of `Program`).** These retire the parser's parallel `Program` tree —
  the largest, highest-risk piece (a ~3k-line semantic rewrite onto views) — and are done with
  S5 (lower onto views) and S8 (delete `Program`). Since `resolved_type` lives on `Program`
  (not the immutable CST), 4c falls out naturally when `Program` is deleted in S8.

- **CST-driven formatter (S7), PoC done as an isolated binary** (`arche-fmt-cst`,
  `cst/format_cst.{h,c}`). Walks the CST's token leaves with structure-aware spacing —
  each leaf carries its parent node-kind, so types/generics compact (`handle<X>`,
  `table<P>`, `float[5]`), `:=`/`::` glue as compound operators (arche lexes them as two
  tokens), and archetype fields print one per line. Idempotent across the whole corpus
  (`fmt(fmt(x))==fmt(x)`). Built alongside `arche-fmt` (which still uses the Program-based
  `format_program`) so nothing churns; replacing it is the rest of S7. This validates the
  CST as load-bearing for a real tool, not just highlighting. Known nit: a `static
  table<X> (n)` keeps one space before `(` (cross-node-kind boundary); structural-walk
  polish. Wrapping the `static` archetype/table ref as a `SN_TYPE_REF` (a real S2
  completeness fix) is what made `table<X>` compact.

## Decoupling completed (this run)

The side model is now the **complete** semantic→lowering channel: lowering reads *all*
semantic facts it needs from it — resolved types (`sem_model_expr_type`) and type-alias
binds (`sem_model_bind_alias`, via `Statement.cst_id`) — and **no semantic facts off
`Program`** anymore, only structure. Type positions are now sub-wrapped by form
(`SN_TYPE_REF`/`ARRAY`/`SHAPED_ARRAY`/`TUPLE`/`HANDLE`), so the CST distinguishes
`float[5]` / `handle<X>` / tuples; the highlighter and CST formatter use this.

## CST-driven lowering: DONE — now the default (S5 complete)

`lower_cst_to_ast_v2` (in `lower/lower.c`) lowers straight from the CST via `cst_view` +
the semantic side model, and is now the **default** lowering path (`main.c`). The legacy
Program-tree lowerer (`lower_cst_to_ast`) is retained only behind `ARCHE_LOWER_PROGRAM` for
A/B comparison. Full verification, all green:

- **11/11 codegen goldens byte-identical** through the whole compiler (incl. the tuple and
  module programs); **lit corpus 253/253**; `verify-cst` 292/292; parser/semantic/codegen/
  lower/cst-view units 47/37/8/7/13.

Couplings found + fixed against the IR oracle (each a real bug the goldens caught):
- param/field/return/bind types must be looked up as *any* `SN_TYPE_*` form, not just
  `SN_TYPE_REF` (else array/handle params silently became `i32`).
- nominal aliases (`file`→`opaque`) resolved via `semantic_resolve_type_alias` (the Program
  path got this from in-place erasure); `cst_root` kept alive through lowering.
- static archetype: `(capacity[, init_length]) { field: value … }` parsed phase-aware
  (parens args vs brace init block); capacity = `field_values[0]`, `init_length` drives
  bounds-check elision.
- index over a member chain (`Particle.pos_x[0]`) must rebuild the `FIELD` base (was dropped,
  giving a raw `double*` access); the indexed column's element type is propagated onto that
  base `FIELD` so codegen sizes the store (a literal RHS with no side-model entry is typed
  from its lexeme).
- 3-part `for ( init ; cond ; incr )` headers: the parser now wraps `init`/`incr` as
  statement nodes; lowering splits the clauses by the two `;` (segment-indexed, so empty
  clauses like `for (; i<n; )` work) and takes the body after `{`.
- multi-bind (`a, b := f()` and `(a: T, b) = e`), `SN_FUNC_GROUP_DECL`, and `use`-module
  inlining (modules' CSTs registered via `lower_add_module`, lowered + name-prefixed +
  cross-module-resolved at lowering, mirroring `resolve_uses`).
- tuple groups: top-level `pos (x,y) :: T` and inline `arche { pos (x,y) :: T }` build a
  registry (`build_tgroups`); archetype fields flatten to scalar columns `pos_x`/`pos_y`
  (codegen has no tuple type); sys tuple params expand to component params with body
  rewrite (`tuple_rewrite_stmt`); nested `arch.pos.x` accesses are collapsed to the flat
  column `arch.pos_x` (`tuple_collapse_*`).

Process note: lit **scrubs the subprocess environment**, so `ARCHE_LOWER_CST=1 lit` did not
reach the compiler until `tests/lit.cfg` was made to forward the toggle — until then the
"253/253" runs were silently the *Program* path. Lesson: verify the toggle actually reaches
the binary (a direct `env VAR=1 ./build/arche …` invocation) before trusting a corpus pass.

`Program` still has consumers: the semantic analyzer walks it (populating the side model),
and the `resolve_uses` / `expand_archetype_tuple_groups` pre-passes feed semantic. Those go
away once semantic moves onto the CST (below).

## CST-driven semantic: DONE — behind `ARCHE_SEM_CST` (S4)

`semantic_analyze_cst(const SyntaxNode *root, const char *src)` in `semantic/semantic.c`
runs the type checker from the lossless CST instead of the parser-built `Program`. It is
selected in `main.c` by the env var `ARCHE_SEM_CST`; **the default (unset) stays the legacy
`semantic_analyze(Program*)` path** (the final flip is deferred). `tests/lit.cfg` forwards
the toggle into the test environment.

### Design decision (how, and why this shape)

A full rewrite of the ~3000-line traversal onto views is the highest-risk piece in the
migration; the analyzer also *reads its own inference writes* and *mutates* the `Program`
(tuple-field rewrites, alias erasure), so it can't be decoupled statement-by-statement.
Instead — mirroring how lowering was migrated and keeping the side-model + error contract
**provably identical** — the CST path **reconstructs an analyzable `Program` directly from
the immutable CST** (`cst_to_program` → `cst_build_decl`/`cst_build_stmt`/`cst_build_expr`/
`cst_build_type`, walking the exact node shapes `lower/lower.c` uses), then runs the SAME
analysis core (`analyze_program_core`, factored out of the old `semantic_analyze`). The key
invariant: each reconstructed Expression/Statement carries `cst_id = (CST node id + 1)`, so
the side model — keyed by `cst_id - 1` — is keyed by the exact node id the CST lowerer reads
back. Identical side model ⇒ identical IR. This genuinely removes the dependency on the
*parser-built* `Program`: the analyzer now reconstructs its working tree from the CST that is
the single source of syntax.

**Modules:** `main.c`'s `resolve_uses` registers each used module's CST with the analyzer via
the new `semantic_add_module` (parallel to `lower_add_module`). `cst_to_program` inlines those
module CSTs and applies the same name-prefixing + cross-module bare-reference resolution as
`resolve_uses` (the helpers are reimplemented over the reconstructed `Program` — `sem_rename_*`),
so symbol resolution / error detection match the Program path. Chosen over "read module decls
from `prog`" so the analyzer needs nothing from the parser-built `Program`.

**Pre-passes:** `cst_to_program` reproduces `expand_archetype_tuple_groups` (top-level
`pos (x,y) :: T` + `arche P { pos }` → flat tuple columns) as `sem_expand_tuple_groups`.
Top-level tuple-group consts are reconstructed as `TYPE_TUPLE`-valued `ConstDecl`s, and
inline archetype-component aliases / fixpoint const classification fall out of the unchanged
pass-0 core.

**No erasure on the CST path:** `analyze_program_core(ctx, prog, erase)` takes an `erase`
flag. The Program path passes `erase=1` (the legacy Program lowerer needs aliases rewritten in
place); the CST path passes `erase=0` — the CST lowerer reads aliases from the side model
(`bind_alias` + `semantic_resolve_type_alias`), so the tree mutation is unnecessary. The
reconstructed `Program` is owned by the `SemanticContext` (`owned_prog`) and freed with it,
because the side model borrows type-name strings that live in it (it must outlive
lowering+codegen, exactly as `main.c` keeps the parser-built `prog` alive).

### Constructs handled

All of them — every declaration (world, archetype incl. inline components + tuple-group
fields, proc/sys/func incl. `extern`/`unsafe`/`own`/`@allow_pure_proc`, func-group, const in
all `::`/`: T :`/`: type : T`/tuple-group forms, static archetype-alloc with `(cap[,init]) {
field: v }` init blocks, static array), every statement (bind incl. const vs variable
classification + local type aliases, multi-bind shorthand + paren forms, assign incl.
compound ops, for in C-style / range / infinite forms, if/else, return, free, break, run,
each_field), and every expression form the corpus exercises (literal, string, name incl.
`table<X>`, field/member chains, index incl. member-chain bases, binary, unary incl.
move/copy, call, array literal). Source `line:column` is reconstructed from token leaves so
lint diagnostics report identical positions.

### Verification (this session, exact commands)

- `make build/arche` clean (no new warnings from `semantic/semantic.c` or `main.c`).
- IR-golden parity **11/11** byte-identical: for each P in `VERIFY_CG_PROGRAMS`,
  `ARCHE_SEM_CST=1 ./build/arche -emit-llvm -o /tmp/s.ll P` == `tests/codegen_golden/<base>.ll`
  (incl. the module + tuple programs).
- `ARCHE_SEM_CST=1 lit tests/` = **253/253** (CST-semantic + CST-lower together, incl. every
  error test ⇒ error parity).
- Diagnostic parity: a sweep of 110 error/overload/type/ownership/handle/extern/alloc tests —
  sorted stderr + exit codes byte-identical between the two paths (including lint line:col).
- Default path fully green: plain `lit tests/` **253/253**, `make verify-codegen` matches
  golden, `make test-semantic` **37/37**, `make verify-cst` **292/292**.
- UBSAN build of the whole compiler: **0** runtime errors across the csv/tuple/use/archetype/
  errors/overloads corpora under `ARCHE_SEM_CST=1` (ASAN can't init under the sandbox's
  address-space limit; UBSAN was used instead).

### Remaining gaps / follow-ons

- The CST path still *builds a `Program`* internally (it just no longer needs the
  parser-built one). The rust-analyzer end-state (analysis as a typed view over the CST, no
  intermediate `Program`) is the larger S8 rewrite; this lands the decoupling without that
  risk. When the default flips and the parser stops building `Program` (S8), the analyzer's
  internal builder + the reimplemented `sem_rename_*`/`sem_expand_tuple_groups` are the
  natural home for those pre-passes (S6), letting `main.c`'s `resolve_uses`/
  `expand_archetype_tuple_groups`/erasure be deleted.
- `EXPR_ALLOC` is reconstructed defensively but never occurs (the parser's heap-`alloc`
  expression form is commented out; `alloc` only exists as `static`).

## Resume plan for the heavy half (mapped, not yet done)

Order: **lower → semantic → delete Program**. Lowering is the easier first switch because
its output (`AstProgram`) is unchanged, so the IR goldens validate it independently.

1. **Lower onto the CST — DONE.** `lower_cst_to_ast_v2` walks the CST purely structurally
   via `cst_view.h` (no `cst_id` on `Decl`/param/field was needed), reads types from the side
   model, and is now the default. The Program lowerer survives only as the `ARCHE_LOWER_PROGRAM`
   A/B path. See "CST-driven lowering: DONE" above for the construct list + couplings fixed.
2. **Semantic onto the CST** (~3000 lines): DONE behind `ARCHE_SEM_CST` (see "CST-driven
   semantic" below). Default path unchanged.
3. **Delete `Program`** (S8): parser stops building it (wraps are the sole output), remove
   the structs + `cst_id` scaffolding, port the 47 parser-tests to assert via views.
4. **S6 finish** (module name-prefixing → symbol layer), **S9** rename (`ast/`→`hir/`, etc.).

Risk: steps 1–2 are where codegen can shift with no failing unit test — gate every sub-step
on `verify-codegen` + the full `make test` lit corpus.

## Guiding references

Decisions follow Rust/Go practice: rowan/Roslyn green-red trees (lossless, span-carrying),
two-layer highlighting (syntactic from the tree, semantic from analysis), and LSP semantic
tokens as the editor protocol (rust-analyzer, gopls).
