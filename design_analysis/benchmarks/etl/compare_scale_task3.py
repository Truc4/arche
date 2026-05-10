#!/usr/bin/env python3
"""Compare Arche Task 3 output against Pandas."""

import sys
import re
import subprocess
import pandas as pd
from datetime import datetime
import time

def run_arche_task3():
    """Run Arche task 3 and capture checksum."""
    print("Running Arche Task 3...")
    result = subprocess.run(
        ["./build/arche", "-o", "/tmp/task3_bench",
         "design_analysis/benchmarks/etl/arche_scale/task_3_bucket_timestamps.arche"],
        capture_output=True, text=True, timeout=600
    )
    if result.returncode != 0:
        print(f"Compilation failed: {result.stderr}")
        return None, None

    print("Running compiled Arche program...")
    run_start = time.time()
    result = subprocess.run(["/tmp/task3_bench"], capture_output=True, text=True, timeout=600)
    elapsed = time.time() - run_start

    if result.returncode != 0:
        print(f"Execution failed: {result.stderr}")
        return None, elapsed

    match = re.search(r"task3_checksum: ([\d.e+-]+)", result.stdout)
    if match:
        checksum = float(match.group(1))
        print(f"Arche result: {checksum} ({elapsed:.2f}s)")
        return checksum, elapsed
    return None, elapsed

def run_pandas_task3():
    """Run Pandas task 3 and compute checksum."""
    print("Running Pandas Task 3...")
    start = time.time()

    df = pd.read_csv("design_analysis/benchmarks/etl/data/data_100m.csv")
    df['price_bucket'] = df['price'] / 10.0
    df['hour_bucket'] = pd.to_datetime(df['timestamp']).dt.hour
    checksum = df['price_bucket'].sum()

    elapsed = time.time() - start
    print(f"Pandas result: {checksum} ({elapsed:.2f}s)")
    return checksum, elapsed

if __name__ == "__main__":
    arche_checksum, arche_time = run_arche_task3()
    pandas_checksum, pandas_time = run_pandas_task3()

    if arche_checksum is None or pandas_checksum is None:
        print("FAIL: Could not run both implementations")
        sys.exit(1)

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
