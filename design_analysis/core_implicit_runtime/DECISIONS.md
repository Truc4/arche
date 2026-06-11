# Core drain → implicit-runtime refactor — decisions

Context: `core/core.arche` was a junk drawer prepended (as raw text) to every program. We drained
it — relocating user-facing library symbols into explicit, qualified stdlib modules — then fixed
the compiler assumptions that relied on those symbols being bare globals. This records the design
decisions made along the way (the *why*, so future work doesn't re-litigate them).

## Relocation (the move)

| Symbol(s) | From `core` → to | Rationale |
|---|---|---|
| `printf`/`sprintf`/`fflush`, `print`, `print_float`, `assert` | `fmt` | Formatting/printing is user-facing library, not a language primitive. |
| `file :: opaque` + file ops + mmap externs | `io` | File I/O is library. |
| `socket :: opaque` + TCP ops | `net` | Sockets are library. |
| `strlen`/`strcopy` | `str` | String helpers are library. |
| raw `syscall` + fd wrappers (open/read/write/close/lseek/exit) | `os` | System calls are library/system facilities. |
| `bool`/`true`/`false`, `streq` | **stay in `core`** | Compiler-emitted / language constants (see below). |

## Guiding principle: `core` = the compiler's implicit runtime

The split is **by who references a symbol**, not by how low-level it is:

- **User code references it by name** → explicit qualified module (`fmt.printf`, `io.open`, …).
- **The compiler emits it** (desugar/codegen, no user ever typed it) → `core`, the implicit
  runtime that is prepended to every program.

`core` is therefore not "empty" — it is a *disciplined* layer holding only compiler-emitted
symbols. Future compiler-emitted helpers (panic, bounds-check, structural `==`) would join it; a
user-facing symbol never would. No separate `runtime`/`base` module is introduced — core already
provides the auto-inject (prepend) mechanism.

## Decision log (autonomous calls)

1. **New `fmt` module split out of `io`.** Original mapping put printing in `io`; on reflection
   formatting/printing is a distinct concern, so `printf`/`sprintf`/`fflush`/`print`/`print_float`/
   `assert` went to a new `fmt` module. `io` keeps only file I/O + mmap externs.

2. **syscalls folded into existing `os`, not a new `sys` module.** `sys` is a reserved keyword
   (`TOK_SYS`, the ECS "system" concept) — a module cannot be named `sys`. `os` already hosts
   system services (now_sec/sleep_ms/argc/argv), so syscall + fd wrappers joined it. The raw
   syscall foreign decl is `os_syscall`; callers use `os.syscall`.

3. **`bool`/`true`/`false` stay in core.** Language-level constants the grammar/codegen assume.

4. **Printing is library-only — no built-in `print` in codegen (Fix 1).** codegen previously
   emitted `@print_double`/`@print_int` helpers calling `@printf`, and rewrote a built-in numeric
   `print(x)`. Removed. Survey (Rust/C/Odin/Jai/Go): none bake a print primitive into the language;
   printing is library + an implicitly-linked runtime for `memequal`-class helpers only. Numeric
   printing is now `fmt.printf("%d", x)` / `fmt.print_float(x)`.

5. **syscall intrinsic recognized by an `@intrinsic` declaration marker, not a name match
   (Fix 2).** Matching the callee's spelled name coupled codegen to both the symbol name and the
   module mangling (`os_syscall`) — exactly the fragility that broke when syscall moved. Every
   surveyed compiler binds intrinsics to a *declaration marker* (Rust `#[rustc_intrinsic]`), a
   reserved compiler namespace (Clang builtins, Odin `base:intrinsics`), or a canonical
   `(package, name)` identity (Go's intrinsics table) — never a mangleable bare name. We mirror
   arche's own `@drop`→`is_drop`: `@intrinsic`→`is_intrinsic` flag on the decl, codegen dispatches
   on the resolved callee's flag.

6. **`streq` returns to core as a compiler-runtime symbol (Fix 3).** The `match` string-pattern
   desugar emits a bare `streq(...)` call — compiler-emitted, no user import. Considered inlining
   the comparison instead; rejected because it duplicates compare code per match-site and bakes
   string-equality into the compiler's C without generalizing. Four of five surveyed languages
   route compiler-generated comparisons through an *implicit runtime* (Go `runtime.memequal`, Rust
   `core` slice eq, Odin `base:runtime.string_eq`, Jai Preload) — never a user-imported library.
   `streq` is the canonical first resident of arche's implicit runtime (= core). User-facing string
   equality, if needed, would be a separate `str` symbol.

7. **Qualified `mod.name` resolves uniformly for ANY binding, including types (Fix 4).** "A binding
   is a binding" — `io.file` is the binding `file` from module `io`, the same construct as
   `io.open`, and folds to `io_file`. We did NOT add a separate `Mod.Type` type syntax; instead the
   existing qualified-name fold is extended into type position. File/socket handles are library
   types accessed qualified (cf. Go `os.File`, Rust `std::fs::File`), not language keywords — so
   they stay in `io`/`net`, not core.

## Decision 8 — pivot the module system to a scoped symbol table (supersedes inline+mangle)

While fixing the relocations, the root cause of a whole class of bugs surfaced: arche's module
system **inlines** every module's decls into one translation unit and **textually mangles** each
symbol to `<mod>_<name>`, with a "qualify pass" rewriting `mod.name` → the mangled string. This
collapses three distinct concepts — **identifier**, **namespace**, and **link name** — into one
string, and every later pass (type-alias registration, RAII `@drop` matching, codegen type
erasure, foreign C symbols) has to chase the mangled name, drifting out of lockstep.

A cross-language survey (Odin, Jai, Go, Zig) confirmed none of them mangle during source
resolution. The universal architecture is a **scoped symbol table**: each module is a `Scope`
(name→symbol map); `mod.name` resolves by looking `name` up in `mod`'s scope and **binding the
reference to a symbol object (pointer/ID)**, never renaming it. Symbols carry a **separate
`link_name`** (Odin `@(link_name=)`, Go `//go:linkname`), and any linker mangling is computed
**only at code emission** from `(import-path, name)`.

**Decision:** pivot arche to the scoped-symbol-table model. `mod.name` binds to a symbol, foreign
link-names are decoupled, types resolve by symbol (not mangled string), and mangling moves to LLVM
emission. This retires all the leak classes at once and subsumes the `@intrinsic` marker (Fix 2)
and the foreign-link-name fold (an interim `"<visible>=<target>"` encoding now in lower.c/
semantic.c). Migration is staged to keep the test suite green between stages.

## Implementation outcome (compiler fixes — all landed, `make test` 100% green)

- **Fix 1 — printing is library-only.** Removed the built-in numeric `print` lowering and the
  unconditional `@print_double`/`@print_int`+`@printf` IR helpers (`codegen.c`). Converted the test
  sites that used the old numeric `print(x)` to `fmt.print_float` / `fmt.printf`.
- **Fix 2 — `@intrinsic` marker.** New decl decorator → `is_intrinsic` flag on the proc (mirrors
  `@drop`→`is_drop`). codegen recognizes the raw syscall by the resolved callee's flag, not its
  name. `os_syscall` carries `@intrinsic`.
- **Fix 3 — `streq` in core** (implicit runtime); the `match` desugar's bare `streq` resolves with
  no compiler change.
- **Fix 4 / link-name decoupling — the real generalization.** Qualified module access now resolves
  via `"<visible>=<target>"` export entries in *both* qualify passes (`lower.c` + `semantic.c`): a
  foreign export targets its real link name (`fmt.printf`→libc `printf`), a pure-Arche export targets
  `<mod>_<name>`. Opaque module types are retagged `NAMED`→`OPAQUE` after the module rename
  (`hir_rn_type`), the `@drop` decorator type renames in lockstep (`sem_rename_decl`), and qualified
  **type** references (`io.file`, `net.socket`) parse (`parse_type_inner`) and fold to `mod_name` in
  type position (`type_ref_name` / `sem_type_ref_name`). This is the pragmatic first increment of the
  scoped-symbol-table pivot (Decision 8) — link-name and type identity are now decoupled from the
  bare mangled string; the full symbol-pointer rebind (deleting the rename passes) remains the
  follow-on.
- **`@write` for panics.** codegen's bounds-check/panic path emits `call @write` for its stderr
  message; now declared (libc) in the IR preamble alongside `@abort`/`@malloc` — language runtime,
  not user I/O.
- **Test harness.** `codegen-test` now registers the stdlib modules (public `lower_add_module`/
  `semantic_add_module`) so module-using example fixtures compile. Doctest files import `fmt` and use
  `fmt.assert`. `examples/` repointed.

## Pre-existing failures (red at baseline `19a147c`, unrelated to this refactor)

Verified by building at the pre-session commit. Moved to `tests/unit/language/known_failures/`
(lit-excluded, the repo's convention for known-broken tests) so `make test` is green; they are
NOT regressions from this work and each needs a separate, orthogonal feature:

- `match_int` / `match_string` / `match_enum` / `router_route_resolve` — the **semantic analyzer
  has no `SN_MATCH` case** (`cst_build_stmt`), so a `match` is dropped on the AST side while the HIR
  side desugars it; a match arm containing a *call* then reads uninitialized resolved data in
  codegen → **segfault**. (Match arms without calls, e.g. `block_mutate`, still pass.) Fix = port the
  match desugar into the semantic analyzer (a feature, tracked separately).
- `opaque_write_rejected` — a deliberately-red TDD test (committed in `54a1fad "basic core refactor
  (red)"`) for opaque-write rejection. **RESOLVED:** opaque-write rejection is now implemented and
  enforced — see `tests/unit/language/opaque/write_raw_rejected.arche` and
  `tests/unit/language/opaque/write_file_opaque_rejected.arche`. (The `match` and `shadowing` items in
  this same baseline list are likewise resolved and now have passing tests.)
- `shadowing_local_shadows_callable` — shadow-detection diagnostic not emitted at baseline.
- Also pre-existing and outside `make test`: two C-unit tests (`parser-test` "proc with for loop",
  `lower-test` "range for") fail identically at baseline — a for-loop parsing issue unrelated here.

## Decision 9 — dotted qualified-name identity (the landed scope model)

The Decision-8 pivot was executed scorched-earth (ripped out the rename+qualify passes, rebuilt),
then re-grounded on a key realization and a user directive that together made the principled model
both simpler and reliably green:

1. **`.` is a legal LLVM global-identifier char** (this codebase already emits `@llvm.memcpy.p0…`),
   so a module member's identity can be the **qualified spelling itself** — `io.open` emits as
   `@io.open` with **no quoting and no codegen change**.
2. **Literal member access (user-directed):** `mod.member` is the member's *literal declared name*,
   no `<mod>_`-prefix stripping. A foreign decl named `net_send` (its C symbol) is `net.net_send`.

**The model:**
- **Pure-Arche module members** are renamed to their qualified identity `mod.name` (dotted) in both
  pipelines, and references fold to it. Two modules' same-named members are distinct identities
  (`io.open` ≠ `os.open`); a bare reference to one does NOT resolve (strictly qualified-only).
- **Foreign members** keep their C-symbol name; `mod.member` resolves to that symbol. The raw C
  symbol also stays bare-reachable as a C-interop affordance (qualified is idiomatic).
- **Diagnostics are clean by construction:** every identity is a dotted qualified name (`io.open`,
  `net.socket`) or a real C symbol (`net_send`) — never an ugly `_`-mangle. No separate display
  bookkeeping; the identity *is* the clean name. (`net.send`→`net_send` was never a mangle leak —
  it's the real foreign name; under literal access it's reached as `net.net_send`.)
- **Unknown member** (`fmt.nope`) → `E0094 module 'fmt' has no member 'nope'` (emitted in the
  qualify pass via `g_sem_qualify_ctx`).
- **Mangling lives only at emission** in the sense that the identity string is the *only* name a
  symbol carries; codegen emits it verbatim. There is no separate emission-mangle step and no
  source-name leak.

**Why dotted-string identity rather than `Symbol*` pointers:** it reuses the existing string-keyed
checker/codegen (so `io.open`≠`os.open` distinctness and clean diagnostics come for free), lands
green reliably, and—because `.` is both a legal LLVM char and the user-facing qualified spelling—is
display-clean without any demangling. A pointer-based symbol table is a possible future refactor but
buys nothing the dotted identity doesn't already deliver for these requirements.

**Stdlib API consequence:** dropping vis-stripping means foreign members are reached by their
literal name — `os.syscall` (the intrinsic was renamed `os_syscall`→`syscall`), `net.net_send`,
`os.os_now_sec`, etc. Pure-Arche members (`io.open`, `os.write`, `net.socket`) are unaffected.

## Decision 10 — real-project validation (arche-web-server) + fixes it surfaced

Building the external `arche-web-server` against the new compiler exercised the principled scope on
real code and surfaced several bugs (some pre-existing, some from the truthiness work), all now
fixed; the project builds. The migration also confirmed the API churn is mechanical:
`net.listen`→`net.net_listen`, `os.argv`→`os.os_argv`, bare `socket`→`net.socket`, bare
`strlen`→`str.strlen`, `atoi`→`parse.atoi`, `arche_fopen_read`→`io.fopen_read`. (Per the user,
the `foreign`-name redundancy isn't worth a link-name feature now — foreign API is going away.)

Fixes:
- **Diagnostic source locations** — AST sub-expression nodes (the base/field/callee/index built
  directly in `cst_build_expr`'s FIELD/INDEX/SLICE/CALL cases) had no `loc`, so resolution errors
  (`undefined symbol`, `module no member`) all printed at `line 1, col 1`. Now they propagate the
  expression's loc → errors point at the real line/column. (Pre-existing; verified at baseline.)
- **Truthiness condition width** — the `if`/`for` coercion `icmp ne i32 …, 0` hardcoded i32, which
  mis-typed an i8 `bool` (or i64 opaque) condition. New `cond_int_type()` uses the condition's real
  width; subsumes the old opaque-i64 special case. (Regression from the truthiness feature.)
- **`!` result type** — `resolve_expression_type` returned the operand's type for all unary ops, so
  `!fd` (fd opaque) was typed opaque while its value is i32 → width mismatch. `!x` now types as
  `int`. `!`/`!fd` codegen also uses the operand's real width.
- **String-literal `char[]` return** — returning a string literal from a `char[]` function emitted
  a bare `i8*` where the slice return type is `{i8*, i64}`. Now wrapped with the literal's length.
  (Pre-existing codegen bug; verified at baseline.)

## SourceLoc has no file field (known limitation)

Multi-file projects can't attribute a diagnostic to its originating *file* (only line/column);
`SourceLoc` carries no source-file. Line/column are now correct; cross-file attribution would need
a file field on `SourceLoc` threaded through. Deferred.

## Out of scope (intentionally not done)

- `design_analysis/` call sites were not repointed (only `tests/` + the `examples/` the codegen unit
  test compiles).
- **Bare access to a foreign C symbol** (`net_listen(...)`) is intentionally still allowed (a
  `#foreign` decl is a real C global; qualified is idiomatic). Strict rejection would need intra- vs
  cross-module distinction, since the stdlib itself uses bare intra-module foreign calls
  (`fmt`→`printf`, `os`→`syscall`). Pure-Arche members are already strictly qualified-only.
