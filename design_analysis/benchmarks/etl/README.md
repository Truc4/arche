# Benchmark: ETL (Extract, Transform, Load)

Compare Arche against Pandas/NumPy on typical ETL workloads.

## Structure

```
etl/
├── data/
│   └── generate_data.py
├── pandas/
│   ├── task_1_derived_columns.py
│   ├── task_2_filter_invalid.py
│   ├── task_3_bucket_timestamps.py
│   └── task_4_aggregate_region.py
└── arche/
    ├── task_1_derived_columns.arche
    ├── task_2_filter_invalid.arche
    ├── task_3_bucket_timestamps.arche
    └── task_4_aggregate_region.arche
```

## Dataset

Generate CSV with millions of rows (timestamp, price, quantity, region, flags):

```bash
python3 data/generate_data.py 10000000 data.csv
```

## Running Benchmarks

### Pandas

```bash
python3 pandas/task_1_derived_columns.py data.csv
python3 pandas/task_2_filter_invalid.py data.csv
python3 pandas/task_3_bucket_timestamps.py data.csv
python3 pandas/task_4_aggregate_region.py data.csv
```

### Arche

See `.arche` files for design sketches and implementation notes.

## Tasks

1. **Task 1: Derived Columns** - Compute `revenue = price * quantity`
2. **Task 2: Filter Invalid** - Keep rows where `quantity > 0`
3. **Task 3: Bucket Timestamps** - Floor to hourly buckets
4. **Task 4: Aggregate Per Region** - Sum revenue grouped by region
