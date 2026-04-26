#!/usr/bin/env python3
"""Benchmark Task 2: Filter rows with invalid quantity (quantity > 0)."""

import pandas as pd
import time
import sys

def main(csv_file):
    print(f"Loading {csv_file}...")
    df = pd.read_csv(csv_file)
    print(f"Loaded {len(df)} rows\n")

    print("Task 2: Filter invalid rows (quantity > 0)")
    start = time.perf_counter()
    valid = df[df['quantity'] > 0]
    elapsed = time.perf_counter() - start

    print(f"  Time: {elapsed:.4f}s")
    print(f"  Total rows: {len(df)}")
    print(f"  Valid rows: {len(valid)}")
    print(f"  Invalid rows: {len(df) - len(valid)}")
    print(f"  Validity rate: {100 * len(valid) / len(df):.2f}%")

if __name__ == "__main__":
    csv_file = sys.argv[1] if len(sys.argv) > 1 else "data.csv"
    main(csv_file)
