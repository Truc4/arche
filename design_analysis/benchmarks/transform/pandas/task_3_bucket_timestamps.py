#!/usr/bin/env python3
"""Task 3 (Pandas, transform-only): price_bucket = price / 10.0, repeated N times."""

import os
import sys
import time
import pandas as pd

DEFAULT_CSV = "design_analysis/benchmarks/etl/data/data_10k.csv"
N_ITERS = 216_000
OUT_DIR = "design_analysis/benchmarks/transform/out/pandas"


def main(csv_path):
    df = pd.read_csv(csv_path, usecols=["price"])

    start = time.perf_counter()
    for _ in range(N_ITERS):
        df["price_bucket"] = df["price"] / 10.0
    elapsed = time.perf_counter() - start

    checksum = df["price_bucket"].sum()

    os.makedirs(OUT_DIR, exist_ok=True)
    df[["price_bucket"]].to_csv(f"{OUT_DIR}/task_3.csv", index=False)

    print(f"task3_checksum: {checksum}")
    print(f"task3_time: {elapsed / N_ITERS}")


if __name__ == "__main__":
    csv_path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_CSV
    main(csv_path)
