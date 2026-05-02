#!/usr/bin/env python3
"""Benchmark Task 4: Aggregate revenue per region."""

import pandas as pd
import time
import sys

def main(csv_file):
    print(f"Loading {csv_file}...")
    df = pd.read_csv(csv_file)
    print(f"Loaded {len(df)} rows\n")

    print("Task 4: Aggregate revenue per region (sum revenue = price * quantity)")

    # Compute revenue and aggregate in one go
    start = time.perf_counter()
    df['revenue'] = df['price'] * df['quantity']
    result = df.groupby('region')['revenue'].sum()
    elapsed = time.perf_counter() - start

    print(f"  Time: {elapsed:.4f}s")
    print(f"  Rows processed: {len(df)}")
    print(f"  Regions: {len(result)}")
    print(f"\n  Revenue per region:")
    for region, revenue in result.items():
        print(f"    {region}: ${revenue:,.2f}")

if __name__ == "__main__":
    csv_file = sys.argv[1] if len(sys.argv) > 1 else "data.csv"
    main(csv_file)
