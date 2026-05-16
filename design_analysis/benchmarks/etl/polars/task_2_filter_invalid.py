#!/usr/bin/env python3
"""Task 2 (Polars): count rows where quantity > 0."""

import sys
import time
import polars as pl

DEFAULT_CSV = "design_analysis/benchmarks/etl/data/data_100m.csv"


def main(csv_path):
    start = time.perf_counter()
    result = (
        pl.scan_csv(csv_path)
        .filter(pl.col("quantity") > 0)
        .select(pl.len())
        .collect()
    )
    elapsed = time.perf_counter() - start
    checksum = result.item()
    print(f"task2_checksum: {checksum}")
    print(f"task2_time: {elapsed}")


if __name__ == "__main__":
    csv_path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_CSV
    main(csv_path)
