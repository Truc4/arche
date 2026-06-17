#!/usr/bin/env python3
"""Task 3 (DataFusion, transform-only): price_bucket = price / 10.0, repeated N times. In-memory."""

import os
import sys
import time

import pyarrow.csv as pa_csv
from datafusion import SessionContext

DEFAULT_CSV = "design_analysis/benchmarks/etl/data/data_10k.csv"
N_ITERS = 216_000
OUT_DIR = "design_analysis/benchmarks/etl/out/datafusion"


def main(csv_path):
    tbl = pa_csv.read_csv(csv_path).select(["price"])
    ctx = SessionContext()
    ctx.from_arrow(tbl, name="t")

    start = time.perf_counter()
    for _ in range(N_ITERS):
        batches = ctx.sql("SELECT price / 10.0 AS price_bucket FROM t").collect()
    elapsed = time.perf_counter() - start

    checksum = 0.0
    for b in batches:
        checksum += sum(b.column(0).to_pylist())

    os.makedirs(OUT_DIR, exist_ok=True)
    with open(f"{OUT_DIR}/task_3.csv", "w") as f:
        f.write("price_bucket\n")
        for b in batches:
            for v in b.column(0).to_pylist():
                f.write(f"{v}\n")

    print(f"task3_checksum: {checksum}")
    print(f"task3_time: {elapsed / N_ITERS}")


if __name__ == "__main__":
    csv_path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_CSV
    main(csv_path)
