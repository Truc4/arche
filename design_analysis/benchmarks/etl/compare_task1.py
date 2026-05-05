#!/usr/bin/env python3
"""Compare Arche task1 with Pandas for correctness and performance."""

import subprocess
import time
import pandas as pd
import csv
import sys
import os

def run_arche(iterations=10):
    """Run Arche task1 and measure time over multiple iterations."""
    # Create output directory
    os.makedirs('build/benchmarks/etl/task1', exist_ok=True)

    # Compile once
    start = time.perf_counter()
    result = subprocess.run(['./build/arche', '-o', 'build/benchmarks/etl/task1/task1_arche_bin',
                           'design_analysis/benchmarks/etl/arche/task_1_derived_columns.arche'],
                          capture_output=True, text=True)
    if result.returncode != 0:
        print("Arche compilation failed:")
        print(result.stderr)
        sys.exit(1)

    arche_compile_time = time.perf_counter() - start

    # Run multiple times
    times = []
    for i in range(iterations):
        start = time.perf_counter()
        result = subprocess.run(['build/benchmarks/etl/task1/task1_arche_bin'], capture_output=True, text=True)
        elapsed = time.perf_counter() - start
        times.append(elapsed)
        if result.returncode != 0:
            print("Arche execution failed:")
            print(result.stderr)
            sys.exit(1)

    # Read Arche output (from last run)
    arche_data = []
    with open('build/benchmarks/etl/task1/arche_output.csv') as f:
        reader = csv.reader(f)
        for row in reader:
            arche_data.append((float(row[0]), int(row[1]), float(row[2])))

    return arche_data, arche_compile_time, times

def run_pandas(iterations=10):
    """Run Pandas task1 and measure time over multiple iterations (including write)."""
    times = []
    for i in range(iterations):
        start = time.perf_counter()
        df = pd.read_csv('design_analysis/benchmarks/etl/data/data.csv')
        df['revenue'] = df['price'] * df['quantity']
        df_output = df[['price', 'quantity', 'revenue']].head(1000)
        df_output.to_csv('build/benchmarks/etl/task1/pandas_output.csv', index=False, header=False)
        elapsed = time.perf_counter() - start
        times.append(elapsed)

    pandas_data = [(row['price'], row['quantity'], row['revenue'])
                   for _, row in df_output.iterrows()]

    return pandas_data, times

def compare_data(arche_data, pandas_data):
    """Compare Arche and Pandas outputs."""
    if len(arche_data) != len(pandas_data):
        print(f"ERROR: Row count mismatch: Arche={len(arche_data)}, Pandas={len(pandas_data)}")
        return False

    tolerance = 1e-5
    matches = 0
    mismatches = []

    for i, (arche_row, pandas_row) in enumerate(zip(arche_data, pandas_data)):
        arche_price, arche_qty, arche_revenue = arche_row
        pandas_price, pandas_qty, pandas_revenue = pandas_row

        if (abs(arche_price - pandas_price) < tolerance and
            arche_qty == pandas_qty and
            abs(arche_revenue - pandas_revenue) < tolerance):
            matches += 1
        else:
            if len(mismatches) < 5:  # Show first 5 mismatches
                mismatches.append((i, arche_row, pandas_row))

    if matches == len(arche_data):
        print(f"✓ All {matches} rows match!")
        return True
    else:
        print(f"✗ Data mismatch: {matches}/{len(arche_data)} rows match")
        for i, arche_row, pandas_row in mismatches:
            print(f"  Row {i}: Arche={arche_row}, Pandas={pandas_row}")
        return False

def main():
    iterations = int(sys.argv[1]) if len(sys.argv) > 1 else 30
    print(f"Running Arche task1 ({iterations} iterations)...")
    arche_data, arche_compile, arche_times = run_arche(iterations)

    print(f"Running Pandas task1 ({iterations} iterations)...")
    pandas_data, pandas_times = run_pandas(iterations)

    print("\n=== Correctness ===")
    correct = compare_data(arche_data, pandas_data)

    print("\n=== Performance ===")
    arche_avg = sum(arche_times) / len(arche_times)
    arche_min = min(arche_times)
    arche_max = max(arche_times)

    pandas_avg = sum(pandas_times) / len(pandas_times)
    pandas_min = min(pandas_times)
    pandas_max = max(pandas_times)

    print(f"Arche runtime:  {arche_avg*1000:.3f}ms (min: {arche_min*1000:.3f}ms, max: {arche_max*1000:.3f}ms)")
    print(f"Pandas:         {pandas_avg*1000:.3f}ms (min: {pandas_min*1000:.3f}ms, max: {pandas_max*1000:.3f}ms)")
    print(f"(Arche compilation: {arche_compile:.4f}s, one-time cost)")

    speedup = pandas_avg / arche_avg
    print(f"\nSpeedup: {speedup:.2f}x (Arche is {speedup:.2f}x faster)")

    return 0 if correct else 1

if __name__ == '__main__':
    sys.exit(main())
