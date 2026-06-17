#!/usr/bin/env python3
"""Task 1 (Pandas, transform-only): revenue = price * quantity, repeated N times."""

import os
import sys
import time
import pandas as pd

DEFAULT_CSV = "design_analysis/benchmarks/etl/data/data_10k.csv"
N_ITERS = 216_000
OUT_DIR = "design_analysis/benchmarks/etl/out/pandas"


def main(csv_path):
    df = pd.read_csv(csv_path, usecols=["price", "quantity"])

    start = time.perf_counter()
    for _ in range(N_ITERS):
        df["revenue"] = df["price"] * df["quantity"]
    elapsed = time.perf_counter() - start

    checksum = df["revenue"].sum()

    os.makedirs(OUT_DIR, exist_ok=True)
    df[["revenue"]].to_csv(f"{OUT_DIR}/task_1.csv", index=False)

    print(f"task1_checksum: {checksum}")
    print(f"task1_time: {elapsed / N_ITERS}")


if __name__ == "__main__":
    csv_path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_CSV
    main(csv_path)
