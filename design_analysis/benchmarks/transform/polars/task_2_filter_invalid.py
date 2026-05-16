#!/usr/bin/env python3
"""Task 2 (Polars, transform-only): count(quantity > 0), repeated N times."""

import os
import sys
import time
import polars as pl

DEFAULT_CSV = "design_analysis/benchmarks/etl/data/data_10k.csv"
N_ITERS = 216_000
OUT_DIR = "design_analysis/benchmarks/transform/out/polars"


def main(csv_path):
    df = pl.read_csv(csv_path, columns=["quantity"])

    count = 0
    start = time.perf_counter()
    for _ in range(N_ITERS):
        count += df.select((pl.col("quantity") > 0).sum()).item()
    elapsed = time.perf_counter() - start

    os.makedirs(OUT_DIR, exist_ok=True)
    pl.DataFrame({"count": [count]}).write_csv(f"{OUT_DIR}/task_2.csv")

    print(f"task2_checksum: {count}")
    print(f"task2_time: {elapsed / N_ITERS}")


if __name__ == "__main__":
    csv_path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_CSV
    main(csv_path)
