# Aggregate value constants + array/slice rework — autonomous decision log

Context: started from "why does the `extras/platform` demo use zero-arg funcs instead of consts?"
The answer required real aggregate value constants, which cascaded into a broader array/slice rework.
User-made decisions are noted as such; everything else here is an **autonomous** call made while
implementing, recorded so it can be reviewed.

## User-made decisions (for context)
- Fixed-size array `[N]T` vs slice `[]T` are distinct types (the universal split). **Rename** the
  kinds so "array" = fixed-size, "slice" = view.
- **Prefix** type syntax `[N]T` / `[]T` (migrate from postfix `T[N]`).
- **Bare `{ … }`** array literals; the type rides on the binding slot or is inferred (no type on the
  literal). arche is statement-oriented, so a brace in value position is unambiguously a literal.
- **Pools prefix** too: `[5]Particle` (a pool is a fixed-capacity array of archetype rows).
- **Require `;`** on non-brace top-level decls (optional after a `}` body — Jai's rule), to resolve
  the prefix-pool ambiguity (`val :: float` + `[4]Node` → `float[4]`).
- Maximal scope: scalar `int/char/float`, N-D rectangular, strings as NUL-padded char matrices,
  **no pointer indirection**.
- Multi-dim **indexing is comma** `a[i, j]` (arche's existing convention, from `multidim_2d.arche`),
  not chained `a[i][j]`. Type stays chained-prefix `[N][M]T`.

## Autonomous decisions

1. **String value-const = the literal, inlined.** A `char[]` string const has no storage of its
   own; a reference to it lowers to the string literal (`HIR_EXPR_STRING`) so every existing
   `char[]`/`.length`/slice-decay path applies unchanged. Same-file refs are rewritten at lowering;
   device-qualified refs (`platform.name`, only a NAME after qualification) are rewritten in the
   qualify pass and, as a backstop, materialized in codegen's NAME path. Rationale: reuses all
   string machinery; no new value kind; works across the device boundary.

2. **Array value-const = an immutable global, via the static-array machinery.** `XS :: {1,2,3}`
   (and `XS : [N]T : {…}`) is routed — in both the semantic decl-summary and lowering — to a
   `HIR_STATIC_ARRAY` (the same path as the mutable `XS : [N]T = {…}`), flagged const so writes are
   rejected. References resolve via the existing static-array path (`@XS`, indexing, `.length`).
   Rationale: reuses the existing global-array codegen rather than inventing aggregate-const storage.

3. **Const immutability** is a `static_is_const` flag on the array decl; the assign analysis rejects
   both `XS = …` and an element write `XS[i] = …` (leftmost-IDENT lookup). The backing global is
   `constant`/read-only, so this is correctness, not a lint.

4. **`.length` on a static array / array const** resolves to the declared element count
   (compile-time) — added to both the semantic field-base check (a static array's VariableInfo
   carries its *element* type, so the array-length rule was being skipped) and codegen (via the
   static-array registry size).

5. **N-D arrays use arche's flat row-stride model** — the same representation its archetype columns
   already use (`row*stride + col`, see `get_shaped_field_info`). A standalone N-D const is stored
   as a flat `[N*M]T` global with the row stride recorded; comma-indexing computes the flat offset.
   This is the existing model, not a workaround — it keeps one bounds-check, one block, no pointers.

6. **Codemod-driven corpus migration.** The postfix→prefix and pool migrations were done with a
   throwaway parser-driven codemod (tree spans, never touching indexing/comments), not regex, so
   `buf[i]` (lexically identical to `char[N]`) could never be corrupted. The `;` insertion was a
   second tree-driven pass. Embedded snippets in `.c`/`.py` test harnesses were migrated by hand.

7. **Rejection tests hardened.** Bare `! arche build` rejection tests (overloads/*, check_bad) were
   given message-specific greps so a missing-`;` (or any wrong error) can't mask the intended
   rejection.

8. **N-D matrices: comma indexing, flat storage.** Multi-dim *indexing* is comma `M[i, j]` (arche's
   existing convention — `multidim_2d.arche` — not chained `M[i][j]`, which I had briefly added and
   then reverted; chained would imply jagged/array-of-pointers, which arche doesn't have). A
   standalone N-D const is a flat `[N*M]T` global; `M[i, j]` → `row*stride + col`, `M[i]` → the row
   element-pointer. A `{ "a","bb","ccc" }` char matrix is `[3][3]char` with rows NUL-padded to the
   widest. The decl carries the row stride (HIR `array.row_stride`, decl-summary `static_row_stride`)
   so a single index `S[i]` types as a row slice. Standalone multi-dim arrays did not previously exist
   in arche (only archetype columns) — this adds them via the column flat-stride model, no new kind.

9. **count-mismatch is a real diagnostic.** `XS : [3]int : {1,2}` errors in semantic analysis with a
   clear message (declared size vs initializer element count), not by falling through to an LLVM
   type-mismatch — that earlier behavior was a hack and is now removed.

10. **`const_str_matrix` exercises the char matrix via 2-D comma-indexing** (`S[i, j]`, which
    verifies the NUL-padded layout and the row stride). That stays as the layout test; the
    *row-as-slice* path it originally side-stepped is now its own test + fix — see #11.

11. **Matrix ROW decays to a `(ptr, len)` slice (`matrix_row_as_slice`).** A single index into a
    matrix const — `M[i]` of an `[N][W]T` — is a ROW, and passing it where a `[]T` slice is expected
    now decays to a fat pointer `(base + i*stride, len = W)`, exactly as a sized local `[W]T` decays
    (`slice_decay_from_sized`). Earlier this silently miscompiled: the row reached the callee as a
    single scalar arg with no length, so the slice arrived empty (`os.write(1, S[i], …)` printed
    nothing; an arche-func slice param hit an LLVM `ptr` vs `i32` mismatch). Fixed in the call-arg
    lowering (`codegen_matrix_row_arg` + a matrix-row branch that mirrors the whole-static-array
    decay): a partial index into a static matrix const emits the row element-pointer plus the row
    width as the carried length, for `[]T` slice params, `char[]` (arche_array) params, and bare
    `T[N]`/extern params alike. This corrects an earlier inaccurate note here that "plain string
    locals" shared the limitation — they do not: `n := name; …(n, n.length)` works. Two narrower
    surface gaps remain genuinely separate (and now loudly erroring, not silently wrong): binding a
    row to a local (`r := M[i]`) and the inline postfix chain `M[i].length` (the primary parser does
    single-level postfix, so `.field` after `]` doesn't chain). Tracked, not papered over.

## Concern-review fixes (correctness bugs found by stress-probing the decisions)

A pass over the decisions above (not just the tests that existed) surfaced four real defects in the
array-const feature — three silently shipped because no fixture exercised them:

12. **`float` array consts stored 32-bit, read 64-bit (CORRUPTION).** The backing global was emitted
    as `[N x float]` (a hand-rolled `float`→`float` map in two codegen sites), but arche's `float` is
    LLVM `double` (64-bit) everywhere, and every *read* path used `double`. So the bytes were
    reinterpreted and reads returned garbage (`sum(F)`→`8.000002`, not `7.0`). Fixed both sites to map
    `float`→`double`; covered 1-D index, 1-D decay, and matrix-row decay in `const_array_float`. No
    float array-const test existed before — that is how it shipped.

13. **Typed 2-D matrix const `M : [N][W]T : {…}` was rejected** as "element type must be scalar". The
    decl builder took a single `tyid_elem` step (yielding the inner row type `[W]int`) instead of
    walking to the innermost scalar. Now it walks all declared array dims: element = innermost scalar,
    size = product of dims, stride = innermost dim. Covered by `const_matrix_typed`. (The *untyped*
    `M :: {…}` form always worked, which masked this.)

14. **>2-D and ragged matrices miscompiled at the LLVM layer.** A 3-D literal under-counted its flat
    size (the size walk descended only one level) and a ragged numeric row under-filled a no-padding
    global — both surfaced as opaque LLVM type-mismatch errors. Now rejected in semantic analysis with
    clear messages: "has N dimensions … at most 2" (`const_array_3d_rejected`) and "matrix … is not
    rectangular — row R has W elements but the matrix width is S" (`const_matrix_ragged_rejected`).
    True N-D (rank > 2) remains a deliberate non-goal: the flat model carries a single row stride, so
    arbitrary-rank indexing/decay would need a multi-stride shape vector — a separate feature, not a
    bug. The 2-D ceiling is now enforced, not assumed.

## Verification
658/658 lit tests, doctests, and AddressSanitizer+UBSan all green; `extras/demo.arche` runs correctly
for both `linux` and `windows` targets with the backends as `char[]` consts. `matrix_row_as_slice`
covers the row→slice decay (distinct sums per row prove pointer + length both reach the callee);
`const_array_float` guards the float-width corruption; `const_matrix_typed` the typed-matrix element
walk; `const_array_3d_rejected` / `const_matrix_ragged_rejected` the dimensionality + rectangularity
guards.
