# Array archetype columns (`char[N]`, `T[N]`) — insert + read

Date: 2026-06-01
Status: ✅ implemented

## Problem

An archetype could *declare* a `char[N]` column (`Node :: arche { seg :: char[32] }`)
and the program compiled — but only with a no-op `main`. Any actual use crashed or
miscompiled:

1. `insert(Node, "users", 7)` failed to optimize:
   `'%v336' defined with type 'ptr' but expected 'i8'` — the generated
   `@arche_insert_Node` typed the `char[32]` param as the element type `i8` and did a
   single scalar `store`, but the call site passes the array **pointer**.
2. The column's struct field was sized `[capacity x i8]` instead of
   `[capacity * N x i8]`, so the per-row stride was 1 byte. A second insert's write
   ran off the end and corrupted the rest of the struct (manifested as "stack overflow").
3. Runtime-indexed reads (`Node.seg[i]` with `i: i32`) emitted `mul i64 %i, N` without
   widening `%i` to i64 → `'%i' defined with type 'i32' but expected 'i64'`.

The case was parked under `tests/unit/language/known_failures/multidim_varlen.arche`
(no-op `main`, hiding the bug).

## Fix (all in `codegen/codegen.c`)

- **Struct layout** (`%struct.<A> = type {…}` builder): column array element count is
  `static_cap * field_total_elements(field->type)`, so a `char[32]` column in a pool of
  8 is `[256 x i8]`, not `[8 x i8]`.
- **`arche_insert_<A>` ABI** (param list + `write_fields`): for a `char[N]` column
  (`field_total_elements > 1` and base type `i8`), the param is a pointer and the body
  `llvm.memcpy`s `N` bytes from it into the row slot. Numeric array columns (`float[10]`)
  keep their legacy scalar element-0 init (a single value, single `store`).
- **Call site** (`insert(...)` arg emission): a `char[N]` column arg is annotated as a
  pointer to match the new ABI.
- **Shaped read** (`A.col[i]` / `A.col[i, j]`): both index operands are coerced to i64
  via `emit_index_i64` before the flat-index `mul`/`add`.

## Scope / not covered

- Pointer-init applies to `char[N]` (base `i8`) only. Initializing a numeric `T[N]`
  column from a whole-array pointer is unchanged (scalar element-0 init); no caller needs
  it yet.
- A **char[] column row read passed directly as a printf vararg** still mistypes to
  `i32` (e.g. `printf("%s", Node.seg[0])`). Workaround: bind to a local first
  (`r := Node.seg[0]; printf("%s", r)`), which carries the correct type. Tracked as a
  follow-up (vararg arg typing for inline shaped reads).

## Tests

- `tests/unit/language/arch/char_column.arche` — insert two string rows, loop-read by
  runtime index, `strlen`/`streq`.
- `tests/unit/language/arch/multi_char_columns.arche` — two `char[N]` columns round-trip
  (promoted out of `known_failures/`).
