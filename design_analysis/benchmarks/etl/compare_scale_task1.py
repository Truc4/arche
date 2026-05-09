#!/usr/bin/env python3
"""Compare Arche task1 with Pandas at scale (100M rows) for performance."""

import subprocess
import time
import pandas as pd
import sys
import os
import re

def run_arche(iterations=3):
    """Run Arche task1 and measure time."""
    os.makedirs('build/benchmarks/etl/task1_scale', exist_ok=True)

    # Compile once
    start = time.perf_counter()
    result = subprocess.run(['./build/arche', '-o', 'build/benchmarks/etl/task1_scale/task1_arche_bin',
                           'design_analysis/benchmarks/etl/arche_scale/task_1_derived_columns.arche'],
                          capture_output=True, text=True)
    if result.returncode != 0:
        print("Arche compilation failed:")
        print(result.stderr)
        sys.exit(1)

    arche_compile_time = time.perf_counter() - start
    print(f"Arche compiled in {arche_compile_time:.2f}s")

    # Run multiple times
    times = []
    arche_checksum = None
    for i in range(iterations):
        start = time.perf_counter()
        result = subprocess.run(['build/benchmarks/etl/task1_scale/task1_arche_bin'],
                              capture_output=True, text=True)
        elapsed = time.perf_counter() - start
        times.append(elapsed)

        if result.returncode != 0:
            print("Arche execution failed:")
            print(result.stderr)
            sys.exit(1)

        # Extract checksum from stdout
        match = re.search(r'task1_checksum: ([\d.e+-]+)', result.stdout)
        if match:
            arche_checksum = float(match.group(1))

    return arche_checksum, arche_compile_time, times

def run_pandas(iterations=3):
    """Run Pandas task1 at scale."""
    times = []
    pandas_checksum = None

    for i in range(iterations):
        start = time.perf_counter()
        df = pd.read_csv('design_analysis/benchmarks/etl/data/data_100m.csv')
        df['revenue'] = df['price'] * df['quantity']
        pandas_checksum = df['revenue'].sum()
        elapsed = time.perf_counter() - start
        times.append(elapsed)

    return pandas_checksum, times

def main():
    iterations = int(sys.argv[1]) if len(sys.argv) > 1 else 3

    print(f"Running Arche task1 scale ({iterations} iterations)...")
    try:
        arche_checksum, arche_compile, arche_times = run_arche(iterations)
    except FileNotFoundError:
        print("ERROR: data_100m.csv not found. Run: make bench-etl-gen")
        sys.exit(1)

    print(f"Running Pandas task1 scale ({iterations} iterations)...")
    try:
        pandas_checksum, pandas_times = run_pandas(iterations)
    except FileNotFoundError:
        print("ERROR: data_100m.csv not found. Run: make bench-etl-gen")
        sys.exit(1)

    print("\n=== Correctness ===")
    if arche_checksum is None:
        print("✗ Arche did not output checksum")
        return 1

    if pandas_checksum is None:
        print("✗ Pandas did not compute checksum")
        return 1

    # Allow 1% relative error due to float accumulation ordering
    relative_error = abs(arche_checksum - pandas_checksum) / pandas_checksum
    if relative_error < 0.01:
        print(f"✓ Checksums match (relative error: {relative_error:.6f})")
        print(f"  Arche:  {arche_checksum:.2f}")
        print(f"  Pandas: {pandas_checksum:.2f}")
    else:
        print(f"✗ Checksum mismatch (relative error: {relative_error:.6f})")
        print(f"  Arche:  {arche_checksum:.2f}")
        print(f"  Pandas: {pandas_checksum:.2f}")

    print("\n=== Performance ===")
    arche_avg = sum(arche_times) / len(arche_times)
    arche_min = min(arche_times)
    arche_max = max(arche_times)

    pandas_avg = sum(pandas_times) / len(pandas_times)
    pandas_min = min(pandas_times)
    pandas_max = max(pandas_times)

    print(f"Arche runtime:  {arche_avg:.2f}s (min: {arche_min:.2f}s, max: {arche_max:.2f}s)")
    print(f"Pandas:         {pandas_avg:.2f}s (min: {pandas_min:.2f}s, max: {pandas_max:.2f}s)")
    print(f"(Arche compilation: {arche_compile:.2f}s, one-time cost)")

    speedup = pandas_avg / arche_avg
    if speedup > 1:
        print(f"\nSpeedup: {speedup:.2f}x (Arche is {speedup:.2f}x faster)")
    else:
        print(f"\nSlowdown: {1/speedup:.2f}x (Pandas is {1/speedup:.2f}x faster)")

    return 0

if __name__ == '__main__':
    sys.exit(main())
