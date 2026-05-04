#!/usr/bin/env python3
"""Compare Arche task4 (aggregate revenue) with Pandas."""

import subprocess
import time
import pandas as pd
import sys
import os
import re

def run_arche(iterations=10):
    os.makedirs('build/benchmarks/etl/task4', exist_ok=True)
    start = time.perf_counter()
    result = subprocess.run(['./build/arche', '-o', 'build/benchmarks/etl/task4/task4_arche_bin',
                           'design_analysis/benchmarks/etl/arche/task_4_aggregate_region.arche'],
                          capture_output=True, text=True)
    if result.returncode != 0:
        print("Arche compilation failed:", result.stderr)
        sys.exit(1)
    arche_compile_time = time.perf_counter() - start

    times = []
    total_revenues = []
    for i in range(iterations):
        start = time.perf_counter()
        result = subprocess.run(['build/benchmarks/etl/task4/task4_arche_bin'], capture_output=True, text=True)
        elapsed = time.perf_counter() - start
        times.append(elapsed)
        if result.returncode != 0:
            print("Arche execution failed:", result.stderr)
            sys.exit(1)

        match = re.search(r'total revenue = ([\d.]+)', result.stdout)
        if match:
            total_revenues.append(float(match.group(1)))

    return total_revenues[-1] if total_revenues else 0, arche_compile_time, times

def run_pandas(iterations=10):
    times = []
    total_revenues = []
    for i in range(iterations):
        start = time.perf_counter()
        df = pd.read_csv('design_analysis/benchmarks/etl/data/data.csv')
        df['revenue'] = df['price'] * df['quantity']
        total_rev = df['revenue'].head(1000).sum()
        elapsed = time.perf_counter() - start
        times.append(elapsed)
        total_revenues.append(total_rev)

    return total_revenues[-1], times

def main():
    iterations = int(sys.argv[1]) if len(sys.argv) > 1 else 10
    print(f"Running Arche task4 ({iterations} iterations)...")
    arche_revenue, arche_compile, arche_times = run_arche(iterations)

    print(f"Running Pandas task4 ({iterations} iterations)...")
    pandas_revenue, pandas_times = run_pandas(iterations)

    print("\n=== Correctness ===")
    tolerance = 0.01
    if abs(arche_revenue - pandas_revenue) < tolerance:
        print(f"✓ Total revenue matches!")
        print(f"  Arche:  {arche_revenue:.2f}")
        print(f"  Pandas: {pandas_revenue:.2f}")
        correct = True
    else:
        print(f"✗ Total revenue mismatch!")
        print(f"  Arche:  {arche_revenue:.2f}")
        print(f"  Pandas: {pandas_revenue:.2f}")
        print(f"  Diff:   {abs(arche_revenue - pandas_revenue):.2f}")
        correct = False

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
