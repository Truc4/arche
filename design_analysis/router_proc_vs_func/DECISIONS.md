# Decision log — owned-array threading + procedural-vs-functional router

Running log of non-obvious decisions while implementing the approved plan
(`.claude/plans/there-is-a-problem-calm-aho.md`). Newest at the bottom.

## Status (current — read this first)
- **All element types work** — `int`/`i64`/`float`/`double` (and other int widths) — as locals,
  in-params, in-out params, out-only params, and `own`-threaded returns **by reference**. `char`
  arrays unchanged (separate type-7 path, the `slice` idiom).
- **`.length`/`.cap`/`.capacity`/`.max_length`** return the compile-time count N for these arrays.
- **Bounds-checked** indexing (type-6 and type-7): runtime OOB aborts; provably-in-range
  literal/loop indices elide the check.
- Fresh-LOCAL array value-return (copy-out) is **deferred** → clear compile error (not silent), for
  every element type. An `own`/borrowed PARAM is returned by reference and is valid.
- Router comparison done: procedural (module statics) vs **reentrant (out-param)**; identical
  results; perf a wash. See COMPARISON.md.
- **Int call ARGUMENTS adopt the callee's declared int width** — a bare literal (default i32) handed
  to an `i64` param no longer truncates; an i32 register widens (sext) and an i64 narrows (trunc) to
  match. (D10.)
- **Full lit suite: 385/385 green.** Array matrix (float/i64/int × local/inout/out/own-thread/
  length/oob) + char_array_oob + return-guard + ints/i64_arg_width tests.

## Goal (original)
Plan fully implemented + verified: (1) non-char arrays work as `own`/in-out params (threading);
(2) fresh-local array value-return errors cleanly (copy-out deferred); (3) procedural vs functional
router implemented, benchmarked, and honestly compared. Full lit suite stays green.

## D1 — Root cause is the **param signature**, not just returns
Empirically (built+ran):
- `proc(buf: int[8])(buf: int[8])` in-out → codegen `'%arg0' i32 expected ptr`.
- `func(own acc: int[8],…) -> int[8]` → same `i32 expected ptr`.
- mutating a non-`own`/non-in-out array param → (correct) semantic error "borrowed read-only".

So the single root defect: **non-`char` array params are emitted as a scalar (`i32`) instead of a
pointer** (`[N x T]*` shaped / `%struct.arche_array*` unbounded). This breaks in-out, `own`, and
return for `int[]` alike. `char` arrays are special-cased and work.

## D2 — Functional router uses **in-out params**, not array return  [SUPERSEDED by D6]
**(Superseded: in-out was the wrong idiom for our own procs — D6 switched to out-only params.)**
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
- Part 3 (router): procedural (`bench_proc.arche`) vs reentrant/out-param (`bench_func.arche`);
  identical checksum (20000000); ns/op a wash (~20.2 vs ~20.8 best-of-5); see COMPARISON.md.
- Verification: 4 new tests (`arrays/int_array_inout_param`, `arrays/int_array_own_thread`,
  `arrays/int_array_out_param`, `errors/array_local_return_rejected`); **full lit suite 369/369
  green**, no regressions, no tests removed. Bench programs live in design_analysis/ (not run by
  `make test`). (Note: D5 predates D6's out-param switch — the enabler is out-only array params.)

## D6 — In-out shadow is FOREIGN-only; our procs use OUT-only array params
Feedback: re-listing a buffer in both the in-list and out-list (`(buf)(buf)`) is the FOREIGN-ABI
idiom (lining up with a C signature like `read(fd, buf, n)`). For procs whose signature we own, a
filled buffer is a pure **output** — use an out-only array param, named once. Out-only array params
were broken in codegen (the out-param path used `return_member_llvm` → `i8*`/scalar for arrays).
Fixed at 3 sites (additive, parallel to the in-param work): out-param signature → `<T>* %outN`;
out-param body binding → type-6 element pointer; caller out-arg → allocate `[N x T]`, pass element
pointer, bind type-6 for reading. Now `resolve(path)(starts: int[8], lens: int[8], handler, count)`
works with the caller writing `resolve(path)(s0:, l0:, h:, c:)` — no shadow. Suite 368/368.
Also: "functional" router is more precisely **reentrant / out-param** (still in-place mutation, still
a global trie pool); pure-functional is impossible here (no heap). Wording fixed in COMPARISON.md.
Handlers use a `route` enum, not raw ints.

## D7 — Known limitations of the new array-param support (verified)
The work wired up **`int` (i32)** element arrays end-to-end. These gaps remain (all confirmed by
building the repro):

1. **Only i32 element arrays work.** `float`/`double` and `i64` (and other non-32 int widths)
   element arrays emit invalid IR, e.g. `b: float[4]; b[0]+b[1]` → `opt: 'double' ... expected
   'i32'`; `i64[4]` → `'i64' ... expected 'i32'`. Root cause: the index-read GEP/load uses the
   correct element type (via `field_type`), but the **indexed expression's resolved type is not
   propagated** to the element type for these type-6 arrays — it defaults to int (i32), so a
   `double`/`i64` result feeds an i32-typed consumer (arithmetic/printf). For i32 it coincidentally
   matches; for others it's a verifier error. Fix = set the index expr's resolved type from the
   array element type in semantic. `char` arrays are unaffected (separate path).
2. **`.length` / `.cap` don't work** on these arrays — `b.length` on an `int[8]` local → IR parse
   error. The type-6 element pointer carries the size in `string_len` but no `.length` accessor is
   wired for it. (char[N] type-7 has its own length handling.)
3. **No bounds checking.** Indexing an `int[N]` local/param past its end is silent UB (no trap) —
   `b: int[4]; b[i]=i for i in 0..99` runs without error. The general index path only bounds-checks
   archetype columns, not these arrays.

None of these block the router (it uses bounded `int[8]` span buffers, i32, indexed in range, no
`.length`). They are the honest edge of "non-char arrays work": **`int` works; float/i64/length/
bounds are unfinished.**

## D8 — Proper bounded arrays: element types, .length, bounds (issues 1/2/3 from D7)
- **Issue 1 (element type):** semantic.c `resolve_expression_type` EXPR_NAME now unwraps a
  shaped/array var to its ELEMENT base type, and the bind-annotation union bug (`t->data.name` for a
  shaped array) is fixed. One channel (semantic string → HIR resolved), so `float[]`/`i64[]` index +
  arithmetic + printf now use the right width. (`float[4]; b[0]+b[1]` → 7.5; `i64` sums → 5e9.)
- **Issue 2 (.length/.cap):** codegen EXPR_FIELD type-6 branch emits the constant N (string_len),
  mirroring type-7.
- **Issue 3 (bounds):** `emit_const_bounds_check` (idx `ult` N, reuse `@.arche_oob`+abort) +
  `const_index_in_range` (elide literal/loop indices). Inserted at type-6 read+write AND type-7
  char[N] read+write (closes the same hole for char buffers). In-range unaffected; OOB aborts.
- **Bonus / honesty:** non-char array RETURN byte-truncated (300→44) — the earlier "B fixed" was
  byte-luck. Now returning a non-char array errors cleanly ("hand back via an out-param"); char[]
  `own`-param return (slice idiom) still works; fresh-local return still errors (copy-out deferred).
- **Tests:** tests/unit/language/arrays/ matrix — float/i64/int × {local, in-out, out, .length, OOB}
  + char_array_oob + repurposed int_array_own_thread (asserts the non-char-return error). Full lit
  suite **382/382 green**.
- **Still NOT supported (honest):** array VALUE return / copy-out (deferred); unbounded non-char
  slices `T[]` (use bounded `T[N]` or out-params); these error or are avoided, never silently wrong.

## D9 — Non-char array RETURN by reference now FIXED (supersedes the D8 "errors cleanly" stance)
D8 wrongly *rejected* non-char array return instead of fixing it (and inverted the own-thread smoke
tests into negative ones). Corrected: an `own`/borrowed array PARAM is returned **by reference** for
ANY element type, the same way `char[]` `slice` always has — the caller owns the storage, so handing
back the element pointer is sound. Three precise fixes (no copy-out, no type-7 refactor):
- **`return_member_llvm` (codegen.c ~556):** a func returning a non-char array has LLVM return type
  = element pointer (`i32*`/`i64*`/`double*`/`i16*`), not `i8*`.
- **Caller bind (`is_char_array_call`, ~4490):** the call result is bound as a type-6 array carrying
  the element base name + count, so a later `r[i]` reads at the right width.
- **Arg passing (~3239):** a type-6 array arg to a non-`arche_array` callee is passed at its element
  pointer type (`double*`/`i64*`/…), not hardcoded `i8*` — fixes the `move a` forward on re-threading.
- **EXPR_CALL resolve (semantic.c ~1208):** an array return resolves to its ELEMENT type (matching
  the array-name convention) so the rebound var's `r[i]` isn't a default int (fixed float→i32 printf).
- **Return guard (semantic.c ~2258):** errors ONLY a fresh-LOCAL array return (`!is_param`); an
  `own`/borrowed param return is allowed.
Verified round-trips with no narrowing: `int` 300→300 (not 44), `float` 9.5/2.25, `i64` sum 1e10;
`char` slice still works; fresh-local still errors ("copy-out not implemented"). Smoke tests restored
as POSITIVE: int/float/i64 `*_array_own_thread`. Full lit suite **384/384 green**.

## D10 — Int call arguments adopt the callee's declared width (literal-arg truncation fix)
Surfaced while testing i64 array return: `step(move a, 0, 5000000000)` to an `i64` param gave
705032704 (= 5e9 mod 2^32). Root cause was NOT arrays — a bare int literal's resolved type defaults
to i32, and call-arg passing emitted it at the ARG's width, so a literal (or any i32 register) handed
to an i64 param truncated/mismatched. Typed locals (`x: i64 = …`) and array-element stores already
coerced because the target width is known there; only the call-arg path didn't.
- **Fix (codegen.c arg passing, ~3289):** when the callee's DECLARED param type is `HIR_TYPE_INT`,
  coerce the arg to that width via the existing `emit_int_convert` (relabels a constant for free,
  sext/zext for widening, trunc for narrowing). Takes precedence over the old "pass at the arg's own
  width" branch, which also fixes a wider/narrower *register* arg meeting the param.
- **Verified:** literal 9000000000 → i64 param intact; i32 reg 5000 → i64 param ×1e6 = 5e9; i64 42 →
  i32 param truncs to 42; normal int calls unaffected. Test: ints/i64_arg_width. Suite 385/385 green.
