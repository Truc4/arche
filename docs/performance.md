# Performance

Arche compiles to LLVM IR and aims for **C-class speed with scripting-level brevity**.
Because both Arche and the C baseline lower through LLVM, the realistic target is *parity*
with `-O3` C, not beating it - the column math vectorizes the same way in both.

Full cross-engine numbers and a 100M-row end-to-end run live in
[`../design_analysis/README.md`](../design_analysis/README.md).

## Transform benchmark (single-threaded)

A 10k-row CSV is loaded once (off the clock), then the transform body is timed over 216,000
iterations. All engines produce identical checksums. Machine: AMD Ryzen 5 7600X, native
Linux, AVX2.

| Task | C (`-O3`) | Arche  | pandas | Arche vs C | Arche vs pandas |
| ---- | --------: | -----: | -----: | :--------: | :-------------: |
| `revenue = price × quantity` | 1.24µs | 1.17µs | 41.7µs | ~1.0× | 36× faster |
| count `quantity > 0`         | 0.44µs | 0.52µs | 28.6µs | 1.2×  | 55× faster |
| bucket timestamps            | 2.30µs | 2.35µs | 29.2µs | 1.02× | 12× faster |
| aggregate by region          | 6.22µs | 6.67µs | 46.3µs | 1.07× | 7× faster  |
| combined pipeline            | 6.36µs | 8.34µs | 181µs  | 1.31× | 22× faster |

**Verbosity** - lines to express a task end-to-end (load + compute + output):

| C | Arche | pandas |
| ----: | ----: | -----: |
| ~100 | ~38 | ~22 |

Arche runs **within ~1–1.3× of hand-written `-O3` C** while the code reads like a vectorized
script: the transform is a single line in both Arche and pandas
(`Transaction.revenue = Transaction.price * Transaction.quantity`), where C needs an explicit
element loop plus ~80 lines of manual CSV parsing.

A result at or just under 1.0× (like task 1) is **codegen noise, not a real speed advantage** -
both backends are LLVM, so the lane-for-lane outcome swings by a few percent depending on which
loop the vectorizer happens to widen. The honest claim is *on par with C*, roughly ⅓ the code,
and ~7–55× faster than single-threaded pandas.

> Single-threaded, AVX2. polars/duckdb are multi-threaded and win on *large* data but lose on
> this tight per-op loop - see the design analysis for the multi-engine breakdown.

## Where the speed comes from

Whole-column operations compile to hoisted, vectorizable loops (no per-element source loop). See
[Implicit loop codegen](language.md#implicit-loop-codegen) in the language reference for the
details (column base hoisting, vector loads/stores, single-load bounds metadata).
