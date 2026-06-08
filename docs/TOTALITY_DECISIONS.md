# `proc!` / totality / errors-as-values — implementation & autonomous decisions

This document records the design decisions made while implementing the `proc!` panic discipline,
mandatory-ok `insert`/`delete`, and full out-of-bounds totality. User-approved choices come from the
plan (`proc!` suffix, full OOB totality, mandatory-ok, decl-only call grammar + LSP hint,
stale-handle stays an abort). Everything below the line marked **Autonomous** was decided during
implementation and is logged here per the project goal.

## What shipped (Phases A–F), all green

- **A — `proc!` marker.** Parses at the value-form and proc-type positions; `can_panic_marked`
  threads parser → syntax-tree leaf → `lower` → `HirProcDecl`/`DeclSummary`. `func!` and
  `proc!{group}` are rejected. Formatter prints `proc!(…)` tightly; all files round-trip.
- **B — contagion.** Whole-program fixpoint (`sem_check_panic_contagion`, semantic.c): a proc is
  `can_panic` if declared `proc!`, calls a `can_panic` proc, calls `delete`, or contains an
  unprovable index. A plain `proc`/`func` that is `can_panic` is **E0095 `panic_in_total`**.
- **C — mandatory-ok insert/delete.** Codegen helpers write `(handle, ok)` / `(ok)` out-pointers
  instead of returning/aborting; the value form is removed (**E0096 `insert_delete_outlist`**).
  ~150 call sites migrated via `scripts/migrate_insert_ok.py` (paren-balancing codemod).
- **D — bounds totality.** A static value-array/slice bounds analysis in semantic.c gates the
  `can_panic` bit for indexing.
- **E — stdlib + test migration.** stdlib funcs rewritten to provable form; abort/feature tests
  converted or guarded; rejection + feature regression tests added.
- **F — analyzer hint.** `arche_analyzer.c` emits a `panic` inlay (`!`) at any call to a `proc!`.

New diagnostics: `E0095 panic_in_total`, `E0096 insert_delete_outlist`.

---

## Autonomous decisions

1. **`extern`/FFI procs are OUTSIDE the totality system (not panic-capable).** Treating a foreign C
   boundary as panic-capable would force `proc!` onto everything calling `write`/`print`, making
   nearly the whole stdlib `proc!` and gutting the "a plain proc can't crash" signal. Panic = arche's
   own abort sites only (insert overflow, stale handle, OOB index). The plan floated externs-as-panic;
   this was overridden for ergonomics.

2. **`delete` is inherently `proc!`.** A *stale* handle (use-after-free) is a bug and still aborts;
   only generation-exhaustion (a resource limit) is reported via `ok=0`. So any `delete` makes its
   caller `proc!`. (User-approved direction; implemented as a `delete`-is-an-abort-site rule in the
   contagion walk.)

3. **Bounds totality gates indexing of array/slice PARAMETERS and explicitly-typed LOCAL arrays**
   (`s[i]`, `buf[i]`, a local `b: char[8]; b[i]`). This covers the string/buffer code where OOB bugs
   bite. Per the "we need real guarantees; use `proc!` where we can't verify — so be it" directive,
   local-array gating was added on top of the initial params-only scope; the fallout (csv loaders,
   router) was handled by guards or `proc!`. Still deliberately **exempt** (these can still abort at
   runtime inside a "total" proc — see Known limitations):
   - **Pool columns** (`Arch.field[i]`) — the trusted data-oriented bulk model. Gating them is
     impractical: column liveness is dynamic, so almost all column indexing would become unprovable
     and force `proc!` virtually everywhere.
   - **Module/global static arrays** (e.g. router's `cap_start[cap_count[0]]`).
   - **Inferred-type locals** (only explicitly-typed local arrays are tracked) — kept exempt to avoid
     false positives.
   - **Slice-creation expressions** (`a[x:y]`) — only *indexing* is gated; slice bounds keep their
     runtime check.
   Runtime bounds checks in codegen are **retained as a backstop** (no check elision was implemented),
   so memory safety holds regardless of the prover's conservatism, and a prover bug aborts loudly
   rather than corrupting. The prover gates the `proc!`/E0095 classification.

4. **Accepted "provably in-bounds" patterns** (sound, conservative — see `bnd_*` in semantic.c):
   - literal `k` into a sized `T[N]` param with `0 <= k < N`;
   - index var proven `0 <= v < base.length/.cap` by a `for v:=K>=0; v<base.len` loop or guard-exits
     (`if (v<0){…} … if (v>=base.len){…}`), including the **`&&`-conjunction** and **`||`** forms;
   - min-length facts from `if (s.length > 0)` etc. (validates `s[0]`);
   - literal upper bounds (`i < K` / `if (i >= K)`) into a sized `T[N]` when `K <= N`;
   - nonneg inferred from `i := K>=0`, `i >= 0`, and `i + j` of nonnegs.
   Block scoping uses snapshot/restore so a conditional reassignment doesn't leak facts. Anything not
   matching is reported unprovable (→ guard it, or mark the proc `proc!`).

5. **`&&` / `||` were added to `sem_tok_to_op`** (previously `OP_NONE`) so the analysis can read
   conjunctive/disjunctive guards. No other consumer of `sem_binary_op` regressed.

6. **Codegen fix: sized-array params (`T[N]`) now carry their static length N** (was `-1`). This
   fixes `.length` on such params and lets them forward into a slice param with the correct length —
   a pre-existing bug (documented in `func_char_array_param_forwarding`) that the now-total `strcopy`
   surfaced. Only the **func** registration path was fixed; proc/monomorph sized-array params still
   register without length, so the few sized-array guards in *proc* tests use the literal size (which
   is the exact bound).

7. **router `seg_eq`'s `stored` param retyped `char[] → char[32]`.** It receives a `TrieNode.seg`
   column cell (a `char[32]`); as a `char[]` slice its `.length` was unreliable (~0), so the bounds
   guard fired immediately and static-segment matching broke. Typing it `char[32]` gives a reliable
   `.length` (32) via decision #6.

8. **Test handling** (within the user-approved "convert/guard" mandate):
   - `capacity_exhausted.arche` → asserts `ok==0` (no abort).
   - `stale_handle_abort.arche` and the handle tests → `main` wrapped in `proc!` (stale still aborts).
   - Array/ownership *feature* tests (ownership threading, fills, char-array params) → given **real
     bounds guards** so they stay green AND keep their feature coverage (not converted to
     compile-error, to avoid losing coverage).
   - The genuinely-unbounded `step(own a, i){a[i]=v}` shapes → guarded with the exact size bound.
   - Added `tests/unit/language/panic/`: `proc_bang_chain_runs`, `plain_proc_calls_bang`,
     `func_bang_rejected`, `insert_mandatory_ok`, `bare_insert_rejected`, `unprovable_index_rejected`.

## Known limitations (honest)

- **No codegen check elision.** "Total" means *statically no unprovable index in a gated category* +
  the runtime check as a safety net. This is deliberate: eliding checks based on a hand-rolled prover
  would turn a prover bug into memory corruption; keeping them means a prover bug aborts loudly. So a
  "total" proc is guaranteed not to OOB-*corrupt*, and is compile-checked free of unprovable indices
  in the gated categories — but the binary still contains (provably-unreached) abort paths.
- **Exempt categories can still abort in a "total" proc** (decision #3): pool columns, module
  statics, inferred-type locals, and slice-creation. The most consequential is **pool columns** —
  e.g. router's `cap_start[cap_count[0]]` over-8-captures write is still only a runtime abort.
  Gating columns was judged impractical (dynamic liveness ⇒ ~everything becomes `proc!`); flag if you
  want it anyway.
- The bounds prover is **conservative, not fully flow-sensitive** (e.g. it won't relate
  `a.length == b.length`); provable code that doesn't match a recognized pattern must be guarded or
  marked `proc!`. Soundness is hand-verified against the test suite, not proven.
- Sized-array params in **proc/monomorph** contexts don't carry their length (only the func path was
  fixed, decision #6), so a few proc tests guard sized-array params with the literal size.

## Cheap follow-ups done this round

- **Local-buffer gating** (decision #3 above) — closes the biggest guarantee hole; `*_array_oob`
  tests now assert a **compile-time** rejection (not a runtime abort).
- **Sized-array-param `.length` fixed on the func path** — the array `own`-thread func tests use
  `a.length` again instead of magic numbers.
- **Ignored `ok` is already surfaced**: a bound-but-unused `insert`/`delete` `ok` triggers the
  existing `unused_local` lint; `_` is the explicit, intentional discard (the Go `_ = err` model), so
  no new lint was needed.
