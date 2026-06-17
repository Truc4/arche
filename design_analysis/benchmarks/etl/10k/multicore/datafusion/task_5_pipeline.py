#!/usr/bin/env python3
"""Task 5 (DataFusion, transform-only): filter q>0 AND p>10, sum(p*q), repeated N times. In-memory."""

import os
import sys
import time

import pyarrow.csv as pa_csv
from datafusion import SessionContext

DEFAULT_CSV = "design_analysis/benchmarks/etl/data/data_10k.csv"
N_ITERS = 216_000
OUT_DIR = "design_analysis/benchmarks/etl/out/datafusion"


def main(csv_path):
    tbl = pa_csv.read_csv(csv_path).select(["price", "quantity"])
    ctx = SessionContext()
    ctx.from_arrow(tbl, name="t")

    total = 0.0
    start = time.perf_counter()
    for _ in range(N_ITERS):
        batches = ctx.sql(
            "SELECT SUM(price * quantity) FROM t WHERE quantity > 0 AND price > 10.0"
        ).collect()
        total += batches[0].column(0)[0].as_py()
    elapsed = time.perf_counter() - start

    os.makedirs(OUT_DIR, exist_ok=True)
    with open(f"{OUT_DIR}/task_5.csv", "w") as f:
        f.write(f"total\n{total}\n")

    print(f"task5_checksum: {total}")
    print(f"task5_time: {elapsed / N_ITERS}")


if __name__ == "__main__":
    csv_path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_CSV
    main(csv_path)
