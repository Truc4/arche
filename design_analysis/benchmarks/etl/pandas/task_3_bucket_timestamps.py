#!/usr/bin/env python3
"""Benchmark Task 3: Bucket timestamps into hourly buckets."""

import pandas as pd
import time
import sys

def main(csv_file):
    print(f"Loading {csv_file}...")
    df = pd.read_csv(csv_file, parse_dates=['timestamp'])
    print(f"Loaded {len(df)} rows\n")

    print("Task 3: Bucket timestamps into hourly buckets")
    start = time.perf_counter()
    df['hour_bucket'] = df['timestamp'].dt.floor('H')
    elapsed = time.perf_counter() - start

    print(f"  Time: {elapsed:.4f}s")
    print(f"  Rows processed: {len(df)}")
    print(f"  Unique hour buckets: {df['hour_bucket'].nunique()}")
    print(f"  Date range: {df['hour_bucket'].min()} to {df['hour_bucket'].max()}")

if __name__ == "__main__":
    csv_file = sys.argv[1] if len(sys.argv) > 1 else "data.csv"
    main(csv_file)
