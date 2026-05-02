#!/usr/bin/env python3
"""Benchmark Task 1: Compute derived columns (revenue = price * quantity)."""

import pandas as pd
import time
import sys

def main(csv_file):
    print(f"Loading {csv_file}...")
    df = pd.read_csv(csv_file)
    print(f"Loaded {len(df)} rows\n")

    print("Task 1: Compute derived columns (revenue = price * quantity)")
    start = time.perf_counter()
    df['revenue'] = df['price'] * df['quantity']
    elapsed = time.perf_counter() - start

    print(f"  Time: {elapsed:.4f}s")
    print(f"  Rows processed: {len(df)}")
    print(f"  Sample revenue values:\n{df['revenue'].head()}")

if __name__ == "__main__":
    csv_file = sys.argv[1] if len(sys.argv) > 1 else "data.csv"
    main(csv_file)
