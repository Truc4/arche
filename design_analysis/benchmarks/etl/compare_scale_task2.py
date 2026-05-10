#!/usr/bin/env python3
"""Compare Arche Task 2 output against Pandas."""

import sys
import re
import subprocess
import pandas as pd
import time

def run_arche_task2():
    """Run Arche task 2 and capture checksum."""
    print("Running Arche Task 2...")
    result = subprocess.run(
        ["./build/arche", "-o", "/tmp/task2_bench",
         "design_analysis/benchmarks/etl/arche_scale/task_2_filter_invalid.arche"],
        capture_output=True, text=True, timeout=600
    )
    if result.returncode != 0:
        print(f"Compilation failed: {result.stderr}")
        return None, None

    print("Running compiled Arche program...")
    run_start = time.time()
    result = subprocess.run(["/tmp/task2_bench"], capture_output=True, text=True, timeout=600)
    elapsed = time.time() - run_start

    if result.returncode != 0:
        print(f"Execution failed: {result.stderr}")
        return None, elapsed

    match = re.search(r"task2_checksum: (\d+)", result.stdout)
    if match:
        checksum = int(match.group(1))
        print(f"Arche result: {checksum} ({elapsed:.2f}s)")
        return checksum, elapsed
    return None, elapsed

def run_pandas_task2():
    """Run Pandas task 2 and compute checksum."""
    print("Running Pandas Task 2...")
    start = time.time()

    df = pd.read_csv("design_analysis/benchmarks/etl/data/data_100m.csv")
    count = len(df[df['quantity'] > 0])

    elapsed = time.time() - start
    print(f"Pandas result: {count} ({elapsed:.2f}s)")
    return count, elapsed

if __name__ == "__main__":
    arche_checksum, arche_time = run_arche_task2()
    pandas_checksum, pandas_time = run_pandas_task2()

    if arche_checksum is None or pandas_checksum is None:
        print("FAIL: Could not run both implementations")
        sys.exit(1)

    if arche_checksum == pandas_checksum:
        print(f"PASS: Checksums match")
        if arche_time:
            speedup = pandas_time / arche_time
            print(f"Speedup: {speedup:.2f}x")
    else:
        print(f"FAIL: Checksums differ")
        print(f"  Arche: {arche_checksum}")
        print(f"  Pandas: {pandas_checksum}")
        sys.exit(1)
