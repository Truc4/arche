#!/usr/bin/env python3
"""Task 4 (Pandas): sum(price * quantity). Matches 100m/arche/task_4 checksum semantics."""

import sys
import time
import pandas as pd

DEFAULT_CSV = "design_analysis/benchmarks/etl/data/data_100m.csv"


def main(csv_path):
    start = time.perf_counter()
    df = pd.read_csv(csv_path, usecols=["price", "quantity"])
    df["revenue"] = df["price"] * df["quantity"]
    checksum = df["revenue"].sum()
    elapsed = time.perf_counter() - start
    print(f"task4_checksum: {checksum}")
    print(f"task4_time: {elapsed}")


if __name__ == "__main__":
    csv_path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_CSV
    main(csv_path)
