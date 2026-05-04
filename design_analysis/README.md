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
