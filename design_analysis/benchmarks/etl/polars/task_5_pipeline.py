#!/usr/bin/env python3
"""Task 5 (Polars): multi-step pipeline — filter q>0 AND price>10, then sum(price*quantity)."""

import sys
import time
import polars as pl

DEFAULT_CSV = "design_analysis/benchmarks/etl/data/data_100m.csv"


def main(csv_path):
    start = time.perf_counter()
    result = (
        pl.scan_csv(csv_path)
        .filter((pl.col("quantity") > 0) & (pl.col("price") > 10.0))
        .with_columns((pl.col("price") * pl.col("quantity")).alias("revenue"))
        .select(pl.col("revenue").sum())
        .collect()
    )
    elapsed = time.perf_counter() - start
    checksum = result.item()
    print(f"task5_checksum: {checksum}")
    print(f"task5_time: {elapsed}")


if __name__ == "__main__":
    csv_path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_CSV
    main(csv_path)
