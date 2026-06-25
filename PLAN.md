# Proc elimination via ECS decomposition — no `routine`, funcs stay pure

## Context

`proc` is being removed from the language except `#foreign` externs and primitives (`@syscall`/`@intrinsic`).
A four-lens analysis (data-oriented, PL-theory, ergonomics, minimalist) converged on adding a **`routine`**
kind (a bounded free monad ≡ compile-time-spliced callable) plus **pool-reads-inside-funcs**. The user
**rejected both**, and correctly:

- A `func` that reads a **mutable pool** is **not pure** — that read belongs in a system, not a func. Funcs
  stay strictly pure (params + `::` constants only). `func_impure` stays strict.
- There must be **no intra-row monadic residue** and **no `routine`**. ECS already replaces value-level
  `bind`: **system 1 produces `n` (writes a column); system 2 consumes `n` (reads it) and does the `n+1`
  step.** The schedule orders writer-before-reader *within one pass* (`seq`/data-derived), so a
  result-dependent step is just a producer→consumer **data edge**, not a callee. Branch-on-result → the
  producer **routes** rows into type-specific pools; unbounded loop → **bulk-read + columnar scan** (+ a
  pushback column for over-read), not per-byte sequencing.

So the model is pure ECS: **`proc` → `#foreign`/primitives only; every effectful or pool-touching op → a
system over request/result pools, ordered by data edges; funcs stay pure; no new kind.** It needs almost no
new language surface — it is a *rewrite* onto mechanisms that already exist (`each`/`system`/`map`/`query`/
`insert`/`delete`/`#run`/`@drop`), plus the lint flips. Accepted costs: no called effectful/stateful helpers
(more setup per op), and a genuinely unbounded no-read-ahead interactive loop costs a schedule tick per
round-trip. Rollout: **mechanisms first, one module at a time, build green throughout**, flip lints only once
stdlib is clean.

Already landed & green (keep): **`fmt.fail`** (pure func builds the abort Eff; the condition is system
control flow — the assert replacement), and the **char-column query-read codegen fix** (systems can now read
`[N]char` columns — needed for buffer/line scanning).

## Views & substrings: the same-row slice rule (resolved by four-tradition research)

Cross-tradition research (Cyclone/regions, Rust+rustc spans, ECS handles, Swift `~Escapable`/Vale)
converged on one model for "pass a borrowed substring of a buffer between systems without UAF":

- A view has **three orthogonal axes** — temporal (does storage exist), identity (same logical row),
  spatial (in bounds). For arche's **immortal, non-relocating pools** the temporal axis is a no-op
  (Cyclone heap-region `` `H ``; rustc stores integer `BytePos` spans into the immortal SourceMap for
  exactly this reason). Identity is the only axis pure regions miss — it's the **per-row free/reuse**
  case, covered by arche's existing generational handle (== Vale generational reference).
- **Rule:** a slice (`[]char`/`[]byte`) **column is storable iff its source buffer is in the *same row*.**
  Same-row ⇒ co-lifetime ⇒ can't dangle (the substring and its buffer die together). This is the parse
  case and it is *all* the HTTP server ever needs.
- **Storing a slice whose source is a different row/pool is a HARD ERROR** (independent lifetimes →
  UAF). **Decision: hard-error only, defer the escapes.** Cross-row simply won't compile; the manual fix
  is "copy the bytes into an owned buffer column" (no first-class blessing built yet). The
  **handle-backed view** `(handle, off, len)` gen-checked on materialization is the eventual cross-row
  mechanism but is **YAGNI** — no current program needs it; it only arises for a cache/index holding
  substrings across pools over time. Build it when a real case appears, not before.
- A bare span `(off,len)` int pair is *vindicated* for the same-row case (it's what rustc ships) but a
  slice column is the more ergonomic same-row form (consumer says `method`, not `raw[ms:ms+ml]`).

**Concrete change:** rewrite `stdlib/http/http.arche` `parse_request_line` from the 4 span columns
(`method_start/len`, `target_start/len`) to same-row slice columns `method :: []char`, `target :: []char`
(`method = raw[0:sp1]`). Add the same-row-source check as a hard error in semantic analysis (a slice
stored into a column must trace to a column of the *same* archetype). Capture the model in `docs/design/`
(views, substrings, the same-row rule, the three axes). Defer the handle-view.

## Mechanisms (build/confirm first)

1. **Lints (the discipline).** Strengthen the proc rule (`semantic.c` `lint_proc_decl` ~4564): a `proc` is
   legal **only** if `#foreign`/`@syscall`/`@intrinsic`; every other proc is an error naming the fix (→ a
   `func`/`system`/`each`/`map`, or decompose across systems). Promote it + `func_impure` (W0003, keeps funcs
   pure — *do not* relax for pool reads) + `pool_index_outside_query` (W0029) to error-by-default via
   `g_werror` in `ensure_init` (`sem_diagnostics.c` ~211). Promote **after** stdlib is converted (flipping
   first bricks every program that imports stdlib).
2. **Data-edge ordering.** Bounded dependent stages order with explicit `seq({ producer, consumer })` *today*
   — sufficient to start. Optionally implement **data-derived scheduling** (writer-before-reader on columns,
   `docs/design/data-oriented-io-and-scheduling.md`) later for ergonomics; not a prerequisite.
3. **`@drop` close-on-delete** already exists (the column-type-targeted `@drop`-on-`delete`). It is the
   resource-cleanup hook (delete a row → its registered dtor fires). Keep; revisit only if the dtor itself
   must become non-proc.

## Conversion — one stdlib module at a time (build stays green)

For each module, rewrite called effectful/stateful procs into **systems over pools**; pure logic into pure
**funcs**; sequencing into **`seq`-ordered systems sharing pools**. Representative shapes:
- **`stdlib/io`** → io-as-data: a `File` archetype (fd column), a `read` system filling a buffer column, a
  `lines`/parse system scanning buffers (uses the char-column read fix), close = `delete` → `@drop`. Replaces
  `fread_line`/`skip_header`/`file_map` (no callee; consumers read result rows).
- **`stdlib/router`** → `RouteReq`/`RouteResult` pools + a `route` system (reads `TrieNode`, writes the
  handler column); `add`/`init`/`new_node` become **boot systems** that build the trie from route data
  (`insert`).
- **`stdlib/csv`** → a `load` system: the mmap is one `func→Eff` run once, the row parse is a columnar fan.
- **`stdlib/http`, `stdlib/os`, `stdlib/fmt`** (assert→`fmt.fail` done) similarly.
Verify each module by compiling a driver that uses it (systems + `#run`) and round-tripping its behavior
before moving on.

## Flip lints, then convert programs

Once stdlib is proc-free: promote the lints to error (all at once). Then:
- **Remove `main` → `#run`** (`semantic.c` ~4590 entry rule; `codegen.c` `main`→`main_user` ~10673 and the
  `@arche_run` path). An executable is declarations + one `#run`.
- **Convert ~643 test `main`s + ~134 helper procs + every `assert` site**: `main { … }` → systems + `#run`;
  `fmt.assert(c,m)` → `if (!c) { fmt.fail(m)(_:); }` in a system; helper procs → systems/funcs. Codemod the
  common shapes; hand-convert the rest. Update `examples/`, `docs/`, `README.md`.

## Critical files
- `semantic/sem_diagnostics.c` (`ensure_init` werror), `semantic/semantic.c` (`lint_proc_decl` ~4564, main
  entry ~4590), `codegen/codegen.c` (`main_user`/`@arche_run`; `@drop` already done; char-column read done).
- `stdlib/{io,router,csv,http,os,fmt}/*` (rewrite to systems+pools), `tests/**`, `examples/**`, `docs/`.

## Verification
- Per module: a driver using it compiles + round-trips; build stays green until the deliberate lint flip.
- End state: `make test` (lit + doctests) 100%, `make check-corpus -Werror` clean (no proc outside
  `#foreign`/primitives anywhere), `make verify-fmt` clean. No `@allow` added for the promoted lints.

## Out of scope / non-goals
- **No `routine` kind, no value-level `bind`, no pool-reads-in-funcs** (the rejected convergence).
- Genuinely-unbounded low-latency interactive streaming stays tick-granular (accepted DOD trade).
