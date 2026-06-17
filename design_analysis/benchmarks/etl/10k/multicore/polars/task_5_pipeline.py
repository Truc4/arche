#!/usr/bin/env python3
"""Task 5 (Polars, transform-only): filter q>0 AND p>10, sum(p*q), repeated N times."""

import os
import sys
import time
import polars as pl

DEFAULT_CSV = "design_analysis/benchmarks/etl/data/data_10k.csv"
N_ITERS = 216_000
OUT_DIR = "design_analysis/benchmarks/etl/out/polars"


def main(csv_path):
    df = pl.read_csv(csv_path, columns=["price", "quantity"])

    total = 0.0
    start = time.perf_counter()
    for _ in range(N_ITERS):
        total += (
            df.filter((pl.col("quantity") > 0) & (pl.col("price") > 10.0))
              .select((pl.col("price") * pl.col("quantity")).sum())
              .item()
        )
    elapsed = time.perf_counter() - start

    os.makedirs(OUT_DIR, exist_ok=True)
    pl.DataFrame({"total": [total]}).write_csv(f"{OUT_DIR}/task_5.csv")

    print(f"task5_checksum: {total}")
    print(f"task5_time: {elapsed / N_ITERS}")


if __name__ == "__main__":
    csv_path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_CSV
    main(csv_path)
