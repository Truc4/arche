# Codegen Bug: int-mask × float-column at proc level

## Symptom

Arche source like the following — directly from the "Conditional Behavior"
example in the top-level `README.md` — fails to compile when used as a
column-level expression at proc level:

```arche
Row.price = Row.price * (Row.quantity > 0);
```

The compiler reports a `llc` error:

```
llc: error: '%v49' defined with type 'i32' but expected '<4 x i32>'
  %v52 = sitofp <4 x i32> %v49 to <4 x double>
                          ^
Failed to compile LLVM IR to assembly
```

## Repro

`tests/unit/language/known_failures/codegen_int_mask_times_float_column.arche`
is the minimal failing test: two columns (one `float`, one `int`), single
column-level expression mixing them through an int comparison mask.

```arche
arche Row {
  price: float,
  quantity: int,
}

static Row(10, 10) {
  price: 5.0,
  quantity: 1,
};

proc main() {
  Row.price = Row.price * (Row.quantity > 0);  // <-- fails at llc
}
```

The mask is all-ones for this data, so the operation is a logical no-op —
the bug is purely in codegen, not in any runtime arithmetic.

## What's actually wrong

Codegen emits `<4 x double> = sitofp <4 x i32> %v49 to <4 x double>` but the
operand `%v49` is a scalar `i32`, not a 4-wide vector. The vectorizer is
applied to part of the expression chain but not to the integer comparison
result that feeds it.

Likely fix-shape (untested):

- Either broadcast the scalar `i32` to `<4 x i32>` before the `sitofp`
- Or skip vectorization on this branch and emit scalar `sitofp i32 to double`
  with a scalar-broadcast multiply
- Or vectorize the comparison itself so its result is already `<4 x i32>`

The third is closest to what the README documents and is the expected behavior.

## Where it works (and so the workaround)

The same pattern works inside a `sys` body — that codegen path is different.
The README's example uses a `sys`:

```arche
sys dampen(vel, pos) {
  vel = vel * (pos > 10);   // OK
}
```

So the workaround at proc level is one of:

1. Wrap the masked compute in a `sys` and `run` it. Most idiomatic.
2. Fall back to a scalar `if`-filter loop. Slower but compiles. This is
   what `design_analysis/benchmarks/etl/arche_scale/task_5_pipeline.arche`
   currently does — see the comment block in that file.

## Discovered

2026-05-10, while writing Task 5 (multi-step pipeline) for the ETL
benchmark suite. The natural form — `revenue = price * quantity * (quantity > 0) * (price > 10.0)`
— hit this bug, as did the split form (`revenue = revenue * (quantity > 0)`
as its own statement). Both produce identical `sitofp` IR errors.

## Test runs

The repro test lives under `tests/unit/language/known_failures/` and is
excluded from the lit suite by `tests/lit.cfg` (`config.excludes = ['archive',
'known_failures']`). To run it manually and observe the failure:

```
./build/arche -o /tmp/repro tests/unit/language/known_failures/codegen_int_mask_times_float_column.arche
```

Once the codegen fix lands, the test should compile and print `ok`. At that
point move it out of `known_failures/` so lit picks it up as a passing test.
