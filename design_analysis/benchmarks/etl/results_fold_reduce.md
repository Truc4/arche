# Fold benchmark: `reduce(+, col)` vs hand-written scalar loop

The ETL **sum** task (task 4, `aggregate_region`) used to fold a column with a manual scalar loop:

```arche
total := 0.0;
for (m := 0; m < N; m += 1) { total = total + col[m]; }
```

It now uses the monoid fold:

```arche
total := reduce(+, col);
```

`+` is associative, which licenses reassociating the fold — `reduce` emits a `<4 x float>` SIMD
reduction (4 independent lanes + horizontal combine + scalar tail), whereas the scalar loop is a single
accumulator with a serial `fadd` dependency chain (latency-bound, *below* memory bandwidth).

## How this was measured

Isolated from the 100M-row CSV load (which otherwise dwarfs the fold): a column is filled in-program,
all pages faulted in by the broadcast init, then the *same* fold is timed both ways with `os.now_ms()`
(monotonic ms — **not** `os.os_now_sec`, see Bugs below). Reproduce:

```
arche build -o /tmp/fold design_analysis/benchmarks/etl/100m/single-thread/arche/fold_sum_bench.arche && /tmp/fold
```

Machine: single-threaded CPU, AVX2. Column = one `float` (f32) column. Times are a representative run.

| N (rows) | `reduce` (SIMD) | scalar loop | speedup | reduce sum | loop sum |
|---------:|----------------:|------------:|--------:|-----------:|---------:|
| 10,000,000  |  3 ms |  7 ms | **2.3×** | 1e7 (exact)   | 1e7 (exact)         |
| 50,000,000  | 13 ms | 36 ms | **2.8×** | 5e7 (exact)   | 1.67772e7 (2²⁴ ❌)  |
| 100,000,000 | 23 ms | 73 ms | **3.2×** | 6.71e7 (2²⁶)  | 1.67772e7 (2²⁴ ❌)  |

The speedup grows with N: as the column exceeds cache, the scalar loop is pinned by its `fadd` latency
chain while the 4-lane reduce approaches memory bandwidth.

### Bonus: `reduce` is also more *accurate*

f32 summation stops growing once the accumulator passes 2²⁴ (16,777,216) — adding 1.0 becomes a no-op.
The scalar loop has **one** accumulator, so it saturates at 2²⁴. `reduce` has **4** partial accumulators,
so it saturates ~4× higher (2²⁶), and at N=50M (each lane ≤ 12.5M < 2²⁴) it is **exact** (5e7) where the
loop is already wrong. More parallelism ⇒ faster *and* more numerically robust. (For a truly accurate
large sum, an f64 accumulator is the staged fix — see docs/OPEN_ITEMS.md.)

## Bugs found and fixed while doing this (real regression tests, no XFAIL)

1. **`os_now_sec` f32/double ABI mismatch.** `runtime/io.c` returned a C `double`, but the arche FFI
   decl is `float` (now f32) — the caller read the wrong register bytes, so timing came back as garbage
   (e.g. `-10842500s`). Fixed: `os_now_sec` now returns `float`. This is fallout from the f32
   unification ("a C shim taking arche `float` must use C `float`"). Test:
   `tests/unit/language/io/os_now_sec_abi.arche`. (For sub-second timing prefer `os.now_ms` anyway —
   f32 monotonic seconds is coarse; these benchmarks use `os.now_ms`.)
2. **`iN(floatval)` cast emitted bad IR.** `i32(f)` / `i64(f)` routed the float through the integer
   width path (sext/zext/trunc) instead of `fptosi`. Fixed in `codegen.c` (float arg → `fptosi`).
   Test: `tests/unit/language/types/float_to_int_cast.arche`.
3. **`float(iN)` cast emitted bad IR.** The mirror bug: `float(i64)` emitted `sitofp i32 <i64>` (hardcoded
   i32 source) → `i64 but expected i32`. Fixed in `codegen.c` (`sitofp` from the value's real width).
   Test: `tests/unit/language/types/int_to_float_cast.arche`. (Surfaced converting `os.now_ms` i64 → s.)

Full suite after the fixes: **767/767 green, 0 expected-failures.**

## Timing methodology — what's the benchmark vs the checksum

Each task reports `taskN_time` = **only its operation** (load + transform), via `os.now_ms`. The
per-task **checksum is verification, not the benchmark**: for the column-producing tasks (1 = derived
column, 3 = bucket) the checksum sum is computed **after** the timed region (untimed) — those tasks
benchmark the map, not a sum. For the aggregation tasks the result *is* both the work and the checksum:
task 4 (`reduce`) and task 5 (filtered pipeline → `reduce`) time the fold; task 2 times the count.
(The 10k throughput tier already did this — times the op, sums untimed.) So exactly the aggregation
tasks sum *as the benchmark*; tasks 1/3 sum only to check correctness.

## Benchmark layout (consolidated this change)

The old `benchmarks/transform/` + `benchmarks/etl/{arche,arche_scale,…}` split was merged into one tree
along **two orthogonal axes**: **execution mode** (single-thread / multicore / gpu) and **backend**
(arche, c, pandas, polars, …). Mode is the category; backend lives under it. The harness discovers a
backend by globbing under whatever mode dir holds it — categorization is in the *file tree*, not code:

```
etl/<size>/<mode>/<backend>/      # mode ∈ {single-thread, multicore, gpu}
  single-thread/ arche/ c/ pandas/
  multicore/     polars/ duckdb/ datafusion/
  gpu/                              # stub: future arche GPU reduce
```

- `10k` is the throughput config (each source self-loops N=216,000×, reports per-iter time).
- `100m` is single-pass; `1k` is the legacy arche-only dev tier (old output style, not harness-wired).
- One harness, `etl/compare.py --size {1k,10k,100m} --task N --engines …`.
- `multicore/` holds the multi-threaded baselines; a future arche multicore reduce belongs there too.
  `gpu/` is the stub for a future arche GPU tree-fold. Both unbuilt (arche is single-threaded, `@gpu`
  is maps-only) — the monoid makes them correct-by-construction when built; see the READMEs.

## Cross-language: arche vs a REAL C baseline

The C baseline now uses a **fast custom parser** (`c/fast_parse.h` — single forward pass, branchless
digit scan), not libc `strtod`/`strtol` (which are ~3× slower and were masking the real gap). 10M rows
single-pass, 5-rep min:

| impl | what's timed | min | per-row |
|------|------|----:|--------:|
| arche task_1 | load + map | 0.71s | ~71 ns |
| arche task_4 | load + map + `reduce` | 0.71s | ~71 ns |
| real-C task_1 / task_4 | mmap + fast parse + sum | 0.31s | ~31 ns |

A properly-optimized C is **~2.3× faster than arche**, and the gap is **entirely arche's CSV parser**
(scalar byte scan) vs C's fast parse — arche's column compute and `reduce` are vectorized and
competitive (exactly the README's conclusion, now with a fair C, not the `strtod` strawman that made
arche look even/faster). **No arche regression**: arche's ~71 ns/row matches the README's ~73 ns/row.

Checksums match within tolerance (arche f32 vs C f64) — and task_4's `reduce` (`2.81129e10`) lands
*closer* to C's f64 sum (`2.81115e10`) than task_1's scalar loop (`2.81061e10`): the 4-lane fold is more
accurate.

**Takeaway:** arche's next single-thread perf lever is the **CSV parser** (a SIMD/custom numeric parse),
not the compute. The 100M CSV (~3.4 GB) is generated, not in-repo —
`python data/generate_data.py <rows> data/data_<size>.csv`.

## Multicore reduce: does it help? (no — the sum is bandwidth-bound)

After unifying the data-parallel ops under one IR + a pluggable backend interface (`ParOp` + `Backend`
in codegen.c — map/reduce/scan/sort all flow through it), adding a **`cores` backend** was ~40 lines of
codegen + one runtime function (`arche_par_reduce_*` in `runtime/io.c`: chunk the column → per-thread
fold → combine partials with the monoid). That cheapness *is* the payoff of the unification — a new
backend is "implement the interface once," not per-op. Reachable experimentally via
`ARCHE_REDUCE_CORES=1 arche build …` (a per-op `@cores` annotation is the eventual surface).

But it is **not faster** for a pure column sum. 100M f32 `reduce(+, col)`, this machine (12 cores):

| backend | 100M f32 sum | result |
|---|---:|---|
| AUTO (single-thread SIMD `<4×f32>`) | ~26–32 ms | `6.71e7` (2²⁶, saturated) |
| CORES (multicore) | ~30 ms | `1e8` (**exact** — 12 chunk-accumulators) |

A wash on time. The sum reads 400 MB and does ~1 add/element → it's **memory-bandwidth-bound**, and the
single-thread SIMD reduce *already* saturates bandwidth (~14 GB/s here), so more threads add nothing
(thread-spawn overhead even nudges it slower). Multicore only wins when the kernel is **compute-bound** —
real arithmetic per element (a fused map→reduce), or working sets that fit per-core caches. (Side note:
cores is *more accurate* — each ~8M-element chunk stays under f32's 2²⁴ saturation, so the combined sum
is exact, beating even the 4-lane SIMD's 2²⁶.)

**Conclusion:** the earlier "monoids → parallel → faster" intuition is false for bandwidth-bound folds.
The real multicore lever is **fusion** (next): fold an elementwise expression — `reduce(+, price*qty)` —
in one pass without materializing the intermediate column, turning a bandwidth-bound sum into a
compute-bound kernel that *both* SIMD and cores accelerate (and eliminating the column the current ETL
task 4 writes only to immediately sum).
