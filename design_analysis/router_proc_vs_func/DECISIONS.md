# Decision log — owned-array threading + procedural-vs-functional router

Running log of non-obvious decisions while implementing the approved plan
(`.claude/plans/there-is-a-problem-calm-aho.md`). Newest at the bottom.

## Goal
Plan fully implemented + verified: (1) non-char arrays work as `own`/in-out params (threading);
(2) fresh-local array value-return errors cleanly (copy-out deferred); (3) procedural vs functional
router implemented, benchmarked, and honestly compared. Full lit suite stays green (365/365).

## D1 — Root cause is the **param signature**, not just returns
Empirically (built+ran):
- `proc(buf: int[8])(buf: int[8])` in-out → codegen `'%arg0' i32 expected ptr`.
- `func(own acc: int[8],…) -> int[8]` → same `i32 expected ptr`.
- mutating a non-`own`/non-in-out array param → (correct) semantic error "borrowed read-only".

So the single root defect: **non-`char` array params are emitted as a scalar (`i32`) instead of a
pointer** (`[N x T]*` shaped / `%struct.arche_array*` unbounded). This breaks in-out, `own`, and
return for `int[]` alike. `char` arrays are special-cased and work.

## D2 — Functional router uses **in-out params**, not array return
Threading via in-out span buffers (`resolve(path, starts, lens)(starts, lens, count)`) is the
`io.fread` pattern and needs only the **param** fix (D1), not array *return*. So the core codegen
work is: param signature + arg passing + in-func indexing for non-char arrays. Array *return*
generalization is secondary; fresh-local return stays deferred (plan Part 2).

## D3 — Represent non-char arrays as type-6 element pointers (reuse, don't refactor type-7)
type-7 (`[N x i8]*`) is pervasively i8/char-coupled (bitcasts, NUL/length/string semantics) —
generalizing it is high-risk. Instead, represent a non-char array (param or local) as a type-6
element pointer (`<T>*`, `field_type` = element name, `string_len` = N). Contained because:
- Index READ already works for type-6 non-char: general read path (~2472) uses
  `scalar_type = llvm_type_from_arche(type6_elem_type)` = i32 for int. No change.
- Index WRITE (~5391) hardcodes i8 (trunc+store i8) — add non-char branch: GEP T, store T, keyed
  on `field_type != char`.
- Param signature (func ~6848 / proc equiv): non-char array -> emit `<T>* %argN` (was scalar).
- Param body: register type-6, field_type = element base name, llvm_name = `%argN`.
- Local shaped non-char array (~4321): alloca `[N x T]`, GEP->first elem `T*`, register type-6.
- Arg passing (~2925-3121): non-char array arg -> pass element pointer (type-6 llvm_name).
All edits ADDITIVE non-char branches; char paths untouched -> low risk to the 365-green suite.
Scope: shaped `int[N]` (router's bounded span buffers). Build+test after each site.

## D4 — Part 1 done; A & B fixed, C & D are fresh-local returns (Part 2)
After the param/local generalization (signature, body, local alloca, type-6 write):
- A (own char[] return) RUN A 65; B (own int[] threaded+returned) RUN B 5 9 — both work, incl.
  the in-out `int[8]` proc param (router pattern) and a func reading an int[] param. Full lit suite
  **365/365** — char paths untouched, no regressions.
- C (fresh local char[8] return) still returns 0 (dangling); D (fresh local int[8] return) invalid IR.
  These are the deferred copy-out. Part 2: in semantic return analysis, error when returning an
  array-typed **local** (is_param==0) — returning a parameter (own/borrowed, by-reference) stays OK.
Float local arrays: storage/load correct, but printf arg coercion treats the element as i32 (a
pre-existing printf-type-inference gap; float arrays were fully broken before). Out of scope; int
(router's need) is correct.

## D5 — Outcome (plan fully implemented + verified)
- Part 1 (codegen): non-char array params/locals generalized (signature, body, local alloca,
  type-6 element-typed write); char paths untouched. Fixes A, B, in-out `int[N]`, func int[] read.
- Part 2 (semantic): fresh-local array value-return → clear "copy-out is not implemented" error
  (C, D); returning an `own`/borrowed param stays valid.
- Part 3 (router): procedural (`bench_proc.arche`) vs functional/threaded (`bench_func.arche`);
  identical checksum (18000000); ns/op a wash (~20.4 vs ~21.0 best-of-5); see COMPARISON.md.
- Verification: 3 new tests (`arrays/int_array_inout_param`, `arrays/int_array_own_thread`,
  `errors/array_local_return_rejected`); **full lit suite 368/368 green**, no regressions, no tests
  removed. Bench programs live in design_analysis/ (not run by `make test`).
