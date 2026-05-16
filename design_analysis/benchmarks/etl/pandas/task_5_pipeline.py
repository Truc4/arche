#!/usr/bin/env python3
"""Task 5 (Pandas): multi-step pipeline — filter q>0 AND price>10, then sum(price*quantity)."""

import sys
import time
import pandas as pd

DEFAULT_CSV = "design_analysis/benchmarks/etl/data/data_100m.csv"


def main(csv_path):
    start = time.perf_counter()
    df = pd.read_csv(csv_path, usecols=["price", "quantity"])
    mask = (df["quantity"] > 0) & (df["price"] > 10.0)
    filtered = df[mask]
    revenue = filtered["price"] * filtered["quantity"]
    checksum = revenue.sum()
    elapsed = time.perf_counter() - start
    print(f"task5_checksum: {checksum}")
    print(f"task5_time: {elapsed}")


if __name__ == "__main__":
    csv_path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_CSV
    main(csv_path)
