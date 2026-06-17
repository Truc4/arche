#!/usr/bin/env python3
"""Task 1 (Polars, transform-only): revenue = price * quantity, repeated N times."""

import os
import sys
import time
import polars as pl

DEFAULT_CSV = "design_analysis/benchmarks/etl/data/data_10k.csv"
N_ITERS = 216_000
OUT_DIR = "design_analysis/benchmarks/etl/out/polars"


def main(csv_path):
    df = pl.read_csv(csv_path, columns=["price", "quantity"])

    start = time.perf_counter()
    for _ in range(N_ITERS):
        df = df.with_columns((pl.col("price") * pl.col("quantity")).alias("revenue"))
    elapsed = time.perf_counter() - start

    checksum = df.select(pl.col("revenue").sum()).item()

    os.makedirs(OUT_DIR, exist_ok=True)
    df.select("revenue").write_csv(f"{OUT_DIR}/task_1.csv")

    print(f"task1_checksum: {checksum}")
    print(f"task1_time: {elapsed / N_ITERS}")


if __name__ == "__main__":
    csv_path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_CSV
    main(csv_path)
