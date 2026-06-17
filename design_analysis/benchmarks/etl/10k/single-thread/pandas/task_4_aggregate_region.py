#!/usr/bin/env python3
"""Task 4 (Pandas, transform-only): sum(price * quantity) scalar, repeated N times."""

import os
import sys
import time
import pandas as pd

DEFAULT_CSV = "design_analysis/benchmarks/etl/data/data_10k.csv"
N_ITERS = 216_000
OUT_DIR = "design_analysis/benchmarks/etl/out/pandas"


def main(csv_path):
    df = pd.read_csv(csv_path, usecols=["price", "quantity"])

    total = 0.0
    start = time.perf_counter()
    for _ in range(N_ITERS):
        total += (df["price"] * df["quantity"]).sum()
    elapsed = time.perf_counter() - start

    os.makedirs(OUT_DIR, exist_ok=True)
    pd.DataFrame({"total": [total]}).to_csv(f"{OUT_DIR}/task_4.csv", index=False)

    print(f"task4_checksum: {total}")
    print(f"task4_time: {elapsed / N_ITERS}")


if __name__ == "__main__":
    csv_path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_CSV
    main(csv_path)
