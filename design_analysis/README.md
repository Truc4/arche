# Design Analysis

Performance and design decision validation for the Arche compiler and runtime.

## Array Operations

Benchmarks validating columnar (SoA) layout for performance-critical workloads.

### Physics Update (SIMD Workload)

**Benchmark**: `array_ops/physics_update.c` — N-body particle update with position/velocity.

**Results** (10000 particles, 1000 iterations):

| Layout | Time | Speed | Notes |
|--------|------|-------|-------|
| **AoS** (baseline) | 8.21 µs/op | 1.0x | Interleaved x,y,z,vx,vy,vz per particle |
| **SoA flat** | 1.67 µs/op | **4.9x** | Separate arrays for position, velocity |
| **SoA+AVX2** | 1.59 µs/op | **5.2x** | SoA with explicit SIMD vectorization |
| SoA nested | 1.68 µs/op | 4.9x | SoA grouped [pos[3], vel[3]] |
| AoSoA batch | 1.60 µs/op | 5.1x | Hybrid: batches of 64 |
| Packed (manual) | 4.57 µs/op | 1.8x | Manual flat indexing without SoA |

**Why Columnar (SoA)?**
- **CPU cache efficiency**: Each kernel accesses one column. With positions: 3×float×10000 = 120KB fits in L3 cache. AoS needs 6×float×10000 = 240KB (half in cache).
- **Vectorization**: SIMD units work on 4-8 floats at once. SoA layout is naturally aligned for `vmulps, vaddps` on consecutive memory. AoS requires shuffle/transpose.
- **Predictable latency**: No data-dependent branches on per-entity state. Vectorization is automatic and consistent.

### Lifecycle Operations (Allocation/Deallocation)

**Benchmark**: `array_ops/lifecycle_operations.c` — Add/remove/modify entities over time.

**Results** (500 adds + 400 deletes + 200 modifies per iteration, 100 iterations):

| Layout | Time | Entities | Memory | Notes |
|--------|------|----------|--------|-------|
| **AoS** | 17.24 µs/op | 23,338 | 1.09 MB | Varied fragmentation |
| **SoA** | 14.76 µs/op | 8,242 | 0.55 MB | **14.6% faster, 50% less memory** |

**Fragmentation stress** (2x deletes):
- AoS: 7.14 µs/op
- **SoA: 6.93 µs/op** (2.9% faster)

**Why Columnar for Lifecycle?**
- Deletion leaves sparse gaps in columns, but iteration skips them naturally without extra indirection
- Memory usage halved: single entity slot doesn't need 6 fields in nearby cache lines
- Compaction is trivial: dense each column independently

### Mixed Workload (95% compute + 5% lifecycle)

**Benchmark**: `array_ops/mixed_workload.c` — Realistic: mostly math, occasional entity changes.

**Results** (10,000 entities, 1000 iterations):

| Layout | Time | Speedup |
|--------|------|---------|
| AoS | 5.81 ms | 1.0x |
| **SoA** | 2.20 ms | **2.6x (62% faster)** |

**Conclusion**: Columnar layout wins on realistic workloads. Compute dominates, and cache efficiency on compute operations pays for any lifecycle overhead.

## String Operations

Fixed-length string handling with 2D char arrays for predictable access patterns.

### Design Choice: 2D char Arrays

**Arche approach**: `char[N][M]` — Array of fixed-length strings (N strings, each M bytes max).

**Why this over alternatives?**

| Approach | Trade-off |
|----------|-----------|
| **char[N][M]** ✓ | Columnar, predictable bounds, no pointers, vectorizable |
| `char[][]` (jagged) | Variable length, requires indirection (pointers) |
| `char*[N]` (string pointers) | Pointers forbidden in Arche; requires heap |
| Single flat buffer with indices | Manual offset tracking, error-prone |

**Benefits**:
- **No pointers**: Each string at `strings[i]` is always `strings[i][0..M-1]`, bounds-checked at compile time
- **Vectorization**: String operations work on contiguous memory (suitable for memcpy, strcmp SIMD kernels)
- **Fixed allocation**: No dynamic resizing; predictable memory layout for bounded systems
- **Columnar iteration**: Processing all strings in parallel is natural: `for (i=0; i<N; i++) process(strings[i])`

**Example**: Task1 uses `char[32]` for price/quantity string buffers. No malloc, no pointer dereference, bounds guaranteed.

Run benchmarks with `make bench-physics`, `make bench-lifecycle`, `make bench-mixed`.

## Data Generation

Test datasets for benchmarks:

- `benchmarks/etl/data/generate_data.py` — Generate CSV with 1M rows (configurable)
  - Columns: timestamp, price (1.0-1000.0), quantity (-100 to 10000), region, flags
  - Usage: `python3 generate_data.py <rows> <output_file>`
  - Default: `python3 generate_data.py 1000 data.csv`

## Design Decisions Validated

1. **Bounded arrays over dynamic allocation**: Safety without overhead (Task1 validates predictable latency)
2. **Vectorized operations**: Column-wise computation matches hand-optimized loops
3. **C stdlib interop via bounded handle table**: Portable file I/O without platform-specific flags (O_CREAT)
4. **Manual CSV parsing**: No string library overhead, explicit bounds

## ETL Benchmarks (100M rows)

Real-world data processing tasks at scale. All tasks read a 3.4 GB CSV file (100M rows, columns: timestamp, price, quantity, region, flags) using mmap-based I/O, load into columnar archetypes, run a compute pass, and print a checksum.

**Dataset**: `benchmarks/etl/data/data_100m.csv` — 100,000,000 rows, ~35 bytes/row, 3.4 GB.

**Arche implementation**: mmap + `use csv;` module, columnar static archetypes, vectorized column ops.

**Pandas implementation**: `pd.read_csv()` + vectorized DataFrame operations.

### Results

| Task | Operation | Arche | Pandas | Speedup |
|------|-----------|-------|--------|---------|
| Task 1 | `revenue = price × quantity` (load + compute + sum) | 7.1s | 28.8s | **4.1x** |
| Task 2 | count rows where `quantity > 0` | 2.9s | 29.2s | **10.1x** |
| Task 3 | `price_bucket = price / 10`, extract hour from timestamp | 6.7s | 35.9s | **5.4x** |
| Task 4 | `Σ(price × quantity)` across all rows | 7.2s | 31.6s | **4.4x** |

Machine: Linux x86-64, file hot in page cache.

### Task Details

**Task 1 — Derived columns** (`arche_scale/task_1_derived_columns.arche`):
- Loads price (float) and quantity (int) for 100M rows via mmap
- Computes `Transaction.revenue = Transaction.price * Transaction.quantity` (vectorized SIMD column op)
- Sums all revenue values; prints checksum

**Task 2 — Filter invalid rows** (`arche_scale/task_2_filter_invalid.arche`):
- Loads quantity only (skips price field parsing entirely — only scans to second comma)
- Counts rows where `quantity > 0`
- 10x speedup partly because Pandas parses all 5 columns while Arche stops at column 2

**Task 3 — Bucket timestamps** (`arche_scale/task_3_bucket_timestamps.arche`):
- Extracts hour from timestamp via `csv_mmap_parse_int(mm, pos + 11)` — no temp buffer, no string copy
- Computes `price_bucket = price / 10` (vectorized)
- Pandas extra cost: timestamp column parsed as datetime strings

**Task 4 — Aggregate revenue** (`arche_scale/task_4_aggregate_region.arche`):
- Same load as Task 1, same vectorized multiply
- Sums per-region revenue; prints total checksum

### I/O Strategy

Arche uses mmap via a thin `csv.arche` module:

```arche
use csv;

proc load_transactions() {
  let mm := csv_mmap_open("data_100m.csv");
  let size := csv_mmap_size(mm);
  let pos := csv_mmap_skip_header(mm, size);
  let idx := 0;
  for (;idx < 100000000;) {
    let nl := csv_mmap_next_line(mm, pos, size);
    let c1 := csv_mmap_find_comma(mm, pos, nl);
    let c2 := csv_mmap_find_comma(mm, c1 + 1, nl);
    Transaction.price[idx] = csv_mmap_parse_float(mm, c1 + 1);
    Transaction.quantity[idx] = csv_mmap_parse_int(mm, c2 + 1);
    pos = nl + 1;
    idx = idx + 1;
  }
  csv_mmap_close(mm);
}
```

No temp buffers, no per-line syscalls, no string copies. `memchr` locates delimiters; `strtod`/`strtol` parse in-place from mapped memory.

### Previous (1000-row) Results

Earlier benchmarks on 1000-row datasets showed 1.5–2.1x speedups. Those numbers are not meaningful — 1000 rows is dominated by Python interpreter startup and pandas import overhead, not actual CSV parsing or compute performance. The 100M row results above are more representative.

## Implementation Complexity: Python vs Arche

With a CSV loading library, implementation complexity comparison (assuming simplified Arche CSV API):

### Task1: Derived Columns (revenue = price × quantity)

**Python**:
```python
df = pd.read_csv('data.csv')
df['revenue'] = df['price'] * df['quantity']
df[['price', 'quantity', 'revenue']].to_csv('output.csv')
```

**Arche** (with CSV library):
```arche
let txns = csv_load(Transaction, "data.csv");
txns.revenue = txns.price * txns.quantity;  // Vectorized
csv_write(txns, "output.csv");
```

**Complexity delta**: Identical. Arche supports column-wise operations natively on archetypes.

### Task2: Filtering (valid = quantity > 0)

**Python**:
```python
df['valid'] = (df['quantity'] > 0).astype(int)
```

**Arche** (with CSV library):
```arche
txns.valid = txns.quantity > 0;  // Vectorized: comparison broadcasts to all rows
```

**Complexity delta**: Identical. Arche supports vectorized comparisons on columns.

### Task3: Bucketing (price_bucket = price / 10)

**Python**:
```python
df['price_bucket'] = (df['price'] / 10).astype(int)
```

**Arche** (with CSV library):
```arche
txns.price_bucket = txns.price / 10.0;  // Vectorized
```

**Complexity delta**: Identical. Arche type safety (explicit float storage) built in.

### Task4: Aggregate (total = sum of price × quantity)

**Python**:
```python
df = pd.read_csv('data.csv')
df['revenue'] = df['price'] * df['quantity']
total = df['revenue'].sum()
```

**Arche** (with CSV library):
```arche
let txns = csv_load(Transaction, "data.csv");
txns.revenue = txns.price * txns.quantity;  // Vectorized column op
let total: float = 0.0;
let i = 0;
for (;i < txns.count;) {
  total = total + txns.revenue[i];  // Direct accumulation from column
  i = i + 1;
}
```

**Complexity delta**: Loop needed for accumulation. Python aggregation is one-liner; Arche requires explicit loop. Trade-off: loop verbosity for compile-time type safety and predictable latency.

### Summary: Complexity Trade-offs

| Aspect | Python | Arche |
|--------|--------|-------|
| **Terseness** | 1-3 lines per operation | 1-3 lines (vectorized on archetypes) |
| **Type safety** | Implicit, deferred errors | Explicit, compile-time errors |
| **Memory layout** | Hidden (row-major by default) | Explicit (columnar archetypes) |
| **Performance predictability** | High variance (GC, broadcasting) | Low variance (compiled, bounded) |
| **Debugging data issues** | Runtime discovery | Compile-time bounds checking |
| **Learning curve** | Shallow (functional API) | Moderate (archetype syntax, column ops) |

**With CSV library**: Arche expressiveness matches Python for vectorized operations (Tasks 1–3). Aggregation (Task 4) requires explicit loop boilerplate. Gain: compile-time safety, predictable latency (1.08–1.69x faster), columnar layout benefits (SIMD, cache efficiency).

## Benchmark Fairness Disclaimer

These results need more validation before drawing strong conclusions. Addressed and remaining items:

**Addressed:**
- ✅ **Compiled baselines**: Polars, DuckDB, DataFusion, and a hand-written C baseline (mmap + manual parse + accumulate) all run via `compare_scale.py`. See `polars/`, `duckdb/`, `datafusion/`, and `c_baseline/`.
- ✅ **Selective column loading (Task 2)**: All `pandas/task_*.py` scripts now use `usecols` to read only the fields they need. Removes the apples-to-oranges effect where pandas was parsing 5 columns vs. arche's 2.
- ✅ **Multi-run variance**: `compare_scale.py` defaults to 10 iterations and reports min / median / max for both wall time and internal (parse+compute) time.
- ✅ **Cold-cache mode**: `compare_scale.py --cold-cache` drops the OS page cache before each iteration (Linux only; requires NOPASSWD sudo for `/sbin/sysctl vm.drop_caches=3` — see runner module docstring).
- ✅ **Multi-step pipelines**: Task 5 (`pipeline`) implements a filter→compute→reduce pipeline (`quantity > 0 AND price > 10`, then `sum(price*quantity)`) across all engines.
- ✅ **Internal vs wall time**: Runner reports both. `internal` excludes Python interpreter startup and library import; `wall` includes them. Apples-to-apples comparison for compiled engines uses `internal`.

**Still open:**
- **No multi-core parity**: Polars, DuckDB, and DataFusion default to multi-threaded CSV scan and compute. Pandas and Arche are single-threaded. The "compiled engines vs pandas" gap therefore conflates SIMD/compiled wins with parallelism wins. Equalizing would require Dask/modin for pandas (separate dep) or restricting the others' thread counts.
- **Real-world data**: A `data/generate_data_dirty.py` variant exists (quoted regions with embedded commas, occasional null `quantity`, mixed CRLF/LF line endings, irregular numeric widths). Not used by the default benchmark; generate explicitly and pass `--csv` to test parser robustness. The current 100M numbers all use the clean generator.
- **Joins**: Task 5 covers filter→aggregate but not joins. Adding a small dimension table (e.g. region metadata) for a star-schema join would round out the pipeline coverage.

## Future Benchmarks

Remaining ideas (cheaper-first):

1. **Multi-thread variants**: Add Dask version of pandas; run polars/duckdb/datafusion with `--threads=1` to isolate compiled-vs-pandas single-thread speedup from the parallelism dividend.
2. **Star-schema join**: Joining region dimension data to fact-table revenue. Forces engines to actually plan a join, not just scan.
3. **Dirty-data parity**: Verify each engine's behavior on the dirty generator (skip rows? error? coerce?) and adjust the runner to handle expected-different checksums.
4. **Cold-vs-hot comparison table**: Run the suite both with and without `--cold-cache` and show both columns side-by-side — that's where I/O cost actually shows up vs. parse+compute.
5. **Cross-machine reference run**: The README's headline Arche numbers (Linux x86-64) and the current WSL numbers (i5-4570 reading from `/mnt/c` 9p mount) aren't directly comparable. A single-machine native-Linux run would give the canonical headline numbers.
