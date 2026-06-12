# First-class slice/array types + chained `a[i][j]` indexing — autonomous decision log

Two user-reported symptoms shared one root cause: arche ran two type systems — a legacy *stringly*
resolver (`resolve_expression_type` → `"int"`/`"char_array"`) and the modern interned `TypeArena`
(`TYK_SLICE`/`TYK_ARRAY`). Index/slice/row expressions were typed by the string path, which returns the
**element** type, collapsing the slice; the slice survived only as a codegen side-channel (the type-6
ValueInfo). So the type system recorded `r := buf[1:4]` as `r : int` — which produced (1) **no slice
hints** in the analyzer, (2) the **dangling `return r`** hole (the return check saw a scalar), and (3)
**`M[i] + 1` mis-typing**. Separately, the comma multi-index `a[i, j]` could not express **per-index
failure policies** (`foo[i] !clamp [j] !abort`), which need chained `a[i][j]`.

User decisions: make slices **first-class via the TypeId-native path**, combined with the
**comma→chained + per-index-policy** change.

## Phase 1 — first-class slices (TypeId-native)

1. **`sem_expr_type_id` at the model-store seam** (semantic.c). Index/slice/row and NAME-to-slice
   expressions now record a real `TYK_SLICE`/`TYK_ARRAY` TypeId in the model instead of round-tripping
   through the element string. A `SN_SLICE_EXPR` → `[]elem`; a partial matrix index → a row `[]elem`; a
   NAME bound to a slice var/param keeps its `var->type_id`; a NAME referring to an array VALUE CONST
   resolves to `tyid_of_slice(static_type_id)` (the static-array VariableInfo carries the *element*
   type, so the decl is consulted directly). Hints now show `[]i32`/`[]char` everywhere.

2. **Slice→pointer decay in tycheck** (tycheck.c `check`). A slice/array GOT is accepted where an
   integer is EXPECTED (syscall args, extern C-ABI params, pointer-width slots). This is **not a
   loosening hack** — it is the same buffer→pointer coercion that worked before, when a buffer
   collapsed to `char` (an intish type); now expressed at the slice level. Without it, slices becoming
   first-class would (correctly typed but) break ~377 FFI/syscall arg checks.

3. **Slice operand rejected in binops** (tycheck.c). An aggregate is not an arithmetic/comparison
   operand, so `M[i] + 1` is now a clean semantic error (it formerly reached an LLVM type error).

4. **`char_array` reconciled at the display layer** (arche_analyzer.c). A char buffer/string renders as
   `[]char` (matching real slices) instead of the legacy postfix `char[]`. The underlying type stays
   the `char_array` nominal — it is load-bearing for lowering's `CHAR_ARRAY` and the string round-trip,
   and the flagged top-risk was a global nominal→`TYK_SLICE(char)` change. Rendering it as `[]char` is
   accurate (it *is* a char slice) and achieves hint consistency without that risk. One golden test
   updated (`infer_var_type`).

5. **Borrow-source taint for the indirect dangling** (semantic.c `borrows_local`). `r := loc[0:3];
   return r` borrows a fresh local's storage, so returning it dangles. A precise taint (set when a bind
   RHS is a slice/row of a fresh-local base, propagated from a tainted base) drives the return
   rejection. The blunt alternative — typing every slice local as a byref aggregate — false-positived
   25 SAFE patterns (own-buffer threading `b := fill(move b); return b`, param-borrow returns), so it
   was reverted in favour of the taint. Direct `return loc[0:3]` was already caught.

## Phase 2 — chained `a[i][j]`, per-index policies, comma removed

6. **Per-index policies via the postfix nesting.** `parse_bracket_index_or_slice` now parses ONE index
   per bracket; a `!policy`/`?policy` is consumed after EACH `[index]` (and the first group) via
   `parse_opt_policy`, so the nested chain gives every index its own `SN_POLICY_REF`. `foo[i] !clamp [j]
   !abort` lowers to nested INDEX nodes, each carrying its policy — which a single comma bracket could
   not represent.

7. **Row index bounds-checked** (codegen.c). The matrix-row index (`M[i]`) now applies its failure
   policy against the row count (`total/stride`) — the default policy when none is written. This also
   fixes a PRE-EXISTING gap: the comma form `M[i, j]` left the row index unchecked (it read out of
   bounds), and couldn't express a fix. The column index rides the existing nested-slice-base bounds
   path with its own policy.

8. **Comma removed; corpus migrated; dead code dropped.** `a[i, j]` is now a clear parse error
   directing to `a[i][j]`. The 4 corpus uses (const_matrix, const_matrix_typed, const_str_matrix,
   multidim_2d) were migrated to chained. The now-unreachable `index_count == 2` codegen branches
   (matrix-const + shaped-column) were removed. docs/language.md updated.

9. **Nested array/matrix types render nested, not flat** (`array_const_type_id`). A whole array const
   is a sized `[N]elem`; a 2-D matrix is the NESTED `[rows][stride]elem` (e.g. `[2][3]i32`,
   `[3][3]char`), not a flat `[]i32`; a partial matrix row is the sized inner `[stride]elem`
   (`M[i]` : `[3]i32`); a `buf[lo:hi]` sub-slice stays a runtime `[]T` view. The row count is taken
   from the *initializer literal*, because `static_size` is the FLAT total for a numeric matrix but the
   ROW count for a string matrix — counting the literal sidesteps that bookkeeping difference. Used by
   both `sem_decl_type_id` (the decl hint) and the NAME-const / partial-index paths.

## Verification
672/672 lit tests, doctests, AddressSanitizer+UBSan all green; `extras/demo.arche` correct for both
targets. Analyzer hints: a matrix const → `[2][3]i32`, a string matrix → `[3][3]char`, a row →
`[3]i32`/`[3]char`, a 1-D const → `[N]i32`, a sub-slice → `[]i32`, a string → `[]char`. New fixtures:
`slice_array_hints` (incl. the nested-matrix assertions), `slice_not_arith_operand`,
`return_slice_of_local_var_rejected`, `per_index_policy_chained`, `comma_index_rejected`.
