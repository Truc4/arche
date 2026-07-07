# Documented bugs (expected-RED tests)

Each `.arche` file here is a **minimal reproduction of a known compiler bug**, written to assert the
*correct* behaviour. Because the bug is still present, the test **fails (is red)** — that is intentional.
When a bug is fixed, its test turns green; delete it or relocate it into the live suite at that point.

These were found while writing the `extras/{vec,camera,physics}` devices and the `arche-wasm` scene demo.
Two of them (`eff_fan_whole_tuple_write_dropped`, `nested_inner_write_to_outer_scalar_dropped`) are
**silent no-ops** — a write that the compiler accepts and then drops, producing wrong results with no
diagnostic. Per the project's "honest-red over swept-green" stance these should become either working
writes or hard errors, never silent drops.

| test | bug |
| --- | --- |
| `eff_fan_whole_tuple_write_dropped.arche` | a whole-tuple column write (`pos = pos + vel`) inside an effectful `map … eff` fan is silently dropped (works in a pure `map`). |
| `nested_inner_write_to_outer_scalar_dropped.arche` | writing an *outer* fan's scalar column from an *inner* nested fan is silently dropped (writing an outer tuple *field* from inner works — so this is scalar-specific). |
| `runtime_tuple_literal_bad_ir.arche` | a runtime `return (a, b)` tuple literal lowers to invalid LLVM IR (`ptr` where a struct is expected), so the program fails to build. |

Not captured here (observed but not minimally reproducible): a stack overflow / wasm memory-OOB in a
`forever` reactor whose per-frame schedule nests a cross-device tuple-returning call under a fan — it did
not reproduce once reduced to a single system, so there is no reliable minimal case yet.
