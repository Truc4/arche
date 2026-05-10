#!/usr/bin/env python3
"""Compare Arche Task 1 output against Pandas."""

import sys
import re
import subprocess
import pandas as pd
import time

def run_arche_task1():
    """Run Arche task 1 and capture checksum."""
    print("Running Arche Task 1...")
    start = time.time()
    result = subprocess.run(
        ["./build/arche", "-o", "/tmp/task1_bench",
         "design_analysis/benchmarks/etl/arche_scale/task_1_derived_columns.arche"],
        capture_output=True, text=True, timeout=600
    )
    if result.returncode != 0:
        print(f"Compilation failed: {result.stderr}")
        return None, None

    print("Running compiled Arche program...")
    run_start = time.time()
    result = subprocess.run(["/tmp/task1_bench"], capture_output=True, text=True, timeout=600)
    elapsed = time.time() - run_start

    if result.returncode != 0:
        print(f"Execution failed: {result.stderr}")
        return None, elapsed

    # Extract checksum
    match = re.search(r"task1_checksum: ([\d.e+-]+)", result.stdout)
    if match:
        checksum = float(match.group(1))
        print(f"Arche result: {checksum} ({elapsed:.2f}s)")
        return checksum, elapsed
    return None, elapsed

def run_pandas_task1():
    """Run Pandas task 1 and compute checksum."""
    print("Running Pandas Task 1...")
    start = time.time()

    df = pd.read_csv("design_analysis/benchmarks/etl/data/data_100m.csv")
    df['revenue'] = df['price'] * df['quantity']
    checksum = df['revenue'].sum()

    elapsed = time.time() - start
    print(f"Pandas result: {checksum} ({elapsed:.2f}s)")
    return checksum, elapsed

if __name__ == "__main__":
    arche_checksum, arche_time = run_arche_task1()
    pandas_checksum, pandas_time = run_pandas_task1()

    if arche_checksum is None or pandas_checksum is None:
        print("FAIL: Could not run both implementations")
        sys.exit(1)

    # Compare (allow 1% relative error due to float precision)
    rel_error = abs(arche_checksum - pandas_checksum) / pandas_checksum
    if rel_error < 0.01:
        print(f"PASS: Checksums match (error: {rel_error*100:.4f}%)")
        if arche_time:
            speedup = pandas_time / arche_time
            print(f"Speedup: {speedup:.2f}x")
    else:
        print(f"FAIL: Checksums differ")
        print(f"  Arche: {arche_checksum}")
        print(f"  Pandas: {pandas_checksum}")
        sys.exit(1)
