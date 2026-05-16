#!/usr/bin/env python3
"""Task 1 (DataFusion, transform-only): revenue = price * quantity, repeated N times.

Loads CSV into an in-memory Arrow table via pyarrow once, registers with DataFusion.
Each iter executes the SELECT against the in-memory table.
"""

import os
import sys
import time

import pyarrow.csv as pa_csv
from datafusion import SessionContext

DEFAULT_CSV = "design_analysis/benchmarks/etl/data/data_10k.csv"
N_ITERS = 216_000
OUT_DIR = "design_analysis/benchmarks/transform/out/datafusion"


def main(csv_path):
    tbl = pa_csv.read_csv(csv_path).select(["price", "quantity"])
    ctx = SessionContext()
    ctx.from_arrow(tbl, name="t")

    start = time.perf_counter()
    for _ in range(N_ITERS):
        batches = ctx.sql("SELECT price * quantity AS revenue FROM t").collect()
    elapsed = time.perf_counter() - start

    # Checksum from the last iter's batches.
    checksum = 0.0
    for b in batches:
        checksum += sum(b.column(0).to_pylist())

    os.makedirs(OUT_DIR, exist_ok=True)
    with open(f"{OUT_DIR}/task_1.csv", "w") as f:
        f.write("revenue\n")
        for b in batches:
            for v in b.column(0).to_pylist():
                f.write(f"{v}\n")

    print(f"task1_checksum: {checksum}")
    print(f"task1_time: {elapsed / N_ITERS}")


if __name__ == "__main__":
    csv_path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_CSV
    main(csv_path)
