#!/usr/bin/env python3
"""Task 3 (Pandas): sum(price / 10.0). Matches 100m/arche/task_3 checksum semantics."""

import sys
import time
import pandas as pd

DEFAULT_CSV = "design_analysis/benchmarks/etl/data/data_100m.csv"


def main(csv_path):
    start = time.perf_counter()
    df = pd.read_csv(csv_path, usecols=["price"])
    df["price_bucket"] = df["price"] / 10.0
    checksum = df["price_bucket"].sum()
    elapsed = time.perf_counter() - start
    print(f"task3_checksum: {checksum}")
    print(f"task3_time: {elapsed}")


if __name__ == "__main__":
    csv_path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_CSV
    main(csv_path)
