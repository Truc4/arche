#!/usr/bin/env python3
"""Task 2 (Pandas): count rows where quantity > 0."""

import sys
import time
import pandas as pd

DEFAULT_CSV = "design_analysis/benchmarks/etl/data/data_100m.csv"


def main(csv_path):
    start = time.perf_counter()
    df = pd.read_csv(csv_path, usecols=["quantity"])
    checksum = int((df["quantity"] > 0).sum())
    elapsed = time.perf_counter() - start
    print(f"task2_checksum: {checksum}")
    print(f"task2_time: {elapsed}")


if __name__ == "__main__":
    csv_path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_CSV
    main(csv_path)
