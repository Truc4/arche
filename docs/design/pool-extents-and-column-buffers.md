# Pool extents and columns-as-buffers

Status: implemented. Standardizes the size vocabulary across sequences and pools, lets a pool column
back a `[]T` buffer, and adds two lints. Companion reference docs: `docs/language.md` (Archetypes,
Arrays and slices, Ownership), `docs/explain/W0026.md`, `docs/explain/W0027.md`.

## Problem

Two things were tangled:

1. **Extent vocabulary was inconsistent.** `.length` on a pool column meant *live row count*, but on
   an array/slice/string it meant *storage size* — the same word, two meanings. `.count` was accepted
   in bounds-guards but had no codegen (so `i < P.count` silently misbehaved), and `.capacity` only
   worked on an `archetype` *parameter*, not on a pool name or column.
2. **Pools couldn't back a buffer.** After global top-level arrays were removed, large scratch/IO
   buffers had been relocated onto the stack (flagged by `W0026 large_stack_array`). The intended home
   — a single-type pool — couldn't be passed where a `[]T` was expected: codegen rejected a pool
   column as a slice argument.

## The model

The extent a value exposes follows what the value *is*:

| Concept     | Spelling      | Meaning                                   | Iteration bound? |
| ----------- | ------------- | ----------------------------------------- | ---------------- |
| sequence    | `.length`     | element count of an array/slice/string    | yes              |
| pool        | `.count`      | live rows currently in the pool           | yes (`i < P.count`) |
| pool        | `.capacity`   | allocated slots `N` (fixed storage size)  | no               |

Key rule, and the reason `.length` ≠ `.capacity`: **`.length` is what you iterate.** Capacity is the
allocated storage; iterating to capacity would walk uninitialized slots. So a pool never borrows the
sequence word — it speaks `.count` (the live bound you loop over) and `.capacity` (the storage size).

Consequences:
- `.length` is rejected on a pool or a bare column (use `.count`/`.capacity`).
- `.count`/`.capacity` are rejected on a sequence.
- A **pool column is not itself a sequence.** `Foo.col` is a column, not a `[]T`; it has no `.length`.

## A column as a buffer

To use a single-field pool's column as a flat `[]T` buffer, **slice it** — the slice is what has a
length:

```
b :: char;
Buf :: arche { b }
[65536]Buf;

io.read_chunk(fd, Buf.b[0:Buf.capacity], 65536)(chunk:);   // []char over the pool's storage
```

`Buf.b[0:Buf.capacity]` is a `{ptr, len}` view (`ptr` = column base, `len = hi - lo`). Slicing is
required rather than a bare `Buf.b` precisely because the length must be stated: `.count` (often 0 for
a scratch pool) and `.capacity` are different numbers, and there is no single right default. A column
slice is valid up to capacity (its allocated storage), the established raw-column view.

## Ownership: a column slice is borrow-only

A column slice borrows the pool's shared, fixed storage. A pool is permanent static storage (no heap,
nothing freed), so there is **no ownership to transfer** — a column slice is only ever a borrow. The
borrowing IO/read APIs (`io.read_chunk`, `io.fread`: `buf: []char`) take it directly; the FFI writes
through the pointer exactly as it does for a stack array.

`move`-ing a column slice is therefore pointless: it transfers nothing, and (being an expression, not
a variable) it consumes nothing either — flagged by `W0027 pointless_move`. The consume-and-prevent-
reuse pattern still works via a *named local*: `buf := Buf.b[0:Buf.capacity]; f(move buf)` kills the
local `buf`, leaving the pool storage intact. (This is a lint, not an error, because the loose `move`
is benign — no use-after-free is possible against permanent storage.)

## Diagnostics added

- `W0026 large_stack_array` — a local `[N]T` ≥ 1 KB on the stack; prefer a pool (sliced column) or a
  `#module`-private global. Warn by default; `-Wno-large-stack-array` / `-Werror=large-stack-array`.
- `W0027 pointless_move` — `move` of a pool column (slice); drop it. Warn by default; `@allow(pointless_move)`.
- `.length` on a pool/column and `.count`/`.capacity` on a column are hard rejections (reuse `no_field`).

Both lint flags are accepted by `build`, `run`, and `check`.

## Implementation pointers

- Semantic (`semantic/semantic.c`): `is_pool_extent_prop`; accept `.count`/`.capacity` on archetype
  bases and reject `.length` (field validation); reject extent props on a bare column (nf==2 guard);
  `expr_is_pool_column_view` drives the `W0027` check in the `move`/unary path.
- Codegen (`codegen/codegen.c`): `.count`/`.capacity` field loads (count at struct index
  `field_count`, static capacity via `get_arch_static_capacity`); the `.length`-on-column path was
  removed; `codegen_slice` gained a pool-column base case producing a `{ptr,len}` slice (length =
  capacity), so it flows through the existing slice machinery — no new call-arg branch.

## Incidental fix

Markdown doctests silently ignored per-block fences (`arche,ignore` etc.) — `run_group` never
consulted the flags, unlike the `.arche` runner. `arche,ignore` is now honored (see `docs/DOCTESTS.md`).
