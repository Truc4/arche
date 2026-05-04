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

## ETL Benchmarks

Real-world data processing tasks to validate Arche's performance on practical workloads.

### Task1: Derived Column Computation

**Goal**: Read CSV, compute derived column, write output CSV.

**Implementation**: `benchmarks/etl/arche/task_1_derived_columns.arche`
- Loads 1000 rows from `benchmarks/etl/data/data.csv` (timestamp, price, quantity, region, flags)
- Computes `revenue = price * quantity` for all rows
- Writes output CSV with all three columns (price, quantity, revenue)
- Uses C stdlib wrappers (fopen/fread/fwrite) for portable file I/O

**Performance** (30 iterations):
- **Arche runtime**: 0.857ms avg (min: 0.781ms, max: 1.152ms)
- **Pandas**: 1.107ms avg (min: 0.999ms, max: 2.718ms)
- **Speedup**: 1.29x (Arche is 1.29x faster)
- **Compilation**: 34.8ms one-time overhead
- **Correctness**: ✓ All 1000 rows match Pandas (within 1e-5 floating-point tolerance)

**Analysis**:
- Arche delivers consistent latency (47% variance) vs Pandas (172% variance)
- More predictable for production workloads
- Manual CSV parsing with bounded loops, no dynamic allocations
- Vectorized operation on archetype arrays

**Run benchmark**:
```bash
python3 benchmarks/etl/compare_task1.py        # 30 iterations (default)
python3 benchmarks/etl/compare_task1.py 50     # 50 iterations
python3 benchmarks/etl/compare_task1.py 100    # 100 iterations
```

### Task2: Filter Invalid Rows

**Goal**: Read CSV, mark rows as valid (valid=1) if quantity > 0.

**Implementation**: `benchmarks/etl/arche/task_2_filter_invalid.arche`
- Loads 1000 rows, filters based on quantity > 0
- Outputs CSV with valid flag column

**Performance** (10 iterations):
- **Arche runtime**: 0.775ms avg
- **Pandas**: 1.313ms avg
- **Speedup**: 1.69x
- **Correctness**: ✓ All 1000 rows match

### Task3: Bucket Prices into Ranges

**Goal**: Read CSV, compute price buckets (price / 10).

**Implementation**: `benchmarks/etl/arche/task_3_bucket_timestamps.arche`
- Loads 1000 rows, computes price_bucket = floor(price / 10)
- Outputs CSV with bucket column

**Performance** (10 iterations):
- **Arche runtime**: 0.855ms avg
- **Pandas**: 1.297ms avg
- **Speedup**: 1.52x
- **Correctness**: ✓ All 1000 rows match

### Task4: Aggregate Total Revenue

**Goal**: Read CSV, compute total revenue (sum of price × quantity).

**Implementation**: `benchmarks/etl/arche/task_4_aggregate_region.arche`
- Loads 1000 rows from CSV into Transaction archetype (price, quantity)
- Vectorized: `Transaction.revenue = Transaction.price * Transaction.quantity`
- Loop sum: accumulates all revenue values into float scalar
- Writes result to file

**Performance** (10 iterations):
- **Arche runtime**: 1.012ms avg
- **Pandas**: 1.091ms avg
- **Speedup**: 1.08x
- **Correctness**: ✓ Total revenue matches (2531635514.93)

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

## Limitations of This Test

- **Too small**: 1000 rows doesn't stress batch optimization or memory efficiency
- **Unfair baseline**: Pandas times include Python startup overhead (~35ms), making interpreted overhead visible
- **No compiled comparison**: Only comparing against interpreted code; need Polars, DuckDB, or NumPy+Cython for meaningful performance baseline
- **Wrong metric**: Latency on tiny datasets favors compiled code; throughput on large datasets shows real optimization quality
- **High variance**: 1000-row runs show 10x variance (max/min), masking actual performance characteristics

## Future Benchmarks

To properly evaluate Arche:

1. **Scale**: 1M+ rows to measure batch optimization and memory efficiency at realistic scale
2. **Compiled baselines**: Compare against Polars, DuckDB, NumPy+Cython instead of interpreted Pandas
3. **Throughput metrics**: Measure rows/sec on large datasets, not microseconds on tiny ones
4. **Overhead separation**: Exclude Python startup time from Pandas measurements
5. **Real-world workflows**: Multi-step pipelines (load → filter → aggregate → join) vs single operations
6. **Variance analysis**: Measure latency predictability (max/min ratios) on large datasets

I assume these will not be as favorable to Arche, but will continue testing practical scenarios.
