#!/usr/bin/env python3
"""Compare Arche task3 (bucket prices) with Pandas."""

import subprocess
import time
import pandas as pd
import csv
import sys
import os

def run_arche(iterations=10):
    os.makedirs('build/benchmarks/etl/task3', exist_ok=True)
    start = time.perf_counter()
    result = subprocess.run(['./build/arche', '-o', 'build/benchmarks/etl/task3/task3_arche_bin',
                           'design_analysis/benchmarks/etl/arche/task_3_bucket_timestamps.arche'],
                          capture_output=True, text=True)
    if result.returncode != 0:
        print("Arche compilation failed:", result.stderr)
        sys.exit(1)
    arche_compile_time = time.perf_counter() - start

    times = []
    for i in range(iterations):
        start = time.perf_counter()
        result = subprocess.run(['build/benchmarks/etl/task3/task3_arche_bin'], capture_output=True, text=True)
        elapsed = time.perf_counter() - start
        times.append(elapsed)
        if result.returncode != 0:
            print("Arche execution failed:", result.stderr)
            sys.exit(1)

    arche_data = []
    with open('build/benchmarks/etl/task3/arche_output.csv') as f:
        reader = csv.reader(f)
        for row in reader:
            arche_data.append((float(row[0]), int(row[1]), float(row[2])))

    return arche_data, arche_compile_time, times

def run_pandas(iterations=10):
    times = []
    for i in range(iterations):
        start = time.perf_counter()
        df = pd.read_csv('design_analysis/benchmarks/etl/data/data.csv')
        df['price_bucket'] = df['price'] / 10.0
        df_output = df[['price', 'quantity', 'price_bucket']].head(1000)
        elapsed = time.perf_counter() - start
        times.append(elapsed)

    pandas_data = [(row['price'], row['quantity'], float(row['price_bucket']))
                   for _, row in df_output.iterrows()]

    os.makedirs('build/benchmarks/etl/task3', exist_ok=True)
    df_output.to_csv('build/benchmarks/etl/task3/pandas_output.csv', index=False, header=False)

    return pandas_data, times

def compare_data(arche_data, pandas_data):
    if len(arche_data) != len(pandas_data):
        print(f"ERROR: Row count mismatch: Arche={len(arche_data)}, Pandas={len(pandas_data)}")
        return False

    tolerance = 1e-5
    matches = 0
    mismatches = []

    for i, (arche_row, pandas_row) in enumerate(zip(arche_data, pandas_data)):
        arche_price, arche_qty, arche_bucket = arche_row
        pandas_price, pandas_qty, pandas_bucket = pandas_row

        if (abs(arche_price - pandas_price) < tolerance and
            arche_qty == pandas_qty and
            abs(arche_bucket - pandas_bucket) < tolerance):
            matches += 1
        else:
            if len(mismatches) < 5:
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
    iterations = int(sys.argv[1]) if len(sys.argv) > 1 else 10
    print(f"Running Arche task3 ({iterations} iterations)...")
    arche_data, arche_compile, arche_times = run_arche(iterations)

    print(f"Running Pandas task3 ({iterations} iterations)...")
    pandas_data, pandas_times = run_pandas(iterations)

    print("\n=== Correctness ===")
    correct = compare_data(arche_data, pandas_data)

    print("\n=== Performance ===")
    arche_avg = sum(arche_times) / len(arche_times)
    pandas_avg = sum(pandas_times) / len(pandas_times)
    print(f"Arche runtime:  {arche_avg*1000:.3f}ms (min: {min(arche_times)*1000:.3f}ms, max: {max(arche_times)*1000:.3f}ms)")
    print(f"Pandas:         {pandas_avg*1000:.3f}ms (min: {min(pandas_times)*1000:.3f}ms, max: {max(pandas_times)*1000:.3f}ms)")
    print(f"(Arche compilation: {arche_compile:.4f}s)")
    print(f"\nSpeedup: {pandas_avg/arche_avg:.2f}x")

    return 0 if correct else 1

if __name__ == '__main__':
    sys.exit(main())
