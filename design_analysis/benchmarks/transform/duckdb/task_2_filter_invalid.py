#!/usr/bin/env python3
"""Task 2 (DuckDB, transform-only): count(quantity > 0), repeated N times. In-memory table."""

import os
import sys
import time
import duckdb

DEFAULT_CSV = "design_analysis/benchmarks/etl/data/data_10k.csv"
N_ITERS = 216_000
OUT_DIR = "design_analysis/benchmarks/transform/out/duckdb"


def main(csv_path):
    con = duckdb.connect(":memory:")
    con.execute(f"CREATE TABLE t AS SELECT quantity FROM read_csv('{csv_path}')")

    count = 0
    start = time.perf_counter()
    for _ in range(N_ITERS):
        count += con.execute("SELECT COUNT(*) FROM t WHERE quantity > 0").fetchone()[0]
    elapsed = time.perf_counter() - start

    os.makedirs(OUT_DIR, exist_ok=True)
    con.execute(f"COPY (SELECT {count} AS count) TO '{OUT_DIR}/task_2.csv' (HEADER, DELIMITER ',')")

    print(f"task2_checksum: {count}")
    print(f"task2_time: {elapsed / N_ITERS}")


if __name__ == "__main__":
    csv_path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_CSV
    main(csv_path)
