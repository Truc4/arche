#!/usr/bin/env python3
"""Task 3 (Polars): sum(price / 10.0). Matches 100m/arche/task_3 checksum semantics."""

import sys
import time
import polars as pl

DEFAULT_CSV = "design_analysis/benchmarks/etl/data/data_100m.csv"


def main(csv_path):
    start = time.perf_counter()
    result = (
        pl.scan_csv(csv_path)
        .with_columns((pl.col("price") / 10.0).alias("price_bucket"))
        .select(pl.col("price_bucket").sum())
        .collect()
    )
    elapsed = time.perf_counter() - start
    checksum = result.item()
    print(f"task3_checksum: {checksum}")
    print(f"task3_time: {elapsed}")


if __name__ == "__main__":
    csv_path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_CSV
    main(csv_path)
