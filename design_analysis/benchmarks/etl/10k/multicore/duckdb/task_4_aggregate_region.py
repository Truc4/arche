#!/usr/bin/env python3
"""Task 4 (DuckDB, transform-only): sum(price * quantity), repeated N times. In-memory table."""

import os
import sys
import time
import duckdb

DEFAULT_CSV = "design_analysis/benchmarks/etl/data/data_10k.csv"
N_ITERS = 216_000
OUT_DIR = "design_analysis/benchmarks/etl/out/duckdb"


def main(csv_path):
    con = duckdb.connect(":memory:")
    con.execute(f"CREATE TABLE t AS SELECT price, quantity FROM read_csv('{csv_path}')")

    total = 0.0
    start = time.perf_counter()
    for _ in range(N_ITERS):
        total += con.execute("SELECT SUM(price * quantity) FROM t").fetchone()[0]
    elapsed = time.perf_counter() - start

    os.makedirs(OUT_DIR, exist_ok=True)
    con.execute(f"COPY (SELECT {total} AS total) TO '{OUT_DIR}/task_4.csv' (HEADER, DELIMITER ',')")

    print(f"task4_checksum: {total}")
    print(f"task4_time: {elapsed / N_ITERS}")


if __name__ == "__main__":
    csv_path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_CSV
    main(csv_path)
